/* smcread.c — read SMC keys via AppleSMC user client (canonical 80-byte SMCKeyData_t). */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <IOKit/IOKitLib.h>

typedef struct { char major, minor, build, reserved[1]; uint16_t release; } SMCVers;
typedef struct { uint16_t version, length; uint32_t cpuPLimit, gpuPLimit, memPLimit; } SMCPLimit;
typedef struct { uint32_t dataSize, dataType; char dataAttributes; } SMCKeyInfo;
typedef struct {
    uint32_t key;
    SMCVers vers;
    SMCPLimit pLimitData;
    SMCKeyInfo keyInfo;
    char result, status, data8;
    uint32_t data32;
    uint8_t bytes[32];
} SMCKeyData;

static io_connect_t conn = 0;
static uint32_t str4(const char *s) {
    return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) |
           ((uint32_t)(uint8_t)s[2] << 8) | (uint32_t)(uint8_t)s[3];
}
static kern_return_t smcCall(SMCKeyData *in, SMCKeyData *out) {
    size_t outSize = sizeof(*out);
    return IOConnectCallStructMethod(conn, 2, in, sizeof(*in), out, &outSize);
}
static int readKey(const char *name, SMCKeyData *val) {
    SMCKeyData in, out;
    memset(&in, 0, sizeof in); memset(&out, 0, sizeof out);
    in.key = str4(name);
    in.data8 = 9;                                   /* kSMCGetKeyInfo */
    kern_return_t kr = smcCall(&in, &out);
    if (kr != KERN_SUCCESS) { if (getenv("SMCDEBUG")) fprintf(stderr, "  [%s] keyinfo kr=0x%x\n", name, kr); return -1; }
    uint32_t size = out.keyInfo.dataSize, type = out.keyInfo.dataType;
    memset(&in, 0, sizeof in);
    in.key = str4(name);
    in.keyInfo.dataSize = size;
    in.data8 = 5;                                   /* kSMCReadKey */
    kr = smcCall(&in, &out);
    if (kr != KERN_SUCCESS || out.result) { if (getenv("SMCDEBUG")) fprintf(stderr, "  [%s] read kr=0x%x result=0x%x\n", name, kr, out.result); return -1; }
    out.keyInfo.dataSize = size; out.keyInfo.dataType = type;
    *val = out;
    return 0;
}
int main(int argc, char **argv) {
    printf("sizeof(SMCKeyData)=%zu\n", sizeof(SMCKeyData));
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
    if (!svc) { fprintf(stderr, "no AppleSMC\n"); return 1; }
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "open failed 0x%x\n", kr); return 1; }
    const char *keys[] = {"TG0D", "TG0P", "TC0D", "TC0P"};
    int n = argc > 1 ? 1 : 4;
    for (int i = 0; i < n; i++) {
        const char *k = argc > 1 ? argv[1] : keys[i];
        SMCKeyData v;
        if (readKey(k, &v) == 0) {
            char t[5] = {0}; uint32_t dt = v.keyInfo.dataType;
            t[0] = dt >> 24; t[1] = dt >> 16; t[2] = dt >> 8; t[3] = dt;
            if (!strcmp(t, "sp78") && v.keyInfo.dataSize == 2)
                printf("%s = %d.%02d C   (type %s size %u)\n", k, (int8_t)v.bytes[0],
                       (int)(v.bytes[1] * 100 / 256), t, v.keyInfo.dataSize);
            else
                printf("%s = type %s size %u bytes %02x %02x %02x %02x\n", k, t, v.keyInfo.dataSize,
                       v.bytes[0], v.bytes[1], v.bytes[2], v.bytes[3]);
        } else printf("%s = <not readable>\n", k);
    }
    IOServiceClose(conn);
    return 0;
}
