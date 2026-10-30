#!/bin/bash
# Build the menu bar app. Needs only the Command Line Tools (swiftc).
#
#     app/build.sh          → app/build/SMCNvidiaGPUStatus.app
#
# -target keeps the deployment target at macOS 11 so the binary also runs on Big Sur and later;
# without it swiftc stamps the current SDK version into LC_BUILD_VERSION and older systems refuse it.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
APP="$HERE/build/SMCNvidiaGPUStatus.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
swiftc -O -target x86_64-apple-macos12.0 -o "$APP/Contents/MacOS/SMCNvidiaGPUStatus" \
    "$HERE/main.swift" "$HERE/SMCNvidiaGPUStatus.swift" "$HERE/SMCFans.swift" "$HERE/LoginItem.swift" "$HERE/InstanceLock.swift" "$HERE/GPUInfo.swift" "$HERE/Machine.swift" -framework Cocoa -framework IOKit
cp "$HERE/Info.plist" "$APP/Contents/Info.plist"
if [ -d "$HERE/Resources" ]; then mkdir -p "$APP/Contents/Resources" && cp -R "$HERE/Resources/." "$APP/Contents/Resources/"; fi
chmod 755 "$APP/Contents/MacOS/SMCNvidiaGPUStatus"
codesign --force --sign - "$APP" >/dev/null 2>&1 || true
echo "built: $APP"
