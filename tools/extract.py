#!/usr/bin/env python3
"""Extract Muscle March (USA, WiiWare) from the user's own WAD.

The WAD's contents are AES-encrypted with a title key that is itself encrypted
with the Wii common key. We don't ship or download that key. Instead, Dolphin
installs the WAD into a NAND folder (decrypting it) and this script takes the
decrypted contents from there, checks every one against the SHA-1 in the TMD,
and unpacks them:

  extracted/contents/NN.app   decrypted contents, as listed in the TMD
  extracted/main.dol          the game DOL (content 1, LZ11-decompressed)
  extracted/loader.dol        the NAND boot program (the boot content)
  extracted/files/<NN>/...    the U8 archives, unpacked (recursively)

Usage:
  tools/extract.py [--wad FILE] [--nand DIR] [--out DIR]

--nand defaults to ./dolphin_user/Wii, which `make nand` fills by running
Dolphin once with that user folder (booting a WAD installs it).
"""
import argparse
import hashlib
import os
import shutil
import struct
import sys

WAD_SHA1 = "ed99ce114d9793dbf21d5c9589d7947afebd4259"
TITLE_ID = "00010001574d4d45"  # WMME


def align(x, a=0x40):
    return (x + a - 1) & ~(a - 1)


def parse_wad(data):
    hs, typ, ver, cert_sz, _, tik_sz, tmd_sz, data_sz, foot_sz = struct.unpack(">IHHIIIIII", data[:0x20])
    o = align(hs)
    o = align(o + cert_sz)
    tik = data[o:o + tik_sz]
    o = align(o + tik_sz)
    tmd = data[o:o + tmd_sz]
    o = align(o + tmd_sz)
    footer = data[align(o + data_sz):align(o + data_sz) + foot_sz]
    return tik, tmd, footer


def parse_tmd(tmd):
    t = tmd[0x140:]
    info = {
        "title_id": t[0x4c:0x54].hex(),
        "ios": t[0x44:0x4c].hex(),
        "title_version": struct.unpack(">H", t[0x9c:0x9e])[0],
        "boot_index": struct.unpack(">H", t[0xa0:0xa2])[0],
        "contents": [],
    }
    n = struct.unpack(">H", t[0x9e:0xa0])[0]
    for i in range(n):
        e = t[0xa4 + 36 * i:0xa4 + 36 * (i + 1)]
        cid, idx, typ, size = struct.unpack(">IHHQ", e[:16])
        info["contents"].append({"cid": cid, "index": idx, "type": typ, "size": size, "sha1": e[16:36].hex()})
    return info


def shared_map(nand):
    """content.map: 8-byte ascii name + 20-byte SHA-1 per entry."""
    m = {}
    path = os.path.join(nand, "shared1", "content.map")
    if os.path.exists(path):
        raw = open(path, "rb").read()
        for i in range(0, len(raw) - 27, 28):
            m[raw[i + 8:i + 28].hex()] = os.path.join(nand, "shared1", raw[i:i + 8].decode() + ".app")
    return m


# --- compression ------------------------------------------------------------

def lz_decompress(data):
    """Nintendo LZ10 (0x10) / LZ11 (0x11)."""
    kind = data[0]
    size = data[1] | data[2] << 8 | data[3] << 16
    pos = 4
    if size == 0:
        size = struct.unpack("<I", data[4:8])[0]
        pos = 8
    out = bytearray()
    while len(out) < size:
        flags = data[pos]
        pos += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if not flags & (0x80 >> bit):
                out.append(data[pos])
                pos += 1
                continue
            if kind == 0x10:
                b0, b1 = data[pos], data[pos + 1]
                pos += 2
                n = (b0 >> 4) + 3
                disp = ((b0 & 0xF) << 8 | b1) + 1
            else:
                b0 = data[pos]
                ind = b0 >> 4
                if ind == 0:
                    b1, b2 = data[pos + 1], data[pos + 2]
                    pos += 3
                    n = ((b0 & 0xF) << 4 | b1 >> 4) + 0x11
                    disp = ((b1 & 0xF) << 8 | b2) + 1
                elif ind == 1:
                    b1, b2, b3 = data[pos + 1:pos + 4]
                    pos += 4
                    n = ((b0 & 0xF) << 12 | b1 << 4 | b2 >> 4) + 0x111
                    disp = ((b2 & 0xF) << 8 | b3) + 1
                else:
                    b1 = data[pos + 1]
                    pos += 2
                    n = ind + 1
                    disp = ((b0 & 0xF) << 8 | b1) + 1
            for _ in range(n):
                out.append(out[-disp])
    return bytes(out[:size])


def yaz0_decompress(data):
    size = struct.unpack(">I", data[4:8])[0]
    pos, out = 16, bytearray()
    while len(out) < size:
        flags = data[pos]
        pos += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> bit):
                out.append(data[pos])
                pos += 1
            else:
                b0, b1 = data[pos], data[pos + 1]
                pos += 2
                disp = ((b0 & 0xF) << 8 | b1) + 1
                n = b0 >> 4
                if n == 0:
                    n = data[pos] + 0x12
                    pos += 1
                else:
                    n += 2
                for _ in range(n):
                    out.append(out[-disp])
    return bytes(out)


def looks_lz(data):
    if len(data) < 8 or data[0] not in (0x10, 0x11):
        return False
    size = data[1] | data[2] << 8 | data[3] << 16
    return size == 0 or len(data) < size * 9 // 8 + 16 and size >= len(data) // 2


# --- U8 -------------------------------------------------------------------

def u8_extract(data, outdir):
    root_off, hdr_sz, data_off = struct.unpack(">III", data[4:16])
    total = struct.unpack(">I", data[root_off + 8:root_off + 12])[0]
    strtab = root_off + total * 12
    names = []

    def name_at(off):
        end = data.index(b"\0", strtab + off)
        return data[strtab + off:end].decode("shift_jis", "replace")

    files = []
    stack = [(total, outdir)]  # (end index, dir)
    for i in range(1, total):
        while stack and i >= stack[-1][0]:
            stack.pop()
        e = data[root_off + 12 * i:root_off + 12 * (i + 1)]
        typ = e[0]
        noff = int.from_bytes(e[1:4], "big")
        a, b = struct.unpack(">II", e[4:12])
        path = os.path.join(stack[-1][1], name_at(noff))
        if typ == 1:
            os.makedirs(path, exist_ok=True)
            stack.append((b, path))
        else:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, "wb").write(data[a:a + b])
            files.append(path)
    return files


def unpack_tree(paths, log):
    """Recursively unpack U8 / Yaz0 / LZ files found under extracted dirs."""
    queue = list(paths)
    while queue:
        p = queue.pop()
        d = open(p, "rb").read()
        if d[:4] == b"Yaz0":
            d = yaz0_decompress(d)
            p2 = p + ".dec"
            open(p2, "wb").write(d)
            log.append(f"  yaz0 {p}")
            queue.append(p2)
        elif d[:4] == b"\x55\xAA\x38\x2D":
            dest = p + ".d"
            queue.extend(u8_extract(d, dest))
            log.append(f"  u8   {p}")
        elif looks_lz(d):
            try:
                dec = lz_decompress(d)
            except IndexError:
                continue
            open(p + ".dec", "wb").write(dec)
            log.append(f"  lz{d[0]:x} {p}")
            queue.append(p + ".dec")


# --- DOL ------------------------------------------------------------------

def dol_info(d):
    offs = struct.unpack(">18I", d[0:0x48])
    addrs = struct.unpack(">18I", d[0x48:0x90])
    sizes = struct.unpack(">18I", d[0x90:0xd8])
    bss_addr, bss_size, entry = struct.unpack(">III", d[0xd8:0xe4])
    secs = []
    for i in range(18):
        if sizes[i]:
            secs.append(("text%d" % i if i < 7 else "data%d" % (i - 7), offs[i], addrs[i], sizes[i]))
    return secs, bss_addr, bss_size, entry


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wad", default="Muscle March (USA) (WiiWare).wad")
    ap.add_argument("--nand", default="dolphin_user/Wii")
    ap.add_argument("--out", default="extracted")
    args = ap.parse_args()

    wad = open(args.wad, "rb").read()
    h = hashlib.sha1(wad).hexdigest()
    if h != WAD_SHA1:
        print(f"warning: WAD SHA-1 {h} != expected {WAD_SHA1}", file=sys.stderr)
    tik, tmd, footer = parse_wad(wad)
    info = parse_tmd(tmd)
    if info["title_id"] != TITLE_ID:
        sys.exit(f"unexpected title id {info['title_id']}")

    title_dir = os.path.join(args.nand, "title", TITLE_ID[:8], TITLE_ID[8:], "content")
    smap = shared_map(args.nand)
    cdir = os.path.join(args.out, "contents")
    os.makedirs(cdir, exist_ok=True)
    open(os.path.join(args.out, "title.tmd"), "wb").write(tmd)
    open(os.path.join(args.out, "footer.bin"), "wb").write(footer)

    print(f"title {info['title_id']} (WMME)  IOS{int(info['ios'][8:], 16)}  v{info['title_version']}  boot content {info['boot_index']}")
    contents = {}
    for c in info["contents"]:
        if c["type"] & 0x8000:
            src = smap.get(c["sha1"])
        else:
            src = os.path.join(title_dir, "%08x.app" % c["cid"])
        if not src or not os.path.exists(src):
            sys.exit(f"content {c['index']} missing from NAND ({src}); run `make nand` first")
        d = open(src, "rb").read()[:c["size"]]
        if hashlib.sha1(d).hexdigest() != c["sha1"]:
            sys.exit(f"content {c['index']}: SHA-1 mismatch")
        dst = os.path.join(cdir, "%02d.app" % c["index"])
        open(dst, "wb").write(d)
        contents[c["index"]] = d
        kind = "shared" if c["type"] & 0x8000 else "normal"
        print(f"  {c['index']:2d} cid={c['cid']:08x} {kind:6s} {c['size']:>9d}  {d[:4].hex()}  ok")

    log = []
    shutil.copyfile(os.path.join(cdir, "%02d.app" % info["boot_index"]), os.path.join(args.out, "loader.dol"))
    for idx, d in sorted(contents.items()):
        src = os.path.join(cdir, "%02d.app" % idx)
        if d[:4] == b"\x55\xAA\x38\x2D":
            unpack_tree([src], log)
        elif looks_lz(d):
            dec = lz_decompress(d)
            if struct.unpack(">I", dec[:4])[0] == 0x100:
                open(os.path.join(args.out, "main.dol"), "wb").write(dec)
                log.append(f"  lz{d[0]:x} content {idx} -> main.dol ({len(dec)} bytes)")
    # move unpacked trees under files/
    fdir = os.path.join(args.out, "files")
    os.makedirs(fdir, exist_ok=True)
    for name in sorted(os.listdir(cdir)):
        if name.endswith(".app.d"):
            dst = os.path.join(fdir, name[:2])
            shutil.rmtree(dst, ignore_errors=True)
            shutil.move(os.path.join(cdir, name), dst)
    print("\n".join(log[:20]) + (f"\n  ... {len(log) - 20} more" if len(log) > 20 else ""))

    for name in ("loader.dol", "main.dol"):
        p = os.path.join(args.out, name)
        if not os.path.exists(p):
            continue
        secs, bss_a, bss_s, entry = dol_info(open(p, "rb").read())
        print(f"{name}: entry {entry:08x}  bss {bss_a:08x}+{bss_s:x}")
        for s in secs:
            print(f"    {s[0]:6s} {s[2]:08x}-{s[2] + s[3]:08x} ({s[3]:#x})")


if __name__ == "__main__":
    main()
