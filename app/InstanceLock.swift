// InstanceLock.swift — keep exactly one copy of the menu bar app alive.
//
// Without this, the login agent and a manual double-click can both start the app, so two identical items
// appear in the menu bar. The lock is an flock held for the lifetime of the process; the kernel releases it
// when the process exits, so there is no stale-file problem.

import Foundation

enum InstanceLock {
    private static var fd: Int32 = -1

    /// Returns false when another copy is already running.
    @discardableResult
    static func claim() -> Bool {
        let dir = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/SMCNvidiaGPUStatus")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let path = dir.appendingPathComponent("instance.lock").path

        fd = open(path, O_CREAT | O_RDWR, 0o644)
        guard fd >= 0 else { return true }          // cannot create a lock: better to run than to vanish
        if flock(fd, LOCK_EX | LOCK_NB) != 0 {
            return false
        }
        return true
    }
}
