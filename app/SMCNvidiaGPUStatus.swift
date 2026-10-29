// SMCNvidiaGPUStatus — menu bar readout for SMCNvidiaGPU.kext + smcnvgpud.
//
// Everything comes from the sysctls the kext publishes (kern.nvgpu*): power, fan, temperature, clocks,
// utilisation, VRAM, P-State, PCIe link, throttle reasons, ECC/BAR1 when the firmware fills them, and the
// VBIOS version when kern.nvgpuvbios_override is set.
//
// Build:   app/build.sh      → app/build/SMCNvidiaGPUStatus.app
// Install: sudo app/install.sh (copies to /Applications and starts it at login)

import Cocoa

// ---------------------------------------------------------------- sysctls

private func sysctlInt(_ name: String) -> Int? {
    var size = 0
    guard sysctlbyname(name, nil, &size, nil, 0) == 0 else { return nil }
    if size == 4 {
        var v: Int32 = 0
        guard sysctlbyname(name, &v, &size, nil, 0) == 0 else { return nil }
        return Int(v)
    }
    if size == 8 {
        var v: Int64 = 0
        guard sysctlbyname(name, &v, &size, nil, 0) == 0 else { return nil }
        return Int(v)
    }
    return nil
}

private func sysctlString(_ name: String) -> String? {
    var size = 0
    guard sysctlbyname(name, nil, &size, nil, 0) == 0, size > 0 else { return nil }
    var buf = [CChar](repeating: 0, count: size + 1)
    guard sysctlbyname(name, &buf, &size, nil, 0) == 0 else { return nil }
    let s = String(cString: buf)
    return s.isEmpty ? nil : s
}

private struct Snapshot {
    var loaded = false
    var temp = 0, powerMW = 0, instMW = 0, tgpMW = 0, limitMW = 0
    var fanRPM = 0, fanCount = 0, fanMax = 3500
    var fans: [Int] = []
    var systemFans: [SMCFan] = []      // fallback for laptop GPUs that expose no fan of their own
    var gpcMHz = 0, dramMHz = 0, nvdMHz = 0
    var util = 0, fb = 0, enc = 0, dec = 0
    var vramUsedMB = 0, vramTotalMB = 0
    var pstate = 0
    var pcieGen = 0, pcieWidth = 0
    var throttle = -1
    var eccCorr = 0, eccUncorr = 0
    var bar1Total = 0, bar1Free = 0
    var vbios: String?
    var state = ""
    var live = 0, poll = 0, layout = 0
}

private func readSnapshot() -> Snapshot {
    var s = Snapshot()
    guard let temp = sysctlInt("kern.nvgputemp") else { return s }
    s.loaded = true
    s.temp = temp
    s.powerMW = sysctlInt("kern.nvgpupower") ?? 0
    s.instMW = sysctlInt("kern.nvgpuinstpower") ?? 0
    s.tgpMW = sysctlInt("kern.nvgputgp") ?? 0
    s.limitMW = sysctlInt("kern.nvgpupwrlimit") ?? 0
    s.fanRPM = sysctlInt("kern.nvgpufan") ?? 0
    s.fanCount = sysctlInt("kern.nvgpufancount") ?? 0
    s.fanMax = sysctlInt("kern.nvgpufanmax") ?? 3500
    /* Every slot the card reports gets a row, including one that is momentarily stopped (0 RPM), so a
     * three-fan card always shows three fans instead of hiding the idle one. The high-water mark is
     * remembered across launches because the firmware only reports fans that are actually turning. */
    let fanCountKey = "fanSlotsSeen"
    let rememberedSlots = UserDefaults.standard.integer(forKey: fanCountKey)
    if s.fanCount > rememberedSlots {
        UserDefaults.standard.set(s.fanCount, forKey: fanCountKey)
    }
    let slots = max(s.fanCount, rememberedSlots, 1)
    for i in 0..<min(slots, 8) {
        if let v = sysctlInt("kern.nvgpufan\(i)") { s.fans.append(v) }
    }
    if s.fans.allSatisfy({ $0 == 0 }) {
        s.fanRPM = s.fans.max() ?? 0
    }
    if s.fans.isEmpty && s.fanRPM > 0 { s.fans = [s.fanRPM] }
    /* Only when the card reports no fan of its own (laptops usually hand the cooler to the EC): fall back to
     * the system fans. A desktop card whose fans are simply stopped keeps showing its own two fans as
     * "stopped" — other case fans must not appear there. */
    /* Fall back only for a card that has never reported a fan of its own. A desktop card whose fans are
     * stopped reports count 0 on that tick, which used to flip the whole fan section over to the case fans;
     * the remembered high-water mark rules that out once the card has spun its fans even once. */
    let forceSystemFans = ProcessInfo.processInfo.environment["SMCNVIDIA_FORCE_SYSTEM_FANS"] == "1"
    let cardHasOwnFans = rememberedSlots > 0 || s.fanCount > 0
    /* Three signals decide whether the system fans are relevant: the card model (desktop parts never fall
     * back), a battery (a real laptop), and whether this card ever reported a fan of its own. */
    let form = GPUForm.classify(model: GPUIdentity.current()?.name)
    let laptop = Machine.hasBattery
    let allowSystemFans = (form == .laptop) || (form == .unknown && laptop)
    if forceSystemFans || (s.fanCount == 0 && !cardHasOwnFans && allowSystemFans) {
        s.systemFans = SMCReader.shared.systemFans()
        if forceSystemFans { s.fans = [] }
    }
    if s.fans.isEmpty, let first = s.systemFans.first, s.fanRPM == 0 {
        s.fanRPM = first.rpm
    }
    s.gpcMHz = sysctlInt("kern.nvggpcclk") ?? 0
    s.dramMHz = sysctlInt("kern.nvgdramclk") ?? 0
    s.nvdMHz = sysctlInt("kern.nvgnvdclk") ?? 0
    s.util = (sysctlInt("kern.nvgutil") ?? 0) / 100
    s.fb = (sysctlInt("kern.nvgutilfb") ?? 0) / 100
    s.enc = (sysctlInt("kern.nvgutilenc") ?? 0) / 100
    s.dec = (sysctlInt("kern.nvgutildec") ?? 0) / 100
    s.vramUsedMB = sysctlInt("kern.nvgvramused") ?? 0
    s.vramTotalMB = sysctlInt("kern.nvgvramtotal") ?? 0
    s.pstate = sysctlInt("kern.nvgpstate") ?? 0
    s.pcieGen = sysctlInt("kern.nvgpupciegen") ?? 0
    s.pcieWidth = sysctlInt("kern.nvgpupciewidth") ?? 0
    s.throttle = sysctlInt("kern.nvgputhrottle") ?? -1
    s.eccCorr = sysctlInt("kern.nvgpuecccorr") ?? 0
    s.eccUncorr = sysctlInt("kern.nvgpueccuncorr") ?? 0
    s.bar1Total = sysctlInt("kern.nvgpubar1total") ?? 0
    s.bar1Free = sysctlInt("kern.nvgpubar1free") ?? 0
    s.vbios = sysctlString("kern.nvgpuvbios")
    s.state = sysctlString("kern.nvgpustate") ?? ""
    s.live = sysctlInt("kern.nvgpulive") ?? 0
    s.poll = sysctlInt("kern.nvgpupoll") ?? 0
    s.layout = sysctlInt("kern.nvgpulayout") ?? 0
    return s
}

private func watts(_ mw: Int) -> String { String(format: "%.1f W", Double(mw) / 1000.0) }

private func throttleNames() -> [String] { [L("空载", "閒置", "idle"), L("应用限频", "應用限頻", "app clock limit"), L("软件功耗墙", "軟體功耗牆", "SW power cap"), L("硬件降频", "硬體降頻", "HW slowdown"), L("同步加速", "同步加速", "sync boost"),
                             L("软件温度墙", "軟體溫度牆", "SW thermal"), L("硬件温度墙", "硬體溫度牆", "HW thermal"), L("电源刹车", "電源剎車", "power brake"), L("显示时钟", "顯示時鐘", "display clock"), L("板级限制", "板級限制", "board limit"), L("可靠性", "可靠性", "reliability")] }

/// Bit 10 (RELIABILITY) is set permanently by this firmware — idle and under load alike — and the GPU_IDLE
/// bit never appears, so it is a static guard flag rather than a live reason. Only the power/thermal/brake
/// bits mean the card is actually being limited right now.
private let throttleRealBits = (1 << 2) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 9)

private func throttleText(_ mask: Int) -> String {
    if mask < 0 { return "—" }
    if mask == 0 { return L("无（满频）", "無（滿頻）", "none (full clocks)") }
    var parts: [String] = []
    let names = throttleNames()
    for i in 0..<names.count where mask & (1 << i) != 0 { parts.append(names[i]) }
    if mask & throttleRealBits == 0 {
        return L("无（", "無（", "none (") + parts.joined(separator: "·") + L("常态标记）", "常態標記）", " flag)")
    }
    return parts.joined(separator: " · ")
}

private func isThrottling(_ mask: Int) -> Bool {
    mask >= 0 && (mask & throttleRealBits) != 0
}

/// Green below 60 C, amber up to 75 C, red above — the same thresholds NVIDIA tools use for "hot".
private func tempColor(_ t: Int) -> NSColor {
    if t >= 75 { return .systemRed }
    if t >= 60 { return .systemOrange }
    return .systemGreen
}

private func loadColor(_ fraction: Double) -> NSColor {
    if fraction >= 0.85 { return .systemRed }
    if fraction >= 0.6 { return .systemOrange }
    return .systemBlue
}

// ---------------------------------------------------------------- views

private final class BarView: NSView {
    var fraction: Double = 0
    var color: NSColor = .systemBlue

    override func draw(_ dirtyRect: NSRect) {
        let h: CGFloat = 6
        let y = (bounds.height - h) / 2
        NSColor.quaternaryLabelColor.setFill()
        NSBezierPath(roundedRect: NSRect(x: 0, y: y, width: bounds.width, height: h),
                     xRadius: h / 2, yRadius: h / 2).fill()
        let w = max(h, bounds.width * CGFloat(min(1, max(0, fraction))))
        color.setFill()
        NSBezierPath(roundedRect: NSRect(x: 0, y: y, width: w, height: h),
                     xRadius: h / 2, yRadius: h / 2).fill()
    }
}

/// Shared rotor state for one fan. The angular speed *eases* towards the target instead of snapping, so
/// spin-up and spin-down look silky, and the phase survives menu rebuilds.
private final class FanRotor {
    var angle: CGFloat = 0
    var revPerSec: Double = 0
    var targetRevPerSec: Double = 0

    /// Exponential ease with a ~0.35 s time constant: quick enough to feel responsive, smooth enough that
    /// a fan stepping from 1100 to 2200 RPM accelerates visibly instead of jumping.
    func step(dt: Double) {
        let k = 1.0 - exp(-dt / 0.35)
        revPerSec += (targetRevPerSec - revPerSec) * k
        if abs(targetRevPerSec - revPerSec) < 0.002 { revPerSec = targetRevPerSec }
        angle += CGFloat(revPerSec * dt * 2.0 * Double.pi)
        let turn = CGFloat(2.0 * Double.pi)
        if angle > turn { angle -= turn }
    }
}

/// Global animation preference: literal rotor speed, or the smooth proportional one.
private enum FanSettings {
    static var useTrueSpeed = false
}

/// Fan icon: seven black blades traced from the 5080 FE rotor (narrow at the hub, flaring outwards, long
/// tip hugging the rim), a solid black hub, transparent gaps and no strokes or other colours.
private final class SmallFanView: NSView {
    var rotor: FanRotor?
    private let bladeCount = 7

    override var isFlipped: Bool { false }

    private func polar(_ c: NSPoint, _ r: CGFloat, _ a: CGFloat) -> NSPoint {
        NSPoint(x: c.x + r * cos(a), y: c.y + r * sin(a))
    }

    override func draw(_ dirtyRect: NSRect) {
        let c = NSPoint(x: bounds.midX, y: bounds.midY)
        let R = min(bounds.width, bounds.height) / 2 - 0.5
        let hub = R * 0.33
        let base = rotor?.angle ?? 0
        let pitch = CGFloat.pi * 2.0 / CGFloat(bladeCount)

        /* adapt to the menu appearance: near-black on a light menu, near-white on a dark one */
        let dark = effectiveAppearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua
        let ink = NSColor(white: dark ? 0.92 : 0.05, alpha: 1.0)
        ink.setFill()

        /* seven blades, each bounded by two curved (S-shaped) edges, joined to the shroud ring */
        let rIn = hub, rOut = R * 0.965
        let width = pitch * 0.52
        let sweep = pitch * 0.30
        let sCurve = pitch * 0.16
        /* S-curve direction is mirrored: the blades bow the other way round */
        func edge(_ s: CGFloat) -> CGFloat {
            sweep * s - sCurve * CGFloat(sin(Double(s) * Double.pi * 1.1))
        }
        for i in 0..<bladeCount {
            let a0 = base + CGFloat(i) * pitch
            let steps = 22
            let p = NSBezierPath()
            for k in 0...steps {                       // leading edge, hub → rim
                let s = CGFloat(k) / CGFloat(steps)
                let r = rIn + (rOut - rIn) * s
                let pt = polar(c, r, a0 - width / 2 + edge(s))
                if k == 0 { p.move(to: pt) } else { p.line(to: pt) }
            }
            for k in 0...6 {                           // tip, along the rim
                let a = a0 - width / 2 + edge(1) + width * CGFloat(k) / 6
                p.line(to: polar(c, rOut, a))
            }
            for k in stride(from: steps, through: 0, by: -1) {
                let s = CGFloat(k) / CGFloat(steps)
                let r = rIn + (rOut - rIn) * s
                p.line(to: polar(c, r, a0 + width / 2 + edge(s)))
            }
            p.close()
            p.fill()
        }

        /* shroud ring, same black, overlapping the tips */
        let ring = NSBezierPath(ovalIn: NSRect(x: c.x - R, y: c.y - R, width: R * 2, height: R * 2))
        ring.appendOval(in: NSRect(x: c.x - R * 0.93, y: c.y - R * 0.93,
                                   width: R * 1.86, height: R * 1.86))
        ring.windingRule = .evenOdd
        ring.fill()

        NSBezierPath(ovalIn: NSRect(x: c.x - hub, y: c.y - hub,
                                    width: hub * 2, height: hub * 2)).fill()
    }
}

/// Icon that can breathe / blink / show tiny animated bars, driven by the same 30 Hz menu timer.
private final class LiveIconView: NSView {
    enum Effect {
        case none
        case pulse(amp: Double, hz: Double)      // scale breathing (hot / high load)
        case blink(hz: Double)                   // alpha blink (actual throttling)
        case bars(value: Double, color: NSColor) // mini bar chart (utilisation)
    }

    var symbol = "circle"
    var tint: NSColor = .secondaryLabelColor
    var effect: Effect = .none
    private static let epoch = CACurrentMediaTime()
    private var t: Double { CACurrentMediaTime() - LiveIconView.epoch }

    override var isFlipped: Bool { false }

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        let time = t
        switch effect {
        case .bars(let value, let color):
            /* keep a visible floor so the bars breathe even at 10-20 % load */
            let base = CGFloat(min(1, max(0.22, value)))
            let shape: [CGFloat] = [0.55, 1.0, 0.75]
            for (i, h) in shape.enumerated() {
                let shimmer = 0.72 + 0.28 * sin(time * 5.0 + Double(i) * 1.9)
                let frac = max(0.12, min(1.0, base * h * shimmer))
                let bw: CGFloat = 3.5
                let x = bounds.minX + CGFloat(i) * (bw + 1.5)
                let bh = max(2, bounds.height * frac)
                ctx.setFillColor(color.cgColor)
                NSBezierPath(roundedRect: NSRect(x: x, y: 0, width: bw, height: bh),
                             xRadius: 1.2, yRadius: 1.2).fill()
            }
        default:
            var alpha: CGFloat = 1
            var scale: CGFloat = 1
            if case .pulse(let amp, let hz) = effect {
                scale = 1 + CGFloat(amp) * CGFloat(sin(time * hz * 2 * Double.pi))
            }
            if case .blink(let hz) = effect {
                alpha = 0.30 + 0.70 * CGFloat(0.5 + 0.5 * sin(time * hz * 2 * Double.pi))
            }
            let cfg = NSImage.SymbolConfiguration(paletteColors: [tint.withAlphaComponent(alpha)])
            guard let base = NSImage(systemSymbolName: symbol, accessibilityDescription: nil),
                  let img = base.withSymbolConfiguration(cfg) else { return }
            /* SF Symbols have their own aspect ratio (a thermometer is tall and narrow); fit inside the
             * square instead of stretching it. */
            let maxSide = min(bounds.width, bounds.height) * scale
            let ratio = img.size.height > 0 ? img.size.width / img.size.height : 1
            var w = maxSide, h = maxSide
            if ratio > 1 { h = maxSide / ratio } else if ratio > 0 { w = maxSide * ratio }
            img.draw(in: NSRect(x: bounds.midX - w / 2, y: bounds.midY - h / 2, width: w, height: h))
        }
    }
}

// ---------------------------------------------------------------- localization

enum AppLanguage: String {
    case system, zh, zhHant, en

    /// Resolve "follow the system" into a concrete language code.
    static var effective: AppLanguage {
        let raw = UserDefaults.standard.string(forKey: "language") ?? AppLanguage.system.rawValue
        switch AppLanguage(rawValue: raw) ?? .system {
        case .zh: return .zh
        case .zhHant: return .zhHant
        case .en: return .en
        case .system:
            let preferred = Locale.preferredLanguages.first ?? "en"
            guard preferred.hasPrefix("zh") else { return .en }
            let traditional = preferred.contains("Hant") || preferred.contains("TW")
                || preferred.contains("HK") || preferred.contains("MO")
            return traditional ? .zhHant : .zh
        }
    }
}

/// Localised literal: simplified Chinese, traditional Chinese, English.
func L(_ zh: String, _ hant: String, _ en: String) -> String {
    switch AppLanguage.effective {
    case .zh: return zh
    case .zhHant: return hant
    case .en, .system: return en
    }
}

// ---------------------------------------------------------------- app

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private var statusItem: NSStatusItem!
    private var timer: Timer?
    private var last = Snapshot()

    private let menuWidth: CGFloat = 364
    private let rowHeight: CGFloat = 22

    private let showTempKey = "showTemp", showPowerKey = "showPower"
    private let showFanKey = "showFan", showUtilKey = "showUtil"
    private let trueFanSpeedKey = "fanTrueSpeed"

    private var dataMenu: NSMenu!
    private var optionsMenu: NSMenu!
    private var liveViews: [NSView] = []
    private var rotors: [Int: FanRotor] = [:]
    private var lastTick: CFTimeInterval = 0
    private var animTimer: Timer?
    private var menuOpen = false

    func applicationDidFinishLaunching(_ notification: Notification) {
        LoginItem.repairIfNeeded()      /* keep the agent pointing at this copy after an update or a move */
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        statusItem.button?.target = self
        statusItem.button?.action = #selector(handleClick(_:))
        statusItem.button?.sendAction(on: [.leftMouseUp, .rightMouseUp])
        FanSettings.useTrueSpeed = UserDefaults.standard.bool(forKey: trueFanSpeedKey)
        statusItem.button?.font = NSFont.monospacedDigitSystemFont(ofSize: 11, weight: .regular)
        if let img = NSImage(systemSymbolName: "memorychip", accessibilityDescription: "GPU") {
            img.isTemplate = true
            statusItem.button?.image = img
            statusItem.button?.imagePosition = .imageLeading
        }
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { [weak self] _ in self?.refresh() }
        RunLoop.main.add(timer!, forMode: .common)
    }

    private func refresh() {
        last = readSnapshot()
        updateTitle()
        if menuOpen { return }          /* keep the visible menu (and its spinner views) intact */
        dataMenu = buildDataMenu()
        optionsMenu = buildOptionsMenu()
    }

    // ---- menu delegate: animate the fans only while the menu is on screen

    func menuWillOpen(_ menu: NSMenu) {
        menuOpen = true
        animTimer?.invalidate()
        lastTick = CACurrentMediaTime()
        animTimer = Timer.scheduledTimer(withTimeInterval: 1.0 / 60.0, repeats: true) { [weak self] _ in
            guard let self else { return }
            let now = CACurrentMediaTime()
            let dt = min(0.1, now - self.lastTick)
            self.lastTick = now
            for rotor in self.rotors.values { rotor.step(dt: dt) }
            for v in self.liveViews { v.needsDisplay = true }
        }
        RunLoop.main.add(animTimer!, forMode: .common)
    }

    func menuDidClose(_ menu: NSMenu) {
        menuOpen = false
        animTimer?.invalidate()
        animTimer = nil
    }

    /// Left click shows readings only; right click (or ctrl-click) shows the actions.
    @objc private func handleClick(_ sender: NSStatusBarButton) {
        let event = NSApp.currentEvent
        let right = event?.type == .rightMouseUp || (event?.modifierFlags.contains(.control) ?? false)
        if right {
            optionsMenu = buildOptionsMenu()
            optionsMenu.delegate = self
            statusItem.menu = optionsMenu
        } else {
            dataMenu = buildDataMenu()
            dataMenu.delegate = self
            statusItem.menu = dataMenu
        }
        statusItem.button?.performClick(nil)
        statusItem.menu = nil
    }

    // ---- status bar text

    private func on(_ key: String, default def: Bool) -> Bool {
        if UserDefaults.standard.object(forKey: key) == nil { return def }
        return UserDefaults.standard.bool(forKey: key)
    }

    private func updateTitle() {
        guard let button = statusItem.button else { return }
        if !last.loaded {
            button.title = " " + L("未加载", "未載入", "not loaded")
            button.contentTintColor = nil
            return
        }
        var parts: [String] = []
        if on(showTempKey, default: true) { parts.append("\(last.temp)°") }
        if on(showPowerKey, default: true) { parts.append("\(last.powerMW / 1000)W") }
        if on(showFanKey, default: true) {
            let rpm = last.fans.max() ?? last.systemFans.first?.rpm ?? last.fanRPM
            parts.append("\(rpm)rpm")
        }
        if on(showUtilKey, default: false) { parts.append("\(last.util)%") }
        if parts.isEmpty { parts.append("\(last.temp)°") }
        button.title = " " + parts.joined(separator: " ")
        /* Leave the normal colour to AppKit: the status bar picks its own ink from the wallpaper behind it,
         * and forcing white/black here is exactly what made the item disagree with the rest of the bar. */
        if last.temp >= 75 {
            button.contentTintColor = .systemRed
        } else if last.temp >= 60 {
            button.contentTintColor = .systemOrange
        } else {
            button.contentTintColor = nil
        }
    }

    // ---- building blocks

    /// Card name plus VRAM on the first line, PCIe link (and a merge of the throttle reason) on the second.
    private func gpuHeaderRow() -> NSMenuItem {
        let width = menuWidth, height: CGFloat = 40
        let v = NSView(frame: NSRect(x: 0, y: 0, width: width, height: height))

        let gpu = GPUIdentity.current()
        let gpuName = gpu?.name ?? "NVIDIA GPU"
        /* VRAM from the kext when it has it, otherwise the card's own VRAM,totalMB property. */
        let vramMB = last.vramTotalMB > 0 ? last.vramTotalMB : (gpu?.vramMB ?? 0)
        let nameText = vramMB > 0
            ? "\(gpuName) \(Int((Double(vramMB) / 1024.0).rounded()))GB"
            : gpuName
        let name = NSTextField(labelWithString: nameText)
        name.font = NSFont.systemFont(ofSize: 13, weight: .semibold)
        name.textColor = .labelColor
        name.frame = NSRect(x: 16, y: 20, width: width - 32, height: 17)
        name.lineBreakMode = .byTruncatingTail

        let detail = pcieText()
        let slot = NSTextField(labelWithString: detail)
        slot.font = NSFont.systemFont(ofSize: 11)
        slot.textColor = .secondaryLabelColor
        slot.frame = NSRect(x: 16, y: 4, width: width - 32, height: 15)
        slot.lineBreakMode = .byTruncatingTail

        v.addSubview(name)
        v.addSubview(slot)
        let item = NSMenuItem()
        item.view = v
        return item
    }

    /// Same three fields System Information lists for a GPU: bus, link speed in GT/s and lane width.
    private func pcieText() -> String {
        var parts: [String] = []
        if last.pcieGen > 0 {
            let bus: String
            switch GPUIdentity.current()?.bus {
            case .thunderbolt: bus = L("雷雳", "雷靂", "Thunderbolt")
            default:           bus = "PCIe"
            }
            parts.append(L("总线 ", "匯流排 ", "Bus ") + bus)
            parts.append(L("链接速度 ", "連結速度 ", "Link speed ") + gtsText(for: last.pcieGen))
        }
        if last.pcieWidth > 0 {
            parts.append(L("PCIe Lane 宽度 x", "PCIe Lane 寬度 x", "PCIe lane width x") + "\(last.pcieWidth)")
        }
        return parts.joined(separator: " · ")
    }

    /// GT/s per lane for a PCIe generation, the way System Information writes it (8.0 GT/s, 32.0 GT/s, …).
    private func gtsText(for generation: Int) -> String {
        let table: [Int: Double] = [1: 2.5, 2: 5.0, 3: 8.0, 4: 16.0, 5: 32.0, 6: 64.0]
        guard let rate = table[generation] else { return "?\(generation) GT/s" }
        return rate == rate.rounded() ? String(format: "%.1f GT/s", rate) : String(format: "%.1f GT/s", rate)
    }

    private func sectionHeader(_ text: String) -> NSMenuItem {
        let width = menuWidth, height: CGFloat = 18
        let v = NSView(frame: NSRect(x: 0, y: 0, width: width, height: height))
        let l = NSTextField(labelWithString: text.uppercased())
        l.font = NSFont.systemFont(ofSize: 10, weight: .semibold)
        l.textColor = .tertiaryLabelColor
        l.frame = NSRect(x: 14, y: 1, width: width - 28, height: height - 2)
        v.addSubview(l)
        let item = NSMenuItem()
        item.view = v
        item.isEnabled = false
        return item
    }

    private func row(_ label: String, _ value: String,
                     color: NSColor = .labelColor,
                     symbol: String? = nil,
                     iconTint: NSColor = .secondaryLabelColor,
                     effect: LiveIconView.Effect = .none,
                     bar: (fraction: Double, color: NSColor)? = nil) -> NSMenuItem {
        let width = menuWidth, height = rowHeight
        let v = NSView(frame: NSRect(x: 0, y: 0, width: width, height: height))

        var labelX: CGFloat = 14
        if let name = symbol {
            let iv = LiveIconView(frame: NSRect(x: 12, y: (height - 15) / 2, width: 16, height: 15))
            iv.symbol = name
            iv.tint = iconTint
            iv.effect = effect
            v.addSubview(iv)
            liveViews.append(iv)
            labelX = 34
        }

        let l = NSTextField(labelWithString: label)
        l.font = NSFont.systemFont(ofSize: 12)
        l.textColor = .secondaryLabelColor
        l.lineBreakMode = .byTruncatingTail
        l.frame = NSRect(x: labelX, y: 3, width: 150, height: height - 6)
        v.addSubview(l)

        let barWidth: CGFloat = bar == nil ? 0 : 44
        let valueRight: CGFloat = width - (bar == nil ? 14 : 14 + barWidth + 10)
        let r = NSTextField(labelWithString: value)
        r.font = NSFont.monospacedDigitSystemFont(ofSize: 12, weight: .medium)
        r.textColor = color
        r.alignment = .right
        r.frame = NSRect(x: labelX + 140, y: 3, width: max(60, valueRight - labelX - 140), height: height - 6)
        v.addSubview(r)

        if let b = bar {
            let bv = BarView(frame: NSRect(x: width - 14 - barWidth, y: 0, width: barWidth, height: height))
            bv.fraction = b.fraction
            bv.color = b.color
            v.addSubview(bv)
        }

        let item = NSMenuItem()
        item.view = v
        item.isEnabled = false
        return item
    }

    /// Compact row: one small spinning fan per row with the live reading. The row is only 34 pt tall, so a
    /// single fan and a pair both look balanced.
    private func fanRow(index: Int, rpm: Int, label: String? = nil, showPercent: Bool = true) -> NSMenuItem {
        let width = menuWidth
        let height: CGFloat = 28
        let v = NSView(frame: NSRect(x: 0, y: 0, width: width, height: height))

        let rotor = rotors[index] ?? FanRotor()
        rotors[index] = rotor
        let real = Double(rpm) / 60.0
        rotor.targetRevPerSec = FanSettings.useTrueSpeed ? real : min(2.5, real / 10.0)

        let fan = SmallFanView(frame: NSRect(x: 12, y: (height - 16) / 2, width: 16, height: 16))
        fan.rotor = rotor
        v.addSubview(fan)
        liveViews.append(fan)

        let l = NSTextField(labelWithString: label ?? (L("风扇", "風扇", "Fan") + " \(index + 1)"))
        l.font = NSFont.systemFont(ofSize: 12)
        l.textColor = .secondaryLabelColor
        l.frame = NSRect(x: 36, y: 6, width: 90, height: 16)
        v.addSubview(l)

        let pct = last.fanMax > 0 ? Int((Double(rpm) / Double(last.fanMax) * 100).rounded()) : 0
        let rpmText = rpm > 0 ? "\(rpm) " + L("转/分", "轉/分", "RPM") : L("停转", "停轉", "Stopped")
        let r = NSTextField(labelWithString: rpm > 0 && showPercent ? rpmText + " · \(pct)%" : rpmText)
        r.font = NSFont.monospacedDigitSystemFont(ofSize: 12, weight: .medium)
        r.textColor = rpm > 0 ? .labelColor : .tertiaryLabelColor
        r.alignment = .right
        r.frame = NSRect(x: 130, y: 6, width: width - 144, height: 16)
        v.addSubview(r)

        let item = NSMenuItem()
        item.view = v
        item.isEnabled = false
        return item
    }

    private func action(_ title: String, _ sel: Selector, symbol: String? = nil) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: sel, keyEquivalent: "")
        item.target = self
        if let s = symbol, let img = NSImage(systemSymbolName: s, accessibilityDescription: nil) {
            img.isTemplate = true
            item.image = img
        }
        return item
    }

    private func toggleItem(_ title: String, key: String, default def: Bool) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: #selector(toggleTitleOption(_:)), keyEquivalent: "")
        item.target = self
        item.representedObject = key
        item.state = on(key, default: def) ? .on : .off
        return item
    }

    // ---- menu

    private func buildDataMenu() -> NSMenu {
        let menu = NSMenu()
        menu.autoenablesItems = false
        liveViews.removeAll()

        if !last.loaded {
            menu.addItem(sectionHeader(L("状态", "狀態", "Status")))
            menu.addItem(row("SMCNvidiaGPU.kext", L("未加载", "未載入", "not loaded"), color: .systemRed, symbol: "exclamationmark.triangle"))
            menu.addItem(.separator())
            menu.addItem(row(L("右键", "右鍵", "Right click"), L("更多选项", "更多選項", "More options"), color: .tertiaryLabelColor, symbol: "cursorarrow.click.2"))
            return menu
        }

        menu.addItem(gpuHeaderRow())
        menu.addItem(.separator())
        menu.addItem(sectionHeader(L("功耗与温度", "功耗與溫度", "Power & temperature")))
        menu.addItem(row(L("温度", "溫度", "Temperature"), "\(last.temp) °C", color: tempColor(last.temp), symbol: "thermometer.medium",
                         iconTint: tempColor(last.temp),
                         effect: last.temp >= 75 ? .pulse(amp: 0.18, hz: 1.2) : .none))
        let powerFrac = last.limitMW > 0 ? Double(last.powerMW) / Double(last.limitMW) : 0
        menu.addItem(row(L("功耗（平均）", "功耗（平均）", "Average power"), watts(last.powerMW), symbol: "bolt.fill",
                         iconTint: powerFrac > 0.85 ? .systemRed : (powerFrac > 0.6 ? .systemOrange : .systemYellow),
                         effect: powerFrac > 0.5 ? .pulse(amp: 0.10, hz: 2.2) : .none,
                         bar: last.limitMW > 0 ? (powerFrac, loadColor(powerFrac)) : nil))
        if last.instMW > 0 { menu.addItem(row(L("功耗（瞬时）", "功耗（瞬時）", "Instant power"), watts(last.instMW), symbol: "bolt")) }
        if last.tgpMW > 0 { menu.addItem(row("TGP", watts(last.tgpMW), symbol: "gauge.with.dots.needle.33percent")) }
        if last.limitMW > 0 { menu.addItem(row(L("功率上限", "功率上限", "Power limit"), watts(last.limitMW), symbol: "speedometer")) }

        menu.addItem(.separator())
        if last.fans.isEmpty && !last.systemFans.isEmpty {
            menu.addItem(sectionHeader(L("风扇（系统）", "風扇（系統）", "Fans (system)")))
            for (i, fan) in last.systemFans.enumerated() {
                menu.addItem(fanRow(index: i, rpm: fan.rpm, label: fan.name, showPercent: false))
            }
        } else {
            menu.addItem(sectionHeader(L("风扇", "風扇", "Fan")))
            if last.fans.isEmpty {
                menu.addItem(row(L("风扇", "風扇", "Fan"), L("停转（Zero RPM）", "停轉（Zero RPM）", "Stopped (Zero RPM)"), color: .secondaryLabelColor, symbol: "fanblades.slash"))
            }
            for (i, rpm) in last.fans.enumerated() {
                menu.addItem(fanRow(index: i, rpm: rpm))
            }
        }

        menu.addItem(.separator())
        menu.addItem(sectionHeader(L("负载", "負載", "Load")))
        menu.addItem(row(L("GPU 利用率", "GPU 使用率", "GPU utilization"), "\(last.util) %", symbol: "chart.bar.fill",
                         effect: .bars(value: Double(last.util) / 100.0, color: loadColor(Double(last.util) / 100.0)),
                         bar: (Double(last.util) / 100.0, loadColor(Double(last.util) / 100.0))))
        menu.addItem(row(L("引擎", "引擎", "Engines"), L("FB\(last.fb)%  编\(last.enc)%  解\(last.dec)%", "FB\(last.fb)%  編\(last.enc)%  解\(last.dec)%", "FB \(last.fb)%  enc \(last.enc)%  dec \(last.dec)%"), symbol: "square.stack.3d.up"))

        menu.addItem(.separator())
        menu.addItem(sectionHeader(L("频率与显存", "頻率與顯存", "Clocks & VRAM")))
        menu.addItem(row(L("核心频率", "核心頻率", "Core clock"), "\(last.gpcMHz) MHz", symbol: "cpu"))
        menu.addItem(row(L("显存频率", "顯存頻率", "Memory clock"), "\(last.dramMHz) MHz", symbol: "memorychip"))
        if last.nvdMHz > 0 { menu.addItem(row(L("NVD 频率", "NVD 頻率", "NVD clock"), "\(last.nvdMHz) MHz", symbol: "waveform.path")) }
        if last.vramTotalMB > 0 {
            let f = Double(last.vramUsedMB) / Double(last.vramTotalMB)
            menu.addItem(row(L("显存占用", "顯存佔用", "VRAM used"), "\(last.vramUsedMB) / \(last.vramTotalMB) MB", symbol: "internaldrive",
                             bar: (f, loadColor(f))))
        }
        menu.addItem(row("P-State", "P\(last.pstate)", symbol: "leaf"))

        menu.addItem(.separator())
        if last.throttle >= 0 {
            let throttling = isThrottling(last.throttle)
            menu.addItem(sectionHeader(L("限制", "限制", "Limits")))
            menu.addItem(row(L("降频原因", "降頻原因", "Throttle reason"), throttleText(last.throttle),
                             color: throttling ? .systemOrange : .labelColor,
                             symbol: throttling ? "exclamationmark.triangle" : "checkmark.circle",
                             iconTint: throttling ? .systemOrange : .secondaryLabelColor,
                             effect: throttling ? .blink(hz: 1.0) : .none))
        }

        if last.vbios != nil || last.eccCorr > 0 || last.eccUncorr > 0 || last.bar1Total > 0 {
            menu.addItem(.separator())
            menu.addItem(sectionHeader(L("其它", "其它", "Other")))
            if let v = last.vbios { menu.addItem(row("VBIOS", v, symbol: "doc.text")) }
            if last.eccCorr > 0 || last.eccUncorr > 0 {
                menu.addItem(row("ECC", L("校正 \(last.eccCorr) · 未校正 \(last.eccUncorr)", "校正 \(last.eccCorr) · 未校正 \(last.eccUncorr)", "corrected \(last.eccCorr) · uncorrected \(last.eccUncorr)"), symbol: "checkmark.shield"))
            }
            if last.bar1Total > 0 {
                menu.addItem(row(L("BAR1 窗口", "BAR1 窗口", "BAR1 window"), "\(last.bar1Free) / \(last.bar1Total) MB", symbol: "rectangle.split.3x1"))
            }
        }

        return menu
    }

    /// Right-click menu: everything that changes state or leaves the app.
    private func buildOptionsMenu() -> NSMenu {
        let menu = NSMenu()
        menu.autoenablesItems = false

        if last.loaded {
            menu.addItem(sectionHeader(L("操作", "操作", "Actions")))
            menu.addItem(action(L("复制全部", "複製全部", "Copy all"), #selector(copyAll), symbol: "doc.on.doc"))
            menu.addItem(action(L("立即刷新", "立即重新整理", "Refresh now"), #selector(refreshNow), symbol: "arrow.clockwise"))
        }

        menu.addItem(.separator())
        menu.addItem(sectionHeader(L("设置", "設定", "Settings")))
        let enabledNow = LoginItem.isEnabled
        let loginItem = NSMenuItem(title: enabledNow ? L("开机自启动", "開機自動啟動", "Start at login") : L("开机自启动（已关闭）", "開機自動啟動（已關閉）", "Start at login (off)"),
                                   action: #selector(toggleLoginItem(_:)), keyEquivalent: "")
        loginItem.target = self
        loginItem.image = NSImage(systemSymbolName: "power.circle", accessibilityDescription: nil)
        loginItem.state = enabledNow ? .on : .off
        menu.addItem(loginItem)

        let titleMenu = NSMenu()
        titleMenu.addItem(toggleItem(L("温度", "溫度", "Temperature"), key: showTempKey, default: true))
        titleMenu.addItem(toggleItem(L("功耗", "功耗", "Power"), key: showPowerKey, default: true))
        titleMenu.addItem(toggleItem(L("风扇", "風扇", "Fan"), key: showFanKey, default: true))
        titleMenu.addItem(toggleItem(L("利用率", "使用率", "Utilization"), key: showUtilKey, default: false))
        let langMenu = NSMenu()
        let stored = UserDefaults.standard.string(forKey: "language") ?? AppLanguage.system.rawValue
        for (title, value) in [("跟随系统 / 跟隨系統 / System", AppLanguage.system.rawValue),
                               ("简体中文", AppLanguage.zh.rawValue),
                               ("繁體中文", AppLanguage.zhHant.rawValue),
                               ("English", AppLanguage.en.rawValue)] {
            let item = NSMenuItem(title: title, action: #selector(setLanguage(_:)), keyEquivalent: "")
            item.target = self
            item.representedObject = value
            item.state = stored == value ? .on : .off
            langMenu.addItem(item)
        }
        let langItem = NSMenuItem(title: L("语言", "語言", "Language"), action: nil, keyEquivalent: "")
        langItem.image = NSImage(systemSymbolName: "globe", accessibilityDescription: nil)
        langItem.submenu = langMenu
        menu.addItem(langItem)

        let titleItem = NSMenuItem(title: L("菜单栏显示", "選單列顯示", "Menu bar"), action: nil, keyEquivalent: "")
        titleItem.image = NSImage(systemSymbolName: "menubar.rectangle", accessibilityDescription: nil)
        titleItem.submenu = titleMenu
        menu.addItem(titleItem)

        if last.loaded {
            menu.addItem(.separator())
            menu.addItem(sectionHeader(L("状态", "狀態", "Status")))
            menu.addItem(row(L("驱动数据", "驅動數據", "Driver data"), "live \(last.live) · poll \(last.poll) · layout \(last.layout)",
                             color: .tertiaryLabelColor, symbol: "waveform.path.ecg",
                             effect: last.live == 1 ? .pulse(amp: 0.20, hz: 0.7) : .none))
        }

        menu.addItem(.separator())
        menu.addItem(action(L("退出", "結束", "Quit"), #selector(quit), symbol: "power"))
        return menu
    }

    // ---- actions

    // ---- autostart (the LaunchAgent installed by app/install.sh; enabled/disabled per user domain)

    @objc private func toggleFanSpeedMode(_ sender: NSMenuItem) {
        FanSettings.useTrueSpeed.toggle()
        UserDefaults.standard.set(FanSettings.useTrueSpeed, forKey: trueFanSpeedKey)
        sender.state = FanSettings.useTrueSpeed ? .on : .off
    }

    @objc private func toggleLoginItem(_ sender: NSMenuItem) {
        if LoginItem.isEnabled {
            LoginItem.disable()
        } else if !LoginItem.enable() {
            /* Creating the agent can fail when LaunchAgents is not writable; say so instead of lying. */
            let alert = NSAlert()
            alert.messageText = L("无法设置开机自启动", "無法設定開機自動啟動", "Could not enable start at login")
            alert.informativeText = plistURLMessage()
            alert.runModal()
        }
        let enabled = LoginItem.isEnabled
        sender.state = enabled ? .on : .off
        sender.title = enabled ? L("开机自启动", "開機自動啟動", "Start at login")
                              : L("开机自启动（已关闭）", "開機自動啟動（已關閉）", "Start at login (off)")
    }

    private func plistURLMessage() -> String {
        L("请确认 \(LoginItem.plistURL.path) 可写。", "請確認 \(LoginItem.plistURL.path) 可寫。",
          "Please check that \(LoginItem.plistURL.path) is writable.")
    }

    @objc private func quit() { NSApplication.shared.terminate(nil) }
    @objc private func refreshNow() { refresh() }

    @objc private func setLanguage(_ sender: NSMenuItem) {
        guard let raw = sender.representedObject as? String else { return }
        UserDefaults.standard.set(raw, forKey: "language")
        dataMenu = buildDataMenu()
        optionsMenu = buildOptionsMenu()
        updateTitle()
    }

    @objc private func toggleTitleOption(_ sender: NSMenuItem) {
        guard let key = sender.representedObject as? String else { return }
        let now = !UserDefaults.standard.bool(forKey: key)
        UserDefaults.standard.set(now, forKey: key)
        sender.state = now ? .on : .off
        updateTitle()
    }

    @objc private func copyAll() {
        var lines: [String] = ["SMCNvidiaGPU"]
        lines.append(L("温度: \(last.temp) °C", "溫度: \(last.temp) °C", "Temperature: \(last.temp) C"))
        lines.append(L("功耗(平均): \(watts(last.powerMW))", "功耗(平均): \(watts(last.powerMW))", "Average power: \(watts(last.powerMW))"))
        if last.instMW > 0 { lines.append(L("功耗(瞬时): \(watts(last.instMW))", "功耗(瞬時): \(watts(last.instMW))", "Instant power: \(watts(last.instMW))")) }
        if last.tgpMW > 0 { lines.append("TGP: \(watts(last.tgpMW))") }
        if last.limitMW > 0 { lines.append(L("功率上限: \(watts(last.limitMW))", "功率上限: \(watts(last.limitMW))", "Power limit: \(watts(last.limitMW))")) }
        for (i, rpm) in last.fans.enumerated() { lines.append(L("风扇 ", "風扇 ", "Fan ") + "\(i + 1): \(rpm) RPM") }
        for fan in last.systemFans { lines.append("\(fan.name): \(fan.rpm) RPM") }
        lines.append(L("GPU 利用率: \(last.util) %", "GPU 使用率: \(last.util) %", "GPU utilization: \(last.util) %"))
        lines.append(L("核心频率: \(last.gpcMHz) MHz", "核心頻率: \(last.gpcMHz) MHz", "Core clock: \(last.gpcMHz) MHz"))
        lines.append(L("显存频率: \(last.dramMHz) MHz", "顯存頻率: \(last.dramMHz) MHz", "Memory clock: \(last.dramMHz) MHz"))
        if last.vramTotalMB > 0 { lines.append(L("显存: \(last.vramUsedMB)/\(last.vramTotalMB) MB", "顯存: \(last.vramUsedMB)/\(last.vramTotalMB) MB", "VRAM: \(last.vramUsedMB)/\(last.vramTotalMB) MB")) }
        lines.append("P-State: P\(last.pstate)")
        if last.pcieGen > 0 || last.pcieWidth > 0 {
            lines.append("PCIe: " + (last.pcieGen > 0 ? "Gen\(last.pcieGen)" : "")
                         + (last.pcieWidth > 0 ? " x\(last.pcieWidth)" : ""))
        }
        lines.append(L("降频原因: ", "降頻原因: ", "Throttle reason: ") + throttleText(last.throttle))
        if let v = last.vbios { lines.append("VBIOS: \(v)") }
        lines.append(L("状态: ", "狀態: ", "Status: ") + last.state)

        let pb = NSPasteboard.general
        pb.clearContents()
        pb.setString(lines.joined(separator: "\n"), forType: .string)
    }
}

func runApp() {
    let app = NSApplication.shared
    let delegate = AppDelegate()
    app.delegate = delegate
    app.setActivationPolicy(.accessory)
    app.run()
}
