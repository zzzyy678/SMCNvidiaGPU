// Entry point. Kept in main.swift because top-level code is only allowed there when the app is built
// from several files.
//
// Two hidden arguments make the login item testable without clicking through the menu:
//   SMCNvidiaGPUStatus --enable-login-item   → installs the LaunchAgent, exit code 0 on success
//   SMCNvidiaGPUStatus --disable-login-item  → removes it

import Foundation

if CommandLine.arguments.contains("--enable-login-item") {
    exit(LoginItem.enable() ? 0 : 1)
}
if CommandLine.arguments.contains("--disable-login-item") {
    LoginItem.disable()
    exit(0)
}

guard InstanceLock.claim() else {
    exit(0)             // another copy is already in the menu bar
}

runApp()
