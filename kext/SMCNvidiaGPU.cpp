/*
 * SMCNvidiaGPU — NVIDIA GPU sensors for macOS, as a VirtualSMC plugin.
 *
 * Everything this kext publishes is read by iStat Menus, AMD Power Gadget and anything else that looks at
 * SMC keys or at an IOAccelerator's PerformanceStatistics:
 *
 *   SMC keys      TG0D / TG0P / TG0d      GPU temperature (sp78)
 *   PerfStats     Temperature(C), Core Clock(MHz), Memory Clock(MHz), Device Utilization %,
 *                 Total Power(W), Fan Speed(RPM), fan-rpm, fan-percentage, VRAM bytes, …
 *
 * Two sources feed it:
 *
 *   1. sysctls kern.nvgpu* — written by the smcnvgpud daemon through RM's user client (temperature, clocks,
 *      utilization, VRAM, P-state). This is the easy half: RM answers user-space controls.
 *
 *   2. RUSD, the NV00DE_SHARED_DATA block the GPU's GSP firmware fills with what only nvidia-smi normally
 *      shows: average GPU/module/memory power, the enforced power limit, TGP, fan RPM. RM maps that block
 *      only for itself, and NV2080_CTRL_CMD_INTERNAL_INIT_USER_SHARED_DATA is refused (0x56) to every other
 *      client, so it cannot be reached from user space at all.
 *
 * The RUSD half therefore runs here, in the kernel. The kext is injected by OpenCore, i.e. it loads from the
 * boot kernel collection *before* the NullMoth driver (auxiliary collection), so it cannot link against
 * com.nullmoth.NVRM. Instead it finds the driver at run time: NVRM publishes the address of its own
 * nv_xnu_phys_for_kernel_va as "phys-for-va" on the NVRMDisplay nub, which gives the load address (KASLR
 * included); every other entry point is that base plus the offset recovered from the installed driver by
 * kext/nvrm_bindings.py, and each one is checked byte for byte against the build-time signature before it is
 * called. A driver that does not match is simply left alone.
 *
 * With the entry points in hand it does what the driver would do:
 *   gpuCreateRusdMemory(OBJGPU *)  allocate + map the RUSD block, hand it to the GSP, set up polling
 *   RM_API::Control(SET_DATA_POLL) ask the GSP for POWER | FAN | THERMAL | CLOCK | PERF every 500 ms
 * and then reads the block with the seqlock protocol class/cl00de.h prescribes.
 */

#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <sys/sysctl.h>
#include <libkern/OSByteOrder.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <string.h>

#include <VirtualSMCSDK/kern_vsmcapi.hpp>
#include <Headers/kern_util.hpp>

#include "nvrm_bindings.h"
#include "nvrm_bindings_legacy.h"

#define SMCNVG_LOG(fmt, ...) do { IOLog("SMCNvidiaGPU: " fmt "\n", ##__VA_ARGS__); } while (0)

#ifdef SMCNVG_DEBUG
#define SMCNVG_DBG(fmt, ...) do { IOLog("SMCNvidiaGPU[dbg]: " fmt "\n", ##__VA_ARGS__); } while (0)
#else
#define SMCNVG_DBG(fmt, ...) do { } while (0)
#endif

/* ---- sysctls written by smcnvgpud (RM user client) ------------------------------------------------- */

static int nvgputemp = 0, nvggpcclk = 0, nvgdramclk = 0, nvgnvdclk = 0, nvgactivegpc = 0;
static int nvgutil = 0, nvgutilfb = 0, nvgutilenc = 0, nvgutildec = 0, nvgutiljpg = 0, nvgutilofa = 0;
static int nvgvramtotal = 0, nvgvramused = 0, nvgpstate = 0;

/* ---- RUSD values read by this kext ----------------------------------------------------------------- */

static int nvgpupower = 0, nvgpupower_mod = 0, nvgpupower_mem = 0;
static int nvgputgp = 0, nvgpupwrlimit = 0, nvgpupwrlimit_req = 0;
static int nvgpufan = 0, nvgpufancount = 0;
static int nvgpufans[8] = {0, 0, 0, 0, 0, 0, 0, 0};   /* per-fan RPM, in the order the GSP reports them */
static UInt64 nvgpufan_last_ns = 0;   /* last time a non-zero fan reading was seen */
static int nvgpufan_last = 0;
static int nvgpufan_known_count = 0;   /* most coolers ever seen: a stopped fan stays "present" */
static char nvgpufanraw[160] = "";     /* rpmCurr[0..15] as reported, for diagnosing multi-fan cards */
static int nvgpusmclk = 0, nvgpugfxclk = 0, nvgpurusdtemp = 0;
static int nvgpurusdtemps[5] = {0, 0, 0, 0, 0};   /* every NV00DE temperature sensor, in C */
static int nvgputemp_mode = 0;   /* 0 = RM edge temperature, 1..5 = RUSD sensor 0..4 */
static int nvgpuinstpower = 0, nvgpuinstpower_mod = 0, nvgpuinstpower_cpu = 0;
static int nvgpupciegen = 0, nvgpupciewidth = 0, nvgpupciewidthmax = 0;
static int nvgpupcie_errs[8] = {0};      /* linecode, crc, naks, l0s, corr, nonfatal, fatal, unsupported */
static int nvgpubar1total = 0, nvgpubar1free = 0;   /* MB */
static int nvgpuecccorr = 0, nvgpueccuncorr = 0;    /* aggregate counters, DRAM */
static int nvgpupcieraw = 0;
static int nvgputhrottle = 0;   /* RUSD_CLK_THROTTLE_REASON_* bitmask */
static int nvgpudbg_bar1 = 0, nvgpudbg_pci = 0, nvgpudbg_ecc = 0;
static void nvsmc_strcpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    if (!n) return;
    for (; i + 1 < n && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

static char nvgpumodel[96] = "";            /* model string as the driver reports it, before shortening */
static char nvgpuvbios[64] = "";
static char nvgpuvbios_override[64] = "";   /* user-supplied VBIOS version, used when the ROM cannot be read */
static int nvgpurombar = 0;                 /* raw PCI expansion ROM BAR value */
static int nvgpurdrom = 0;                  /* diagnostics: 1 mapped, 2 signature ok, 4 version found, 8 map failed */
static int nvgpulive = 0, nvgpulayout = 0, nvgpupoll = 0;
static int nvgpuforced = 0;      /* set when the kext really bound without a signature match */
static int nvgpufForceArg = 0;   /* nvsmcforce=1 boot argument */

/* RM-side sensors are read here now (see nvsmc_rm_* below); the daemon is no longer required. */

/* Diagnostic: hex dump of the RUSD block, for finding fields we do not decode yet. */
#define NVGPU_RUSD_SIZE 0xc70
static const volatile UInt8 *gRusdBase = nullptr;
static char nvgpurusddump_buf[NVGPU_RUSD_SIZE * 2 + 64] = "no data";
static char nvgpustate[192] = "not started";

static int nvgpusysctl_rusddump SYSCTL_HANDLER_ARGS
{
    static const char hex[] = "0123456789abcdef";
    const volatile UInt8 *p = gRusdBase;
    if (!p) {
        strlcpy(nvgpurusddump_buf, "no data", sizeof nvgpurusddump_buf);
    } else {
        for (int i = 0; i < NVGPU_RUSD_SIZE; i++) {
            UInt8 v = p[i];
            nvgpurusddump_buf[i * 2] = hex[v >> 4];
            nvgpurusddump_buf[i * 2 + 1] = hex[v & 0xf];
        }
        nvgpurusddump_buf[NVGPU_RUSD_SIZE * 2] = 0;
    }
    return sysctl_handle_string(oidp, nvgpurusddump_buf, 0, req);
}

SYSCTL_PROC(_kern, OID_AUTO, nvgpurusddump, CTLTYPE_STRING | CTLFLAG_RD, 0, 0,
            nvgpusysctl_rusddump, "A", "hex dump of the NV00DE_SHARED_DATA block");

SYSCTL_INT(_kern, OID_AUTO, nvgputemp, CTLFLAG_RW, &nvgputemp, 0, "NVIDIA GPU temperature C");
SYSCTL_INT(_kern, OID_AUTO, nvggpcclk, CTLFLAG_RW, &nvggpcclk, 0, "NVIDIA GPU core clock MHz");
SYSCTL_INT(_kern, OID_AUTO, nvgdramclk, CTLFLAG_RW, &nvgdramclk, 0, "NVIDIA GPU memory clock MHz");
SYSCTL_INT(_kern, OID_AUTO, nvgnvdclk, CTLFLAG_RW, &nvgnvdclk, 0, "NVIDIA GPU NVD clock MHz");
SYSCTL_INT(_kern, OID_AUTO, nvgactivegpc, CTLFLAG_RW, &nvgactivegpc, 0, "NVIDIA GPU active GPC clock MHz");
SYSCTL_INT(_kern, OID_AUTO, nvgutil, CTLFLAG_RW, &nvgutil, 0, "NVIDIA GPU utilization pct*100");
SYSCTL_INT(_kern, OID_AUTO, nvgutilfb, CTLFLAG_RW, &nvgutilfb, 0, "NVIDIA GPU FB utilization pct*100");
SYSCTL_INT(_kern, OID_AUTO, nvgutilenc, CTLFLAG_RW, &nvgutilenc, 0, "NVIDIA GPU encoder utilization pct*100");
SYSCTL_INT(_kern, OID_AUTO, nvgutildec, CTLFLAG_RW, &nvgutildec, 0, "NVIDIA GPU decoder utilization pct*100");
SYSCTL_INT(_kern, OID_AUTO, nvgutiljpg, CTLFLAG_RW, &nvgutiljpg, 0, "NVIDIA GPU JPEG utilization pct*100");
SYSCTL_INT(_kern, OID_AUTO, nvgutilofa, CTLFLAG_RW, &nvgutilofa, 0, "NVIDIA GPU OFA utilization pct*100");
SYSCTL_INT(_kern, OID_AUTO, nvgvramtotal, CTLFLAG_RW, &nvgvramtotal, 0, "NVIDIA GPU VRAM total MB");
SYSCTL_INT(_kern, OID_AUTO, nvgvramused, CTLFLAG_RW, &nvgvramused, 0, "NVIDIA GPU VRAM used MB");
SYSCTL_INT(_kern, OID_AUTO, nvgpstate, CTLFLAG_RW, &nvgpstate, 0, "NVIDIA GPU current P-state");

SYSCTL_INT(_kern, OID_AUTO, nvgpupower, CTLFLAG_RD, &nvgpupower, 0, "NVIDIA GPU average power mW (RUSD)");
SYSCTL_INT(_kern, OID_AUTO, nvgpupower_mod, CTLFLAG_RD, &nvgpupower_mod, 0, "NVIDIA GPU module power mW (RUSD)");
SYSCTL_INT(_kern, OID_AUTO, nvgpupower_mem, CTLFLAG_RD, &nvgpupower_mem, 0, "NVIDIA GPU memory power mW (RUSD)");
SYSCTL_INT(_kern, OID_AUTO, nvgputgp, CTLFLAG_RD, &nvgputgp, 0, "NVIDIA total graphics power mW (RUSD)");
SYSCTL_INT(_kern, OID_AUTO, nvgpupwrlimit, CTLFLAG_RD, &nvgpupwrlimit, 0, "NVIDIA enforced power limit mW (RUSD)");
SYSCTL_INT(_kern, OID_AUTO, nvgpupwrlimit_req, CTLFLAG_RD, &nvgpupwrlimit_req, 0, "NVIDIA requested power limit mW (RUSD)");
SYSCTL_INT(_kern, OID_AUTO, nvgpufan, CTLFLAG_RD, &nvgpufan, 0, "NVIDIA GPU fan RPM, highest of all fans (RUSD)");
SYSCTL_INT(_kern, OID_AUTO, nvgpufan0, CTLFLAG_RD, &nvgpufans[0], 0, "NVIDIA GPU fan 1 RPM (RUSD)");
SYSCTL_INT(_kern, OID_AUTO, nvgpufan1, CTLFLAG_RD, &nvgpufans[1], 0, "NVIDIA GPU fan 2 RPM (RUSD)");
SYSCTL_INT(_kern, OID_AUTO, nvgpufan2, CTLFLAG_RD, &nvgpufans[2], 0, "NVIDIA GPU fan 3 RPM (RUSD)");
SYSCTL_INT(_kern, OID_AUTO, nvgpufan3, CTLFLAG_RD, &nvgpufans[3], 0, "NVIDIA GPU fan 4 RPM (RUSD)");
SYSCTL_INT(_kern, OID_AUTO, nvgpufan4, CTLFLAG_RD, &nvgpufans[4], 0, "NVIDIA GPU fan 5 RPM (RUSD)");
SYSCTL_INT(_kern, OID_AUTO, nvgpufan5, CTLFLAG_RD, &nvgpufans[5], 0, "NVIDIA GPU fan 6 RPM (RUSD)");
SYSCTL_INT(_kern, OID_AUTO, nvgpufan6, CTLFLAG_RD, &nvgpufans[6], 0, "NVIDIA GPU fan 7 RPM (RUSD)");
SYSCTL_INT(_kern, OID_AUTO, nvgpufan7, CTLFLAG_RD, &nvgpufans[7], 0, "NVIDIA GPU fan 8 RPM (RUSD)");
SYSCTL_INT(_kern, OID_AUTO, nvgpufancount, CTLFLAG_RD, &nvgpufancount, 0, "NVIDIA GPU fans reporting RPM (RUSD)");
SYSCTL_STRING(_kern, OID_AUTO, nvgpufanraw, CTLFLAG_RD, nvgpufanraw, sizeof(nvgpufanraw), "raw rpmCurr[0..15] from RUSD");
SYSCTL_INT(_kern, OID_AUTO, nvgpusmclk, CTLFLAG_RD, &nvgpusmclk, 0, "NVIDIA GPU SM clock MHz (RUSD)");
SYSCTL_INT(_kern, OID_AUTO, nvgpugfxclk, CTLFLAG_RD, &nvgpugfxclk, 0, "NVIDIA GPU graphics clock MHz (RUSD)");
SYSCTL_INT(_kern, OID_AUTO, nvgpurusdtemp, CTLFLAG_RD, &nvgpurusdtemp, 0, "NVIDIA GPU temperature C (RUSD)");
SYSCTL_INT(_kern, OID_AUTO, nvgpurusdtemp0, CTLFLAG_RD, &nvgpurusdtemps[0], 0, "NVIDIA RUSD temperature sensor 0 C");
SYSCTL_INT(_kern, OID_AUTO, nvgpurusdtemp1, CTLFLAG_RD, &nvgpurusdtemps[1], 0, "NVIDIA RUSD temperature sensor 1 C");
SYSCTL_INT(_kern, OID_AUTO, nvgpurusdtemp2, CTLFLAG_RD, &nvgpurusdtemps[2], 0, "NVIDIA RUSD temperature sensor 2 C");
SYSCTL_INT(_kern, OID_AUTO, nvgpurusdtemp3, CTLFLAG_RD, &nvgpurusdtemps[3], 0, "NVIDIA RUSD temperature sensor 3 C");
SYSCTL_INT(_kern, OID_AUTO, nvgpurusdtemp4, CTLFLAG_RD, &nvgpurusdtemps[4], 0, "NVIDIA RUSD temperature sensor 4 C");
SYSCTL_INT(_kern, OID_AUTO, nvgputemp_mode, CTLFLAG_RW, &nvgputemp_mode, 0, "GPU temperature source: 0 = RM edge, 1..5 = RUSD sensor");
SYSCTL_INT(_kern, OID_AUTO, nvgpuinstpower, CTLFLAG_RD, &nvgpuinstpower, 0, "NVIDIA instant GPU power mW (RUSD)");
SYSCTL_INT(_kern, OID_AUTO, nvgpuinstpower_mod, CTLFLAG_RD, &nvgpuinstpower_mod, 0, "NVIDIA instant module power mW (RUSD)");
SYSCTL_INT(_kern, OID_AUTO, nvgpuinstpower_cpu, CTLFLAG_RD, &nvgpuinstpower_cpu, 0, "NVIDIA instant CPU power mW (RUSD)");
SYSCTL_INT(_kern, OID_AUTO, nvgpupciegen, CTLFLAG_RW, &nvgpupciegen, 0, "NVIDIA PCIe link generation");
SYSCTL_INT(_kern, OID_AUTO, nvgpupciewidth, CTLFLAG_RW, &nvgpupciewidth, 0, "NVIDIA PCIe link width");
SYSCTL_INT(_kern, OID_AUTO, nvgpupciewidthmax, CTLFLAG_RD, &nvgpupciewidthmax, 0, "NVIDIA PCIe max link width");
SYSCTL_INT(_kern, OID_AUTO, nvgpupcie_linecode, CTLFLAG_RD, &nvgpupcie_errs[0], 0, "PCIe line code errors");
SYSCTL_INT(_kern, OID_AUTO, nvgpupcie_crc, CTLFLAG_RD, &nvgpupcie_errs[1], 0, "PCIe CRC errors");
SYSCTL_INT(_kern, OID_AUTO, nvgpupcie_naks, CTLFLAG_RD, &nvgpupcie_errs[2], 0, "PCIe NAKs received");
SYSCTL_INT(_kern, OID_AUTO, nvgpupcie_l0s, CTLFLAG_RD, &nvgpupcie_errs[3], 0, "PCIe failed L0s exits");
SYSCTL_INT(_kern, OID_AUTO, nvgpupcie_corr, CTLFLAG_RD, &nvgpupcie_errs[4], 0, "PCIe correctable errors");
SYSCTL_INT(_kern, OID_AUTO, nvgpupcie_nonfatal, CTLFLAG_RD, &nvgpupcie_errs[5], 0, "PCIe non-fatal errors");
SYSCTL_INT(_kern, OID_AUTO, nvgpupcie_fatal, CTLFLAG_RD, &nvgpupcie_errs[6], 0, "PCIe fatal errors");
SYSCTL_INT(_kern, OID_AUTO, nvgpupcie_unsupported, CTLFLAG_RD, &nvgpupcie_errs[7], 0, "PCIe unsupported requests");
SYSCTL_INT(_kern, OID_AUTO, nvgpubar1total, CTLFLAG_RD, &nvgpubar1total, 0, "BAR1 window total MB");
SYSCTL_INT(_kern, OID_AUTO, nvgpubar1free, CTLFLAG_RD, &nvgpubar1free, 0, "BAR1 window free MB");
SYSCTL_INT(_kern, OID_AUTO, nvgpuecccorr, CTLFLAG_RD, &nvgpuecccorr, 0, "GPU memory ECC corrected (aggregate)");
SYSCTL_INT(_kern, OID_AUTO, nvgpueccuncorr, CTLFLAG_RD, &nvgpueccuncorr, 0, "GPU memory ECC uncorrected (aggregate)");
SYSCTL_INT(_kern, OID_AUTO, nvgputhrottle, CTLFLAG_RD, &nvgputhrottle, 0, "Clock throttle reason bitmask (RUSD)");
SYSCTL_INT(_kern, OID_AUTO, nvgpupcieraw, CTLFLAG_RD, &nvgpupcieraw, 0, "raw PCIe gen info word (RUSD)");
SYSCTL_INT(_kern, OID_AUTO, nvgpudbg_bar1, CTLFLAG_RD, &nvgpudbg_bar1, 0, "BAR1 section readable");
SYSCTL_INT(_kern, OID_AUTO, nvgpudbg_pci, CTLFLAG_RD, &nvgpudbg_pci, 0, "PCI section readable");
SYSCTL_INT(_kern, OID_AUTO, nvgpudbg_ecc, CTLFLAG_RD, &nvgpudbg_ecc, 0, "ECC section readable");
SYSCTL_INT(_kern, OID_AUTO, nvgpuforced, CTLFLAG_RD, &nvgpuforced, 0, "boot-arg nvsmcforce: bound without a signature match");
SYSCTL_STRING(_kern, OID_AUTO, nvgpumodel, CTLFLAG_RD, nvgpumodel, sizeof(nvgpumodel), "GPU model as reported by the driver");
SYSCTL_STRING(_kern, OID_AUTO, nvgpuvbios, CTLFLAG_RD, nvgpuvbios, sizeof(nvgpuvbios), "GPU VBIOS version");
SYSCTL_STRING(_kern, OID_AUTO, nvgpuvbios_override, CTLFLAG_RW, nvgpuvbios_override, sizeof(nvgpuvbios_override), "GPU VBIOS version override (set when the ROM cannot be read)");
SYSCTL_INT(_kern, OID_AUTO, nvgpurombar, CTLFLAG_RD, &nvgpurombar, 0, "PCI expansion ROM BAR value");
SYSCTL_INT(_kern, OID_AUTO, nvgpurdrom, CTLFLAG_RD, &nvgpurdrom, 0, "ROM read diagnostics bits");
SYSCTL_INT(_kern, OID_AUTO, nvgpulive, CTLFLAG_RD, &nvgpulive, 0, "NVIDIA RUSD data updated recently");
SYSCTL_INT(_kern, OID_AUTO, nvgpulayout, CTLFLAG_RD, &nvgpulayout, 0, "NVRM matched nvrm_bindings.h");
SYSCTL_INT(_kern, OID_AUTO, nvgpupoll, CTLFLAG_RD, &nvgpupoll, 0, "GSP accepted the RUSD poll request");
SYSCTL_STRING(_kern, OID_AUTO, nvgpustate, CTLFLAG_RD, nvgpustate, sizeof(nvgpustate), "SMCNvidiaGPU last state");

/* ---- RUSD layout (class/cl00de.h, NVIDIA 615.78.08) ------------------------------------------------ */

#define RUSD_POLL_CLOCK 0x1u
#define RUSD_POLL_PERF 0x2u
#define RUSD_POLL_POWER 0x8u
#define RUSD_POLL_THERMAL 0x10u
#define RUSD_POLL_FAN 0x40u
#define RUSD_POLL_MEMORY 0x4u
#define RUSD_POLL_PCI 0x20u
#define RUSD_POLL_MASK (RUSD_POLL_CLOCK | RUSD_POLL_PERF | RUSD_POLL_POWER | RUSD_POLL_THERMAL | \
                        RUSD_POLL_FAN | RUSD_POLL_MEMORY | RUSD_POLL_PCI)
#define RUSD_POLL_INTERVAL_MS 500u

#define NV2080_CTRL_CMD_INTERNAL_SET_DATA_POLL 0x20800affu

#define RUSD_OFF_MEM_TOTAL 0x0018u    /* framebuffer size, bytes */
#define RUSD_OFF_MEM_FREE  0x0020u    /* free framebuffer memory, bytes */
#define RUSD_OFF_BAR1_INFO 0x0000u
#define RUSD_OFF_CLK_DOMAINS 0x0048u
#define RUSD_OFF_PSTATE 0x08e8u
#define RUSD_OFF_POWER_LIMIT 0x08f8u
#define RUSD_OFF_TEMPERATURES 0x0908u
#define RUSD_OFF_AVG_POWER 0x09a8u
#define RUSD_OFF_POWER_POLICY 0x09d8u
#define RUSD_OFF_CLK_THROTTLE 0x0070u   /* ts + u32 reasonMask ... */
#define RUSD_OFF_MEM_ECC 0x0878u
#define RUSD_OFF_INST_POWER 0x09c0u
#define RUSD_OFF_PCI_BUS_DATA 0x09e8u
#define RUSD_OFF_FAN 0x0a18u
#define RUSD_PCI_DATA_COUNT 9
#define RUSD_TEMP_STRIDE 16u
#define RUSD_FAN_MAX_COOLERS 16u
#define RUSD_CLK_DOMAIN_GRAPHICS 0u
#define RUSD_CLK_DOMAIN_SM 3u

#define RUSD_TIMESTAMP_INVALID 0ull
#define RUSD_TIMESTAMP_WRITE_IN_PROGRESS (~0ull)
#define RUSD_SEQ_START 0xff00000000000000ull

#define RMSG_LOCK_WRITE 0x0u
#define RMSG_MODULE_WORKITEM 0x100u   /* RM_LOCK_MODULE_VAL(0x000001, 0x00) */

/* Fan percentage: RUSD reports RPM only, so the percentage iStat's GPU page wants is RPM over the card's
 * maximum, the way nvidia-smi defines it. RTX 5080-class coolers top out around 3 500 RPM; override with
 * the nvgpufanmax sysctl if a card differs. */
static int nvgpufanmax = 3500;
SYSCTL_INT(_kern, OID_AUTO, nvgpufanmax, CTLFLAG_RW, &nvgpufanmax, 0, "NVIDIA GPU fan RPM at 100% (for the percentage)");

/* ---- driver entry points, resolved at run time ----------------------------------------------------- */

struct NvrmApi {
    void *(*gpu)(unsigned int gpuInstance);
    int (*createRusd)(void *pGpu);
    int (*linkRusd)(void *pGpu);
    int (*lock)(unsigned int flags, unsigned int module);
    void (*unlock)(void);
    int (*pmEnter)(void);
    void (*pmExit)(int counted);
};
static NvrmApi gApi;
static UInt64 gNvrmBase = 0;
static UInt64 gNvrmAnchor = 0;

static inline UInt64 nvsens_now_ns(void)
{
    UInt64 ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}

static void nvsens_state_set(const char *fmt, ...) __printflike(1, 2);
static void nvsens_state_set(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(nvgpustate, sizeof(nvgpustate), fmt, ap);
    va_end(ap);
}

static bool sig_matches(UInt64 addr, const unsigned char *sig)
{
    if (!addr)
        return false;
    return memcmp((const void *)(uintptr_t)addr, sig, NVRM_SIG_LEN) == 0;
}

/* Install one binding table if every entry point matches its recorded signature. */
struct NvrmTable {
    const char *label;
    UInt64 anchorOff; const unsigned char *sigAnchor;
    UInt64 gpuOff; const unsigned char *sigGpu;
    UInt64 createOff; const unsigned char *sigCreate;
    UInt64 linkOff; const unsigned char *sigLink;
    UInt64 lockOff; const unsigned char *sigLock;
    UInt64 unlockOff; const unsigned char *sigUnlock;
    UInt64 pmInOff; const unsigned char *sigPmIn;
    UInt64 pmOutOff; const unsigned char *sigPmOut;
};

static const NvrmTable kCurrentTable = {
    NVRM_DRIVER_SHA256,
    NVRM_ANCHOR_OFF, NVRM_SIG_anchor,
    NVRM_OFF_gpumgrGetGpu, NVRM_SIG_gpumgrGetGpu,
    NVRM_OFF_createRusd, NVRM_SIG_createRusd,
    NVRM_OFF_linkRusd, NVRM_SIG_linkRusd,
    NVRM_OFF_lock, NVRM_SIG_lock,
    NVRM_OFF_unlock, NVRM_SIG_unlock,
    NVRM_OFF_pmEnter, NVRM_SIG_pmEnter,
    NVRM_OFF_pmExit, NVRM_SIG_pmExit,
};

static bool nvrm_apply_table(UInt64 anchor, const NvrmTable &t)
{
    const UInt64 base = anchor - t.anchorOff;
    if (!sig_matches(anchor, t.sigAnchor))
        return false;
    if (!sig_matches(base + t.gpuOff, t.sigGpu) ||
        !sig_matches(base + t.createOff, t.sigCreate) ||
        !sig_matches(base + t.linkOff, t.sigLink) ||
        !sig_matches(base + t.lockOff, t.sigLock) ||
        !sig_matches(base + t.unlockOff, t.sigUnlock) ||
        !sig_matches(base + t.pmInOff, t.sigPmIn) ||
        !sig_matches(base + t.pmOutOff, t.sigPmOut))
        return false;

    gApi.gpu = (void *(*)(unsigned int))(uintptr_t)(base + t.gpuOff);
    gApi.createRusd = (int (*)(void *))(uintptr_t)(base + t.createOff);
    gApi.linkRusd = (int (*)(void *))(uintptr_t)(base + t.linkOff);
    gApi.lock = (int (*)(unsigned int, unsigned int))(uintptr_t)(base + t.lockOff);
    gApi.unlock = (void (*)(void))(uintptr_t)(base + t.unlockOff);
    gApi.pmEnter = (int (*)(void))(uintptr_t)(base + t.pmInOff);
    gApi.pmExit = (void (*)(int))(uintptr_t)(base + t.pmOutOff);
    gNvrmBase = base;
    return true;
}

/* nvsmcforce=1 helper: the anchor is `83 3d <disp32> ...`, so a rebuild changes only the displacement.
 * Comparing everything but those four bytes still identifies which table belongs to this driver. */
static bool sig_matches_anchor_relaxed(UInt64 addr, const unsigned char *sig)
{
    if (!addr)
        return false;
    const unsigned char *p = (const unsigned char *)(uintptr_t)addr;
    for (unsigned i = 0; i < NVRM_SIG_LEN; i++) {
        if (i >= 2 && i < 6)          /* rel32 of the RIP-relative compare */
            continue;
        if (p[i] != sig[i])
            return false;
    }
    return true;
}

/* nvsmcforce=1: install the table without checking the entry points. Only useful when the driver was
 * rebuilt with identical code layout but shifted bytes, and it can call into the wrong place otherwise —
 * that is why it is a boot argument and not something the kext decides on its own. */
static void nvrm_apply_table_forced(UInt64 anchor, const NvrmTable &t)
{
    const UInt64 base = anchor - t.anchorOff;
    gApi.gpu = (void *(*)(unsigned int))(uintptr_t)(base + t.gpuOff);
    gApi.createRusd = (int (*)(void *))(uintptr_t)(base + t.createOff);
    gApi.linkRusd = (int (*)(void *))(uintptr_t)(base + t.linkOff);
    gApi.lock = (int (*)(unsigned int, unsigned int))(uintptr_t)(base + t.lockOff);
    gApi.unlock = (void (*)(void))(uintptr_t)(base + t.unlockOff);
    gApi.pmEnter = (int (*)(void))(uintptr_t)(base + t.pmInOff);
    gApi.pmExit = (void (*)(int))(uintptr_t)(base + t.pmOutOff);
    gNvrmBase = base;
    nvgpuforced = 1;
}

/* Find the driver: NVRMDisplay publishes "phys-for-va", the address of nv_xnu_phys_for_kernel_va. */
static bool nvrm_base_from_display_nub(void)
{
    if (gNvrmBase)
        return true;
    OSDictionary *m = IOService::serviceMatching("NVRMDisplay");
    if (!m)
        return false;
    OSIterator *it = IOService::getMatchingServices(m);
    m->release();
    if (!it)
        return false;
    UInt64 anchor = 0;
    while (IOService *nub = OSDynamicCast(IOService, it->getNextObject())) {
        OSNumber *n = OSDynamicCast(OSNumber, nub->getProperty("phys-for-va"));
        if (!n)
            continue;
        UInt64 v = n->unsigned64BitValue();
        if (v > 0xffffff0000000000ull) {      /* must be a kernel text address */
            anchor = v;
            break;
        }
    }
    it->release();
    if (!anchor)
        return false;
    gNvrmAnchor = anchor;
    (void)gNvrmAnchor;
    return true;
}

static bool nvrm_bind(void)
{
    if (!nvrm_base_from_display_nub())
        return false;
    const UInt64 anchor = gNvrmAnchor;

    if (nvrm_apply_table(anchor, kCurrentTable)) {
        nvgpulayout = 1;
        SMCNVG_LOG("bound NVRM %.12s at 0x%llx", NVRM_DRIVER_SHA256, (unsigned long long)gNvrmBase);
        nvsens_state_set("bound NVRM 0x%llx", (unsigned long long)gNvrmBase);
        return true;
    }
    for (int i = 0; i < kLegacyBindingCount; i++) {
        const NvrmLegacyBinding &l = kLegacyBindings[i];
        NvrmTable t = { l.label, l.anchorOff, l.sigAnchor, l.gpuOff, l.sigGpu, l.createOff, l.sigCreate,
                        l.linkOff, l.sigLink, l.lockOff, l.sigLock, l.unlockOff, l.sigUnlock,
                        l.pmInOff, l.sigPmIn, l.pmOutOff, l.sigPmOut };
        if (nvrm_apply_table(anchor, t)) {
            nvgpulayout = 1;
            SMCNVG_LOG("bound NVRM %s at 0x%llx (legacy table)", l.label, (unsigned long long)gNvrmBase);
            nvsens_state_set("bound NVRM %s 0x%llx", l.label, (unsigned long long)gNvrmBase);
            return true;
        }
    }
    if (nvgpufForceArg) {
        /* Pick the table whose anchor still lines up apart from the displacement bytes, so the entry
         * offsets come from the build that is actually installed; fall back to the current table. */
        bool used = false;
        if (sig_matches_anchor_relaxed(anchor, kCurrentTable.sigAnchor)) {
            nvrm_apply_table_forced(anchor, kCurrentTable);
            used = true;
        }
        for (int i = 0; !used && i < kLegacyBindingCount; i++) {
            const NvrmLegacyBinding &l = kLegacyBindings[i];
            NvrmTable t = { l.label, l.anchorOff, l.sigAnchor, l.gpuOff, l.sigGpu, l.createOff, l.sigCreate,
                            l.linkOff, l.sigLink, l.lockOff, l.sigLock, l.unlockOff, l.sigUnlock,
                            l.pmInOff, l.sigPmIn, l.pmOutOff, l.sigPmOut };
            if (sig_matches_anchor_relaxed(anchor, l.sigAnchor)) {
                nvrm_apply_table_forced(anchor, t);
                nvsens_state_set("forced binding 0x%llx, table %s (nvsmcforce=1)",
                                 (unsigned long long)gNvrmBase, l.label);
                used = true;
            }
        }
        if (!used) {
            nvrm_apply_table_forced(anchor, kCurrentTable);
            nvsens_state_set("forced binding 0x%llx, current table (nvsmcforce=1)",
                             (unsigned long long)gNvrmBase);
        }
        nvgpulayout = 1;
        SMCNVG_LOG("nvsmcforce: bound NVRM without checking the entry points at 0x%llx",
                   (unsigned long long)gNvrmBase);
        return true;
    }
    nvsens_state_set("NVRM at 0x%llx matches none of the %d known builds (nvsmcforce=1 to try anyway)",
                     (unsigned long long)(anchor - kCurrentTable.anchorOff), kLegacyBindingCount + 1);
    return false;
}
/* Run one RM operation with the GPU kept awake and the API lock held. */
template <typename Body>
static int nvsens_rm_call(Body body)
{
    if (!gApi.lock || !gApi.pmEnter || !gApi.pmExit)
        return -1;
    int counted = gApi.pmEnter();
    int lockStatus = gApi.lock(RMSG_LOCK_WRITE, RMSG_MODULE_WORKITEM);
    int status;
    if (lockStatus == 0) {
        status = body();
        gApi.unlock();
    } else {
        status = lockStatus;
    }
    gApi.pmExit(counted);
    return status;
}

/* ---- kext ------------------------------------------------------------------------------------------ */

/* PCIe generation and lane count do not come from RUSD (its gen-info word only carries the generation).
 * NV2080_CTRL_CMD_BUS_GET_INFO_V2 answers with the link status word: bits[19:16] = speed code
 * (1=2.5GT … 5=32GT), bits[25:20] = negotiated lanes. */
static void nvsmc_read_pcie_bus(void *pGpu)
{
    if (!pGpu)
        return;
    UInt8 *base = (UInt8 *)pGpu;
    UInt32 hClient = *(volatile UInt32 *)(base + OFF_HINTERNAL_CLIENT);
    UInt32 hSub = *(volatile UInt32 *)(base + OFF_HINTERNAL_SUBDEVICE);
    UInt64 control = *(volatile UInt64 *)(base + OFF_RMAPI_CONTROL);
    if (!hClient || !hSub || !control)
        return;
    struct BusInfo { UInt32 index, data; };
    struct { UInt32 count; BusInfo list[52]; } params;
    memset(&params, 0, sizeof params);
    params.count = 2;
    params.list[0].index = 0x03u;   /* PCIE_GPU_LINK_CAPS */
    params.list[1].index = 0x07u;   /* PCIE_GPU_LINK_CTRL_STATUS */
    typedef int (*rm_control_t)(void *, UInt32, UInt32, UInt32, void *, UInt32);
    rm_control_t fn = (rm_control_t)(uintptr_t)control;
    int status = nvsens_rm_call([&] {
        return fn((void *)(base + OFF_PHYSICAL_RMAPI), hClient, hSub, 0x20801823u, &params, sizeof params);
    });
    if (status != 0 || params.count < 2 || params.list[1].data == 0)
        return;
    UInt32 sts = params.list[1].data;
    UInt32 speed = (sts >> 16) & 0xfu;
    UInt32 width = (sts >> 20) & 0x3fu;
    if (speed >= 1 && speed <= 6) nvgpupciegen = (int)speed;
    if (width >= 1 && width <= 32) {
        nvgpupciewidth = (int)width;
        nvgpupciewidthmax = (int)width;
    }
}

/* What the SMC keys and PerformanceStatistics report. Default is the RM edge sensor; setting
 * kern.nvgputemp_mode = N (1..5) switches to one of RUSD's sensors, which on this board means the hotter
 * hotspot/memory readings — useful because AMD Power Gadget treats anything under 50 C as "Zero RPM Mode"
 * and stops showing the fan. */
static int nvsmc_current_temp(void)
{
    int mode = nvgputemp_mode;
    if (mode >= 1 && mode <= 5) {
        int t = nvgpurusdtemps[mode - 1];
        if (t > 0 && t < 150)
            return t;
    }
    return nvgputemp;
}

class GPUTempValue : public VirtualSMCValue {
protected:
    SMC_RESULT readAccess() override {
        int t = nvsmc_current_temp();
        if (t < 1 || t > 125) return SmcNotReadable;
        *reinterpret_cast<uint16_t *>(data) = VirtualSMCAPI::encodeIntSp(SmcKeyTypeSp78, t);
        return SmcSuccess;
    }
};

/// GPU fan speed (fpe2, the type Apple uses for F0Ac..): read straight from the RUSD per-fan array.
class GPUFanSpeedValue : public VirtualSMCValue {
    unsigned int fIndex;
public:
    GPUFanSpeedValue(unsigned int index) : fIndex(index) {}
protected:
    SMC_RESULT readAccess() override {
        int rpm = (fIndex < 8) ? nvgpufans[fIndex] : 0;
        *reinterpret_cast<uint16_t *>(data) = VirtualSMCAPI::encodeIntFp(SmcKeyTypeFpe2, (uint16_t)rpm);
        return SmcSuccess;
    }
};

/// Fixed string key (ch8*), used for F<N>ID.
class GPUStringValue : public VirtualSMCValue {
    const char *fText;
public:
    GPUStringValue(const char *text) : fText(text) {}
protected:
    SMC_RESULT readAccess() override {
        size_t n = strlen(fText);
        if (n > 32) n = 32;
        memcpy(data, fText, n);
        for (size_t i = n; i < 32; i++) data[i] = ' ';
        return SmcSuccess;
    }
};

static void nvsmc_set_num(OSDictionary *d, const char *key, unsigned long long v, unsigned int bits = 32)
{
    if (OSNumber *n = OSNumber::withNumber(v, bits)) { d->setObject(key, n); n->release(); }
}

class SMCNvidiaGPU : public IOService {
    OSDeclareDefaultStructors(SMCNvidiaGPU)
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
    static bool vsmcNotificationHandler(void *self, void *refCon, IOService *vsmc, IONotifier *notifier);
private:
    VirtualSMCAPI::Plugin vsmcPlugin {
        "SMCNvidiaGPU", parseModuleVersion("1.5"), VirtualSMCAPI::Version,
    };
    IONotifier *vsmcNotifier {nullptr};
    IOService *fNvrm {nullptr};
    IOService *fAccel {nullptr};
    OSDictionary *fPub {nullptr};
    OSDictionary *fPubPrev {nullptr};
    IOWorkLoop *fWorkloop {nullptr};
    IOTimerEventSource *fTimer {nullptr};
    void *fGpu {nullptr};
    const volatile UInt8 *fRusd {nullptr};
    UInt32 fTicks {0}, fCreateTries {0}, fRelinkTries {0}, fLastPoll {0};
    UInt64 fTsPower {0}, fTsFan {0}, fTsLimit {0}, fTsPolicy {0}, fTsPstate {0}, fTsClk {0}, fTsTemp {0};
    UInt64 fTsInst {0}, fTsPci {0}, fTsEcc {0}, fTsThrottle {0};
    UInt64 fChangedNs {0};
    bool fBound {false}, fCreateFailedLogged {false}, fSysctls {false}, fVbiosTried {false};

    static void onTimer(OSObject *owner, IOTimerEventSource *src);
    void tick();
    void registerSysctls();
    void unregisterSysctls();
    bool driverReady();
    bool ensureRusd();
    bool requestPoll();
    void readRusd();
    void publish();
    IOService *accel();
};

OSDefineMetaClassAndStructors(SMCNvidiaGPU, IOService)

void SMCNvidiaGPU::registerSysctls()
{
    if (fSysctls)
        return;
    sysctl_register_oid(&sysctl__kern_nvgputemp);
    sysctl_register_oid(&sysctl__kern_nvggpcclk);
    sysctl_register_oid(&sysctl__kern_nvgdramclk);
    sysctl_register_oid(&sysctl__kern_nvgnvdclk);
    sysctl_register_oid(&sysctl__kern_nvgactivegpc);
    sysctl_register_oid(&sysctl__kern_nvgutil);
    sysctl_register_oid(&sysctl__kern_nvgutilfb);
    sysctl_register_oid(&sysctl__kern_nvgutilenc);
    sysctl_register_oid(&sysctl__kern_nvgutildec);
    sysctl_register_oid(&sysctl__kern_nvgutiljpg);
    sysctl_register_oid(&sysctl__kern_nvgutilofa);
    sysctl_register_oid(&sysctl__kern_nvgvramtotal);
    sysctl_register_oid(&sysctl__kern_nvgvramused);
    sysctl_register_oid(&sysctl__kern_nvgpstate);
    sysctl_register_oid(&sysctl__kern_nvgpupower);
    sysctl_register_oid(&sysctl__kern_nvgpupower_mod);
    sysctl_register_oid(&sysctl__kern_nvgpupower_mem);
    sysctl_register_oid(&sysctl__kern_nvgputgp);
    sysctl_register_oid(&sysctl__kern_nvgpupwrlimit);
    sysctl_register_oid(&sysctl__kern_nvgpupwrlimit_req);
    sysctl_register_oid(&sysctl__kern_nvgpufan);
    sysctl_register_oid(&sysctl__kern_nvgpurusddump);
    sysctl_register_oid(&sysctl__kern_nvgpufan0);
    sysctl_register_oid(&sysctl__kern_nvgpufan1);
    sysctl_register_oid(&sysctl__kern_nvgpufan2);
    sysctl_register_oid(&sysctl__kern_nvgpufan3);
    sysctl_register_oid(&sysctl__kern_nvgpufan4);
    sysctl_register_oid(&sysctl__kern_nvgpufan5);
    sysctl_register_oid(&sysctl__kern_nvgpufan6);
    sysctl_register_oid(&sysctl__kern_nvgpufan7);
    sysctl_register_oid(&sysctl__kern_nvgpufancount);
    sysctl_register_oid(&sysctl__kern_nvgpufanraw);
    sysctl_register_oid(&sysctl__kern_nvgpusmclk);
    sysctl_register_oid(&sysctl__kern_nvgpugfxclk);
    sysctl_register_oid(&sysctl__kern_nvgpurusdtemp);
    sysctl_register_oid(&sysctl__kern_nvgpurusdtemp0);
    sysctl_register_oid(&sysctl__kern_nvgpurusdtemp1);
    sysctl_register_oid(&sysctl__kern_nvgpurusdtemp2);
    sysctl_register_oid(&sysctl__kern_nvgpurusdtemp3);
    sysctl_register_oid(&sysctl__kern_nvgpurusdtemp4);
    sysctl_register_oid(&sysctl__kern_nvgputemp_mode);
    sysctl_register_oid(&sysctl__kern_nvgpuinstpower);
    sysctl_register_oid(&sysctl__kern_nvgpuinstpower_mod);
    sysctl_register_oid(&sysctl__kern_nvgpuinstpower_cpu);
    sysctl_register_oid(&sysctl__kern_nvgpupciegen);
    sysctl_register_oid(&sysctl__kern_nvgpupciewidth);
    sysctl_register_oid(&sysctl__kern_nvgpupciewidthmax);
    sysctl_register_oid(&sysctl__kern_nvgpupcie_linecode);
    sysctl_register_oid(&sysctl__kern_nvgpupcie_crc);
    sysctl_register_oid(&sysctl__kern_nvgpupcie_naks);
    sysctl_register_oid(&sysctl__kern_nvgpupcie_l0s);
    sysctl_register_oid(&sysctl__kern_nvgpupcie_corr);
    sysctl_register_oid(&sysctl__kern_nvgpupcie_nonfatal);
    sysctl_register_oid(&sysctl__kern_nvgpupcie_fatal);
    sysctl_register_oid(&sysctl__kern_nvgpupcie_unsupported);
    sysctl_register_oid(&sysctl__kern_nvgpubar1total);
    sysctl_register_oid(&sysctl__kern_nvgpubar1free);
    sysctl_register_oid(&sysctl__kern_nvgpuecccorr);
    sysctl_register_oid(&sysctl__kern_nvgpueccuncorr);
    sysctl_register_oid(&sysctl__kern_nvgputhrottle);
    sysctl_register_oid(&sysctl__kern_nvgpupcieraw);
    sysctl_register_oid(&sysctl__kern_nvgpudbg_bar1);
    sysctl_register_oid(&sysctl__kern_nvgpudbg_pci);
    sysctl_register_oid(&sysctl__kern_nvgpudbg_ecc);
    sysctl_register_oid(&sysctl__kern_nvgpuvbios);
    sysctl_register_oid(&sysctl__kern_nvgpuvbios_override);
    sysctl_register_oid(&sysctl__kern_nvgpurombar);
    sysctl_register_oid(&sysctl__kern_nvgpurdrom);
    sysctl_register_oid(&sysctl__kern_nvgpulive);
    sysctl_register_oid(&sysctl__kern_nvgpulayout);
    sysctl_register_oid(&sysctl__kern_nvgpupoll);
    sysctl_register_oid(&sysctl__kern_nvgpustate);
    sysctl_register_oid(&sysctl__kern_nvgpufanmax);
    fSysctls = true;
}

void SMCNvidiaGPU::unregisterSysctls()
{
    if (!fSysctls)
        return;
    sysctl_unregister_oid(&sysctl__kern_nvgputemp);
    sysctl_unregister_oid(&sysctl__kern_nvggpcclk);
    sysctl_unregister_oid(&sysctl__kern_nvgdramclk);
    sysctl_unregister_oid(&sysctl__kern_nvgnvdclk);
    sysctl_unregister_oid(&sysctl__kern_nvgactivegpc);
    sysctl_unregister_oid(&sysctl__kern_nvgutil);
    sysctl_unregister_oid(&sysctl__kern_nvgutilfb);
    sysctl_unregister_oid(&sysctl__kern_nvgutilenc);
    sysctl_unregister_oid(&sysctl__kern_nvgutildec);
    sysctl_unregister_oid(&sysctl__kern_nvgutiljpg);
    sysctl_unregister_oid(&sysctl__kern_nvgutilofa);
    sysctl_unregister_oid(&sysctl__kern_nvgvramtotal);
    sysctl_unregister_oid(&sysctl__kern_nvgvramused);
    sysctl_unregister_oid(&sysctl__kern_nvgpstate);
    sysctl_unregister_oid(&sysctl__kern_nvgpupower);
    sysctl_unregister_oid(&sysctl__kern_nvgpupower_mod);
    sysctl_unregister_oid(&sysctl__kern_nvgpupower_mem);
    sysctl_unregister_oid(&sysctl__kern_nvgputgp);
    sysctl_unregister_oid(&sysctl__kern_nvgpupwrlimit);
    sysctl_unregister_oid(&sysctl__kern_nvgpupwrlimit_req);
    sysctl_unregister_oid(&sysctl__kern_nvgpufan);
    sysctl_unregister_oid(&sysctl__kern_nvgpurusddump);
    sysctl_unregister_oid(&sysctl__kern_nvgpufan0);
    sysctl_unregister_oid(&sysctl__kern_nvgpufan1);
    sysctl_unregister_oid(&sysctl__kern_nvgpufan2);
    sysctl_unregister_oid(&sysctl__kern_nvgpufan3);
    sysctl_unregister_oid(&sysctl__kern_nvgpufan4);
    sysctl_unregister_oid(&sysctl__kern_nvgpufan5);
    sysctl_unregister_oid(&sysctl__kern_nvgpufan6);
    sysctl_unregister_oid(&sysctl__kern_nvgpufan7);
    sysctl_unregister_oid(&sysctl__kern_nvgpufancount);
    sysctl_unregister_oid(&sysctl__kern_nvgpufanraw);
    sysctl_unregister_oid(&sysctl__kern_nvgpusmclk);
    sysctl_unregister_oid(&sysctl__kern_nvgpugfxclk);
    sysctl_unregister_oid(&sysctl__kern_nvgpurusdtemp);
    sysctl_unregister_oid(&sysctl__kern_nvgpurusdtemp0);
    sysctl_unregister_oid(&sysctl__kern_nvgpurusdtemp1);
    sysctl_unregister_oid(&sysctl__kern_nvgpurusdtemp2);
    sysctl_unregister_oid(&sysctl__kern_nvgpurusdtemp3);
    sysctl_unregister_oid(&sysctl__kern_nvgpurusdtemp4);
    sysctl_unregister_oid(&sysctl__kern_nvgputemp_mode);
    sysctl_unregister_oid(&sysctl__kern_nvgpuinstpower);
    sysctl_unregister_oid(&sysctl__kern_nvgpuinstpower_mod);
    sysctl_unregister_oid(&sysctl__kern_nvgpuinstpower_cpu);
    sysctl_unregister_oid(&sysctl__kern_nvgpupciegen);
    sysctl_unregister_oid(&sysctl__kern_nvgpupciewidth);
    sysctl_unregister_oid(&sysctl__kern_nvgpupciewidthmax);
    sysctl_unregister_oid(&sysctl__kern_nvgpupcie_linecode);
    sysctl_unregister_oid(&sysctl__kern_nvgpupcie_crc);
    sysctl_unregister_oid(&sysctl__kern_nvgpupcie_naks);
    sysctl_unregister_oid(&sysctl__kern_nvgpupcie_l0s);
    sysctl_unregister_oid(&sysctl__kern_nvgpupcie_corr);
    sysctl_unregister_oid(&sysctl__kern_nvgpupcie_nonfatal);
    sysctl_unregister_oid(&sysctl__kern_nvgpupcie_fatal);
    sysctl_unregister_oid(&sysctl__kern_nvgpupcie_unsupported);
    sysctl_unregister_oid(&sysctl__kern_nvgpubar1total);
    sysctl_unregister_oid(&sysctl__kern_nvgpubar1free);
    sysctl_unregister_oid(&sysctl__kern_nvgpuecccorr);
    sysctl_unregister_oid(&sysctl__kern_nvgpueccuncorr);
    sysctl_unregister_oid(&sysctl__kern_nvgputhrottle);
    sysctl_unregister_oid(&sysctl__kern_nvgpupcieraw);
    sysctl_unregister_oid(&sysctl__kern_nvgpudbg_bar1);
    sysctl_unregister_oid(&sysctl__kern_nvgpudbg_pci);
    sysctl_unregister_oid(&sysctl__kern_nvgpudbg_ecc);
    sysctl_unregister_oid(&sysctl__kern_nvgpuvbios);
    sysctl_unregister_oid(&sysctl__kern_nvgpuvbios_override);
    sysctl_unregister_oid(&sysctl__kern_nvgpurombar);
    sysctl_unregister_oid(&sysctl__kern_nvgpurdrom);
    sysctl_unregister_oid(&sysctl__kern_nvgpulive);
    sysctl_unregister_oid(&sysctl__kern_nvgpulayout);
    sysctl_unregister_oid(&sysctl__kern_nvgpupoll);
    sysctl_unregister_oid(&sysctl__kern_nvgpustate);
    sysctl_unregister_oid(&sysctl__kern_nvgpufanmax);
    fSysctls = false;
}

/* NVRM's own start thread publishes "nvrm-autogo" = "up" once go(2) has brought RM up. Before that there is
 * no GPU to ask: memdescAlloc would run against a memory manager that is not initialised yet. */
bool SMCNvidiaGPU::driverReady()
{
    if (!fNvrm) {
        OSDictionary *m = IOService::serviceMatching("NVRM");
        if (!m)
            return false;
        OSIterator *it = IOService::getMatchingServices(m);
        m->release();
        if (!it)
            return false;
        fNvrm = OSDynamicCast(IOService, it->getNextObject());
        if (fNvrm)
            fNvrm->retain();
        it->release();
        if (!fNvrm) {
            nvsens_state_set("waiting for the NVRM service");
            return false;
        }
    }
    OSString *autoGo = OSDynamicCast(OSString, fNvrm->getProperty("nvrm-autogo"));
    if (!autoGo || !autoGo->isEqualTo("up")) {
        nvsens_state_set("waiting for NVRM to start the GPU (nvrm-autogo)");
        return false;
    }
    return true;
}

bool SMCNvidiaGPU::ensureRusd()
{
    if (!fGpu)
        return false;
    UInt8 *base = (UInt8 *)fGpu;
    UInt64 memdesc = *(volatile UInt64 *)(base + OFF_USD_PMEMDESC);
    UInt64 mapping = *(volatile UInt64 *)(base + OFF_USD_PMAPBUFFER);
    if (memdesc != 0) {
        if (mapping == 0)
            return false;       /* the driver is in the middle of allocating it */
        fRusd = (volatile UInt8 *)(uintptr_t)mapping;
        gRusdBase = fRusd;
        return true;
    }
    if (fCreateTries >= 600)
        return false;
    fCreateTries++;
    int status = nvsens_rm_call([this] { return gApi.createRusd(fGpu); });
    if (status != 0) {
        if (!fCreateFailedLogged) {
            fCreateFailedLogged = true;
            SMCNVG_LOG("gpuCreateRusdMemory returned 0x%x; will retry", status);
            nvsens_state_set("gpuCreateRusdMemory failed 0x%x (gpu not up yet?)", status);
        }
        return false;
    }
    memdesc = *(volatile UInt64 *)(base + OFF_USD_PMEMDESC);
    mapping = *(volatile UInt64 *)(base + OFF_USD_PMAPBUFFER);
    fRusd = (volatile UInt8 *)(uintptr_t)mapping;
    SMCNVG_LOG("RUSD block allocated by the driver (pMemDesc %p, mapping %p)",
               (void *)(uintptr_t)memdesc, (void *)fRusd);
    nvsens_state_set("rusd created, mapping %p", (void *)fRusd);
    return fRusd != nullptr;
}

bool SMCNvidiaGPU::requestPoll()
{
    if (!fGpu || !fRusd)
        return false;
    UInt8 *base = (UInt8 *)fGpu;
    UInt32 hClient = *(volatile UInt32 *)(base + OFF_HINTERNAL_CLIENT);
    UInt32 hSubdevice = *(volatile UInt32 *)(base + OFF_HINTERNAL_SUBDEVICE);
    UInt64 control = *(volatile UInt64 *)(base + OFF_RMAPI_CONTROL);
    if (!hClient || !hSubdevice || !control)
        return false;

    struct {
        UInt64 polledDataMask;
        UInt32 pollIntervalMs;
        UInt32 pad;
    } params = { RUSD_POLL_MASK, RUSD_POLL_INTERVAL_MS, 0 };
    const UInt64 kFallbackMask = 0x5bull;   /* CLOCK|PERF|POWER|THERMAL|FAN: what older firmware accepted */

    typedef int (*rm_control_t)(void *pRmApi, UInt32 hClient, UInt32 hObject, UInt32 cmd, void *pParams, UInt32 size);
    rm_control_t fn = (rm_control_t)(uintptr_t)control;

    int status = nvsens_rm_call([&] {
        return fn((void *)(base + OFF_PHYSICAL_RMAPI), hClient, hSubdevice,
                  NV2080_CTRL_CMD_INTERNAL_SET_DATA_POLL, &params, sizeof(params));
    });
    if (status != 0 && params.polledDataMask != kFallbackMask) {
        SMCNVG_LOG("set_data_poll(mask 0x%llx) rejected (0x%x), retrying with 0x5b",
                   (unsigned long long)params.polledDataMask, status);
        params.polledDataMask = kFallbackMask;
        status = nvsens_rm_call([&] {
            return fn((void *)(base + OFF_PHYSICAL_RMAPI), hClient, hSubdevice,
                      NV2080_CTRL_CMD_INTERNAL_SET_DATA_POLL, &params, sizeof(params));
        });
    }
    if (status != 0) {
        nvsens_state_set("set_data_poll(0x%x) returned 0x%x", RUSD_POLL_MASK, status);
        return false;
    }
    nvgpupoll = 1;
    SMCNVG_LOG("GSP polling RUSD mask 0x%x every %u ms", RUSD_POLL_MASK, RUSD_POLL_INTERVAL_MS);
    nvsens_state_set("polling mask 0x%x / %u ms", RUSD_POLL_MASK, RUSD_POLL_INTERVAL_MS);
    return true;
}

/* ---- RM sensors, read from the kernel through the driver's internal client -------------------------
 *
 * The daemon used to do this from user space with its own RM client. The internal client accepts the same
 * NV2080 controls, so all of it now happens here and the daemon is gone. Buffers are static (the CLK and
 * UTIL responses are ~4 KB, too large for the kernel stack).
 */

struct ThermInstr { UInt32 result, executed, opcode, operands[8]; };
struct ThermParams {
    UInt32 clientAPIVersion, clientAPIRevision, clientInstructionSizeOf, executeFlags;
    UInt32 successfulInstructions, instructionListSize;
    ThermInstr instructionList[32];
};

struct PmuSuper { UInt32 tailIndex, sequenceId, headIndex, numSamples, resetCount, retrievalMode; };
struct ClkSample { UInt64 ts; UInt32 gpcKHz, dramKHz, nvdKHz, activeGpcKHz; };
struct ClkParams { PmuSuper super; ClkSample samples[250]; };

struct UtilEngine { UInt32 util, vgpuScale, procId, subProcessID; char name[100]; UInt64 pOsPidInfo; };
struct UtilSample { UInt64 ts; UtilEngine fb, gr, nvenc, nvdec, nvjpg, nvofa; };
struct UtilParams { UInt8 type; UInt32 bufSize, count, tracker; UtilSample samples[72]; };

static ClkParams gClkParams;
static UtilParams gUtilParams;

static bool nvsmc_rm_call_gpu(void *pGpu, UInt32 cmd, void *params, UInt32 size, int *statusOut)
{
    UInt8 *base = (UInt8 *)pGpu;
    UInt32 hClient = *(volatile UInt32 *)(base + OFF_HINTERNAL_CLIENT);
    UInt32 hSubdevice = *(volatile UInt32 *)(base + OFF_HINTERNAL_SUBDEVICE);
    UInt64 control = *(volatile UInt64 *)(base + OFF_RMAPI_CONTROL);
    if (!hClient || !hSubdevice || !control) {
        if (statusOut) *statusOut = -1;
        return false;
    }
    typedef int (*rm_control_t)(void *pRmApi, UInt32 hClient, UInt32 hObject, UInt32 cmd, void *pParams, UInt32 size);
    rm_control_t fn = (rm_control_t)(uintptr_t)control;
    int status = nvsens_rm_call([&] {
        return fn((void *)(base + OFF_PHYSICAL_RMAPI), hClient, hSubdevice, cmd, params, size);
    });
    if (statusOut) *statusOut = status;
    return status == 0;
}

/// THERMAL_SYSTEM_EXECUTE_V2, instruction GET_SENSOR_READING on sensor 0 (GPU core).
static int nvsmc_rm_temperature(void *pGpu, int *statusOut)
{
    ThermParams params;
    memset(&params, 0, sizeof params);
    params.clientAPIVersion = 1;
    params.clientInstructionSizeOf = sizeof(ThermInstr);
    params.instructionListSize = 1;
    params.instructionList[0].opcode = 0x00001500u;
    params.instructionList[0].operands[0] = 0;
    if (!nvsmc_rm_call_gpu(pGpu, 0x20800513u, &params, sizeof params, statusOut))
        return -1;
    if (!params.instructionList[0].executed || params.instructionList[0].result != 0)
        return -1;
    return (int)params.instructionList[0].operands[1];
}

/// PMUMON clock domains: newest sample carries GPC / DRAM / NVD / active clocks in kHz.
static bool nvsmc_rm_clocks(void *pGpu)
{
    memset(&gClkParams, 0, sizeof gClkParams);
    if (!nvsmc_rm_call_gpu(pGpu, 0x20801037u, &gClkParams, sizeof gClkParams, nullptr))
        return false;
    UInt32 n = gClkParams.super.numSamples;
    if (n == 0)
        return false;
    if (n > 250) n = 250;
    const ClkSample &x = gClkParams.samples[n - 1];
    nvggpcclk = (int)(x.gpcKHz / 1000);
    nvgdramclk = (int)(x.dramKHz / 1000);
    nvgnvdclk = (int)(x.nvdKHz / 1000);
    nvgactivegpc = (int)(x.activeGpcKHz / 1000);
    return true;
}

/// Per-engine utilisation ring; the daemon used the newest timestamp, not index 0.
static bool nvsmc_rm_util(void *pGpu)
{
    gUtilParams.type = 2;
    gUtilParams.bufSize = (UInt32)(sizeof(UtilSample) * 72);
    if (!nvsmc_rm_call_gpu(pGpu, 0x20802096u, &gUtilParams, sizeof gUtilParams, nullptr))
        return false;
    UInt64 best = 0;
    UInt32 idx = 0;
    for (UInt32 i = 0; i < 72; i++) {
        if (gUtilParams.samples[i].ts > best) { best = gUtilParams.samples[i].ts; idx = i; }
    }
    const UtilSample &x = gUtilParams.samples[idx];
    nvgutil = (int)x.gr.util;
    nvgutilfb = (int)x.fb.util;
    nvgutilenc = (int)x.nvenc.util;
    nvgutildec = (int)x.nvdec.util;
    return true;
}

/// Framebuffer memory: RUSD carries the total and free byte counts, no RM control needed.
static bool nvsmc_rusd_mem(const volatile UInt8 *b)
{
    if (!b)
        return false;
    for (int attempt = 0; attempt < 8; attempt++) {
        UInt64 total = *(volatile UInt64 *)(b + RUSD_OFF_MEM_TOTAL);
        UInt64 free_ = *(volatile UInt64 *)(b + RUSD_OFF_MEM_FREE);
        __sync_synchronize();
        if (total != *(volatile UInt64 *)(b + RUSD_OFF_MEM_TOTAL))
            continue;
        if (total < (1ull << 30) || total > (1ull << 40) || free_ > total)
            return false;
        nvgvramtotal = (int)(total >> 20);
        nvgvramused = (int)((total - free_) >> 20);
        return true;
    }
    return false;
}

/// PCIe link speed and negotiated width come from the display node's IOPCIExpressLinkStatus property,
/// which IOPCIDevice already decodes for us: bits 3:0 = speed (1..6), bits 9:4 = width.
/// Reads a numeric PCI property that macOS may publish as an OSNumber or as little-endian OSData.
static bool nvsmc_pci_prop(IOService *svc, const char *key, UInt32 *out)
{
    if (OSNumber *n = OSDynamicCast(OSNumber, svc->getProperty(key))) {
        *out = n->unsigned32BitValue();
        return true;
    }
    if (OSData *d = OSDynamicCast(OSData, svc->getProperty(key))) {
        const void *bytes = d->getBytesNoCopy();
        switch (d->getLength()) {
        case 2: *out = *(const UInt16 *)bytes; return true;
        case 3: *out = ((const UInt8 *)bytes)[0] | ((UInt32)((const UInt8 *)bytes)[1] << 8) |
                        ((UInt32)((const UInt8 *)bytes)[2] << 16); return true;
        case 4: *out = *(const UInt32 *)bytes; return true;
        default: return false;
        }
    }
    return false;
}

/// PCIe link speed and negotiated width, taken from the display node's PCIe registers.
///
/// Two sources are tried in order because laptops often park the dGPU: the link status word gives the
/// negotiated speed and width, and when the link is down (status 0) the capabilities word still reports the
/// maximum the slot supports, which is better than showing nothing. Devices are picked by IOName "display"
/// first and then by vendor 0x10de, so a card whose node is not named "display" still works.
static bool nvsmc_pci_link_from_registry()
{
    UInt32 bestSpeed = 0, bestWidth = 0;
    int bestRank = 0;   /* 1 = NVIDIA device, 2 = NVIDIA display-class device, 3 = IOName "display" */

    if (OSDictionary *m = IOService::serviceMatching("IOPCIDevice")) {
        if (OSIterator *it = IOService::getMatchingServices(m)) {
            while (IOService *pci = OSDynamicCast(IOService, it->getNextObject())) {
                int rank = 0;
                if (OSString *name = OSDynamicCast(OSString, pci->getProperty("IOName")))
                    if (name->isEqualTo("display"))
                        rank = 3;

                if (rank < 3) {
                    UInt32 vendor = 0, classCode = 0;
                    if (nvsmc_pci_prop(pci, "vendor-id", &vendor) && vendor == 0x10de) {
                        rank = 1;
                        if (nvsmc_pci_prop(pci, "class-code", &classCode) && (classCode >> 16) == 0x03)
                            rank = 2;
                    }
                }
                if (rank == 0 || rank < bestRank)
                    continue;

                UInt32 status = 0, caps = 0;
                bool haveStatus = nvsmc_pci_prop(pci, "IOPCIExpressLinkStatus", &status);
                bool haveCaps = nvsmc_pci_prop(pci, "IOPCIExpressLinkCapabilities", &caps);

                UInt32 speed = 0, width = 0;
                if (haveStatus) {
                    UInt32 s = status & 0xfu, w = (status >> 4) & 0x3fu;
                    if (s >= 1 && s <= 6 && w >= 1 && w <= 32) { speed = s; width = w; }
                }
                if (width == 0 && haveCaps) {
                    UInt32 s = caps & 0xfu, w = (caps >> 4) & 0x3fu;
                    if (s >= 1 && s <= 6 && w >= 1 && w <= 32) { speed = s; width = w; }
                }
                if (width == 0)
                    continue;
                bestRank = rank;
                bestSpeed = speed;
                bestWidth = width;
            }
            it->release();
        }
        m->release();
    }

    if (bestWidth == 0)
        return false;
    if (bestSpeed >= 1 && bestSpeed <= 6)
        nvgpupciegen = (int)bestSpeed;
    nvgpupciewidth = (int)bestWidth;
    return true;
}

/// Perf p-state (0 = P0 …).
static bool nvsmc_rm_pstate(void *pGpu)
{
    UInt32 ps = 0;
    if (!nvsmc_rm_call_gpu(pGpu, 0x20802068u, &ps, sizeof ps, nullptr))
        return false;
    nvgpstate = (int)ps;
    return true;
}

static inline bool nvsens_ts_valid(UInt64 ts)
{
    return ts != RUSD_TIMESTAMP_INVALID && ts != RUSD_TIMESTAMP_WRITE_IN_PROGRESS && ts < RUSD_SEQ_START;
}

static bool nvsens_read_polled(const volatile UInt8 *base, UInt32 off, void *dst, UInt32 len, UInt64 *pTs)
{
    for (int attempt = 0; attempt < 10; attempt++) {
        UInt64 before = *(volatile UInt64 *)(base + off);
        if (!nvsens_ts_valid(before))
            return false;
        memcpy(dst, (const void *)(base + off), len);
        __sync_synchronize();
        UInt64 after = *(volatile UInt64 *)(base + off);
        if (before == after) {
            if (pTs)
                *pTs = after;
            return true;
        }
    }
    return false;
}

void SMCNvidiaGPU::readRusd()
{
    if (!fRusd)
        return;
    const volatile UInt8 *b = fRusd;
    bool anyNew = false;
    UInt64 now = nvsens_now_ns();

    struct { UInt64 ts; UInt32 gpu, module, memory, pad; } power;
    if (nvsens_read_polled(b, RUSD_OFF_AVG_POWER, &power, sizeof(power), &power.ts)) {
        if (power.ts != fTsPower) { fTsPower = power.ts; anyNew = true; }
        if (power.gpu < 2000000u) nvgpupower = (int)power.gpu;
        if (power.module < 2000000u) nvgpupower_mod = (int)power.module;
        if (power.memory < 2000000u) nvgpupower_mem = (int)power.memory;
    }
    struct { UInt64 ts; UInt32 requested, enforced; } limit;
    if (nvsens_read_polled(b, RUSD_OFF_POWER_LIMIT, &limit, sizeof(limit), &limit.ts)) {
        if (limit.ts != fTsLimit) { fTsLimit = limit.ts; anyNew = true; }
        if (limit.requested < 2000000u) nvgpupwrlimit_req = (int)limit.requested;
        if (limit.enforced < 2000000u) nvgpupwrlimit = (int)limit.enforced;
    }
    struct { UInt64 ts; UInt32 tgp, pad; } policy;
    if (nvsens_read_polled(b, RUSD_OFF_POWER_POLICY, &policy, sizeof(policy), &policy.ts)) {
        if (policy.ts != fTsPolicy) { fTsPolicy = policy.ts; anyNew = true; }
        if (policy.tgp < 2000000u) nvgputgp = (int)policy.tgp;
    }
    struct { UInt64 ts; UInt32 rpm[RUSD_FAN_MAX_COOLERS]; } fan;
    if (nvsens_read_polled(b, RUSD_OFF_FAN, &fan, sizeof(fan), &fan.ts)) {
        if (fan.ts != fTsFan) { fTsFan = fan.ts; anyNew = true; }
        UInt32 max = 0;
        int lastValid = -1;
        for (int i = 0; i < 8; i++)
            nvgpufans[i] = 0;
        /* Keep the slot order: a fan that is momentarily stopped reads 0 but still occupies its index, so a
         * three-fan card keeps three rows instead of dropping the idle one. */
        for (UInt32 i = 0; i < RUSD_FAN_MAX_COOLERS && i < 8; i++) {
            UInt32 rpm = fan.rpm[i];
            if (rpm == 0xffffffffu || rpm > 30000u)
                rpm = 0;
            if (rpm > 0) {
                lastValid = (int)i;
                nvgpufans[i] = (int)rpm;
                if (rpm > max)
                    max = rpm;
            }
        }
        {
            size_t used = 0;
            nvgpufanraw[0] = 0;
            for (UInt32 i = 0; i < RUSD_FAN_MAX_COOLERS && i < 16; i++) {
                UInt32 rpm = fan.rpm[i];
                if (rpm == 0xffffffffu || rpm > 100000u)
                    rpm = 0;
                int n = snprintf(nvgpufanraw + used, sizeof nvgpufanraw - used, "%s%u", i ? " " : "", rpm);
                if (n <= 0 || (size_t)n >= sizeof nvgpufanraw - used)
                    break;
                used += (size_t)n;
            }
        }
        int count = lastValid + 1;
        if (count > nvgpufan_known_count)
            nvgpufan_known_count = count;
        nvgpufancount = nvgpufan_known_count;
        nvgpufan = (int)max;
        if (max > 0) { nvgpufan_last = (int)max; nvgpufan_last_ns = now; }
    }
    struct { UInt64 ts; UInt32 pstate, pad; } pstate;
    if (nvsens_read_polled(b, RUSD_OFF_PSTATE, &pstate, sizeof(pstate), &pstate.ts)) {
        if (pstate.ts != fTsPstate) { fTsPstate = pstate.ts; anyNew = true; }
    }
    struct { UInt64 ts; UInt32 info[8]; } clk;
    if (nvsens_read_polled(b, RUSD_OFF_CLK_DOMAINS, &clk, sizeof(clk), &clk.ts)) {
        if (clk.ts != fTsClk) { fTsClk = clk.ts; anyNew = true; }
        UInt32 gfxKHz = clk.info[RUSD_CLK_DOMAIN_GRAPHICS * 2 + 1];
        UInt32 smKHz = clk.info[RUSD_CLK_DOMAIN_SM * 2 + 1];
        if (gfxKHz && gfxKHz < 10000000u) nvgpugfxclk = (int)(gfxKHz / 1000u);
        if (smKHz && smKHz < 10000000u) nvgpusmclk = (int)(smKHz / 1000u);
    }
    for (int i = 0; i < 5; i++) {
        struct { UInt64 ts; SInt32 temp; UInt32 pad; } temp;
        if (!nvsens_read_polled(b, RUSD_OFF_TEMPERATURES + (UInt32)i * RUSD_TEMP_STRIDE, &temp, sizeof(temp), &temp.ts))
            continue;
        if (i == 0 && temp.ts != fTsTemp) { fTsTemp = temp.ts; anyNew = true; }
        if (temp.temp > 0 && temp.temp < 150) nvgpurusdtemps[i] = (int)temp.temp;
    }
    nvgpurusdtemp = nvgpurusdtemps[0];

    struct { UInt64 ts; UInt32 reasonMask, pad; } thr;
    if (nvsens_read_polled(b, RUSD_OFF_CLK_THROTTLE, &thr, sizeof(thr), &thr.ts)) {
        if (thr.ts != fTsThrottle) { fTsThrottle = thr.ts; anyNew = true; }
        nvgputhrottle = (int)thr.reasonMask;
    }

    struct { UInt64 ts; UInt32 gpu, module, cpu; } inst;
    if (nvsens_read_polled(b, RUSD_OFF_INST_POWER, &inst, sizeof(inst), &inst.ts)) {
        if (inst.ts != fTsInst) { fTsInst = inst.ts; anyNew = true; }
        if (inst.gpu < 2000000u) nvgpuinstpower = (int)inst.gpu;
        if (inst.module < 2000000u) nvgpuinstpower_mod = (int)inst.module;
        if (inst.cpu < 2000000u) nvgpuinstpower_cpu = (int)inst.cpu;
    }

    struct { UInt64 ts; UInt32 data[RUSD_PCI_DATA_COUNT]; } pci;
    if (nvsens_read_polled(b, RUSD_OFF_PCI_BUS_DATA, &pci, sizeof(pci), &pci.ts)) {
        if (pci.ts != fTsPci) { fTsPci = pci.ts; anyNew = true; }
        nvgpudbg_pci = 1;
        UInt32 raw = pci.data[0];
        nvgpupcieraw = (int)raw;
        /* NV2080_CTRL_BUS_INFO_PCIE_LINK_CAP_* layout (ctrl2080bus.h):
         *   bits 3:0   MAX_SPEED   1=2.5GT 2=5GT 3=8GT 4=16GT 5=32GT 6=64GT
         *   bits 9:4   MAX_WIDTH   (lanes)
         *   bits 15:12 GEN        (0 = Gen1 … 4 = Gen5)
         *   bits 19:16 CURR_LEVEL (0 = Gen1 … 4 = Gen5)
         *   bits 23:20 GPU_GEN
         * On this machine the word is 0x00444000: CURR_LEVEL = 4 → Gen5, i.e. the link really runs at
         * 5.0 x16 (same as GPU-Z reported under Windows). The lane count is not in this word; nvstat's
         * "bus" command asks RM (NV2080_CTRL_CMD_BUS_GET_INFO_V2) for the link status word instead. */
        UInt32 cur = (raw >> 16) & 0xfu;
        UInt32 gen = (raw >> 12) & 0xfu;
        if (cur <= 5) nvgpupciegen = (int)cur + 1;
        else if (gen <= 5) nvgpupciegen = (int)gen + 1;
        UInt32 width = (raw >> 4) & 0x3fu;
        if (width >= 1 && width <= 32) {
            nvgpupciewidth = (int)width;
            nvgpupciewidthmax = (int)width;
        }
        for (int i = 0; i < 8; i++) nvgpupcie_errs[i] = (int)pci.data[i + 1];
    }

    struct { UInt64 ts; UInt64 counts[3][4]; UInt64 info; } ecc;
    if (nvsens_read_polled(b, RUSD_OFF_MEM_ECC, &ecc, sizeof(ecc), &ecc.ts)) {
        if (ecc.ts != fTsEcc) { fTsEcc = ecc.ts; anyNew = true; }
        nvgpudbg_ecc = 1;
        UInt64 corr = ecc.counts[1][0] + ecc.counts[1][1];
        UInt64 uncorr = ecc.counts[1][2] + ecc.counts[1][3];
        nvgpuecccorr = (int)(corr > 0x7fffffffULL ? 0x7fffffff : corr);
        nvgpueccuncorr = (int)(uncorr > 0x7fffffffULL ? 0x7fffffff : uncorr);
    }

    struct { UInt64 ts; UInt32 size, avail; } bar1;
    if (nvsens_read_polled(b, RUSD_OFF_BAR1_INFO, &bar1, sizeof(bar1), &bar1.ts)) {
        nvgpudbg_bar1 = 1;
        if (bar1.size > 0) {
            nvgpubar1total = (int)(bar1.size >> 20);
            nvgpubar1free = (int)(bar1.avail >> 20);
        }
    }
    if (anyNew)
        fChangedNs = now;
    nvgpulive = (fChangedNs != 0 && now - fChangedNs < 3000000000ull) ? 1 : 0;
}

/* Publish the VBIOS version the way AMD drivers do it: System Information's display reporter shows a
 * "ROM Revision" line when the display service (IOFramebuffer) or its provider carries a "rom-revision"
 * (or "ATY,Rom#") property. The version itself is not exposed by NVRM, so read it straight out of the GPU's
 * expansion ROM: mapDeviceMemoryWithRegister / setMemoryEnable are virtual IOPCIDevice methods, so they do
 * not need the class symbols that a third-party kext cannot link against. */
static IOService *fVbiosGpuNode = nullptr;
static bool nvsmc_publish_vbios()
{
    if (nvgpuvbios[0])
        return true;
    OSDictionary *m = IOService::serviceMatching("IOPCIDevice");
    if (!m)
        return false;
    OSIterator *it = IOService::getMatchingServices(m);
    m->release();
    if (!it)
        return false;
    IOService *gpu = nullptr;
    while (IOService *dev = OSDynamicCast(IOService, it->getNextObject())) {
        OSData *v = OSDynamicCast(OSData, dev->getProperty("vendor-id"));
        if (v && v->getLength() >= 4 && *(const UInt32 *)v->getBytesNoCopy() == 0x10de) { gpu = dev; break; }
    }
    it->release();
    if (!gpu)
        return false;

    IOPCIDevice *pci = (IOPCIDevice *)gpu;      /* matched by class name above */
    pci->setMemoryEnable(true);
    /* NOTE: do not touch config space here — configRead32/configWrite32 expand to
     * IOPCIDevice::extendedConfigRead32/extendedConfigWrite32, which the kernel does not export to
     * third-party kexts (the kext would fail to load). Mapping the ROM only works when firmware left the
     * ROM BAR assigned; when it did not, use kern.nvgpuvbios_override instead. */
    IOMemoryMap *map = pci->mapDeviceMemoryWithRegister(kIOPCIConfigExpansionROMBase);
    if (!map) {
        nvgpurdrom |= 8;
        return false;
    }
    nvgpurdrom |= 1;
    const UInt8 *rom = (const UInt8 *)map->getVirtualAddress();
    UInt32 len = (UInt32)map->getLength();
    char ver[48] = {0};
    if (rom && len > 0x400 && rom[0] == 0x55 && rom[1] == 0xaa) {
        nvgpurdrom |= 2;
        UInt32 lim = len < 0x20000 ? len : 0x20000;
        for (UInt32 i = 0; i + 8 < lim && !ver[0]; i++) {
            if (memcmp(rom + i, "Version ", 8) != 0)
                continue;
            for (UInt32 j = 0; j < sizeof(ver) - 1 && i + 8 + j < lim; j++) {
                char c = (char)rom[i + 8 + j];
                if (c < 0x20 || c > 0x7e)
                    break;
                ver[j] = c;
            }
        }
    }
    map->release();
    if (!ver[0]) {
        SMCNVG_LOG("expansion ROM mapped but no version string found");
        return false;
    }
    nvgpurdrom |= 4;
    nvsmc_strcpy(nvgpuvbios, ver, sizeof(nvgpuvbios));

    fVbiosGpuNode = gpu;   /* publish_rom_property() uses it */
    if (OSString *s = OSString::withCString(ver)) {
        gpu->setProperty("rom-revision", s);
        gpu->setProperty("vbios-version", s);
        s->release();
    }
    /* also tag the framebuffers: that is where the display reporter reads the property from on real Macs */
    if (OSDictionary *fm = IOService::serviceMatching("IOFramebuffer")) {
        if (OSIterator *fit = IOService::getMatchingServices(fm)) {
            while (IOService *fb = OSDynamicCast(IOService, fit->getNextObject())) {
                if (OSString *s = OSString::withCString(ver)) {
                    fb->setProperty("rom-revision", s);
                    fb->setProperty("ATY,Rom#", s);
                    s->release();
                }
            }
            fit->release();
        }
        fm->release();
    }
    return true;
}

/* Push the VBIOS string to the nodes System Information reads: the GPU PCI device and every framebuffer. */
/* iStat Menus labels GPU clocks as "<model> Core" / "<model> Memory"; with the full marketing name the
 * suffix is clipped by its column width. Drop the "GeForce" word so the label fits — on any card, not just
 * one particular model: "NVIDIA GeForce RTX 5080" becomes "NVIDIA RTX 5080", "NVIDIA GeForce GTX 1660"
 * becomes "NVIDIA GTX 1660". Models without that word are left untouched. */
static void nvsmc_shorten_gpu_model()
{
    static const char kGeForce[] = " GeForce ";
    if (OSDictionary *m = IOService::serviceMatching("IOPCIDevice")) {
        if (OSIterator *it = IOService::getMatchingServices(m)) {
            while (IOService *pci = OSDynamicCast(IOService, it->getNextObject())) {
                OSString *name = OSDynamicCast(OSString, pci->getProperty("IOName"));
                if (!name || !name->isEqualTo("display"))
                    continue;
                OSData *cur = OSDynamicCast(OSData, pci->getProperty("model"));
                const char *full = cur ? (const char *)cur->getBytesNoCopy() : nullptr;
                if (!full)
                    continue;
                strlcpy(nvgpumodel, full, sizeof nvgpumodel);
                const char *hit = strstr(full, kGeForce);
                if (!hit)
                    continue;                       /* already short, or a different naming scheme */
                size_t head = (size_t)(hit - full);
                const char *tail = hit + sizeof(kGeForce) - 1;
                char shortName[160];
                if (head + 1 + strlen(tail) >= sizeof shortName)
                    continue;
                memcpy(shortName, full, head);
                shortName[head] = ' ';
                strlcpy(shortName + head + 1, tail, sizeof shortName - head - 1);
                if (OSData *d = OSData::withBytes(shortName, (unsigned)strlen(shortName) + 1))
                    pci->setProperty("model", d);
                SMCNVG_LOG("gpu model shortened to %s", shortName);
            }
            it->release();
        }
        m->release();
    }
}

static void nvsmc_publish_rom_property()
{
    if (!nvgpuvbios[0])
        return;
    OSString *s = OSString::withCString(nvgpuvbios);
    if (!s)
        return;
    /* The display controller's IOPCIDevice is the node the display reporter walks up to; matching by
     * vendor-id alone can land on the HDMI audio function (same vendor). Walk up from the accelerator. */
    IOService *accel = nullptr;
    if (OSDictionary *am = IOService::serviceMatching("NVAccel")) {
        if (OSIterator *ait = IOService::getMatchingServices(am)) {
            accel = OSDynamicCast(IOService, ait->getNextObject());
            ait->release();
        }
        am->release();
    }
    IORegistryEntry *pci = accel;
    while (pci) {
        OSString *cls = OSDynamicCast(OSString, pci->getProperty("IOClass"));
        if (cls && cls->isEqualTo("IOPCIDevice"))
            break;
        pci = pci->getParentEntry(gIOServicePlane);
    }
    if (pci) {
        pci->setProperty("rom-revision", s);
        pci->setProperty("vbios-version", s);
        SMCNVG_LOG("published rom-revision on %s", "IOPCIDevice");
    }
    if (fVbiosGpuNode && fVbiosGpuNode != pci) {
        fVbiosGpuNode->setProperty("rom-revision", s);
        fVbiosGpuNode->setProperty("vbios-version", s);
    }
    if (OSDictionary *fm = IOService::serviceMatching("IOFramebuffer")) {
        if (OSIterator *fit = IOService::getMatchingServices(fm)) {
            while (IOService *fb = OSDynamicCast(IOService, fit->getNextObject())) {
                fb->setProperty("rom-revision", s);
                fb->setProperty("ATY,Rom#", s);
            }
            fit->release();
        }
        fm->release();
    }
    s->release();
}

IOService *SMCNvidiaGPU::accel()
{
    if (fAccel)
        return fAccel;
    OSDictionary *m = IOService::serviceMatching("NVAccel");
    if (!m)
        return nullptr;
    OSIterator *it = IOService::getMatchingServices(m);
    m->release();
    if (!it)
        return nullptr;
    IOService *svc = OSDynamicCast(IOService, it->getNextObject());
    if (svc) {
        svc->retain();
        fAccel = svc;
    }
    it->release();
    return fAccel;
}

void SMCNvidiaGPU::publish()
{
    IOService *svc = accel();
    if (!svc)
        return;
    /* Merge into what the accelerator already publishes: NVAccel's own PerformanceStatistics carries the
     * driver's VRAM numbers and other keys that consumers (AMD Power Gadget in particular) expect, and
     * replacing the dictionary outright drops them. */
    OSDictionary *d = nullptr;
    if (OSDictionary *cur = OSDynamicCast(OSDictionary, svc->getProperty("PerformanceStatistics")))
        d = OSDictionary::withDictionary(cur, 32);
    if (!d)
        d = OSDictionary::withCapacity(24);
    if (!d)
        return;

    int temp = nvsmc_current_temp();
    if (temp >= 1 && temp <= 125) {
        nvsmc_set_num(d, "Temperature(C)", (unsigned long long)temp);
        nvsmc_set_num(d, "GPU Temperature(C)", (unsigned long long)temp);
    }
    if (nvggpcclk > 0) {
        nvsmc_set_num(d, "Core Clock(MHz)", (unsigned long long)nvggpcclk);
        nvsmc_set_num(d, "GPU Core Clock(MHz)", (unsigned long long)nvggpcclk);
    }
    if (nvgdramclk > 0) {
        nvsmc_set_num(d, "Memory Clock(MHz)", (unsigned long long)nvgdramclk);
        nvsmc_set_num(d, "GPU Memory Clock(MHz)", (unsigned long long)nvgdramclk);
    }
    if (nvgnvdclk > 0) nvsmc_set_num(d, "GPU NVD Clock(MHz)", (unsigned long long)nvgnvdclk);
    if (nvgactivegpc > 0) nvsmc_set_num(d, "GPU Active Core Clock(MHz)", (unsigned long long)nvgactivegpc);

    if (nvgutil >= 0) {
        unsigned long long pct = (unsigned long long)(nvgutil / 100);
        nvsmc_set_num(d, "Device Utilization %", pct);
        nvsmc_set_num(d, "Device Utilization % at cur p-state", pct);
        nvsmc_set_num(d, "GPU Core Utilization", pct);
        nvsmc_set_num(d, "GPU Activity(%)", pct);
    }
    int eng[5] = { nvgutilfb, nvgutilenc, nvgutildec, nvgutiljpg, nvgutilofa };
    const char *engKeys[5] = { "GPU FB Utilization(%)", "GPU Encoder Utilization(%)",
                               "GPU Decoder Utilization(%)", "GPU JPEG Utilization(%)", "GPU OFA Utilization(%)" };
    for (int i = 0; i < 5; i++)
        if (eng[i] >= 0)
            nvsmc_set_num(d, engKeys[i], (unsigned long long)(eng[i] / 100));
    if (nvgpstate >= 0)
        nvsmc_set_num(d, "GPU Power State", (unsigned long long)nvgpstate);
    /* The accelerator publishes vramUsedBytes / vramTotalBytes / inUseVidMemoryBytes itself; leave those to
     * it (they are 64-bit byte counts, and a wrong one here makes consumers such as AMD Power Gadget give
     * up on the GPU). Only the MB figure used by iStat's own rows is added, if the driver did not. */
    if (nvgvramused > 0 && !OSDynamicCast(OSNumber, d->getObject("GPU Memory Used(MB)")))
        nvsmc_set_num(d, "GPU Memory Used(MB)", (unsigned long long)nvgvramused);
    if (nvgvramtotal > 0) {
        /* byte counts must be 64-bit numbers; a truncated one is what made AMD Power Gadget drop the GPU */
        if (!OSDynamicCast(OSNumber, d->getObject("vramTotalBytes")))
            nvsmc_set_num(d, "vramTotalBytes", ((unsigned long long)nvgvramtotal) << 20, 64);
        if (!OSDynamicCast(OSNumber, d->getObject("vramUsedBytes")))
            nvsmc_set_num(d, "vramUsedBytes", ((unsigned long long)nvgvramused) << 20, 64);
        if (!OSDynamicCast(OSNumber, d->getObject("inUseVidMemoryBytes")))
            nvsmc_set_num(d, "inUseVidMemoryBytes", ((unsigned long long)nvgvramused) << 20, 64);
    }

    if (nvgpupower > 0) {
        nvsmc_set_num(d, "Total Power(W)", ((unsigned long long)nvgpupower + 500) / 1000);
        nvsmc_set_num(d, "GPU Power(W)", ((unsigned long long)nvgpupower + 500) / 1000);
        nvsmc_set_num(d, "RUSD Power(mW)", (unsigned long long)nvgpupower);
    }
    if (nvgputgp > 0) nvsmc_set_num(d, "TGP(W)", ((unsigned long long)nvgputgp + 500) / 1000);
    if (nvgpupwrlimit > 0) nvsmc_set_num(d, "Power Limit(W)", ((unsigned long long)nvgpupwrlimit + 500) / 1000);
    if (nvgpuinstpower > 0) nvsmc_set_num(d, "Instant Power(W)", ((unsigned long long)nvgpuinstpower + 500) / 1000);
    if (nvgpupciegen > 0) {
        nvsmc_set_num(d, "PCIe Gen", (unsigned long long)nvgpupciegen);
        if (nvgpupciewidth > 0)
            nvsmc_set_num(d, "PCIe Width", (unsigned long long)nvgpupciewidth);
    }
    if (nvgputhrottle >= 0) nvsmc_set_num(d, "Throttle Reasons", (unsigned long long)nvgputhrottle);
    if (nvgpuecccorr > 0) nvsmc_set_num(d, "ECC Corrected", (unsigned long long)nvgpuecccorr);
    if (nvgpueccuncorr > 0) nvsmc_set_num(d, "ECC Uncorrected", (unsigned long long)nvgpueccuncorr);
    if (nvgpubar1total > 0) {
        nvsmc_set_num(d, "BAR1 Total(MB)", (unsigned long long)nvgpubar1total);
        nvsmc_set_num(d, "BAR1 Free(MB)", (unsigned long long)nvgpubar1free);
    }
    /* Fan: a transient zero reading (or a missed poll) must not make the keys disappear for consumers that
     * re-read the dictionary on their own schedule (AMD Power Gadget drops the row when the key is missing),
     * so hold the last real reading for a minute. */
    int fan = nvgpufan;
    if (fan == 0 && nvgpufan_last != 0 && (nvsens_now_ns() - nvgpufan_last_ns) < 60000000000ull)
        fan = nvgpufan_last;
    if (fan > 0) {
        int fanmax = nvgpufanmax > 0 ? nvgpufanmax : 3500;
        unsigned long long pct = ((unsigned long long)fan * 100ull + fanmax / 2ull) / (unsigned long long)fanmax;
        if (pct > 100ull) pct = 100ull;
        nvsmc_set_num(d, "Fan Speed(RPM)", (unsigned long long)fan);
        nvsmc_set_num(d, "fan-rpm", (unsigned long long)fan);
        nvsmc_set_num(d, "GPU Fan Speed(RPM)", (unsigned long long)fan);
        nvsmc_set_num(d, "Fan RPM", (unsigned long long)fan);
        nvsmc_set_num(d, "GPU Fan RPM", (unsigned long long)fan);
        nvsmc_set_num(d, "FanSpeed", (unsigned long long)fan);
        nvsmc_set_num(d, "Fan Speed", (unsigned long long)fan);
        /* iStat's CPU panel reads the fan percentage keys; they are also what makes its GPU panel show a
         * second fan row in percent form. */
        nvsmc_set_num(d, "Fan Speed(%)", pct);
        nvsmc_set_num(d, "fan-percentage", pct);
        nvsmc_set_num(d, "FanMaxRPM", (unsigned long long)fanmax);
        nvsmc_set_num(d, "Fan Count", (unsigned long long)nvgpufancount);
        nvsmc_set_num(d, "FanCount", (unsigned long long)nvgpufancount);
    }

    svc->setProperty("PerformanceStatistics", d);
    if (fPubPrev) { fPubPrev->release(); fPubPrev = nullptr; }
    fPubPrev = fPub;
    fPub = d;   /* keeps the dictionary alive for one more update: readers take the property unretained */
}

bool SMCNvidiaGPU::vsmcNotificationHandler(void *self, void *refCon, IOService *vsmc, IONotifier *notifier)
{
    if (!self || !vsmc) return false;
    auto *me = static_cast<SMCNvidiaGPU *>(self);
    auto ret = vsmc->callPlatformFunction(VirtualSMCAPI::SubmitPlugin, true, me, &me->vsmcPlugin, nullptr, nullptr);
    if (ret == kIOReturnSuccess) { me->setProperty("SMCSubmitted", kOSBooleanTrue); return true; }
    return false;
}

void SMCNvidiaGPU::tick()
{
    if (!fBound && nvrm_bind())
        fBound = true;

    if (fBound) {
        if (!fGpu) {
            if (driverReady()) {
                for (unsigned int instance = 0; instance < 8 && !fGpu; instance++) {
                    void *candidate = gApi.gpu(instance);
                    if (candidate)
                        fGpu = candidate;
                }
                if (fGpu)
                    SMCNVG_LOG("OBJGPU %p", fGpu);
                else
                    nvsens_state_set("waiting for NVRM to attach the GPU");
            }
        }
        if (fGpu && (fRusd || ensureRusd())) {
            if ((!nvgpupoll && (fTicks - fLastPoll) >= 4) ||
                (nvgpupoll && !nvgpulive && fTsPower != 0 && (fTicks - fLastPoll) >= 30)) {
                fLastPoll = fTicks;
                requestPoll();
            }
            readRusd();
    if ((fTicks % 20) == 7) {
        if (fGpu)
            nvsmc_shorten_gpu_model();
        if (nvgpupciewidth == 0)
            nvsmc_pci_link_from_registry();
    }

    /* RM-side sensors: temperature and utilisation every 500 ms, clocks and VRAM every second. */
    if (fGpu && nvgpulayout) {
        if ((fTicks % 2) == 0) {
            int status = 0;
            int t = nvsmc_rm_temperature(fGpu, &status);
            if (t >= 1 && t <= 125) nvgputemp = t;
            nvsmc_rm_util(fGpu);
        }
        if ((fTicks % 4) == 0) {
            nvsmc_rm_clocks(fGpu);
            nvsmc_rm_pstate(fGpu);
        }
        if ((fTicks % 2) == 0)
            nvsmc_rusd_mem(fRusd);
    }

    SMCNVG_DBG("tick %u temp %d power %d inst %d tgp %d fan %d/%d util %d pstate %d live %d",
               (unsigned)fTicks, nvgputemp, nvgpupower, nvgpuinstpower, nvgputgp,
               nvgpufans[0], nvgpufans[1], nvgutil / 100, nvgpstate, nvgpulive);
    if ((fTicks % 30) == 2 && fGpu)
        nvsmc_read_pcie_bus(fGpu);
    if (!fVbiosTried || (fTicks % 30) == 0) {
        fVbiosTried = true;
        if (!nvgpuvbios[0]) {
            if (nvgpuvbios_override[0])
                nvsmc_strcpy(nvgpuvbios, nvgpuvbios_override, sizeof(nvgpuvbios));
            else
                nvsmc_publish_vbios();
            if (nvgpuvbios[0])
                nvsmc_publish_rom_property();
    nvsmc_shorten_gpu_model();
        }
    }
            if (nvgpupoll && !nvgpulive && fTsPower == 0 && fRelinkTries < 20 && (fTicks % 20) == 0) {
                fRelinkTries++;
                int status = nvsens_rm_call([this] { return gApi.linkRusd(fGpu); });
                SMCNVG_LOG("re-published the RUSD address to the GSP (0x%x), try %u", status, fRelinkTries);
                nvsens_state_set("relinking rusd to gsp, try %u (0x%x)", fRelinkTries, status);
                nvgpupoll = 0;
            }
        }
    }

    /* sysctl-fed sensors are published even when the driver is not the build the bindings came from */
    publish();
}

void SMCNvidiaGPU::onTimer(OSObject *owner, IOTimerEventSource *src)
{
    SMCNvidiaGPU *me = OSDynamicCast(SMCNvidiaGPU, owner);
    if (!me)
        return;
    me->fTicks++;
    me->tick();
    if (me->fTimer)
        me->fTimer->setTimeoutMS(me->fTicks < 120 ? 500 : 1000);
}

bool SMCNvidiaGPU::start(IOService *provider)
{
    int off = 0;
    int forced = 0;
    if (PE_parse_boot_argn("nvsmcforce", &forced, sizeof(forced)) && forced != 0) {
        nvgpufForceArg = forced;
        SMCNVG_LOG("nvsmcforce=%d boot argument: will bind even without a signature match", forced);
    }
    if (PE_parse_boot_argn("nvsensoff", &off, sizeof(off)) && off != 0) {
        SMCNVG_LOG("nvsensoff=%d boot argument: staying out of the way", off);
        return false;
    }
    if (!IOService::start(provider))
        return false;

    registerSysctls();
    setProperty("SMCNvidiaGPUAlive", kOSBooleanTrue);
    setProperty("NVRMDriverSHA256", NVRM_DRIVER_SHA256);
    nvsens_state_set("started, waiting for NVRM");

    VirtualSMCAPI::addKey(SMC_MAKE_IDENTIFIER('T','G','0','D'), vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp78, new GPUTempValue()));
    VirtualSMCAPI::addKey(SMC_MAKE_IDENTIFIER('T','G','0','P'), vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp78, new GPUTempValue()));
    VirtualSMCAPI::addKey(SMC_MAKE_IDENTIFIER('T','G','0','d'), vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp78, new GPUTempValue()));

    /* GPU fans are published as SMC fan slots 2 and 3, the SuperIO slots that report 0 RPM on this board.
     * VirtualSMCKeystore::loadPlugin() lets a later plugin replace an existing key, which is how these take
     * over those slots. Custom values have to be passed to valueWithFp/valueWithData as `thisValue` so the
     * key owns storage; a value without storage is not registered. */
    for (unsigned int fan = 0; fan < 2; fan++) {
        const char ac[4] = { 'F', (char)('2' + fan), 'A', 'c' };
        const char mn[4] = { 'F', (char)('2' + fan), 'M', 'n' };
        const char mx[4] = { 'F', (char)('2' + fan), 'M', 'x' };
        const char tg[4] = { 'F', (char)('2' + fan), 'T', 'g' };
        const char id[4] = { 'F', (char)('2' + fan), 'I', 'D' };

        VirtualSMCAPI::addKey(SMC_MAKE_IDENTIFIER(ac[0], ac[1], ac[2], ac[3]), vsmcPlugin.data,
                              VirtualSMCAPI::valueWithFp(0, SmcKeyTypeFpe2, new GPUFanSpeedValue(fan)));
        VirtualSMCAPI::addKey(SMC_MAKE_IDENTIFIER(mn[0], mn[1], mn[2], mn[3]), vsmcPlugin.data,
                              VirtualSMCAPI::valueWithFp(0, SmcKeyTypeFpe2));
        VirtualSMCAPI::addKey(SMC_MAKE_IDENTIFIER(mx[0], mx[1], mx[2], mx[3]), vsmcPlugin.data,
                              VirtualSMCAPI::valueWithFp(3500, SmcKeyTypeFpe2));
        (void)tg;   /* F<N>Tg is not published: iStat derives a percentage from target/actual */

        char label[32];
        memset(label, ' ', sizeof label);
        const char *text = fan == 0 ? "GPU Fan 1" : "GPU Fan 2";
        memcpy(label, text, strlen(text));
        VirtualSMCAPI::addKey(SMC_MAKE_IDENTIFIER(id[0], id[1], id[2], id[3]), vsmcPlugin.data,
                              VirtualSMCAPI::valueWithData(reinterpret_cast<const SMC_DATA *>(label),
                                                           sizeof label, SmcKeyTypeCh8s,
                                                           new GPUStringValue(text)));
    }

    VirtualSMCAPI::addKey(SMC_MAKE_IDENTIFIER('T','G','0','p'), vsmcPlugin.data, VirtualSMCAPI::valueWithSp(0, SmcKeyTypeSp78, new GPUTempValue()));
    qsort(const_cast<VirtualSMCKeyValue *>(vsmcPlugin.data.data()), vsmcPlugin.data.size(),
          sizeof(VirtualSMCKeyValue), VirtualSMCKeyValue::compare);
    vsmcNotifier = VirtualSMCAPI::registerHandler(vsmcNotificationHandler, this);
    setProperty("SMCKeys", (unsigned long long)vsmcPlugin.data.size(), 32);

    fWorkloop = IOWorkLoop::workLoop();
    if (!fWorkloop)
        return false;
    fTimer = IOTimerEventSource::timerEventSource(this, &SMCNvidiaGPU::onTimer);
    if (!fTimer || fWorkloop->addEventSource(fTimer) != kIOReturnSuccess)
        return false;
    fTimer->setTimeoutMS(1000);
    return vsmcNotifier != nullptr;
}

void SMCNvidiaGPU::stop(IOService *provider)
{
    if (fTimer) {
        fTimer->cancelTimeout();
        if (fWorkloop)
            fWorkloop->removeEventSource(fTimer);
        fTimer->release();
        fTimer = nullptr;
    }
    if (fWorkloop) { fWorkloop->release(); fWorkloop = nullptr; }
    if (fAccel) { fAccel->release(); fAccel = nullptr; }
    if (fNvrm) { fNvrm->release(); fNvrm = nullptr; }
    if (fPubPrev) { fPubPrev->release(); fPubPrev = nullptr; }
    if (fPub) { fPub->release(); fPub = nullptr; }
    if (vsmcNotifier) { vsmcNotifier->remove(); vsmcNotifier = nullptr; }
    unregisterSysctls();
    fGpu = nullptr;
    fRusd = nullptr;
    IOService::stop(provider);
}

void SMCNvidiaGPU::free()
{
    if (fTimer) { fTimer->release(); fTimer = nullptr; }
    if (fAccel) { fAccel->release(); fAccel = nullptr; }
    if (fNvrm) { fNvrm->release(); fNvrm = nullptr; }
    if (fPubPrev) { fPubPrev->release(); fPubPrev = nullptr; }
    if (fPub) { fPub->release(); fPub = nullptr; }
    IOService::free();
}

#include <mach/kmod.h>
extern "C" kern_return_t _start(kmod_info_t *ki, void *d);
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
extern "C" { KMOD_EXPLICIT_DECL(org.nullmoth.SMCNvidiaGPU, "2.0.0", _start, _stop) }
extern "C" kern_return_t _start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
