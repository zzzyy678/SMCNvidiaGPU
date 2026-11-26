/* nvrm_runtime.h — find NVRM's entry points and OBJGPU field offsets at boot, from the driver on disk.
 *
 * nvrm_bindings.h pins one driver build, so every driver update used to leave the kext unbound until it
 * was rebuilt. This does the same job as nvrm_bindings.py, but inside the kext, against whatever NVRM is
 * installed:
 *
 *   1. read the Mach-O symbol table of /Library/Extensions/NVRM.kext/Contents/MacOS/NVRM and look up the
 *      exported functions the kext calls (the driver ships unstripped; nm sees them, so can we);
 *   2. decode the OBJGPU field offsets from the two small functions nvrm_bindings.py disassembles, by
 *      matching the same instructions as raw bytes, and run the same consistency checks;
 *   3. hand back offsets plus the first NVRM_RT_SIG_LEN bytes of each function, and the exact bytes of
 *      every instruction a field offset was decoded from, all read from the file.
 *
 * The caller then checks every one of those byte runs against the code that is actually loaded before it
 * calls anything, so a driver updated on disk but not yet loaded (no reboot yet) simply does not bind.
 * Checking the decoded instructions too matters: two builds can share function prologues while their
 * OBJGPU layout differs, and the field offsets are only trustworthy if the loaded code uses them.
 *
 * Pure C++ with a read callback, so tools/test can run it in user space against the real binary.
 */
#pragma once

#define NVRM_RT_SIG_LEN 16u

enum {
    NVRT_ANCHOR = 0,   /* nv_xnu_phys_for_kernel_va: the address the driver hands out */
    NVRT_GPU,          /* gpumgrGetGpu */
    NVRT_CREATE,       /* gpuCreateRusdMemory_IMPL */
    NVRT_LINK,         /* gpuRusdInitPhysical_KERNEL */
    NVRT_LOCK,         /* rmapiLockAcquire */
    NVRT_UNLOCK,       /* rmapiLockRelease */
    NVRT_PMIN,         /* nv_xnu_pm_enter */
    NVRT_PMOUT,        /* nv_xnu_pm_exit */
    NVRT_WRITESTART,   /* gpushareddataWriteStart_INTERNAL (only decoded, never called) */
    NVRT_COUNT
};

static const char *const kNvrtSymbols[NVRT_COUNT] = {
    "_nv_xnu_phys_for_kernel_va", "_gpumgrGetGpu", "_gpuCreateRusdMemory_IMPL", "_gpuRusdInitPhysical_KERNEL",
    "_rmapiLockAcquire", "_rmapiLockRelease", "_nv_xnu_pm_enter", "_nv_xnu_pm_exit",
    "_gpushareddataWriteStart_INTERNAL",
};

/* One decoded instruction: image offset and its bytes (opcode prefix + 32-bit displacement). None of these
 * carry relocations (they address registers, not symbols), so the loaded copy must match byte for byte. */
#define NVRT_SITE_MAX 8u
enum { NVRT_SITES = 7 };
struct NvrtSite {
    UInt64 off;
    unsigned char len;
    unsigned char bytes[NVRT_SITE_MAX];
};

struct NvrmRuntime {
    UInt64 off[NVRT_COUNT];
    unsigned char sig[NVRT_COUNT][NVRM_RT_SIG_LEN];
    UInt32 hClient, hSubdevice, physicalRmApi, rmapiControl, usdMemDesc, usdMapBuffer, usdData;
    NvrtSite site[NVRT_SITES];
    unsigned char uuid[16];
    bool hasUuid;
    char why[96];   /* set when resolution fails */
};

/* Read len bytes at file offset off. Returns false on any error or short read. */
typedef bool (*nvrt_read_fn)(void *ctx, UInt64 off, void *buf, size_t len);
typedef void *(*nvrt_alloc_fn)(size_t len);
typedef void (*nvrt_free_fn)(void *p, size_t len);

static inline UInt32 nvrt_le32(const unsigned char *p)
{
    return (UInt32)p[0] | ((UInt32)p[1] << 8) | ((UInt32)p[2] << 16) | ((UInt32)p[3] << 24);
}

/* Exactly one occurrence of `prefix` followed by a 32-bit displacement in [0, len); returns it in *out and
 * records the whole instruction (at image offset funcOff + position) in *site. */
static bool nvrt_find_disp(const unsigned char *code, size_t len, UInt64 funcOff, const unsigned char *prefix,
                           size_t plen, UInt32 *out, NvrtSite *site)
{
    int hits = 0;
    size_t at = 0;
    if (plen + 4 > NVRT_SITE_MAX)
        return false;
    for (size_t i = 0; i + plen + 4 <= len; i++) {
        if (memcmp(code + i, prefix, plen) == 0) {
            at = i;
            hits++;
        }
    }
    if (hits != 1)
        return false;
    *out = nvrt_le32(code + at + plen);
    site->off = funcOff + at;
    site->len = (unsigned char)(plen + 4);
    memcpy(site->bytes, code + at, plen + 4);
    return true;
}

#define NVRT_FAIL(...) do { snprintf(rt->why, sizeof rt->why, __VA_ARGS__); goto out; } while (0)

static bool nvrm_runtime_resolve(nvrt_read_fn rd, void *ctx, nvrt_alloc_fn alloc, nvrt_free_fn release,
                                 NvrmRuntime *rt)
{
    bool ok = false;
    unsigned char *cmds = nullptr, *syms = nullptr, *strs = nullptr;
    size_t cmdsLen = 0, symsLen = 0, strsLen = 0;
    struct mach_header_64 mh;
    UInt32 symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
    UInt64 textFileOff = (UInt64)-1, textVmAddr = 0, textSize = 0;
    unsigned found = 0;
    UInt64 linkEnd = (UInt64)-1, wsEnd = (UInt64)-1;

    memset(rt, 0, sizeof *rt);
    if (!rd(ctx, 0, &mh, sizeof mh))
        NVRT_FAIL("cannot read the driver file");
    if (mh.magic != MH_MAGIC_64 || mh.filetype != MH_KEXT_BUNDLE || mh.cputype != CPU_TYPE_X86_64)
        NVRT_FAIL("driver file is not an x86_64 kext (magic %#x)", mh.magic);
    if (mh.sizeofcmds < sizeof(struct load_command) || mh.sizeofcmds > 0x100000)
        NVRT_FAIL("bad load command size %u", mh.sizeofcmds);

    cmdsLen = mh.sizeofcmds;
    cmds = (unsigned char *)alloc(cmdsLen);
    if (!cmds || !rd(ctx, sizeof mh, cmds, cmdsLen))
        NVRT_FAIL("cannot read load commands");
    for (size_t at = 0; at + sizeof(struct load_command) <= cmdsLen;) {
        const struct load_command *lc = (const struct load_command *)(cmds + at);
        if (lc->cmdsize < sizeof(struct load_command) || at + lc->cmdsize > cmdsLen)
            NVRT_FAIL("malformed load commands");
        if (lc->cmd == LC_SYMTAB && lc->cmdsize >= sizeof(struct symtab_command)) {
            const struct symtab_command *st = (const struct symtab_command *)lc;
            symoff = st->symoff; nsyms = st->nsyms; stroff = st->stroff; strsize = st->strsize;
        } else if (lc->cmd == LC_SEGMENT_64 && lc->cmdsize >= sizeof(struct segment_command_64)) {
            const struct segment_command_64 *sg = (const struct segment_command_64 *)lc;
            if (strncmp(sg->segname, "__TEXT", 16) == 0) {
                textFileOff = sg->fileoff; textVmAddr = sg->vmaddr; textSize = sg->filesize;
            }
        } else if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(struct uuid_command)) {
            memcpy(rt->uuid, ((const struct uuid_command *)lc)->uuid, 16);
            rt->hasUuid = true;
        }
        at += lc->cmdsize;
    }
    if (!nsyms || !strsize || nsyms > 0x200000 || strsize > 0x2000000)
        NVRT_FAIL("driver has no usable symbol table");
    /* The offsets the kext uses are image (vm) offsets; file offsets equal them in __TEXT only when the
     * segment starts at 0 in both, which is how every NVRM build so far has been laid out. */
    if (textFileOff != 0 || textVmAddr != 0 || !textSize)
        NVRT_FAIL("unexpected __TEXT layout (fileoff %#llx vmaddr %#llx)",
                  (unsigned long long)textFileOff, (unsigned long long)textVmAddr);

    symsLen = (size_t)nsyms * sizeof(struct nlist_64);
    strsLen = strsize;
    syms = (unsigned char *)alloc(symsLen);
    strs = (unsigned char *)alloc(strsLen + 1);
    if (!syms || !strs)
        NVRT_FAIL("out of memory for the symbol table");
    if (!rd(ctx, symoff, syms, symsLen) || !rd(ctx, stroff, strs, strsLen))
        NVRT_FAIL("cannot read the symbol table");
    strs[strsLen] = 0;

    for (UInt32 i = 0; i < nsyms && found != (1u << NVRT_COUNT) - 1; i++) {
        const struct nlist_64 *n = (const struct nlist_64 *)(syms + (size_t)i * sizeof(struct nlist_64));
        if ((n->n_type & N_STAB) || (n->n_type & N_TYPE) != N_SECT || n->n_un.n_strx >= strsLen)
            continue;
        const char *name = (const char *)strs + n->n_un.n_strx;
        for (unsigned k = 0; k < NVRT_COUNT; k++) {
            if (!(found & (1u << k)) && strcmp(name, kNvrtSymbols[k]) == 0) {
                rt->off[k] = n->n_value;
                found |= 1u << k;
                break;
            }
        }
    }
    /* Each decoded function ends where the next symbol starts: byte matching must not see a neighbour. */
    for (UInt32 i = 0; i < nsyms; i++) {
        const struct nlist_64 *n = (const struct nlist_64 *)(syms + (size_t)i * sizeof(struct nlist_64));
        if ((n->n_type & N_STAB) || (n->n_type & N_TYPE) != N_SECT)
            continue;
        if (n->n_value > rt->off[NVRT_LINK] && n->n_value < linkEnd)
            linkEnd = n->n_value;
        if (n->n_value > rt->off[NVRT_WRITESTART] && n->n_value < wsEnd)
            wsEnd = n->n_value;
    }
    for (unsigned k = 0; k < NVRT_COUNT; k++) {
        if (!(found & (1u << k)))
            NVRT_FAIL("driver does not export %s", kNvrtSymbols[k] + 1);
        if (rt->off[k] + 0x140 > textSize)
            NVRT_FAIL("%s lies outside __TEXT", kNvrtSymbols[k] + 1);
        if (!rd(ctx, rt->off[k], rt->sig[k], NVRM_RT_SIG_LEN))
            NVRT_FAIL("cannot read %s", kNvrtSymbols[k] + 1);
    }

    {
        /* Same instructions nvrm_bindings.py looks for, as bytes:
         *   gpuRusdInitPhysical_KERNEL        leaq d(%rdi),%r14   4c 8d b7 d32   physicalRmApi
         *                                     movq d(%rdi),%rdi   48 8b bf d32   userSharedData.pMemDesc
         *                                     movl d(%rbx),%esi   8b b3 d32      hInternalClient
         *                                     movl d(%rbx),%edx   8b 93 d32      hInternalSubdevice
         *                                     callq *d(%rbx)      ff 93 d32      RM_API::Control
         *   gpushareddataWriteStart_INTERNAL  movq d(%rdi),%rdx   48 8b 97 d32   userSharedData.pMapBuffer
         *                                     addq $i,%rax        48 05 i32      &userSharedData.data      */
        unsigned char init[0x140], ws[0x80];
        if (!rd(ctx, rt->off[NVRT_LINK], init, sizeof init) || !rd(ctx, rt->off[NVRT_WRITESTART], ws, sizeof ws))
            NVRT_FAIL("cannot read the RUSD functions");
        static const unsigned char pRmApi[] = { 0x4c, 0x8d, 0xb7 }, pMemDesc[] = { 0x48, 0x8b, 0xbf },
                                   pClient[] = { 0x8b, 0xb3 }, pSub[] = { 0x8b, 0x93 }, pCtl[] = { 0xff, 0x93 },
                                   pMap[] = { 0x48, 0x8b, 0x97 }, pData[] = { 0x48, 0x05 };
        size_t initLen = sizeof init, wsLen = sizeof ws;
        if (linkEnd - rt->off[NVRT_LINK] < initLen) initLen = (size_t)(linkEnd - rt->off[NVRT_LINK]);
        if (wsEnd - rt->off[NVRT_WRITESTART] < wsLen) wsLen = (size_t)(wsEnd - rt->off[NVRT_WRITESTART]);
        if (!nvrt_find_disp(init, initLen, rt->off[NVRT_LINK], pRmApi, sizeof pRmApi, &rt->physicalRmApi, &rt->site[0]))
            NVRT_FAIL("physicalRmApi not found");
        if (!nvrt_find_disp(init, initLen, rt->off[NVRT_LINK], pMemDesc, sizeof pMemDesc, &rt->usdMemDesc, &rt->site[1]))
            NVRT_FAIL("pMemDesc not found");
        if (!nvrt_find_disp(init, initLen, rt->off[NVRT_LINK], pClient, sizeof pClient, &rt->hClient, &rt->site[2]))
            NVRT_FAIL("hInternalClient not found");
        if (!nvrt_find_disp(init, initLen, rt->off[NVRT_LINK], pSub, sizeof pSub, &rt->hSubdevice, &rt->site[3]))
            NVRT_FAIL("hInternalSubdevice not found");
        if (!nvrt_find_disp(init, initLen, rt->off[NVRT_LINK], pCtl, sizeof pCtl, &rt->rmapiControl, &rt->site[4]))
            NVRT_FAIL("RM_API::Control not found");
        if (!nvrt_find_disp(ws, wsLen, rt->off[NVRT_WRITESTART], pMap, sizeof pMap, &rt->usdMapBuffer, &rt->site[5]))
            NVRT_FAIL("pMapBuffer not found");
        if (!nvrt_find_disp(ws, wsLen, rt->off[NVRT_WRITESTART], pData, sizeof pData, &rt->usdData, &rt->site[6]))
            NVRT_FAIL("userSharedData.data not found");
    }

    /* The checks nvrm_bindings.py makes, plus a sane range: these are offsets inside one OBJGPU. */
    if (rt->usdMemDesc + 8 != rt->usdMapBuffer)
        NVRT_FAIL("pMemDesc/pMapBuffer not adjacent (%#x %#x)", rt->usdMemDesc, rt->usdMapBuffer);
    if (rt->hClient + 8 != rt->hSubdevice)
        NVRT_FAIL("hInternalClient/hInternalSubdevice not one handle apart");
    if (rt->rmapiControl < rt->physicalRmApi || rt->rmapiControl > rt->physicalRmApi + 0x100)
        NVRT_FAIL("RM_API::Control not inside physicalRmApi");
    if (rt->usdData < rt->usdMapBuffer + 8)
        NVRT_FAIL("inline RUSD buffer not after pMapBuffer");
    if (rt->usdData > 0x100000 || rt->hClient < 0x100)
        NVRT_FAIL("field offsets out of range");
    ok = true;

out:
    if (cmds) release(cmds, cmdsLen);
    if (syms) release(syms, symsLen);
    if (strs) release(strs, strsLen + 1);
    return ok;
}

#undef NVRT_FAIL
