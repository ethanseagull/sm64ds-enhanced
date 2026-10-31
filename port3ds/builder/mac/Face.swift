// The HOME Menu banner's 3D model: the game's title logo with "ENHANCED"
// under it, and the Power Star flying round it on its path from the opening.
//
// A port of ../Face.cs, value for value. face.template is that banner (a CGFX)
// with every value that comes from the game zeroed; face.recipe says where
// each value goes and what it is computed from. Here the values are computed
// from the dump's own files (the recipe's file lines, checked by size and
// SHA-256; its model lines say which make which model) the same way Face.cs
// computes them, and written into the template. Where Face.cs leans on C#'s
// int arithmetic (wrapping) or Math.Round (to even), this does the same.
import CryptoKit
import Foundation

enum Face {
    static let turn = Double.pi * 2 / 4096

    // The banner, or nil if the dump's files aren't the ones the recipe was
    // made from (the caller then makes the flat banner). The logo is the
    // game's title-screen logo (Art.titleLogo); without one, that part of the
    // banner stays empty.
    static func banner(dsmt: String, template: [UInt8], recipe: [String], logo: Pixels?,
                       log: (String) -> Void) throws -> [UInt8]? {
        var files: [String: [UInt8]] = [:]
        var models: [String: Model] = [:]
        var start = -1, end = -1, blend = -1
        for line in recipe where !line.isEmpty {
            let f = line.components(separatedBy: "\t")
            if f[0] == "file" {
                guard f.count >= 4 else { throw damaged() }
                let path = (dsmt as NSString).appendingPathComponent(f[1])
                if !FileManager.default.fileExists(atPath: path) {
                    log("Banner: no " + f[1] + " in the extracted files.")
                    return nil
                }
                let b = try Bytes.read(path)
                if b.count != (try number(f[2])) || sha(b) != f[3] {
                    log("Banner: " + f[1] + " is not the one the banner was made from.")
                    return nil
                }
                files[f[1]] = b
            } else if f[0] == "model" {
                // TAG BMD BCA BTP (- for none), from the files above
                guard f.count >= 5, let bmd = files[f[2]], let bca = files[f[3]],
                      f[4] == "-" || files[f[4]] != nil else { throw damaged() }
                models[f[1]] = try Model(Bmd(bmd), Bca(bca), f[4] == "-" ? nil : Btp(files[f[4]]!))
            } else if f[0] == "window" {
                guard f.count >= 4 else { throw damaged() }
                start = try number(f[1]); end = try number(f[2]); blend = try number(f[3])
            }
        }
        if models.isEmpty || start < 0 { throw damaged() }
        let keys = window(start, end, blend)

        var cgfx = template
        var curves: [String: [String: [Double]]] = [:]
        var lids: [String: [Double]]? = nil
        for line in recipe where !line.isEmpty {
            let f = line.components(separatedBy: "\t")
            if f[0] == "file" || f[0] == "model" || f[0] == "window" || f[0].hasPrefix("sm64ds") { continue }
            guard f.count >= 3 else { throw damaged() }
            let offs = try f[1].components(separatedBy: ",").map(number)
            if f[0] == "logo" {
                guard f.count >= 5 else { throw damaged() }
                if let logo = logo {
                    let w = try number(f[3]), h = try number(f[4])
                    let data = swizzle(centred(logo, w, h), w, h, try number(f[2]))
                    for o in offs { try put(&cgfx, o, data) }
                }
                continue
            }
            guard let m = models[f[2]] else { throw damaged() }
            switch f[0] {
            case "vtx", "idx":
                guard f.count >= 7, f[0] == "idx" || f.count >= 8 else { throw damaged() }
                let root = f[f.count - 1] == "root"   // a rigid part, in its root bone's space
                let part = m.part(try number(f[3]), try number(f[4]), root)
                let data = f[0] == "idx" ? part.indices(f[6] == "1") : part.stream(f[5], f[7] == "1")
                for o in offs { try put(&cgfx, o, data) }
            case "tex":
                guard f.count >= 6 else { throw damaged() }
                let ti = try number(f[3]), pi = try number(f[4])
                let data = swizzle(m.bmd.rgba(ti, pi), m.bmd.textures[ti].w, m.bmd.textures[ti].h, try number(f[5]))
                for o in offs { try put(&cgfx, o, data) }
            case "rest":
                guard f.count >= 4 else { throw damaged() }
                let b = m.bmd.bones[try number(f[3])]
                var v = [Double](repeating: 0, count: 9)
                for k in 0..<3 {
                    v[k] = b.scale[k]; v[3 + k] = Double(b.rot[k]) * turn; v[6 + k] = b.trans[k]
                }
                for o in offs { try floats(&cgfx, o, 4, v) }
            case "ibm":
                guard f.count >= 4 else { throw damaged() }
                let inv = invert(m.bind[try number(f[3])])
                var v = [Double](repeating: 0, count: 12)
                for r in 0..<3 {
                    for c in 0..<4 { v[r * 4 + c] = Double(Float(inv[c * 4 + r])) }
                }
                for o in offs { try floats(&cgfx, o, 4, v) }
            case "const", "keys":
                guard f.count >= 6, f[0] == "const" || f.count >= 7 else { throw damaged() }
                if curves[f[2]] == nil { curves[f[2]] = m.boneCurves(keys) }
                guard let vals = curves[f[2]]![f[3] + "/" + f[4] + "/" + f[5]] else { throw damaged() }
                try curve(&cgfx, offs, f[0] == "keys" ? f[6] : nil, vals)
            case "lidc", "lidk":
                guard f.count >= 7, f[0] == "lidc" || f.count >= 8 else { throw damaged() }
                if lids == nil { lids = m.lidCurves(keys) }
                guard let vals = lids![f[3] + "/" + f[4] + "/" + f[5] + "/" + f[6]] else { throw damaged() }
                try curve(&cgfx, offs, f[0] == "lidk" ? f[7] : nil, vals)
            default:
                throw BuildError("The face banner's recipe is damaged (" + f[0] + ").")
            }
        }
        return cgfx
    }

    static func damaged() -> BuildError { return BuildError("The banner's recipe is damaged.") }

    static func number(_ s: String) throws -> Int {
        guard let n = Int(s) else { throw damaged() }
        return n
    }

    // a constant (the value once) or keys ((frame, value) pairs: OFF is the
    // first key's value, key i's 8i bytes on, for window key K[i])
    static func curve(_ cgfx: inout [UInt8], _ offs: [Int], _ keyList: String?, _ vals: [Double]) throws {
        guard let keyList = keyList else {
            for o in offs { try floats(&cgfx, o, 4, [vals[0]]) }
            return
        }
        let ks = keyList.components(separatedBy: ",")
        var v = [Double](repeating: 0, count: ks.count)
        for i in 0..<ks.count {
            let k = try number(ks[i])
            guard k >= 0 && k < vals.count else { throw damaged() }
            v[i] = vals[k]
        }
        for o in offs { try floats(&cgfx, o, 8, v) }
    }

    // the logo, edges cleaned, centred in a w x h RGBA texture, top row
    // first (face_gltf.logo_image). The game draws it over its blue sky, so
    // the pixels just outside its outline are a light blue halo anywhere
    // else: an opaque pixel touching transparency (or the picture's edge)
    // that is light (a channel at 100 or more) and bluish (blue more than
    // red + 8) goes, in one pass (face_gltf.clean_edges)
    static func centred(_ pic: Pixels, _ w: Int, _ h: Int) -> [UInt8] {
        let pw = pic.width, ph = pic.height
        let c = pic.rgba
        @inline(__always) func alpha(_ x: Int, _ y: Int) -> UInt8 { return c[(y * pw + x) * 4 + 3] }
        var drop = [Bool](repeating: false, count: pw * ph)
        let dx = [1, -1, 0, 0], dy = [0, 0, 1, -1]
        for y in 0..<ph {
            for x in 0..<pw {
                let p = (y * pw + x) * 4
                let r = Int(c[p]), g = Int(c[p + 1]), b = Int(c[p + 2]), a = c[p + 3]
                if a == 0 || max(r, max(g, b)) < 100 || b <= r + 8 { continue }
                for k in 0..<4 {
                    let nx = x + dx[k], ny = y + dy[k]
                    if nx < 0 || ny < 0 || nx >= pw || ny >= ph || alpha(nx, ny) == 0 { drop[y * pw + x] = true; break }
                }
            }
        }
        var o = [UInt8](repeating: 0, count: w * h * 4)
        let x0 = (w - pw) / 2, y0 = (h - ph) / 2
        for y in 0..<ph {
            for x in 0..<pw {
                let X = x0 + x, Y = y0 + y
                if X < 0 || Y < 0 || X >= w || Y >= h || drop[y * pw + x] { continue }
                let p = (y * pw + x) * 4, at = (Y * w + X) * 4
                o[at] = c[p]; o[at + 1] = c[p + 1]; o[at + 2] = c[p + 2]; o[at + 3] = c[p + 3]
            }
        }
        return o
    }

    static func put(_ cgfx: inout [UInt8], _ at: Int, _ data: [UInt8]) throws {
        if at < 0 || at + data.count > cgfx.count { throw BuildError("The face banner's recipe is damaged.") }
        cgfx.replaceSubrange(at..<at + data.count, with: data)
    }

    static func floats(_ cgfx: inout [UInt8], _ at: Int, _ step: Int, _ v: [Double]) throws {
        for i in 0..<v.count {
            let bits = Float(v[i]).bitPattern
            try put(&cgfx, at + i * step, [UInt8(bits & 0xff), UInt8(bits >> 8 & 0xff),
                                           UInt8(bits >> 16 & 0xff), UInt8(bits >> 24)])
        }
    }

    static func sha(_ b: [UInt8]) -> String {
        return SHA256.hash(data: b).map { String(format: "%02x", $0) }.joined()
    }

    // The game frames the loop plays (face_gltf.window): start..end-1, then
    // start again; over the last `blend` frames the pose crossfades into the
    // frames just before start. Each is (frame a, frame b, weight of b).
    struct Key { var a: Int, b: Int, t: Double }

    static func window(_ start: Int, _ end: Int, _ blend: Int) -> [Key] {
        var w: [Key] = []
        var f = start
        while f < end {
            let k = f - (end - blend)
            w.append(k >= 0 ? Key(a: f, b: start - blend + k, t: Double(k + 1) / Double(blend))
                            : Key(a: f, b: f, t: 0))
            f += 1
        }
        w.append(Key(a: start, b: start, t: 0))
        return w
    }

    // --- matrices: 4x4, row-vector convention, row-major ----------------

    static func euler(_ rx: Double, _ ry: Double, _ rz: Double) -> [Double] {
        let a0 = sin(rx * turn), a1 = cos(rx * turn)
        let b0 = sin(ry * turn), b1 = cos(ry * turn)
        let c0 = sin(rz * turn), c1 = cos(rz * turn)
        return [b1 * c1, b1 * c0, -b0,
                b0 * a0 * c1 - a1 * c0, a1 * c1 + b0 * a0 * c0, b1 * a0,
                a0 * c0 + b0 * a1 * c1, b0 * a1 * c0 - a0 * c1, b1 * a1]
    }

    static func local(_ scale: [Double], _ rot: [Int], _ trans: [Double]) -> [Double] {
        let r = euler(Double(rot[0]), Double(rot[1]), Double(rot[2]))
        var m = [Double](repeating: 0, count: 16)
        for i in 0..<3 {
            for j in 0..<3 { m[i * 4 + j] = scale[i] * r[i * 3 + j] }
        }
        m[12] = trans[0]; m[13] = trans[1]; m[14] = trans[2]; m[15] = 1
        return m
    }

    static func mul(_ a: [Double], _ b: [Double]) -> [Double] {
        var r = [Double](repeating: 0, count: 16)
        for i in 0..<4 {
            for j in 0..<4 {
                var s = 0.0
                for k in 0..<4 { s += a[i * 4 + k] * b[k * 4 + j] }
                r[i * 4 + j] = s
            }
        }
        return r
    }

    // v (x, y, z, w) times m; the first three
    static func apply(_ v: [Double], _ m: [Double], _ n: Int) -> [Double] {
        var r = [Double](repeating: 0, count: 3)
        for j in 0..<3 {
            var s = 0.0
            for k in 0..<n { s += v[k] * m[k * 4 + j] }
            r[j] = s
        }
        return r
    }

    static func invert(_ m: [Double]) -> [Double] {
        var a = m
        var r = [Double](repeating: 0, count: 16)
        for i in 0..<4 { r[i * 5] = 1 }
        for c in 0..<4 {
            var p = c
            for i in c + 1..<4 where abs(a[i * 4 + c]) > abs(a[p * 4 + c]) { p = i }
            for j in 0..<4 {
                a.swapAt(c * 4 + j, p * 4 + j)
                r.swapAt(c * 4 + j, p * 4 + j)
            }
            let d = a[c * 4 + c]
            for j in 0..<4 { a[c * 4 + j] /= d; r[c * 4 + j] /= d }
            for i in 0..<4 where i != c {
                let k = a[i * 4 + c]
                for j in 0..<4 { a[i * 4 + j] -= k * a[c * 4 + j]; r[i * 4 + j] -= k * r[c * 4 + j] }
            }
        }
        return r
    }

    // --- the game's files ---------------------------------------------

    static func u32(_ d: [UInt8], _ o: Int) -> UInt32 {
        return UInt32(d[o]) | UInt32(d[o + 1]) << 8 | UInt32(d[o + 2]) << 16 | UInt32(d[o + 3]) << 24
    }
    static func u(_ d: [UInt8], _ o: Int) -> Int { return Int(u32(d, o)) }   // C#'s (int)U32
    static func s32(_ d: [UInt8], _ o: Int) -> Int32 { return Int32(bitPattern: u32(d, o)) }
    static func u16(_ d: [UInt8], _ o: Int) -> Int { return Int(d[o]) | Int(d[o + 1]) << 8 }
    static func s16(_ d: [UInt8], _ o: Int) -> Int { return Int(Int16(bitPattern: UInt16(u16(d, o)))) }

    static func name(_ d: [UInt8], _ o: Int) -> String {
        if o == 0 { return "" }
        var e = o
        while e < d.count && d[e] != 0 { e += 1 }
        return Bytes.ascii(d, o, e - o)
    }

    static func sx(_ v: UInt32, _ bits: Int) -> Int {
        let x = Int(v & ((1 << UInt32(bits)) - 1))
        return (x & (1 << (bits - 1))) != 0 ? x - (1 << bits) : x
    }

    // the game's LZ77 files: "LZ77" and a type-0x10 stream, or plain
    static func lz77(_ raw: [UInt8]) -> [UInt8] {
        var i = raw.count >= 4 && raw[0] == 0x4c && raw[1] == 0x5a && raw[2] == 0x37 && raw[3] == 0x37 ? 4 : 0
        if raw[i] != 0x10 { return raw }
        let n = Int(raw[i + 1]) | Int(raw[i + 2]) << 8 | Int(raw[i + 3]) << 16
        var o = [UInt8](repeating: 0, count: n)
        var at = 0
        i += 4
        while at < n {
            let flags = Int(raw[i]); i += 1
            var b = 0
            while b < 8 && at < n {
                if (flags & (0x80 >> b)) != 0 {
                    let v = Int(raw[i]) << 8 | Int(raw[i + 1])
                    i += 2
                    let len = (v >> 12) + 3, disp = (v & 0xfff) + 1
                    var k = 0
                    while k < len && at < n { o[at] = o[at - disp]; k += 1; at += 1 }
                } else {
                    o[at] = raw[i]; at += 1; i += 1
                }
                b += 1
            }
        }
        return o
    }

    static func rgb555(_ c: Int) -> [Int] {
        let r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31
        return [r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2]
    }

    final class Bone { var parent = 0; var scale = [0.0, 0, 0], trans = [0.0, 0, 0]; var rot = [0, 0, 0]; var mats: [UInt8] = [], dls: [UInt8] = [] }
    struct Dl { var transforms: [UInt8], data: [UInt8] }
    struct Tex { var name: String, data: [UInt8], extra: [UInt8], param: UInt32, fmt: Int, w: Int, h: Int }
    struct Mat { var name: String, tex: Int, pal: Int, words: [UInt32] }

    // A BMD0 model (include/BMD_File.h)
    final class Bmd {
        let d: [UInt8]
        let scaleShift: Int
        var bones: [Bone] = []
        var dls: [Dl] = []
        var textures: [Tex] = []
        var paletteNames: [String] = []
        var palettes: [[[Int]]] = []
        var materials: [Mat] = []
        var tmap = [Int](repeating: 0, count: 32)

        init(_ raw: [UInt8]) {
            let d = lz77(raw)
            self.d = d
            scaleShift = Int(u32(d, 0))
            for i in 0..<u(d, 4) {
                let o = u(d, 8) + i * 0x40
                let n = u(d, o + 0x30)
                let b = Bone()
                b.parent = s16(d, o + 8) != 0 ? s16(d, o) + s16(d, o + 8) : -1
                for k in 0..<3 {
                    b.scale[k] = Double(s32(d, o + 0x10 + k * 4)) / 4096.0
                    b.rot[k] = s16(d, o + 0x1c + k * 2)
                    b.trans[k] = Double(s32(d, o + 0x24 + k * 4)) / 4096.0
                }
                b.mats = Bmd.slice(d, u(d, o + 0x34), n)
                b.dls = Bmd.slice(d, u(d, o + 0x38), n)
                bones.append(b)
            }
            for g in 0..<u(d, 0x0c) {
                let rec = u(d, 0x10) + g * 8
                for k in 0..<u(d, rec) {
                    let s = u(d, rec + 4) + k * 0x10
                    dls.append(Dl(transforms: Bmd.slice(d, u(d, s + 4), u(d, s)),
                                  data: Bmd.slice(d, u(d, s + 0xc), u(d, s + 8))))
                }
            }
            for i in 0..<u(d, 0x14) {
                let o = u(d, 0x18) + i * 0x14
                let p = u32(d, o + 0x10)
                let at = u(d, o + 4), size = u(d, o + 8)
                textures.append(Tex(name: name(d, u(d, o)), data: Bmd.slice(d, at, size),
                                    extra: Bmd.slice(d, at + size, d.count - at - size), param: p,
                                    fmt: Int(p >> 26) & 7, w: 8 << Int((p >> 20) & 7), h: 8 << Int((p >> 23) & 7)))
            }
            for i in 0..<u(d, 0x1c) {
                let o = u(d, 0x20) + i * 0x10
                let at = u(d, o + 4), size = u(d, o + 8)
                var cols: [[Int]] = []
                for k in 0..<size / 2 { cols.append(rgb555(u16(d, at + k * 2))) }
                paletteNames.append(name(d, u(d, o)))
                palettes.append(cols)
            }
            for i in 0..<u(d, 0x24) {
                let o = u(d, 0x28) + i * 0x30
                var words = [UInt32](repeating: 0, count: 9)
                for k in 0..<9 { words[k] = u32(d, o + 0x0c + k * 4) }
                materials.append(Mat(name: name(d, u(d, o)), tex: Int(s32(d, o + 4)), pal: Int(s32(d, o + 8)), words: words))
            }
            // transform ID -> bone: a display list's MTX_RESTORE k is its
            // Transforms[k], which is this table's bone
            for i in 0..<32 { tmap[i] = u16(d, u(d, 0x2c) + i * 2) }
        }

        static func slice(_ d: [UInt8], _ at: Int, _ n: Int) -> [UInt8] {
            let count = max(0, min(n, d.count - at))
            return count == 0 ? [] : Array(d[at..<at + count])
        }

        // a texture as RGBA bytes, top row first (port/ntr/texture.cpp)
        func rgba(_ ti: Int, _ pi: Int) -> [UInt8] {
            let t = textures[ti]
            let pal: [[Int]] = pi >= 0 ? palettes[pi] : []
            let transparent0 = ((t.param >> 29) & 1) != 0
            var o = [UInt8](repeating: 0, count: t.w * t.h * 4)
            func c(_ i: Int) -> [Int] { return i < pal.count ? pal[i] : [0, 0, 0] }
            func put(_ px: Int, _ rgb: [Int], _ a: Int) {
                o[px * 4] = UInt8(rgb[0]); o[px * 4 + 1] = UInt8(rgb[1]); o[px * 4 + 2] = UInt8(rgb[2]); o[px * 4 + 3] = UInt8(a)
            }
            let d = t.data
            if t.fmt == 5 {
                let bw = t.w / 4
                for blk in 0..<bw * (t.h / 4) {
                    let texels = u32(d, blk * 4)
                    let info = u16(t.extra, blk * 2)
                    let b = (info & 0x3fff) * 2, mode = info >> 14
                    let c0 = c(b), c1 = c(b + 1)
                    let four: [[Int]?]
                    if mode == 0 { four = [c0, c1, c(b + 2), nil] }
                    else if mode == 1 { four = [c0, c1, Bmd.blend(c0, c1, 1, 1), nil] }
                    else if mode == 2 { four = [c0, c1, c(b + 2), c(b + 3)] }
                    else { four = [c0, c1, Bmd.blend(c0, c1, 5, 3), Bmd.blend(c0, c1, 3, 5)] }
                    for k in 0..<16 {
                        let px = ((blk / bw) * 4 + k / 4) * t.w + (blk % bw) * 4 + k % 4
                        if let p = four[Int((texels >> UInt32(k * 2)) & 3)] { put(px, p, 255) }
                    }
                }
                return o
            }
            for i in 0..<t.w * t.h {
                if t.fmt == 1 { put(i, c(Int(d[i]) & 31), (Int(d[i]) >> 5) * 255 / 7) }
                else if t.fmt == 6 { put(i, c(Int(d[i]) & 7), (Int(d[i]) >> 3) * 255 / 31) }
                else if t.fmt >= 2 && t.fmt <= 4 {
                    let bits = t.fmt == 2 ? 2 : t.fmt == 3 ? 4 : 8
                    let v = (Int(d[i * bits / 8]) >> ((i * bits) % 8)) & ((1 << bits) - 1)
                    put(i, c(v), v == 0 && transparent0 ? 0 : 255)
                } else if t.fmt == 7 {
                    let v = u16(d, i * 2)
                    put(i, rgb555(v), (v & 0x8000) != 0 ? 255 : 0)
                }
            }
            return o
        }

        static func blend(_ a: [Int], _ b: [Int], _ wa: Int, _ wb: Int) -> [Int] {
            return (0..<3).map { (a[$0] * wa + b[$0] * wb) / (wa + wb) }
        }
    }

    // A BCA joint animation, sampled as func_0204547c does
    final class Bca {
        let d: [UInt8]
        let frames: Int
        let pScale: Int, pRot: Int, pTrans: Int, tracks: Int

        init(_ raw: [UInt8]) {
            d = lz77(raw)
            frames = u16(d, 2)
            pScale = u(d, 8); pRot = u(d, 0xc); pTrans = u(d, 0x10); tracks = u(d, 0x14)
        }

        // func_020457f0, in C#'s wrapping int arithmetic
        func fix(_ pool: Int, _ shift: Int, _ flag: Int, _ start: Int, _ f: Int) -> Int {
            func t(_ k: Int) -> Int32 { return s32(d, pool + (start + k) * 4) }
            if flag == 0 { return Int(t(0)) }
            if shift == 0 { return Int(t(f)) }
            let b = ((frames - 1) >> shift) << shift
            let i = f >> shift
            if f >= b { return Int(t(i + (f - b))) }
            let rem = f - (i << shift)
            if rem == 0 { return Int(t(i)) }
            let mixed = t(i) &* Int32(truncatingIfNeeded: (1 << shift) - rem) &+ t(i + 1) &* Int32(truncatingIfNeeded: rem)
            return Int(mixed >> Int32(shift))
        }

        // func_020456a0, 12-bit angles
        func angle(_ shift: Int, _ flag: Int, _ start: Int, _ f: Int) -> Int {
            func t(_ k: Int) -> Int { return s16(d, pRot + (start + k) * 2) }
            if flag == 0 { return t(0) }
            if shift == 0 { return t(f) }
            let i = f >> shift
            let b = ((frames - 1) >> shift) << shift
            if f >= b { return t(i + (f - b)) }
            let frac = f - (i << shift)
            var lo = t(i), hi = t(i + 1)
            if frac == 0 { return lo }
            let range = (1 << shift) - frac
            if abs(lo - hi) >= 0x800 {
                if lo > hi { hi += 0x1000 } else { lo += 0x1000 }
                return ((hi * frac + lo * range) >> shift) & 0xfff
            }
            return (lo * range + hi * frac) >> shift
        }

        // scale, rotation (1/4096 turns) and translation at frame f
        func sample(_ bone: Int, _ f: Int, _ scale: inout [Double], _ rot: inout [Int], _ trans: inout [Double]) {
            let o = tracks + bone * 0x24
            func sh(_ k: Int) -> Int { return Int(d[o + k * 4]) }
            func fl(_ k: Int) -> Int { return Int(d[o + k * 4 + 1]) }
            func st(_ k: Int) -> Int { return u16(d, o + k * 4 + 2) }
            var any = false
            for k in 0..<3 where st(k) != 0 || fl(k) != 0 { any = true }
            for k in 0..<3 {
                scale[k] = any ? Double(fix(pScale, sh(k), fl(k), st(k), f)) / 4096.0 : 1.0
                rot[k] = angle(sh(3 + k), fl(3 + k), st(3 + k), f)
                trans[k] = Double(fix(pTrans, sh(6 + k), fl(6 + k), st(6 + k), f)) / 4096.0
            }
        }
    }

    // A BTP texture sequence: per material name, its (first frame, texture,
    // palette) keys (func_02046bbc)
    final class Btp {
        let frames: Int
        var order: [String] = []
        var seqs: [String: [(frame: Int, tex: String, pal: String?)]] = [:]

        init(_ raw: [UInt8]) {
            let d = lz77(raw)
            frames = u16(d, 0)
            var texs: [String] = []
            for i in 0..<u16(d, 2) { texs.append(name(d, u(d, u(d, 4) + i * 8 + 4))) }
            var pals: [String] = []
            for i in 0..<u16(d, 8) { pals.append(name(d, u(d, u(d, 0xc) + i * 8 + 4))) }
            let p10 = u(d, 0x10), p14 = u(d, 0x14), p18 = u(d, 0x18)
            for i in 0..<u16(d, 0x1c) {
                let e = u(d, 0x20) + i * 0xc
                let cnt = u16(d, e + 8), off = u16(d, e + 0xa)
                var keys: [(frame: Int, tex: String, pal: String?)] = []
                for j in 0..<cnt {
                    keys.append((u16(d, p10 + (off + j) * 2), texs[u16(d, p14 + (off + j) * 2)],
                                 pals.count > 0 ? pals[u16(d, p18 + (off + j) * 2)] : nil))
                }
                let nm = name(d, u(d, e + 4))
                order.append(nm)
                seqs[nm] = keys
            }
        }
    }

    // one vertex of a display list: in the bound bone's space, with the DS's
    // own values (exact), which is what tells shared vertices apart
    struct Vtx { var pos: [Double], slot: Int, normal: [Double]?, u: Double, v: Double }

    // The geometry engine's packed commands: parameter words per command
    static let params: [Int: Int] = [
        0x00: 0, 0x10: 1, 0x11: 0, 0x12: 1, 0x13: 1, 0x14: 1, 0x15: 0,
        0x16: 16, 0x17: 12, 0x18: 16, 0x19: 12, 0x1a: 9, 0x1b: 3, 0x1c: 3,
        0x20: 1, 0x21: 1, 0x22: 1, 0x23: 2, 0x24: 1, 0x25: 1, 0x26: 1,
        0x27: 1, 0x28: 1, 0x29: 1, 0x2a: 1, 0x2b: 1, 0x30: 1, 0x31: 1,
        0x32: 1, 0x33: 1, 0x34: 32, 0x40: 1, 0x41: 0, 0x50: 1, 0x60: 1,
        0x70: 3, 0x71: 2, 0x72: 1]

    // a display list's triangles (ndsfmt.display_list)
    static func displayList(_ data: [UInt8]) throws -> [[Vtx]] {
        let words = (0..<data.count / 4).map { u32(data, $0 * 4) }
        var pos = [0.0, 0, 0]
        var slot = 0, prim = -1
        var normal: [Double]? = nil
        var uu = 0.0, vv = 0.0
        var tris: [[Vtx]] = []
        var strip: [Vtx] = []
        func emit() {
            let s = strip
            if prim == 0 {
                var i = 0
                while i + 2 < s.count { tris.append([s[i], s[i + 1], s[i + 2]]); i += 3 }
            } else if prim == 1 {
                var i = 0
                while i + 3 < s.count {
                    tris.append([s[i], s[i + 1], s[i + 2]])
                    tris.append([s[i], s[i + 2], s[i + 3]])
                    i += 4
                }
            } else if prim == 2 {
                var i = 0
                while i + 2 < s.count {
                    tris.append(i % 2 == 0 ? [s[i], s[i + 1], s[i + 2]] : [s[i + 1], s[i], s[i + 2]])
                    i += 1
                }
            } else if prim == 3 {
                var i = 0
                while i + 3 < s.count {
                    tris.append([s[i], s[i + 1], s[i + 3]])
                    tris.append([s[i], s[i + 3], s[i + 2]])
                    i += 2
                }
            }
        }
        func vertex() { strip.append(Vtx(pos: pos, slot: slot, normal: normal, u: uu, v: vv)) }
        var w = 0
        while w < words.count {
            let packed = words[w]; w += 1
            for k in 0..<4 {
                let cmd = Int((packed >> UInt32(k * 8)) & 0xff)
                guard let n = params[cmd] else { throw BuildError("The face model has an unknown command.") }
                let p0: UInt32 = n > 0 && w < words.count ? words[w] : 0
                let p1: UInt32 = n > 1 && w + 1 < words.count ? words[w + 1] : 0
                w += n
                switch cmd {
                case 0x14: slot = Int(p0 & 31)
                case 0x21: normal = [Double(sx(p0, 10)) / 512.0, Double(sx(p0 >> 10, 10)) / 512.0, Double(sx(p0 >> 20, 10)) / 512.0]
                case 0x22: uu = Double(sx(p0, 16)) / 16.0; vv = Double(sx(p0 >> 16, 16)) / 16.0
                case 0x23: pos = [Double(sx(p0, 16)) / 4096.0, Double(sx(p0 >> 16, 16)) / 4096.0, Double(sx(p1, 16)) / 4096.0]; vertex()
                case 0x24: pos = [Double(sx(p0, 10)) / 64.0, Double(sx(p0 >> 10, 10)) / 64.0, Double(sx(p0 >> 20, 10)) / 64.0]; vertex()
                case 0x25: pos[0] = Double(sx(p0, 16)) / 4096.0; pos[1] = Double(sx(p0 >> 16, 16)) / 4096.0; vertex()
                case 0x26: pos[0] = Double(sx(p0, 16)) / 4096.0; pos[2] = Double(sx(p0 >> 16, 16)) / 4096.0; vertex()
                case 0x27: pos[1] = Double(sx(p0, 16)) / 4096.0; pos[2] = Double(sx(p0 >> 16, 16)) / 4096.0; vertex()
                case 0x28:
                    pos[0] += Double(sx(p0, 10)) / 32768.0; pos[1] += Double(sx(p0 >> 10, 10)) / 32768.0; pos[2] += Double(sx(p0 >> 20, 10)) / 32768.0
                    vertex()
                case 0x40:
                    if !strip.isEmpty { emit() }
                    strip = []
                    prim = Int(p0 & 3)
                case 0x41: emit(); strip = []
                default: break
                }
            }
        }
        if !strip.isEmpty { emit() }
        return tris
    }

    // one vertex of a part, in model space at the bind pose
    struct PVtx { var pos: [Double], nrm: [Double], u: Double, v: Double, bone: Int, raw: String }
    struct Prim { var material: Int, tris: [[PVtx]] }

    final class Part {
        var pos: [[Double]] = [], nrm: [[Double]] = [], uv: [[Double]] = []
        var jnt: [Int] = [], idx: [Int] = []

        // as pycgfx stores it: floats (or the joints plus one as shorts),
        // twice over for a double-sided part, the second time with the
        // normals turned round
        func stream(_ attr: String, _ dbl: Bool) -> [UInt8] {
            var o: [UInt8] = []
            for i in 0..<pos.count {
                if attr == "jnt" {
                    for x in [jnt[i] + 1, 1, 1, 1] {
                        let s = UInt16(truncatingIfNeeded: x)
                        o.append(UInt8(s & 0xff)); o.append(UInt8(s >> 8))
                    }
                } else {
                    for x in attr == "pos" ? pos[i] : attr == "nrm" ? nrm[i] : uv[i] {
                        let b = Float(x).bitPattern
                        o += [UInt8(b & 0xff), UInt8(b >> 8 & 0xff), UInt8(b >> 16 & 0xff), UInt8(b >> 24)]
                    }
                }
            }
            if !dbl { return o }
            var r = o + o
            if attr == "nrm" {
                var i = o.count + 3
                while i < r.count { r[i] ^= 0x80; i += 4 }
            }
            return r
        }

        func indices(_ dbl: Bool) -> [UInt8] {
            var o: [UInt8] = []
            func add(_ v: Int) { let s = UInt16(truncatingIfNeeded: v); o.append(UInt8(s & 0xff)); o.append(UInt8(s >> 8)) }
            for i in idx { add(i) }
            if dbl { for k in stride(from: idx.count - 1, through: 0, by: -1) { add(idx[k] + pos.count) } }
            return o
        }
    }

    final class Blink { var textures: [String] = []; var shown: [Int] = [] }

    // One BMD with its animation (face_gltf.Model)
    final class Model {
        let bmd: Bmd
        let bca: Bca
        let btp: Btp?
        var bind: [[Double]] = []
        var prims: [Prim] = []
        var blinks: [String: Blink] = [:]
        var blinkOrder: [String] = []   // blinks' keys in the order C#'s Dictionary keeps them

        init(_ bmd: Bmd, _ bca: Bca, _ btp: Btp?) throws {
            self.bmd = bmd; self.bca = bca; self.btp = btp
            for i in 0..<bmd.bones.count {
                let b = bmd.bones[i]
                let l = local(b.scale, b.rot, b.trans)
                bind.append(b.parent >= 0 ? mul(l, bind[b.parent]) : l)
            }
            try geometry()
            if btp != nil { blinksFromSequence() }
        }

        // One primitive per (display list, material) of the root bone, at
        // the bind pose (face_gltf.Model._geometry)
        func geometry() throws {
            let s = Double(1 << bmd.scaleShift)
            let root = bmd.bones[0]
            for pi in 0..<root.mats.count {
                let mi = Int(root.mats[pi])
                let dl = bmd.dls[Int(root.dls[pi])]
                let mat = bmd.materials[mi]
                var sw = 1.0, shh = 1.0
                if mat.tex >= 0 { sw = Double(bmd.textures[mat.tex].w); shh = Double(bmd.textures[mat.tex].h) }
                let env = (mat.words[5] >> 30) == 2
                var prim = Prim(material: mi, tris: [])
                for tri in try displayList(dl.data) {
                    var vs: [PVtx] = []
                    for v in tri {
                        let bone = bmd.tmap[Int(dl.transforms[v.slot])]
                        let w = bind[bone]
                        let pos = apply([v.pos[0] * s, v.pos[1] * s, v.pos[2] * s, 1.0], w, 4)
                        var n = apply(v.normal ?? [0.0, 1.0, 0.0], w, 3)
                        var len = (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]).squareRoot()
                        if len == 0 { len = 1 }
                        n = [n[0] / len, n[1] / len, n[2] / len]
                        var uu = v.u / sw, vv = v.v / shh
                        if env { uu = 0.5 + 0.5 * n[0]; vv = 0.5 - 0.5 * n[1] }
                        var raw = ""
                        for x in v.pos { raw += String(x.bitPattern) + "," }
                        raw += String(bone) + ";"
                        if let normal = v.normal { for x in normal { raw += String(x.bitPattern) + "," } }
                        raw += ";" + String(v.u.bitPattern) + "," + String(v.v.bitPattern)
                        vs.append(PVtx(pos: pos, nrm: n, u: uu, v: vv, bone: bone, raw: raw))
                    }
                    prim.tris.append(vs)
                }
                prims.append(prim)
            }
        }

        // The eyelid pictures kept, and what each of the sequence's becomes:
        // by the picture's last digit, 1 and 2 show 0 (open), 3 shows 4
        // (closed), the rest themselves (face_gltf.LIDS)
        static func lid(_ t: String) -> String {
            guard let c = t.last else { return t }
            if c == "1" || c == "2" { return String(t.dropLast()) + "0" }
            if c == "3" { return String(t.dropLast()) + "4" }
            return t
        }

        // face_gltf.blink_variants
        func blinksFromSequence() {
            guard let btp = btp else { return }
            for mat in btp.order {
                let keys = btp.seqs[mat]!
                var kept = Set<String>()
                for k in keys where Model.lid(k.tex) == k.tex { kept.insert(k.tex) }
                let b = Blink()
                b.textures = kept.sorted { Array($0.utf16).lexicographicallyPrecedes(Array($1.utf16)) }
                for f in 0..<btp.frames {
                    var cur: (frame: Int, tex: String, pal: String?)? = nil
                    for k in keys where k.frame <= f { cur = k }
                    b.shown.append(b.textures.firstIndex(of: Model.lid(cur!.tex)) ?? -1)
                }
                if blinks[mat] == nil { blinkOrder.append(mat) }
                blinks[mat] = b
            }
        }

        // A part's vertices, shared ones once (face_gltf.build): an eyelid
        // copy or a rigid part (ROOT: the star's) in the root bone's space,
        // anything else in model space with its joints numbered among the
        // bones the part uses
        func part(_ pi: Int, _ variant: Int, _ root: Bool) -> Part {
            let prim = prims[pi]
            let lid = root || blinks[bmd.materials[prim.material].name] != nil
            let inv0 = invert(bind[0])
            var used = Set<Int>()
            for t in prim.tris { for v in t { used.insert(v.bone) } }
            let usedList = used.sorted()
            let part = Part()
            var seen: [String: Int] = [:]
            for t in prim.tris {
                for v in t {
                    let joint = lid ? 0 : usedList.firstIndex(of: v.bone)!
                    let key = v.raw + "|" + String(joint)
                    var at: Int
                    if let s = seen[key] {
                        at = s
                    } else {
                        at = part.pos.count
                        seen[key] = at
                        if lid {
                            part.pos.append(apply([v.pos[0], v.pos[1], v.pos[2], 1.0], inv0, 4))
                            part.nrm.append(apply(v.nrm, inv0, 3))
                        } else {
                            part.pos.append(v.pos)
                            part.nrm.append(v.nrm)
                        }
                        part.uv.append([v.u, v.v])
                        part.jnt.append(joint)
                    }
                    part.idx.append(at)
                }
            }
            return part
        }

        // Per "bone/channel/axis", the value at each window key
        // (face_recipe.bone_curves): rotations straight from the DS angles
        // (the DS's XYZ order is CGFX's), crossfaded, then unwound so
        // neighbouring keys never differ by more than half a turn
        func boneCurves(_ keys: [Key]) -> [String: [Double]] {
            var o: [String: [Double]] = [:]
            let n = bca.frames
            let channels = ["scale", "rot", "pos"]
            for bone in 0..<bmd.bones.count {
                var vals = [[Double]](repeating: [Double](repeating: 0, count: keys.count), count: 9)
                var sa = [0.0, 0, 0], ta = [0.0, 0, 0], sb = [0.0, 0, 0], tb = [0.0, 0, 0]
                var ra = [0, 0, 0], rb = [0, 0, 0]
                for i in 0..<keys.count {
                    let k = keys[i]
                    bca.sample(bone, k.a % n, &sa, &ra, &ta)
                    bca.sample(bone, k.b % n, &sb, &rb, &tb)
                    let t = k.t
                    for ax in 0..<3 {
                        let y = Double(rb[ax]) + 4096 * (Double(ra[ax] - rb[ax]) / 4096.0).rounded(.toNearestOrEven)
                        vals[ax][i] = sa[ax] * (1 - t) + sb[ax] * t
                        vals[3 + ax][i] = (Double(ra[ax]) * (1 - t) + y * t) * turn
                        vals[6 + ax][i] = ta[ax] * (1 - t) + tb[ax] * t
                    }
                }
                for ax in 0..<3 {
                    for i in 1..<max(1, keys.count) {
                        vals[3 + ax][i] += 2 * Double.pi * ((vals[3 + ax][i - 1] - vals[3 + ax][i]) / (2 * Double.pi)).rounded(.toNearestOrEven)
                    }
                }
                for c in 0..<3 {
                    for ax in 0..<3 { o[String(bone) + "/" + channels[c] + "/" + String(ax)] = vals[c * 3 + ax] }
                }
            }
            return o
        }

        // The eyelid copies' STEP curves, "side/variant/channel/axis": a copy
        // showing sits in place at full size, otherwise inside the head at
        // half size (face_recipe.lid_curves)
        func lidCurves(_ keys: [Key]) -> [String: [Double]] {
            var o: [String: [Double]] = [:]
            let inv0 = invert(bind[0])
            func mean(_ which: (Prim) -> Bool) -> [Double] {
                var sum = [0.0, 0, 0]
                var count = 0
                for p in prims where which(p) {
                    for t in p.tris {
                        for v in t {
                            let l = apply([v.pos[0], v.pos[1], v.pos[2], 1.0], inv0, 4)
                            for k in 0..<3 { sum[k] += l[k] }
                            count += 1
                        }
                    }
                }
                return [sum[0] / Double(count), sum[1] / Double(count), sum[2] / Double(count)]
            }
            let centre = mean { _ in true }
            for name in blinkOrder {
                let mid = mean { self.bmd.materials[$0.material].name == name }
                let b = blinks[name]!
                let side = String(name.suffix(1))
                for variant in 0..<b.textures.count {
                    var pos = [[Double]](repeating: [Double](repeating: 0, count: keys.count), count: 3)
                    var scale = [Double](repeating: 0, count: keys.count)
                    for i in 0..<keys.count {
                        let k = keys[i]
                        let on = b.shown[(k.t < 0.5 ? k.a : k.b) % b.shown.count] == variant
                        for ax in 0..<3 { pos[ax][i] = on ? 0 : centre[ax] - 0.5 * mid[ax] }
                        scale[i] = on ? 1 : 0.5
                    }
                    for ax in 0..<3 {
                        o[side + "/" + String(variant) + "/pos/" + String(ax)] = pos[ax]
                        o[side + "/" + String(variant) + "/scale/" + String(ax)] = scale
                    }
                }
            }
            return o
        }
    }

    // RGBA (top row first) as a 3DS texture: bottom row first, in 8x8 tiles
    // of Morton-ordered pixels; format 3 RGB565, 2 RGBA5551 (pycgfx's
    // swizzler, at the formats face_cgfx.py picks)
    static func swizzle(_ rgba: [UInt8], _ w: Int, _ h: Int, _ format: Int) -> [UInt8] {
        var o = [UInt8](repeating: 0, count: w * h * 2)
        var at = 0
        for ty in 0..<h / 8 {
            for tx in 0..<w / 8 {
                for y in 0..<8 {
                    for x in 0..<8 {
                        let src = ((h - 1 - (ty * 8 + y)) * w + tx * 8 + x) * 4
                        let r = Int(rgba[src]), g = Int(rgba[src + 1]), b = Int(rgba[src + 2]), a = Int(rgba[src + 3])
                        let px = format == 3 ? (b >> 3) | ((g & 0xfc) << 3) | ((r & 0xf8) << 8)
                                             : (a >> 7) | ((b & 0xf8) >> 2) | ((g & 0xf8) << 3) | ((r & 0xf8) << 8)
                        var m = 0
                        for bit in 0..<3 { m |= ((x >> bit) & 1) << (bit * 2) | ((y >> bit) & 1) << (bit * 2 + 1) }
                        o[at + m * 2] = UInt8(px & 0xff)
                        o[at + m * 2 + 1] = UInt8((px >> 8) & 0xff)
                    }
                }
                at += 128
            }
        }
        return o
    }
}
