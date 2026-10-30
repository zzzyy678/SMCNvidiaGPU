// SMCFans.swift — read the system fans straight from AppleSMC.
//
// Laptop GPUs usually expose no fan of their own (the EC owns the cooler), so when the kext reports no GPU
// fan the menu falls back to these instead.

import Foundation
import IOKit

struct SMCFan {
    let index: Int
    let name: String
    let rpm: Int
}

final class SMCReader {
    static let shared = SMCReader()

    private var conn: io_connect_t = 0
    private let kernelIndex: UInt32 = 2      // KERNEL_INDEX_SMC

    // Layout of SMCKeyData_t as the driver sees it (80 bytes):
    //   0  key[4]   4  vers   12 pLimitData   28 keyInfo{dataSize, dataType, attr}
    //   40 result   41 status 42 data8        44 data32    48 bytes[32]
    private let offDataSize = 28
    private let offData8 = 42
    private let offBytes = 48

    private func open() -> Bool {
        if conn != 0 { return true }
        let service = IOServiceGetMatchingService(kIOMyMainPort, IOServiceMatching("AppleSMC"))
        guard service != 0 else { return false }
        defer { IOObjectRelease(service) }
        guard IOServiceOpen(service, mach_task_self_, 0, &conn) == KERN_SUCCESS else {
            conn = 0
            return false
        }
        return true
    }

    /// One struct call; `cmd` goes into data8, the key into the first four bytes.
    private func call(_ cmd: UInt8, key: String, dataSize: UInt32 = 0) -> [UInt8]? {
        guard open(), key.utf8.count == 4 else { return nil }
        var input = [UInt8](repeating: 0, count: 80)
        var output = [UInt8](repeating: 0, count: 80)

        // The driver reads `key` as a uint32 built from the four characters; storing that value natively
        // (rather than copying the bytes) is what makes the lookup work.
        let keyValue = key.utf8.reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
        input.withUnsafeMutableBytes { raw in
            raw.storeBytes(of: keyValue, toByteOffset: 0, as: UInt32.self)
        }
        input[offData8] = cmd
        if dataSize != 0 {
            input.withUnsafeMutableBytes { raw in
                raw.storeBytes(of: dataSize, toByteOffset: offDataSize, as: UInt32.self)
            }
        }

        var outSize = 80
        let kr = input.withUnsafeBytes { ip in
            output.withUnsafeMutableBytes { op in
                IOConnectCallStructMethod(conn, kernelIndex, ip.baseAddress, 80, op.baseAddress, &outSize)
            }
        }
        return kr == KERN_SUCCESS ? output : nil
    }

    /// Raw payload of one SMC key, or nil when it cannot be read.
    func value(_ key: String) -> [UInt8]? {
        guard let info = call(9, key: key) else { return nil }       // READ_KEYINFO
        let size = info.withUnsafeBytes { $0.load(fromByteOffset: offDataSize, as: UInt32.self) }
        guard size > 0, size <= 32 else { return nil }
        guard let data = call(5, key: key, dataSize: size) else { return nil }   // READ_BYTES
        guard data[40] == 0 else { return nil }                       // result code
        return Array(data[offBytes..<(offBytes + Int(size))])
    }

    func uint8(_ key: String) -> Int? { value(key)?.first.map(Int.init) }

    func number(_ key: String) -> Int? {
        guard let bytes = value(key) else { return nil }
        switch bytes.count {
        case 2:      // fpe2: value << 2, big-endian
            let raw = (UInt16(bytes[0]) << 8) | UInt16(bytes[1])
            return Int(raw >> 2)
        case 4:
            let raw = bytes.reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
            return Int(raw)
        default:
            return nil
        }
    }

    func text(_ key: String) -> String? {
        guard let bytes = value(key) else { return nil }
        let text = String(decoding: bytes.prefix { $0 != 0 }, as: UTF8.self)
            .trimmingCharacters(in: .whitespaces)
        return text.isEmpty ? nil : text
    }

    /// Every fan the SMC knows about, in index order.
    func systemFans() -> [SMCFan] {
        guard let count = uint8("FNum"), count > 0, count <= 8 else { return [] }
        var fans: [SMCFan] = []
        for i in 0..<count {
            guard let rpm = number("F\(i)Ac") else { continue }
            let name = text("F\(i)ID") ?? "Fan \(i)"
            fans.append(SMCFan(index: i, name: name, rpm: rpm))
        }
        return fans
    }
}

// IOConnectCallStructMethod wants the main port symbol; older SDKs call it kIOMasterPortDefault.
private let kIOMyMainPort: mach_port_t = 0
