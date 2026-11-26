/* icx2.c — EVGA iCX2 sensor reader over the NullMoth NVRM user client (see icx2.h).
 *
 * How it reaches the I2C bus: NVRMUserClient (kexts/NVRM/NVRM.cpp) mirrors Linux's /dev/nvidiactl (type 0) and
 * /dev/nvidia0 (type 1). Selector 1 (sEsc) takes a Linux escape number and its structure and hands it to RM's
 * rm_ioctl, so the usual RM API works from user space on the control client: allocate a root client,
 * NV01_DEVICE_0, NV20_SUBDEVICE_0 and NV40_I2C, then issue NV402C_CTRL_CMD_I2C_TRANSACTION. RM validates every
 * call; a wrong argument is an error code.
 */
#include "icx2.h"
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

#include "nvos.h"
#include "class/cl0000.h"
#include "class/cl0080.h"
#include "class/cl2080.h"
#include "class/cl402c.h"
#include "ctrl/ctrl402c.h"

/* Linux escape numbers (kernel-open/common/inc/nv-ioctl-numbers.h, nv_escape.h). */
#define NV_ESC_RM_FREE     0x29
#define NV_ESC_RM_CONTROL  0x2A
#define NV_ESC_RM_ALLOC    0x2B

/* NVRMUserClient selectors (NVRM.cpp externalMethod table). */
enum { SEL_ESC = 1, SEL_GETFD = 2 };

/* The layouts this code depends on, checked against NVIDIA's 620.x headers at compile time. */
_Static_assert(sizeof(NVOS21_PARAMETERS) == 32, "NVOS21 layout");
_Static_assert(sizeof(NVOS54_PARAMETERS) == 32, "NVOS54 layout");
_Static_assert(sizeof(NVOS00_PARAMETERS) == 16, "NVOS00 layout");
_Static_assert(offsetof(NV402C_CTRL_I2C_TRANSACTION_PARAMS, portId) == 0, "portId");
_Static_assert(offsetof(NV402C_CTRL_I2C_TRANSACTION_PARAMS, flags) == 4, "flags");
_Static_assert(offsetof(NV402C_CTRL_I2C_TRANSACTION_PARAMS, deviceAddress) == 8, "deviceAddress");
_Static_assert(offsetof(NV402C_CTRL_I2C_TRANSACTION_PARAMS, transType) == 12, "transType");
_Static_assert(offsetof(NV402C_CTRL_I2C_TRANSACTION_PARAMS, transData) == 16, "transData");
_Static_assert(offsetof(NV402C_CTRL_I2C_TRANSACTION_DATA_SMBUS_BYTE_RW, bWrite) == 0, "bWrite");
_Static_assert(offsetof(NV402C_CTRL_I2C_TRANSACTION_DATA_SMBUS_BYTE_RW, registerAddress) == 1, "registerAddress");
_Static_assert(offsetof(NV402C_CTRL_I2C_TRANSACTION_DATA_SMBUS_BYTE_RW, message) == 2, "message");

/* flags: 0 = 7-bit addressing, port's default speed, normal transaction. RM on this driver rejects an explicit
 * speed field or PING mode with NV_ERR_INVALID_ARGUMENT, so presence is checked with an SMBus quick read instead. */
#define I2C_FLAGS_NORMAL  0u

#define H_DEVICE    0x1c200001u
#define H_SUBDEVICE 0x1c200002u
#define H_I2C       0x1c200003u

struct icx2 {
    io_connect_t conn;
    NvHandle     hClient;
    int          port;
};

const char *const icx2_temp_names[ICX2_NUM_TEMPS] = { "GPU2", "MEM1", "MEM2", "MEM3", "PWR1", "PWR2", "PWR3", "PWR4", "PWR5" };
const char *const icx2_rail_names[ICX2_NUM_RAILS] = { "Input 1", "Input 2", "Input 3" };

static kern_return_t esc(io_connect_t c, uint32_t cmd, void *buf, size_t size)
{
    uint64_t in = cmd;
    size_t outSize = size;
    return IOConnectCallMethod(c, SEL_ESC, &in, 1, buf, size, NULL, NULL, buf, &outSize);
}

static NvU32 rm_alloc(icx2_t *h, NvHandle parent, NvHandle *hNew, NvU32 cls, void *params, NvU32 size)
{
    NVOS21_PARAMETERS p;
    memset(&p, 0, sizeof p);
    p.hRoot = h->hClient; p.hObjectParent = parent; p.hObjectNew = *hNew; p.hClass = cls;
    p.pAllocParms = (NvP64)(uintptr_t)params; p.paramsSize = size;
    kern_return_t kr = esc(h->conn, NV_ESC_RM_ALLOC, &p, sizeof p);
    if (kr != KERN_SUCCESS && p.status == 0) return 0xFFFFFFFFu;
    if (p.status == 0) *hNew = p.hObjectNew;
    return p.status;
}

static NvU32 rm_control(icx2_t *h, NvHandle obj, NvU32 cmd, void *params, NvU32 size)
{
    NVOS54_PARAMETERS p;
    memset(&p, 0, sizeof p);
    p.hClient = h->hClient; p.hObject = obj; p.cmd = cmd;
    p.params = (NvP64)(uintptr_t)params; p.paramsSize = size;
    kern_return_t kr = esc(h->conn, NV_ESC_RM_CONTROL, &p, sizeof p);
    if (kr != KERN_SUCCESS && p.status == 0) return 0xFFFFFFFFu;
    return p.status;
}

static NvU32 smbus_byte(icx2_t *h, int port, bool write, uint8_t reg, uint8_t *val, NvU32 flags)
{
    NV402C_CTRL_I2C_TRANSACTION_PARAMS t;
    memset(&t, 0, sizeof t);
    t.portId = (NvU8)port;
    t.flags = flags;
    t.deviceAddress = ICX2_ADDR << 1;
    t.transType = NV402C_CTRL_I2C_TRANSACTION_TYPE_SMBUS_BYTE_RW;
    t.transData.smbusByteData.bWrite = write ? NV_TRUE : NV_FALSE;
    t.transData.smbusByteData.registerAddress = reg;
    t.transData.smbusByteData.message = write ? *val : 0;
    NvU32 st = rm_control(h, H_I2C, NV402C_CTRL_CMD_I2C_TRANSACTION, &t, sizeof t);
    if (st == 0 && !write) *val = t.transData.smbusByteData.message;
    return st;
}

/* SMBus quick read: address only, no register access. */
static NvU32 smbus_quick(icx2_t *h, int port)
{
    NV402C_CTRL_I2C_TRANSACTION_PARAMS t;
    memset(&t, 0, sizeof t);
    t.portId = (NvU8)port;
    t.flags = I2C_FLAGS_NORMAL;
    t.deviceAddress = ICX2_ADDR << 1;
    t.transType = NV402C_CTRL_I2C_TRANSACTION_TYPE_SMBUS_QUICK_RW;
    t.transData.smbusQuickData.bWrite = NV_FALSE;
    return rm_control(h, H_I2C, NV402C_CTRL_CMD_I2C_TRANSACTION, &t, sizeof t);
}

bool icx2_smbus_read(icx2_t *h, uint8_t reg, uint8_t *val)
{
    return h && h->port >= 0 && smbus_byte(h, h->port, false, reg, val, I2C_FLAGS_NORMAL) == 0;
}

/* Writes are only ever to the two latch registers Precision X1 writes before reading the power block. */
static bool latch_write(icx2_t *h, uint8_t reg, uint8_t v)
{
    if (reg != 0x0E && reg != 0x1E) return false;
    return smbus_byte(h, h->port, true, reg, &v, I2C_FLAGS_NORMAL) == 0;
}

icx2_t *icx2_open(char *err, int errlen)
{
    #define FAIL(...) do { if (err) snprintf(err, errlen, __VA_ARGS__); icx2_close(h); return NULL; } while (0)
    icx2_t *h = calloc(1, sizeof *h);
    if (!h) { if (err) snprintf(err, errlen, "out of memory"); return NULL; }
    h->port = -1;
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVRM"));
    if (!svc) FAIL("NVRM service not found (is the NullMoth driver running?)");
    /* type 0, the /dev/nvidiactl equivalent: RM objects (root, device, subdevice, I2C) are allocated here, as on
     * Linux. The device client (type 1) refuses RM allocations. */
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &h->conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) FAIL("cannot open the NVRM control client (0x%x)", kr);

    NvHandle root = 0;
    NvU32 st = rm_alloc(h, 0, &root, NV01_ROOT_CLIENT, NULL, 0);
    if (st) FAIL("RM root client allocation failed (status 0x%x)", st);
    h->hClient = root;

    NV0080_ALLOC_PARAMETERS dev; memset(&dev, 0, sizeof dev); dev.deviceId = 0;
    NvHandle hDev = H_DEVICE;
    if ((st = rm_alloc(h, h->hClient, &hDev, NV01_DEVICE_0, &dev, sizeof dev))) FAIL("NV01_DEVICE_0 allocation failed (status 0x%x)", st);
    NV2080_ALLOC_PARAMETERS sub; memset(&sub, 0, sizeof sub); sub.subDeviceId = 0;
    NvHandle hSub = H_SUBDEVICE;
    if ((st = rm_alloc(h, hDev, &hSub, NV20_SUBDEVICE_0, &sub, sizeof sub))) FAIL("NV20_SUBDEVICE_0 allocation failed (status 0x%x)", st);
    NvHandle hI2c = H_I2C;
    if ((st = rm_alloc(h, hSub, &hI2c, NV40_I2C, NULL, 0))) FAIL("NV40_I2C allocation failed (status 0x%x)", st);

    NV402C_CTRL_I2C_GET_PORT_INFO_PARAMS info; memset(&info, 0, sizeof info);
    if ((st = rm_control(h, H_I2C, NV402C_CTRL_CMD_I2C_GET_PORT_INFO, &info, sizeof info))) FAIL("I2C port query failed (status 0x%x)", st);

    /* Find the iCX2: skip display (DDC) ports, check presence with a quick read, then the two identity registers. */
    for (int p = 0; p < NV402C_CTRL_NUM_I2C_PORTS && h->port < 0; p++) {
        if (!(info.info[p] & 0x10)) continue;                 /* NV402C_CTRL_I2C_GET_PORT_INFO_VALID */
        if (info.info[p] & 0x04) continue;                    /* NV402C_CTRL_I2C_GET_PORT_INFO_DDC_CHANNEL: a monitor port */
        if (smbus_quick(h, p)) continue;
        uint8_t a = 0, b = 0;
        if (smbus_byte(h, p, false, 0x02, &a, I2C_FLAGS_NORMAL) || smbus_byte(h, p, false, 0x03, &b, I2C_FLAGS_NORMAL)) continue;
        if (a == 0x74 && b == 0xF9) h->port = p;
    }
    if (h->port < 0) FAIL("no EVGA iCX2 controller answered at 0x%02X on any I2C port", ICX2_ADDR);
    return h;
    #undef FAIL
}

void icx2_close(icx2_t *h)
{
    if (!h) return;
    if (h->conn) {
        if (h->hClient) {
            NVOS00_PARAMETERS f; memset(&f, 0, sizeof f);
            f.hRoot = h->hClient; f.hObjectParent = h->hClient; f.hObjectOld = h->hClient;
            esc(h->conn, NV_ESC_RM_FREE, &f, sizeof f);
        }
        IOServiceClose(h->conn);
    }
    free(h);
}

int icx2_port(const icx2_t *h) { return h ? h->port : -1; }

bool icx2_read_temps(icx2_t *h, icx2_reading_t *out)
{
    out->ok = false;
    for (int i = 0; i < ICX2_NUM_TEMPS; i++) {
        uint8_t v;
        if (!icx2_smbus_read(h, (uint8_t)(0x31 + i), &v)) return false;
        out->temp_c[i] = v;
    }
    out->ok = true;
    return true;
}

bool icx2_read_power(icx2_t *h, icx2_reading_t *out)
{
    out->power_ok = false;
    if (!h || h->port < 0) return false;
    /* Precision X1's sequence, byte for byte */
    if (!latch_write(h, 0x0E, 0xE5) || !latch_write(h, 0x0E, 0xE9) || !latch_write(h, 0x1E, 0x01) || !latch_write(h, 0x0E, 0xE0))
        return false;
    uint8_t b[12];
    for (int i = 0; i < 12; i++) if (!icx2_smbus_read(h, (uint8_t)(0xE0 + i), &b[i])) return false;
    for (int r = 0; r < ICX2_NUM_RAILS; r++) {
        out->rail_ma[r] = (uint16_t)(b[r * 4] | b[r * 4 + 1] << 8);
        out->rail_mv[r] = (uint16_t)(b[r * 4 + 2] | b[r * 4 + 3] << 8);
    }
    out->power_ok = true;
    return true;
}

bool icx2_read_fans(icx2_t *h, icx2_reading_t *out)
{
    static const uint8_t reg[ICX2_NUM_FANS][2] = { { 0x51, 0x54 } /* Fan 2 */, { 0x46, 0x49 } /* Fan 3 */ };
    out->fan_ok = false;
    for (int i = 0; i < ICX2_NUM_FANS; i++) {
        uint8_t t = 0, m = 0;
        if (!icx2_smbus_read(h, reg[i][0], &t) || !icx2_smbus_read(h, reg[i][1], &m)) return false;
        out->fan_target[i] = t > 100 ? 100 : t;
        out->fan_rpm[i] = m * 30;
    }
    out->fan_ok = true;
    return true;
}
