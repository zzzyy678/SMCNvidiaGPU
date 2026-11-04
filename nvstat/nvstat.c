/*
 * nvstat — one tool for everything this driver exposes, replacing the pile of one-shot probes in this
 * directory (nvtemp / nvmon / nvdump / nvi2c / nvinv / nvprobe2 / nvrusd / nvrusd2 / nvstatd).
 *
 * Two data sources:
 *
 *   RM user client   (root not needed; this is the path iStat and the daemon use)
 *       NV2080_CTRL_CMD_THERMAL_SYSTEM_EXECUTE_V2      0x20800513  temperature
 *       NV2080_CTRL_CMD_CLK_PMUMON_CLK_DOMAINS_GET_SAMPLES 0x20801037  clocks
 *       NV2080_CTRL_CMD_PERF_GET_GPUMON_PERFMON_UTIL_SAMPLES_V2 0x20802096  utilization
 *       NV2080_CTRL_CMD_FB_GET_UGPU_MEMORY_INFO         0x20801362  VRAM
 *       NV2080_CTRL_CMD_PERF_GET_CURRENT_PSTATE         0x20802068  P-state
 *       NV2080_CTRL_CMD_RUSD_GET_SUPPORTED_FEATURES     0x208001ea  RUSD features
 *
 *   kernel side      (org.nullmoth.SMCNvidiaGPU, which reads RM's RUSD block; see kext/)
 *       kern.nvgpupower, _power_mod, _power_mem, _tgp, _pwrlimit, _fan, _fancount,
 *       _smclk, _gfxclk, _rusdtemp, _live, _poll, _layout, _state
 *
 * Usage:
 *   nvstat                 same as `nvstat all`
 *   nvstat temp|clk|util|vram|pstate|power|fan|rusd
 *   nvstat all [--json]    one line per group
 *   nvstat watch [seconds] [interval-ms]
 *
 * Build: clang -O2 -framework IOKit -o nvstat nvstat.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <IOKit/IOKitLib.h>
#include <sys/sysctl.h>
#include <signal.h>

#define SEL_ESC 1
#define SEL_BINDFD 3
#define NV01_ROOT_CLIENT 0x41u
#define NV01_DEVICE_0 0x80u
#define NV20_SUBDEVICE_0 0x2080u
#define RM_USER_SHARED_DATA 0xdeu
#define NV_ESC_RM_ALLOC 0x2Bu
#define NV_ESC_RM_CONTROL 0x2Au

#define CMD_THERMAL_EXECUTE_V2 0x20800513u
#define CMD_CLK_DOMAINS 0x20801037u
#define CMD_UTIL_V2 0x20802096u
#define CMD_VRAM 0x20801362u
#define CMD_PSTATE 0x20802068u
#define CMD_RUSD_FEATURES 0x208001eau
#define CMD_RUSD_POLL 0xde0001u
#define OP_GET_SENSOR_READING 0x1500u

typedef struct { uint32_t hRoot, hObjectParent, hObjectNew, hClass; uint64_t pAllocParms; uint32_t paramsSize, status; } NVOS21;
typedef struct { uint32_t hClient, hObject, cmd, flags; uint64_t params; uint32_t paramsSize, status; } NVOS54;
typedef struct { uint32_t deviceId, hClientShare, hTargetClient, hTargetDevice, flags; uint64_t vaSpaceSize, vaStartInternal, vaLimitInternal; uint32_t vaMode; } NV0080_ALLOC;
typedef struct { uint32_t subDeviceId; } NV2080_ALLOC;
typedef struct { uint32_t result, executed, opcode; uint32_t operands[8]; } THERM_INSTR;
typedef struct {
    uint32_t clientAPIVersion, clientAPIRevision, clientInstructionSizeOf, executeFlags;
    uint32_t successfulInstructions, instructionListSize;
    THERM_INSTR instructionList[32];
} THERM_PARAMS;
typedef struct { uint32_t tailIndex, sequenceId, headIndex, numSamples, resetCount, retrievalMode; } PMU_SUPER;
typedef struct { uint64_t ts; uint32_t gpcKHz, dramKHz, nvdKHz, activeGpcKHz; } CLK_SAMPLE;
typedef struct { PMU_SUPER super; CLK_SAMPLE samples[250]; } CLK_PARAMS;
typedef struct { uint32_t util, vgpuScale, procId, subProcessID; char name[100]; uint64_t pOsPidInfo; } UTIL_ENGINE;
typedef struct { uint64_t ts; UTIL_ENGINE fb, gr, nvenc, nvdec, nvjpg, nvofa; } UTIL_SAMPLE;
typedef struct { uint8_t type; uint32_t bufSize, count, tracker; UTIL_SAMPLE samples[72]; } UTIL_PARAMS;
typedef struct { uint64_t total, free_, ugpuTotal[2], ugpuFree[2]; } MEMINFO;
typedef struct { uint32_t index, data; } BUS_INFO;
typedef struct { uint32_t busInfoListSize; BUS_INFO busInfoList[52]; } BUS_INFO_V2;

static io_connect_t gConn;
static uint32_t gClient, gDevice, gSubdev;
static int gVerbose;

static int esc(uint32_t cmd, void *data, uint32_t size) {
    uint64_t in = cmd; size_t out = size;
    return IOConnectCallMethod(gConn, SEL_ESC, &in, 1, data, size, NULL, NULL, data, &out) == KERN_SUCCESS ? 0 : -1;
}
static uint32_t rm_alloc(uint32_t parent, uint32_t cls, void *p, uint32_t psize) {
    NVOS21 a; memset(&a, 0, sizeof a);
    a.hRoot = gClient; a.hObjectParent = parent; a.hClass = cls;
    a.pAllocParms = (uint64_t)(uintptr_t)p; a.paramsSize = psize;
    if (esc(NV_ESC_RM_ALLOC, &a, sizeof a) < 0) return 0xFFFFFFFFu;
    if (a.status) { if (gVerbose) fprintf(stderr, "alloc 0x%x -> 0x%08x\n", cls, a.status); return 0xFFFFFFFFu; }
    return a.hObjectNew;
}
static uint32_t rm_ctl(uint32_t h, uint32_t cmd, void *p, uint32_t size) {
    NVOS54 c; memset(&c, 0, sizeof c);
    c.hClient = gClient; c.hObject = h; c.cmd = cmd; c.params = (uint64_t)(uintptr_t)p; c.paramsSize = size;
    if (esc(NV_ESC_RM_CONTROL, &c, sizeof c) < 0) return 0xFFFFFFFFu;
    return c.status;
}
static int rm_connect(void) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVAccel"));
    if (!svc) svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVRM"));
    if (!svc) { fprintf(stderr, "nvstat: no NVAccel/NVRM service (is the NVIDIA driver loaded?)\n"); return 0; }
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0x4E560000u, &gConn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "nvstat: IOServiceOpen -> 0x%08x\n", kr); return 0; }
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) { uint64_t in = (uint64_t)fd; IOConnectCallScalarMethod(gConn, SEL_BINDFD, &in, 1, NULL, NULL); }
    gClient = rm_alloc(0, NV01_ROOT_CLIENT, NULL, 0);
    if (gClient == 0xFFFFFFFFu) return 0;
    NV0080_ALLOC a80; memset(&a80, 0, sizeof a80); a80.hClientShare = gClient;
    gDevice = rm_alloc(gClient, NV01_DEVICE_0, &a80, sizeof a80);
    if (gDevice == 0xFFFFFFFFu) return 0;
    NV2080_ALLOC a2080; memset(&a2080, 0, sizeof a2080);
    gSubdev = rm_alloc(gDevice, NV20_SUBDEVICE_0, &a2080, sizeof a2080);
    return gSubdev != 0xFFFFFFFFu;
}

/* ---- RM readers ----------------------------------------------------------------------------------- */

static int read_temp(uint32_t idx, int32_t *out) {
    THERM_PARAMS p; memset(&p, 0, sizeof p);
    p.clientAPIVersion = 1; p.clientInstructionSizeOf = sizeof(THERM_INSTR); p.instructionListSize = 1;
    p.instructionList[0].opcode = OP_GET_SENSOR_READING;
    p.instructionList[0].operands[0] = idx;
    if (rm_ctl(gSubdev, CMD_THERMAL_EXECUTE_V2, &p, sizeof p) != 0) return 0;
    if (!p.instructionList[0].executed || p.instructionList[0].result != 0) return 0;
    *out = (int32_t)p.instructionList[0].operands[1];
    return 1;
}

static int read_clocks(CLK_SAMPLE *out) {
    static CLK_PARAMS *c;
    if (!c) { c = calloc(1, sizeof *c); if (!c) return 0; }
    if (rm_ctl(gSubdev, CMD_CLK_DOMAINS, c, sizeof *c) != 0 || c->super.numSamples == 0) return 0;
    uint32_t n = c->super.numSamples; if (n > 250) n = 250;
    *out = c->samples[n - 1];
    return 1;
}

static int read_util(uint32_t u[6]) {
    static UTIL_PARAMS *p;
    if (!p) { p = calloc(1, sizeof *p); if (!p) return 0; p->type = 2; p->bufSize = (uint32_t)(sizeof(UTIL_SAMPLE) * 72); }
    if (rm_ctl(gSubdev, CMD_UTIL_V2, p, sizeof *p) != 0) return 0;
    uint64_t best = 0; uint32_t idx = 0;
    for (uint32_t i = 0; i < 72; i++) if (p->samples[i].ts > best) { best = p->samples[i].ts; idx = i; }
    UTIL_SAMPLE *x = &p->samples[idx];
    u[0] = x->gr.util; u[1] = x->fb.util; u[2] = x->nvenc.util;
    u[3] = x->nvdec.util; u[4] = x->nvjpg.util; u[5] = x->nvofa.util;
    return 1;
}

static int read_vram(uint64_t *total, uint64_t *freeb) {
    static MEMINFO *m;
    if (!m) { m = calloc(1, sizeof *m); if (!m) return 0; }
    if (rm_ctl(gSubdev, CMD_VRAM, m, sizeof *m) != 0 || m->total == 0) return 0;
    *total = m->total; *freeb = m->free_;
    return 1;
}

static int read_pstate(uint32_t *ps) { return rm_ctl(gSubdev, CMD_PSTATE, ps, sizeof *ps) == 0; }

/* ---- kernel side (SMCNvidiaGPU) ------------------------------------------------------------------------- */

static int sysctl_int(const char *name, int *out) {
    size_t sz = sizeof(*out);
    return sysctlbyname(name, out, &sz, NULL, 0) == 0;
}
static int sysctl_str(const char *name, char *buf, size_t cap) {
    size_t sz = cap;
    if (sysctlbyname(name, buf, &sz, NULL, 0) != 0) return 0;
    buf[cap - 1] = 0;
    return 1;
}

struct Sens { int power, power_module, power_memory, tgp, power_limit, fan_rpm, fan_count, pstate, smclk, gfxclk, temp, live, poll_ok, layout_ok; int fans[8];
              int inst_power, inst_module, inst_cpu, pcie_gen, pcie_width, pcie_widthmax, pcie_raw, bar1_total, bar1_free, ecc_corr, ecc_uncorr, throttle;
              int pci_speed, pci_lanes, pci_speed_max;
              int pcie_err[8]; char state[192]; int have; };

static void read_sens(struct Sens *s) {
    memset(s, 0, sizeof *s);
    s->have = sysctl_int("kern.nvgpupower", &s->power);
    sysctl_int("kern.nvgpupower_mod", &s->power_module);
    sysctl_int("kern.nvgpupower_mem", &s->power_memory);
    sysctl_int("kern.nvgputgp", &s->tgp);
    sysctl_int("kern.nvgpupwrlimit", &s->power_limit);
    sysctl_int("kern.nvgpufan", &s->fan_rpm);
    for (int i = 0; i < 8; i++) { char key[32]; snprintf(key, sizeof key, "kern.nvgpufan%d", i); sysctl_int(key, &s->fans[i]); }
    sysctl_int("kern.nvgpufancount", &s->fan_count);
    sysctl_int("kern.nvgpstate", &s->pstate);
    sysctl_int("kern.nvgpusmclk", &s->smclk);
    sysctl_int("kern.nvgpugfxclk", &s->gfxclk);
    sysctl_int("kern.nvgputemp", &s->temp);
    sysctl_int("kern.nvgpuinstpower", &s->inst_power);
    sysctl_int("kern.nvgpuinstpower_mod", &s->inst_module);
    sysctl_int("kern.nvgpuinstpower_cpu", &s->inst_cpu);
    sysctl_int("kern.nvgputhrottle", &s->throttle);
    sysctl_int("kern.nvgpupcispeed", &s->pci_speed);
    sysctl_int("kern.nvgpupcispeedmax", &s->pci_speed_max);
    sysctl_int("kern.nvgpupcilanes", &s->pci_lanes);
    sysctl_int("kern.nvgpupciegen", &s->pcie_gen);
    sysctl_int("kern.nvgpupciewidth", &s->pcie_width);
    sysctl_int("kern.nvgpupciewidthmax", &s->pcie_widthmax);
    sysctl_int("kern.nvgpupcieraw", &s->pcie_raw);
    if (s->pcie_gen == 0 && s->pcie_raw > 0) {   /* decode in user space when the kext has not yet */
        unsigned g = (s->pcie_raw >> 20) & 0xfu, w = ((s->pcie_raw >> 12) & 0xfu) * 4u;
        if (g >= 1 && g <= 6 && w > 0 && w <= 32) { s->pcie_gen = (int)g; s->pcie_width = (int)w; }
    }
    sysctl_int("kern.nvgpubar1total", &s->bar1_total);
    sysctl_int("kern.nvgpubar1free", &s->bar1_free);
    sysctl_int("kern.nvgpuecccorr", &s->ecc_corr);
    sysctl_int("kern.nvgpueccuncorr", &s->ecc_uncorr);
    sysctl_int("kern.nvgpupcie_linecode", &s->pcie_err[0]);
    sysctl_int("kern.nvgpupcie_crc", &s->pcie_err[1]);
    sysctl_int("kern.nvgpupcie_naks", &s->pcie_err[2]);
    sysctl_int("kern.nvgpupcie_l0s", &s->pcie_err[3]);
    sysctl_int("kern.nvgpupcie_corr", &s->pcie_err[4]);
    sysctl_int("kern.nvgpupcie_nonfatal", &s->pcie_err[5]);
    sysctl_int("kern.nvgpupcie_fatal", &s->pcie_err[6]);
    sysctl_int("kern.nvgpupcie_unsupported", &s->pcie_err[7]);
    sysctl_int("kern.nvgpulive", &s->live);
    sysctl_int("kern.nvgpupoll", &s->poll_ok);
    sysctl_int("kern.nvgpulayout", &s->layout_ok);
    if (!sysctl_str("kern.nvgpustate", s->state, sizeof s->state)) s->state[0] = 0;
}

/* ---- output --------------------------------------------------------------------------------------- */

/* NV2080_CTRL_CMD_BUS_GET_INFO_V2: PCIe link caps/status straight from the driver. */
static int read_bus(uint32_t idx, uint32_t *out) {
    BUS_INFO_V2 p;
    memset(&p, 0, sizeof p);
    p.busInfoListSize = 1;
    p.busInfoList[0].index = idx;
    uint32_t st = rm_ctl(gSubdev, 0x20801823u, &p, sizeof p);
    if (st != 0) { if (gVerbose) fprintf(stderr, "  BUS_GET_INFO_V2 idx=0x%x status=0x%x\n", idx, st); return 0; }
    if (p.busInfoListSize < 1) return 0;
    *out = p.busInfoList[0].data;
    return 1;
}

static const char *gt_name(unsigned v) {
    switch (v) { case 1: return "2.5 GT/s (Gen1)"; case 2: return "5 GT/s (Gen2)"; case 3: return "8 GT/s (Gen3)";
                 case 4: return "16 GT/s (Gen4)"; case 5: return "32 GT/s (Gen5)"; case 6: return "64 GT/s (Gen6)"; }
    return "unknown";
}

static void cmd_bus(void) {
    uint32_t caps, status, gen;
    if (!read_bus(0x03u, &caps) || !read_bus(0x07u, &status)) { printf("pcie: unavailable\n"); return; }
    printf("link caps    max speed %u (%s)  max width x%u  gen field GEN%u  gpu gen GEN%u\n",
           caps & 0xfu, gt_name(caps & 0xfu), (caps >> 4) & 0x3fu,
           ((caps >> 12) & 0xfu) + 1u, ((caps >> 20) & 0xfu) + 1u);
    unsigned spd = (status >> 16) & 0xfu, width = (status >> 20) & 0x3fu;
    printf("link status  speed %u (%s)  width x%u  aspm 0x%x\n", spd, gt_name(spd), width, status & 3u);
    if (read_bus(0x2du, &gen))
        printf("gen info     0x%08x  curr-level GEN%u  gen GEN%u  gpu-gen GEN%u  speed-changes %s\n",
               gen, ((gen >> 16) & 0xfu) + 1u, ((gen >> 12) & 0xfu) + 1u, ((gen >> 20) & 0xfu) + 1u,
               ((gen >> 24) & 1u) ? "disabled" : "enabled");
    uint32_t board, ups;
    if (read_bus(0x30u, &board)) printf("board caps   max speed %u (%s)  max width x%u\n", board & 0xfu, gt_name(board & 0xfu), (board >> 4) & 0x3fu);
    if (read_bus(0x2fu, &ups))   printf("upstream     max speed %u (%s)  max width x%u\n", ups & 0xfu, gt_name(ups & 0xfu), (ups >> 4) & 0x3fu);
}

static void cmd_temp(int all) {
    int shown = 0;
    for (uint32_t i = 0; i < (all ? 16u : 1u); i++) {
        int32_t t;
        if (!read_temp(i, &t)) { if (all) printf("sensor[%2u] n/a\n", i); continue; }
        if (!all) printf("%d\n", t);
        else { printf("sensor[%2u] %d C\n", i, t); shown++; }
    }
    (void)shown;
}

static void cmd_clk(void) {
    CLK_SAMPLE s;
    if (!read_clocks(&s)) { printf("clocks: unavailable\n"); return; }
    printf("GPC %u MHz  DRAM %u MHz  NVD %u MHz  active GPC %u MHz\n",
           s.gpcKHz / 1000, s.dramKHz / 1000, s.nvdKHz / 1000, s.activeGpcKHz / 1000);
}

static void cmd_util(void) {
    uint32_t u[6];
    if (!read_util(u)) { printf("utilization: unavailable\n"); return; }
    printf("GR %.2f%%  FB %.2f%%  ENC %.2f%%  DEC %.2f%%  JPEG %.2f%%  OFA %.2f%%\n",
           u[0] / 100.0, u[1] / 100.0, u[2] / 100.0, u[3] / 100.0, u[4] / 100.0, u[5] / 100.0);
}

static void cmd_vram(void) {
    uint64_t total, freeb;
    if (!read_vram(&total, &freeb)) { printf("VRAM: unavailable\n"); return; }
    printf("%.0f / %.0f MB used (%.1f%% free)\n",
           (total - freeb) / 1048576.0, total / 1048576.0, 100.0 * freeb / total);
}

static void cmd_pstate(void) {
    uint32_t ps;
    if (!read_pstate(&ps)) { printf("P-state: unavailable\n"); return; }
    printf("%u\n", ps);
}

static void cmd_power(void) {
    struct Sens s; read_sens(&s);
    if (!s.have) {
        printf("power: kern.nvgpupower is not there — SMCNvidiaGPU.kext is not loaded (see kext/README.md)\n");
        return;
    }
    printf("%d mW (%.3f W)  module %d mW  memory %d mW  TGP %d mW  limit %d mW\n",
           s.power, s.power / 1000.0, s.power_module, s.power_memory, s.tgp, s.power_limit);
}

static void cmd_fan(void) {
    struct Sens s; read_sens(&s);
    if (!s.have) {
        printf("fan: kern.nvgpufan is not there — SMCNvidiaGPU.kext is not loaded (see kext/README.md)\n");
        return;
    }
    printf("%d RPM (%d fans reporting)\n", s.fan_rpm, s.fan_count);
}

static void cmd_rusd(void) {
    uint32_t feat = 0;
    uint32_t st = rm_ctl(gSubdev, CMD_RUSD_FEATURES, &feat, sizeof feat);
    printf("RUSD supported features: ctrl=0x%08x features=0x%x%s%s\n", st, feat,
           (feat & 1) ? " non-polling" : "", (feat & 2) ? " polling" : "");

    struct { uint64_t mask; } alloc = { 0x8u | 0x10u | 0x40u };
    uint32_t h = rm_alloc(gDevice, RM_USER_SHARED_DATA, &alloc, sizeof alloc);
    printf("user RM_USER_SHARED_DATA alloc: %s (0x%x)%s\n",
           h == 0xFFFFFFFFu ? "refused" : "ok", h,
           h == 0xFFFFFFFFu ? " — expected: the driver allocates RUSD only for itself" : "");
    if (h != 0xFFFFFFFFu) {
        uint64_t mask = alloc.mask;
        printf("  REQUEST_DATA_POLL(0x%llx) -> 0x%08x\n",
               (unsigned long long)mask, rm_ctl(h, CMD_RUSD_POLL, &mask, sizeof mask));
    }

    struct Sens s; read_sens(&s);
    printf("kernel side (SMCNvidiaGPU): %s\n", s.have ? "loaded" : "not loaded");
    if (s.have) {
        printf("  state: %s\n  layout_ok=%d poll_ok=%d live=%d\n", s.state, s.layout_ok, s.poll_ok, s.live);
        printf("  power %d mW  fan %d RPM  pstate %d  smclk %d MHz  temp %d C\n",
               s.power, s.fan_rpm, s.pstate, s.smclk, s.temp);
    }
}

static void cmd_all(int json) {
    int32_t t = 0; int haveT = read_temp(0, &t);
    CLK_SAMPLE c; int haveC = read_clocks(&c);
    uint32_t u[6] = {0}; int haveU = read_util(u);
    uint64_t total = 0, freeb = 0; int haveV = read_vram(&total, &freeb);
    uint32_t ps = 0; int haveP = read_pstate(&ps);
    struct Sens s; read_sens(&s);

    if (json) {
        printf("{");
        printf("\"temp_c\":%d,", haveT ? t : -1);
        printf("\"gpc_mhz\":%u,\"dram_mhz\":%u,\"nvd_mhz\":%u,\"active_gpc_mhz\":%u,",
               haveC ? c.gpcKHz / 1000 : 0, haveC ? c.dramKHz / 1000 : 0,
               haveC ? c.nvdKHz / 1000 : 0, haveC ? c.activeGpcKHz / 1000 : 0);
        printf("\"gr_pct\":%.2f,\"fb_pct\":%.2f,\"enc_pct\":%.2f,\"dec_pct\":%.2f,",
               haveU ? u[0] / 100.0 : 0, haveU ? u[1] / 100.0 : 0,
               haveU ? u[2] / 100.0 : 0, haveU ? u[3] / 100.0 : 0);
        printf("\"vram_used_mb\":%.0f,\"vram_total_mb\":%.0f,",
               haveV ? (total - freeb) / 1048576.0 : 0, haveV ? total / 1048576.0 : 0);
        printf("\"pstate\":%u,", haveP ? ps : 0);
        printf("\"power_mw\":%d,\"fan_rpm\":%d,\"power_limit_mw\":%d,\"inst_power_mw\":%d,"
               "\"pcie_gen\":%d,\"pcie_width\":%d,\"ecc_corrected\":%d,\"ecc_uncorrected\":%d,"
               "\"bar1_total_mb\":%d,\"bar1_free_mb\":%d,\"smcngpu\":%s}\n",
               s.have ? s.power : 0, s.have ? s.fan_rpm : 0, s.have ? s.power_limit : 0,
               s.have ? s.inst_power : 0, s.have ? s.pcie_gen : 0, s.have ? s.pcie_width : 0,
               s.have ? s.ecc_corr : 0, s.have ? s.ecc_uncorr : 0,
               s.have ? s.bar1_total : 0, s.have ? s.bar1_free : 0,
               s.have ? "true" : "false");
        return;
    }

    char b0[64], b1[96], b2[112], b3[64], b4[16];
    if (haveT) snprintf(b0, sizeof b0, "%d C", t); else snprintf(b0, sizeof b0, "n/a");
    if (haveC) snprintf(b1, sizeof b1, "GPC %u  DRAM %u  NVD %u  active %u MHz",
                        c.gpcKHz / 1000, c.dramKHz / 1000, c.nvdKHz / 1000, c.activeGpcKHz / 1000);
    else snprintf(b1, sizeof b1, "n/a");
    if (haveU) snprintf(b2, sizeof b2, "GR %.1f%%  FB %.1f%%  ENC %.1f%%  DEC %.1f%%",
                        u[0] / 100.0, u[1] / 100.0, u[2] / 100.0, u[3] / 100.0);
    else snprintf(b2, sizeof b2, "n/a");
    if (haveV) snprintf(b3, sizeof b3, "%.0f / %.0f MB",
                        (total - freeb) / 1048576.0, total / 1048576.0);
    else snprintf(b3, sizeof b3, "n/a");
    if (haveP) snprintf(b4, sizeof b4, "%u", ps); else snprintf(b4, sizeof b4, "n/a");

    printf("temp        %s\n", b0);
    printf("clocks      %s\n", b1);
    printf("util        %s\n", b2);
    printf("vram        %s\n", b3);
    printf("pstate      %s\n", b4);
    if (s.have) {
        printf("power       %d mW (%.2f W)  limit %d mW  TGP %d mW\n", s.power, s.power / 1000.0,
               s.power_limit, s.tgp);
        {
            int n = 0; char buf[128] = "";
            for (int i = 0; i < 8; i++) if (s.fans[i] > 0) { char one[24]; snprintf(one, sizeof one, "%sfan%d %d", n ? "  " : "", i + 1, s.fans[i]); strncat(buf, one, sizeof buf - strlen(buf) - 1); n++; }
            if (n > 1) printf("fan         %s RPM   (max %d, %d fans, live=%d)\n", buf, s.fan_rpm, s.fan_count, s.live);
            else       printf("fan         %d RPM (%d fans)   RUSD live=%d\n", s.fan_rpm, s.fan_count, s.live);
        }
        if (s.inst_power > 0)
            printf("power(inst) %d mW (%.2f W)  module %d mW  cpu %d mW\n",
                   s.inst_power, s.inst_power / 1000.0, s.inst_module, s.inst_cpu);
        if (s.pci_speed > 0) {   /* from PCI config space: the authoritative value */
            printf("pcie-link   Gen%u x%u  (max Gen%u)\n", (unsigned)s.pci_speed, (unsigned)s.pci_lanes, (unsigned)s.pci_speed_max);
        }
        if (s.pcie_gen > 0 && s.pcie_width == 0)
            printf("pcie        Gen%u (宽度见 nvstat bus)\n", (unsigned)s.pcie_gen);
        else if (s.pcie_gen > 0)
            printf("pcie        Gen%u x%u   lc %d  crc %d  naks %d  l0s %d  corr %d  nonfatal %d  fatal %d  unsup %d\n",
                   (unsigned)s.pcie_gen, (unsigned)s.pcie_width, s.pcie_err[0], s.pcie_err[1], s.pcie_err[2],
                   s.pcie_err[3], s.pcie_err[4], s.pcie_err[5], s.pcie_err[6], s.pcie_err[7]);
        if (s.throttle >= 0) {
            static const char *names[] = {"idle", "app-clock", "sw-power-cap", "hw-slowdown", "sync-boost",
                                          "sw-thermal", "hw-thermal", "power-brakes", "display-clock",
                                          "board-limit", "reliability"};
            char tb[256]; tb[0] = 0;
            for (int i = 0; i < 11; i++)
                if (s.throttle & (1 << i)) { strncat(tb, names[i], sizeof tb - strlen(tb) - 2); strncat(tb, " ", sizeof tb - strlen(tb) - 2); }
            printf("throttle    0x%03x %s\n", s.throttle, tb[0] ? tb : "(none)");
        }
        if (s.bar1_total > 0)
            printf("bar1        %d MB total, %d MB free\n", s.bar1_total, s.bar1_free);
        if (s.ecc_corr > 0 || s.ecc_uncorr > 0)
            printf("ecc         corrected %d  uncorrected %d (DRAM aggregate)\n", s.ecc_corr, s.ecc_uncorr);
        printf("kext        %s\n", s.state);
    } else {
        printf("power       n/a  (SMCNvidiaGPU.kext not loaded)\n");
        printf("fan         n/a  (SMCNvidiaGPU.kext not loaded)\n");
    }
}

static volatile sig_atomic_t gStop;
static void on_sig(int s) { (void)s; gStop = 1; }

static void cmd_watch(double seconds, int interval_ms) {
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    double end = seconds > 0 ? seconds : 1e18;
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    printf("%8s %6s %6s %6s %6s %9s %7s\n", "t", "temp", "gpc", "gr%", "pstate", "powerW", "fan");
    while (!gStop) {
        int32_t t = 0; CLK_SAMPLE c; uint32_t u[6] = {0}, ps = 0;
        int haveT = read_temp(0, &t), haveC = read_clocks(&c), haveU = read_util(u), haveP = read_pstate(&ps);
        struct Sens s; read_sens(&s);
        clock_gettime(CLOCK_MONOTONIC, &now);
        double el = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9;
        if (el >= end) break;
        printf("%8.1f %6d %6u %6.1f %6u %9.2f %7d\n", el, haveT ? t : -1,
               haveC ? c.gpcKHz / 1000 : 0, haveU ? u[0] / 100.0 : 0.0, haveP ? ps : 0,
               s.have ? s.power / 1000.0 : 0.0, s.have ? s.fan_rpm : 0);
        usleep(interval_ms * 1000);
    }
}

int main(int argc, char **argv) {
    const char *cmd = "all";
    const char *pos[4] = {0};
    int npos = 0, json = 0, allSensors = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) json = 1;
        else if (!strcmp(argv[i], "-v")) gVerbose = 1;
        else if (!strcmp(argv[i], "--all")) allSensors = 1;
        else if (argv[i][0] == '-') { /* unknown flag: ignore */ }
        else if (npos < 4) pos[npos++] = argv[i];
    }
    if (npos > 0) cmd = pos[0];

    /* The kernel-side commands need no RM client; the rest do. */
    if (!strcmp(cmd, "power") || !strcmp(cmd, "fan")) {
        if (!strcmp(cmd, "power")) cmd_power(); else cmd_fan();
        return 0;
    }

    if (!rm_connect()) { fprintf(stderr, "nvstat: RM connect failed\n"); return 1; }

    if (!strcmp(cmd, "temp")) cmd_temp(allSensors);
    else if (!strcmp(cmd, "clk")) cmd_clk();
    else if (!strcmp(cmd, "util")) cmd_util();
    else if (!strcmp(cmd, "vram")) cmd_vram();
    else if (!strcmp(cmd, "pstate")) cmd_pstate();
    else if (!strcmp(cmd, "rusd") || !strcmp(cmd, "sens")) cmd_rusd();
    else if (!strcmp(cmd, "bus") || !strcmp(cmd, "pcie")) cmd_bus();
    else if (!strcmp(cmd, "all")) cmd_all(json);
    else if (!strcmp(cmd, "watch")) {
        double secs = (npos > 1) ? atof(pos[1]) : 0;
        int interval = (npos > 2) ? atoi(pos[2]) : 1000;
        cmd_watch(secs, interval);
    } else if (!strcmp(cmd, "help") || !strcmp(cmd, "--help")) {
        printf("usage: nvstat [temp [--all]|clk|util|vram|pstate|power|fan|rusd|bus|all [--json]|watch [s] [ms]]\n");
    } else { fprintf(stderr, "nvstat: unknown command '%s' (try `nvstat help`)\n", cmd); return 2; }
    return 0;
}
