// LoginItem.swift — "start at login" that actually installs itself.
//
// The old toggle only called `launchctl enable/disable`. That does nothing for anybody who simply dragged
// the .app into place, because no LaunchAgent plist existed to enable — and the menu still showed a tick,
// so it looked like it worked. This writes (or removes) the agent itself, pointed at wherever the app lives
// right now, so it works for drag-installed copies as well.

import Foundation

enum LoginItem {
    static let label = "com.nullmoth.smcngpustatus"

    static var plistURL: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/LaunchAgents/\(label).plist")
    }

    private static var executablePath: String {
        Bundle.main.executableURL?.path ?? CommandLine.arguments[0]
    }

    @discardableResult
    private static func launchctl(_ args: [String]) -> String {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/launchctl")
        p.arguments = args
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        do { try p.run() } catch { return "" }
        p.waitUntilExit()
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        return String(data: data, encoding: .utf8) ?? ""
    }

    /// A label that launchd has marked disabled is listed as `"com.nullmoth.smcngpustatus" => disabled`.
    private static var isDisabledByLaunchd: Bool {
        let out = launchctl(["print-disabled", "gui/\(getuid())"])
        for line in out.split(separator: "\n") where line.contains(label) {
            return line.contains("disabled")
        }
        return false
    }

    private static var plistPointsAtThisApp: Bool {
        guard let data = try? Data(contentsOf: plistURL),
              let text = String(data: data, encoding: .utf8) else { return false }
        return text.contains(executablePath)
    }

    static var isEnabled: Bool {
        FileManager.default.fileExists(atPath: plistURL.path) && !isDisabledByLaunchd
    }

    private static func writePlist() -> Bool {
        let xml = """
        <?xml version="1.0" encoding="UTF-8"?>
        <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
        <plist version="1.0">
        <dict>
        	<key>Label</key>
        	<string>\(label)</string>
        	<key>ProgramArguments</key>
        	<array>
        		<string>\(executablePath)</string>
        	</array>
        	<key>RunAtLoad</key>
        	<true/>
        	<key>LimitLoadToSessionType</key>
        	<string>Aqua</string>
        	<key>ProcessType</key>
        	<string>Interactive</string>
        </dict>
        </plist>
        """
        do {
            try FileManager.default.createDirectory(at: plistURL.deletingLastPathComponent(),
                                                    withIntermediateDirectories: true)
            try xml.write(to: plistURL, atomically: true, encoding: .utf8)
            return true
        } catch {
            return false
        }
    }

    @discardableResult
    static func enable() -> Bool {
        guard writePlist() else { return false }
        _ = launchctl(["enable", "gui/\(getuid())/\(label)"])
        _ = launchctl(["bootout", "gui/\(getuid())/\(label)"])       // re-register with the new path
        let result = launchctl(["bootstrap", "gui/\(getuid())", plistURL.path])
        if result.contains("error") || result.contains("failed") {
            _ = launchctl(["load", "-w", plistURL.path])              // older launchctl syntax
        }
        return isEnabled
    }

    static func disable() {
        _ = launchctl(["bootout", "gui/\(getuid())/\(label)"])
        _ = launchctl(["disable", "gui/\(getuid())/\(label)"])
        try? FileManager.default.removeItem(at: plistURL)
    }

    /// Keeps a previously enabled agent valid after the app has been moved or updated.
    static func repairIfNeeded() {
        guard FileManager.default.fileExists(atPath: plistURL.path) else { return }
        guard !plistPointsAtThisApp else { return }
        _ = enable()
    }
}
