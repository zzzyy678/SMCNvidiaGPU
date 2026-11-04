// Machine.swift — is this a laptop?
//
// The system-fan fallback only makes sense on a laptop, where the EC owns the cooler. A desktop card can
// report zero fans for a tick (both fans stopped in Zero RPM mode), and falling back there replaced the GPU
// fans with the case fans. Check for a battery instead of guessing from the fan count.

import Foundation
import IOKit

enum Machine {
    static var hasBattery: Bool {
        let service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSmartBattery"))
        guard service != 0 else { return false }
        IOObjectRelease(service)
        return true
    }
}
