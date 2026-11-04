// GPUInfo.swift — which card this is and where it sits, straight from the IORegistry.
//
// The display function publishes the model name; the ACPI ancestor of that node (GPP0, PEG0, …) is what a
// Hackintosh user calls the slot, and pcidebug gives the bus:device:function address.

import Foundation
import IOKit

/// Desktop or laptop card, judged from the model name the driver reports.
enum GPUForm {
    case desktop, laptop, unknown

    /// GTX 16 / RTX 20 / 30 / 40 / 50 desktop parts. Laptop parts carry "Laptop" (or Max-Q / Max-P) in their
    /// name, so anything matching a desktop number without that word is a desktop card.
    private static let desktopModels = [
        "gtx 1650", "gtx 1660",
        "rtx 2060", "rtx 2070", "rtx 2080",
        "rtx 3050", "rtx 3060", "rtx 3070", "rtx 3080", "rtx 3090",
        "rtx 4060", "rtx 4070", "rtx 4080", "rtx 4090",
        "rtx 5050", "rtx 5060", "rtx 5070", "rtx 5080", "rtx 5090",
    ]

    static func classify(model: String?) -> GPUForm {
        guard let model, !model.isEmpty else { return .unknown }
        let name = model.lowercased()
        if name.contains("laptop") || name.contains("max-q") || name.contains("max-p") || name.contains("mobile") {
            return .laptop
        }
        if desktopModels.contains(where: { name.contains($0) }) {
            return .desktop
        }
        return .unknown
    }
}

enum GPUBus {
    case pcie
    case thunderbolt
}

struct GPUInfo {
    let name: String
    let slot: String        // ACPI device name, e.g. GPP0
    let address: String     // 01:00.0
    let bus: GPUBus
    let vramMB: Int         // 0 when the node does not publish it

    var slotLine: String {
        address.isEmpty ? slot : "\(slot) · \(address)"
    }
}

enum GPUIdentity {
    private static var cached: GPUInfo?

    private static func u32(_ value: AnyObject?) -> UInt32? {
        if let data = value as? Data, data.count >= 4 {
            return data.withUnsafeBytes { $0.load(as: UInt32.self) }
        }
        if let n = value as? NSNumber { return n.uint32Value }
        return nil
    }

    private static func text(_ value: AnyObject?) -> String? {
        if let data = value as? Data {
            let s = String(decoding: data.prefix { $0 != 0 }, as: UTF8.self).trimmingCharacters(in: .whitespaces)
            return s.isEmpty ? nil : s
        }
        if let s = value as? String { return s }
        return nil
    }

    private static func property(_ entry: io_registry_entry_t, _ key: String) -> AnyObject? {
        IORegistryEntryCreateCFProperty(entry, key as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue()
    }

    /// ACPI name of the slot: the last ancestor before the PCI bridge chain (GPP0, PEG0, SLOT1, …).
    private static func acpiSlot(of entry: io_registry_entry_t) -> String? {
        var path = [CChar](repeating: 0, count: 1024)
        guard IORegistryEntryGetPath(entry, kIOServicePlane, &path) == KERN_SUCCESS else { return nil }
        let components = String(cString: path).split(separator: "/")
        // IOService:/AppleACPIPlatformExpert/PCI0@0/AppleACPIPCI/GPP0@1,1/IOPP/VGA@0
        guard components.count >= 2 else { return nil }
        for component in components.reversed().dropFirst() {
            let name = component.split(separator: "@").first.map(String.init) ?? String(component)
            if ["IOPP", "AppleACPIPCI", "PCI0", "AppleACPIPlatformExpert", "IOService:", "IOPCIDevice"].contains(name) {
                continue
            }
            return name
        }
        return nil
    }

    /// An eGPU sits behind a Thunderbolt controller; the ancestors tell us which bus the card is really on.
    private static func bus(of entry: io_registry_entry_t) -> GPUBus {
        var current = entry
        var owned = false
        defer { if owned { IOObjectRelease(current) } }

        for _ in 0..<12 {
            var parent: io_registry_entry_t = 0
            guard IORegistryEntryGetParentEntry(current, kIOServicePlane, &parent) == KERN_SUCCESS, parent != 0
            else { break }
            if owned { IOObjectRelease(current) }
            current = parent
            owned = true

            var classBuffer = [CChar](repeating: 0, count: 128)
            IOObjectGetClass(parent, &classBuffer)
            let className = String(cString: classBuffer)
            let acpiPath = text(IORegistryEntryCreateCFProperty(parent, "acpi-path" as CFString,
                                                               kCFAllocatorDefault, 0)?.takeRetainedValue()) ?? ""
            if className.localizedCaseInsensitiveContains("thunderbolt")
                || className.localizedCaseInsensitiveContains("TBT")
                || acpiPath.contains("/UPSB") || acpiPath.contains("/DSB")
                || acpiPath.contains("/NHI") || acpiPath.contains("/TBT") {
                return .thunderbolt
            }
            if className == "Root" { break }
        }
        return .pcie
    }

    private static func address(of entry: io_registry_entry_t) -> String? {
        guard let raw = text(property(entry, "pcidebug")) else { return nil }
        let parts = raw.split(separator: ":").compactMap { Int($0.prefix { $0 != "(" }) }
        guard parts.count >= 3 else { return nil }
        return String(format: "%02x:%02x.%x", parts[0], parts[1], parts[2])
    }

    /// The model string the driver reports, published by the kext before it shortens the IORegistry property.
    private static func driverModelName() -> String? {
        var size = 0
        guard sysctlbyname("kern.nvgpumodel", nil, &size, nil, 0) == 0, size > 1 else { return nil }
        var buffer = [CChar](repeating: 0, count: size + 1)
        guard sysctlbyname("kern.nvgpumodel", &buffer, &size, nil, 0) == 0 else { return nil }
        let text = String(cString: buffer).trimmingCharacters(in: .whitespaces)
        return text.isEmpty ? nil : text
    }

    static func current() -> GPUInfo? {
        if let cached { return cached }

        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOPCIDevice"), &iterator) == KERN_SUCCESS
        else { return nil }
        var service = IOIteratorNext(iterator)
        defer { IOObjectRelease(iterator) }

        while service != 0 {
            defer { IOObjectRelease(service); service = IOIteratorNext(iterator) }

            guard u32(property(service, "vendor-id")) == 0x10de else { continue }
            let classCode = (u32(property(service, "class-code")) ?? 0) >> 16
            guard classCode == 0x03 else { continue }      // display controller

            let name = driverModelName() ?? text(property(service, "model")) ?? "NVIDIA GPU"
            let info = GPUInfo(name: name,
                               slot: acpiSlot(of: service) ?? "PCIe",
                               address: address(of: service) ?? "",
                               bus: bus(of: service),
                               vramMB: Int(u32(property(service, "VRAM,totalMB")) ?? 0))
            cached = info
            return info
        }
        return nil
    }
}
