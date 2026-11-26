import Foundation

/// EVGA iCX2 sensors (RTX 20 series FTW3/XC cards), read over the GPU's I2C bus through the NullMoth NVRM
/// user client. See tools/icx2/icx2.h for the register map. Polls on its own serial queue so a slow or stuck
/// I2C transaction never blocks the menu; the menu only reads the last finished sample.
final class EVGAICX {
    struct Sample {
        var temps: [(name: String, celsius: Int)] = []
        var rails: [(name: String, mA: Int, mV: Int)] = []
        var fans: [(target: Int, rpm: Int)] = []   /* card Fan 2 and Fan 3 (Fan 1 is the NVIDIA driver's): target %, RPM */
        var port = -1
        var watts: Double { rails.reduce(0) { $0 + Double($1.mA) * Double($1.mV) / 1e6 } }
    }

    static let shared = EVGAICX()

    /// The C name tables arrive in Swift as fixed-size tuples of C strings.
    private static func names<T>(_ table: T) -> [String] {
        Mirror(reflecting: table).children.compactMap { ($0.value as? UnsafePointer<CChar>).map { String(cString: $0) } }
    }
    private static let tempNames = names(icx2_temp_names)
    private static let railNames = names(icx2_rail_names)

    private let queue = DispatchQueue(label: "org.nullmoth.SMCNvidiaGPU.icx2")
    private let lock = NSLock()
    private var handle: OpaquePointer?
    private var latest: Sample?
    private var lastError = ""
    private var nextOpenAttempt = Date.distantPast
    private var busy = false

    /// The last good sample, or nil when there is no iCX2 card (or reading is off).
    var sample: Sample? { lock.lock(); defer { lock.unlock() }; return latest }
    var error: String { lock.lock(); defer { lock.unlock() }; return lastError }

    /// Kick off one background read. `power` adds the three power inputs, which needs Precision X1's latch
    /// writes (registers 0x0E/0x1E of the iCX2 only).
    func poll(power: Bool) {
        lock.lock()
        if busy { lock.unlock(); return }
        busy = true
        lock.unlock()
        queue.async { [self] in
            defer { lock.lock(); busy = false; lock.unlock() }
            if handle == nil {
                guard Date() >= nextOpenAttempt else { return }
                var err = [CChar](repeating: 0, count: 256)
                handle = icx2_open(&err, Int32(err.count))
                if handle == nil {
                    /* Not an iCX2 card, or the driver is not up yet: try again later, not every two seconds. */
                    nextOpenAttempt = Date().addingTimeInterval(60)
                    let msg = String(cString: err)
                    lock.lock(); latest = nil; lastError = msg; lock.unlock()
                    return
                }
            }
            var r = icx2_reading_t()
            guard icx2_read_temps(handle, &r) else {
                /* The bus or the RM client went away (sleep, driver reload): drop it and reopen next time. */
                icx2_close(handle); handle = nil
                lock.lock(); latest = nil; lastError = "iCX2 read failed"; lock.unlock()
                return
            }
            _ = icx2_read_fans(handle, &r)
            if power { _ = icx2_read_power(handle, &r) }
            var s = Sample()
            s.port = Int(icx2_port(handle))
            withUnsafeBytes(of: r.temp_c) { raw in
                let t = raw.bindMemory(to: Int32.self)
                for i in 0..<Int(ICX2_NUM_TEMPS) {
                    s.temps.append((i < Self.tempNames.count ? Self.tempNames[i] : "T\(i + 1)", Int(t[i])))
                }
            }
            if r.fan_ok {
                s.fans = [(Int(r.fan_target.0), Int(r.fan_rpm.0)), (Int(r.fan_target.1), Int(r.fan_rpm.1))]
            } else {
                /* One failed fan read must not make Fans 2 and 3 vanish: keep the last good values. */
                lock.lock(); s.fans = latest?.fans ?? []; lock.unlock()
            }
            if power && r.power_ok {
                withUnsafeBytes(of: r.rail_ma) { ma in
                    withUnsafeBytes(of: r.rail_mv) { mv in
                        let a = ma.bindMemory(to: UInt16.self), v = mv.bindMemory(to: UInt16.self)
                        for i in 0..<Int(ICX2_NUM_RAILS) {
                            s.rails.append((i < Self.railNames.count ? Self.railNames[i] : "Input \(i + 1)", Int(a[i]), Int(v[i])))
                        }
                    }
                }
            }
            lock.lock(); latest = s; lastError = ""; lock.unlock()
        }
    }

    /// Forget the current sample and close the client (reading turned off).
    func stop() {
        queue.async { [self] in
            if handle != nil { icx2_close(handle); handle = nil }
            nextOpenAttempt = .distantPast
            lock.lock(); latest = nil; lock.unlock()
        }
    }
}
