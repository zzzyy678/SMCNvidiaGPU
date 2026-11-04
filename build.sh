#!/bin/bash
# 构建用户态工具：SMC 读键工具（smcread）、统一查询工具（nvstat）。
#
# 内核部分：
#   kext/build.sh     SMCNvidiaGPU.kext（VirtualSMC 插件 + RUSD）。需要 VirtualSMC + Lilu 源码提供
#                     SDK 头，以及本机已安装的 com.nullmoth.NVRM —— 脚本会从驱动里取入口偏移与签名。
#
#     ./build.sh
set -euo pipefail
cd "$(dirname "$0")"

OUT=build
rm -rf "$OUT"
mkdir -p "$OUT"

echo "== smcread（SMC 读键工具）"
clang -O2 -Wall -framework IOKit -o "$OUT/smcread" tools/smcread.c

echo "== nvstat（统一查询工具）"
clang -O2 -Wall -framework IOKit -o "$OUT/nvstat" nvstat/nvstat.c

echo "== done: $PWD/$OUT"
ls -l "$OUT"
