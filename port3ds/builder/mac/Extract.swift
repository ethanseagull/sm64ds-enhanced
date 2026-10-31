// The game data out of the dump: a port of the PC port's extractor,
// extract_assets.ps1 (tangosdev's sm64ds-decomp, tools/portable_kit; MIT),
// which the Windows builder first ran in PowerShell 5.1 and now has in C#
// (Extract.cs). A Mac has no PowerShell, and the script builds its paths with
// backslashes, so here it is natively. The outputs are the script's (and
// Extract.cs's), byte for byte, under DESTINATION:
//
//   extracted/dsd/files/<path>     every file, byte for byte as the card has it
//   build/assets/files.tsv         file id -> path index
//   build/assets/handles.tsv       game handle -> file id, read out of overlay 0
//   build/assets/nitrofs.tsv       where the card's FNT, FAT and overlay tables live
//   build/assets/nitrofs_fnt.bin, nitrofs_fat.bin, nitrofs_ovt9.bin   those tables, verbatim
//   build/assets/romdata.bin       code-side data tables, rebuilt from the dump by
//                                  romdata.recipe.tsv and hash-checked
//   build/assets/romdata.manifest  copied from the payload
//
// The checks and their messages are the script's; a failure throws its
// ##REASON line as a BuildError. The ones that told a player of upstream's
// launcher to "press Play again" say "build again" here.
import CryptoKit
import Foundation

struct Extract {
    let rom: String
    let destination: String
    let recipe: String      // the payload's romdata.recipe.tsv
    let manifest: String    // and romdata.manifest
    let step: (String) -> Void          // the script's narration (Write-Step)
    let progress: (Int, String) -> Void // its ##PROGRESS lines (0..100)

    // Stop-Politely: a failure about the player's dump
    func stop(_ text: String) -> BuildError { return BuildError(text) }

    func run() throws {
        step("Reading " + (rom as NSString).lastPathComponent)
        let romBytes: [UInt8]
        do {
            romBytes = try Bytes.read(rom)
        } catch {
            throw stop("Could not read your .nds file: " + error.localizedDescription)
        }

        // ------------------------------------------------------- check it is the game
        // Cartridge header: 12-byte internal title, 4-byte game code, then the table
        // offsets. A wrong file trips one of these long before it can write nonsense.
        if romBytes.count < 0x4000 {
            throw stop("That file is only \(romBytes.count) bytes, too small to be a DS cartridge dump.")
        }
        let title = Bytes.ascii(romBytes, 0, 12).trimmingTrailingNuls()
        let code = Bytes.ascii(romBytes, 0x0C, 4)
        if title != "S.MARIO64DS" || !code.hasPrefix("ASM") || code.count != 4 {
            throw stop("That is not a Super Mario 64 DS dump (it says title '\(Bytes.printable(title))', " +
                       "game code '\(Bytes.printable(code))'). Use a dump of a Super Mario 64 DS cartridge.")
        }
        let regions = ["E": "North America", "P": "Europe", "J": "Japan", "K": "Korea", "C": "China", "U": "Australia"]
        let regionLetter = String(code.suffix(1))
        let region = regions[regionLetter] ?? "region '\(regionLetter)'"
        // EU-only gate: the port and the recipe are built from the European ROM
        if code != "ASMP" {
            throw stop("This is the \(region) ROM (game code \(code)). This port is built from the " +
                       "European ROM (ASMP), so its data will not come out of a \(region) cartridge.")
        }

        let fntOffset = Bytes.u32(romBytes, 0x40), fntSize = Bytes.u32(romBytes, 0x44)
        let fatOffset = Bytes.u32(romBytes, 0x48), fatSize = Bytes.u32(romBytes, 0x4C)
        let ovtOffset = Bytes.u32(romBytes, 0x50), ovtSize = Bytes.u32(romBytes, 0x54)
        let ovt7Offset = Bytes.u32(romBytes, 0x58), ovt7Size = Bytes.u32(romBytes, 0x5C)
        let usedSize = Bytes.u32(romBytes, 0x80)

        for (name, off, size) in [("file name table", fntOffset, fntSize),
                                  ("file allocation table", fatOffset, fatSize),
                                  ("overlay table", ovtOffset, ovtSize)] {
            if off == 0 || off + size > romBytes.count {
                throw stop("This dump is truncated: its \(name) runs past the end of the file. Re-dump the cartridge.")
            }
        }
        // An EMPTY ARM7 overlay pair is legal and means "no ARM7 overlays",
        // which is what this cartridge really has
        if !(ovt7Offset == 0 && ovt7Size == 0) {
            if ovt7Offset == 0 || ovt7Size == 0 || ovt7Offset + ovt7Size > romBytes.count {
                throw stop("This dump is truncated: its ARM7 overlay table runs past the end of the file. " +
                           "Re-dump the cartridge.")
            }
        }
        if usedSize > romBytes.count {
            throw stop("This dump is incomplete: the header says the cartridge holds \(usedSize) bytes " +
                       "but the file is only \(romBytes.count). Re-dump the cartridge.")
        }
        step("Super Mario 64 DS (\(code), \(region)), \(Bytes.round(Double(romBytes.count) / 1048576)) MB")
        progress(2, "Reading the file tables")

        // ------------------------------------------------------------- the file tables
        // FAT: one 8-byte {start, end} pair per file id.
        let fatCount = fatSize / 8
        var fatStart = [Int](repeating: 0, count: fatCount), fatEnd = [Int](repeating: 0, count: fatCount)
        for i in 0..<fatCount {
            fatStart[i] = Bytes.u32(romBytes, fatOffset + i * 8)
            fatEnd[i] = Bytes.u32(romBytes, fatOffset + i * 8 + 4)
        }

        // FNT: a directory table of 8-byte entries, each pointing at a name subtable.
        // Walk it from the root so a directory is always named before its children.
        // Names with the high bit set are subdirectories and carry a trailing id.
        var paths: [Int: String] = [:]
        var queue: [(Int, String)] = [(0, "")]
        var visited: Set<Int> = [0]
        let damagedNames = "This dump's file name table is damaged. Re-dump the cartridge."
        while !queue.isEmpty {
            let (dirId, base) = queue.removeFirst()
            let entry = fntOffset + dirId * 8
            if entry + 8 > romBytes.count { throw stop(damagedNames) }
            var o = fntOffset + Bytes.u32(romBytes, entry)
            var fileId = Bytes.u16(romBytes, entry + 4)
            while true {
                if o >= romBytes.count { throw stop(damagedNames) }
                let type = Int(romBytes[o]); o += 1
                if type == 0 { break }
                let len = type & 0x7F
                let needs = len + ((type & 0x80) != 0 ? 2 : 0)
                if o + needs > romBytes.count { throw stop(damagedNames) }
                let name = Bytes.ascii(romBytes, o, len); o += len
                if (type & 0x80) != 0 {
                    let sub = Bytes.u16(romBytes, o) & 0x0FFF; o += 2
                    if visited.insert(sub).inserted {
                        queue.append((sub, base.isEmpty ? name : base + "/" + name))
                    }
                } else {
                    paths[fileId] = base.isEmpty ? name : base + "/" + name
                    fileId += 1
                }
            }
        }
        if paths.isEmpty { throw stop("This dump has no named files in it. Re-dump the cartridge.") }
        let ids = paths.keys.sorted()

        // --------------------------------------------------------------- write them out
        let filesRoot = (destination as NSString).appendingPathComponent("extracted/dsd/files")

        // Add up what is about to be written, and check every entry points inside
        // the file, BEFORE writing anything.
        var needBytes = 0
        for id in ids {
            if id < 0 || id >= fatCount {
                throw stop("This dump is damaged: its file name table names a file (\(paths[id]!)) that its " +
                           "file allocation table does not have. Re-dump the cartridge.")
            }
            let length = fatEnd[id] - fatStart[id]
            if length < 0 || fatStart[id] + length > romBytes.count {
                throw stop("This dump is damaged: its file table points at data past the end of the file " +
                           "(file \(id), '\(paths[id]!)'). Re-dump the cartridge.")
            }
            needBytes += length
        }

        // Free space. Skipped, never fatal, if the volume cannot be measured.
        if let free = try? URL(fileURLWithPath: destination)
            .resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
            .volumeAvailableCapacityForImportantUsage, free > 0 {
            let want = needBytes + 16 * 1048576     // the dump, plus room for romdata.bin and slack
            if free < want {
                throw stop("There is not enough free space on your disk to unpack the game data. " +
                           "About \(Bytes.round(Double(want) / 1048576)) MB is needed and " +
                           "\(Bytes.round(Double(free) / 1048576)) MB is free. Free up some space and build again.")
            }
        } else {
            step("    (could not measure free disk space here; carrying on)")
        }

        step("Writing \(paths.count) files to \(filesRoot)")
        var madeDirs = Set<String>()
        var written = 0, bytes = 0
        let total = paths.count
        for id in ids {
            let target = (filesRoot as NSString).appendingPathComponent(paths[id]!)
            let dir = (target as NSString).deletingLastPathComponent
            if madeDirs.insert(dir).inserted {
                try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
            }
            let length = fatEnd[id] - fatStart[id]
            try Bytes.write(romBytes[fatStart[id]..<fatStart[id] + length], to: target)
            written += 1
            bytes += length
            if written % 500 == 0 {
                step("    \(written) of \(total)")
                progress(2 + 68 * written / total, "Unpacking file \(written) of \(total)")
            }
        }
        step("    \(written) files, \(Bytes.round(Double(bytes) / 1048576, 1)) MB")
        progress(70, "Unpacked \(written) files")

        // ------------------------------------------------------------ overlay 0 for the
        // handle table. The game asks for assets by handle, not by file id; overlay 0
        // carries the handle -> path-string table that translates them.
        // Overlay table entries are 32 bytes: id, ram address, size, bss, two static
        // initialiser bounds, file id, then (compressed size | flags << 24).
        if ovtSize < 32 { throw stop("This dump has no ARM9 overlays. Re-dump the cartridge.") }
        let ov0Ram = Bytes.u32(romBytes, ovtOffset + 4)
        let ov0FileId = Bytes.u32(romBytes, ovtOffset + 24)
        let ov0Flags = Bytes.u32(romBytes, ovtOffset + 28) >> 24
        if ov0FileId < 0 || ov0FileId >= fatCount {
            throw stop("This dump is damaged: its overlay table points at a file its file allocation " +
                       "table does not have. Re-dump the cartridge.")
        }
        // (the script's copy would fail here on its own; a Swift range traps)
        if fatEnd[ov0FileId] < fatStart[ov0FileId] || fatEnd[ov0FileId] > romBytes.count {
            throw stop("This dump is damaged: overlay 0 runs past the end of the file. Re-dump the cartridge.")
        }
        var ov0 = Array(romBytes[fatStart[ov0FileId]..<fatEnd[ov0FileId]])
        progress(71, "Decompressing overlay 0")
        if (ov0Flags & 1) != 0 {
            do { ov0 = try Extract.expandBlz(ov0, "overlay 0") }
            catch { throw stop("Could not read overlay 0 of this dump (\(error)). Re-dump the cartridge.") }
        }

        // The table's address and length are properties of the game, not of the dump:
        // the matched initialiser func_ov000_020aa420 walks exactly this many entries.
        let handleTableAddress = 0x020BD4B8
        let handleCount = 0x80A
        let tableOffset = handleTableAddress - ov0Ram
        if tableOffset < 0 || tableOffset + handleCount * 4 > ov0.count {
            throw stop("This dump is Super Mario 64 DS but not a revision this build knows: " +
                       "its overlay 0 has no asset-handle table where one is expected.")
        }

        var idByPath: [String: Int] = [:]
        for id in ids { idByPath[paths[id]!] = id }

        step("Reading the asset-handle table out of overlay 0")
        progress(72, "Reading the asset-handle table")
        var handleRows = "handle\thex_handle\tfile_id\thex_file_id\tpath\tkind\tsize\n"
        for h in 0..<handleCount {
            if h % 400 == 0 && h > 0 {
                progress(72 + 8 * h / handleCount, "Cataloguing asset handle \(h) of \(handleCount)")
            }
            let pointer = Bytes.u32(ov0, tableOffset + h * 4)
            let stringOffset = pointer - ov0Ram
            if stringOffset < 0 || stringOffset >= ov0.count {
                throw stop("This dump is Super Mario 64 DS but not a revision this build knows: " +
                           "asset handle \(h) points outside overlay 0.")
            }
            let end = ov0[stringOffset...].firstIndex(of: 0) ?? ov0.count
            let assetPath = Bytes.ascii(ov0, stringOffset, end - stringOffset)
            guard let id = idByPath[assetPath] else {
                throw stop("This dump is Super Mario 64 DS but not a revision this build knows: " +
                           "asset handle \(h) names '\(assetPath)', which its filesystem does not have.")
            }
            let size = fatEnd[id] - fatStart[id]
            handleRows += "\(h)\t0x\(Extract.hex4(h))\t\(id)\t0x\(Extract.hex4(id))\t\(assetPath)\t\(Extract.kind(assetPath))\t\(size)\n"
        }

        var fileRows = "file_id\thex_id\tpath\tkind\tsize\n"
        for id in ids {
            let size = fatEnd[id] - fatStart[id]
            fileRows += "\(id)\t0x\(Extract.hex4(id))\t\(paths[id]!)\t\(Extract.kind(paths[id]!))\t\(size)\n"
        }

        let assetsDir = (destination as NSString).appendingPathComponent("build/assets")
        func asset(_ name: String) -> String { return (assetsDir as NSString).appendingPathComponent(name) }
        try FileManager.default.createDirectory(atPath: assetsDir, withIntermediateDirectories: true)
        try Bytes.write(Array(fileRows.utf8), to: asset("files.tsv"))
        try Bytes.write(Array(handleRows.utf8), to: asset("handles.tsv"))
        step("    \(paths.count) files and \(handleCount) handles catalogued")

        // ------------------------------------------------------------ the NitroFS tables
        // The game runs the cartridge's own name walker (port/hal/fs_names.cpp), so
        // the FNT and FAT have to be the card's actual bytes, and its virtual
        // cartridge serves the ARM9 overlay table out of nitrofs_ovt9.bin. The ARM7
        // pair is 0 + 0 on this cartridge, so there is no nitrofs_ovt7.bin.
        for (name, size) in [("file name table", fntSize), ("file allocation table", fatSize)] where size == 0 {
            throw stop("This dump has an empty \(name), so the game could not look up any of its files. " +
                       "Re-dump the cartridge.")
        }
        try Bytes.write(romBytes[fntOffset..<fntOffset + fntSize], to: asset("nitrofs_fnt.bin"))
        try Bytes.write(romBytes[fatOffset..<fatOffset + fatSize], to: asset("nitrofs_fat.bin"))
        for (name, off, size) in [("nitrofs_ovt9.bin", ovtOffset, ovtSize), ("nitrofs_ovt7.bin", ovt7Offset, ovt7Size)]
            where size != 0 {
            try Bytes.write(romBytes[off..<off + size], to: asset(name))
        }
        let nitroRows = "key\tvalue\n" +
            "fnt_offset\t\(fntOffset)\n" + "fnt_size\t\(fntSize)\n" +
            "fat_offset\t\(fatOffset)\n" + "fat_size\t\(fatSize)\n" +
            "ovt9_offset\t\(ovtOffset)\n" + "ovt9_size\t\(ovtSize)\n" +
            "ovt7_offset\t\(ovt7Offset)\n" + "ovt7_size\t\(ovt7Size)\n"
        try Bytes.write(Array(nitroRows.utf8), to: asset("nitrofs.tsv"))
        step("    name table \(fntSize) bytes, allocation table \(fatSize) bytes, overlay table \(ovtSize) bytes")
        progress(80, "Rebuilding romdata.bin")

        // ------------------------------------------------------------ romdata.bin
        // ROM-CLEAN: the game's executable ships with every code-side data table
        // zeroed and fills them at boot from build/assets/romdata.bin. The recipe
        // -- offsets and hashes only, no game bytes -- says where each piece lives:
        // decompress the arm9 program and the overlays it names, copy each range,
        // hash-check the result. Ranges that reach past a decompressed image are
        // runtime-zero (bss) and stay zero, exactly as the recipe's hashes expect.
        let damagedRecipe = BuildError("This builder's romdata.recipe.tsv is damaged, so the game data cannot " +
                                       "be rebuilt. Download the builder again.")
        guard FileManager.default.fileExists(atPath: recipe) else {
            throw BuildError("This builder is incomplete: romdata.recipe.tsv is missing, so the game data " +
                             "cannot be rebuilt. Download the builder again.")
        }
        let recipeLines = try Bytes.lines(recipe)
        guard recipeLines.count >= 2, let head = Extract.recipeHead(recipeLines[0]) else { throw damagedRecipe }
        let (wantSha, blobTotal) = head

        // Which images the recipe needs (in the order it first names them), and
        // where the overlays sit in the dump
        var needed: [String] = []
        var neededSet = Set<String>()
        var rows: [(off: Int, size: Int, src: String, srcOff: Int)] = []
        for line in recipeLines {
            if line.hasPrefix("#") || line.trimmingCharacters(in: .whitespaces).isEmpty { continue }
            let f = line.components(separatedBy: "\t")
            if f.count < 4 {
                throw BuildError("This builder's romdata.recipe.tsv is damaged (a line is incomplete), so " +
                                 "the game data cannot be rebuilt. Download the builder again.")
            }
            if neededSet.insert(f[2]).inserted { needed.append(f[2]) }
            guard let off = Int(f[0]), let size = Int(f[1]), let srcOff = Int(f[3]) else { throw damagedRecipe }
            rows.append((off, size, f[2], srcOff))
        }
        var ovByName: [String: (fileId: Int, flags: Int)] = [:]
        for i in 0..<ovtSize / 32 {
            let e = ovtOffset + i * 32
            ovByName[String(format: "ov%03ld", Bytes.u32(romBytes, e))] =
                (Bytes.u32(romBytes, e + 24), Bytes.u32(romBytes, e + 28) >> 24)
        }

        step("Rebuilding romdata.bin (\(needed.count) images to decompress)")
        var images: [String: [UInt8]] = [:]
        for (done, src) in needed.enumerated() {
            progress(80 + 17 * done / needed.count, "Decompressing \(src) (\(done + 1) of \(needed.count))")
            if src == "arm9" {
                // Program header: rom offset at 0x20, ram address 0x28, size 0x2C. The
                // payload is stored with the same back-to-front compression as the overlays.
                let a9Off = Bytes.u32(romBytes, 0x20), a9Size = Bytes.u32(romBytes, 0x2C)
                if a9Off + a9Size > romBytes.count {
                    throw stop("Could not read this dump's program data (it runs past the end of the file). " +
                               "Re-dump the cartridge.")
                }
                do { images[src] = try Extract.expandBlz(Array(romBytes[a9Off..<a9Off + a9Size]), "the program") }
                catch { throw stop("Could not read this dump's program data (\(error)). Re-dump the cartridge.") }
            } else {
                guard let ov = ovByName[src] else {
                    throw stop("This dump is Super Mario 64 DS but not a revision this build knows: it has no overlay '\(src)'.")
                }
                if ov.fileId < 0 || ov.fileId >= fatCount {
                    throw stop("This dump is damaged: its overlay table points at a file its file allocation " +
                               "table does not have (overlay '\(src)'). Re-dump the cartridge.")
                }
                let length = fatEnd[ov.fileId] - fatStart[ov.fileId]
                if length < 0 || fatStart[ov.fileId] + length > romBytes.count {
                    throw stop("This dump is damaged: overlay '\(src)' runs past the end of the file. Re-dump the cartridge.")
                }
                var raw = Array(romBytes[fatStart[ov.fileId]..<fatStart[ov.fileId] + length])
                if (ov.flags & 1) != 0 {
                    do { raw = try Extract.expandBlz(raw, "overlay '\(src)'") }
                    catch { throw stop("Could not read overlay '\(src)' of this dump (\(error)). Re-dump the cartridge.") }
                }
                images[src] = raw
            }
            step("    \(src) (\(Bytes.round(Double(images[src]!.count) / 1024)) KB)")
        }

        // Assemble. The buffer starts zeroed, so a range past its image's end
        // simply keeps its zeros.
        progress(98, "Assembling and checking romdata.bin")
        var blob = [UInt8](repeating: 0, count: blobTotal)
        for r in rows {
            guard let img = images[r.src], r.off >= 0, r.size >= 0, r.srcOff >= 0, r.off + r.size <= blob.count else {
                throw BuildError("This builder's romdata.recipe.tsv is damaged (a line is out of range), so the " +
                                 "game data cannot be rebuilt. Download the builder again.")
            }
            let have = min(r.size, max(0, img.count - r.srcOff))
            if have > 0 { blob.replaceSubrange(r.off..<r.off + have, with: img[r.srcOff..<r.srcOff + have]) }
        }
        if Face.sha(blob) != wantSha {
            // The header said ASMP, so this is a European cartridge dump, and it still
            // did not rebuild: a patched or translated ROM, a trimmed one, or a bad dump.
            throw stop("This is a European dump (game code \(code)), but its contents do not match the " +
                       "cartridge this build was made from. That usually means the file has been modified " +
                       "(a patch, a translation or a trimmed dump) or the dump is faulty. Use an untouched " +
                       "dump of your own retail cartridge, then build again.")
        }
        try Bytes.write(blob, to: asset("romdata.bin"))
        step("    romdata.bin verified (\(blobTotal) bytes, checksum OK)")

        // The game verifies romdata.bin at boot against romdata.manifest; put it
        // where the game looks.
        guard FileManager.default.fileExists(atPath: manifest) else {
            throw BuildError("This builder is incomplete: romdata.manifest is missing, so the game would " +
                             "refuse to start. Download the builder again.")
        }
        try Bytes.write(try Bytes.read(manifest), to: asset("romdata.manifest"))
        progress(100, "Done")
    }

    // "# romdata-recipe v1 SHA256 LENGTH"
    static func recipeHead(_ line: String) -> (String, Int)? {
        let f = line.components(separatedBy: " ")
        guard f.count == 5, f[0] == "#", f[1] == "romdata-recipe", f[2] == "v1", f[3].count == 64,
              f[3].allSatisfy({ "0123456789abcdef".contains($0) }),
              !f[4].isEmpty, f[4].allSatisfy({ $0.isASCII && $0.isNumber }), let n = Int(f[4]) else { return nil }
        return (f[3], n)
    }

    static let kinds = [".bca": "animation", ".bmd": "model", ".btp": "texture-sequence",
                        ".kcl": "collision", ".narc": "archive", ".sdat": "sound-archive", ".bin": "data"]

    static func kind(_ path: String) -> String {
        let name = (path as NSString).lastPathComponent
        guard let dot = name.lastIndex(of: "."), dot != name.index(before: name.endIndex) else { return "file" }
        return kinds[String(name[dot...]).lowercased()] ?? "file"
    }

    // .NET's {0:x4}
    static func hex4(_ v: Int) -> String {
        let s = String(v, radix: 16)
        return s.count >= 4 ? s : String(repeating: "0", count: 4 - s.count) + s
    }

    struct BlzError: Error, CustomStringConvertible { let description: String }

    // Overlays and the arm9 program are stored back-to-front LZ compressed
    // ("BLZ"); Expand-Blz, step for step, with an error in place of
    // PowerShell's own for an index out of range.
    static func expandBlz(_ data: [UInt8], _ what: String) throws -> [UInt8] {
        if data.count < 8 { return data }
        let footer = Bytes.u32(data, data.count - 8)
        let extraSize = Bytes.u32(data, data.count - 4)
        if extraSize == 0 { return data }        // flagged, but stored plain
        let headerLen = footer >> 24
        var compressedLen = footer & 0xFFFFFF
        if headerLen < 8 || headerLen > data.count || compressedLen > data.count {
            throw BlzError(description: "\(what) has an unreadable compression header")
        }
        if compressedLen >= data.count { compressedLen = data.count }

        // Anything before the compressed run is already plain and stays put.
        let passthrough = data.count - compressedLen
        let compLen = compressedLen - headerLen
        let outLen = data.count + extraSize - passthrough
        var out = [UInt8](repeating: 0, count: outLen)
        let outOfRange = BlzError(description: "\(what) reads past its data")

        // Both streams run backwards: index -1-n from the end of each.
        var read = 0, done = 0, flags = 0, mask = 1
        func next() throws -> Int {
            let at = passthrough + compLen - 1 - read
            if at < 0 || at >= data.count { throw outOfRange }
            read += 1
            return Int(data[at])
        }
        while done < outLen {
            if mask == 1 {
                if read >= compressedLen { throw BlzError(description: "\(what) ended mid-stream") }
                flags = try next()
                mask = 0x80
            } else {
                mask >>= 1
            }
            if (flags & mask) != 0 {
                let b1 = try next(), b2 = try next()
                let length = (b1 >> 4) + 3
                var disp = (((b1 & 0x0F) << 8) | b2) + 3
                if disp > done {
                    if done < 2 { throw BlzError(description: "\(what) back-references data that is not there yet") }
                    disp = 2
                }
                var from = done - disp
                for _ in 0..<length {
                    if done >= outLen { throw outOfRange }
                    out[outLen - 1 - done] = out[outLen - 1 - from]
                    from += 1; done += 1
                }
            } else {
                out[outLen - 1 - done] = UInt8(try next())
                done += 1
            }
        }
        return Array(data[0..<passthrough]) + out
    }
}
