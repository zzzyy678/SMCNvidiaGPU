// GPUProcess.swift — map the pid the driver reports to a human name.
//
// RUSD's procUtil section carries the pid of the process that owned the GPU sample. Kernel-side name lookup
// would need a proc_t dance for no benefit, so the kext publishes the pid and this resolves it in the app.

import AppKit
import Foundation

enum GPUProcess {
    /// The process's icon: the app bundle icon when there is one, otherwise the generic icon of the
    /// executable (so command-line tools such as `grep` still get a recognisable glyph).
    static func icon(for pid: Int) -> NSImage? {
        guard pid > 0 else { return nil }
        if let app = NSRunningApplication(processIdentifier: pid_t(pid)), let icon = app.icon {
            return icon
        }
        guard let path = executablePath(for: pid) else { return nil }
        let icon = NSWorkspace.shared.icon(forFile: path)
        return icon.size.width > 0 ? icon : nil
    }

    /// Absolute path of the process's executable, if the pid is still alive.
    static func executablePath(for pid: Int) -> String? {
        var buffer = [CChar](repeating: 0, count: Int(4 * MAXPATHLEN))
        let length = proc_pidpath(pid_t(pid), &buffer, UInt32(buffer.count))
        guard length > 0 else { return nil }
        return String(cString: buffer)
    }

    /// Best-effort display name for a pid: the app name for GUI apps, the executable name otherwise.
    static func name(for pid: Int) -> String {
        guard pid > 0 else { return "—" }
        if let app = NSRunningApplication(processIdentifier: pid_t(pid)) {
            if let name = app.localizedName, !name.isEmpty { return name }
        }
        if let path = executablePath(for: pid) {
            return (path as NSString).lastPathComponent
        }
        return "pid \(pid)"
    }
}
