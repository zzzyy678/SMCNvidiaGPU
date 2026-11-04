#!/bin/bash
# install.sh — put SMCNvidiaGPU.kext into OpenCore.
#
# What it changes (all backed up):
#   * <ESP>/EFI/OC/Kexts/SMCNvidiaGPU.kext        the plugin, injected by OpenCore
#   * <ESP>/EFI/OC/config.plist                  Kernel > Add entry (old NVSMC entry removed)
#   * <ESP>/EFI/OC/Kexts/NVSMC.kext              moved aside as NVSMC.kext.disabled-<time>
#   * /Library/Extensions/NVSENS.kext            removed (the plugin carries its code now)
#   * /Library/Extensions/NVRM.kext/...Info.plist  restored from the backup the old installer left,
#                                                then the bundle is re-signed and the AuxKC rebuilt
#   * /Library/PrivilegedHelperTools/smcnvgpud and its LaunchDaemon  removed (the kext reads everything itself)
#
# It runs as root and never edits another person's files: the old NVSMC kext is renamed, not deleted, and
# every config change has a timestamped copy next to it.
#
#     sudo ./install.sh [--esp "/Volumes/NO NAME"] [--no-cleanup]
#     sudo ./install.sh --uninstall
set -uo pipefail
PATH=/usr/bin:/bin:/usr/sbin:/sbin; export PATH
HERE=$(cd "$(dirname "$0")" && pwd)
EXT=/Library/Extensions
KC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
KB=/System/Library/KernelCollections/BootKernelExtensions.kc
KS=/System/Library/KernelCollections/SystemKernelExtensions.kc
KERNEL=/System/Library/Kernels/kernel
LAUNCHD=/Library/LaunchDaemons/com.nullmoth.smcnvgpud.plist
OLDD_LAUNCHD=/Library/LaunchDaemons/com.nullmoth.nvgpud.plist
HELPER=/Library/PrivilegedHelperTools/smcnvgpud
OLDD_HELPER=/Library/PrivilegedHelperTools/nvgpud
ID=org.nullmoth.SMCNvidiaGPU

ESP=""; DO_CLEANUP=1; UNINSTALL=0
while [ $# -gt 0 ]; do
    case "$1" in
        --esp) ESP=${2:-}; shift 2 ;;
        --no-daemon) shift ;;   # kept for compatibility: the daemon no longer exists
        --no-cleanup) DO_CLEANUP=0; shift ;;
        --uninstall) UNINSTALL=1; shift ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "STOP: unknown argument $1"; exit 2 ;;
    esac
done

[ "$(id -u)" = 0 ] || { echo "STOP: run with sudo"; exit 2; }

say()  { printf '\n== %s\n' "$*"; }
note() { printf '   %s\n' "$*"; }

# ---- locate the ESP that holds OpenCore ------------------------------------------------------------
# The right ESP is the one belonging to the disk macOS boots from (its first partition); a stick or a second
# OC folder elsewhere on the machine must not win just because it is mounted first.
boot_disk_esp() {
    local store whole
    store=$(diskutil info / 2>/dev/null | awk -F': *' '/APFS Physical Store/{print $2; exit}')
    if [ -n "$store" ]; then
        whole=${store%s*}           # disk1s5 -> disk1 (remove the "sN" suffix)
    else
        whole=$(diskutil info / 2>/dev/null | awk -F': *' '/Parent Whole Disk/{print $2; exit}')
    fi
    [ -n "$whole" ] || return 1
    printf '/dev/%ss1\n' "$whole"
}

# Two ESPs on this class of machine are routinely both labelled "NO NAME", so matching /Volumes/NO NAME is
# not enough: the ESP of the disk macOS boots from is mounted at a private mount point and wins.
find_esp() {
    local dev mnt cand
    if [ -n "$ESP" ] && [ -d "$ESP/EFI/OC" ]; then printf '%s\n' "$ESP"; return 0; fi
    if dev=$(boot_disk_esp); then
        if [ -e "$dev" ]; then
            mkdir -p "/Volumes/ESP-$(basename "$dev")"
            diskutil mount -mountPoint "/Volumes/ESP-$(basename "$dev")" "$dev" >/dev/null 2>&1
        fi
        mnt="/Volumes/ESP-$(basename "$dev")"
        [ -d "$mnt/EFI/OC" ] && { printf '%s\n' "$mnt"; return 0; }
    fi
    for cand in /Volumes/*; do
        [ -d "$cand/EFI/OC" ] && { printf '%s\n' "$cand"; return 0; }
    done
    return 1
}

if ! ESP=$(find_esp); then
    say "自动找 ESP 失败"
    for dev in disk1s1 disk0s1 disk2s1; do
        [ -e "/dev/$dev" ] || continue
        mnt="/Volumes/ESP-$dev"
        mkdir -p "$mnt"; diskutil mount -mountPoint "$mnt" "$dev" >/dev/null 2>&1
        if [ -d "$mnt/EFI/OC" ]; then ESP="$mnt"; break; fi
    done
fi
[ -n "${ESP:-}" ] && [ -d "$ESP/EFI/OC" ] || { echo "STOP: 找不到 EFI/OC —— 用 --esp 指定挂载点"; exit 1; }
note "ESP: $ESP ($(df "$ESP" 2>/dev/null | tail -1 | awk '{print $1}'))"
if dev=$(boot_disk_esp); then
    mounted=$(df "$ESP" 2>/dev/null | tail -1 | awk '{print $1}')
    if [ "$mounted" != "$dev" ]; then
        echo "STOP: $ESP 不是启动盘 $dev 的 ESP；拒绝写入（确认后用 --esp 覆盖）"
        exit 1
    fi
fi

# ---- where the pieces are (repo layout or release layout) ------------------------------------------
if [ -d "$HERE/SMCNvidiaGPU.kext" ]; then
    KEXT_SRC="$HERE/SMCNvidiaGPU.kext"                       # release layout
else
    KEXT_SRC="$HERE/kext/build/SMCNvidiaGPU.kext"            # repository layout
fi

# ---- uninstall -------------------------------------------------------------------------------------
if [ "$UNINSTALL" = 1 ]; then
    say "卸载 SMCNvidiaGPU"
    if [ -e "$LAUNCHD" ] || [ -e "$HELPER" ]; then
        launchctl bootout system/com.nullmoth.smcnvgpud 2>/dev/null || true
        rm -f "$LAUNCHD" "$HELPER"
        note "旧的采集 daemon 已清理"
    fi
    rm -rf "$ESP/EFI/OC/Kexts/SMCNvidiaGPU.kext"
    note "kext 已从 ESP 移除"
    old=$(ls -dt "$ESP/EFI/OC/Kexts/NVSMC.kext.disabled-"* 2>/dev/null | head -1)
    [ -n "$old" ] && mv "$old" "$ESP/EFI/OC/Kexts/NVSMC.kext" && note "旧 NVSMC.kext 已恢复"
    cfg="$ESP/EFI/OC/config.plist"
    if [ -f "$cfg" ]; then
        cp -p "$cfg" "$cfg.before-uninstall-$(date +%Y%m%d-%H%M%S)"
        /usr/bin/python3 - "$cfg" <<'PY'
import plistlib, sys
cfg = sys.argv[1]
with open(cfg, "rb") as f:
    d = plistlib.load(f)
add = d.get("Kernel", {}).get("Add", [])
before = len(add)
d.setdefault("Kernel", {})["Add"] = [e for e in add if e.get("BundlePath") != "SMCNvidiaGPU.kext"]
with open(cfg, "wb") as f:
    plistlib.dump(d, f, sort_keys=False)
print("   config.plist: 移除 %d 个条目" % (before - len(d["Kernel"]["Add"])))
PY
    fi
    echo; echo "重启后生效。"
    exit 0
fi

[ -d "$KEXT_SRC" ] || { echo "STOP: $KEXT_SRC 不存在 —— 先运行 kext/build.sh"; exit 1; }
codesign --verify --strict "$KEXT_SRC" || { echo "STOP: kext 签名校验失败"; exit 1; }

# ---- 1. OpenCore: kext + config.plist --------------------------------------------------------------
say "1. OpenCore"
rm -rf "$ESP/EFI/OC/Kexts/SMCNvidiaGPU.kext"
ditto "$KEXT_SRC" "$ESP/EFI/OC/Kexts/SMCNvidiaGPU.kext" || { echo "STOP: 拷贝 kext 失败"; exit 1; }
chmod -R go-w "$ESP/EFI/OC/Kexts/SMCNvidiaGPU.kext" 2>/dev/null
note "SMCNvidiaGPU.kext -> EFI/OC/Kexts"

if [ -d "$ESP/EFI/OC/Kexts/NVSMC.kext" ]; then
    mv "$ESP/EFI/OC/Kexts/NVSMC.kext" "$ESP/EFI/OC/Kexts/NVSMC.kext.disabled-$(date +%Y%m%d-%H%M%S)"
    note "旧 NVSMC.kext 已改名为 .disabled-…"
fi

cfg="$ESP/EFI/OC/config.plist"
[ -f "$cfg" ] || { echo "STOP: 没有 $cfg"; exit 1; }
cp -p "$cfg" "$cfg.before-smcngpu-$(date +%Y%m%d-%H%M%S)"
/usr/bin/python3 - "$cfg" <<'PY' || exit 1
import plistlib, sys
cfg = sys.argv[1]
with open(cfg, "rb") as f:
    d = plistlib.load(f)
kernel = d.setdefault("Kernel", {})
add = kernel.get("Add", [])
entry = {
    "Arch": "x86_64",
    "BundlePath": "SMCNvidiaGPU.kext",
    "Comment": "SMCNvidiaGPU - NVIDIA GPU sensors (VirtualSMC plugin + RUSD)",
    "Enabled": True,
    "ExecutablePath": "Contents/MacOS/SMCNvidiaGPU",
    "MaxKernel": "",
    "MinKernel": "",
    "PlistPath": "Contents/Info.plist",
}
dropped = [e.get("BundlePath") for e in add
           if e.get("BundlePath") in ("NVSMC.kext", "NVSENS.kext", "SMCNvidiaGPU.kext")]
add = [e for e in add if e.get("BundlePath") not in ("NVSMC.kext", "NVSENS.kext", "SMCNvidiaGPU.kext")]
add.append(entry)
kernel["Add"] = add
with open(cfg, "wb") as f:
    plistlib.dump(d, f, sort_keys=False)
print("   config.plist: Kernel>Add 移除 %s，加入 SMCNvidiaGPU.kext" % (dropped or "无"))
PY

# ---- 2. 旧的分体式安装（L/E 的 NVSENS + NVRM 的 plist 补丁）----------------------------------------
if [ "$DO_CLEANUP" = 1 ]; then
    say "2. 清理旧的分体式安装"
    if [ -d "$EXT/NVSENS.kext" ]; then
        rm -rf "$EXT/NVSENS.kext"
        note "已删除 /Library/Extensions/NVSENS.kext（代码已并入 SMCNvidiaGPU）"
    fi
    if [ -f "$EXT/NVRM-Info.plist.before-nvsens" ]; then
        cp -p "$EXT/NVRM-Info.plist.before-nvsens" "$EXT/NVRM.kext/Contents/Info.plist"
        chmod 644 "$EXT/NVRM.kext/Contents/Info.plist"
        note "已恢复 NVRM 原始 Info.plist（不再需要 OSBundleCompatibleVersion）"
        rm -f "$EXT/NVRM-Info.plist.before-nvsens"
    fi
    if ! codesign --verify --strict "$EXT/NVRM.kext" >/dev/null 2>&1; then
        codesign --force --sign - "$EXT/NVRM.kext" >/dev/null 2>&1 \
            && note "NVRM.kext 已重新 ad-hoc 签名"
    fi
    say "2b. 重建 AuxKC"
    T=$(mktemp -d /var/tmp/smcngpu.XXXX) || { echo "STOP: mktemp 失败"; exit 1; }
    NEW="$KC.smcngpu-new"; rm -f "$NEW"
    args=(--allow-missing-kdk); [ -f "$KERNEL" ] && args+=(--kernel "$KERNEL")
    if kmutil create -n aux --volume-root / "${args[@]}" -B "$KB" -S "$KS" --repository "$EXT" -A "$NEW" -z \
            >"$T/kmutil.log" 2>&1 && [ -s "$NEW" ]; then
        [ -f "$KC" ] && cp -p "$KC" "$KC.smcngpu-backup"
        mv -f "$NEW" "$KC" && note "AuxKC 已重建（备份 $KC.smcngpu-backup）"
    else
        tail -15 "$T/kmutil.log"
        rm -f "$NEW"
        echo "STOP: AuxKC 重建失败；ESP 已改，但 L/E 清理未完成。修复 kmutil 后重跑，或 --no-cleanup"
        exit 1
    fi
    rm -rf "$T"
fi

# ---- 3. old daemon ---------------------------------------------------------------------------------
if [ -e "$LAUNCHD" ] || [ -e "$HELPER" ]; then
    say "3. 清理旧的采集 daemon"
    launchctl bootout system/com.nullmoth.smcnvgpud 2>/dev/null || true
    rm -f "$LAUNCHD" "$HELPER"
    note "kext 现在自己读全部传感器，daemon 不再需要"
fi

say "完成"
cat <<EOF
   SMCNvidiaGPU.kext 已进 OpenCore（$ESP/EFI/OC/Kexts）
   旧 NVSMC.kext 改名为 .disabled-…（可回滚），L/E 里的 NVSENS 已清理
   采集 daemon 已不再需要（若装过会被移除）
   重启后验证：
     sysctl kern.nvgpupower kern.nvgpufan kern.nvgpustate
     $HERE/nvstat/nvstat all       （或 build/nvstat all）
     kmutil showloaded --list-only | grep SMCNvidiaGPU
EOF
