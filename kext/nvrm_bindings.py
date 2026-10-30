#!/usr/bin/env python3
"""nvrm_bindings.py — build the table SMCNvidiaGPU.kext needs to reach NVRM at run time.

The SMC plugin is injected by OpenCore, so it loads from the boot kernel collection *before* the NullMoth
driver. It therefore cannot link against com.nullmoth.NVRM (a kext loaded later, from the auxiliary
collection). Instead it finds the driver at run time:

  * NVRM publishes, on its NVRMDisplay nub, the property "phys-for-va" whose value is the address of its
    own function nv_xnu_phys_for_kernel_va (kexts/NVRM/NVRM.cpp: nub->setProperty("phys-for-va",
    (unsigned long long)(uintptr_t)&nv_xnu_phys_for_kernel_va, 64)). Subtracting that function's offset in
    the binary gives the driver's load address, with KASLR already accounted for.
  * Every other entry point is then base + its own offset, and each one is checked against the first bytes
    of the function captured here before it is called.

This script lifts those offsets, the entry-point signatures and the OBJGPU field offsets out of the NVRM
installed on this machine, and writes nvrm_bindings.h. Run it (through kext/build.sh) on the Mac that will
load the kext; when the driver changes, rebuild and the numbers follow.

Usage: nvrm_bindings.py [-o header.h] [--binary /Library/Extensions/NVRM.kext/Contents/MacOS/NVRM]
"""

import argparse
import hashlib
import os
import re
import subprocess
import sys

NM = "/usr/bin/nm"
OBJDUMP = "/usr/bin/xcrun"

# Entry points, in the order the header declares them. The first one is the anchor: its address is the value
# NVRM publishes, so it must be a symbol the driver really hands out.
SYMBOLS = [
    ("anchor", "_nv_xnu_phys_for_kernel_va", "the address NVRM publishes as \"phys-for-va\""),
    ("gpumgrGetGpu", "_gpumgrGetGpu", "OBJGPU *gpumgrGetGpu(NvU32 gpuInstance)"),
    ("createRusd", "_gpuCreateRusdMemory_IMPL", "allocates/maps the RUSD block and links it to the GSP"),
    ("linkRusd", "_gpuRusdInitPhysical_KERNEL", "re-sends the RUSD address to the GSP"),
    ("lock", "_rmapiLockAcquire", "RM API lock, required around every RM call"),
    ("unlock", "_rmapiLockRelease", "RM API unlock"),
    ("pmEnter", "_nv_xnu_pm_enter", "NVRM's system-sleep gate"),
    ("pmExit", "_nv_xnu_pm_exit", ""),
]

OFFSETS = [
    ("OFF_HINTERNAL_CLIENT", "OBJGPU::hInternalClient"),
    ("OFF_HINTERNAL_SUBDEVICE", "OBJGPU::hInternalSubdevice"),
    ("OFF_PHYSICAL_RMAPI", "OBJGPU::physicalRmApi"),
    ("OFF_RMAPI_CONTROL", "RM_API::Control inside physicalRmApi"),
    ("OFF_USD_PMEMDESC", "OBJGPU::userSharedData.pMemDesc"),
    ("OFF_USD_PMAPBUFFER", "OBJGPU::userSharedData.pMapBuffer"),
    ("OFF_USD_DATA", "OBJGPU::userSharedData.data (inline fallback buffer)"),
]

SIG_LEN = 16


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("STOP: %s failed:\n%s" % (" ".join(cmd), r.stderr.strip()[:2000]))
    return r.stdout


def symbol_table(binary):
    table = {}
    for line in run([NM, "-n", binary]).splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in ("T", "t"):
            table[parts[2]] = int(parts[0], 16)
    return table


def disassemble(binary, start, end):
    out = run([OBJDUMP, "llvm-objdump", "-d", "--no-show-raw-insn",
               "--start-address=0x%x" % start, "--stop-address=0x%x" % end, binary])
    return out.splitlines()


def find_one(lines, pattern, what):
    rx = re.compile(pattern)
    hits = [m for m in (rx.search(l) for l in lines) if m]
    if len(hits) != 1:
        raise SystemExit("STOP: %s: expected one match for %r, found %d" % (what, pattern, len(hits)))
    return int(hits[0].group(1), 16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="/Library/Extensions/NVRM.kext/Contents/MacOS/NVRM")
    ap.add_argument("-o", "--output", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                           "nvrm_bindings.h"))
    args = ap.parse_args()
    binary = args.binary
    if not os.path.isfile(binary):
        raise SystemExit("STOP: no NVRM binary at %s" % binary)
    image = open(binary, "rb").read()

    syms = symbol_table(binary)
    missing = [s for _, s, _ in SYMBOLS if s not in syms]
    if missing:
        raise SystemExit("STOP: NVRM does not export %s — not a build this tool understands"
                         % ", ".join(m.lstrip("_") for m in missing))
    for name in ("_gpuRusdInitPhysical_KERNEL", "_gpushareddataWriteStart_INTERNAL"):
        if name not in syms:
            raise SystemExit("STOP: NVRM does not export %s" % name.lstrip("_"))

    init = disassemble(binary, syms["_gpuRusdInitPhysical_KERNEL"],
                       syms["_gpuRusdInitPhysical_KERNEL"] + 0x140)
    write_start = disassemble(binary, syms["_gpushareddataWriteStart_INTERNAL"],
                              syms["_gpushareddataWriteStart_INTERNAL"] + 0x80)

    fields = {
        "OFF_PHYSICAL_RMAPI": find_one(init, r"leaq\s+0x([0-9a-f]+)\(%rdi\), %r14", "physicalRmApi"),
        "OFF_USD_PMEMDESC": find_one(init, r"movq\s+0x([0-9a-f]+)\(%rdi\), %rdi", "pMemDesc"),
        "OFF_HINTERNAL_CLIENT": find_one(init, r"movl\s+0x([0-9a-f]+)\(%rbx\), %esi", "hInternalClient"),
        "OFF_HINTERNAL_SUBDEVICE": find_one(init, r"movl\s+0x([0-9a-f]+)\(%rbx\), %edx", "hInternalSubdevice"),
        "OFF_RMAPI_CONTROL": find_one(init, r"callq\s+\*0x([0-9a-f]+)\(%rbx\)", "RM_API::Control"),
        "OFF_USD_PMAPBUFFER": find_one(write_start, r"movq\s+0x([0-9a-f]+)\(%rdi\), %rdx", "pMapBuffer"),
        "OFF_USD_DATA": find_one(write_start, r"addq\s+\$0x([0-9a-f]+), %rax", "&userSharedData.data"),
    }

    def need(cond, why):
        if not cond:
            raise SystemExit("STOP: parsed values are inconsistent: %s\nfields: %s"
                             % (why, ", ".join("%s=0x%x" % kv for kv in sorted(fields.items()))))

    need(fields["OFF_USD_PMEMDESC"] + 8 == fields["OFF_USD_PMAPBUFFER"], "pMemDesc/pMapBuffer not adjacent")
    need(fields["OFF_HINTERNAL_CLIENT"] + 8 == fields["OFF_HINTERNAL_SUBDEVICE"],
         "hInternalClient/hInternalSubdevice not one handle apart")
    need(fields["OFF_PHYSICAL_RMAPI"] <= fields["OFF_RMAPI_CONTROL"] <= fields["OFF_PHYSICAL_RMAPI"] + 0x100,
         "RM_API::Control not inside physicalRmApi")
    need(fields["OFF_USD_DATA"] >= fields["OFF_USD_PMAPBUFFER"] + 8, "inline RUSD buffer not after pMapBuffer")

    # Signatures: the first bytes of every entry point, read straight out of __TEXT (vmaddr 0, fileoff 0 in a
    # kext), so the kext can refuse to call an address that is not the function it expects.
    sigs = {}
    for key, sym, _ in SYMBOLS:
        off = syms[sym]
        if off + SIG_LEN > len(image):
            raise SystemExit("STOP: %s lies outside the image" % sym)
        sigs[key] = image[off:off + SIG_LEN]

    digest = hashlib.sha256(image).hexdigest()
    uuid = ""
    try:
        r = subprocess.run(["/usr/bin/dwarfdump", "--uuid", binary], capture_output=True, text=True)
        m = re.search(r"UUID:\s*([0-9A-Fa-f-]+)", r.stdout)
        uuid = m.group(1) if m else ""
    except Exception:
        pass

    lines = [
        "/* nvrm_bindings.h — generated by nvrm_bindings.py; do not edit.",
        " *",
        " * Where NVRM keeps the functions and OBJGPU fields SMCNvidiaGPU.kext calls, recovered from",
        " *   %s" % binary,
        " * driver sha256 %s%s" % (digest, ("  uuid " + uuid) if uuid else ""),
        " *",
        " * The kext finds the driver through the address NVRM publishes as \"phys-for-va\", then checks",
        " * every entry point against NVRM_SIG_* before calling it. A driver that does not match is left alone.",
        " */",
        "#pragma once",
        "",
        "#define NVRM_DRIVER_SHA256 \"%s\"" % digest,
        "#define NVRM_DRIVER_UUID   \"%s\"" % uuid,
        "#define NVRM_ANCHOR_OFF   0x%xULL   /* nv_xnu_phys_for_kernel_va */" % syms["_nv_xnu_phys_for_kernel_va"],
        "",
    ]
    for key, sym, note in SYMBOLS:
        if key == "anchor":
            continue
        lines.append("#define NVRM_OFF_%-10s 0x%xULL%s" % (key, syms[sym], ("   /* " + note + " */") if note else ""))
    lines.append("")
    lines.append("#define NVRM_SIG_LEN %du" % SIG_LEN)
    for key, sym, _ in SYMBOLS:
        body = ", ".join("0x%02x" % b for b in sigs[key])
        lines.append("static const unsigned char NVRM_SIG_%s[NVRM_SIG_LEN] = { %s };" % (key, body))
    lines.append("")
    for name, comment in OFFSETS:
        lines.append("#define %-24s 0x%04x   /* %s */" % (name, fields[name], comment))
    lines.append("")
    lines.append("#define NVRM_RUSD_LAYOUT_MAGIC 0x%08x   /* identifies this layout in logs */"
                 % (((fields["OFF_USD_PMEMDESC"] << 16) | (fields["OFF_RMAPI_CONTROL"] & 0xffff)) & 0xffffffff))
    lines.append("")

    header = "\n".join(lines)
    old = open(args.output).read() if os.path.exists(args.output) else None
    if old != header:
        with open(args.output, "w") as f:
            f.write(header)
    print("nvrm_bindings.h: driver %s%s" % (digest[:12], (" uuid " + uuid) if uuid else ""))
    print("  " + ", ".join("%s=0x%x" % (k[4:].lower(), fields[k]) for k, _ in OFFSETS))
    print("  entry points: " + ", ".join("%s=0x%x" % (k, syms[s]) for k, s, _ in SYMBOLS if k != "anchor"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
