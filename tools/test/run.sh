#!/bin/bash
# run.sh — check the kext's runtime resolver against nvrm_bindings.py on an NVRM binary.
#
#     tools/test/run.sh [path/to/NVRM]     (default: the installed driver)
#
# Generates the reference header from the binary with nvrm_bindings.py, builds nvrt_check against the
# resolver the kext compiles in, and prints the two side by side. Exit status 0 means they agree on every
# entry point, signature and OBJGPU field, and the in-memory guard refuses a moved field.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
NVRM=${1:-/Library/Extensions/NVRM.kext/Contents/MacOS/NVRM}
OUT="$HERE/build"
mkdir -p "$OUT"

[ -f "$NVRM" ] || { echo "STOP: $NVRM not found"; exit 2; }
python3 "$ROOT/kext/nvrm_bindings.py" --binary "$NVRM" -o "$OUT/nvrm_bindings.h" >/dev/null
xcrun clang++ -std=c++17 -O1 -Wall -Wextra -Wno-unused-parameter \
    -I"$OUT" -I"$ROOT/kext" -o "$OUT/nvrt_check" "$HERE/nvrt_check.cpp"
"$OUT/nvrt_check" "$NVRM"
