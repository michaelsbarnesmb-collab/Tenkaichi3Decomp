#!/usr/bin/env python3
"""Takes the Latin-American Spanish voices from the user's own disc image of the fan patch SLUS-21978
(Beta 14 Rev 2: serial SLUS_219.78) for the PC build. Optional: without that image nothing of it is used.

    port/tools/extract_slus21978.py <SLUS-21978 image> [data_dir]        (data_dir default: gamedata/, which must hold the BT3 data)

Result: data_dir/slus21978/pzs3us2/NNNNN.bin (voice lines) and data_dir/slus21978/pzs3us1/NNNNN.bin (voice banks), one file
per file the patch has in Spanish, named as the file of the original's English it replaces, and data_dir/slus21978/source.txt.
The game plays those instead when the voice language "Spanish (Latin America)" is chosen (port/src/gs/snd_adx.c).
Also data_dir/slus21978/songs/: the patch's music, added to the music select whatever the voice language.
And data_dir/characters/SLUS-21978 <name>/: the characters the patch adds, as character packages
(port/tools/slus21978_chars.py; --chars=<c>,<c>... takes only those).

How the patch keeps its data (worked out from its disc, nothing documented):
- Its four archives DATA/PZS4US0..3.AFS have no AFS header: the tables of contents of all four are in BIN/DBZ4.BIN,
  one record per archive (u32 0, u32 record length - 8, u32 count, u32 count, the archive's name, then one u16 per
  file: its length in 2048-byte sectors; the files follow each other from the archive's start).
- Its program maps a file id to (archive, index) in a function of its own: ids 1..0xD47 -> PZS4US1 (index id - 1),
  0xD48..0x10CBF -> PZS4US2 (id - 0xD48), 0x10CC0..0x1FFFF -> PZS4US0 (id - 0x10CC0), 0x20000.. -> PZS4US3.
- The menus' voice lines (ids 0x87F2..0x8D4D, English in the original) are Spanish at the same ids.
- A fighter's voice bank (shouts, hits) is 0x1D012 (Japanese) or 0x1D10C (Spanish) + character, in PZS4US0;
  the announcer's 0x38 lines are ids 0x939 (Japanese) or 0x971 (Spanish), in PZS4US1.
- A fighter's line `line` of character `c` is id 0x10CC2 + c * 100 + line (Japanese, the original's 0x8D4E set:
  the same files) or 0x16E6A + c * 100 + line (Spanish, where the original has English): both in PZS4US0.
- Some characters have other numbers than in the original. Each is found by its Japanese lines, which are the
  original's files: pairs (original, patch) by how many lines are identical, the strongest first, each character
  of the patch taken once (forms of one fighter share lines; a character keeps its own number on a tie)."""
import hashlib, pathlib, struct, sys

SECTOR = 2048
LINES = 100
CHARAS = 161              # the original's characters
PATCH_CHARAS = 250          # room the patch keeps for each voice set (25000 ids)
PATCH_JP = 0x10CC2 - 0x10CC0   # index in PZS4US0 of character 0, line 0
PATCH_ES = 0x16E6A - 0x10CC0
BT3_JP = 0x8D4E - 0xD48      # index in pzs3us2 (file number) of the original's Japanese and English sets
BT3_EN = 0xCC32 - 0xD48
GUIDE_EN = (0x87F2, 0x8D4E)  # the menus' voice lines, English (ids)


class Iso:
    """Reads files of an ISO 9660 image in place (offsets in the image, no extraction)."""
    def __init__(self, path):
        self.f = open(path, "rb")
        root = self.sectors(16, 1)[156:156 + 34]
        self.root = (struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0])

    def sectors(self, lba, count):
        self.f.seek(lba * SECTOR)
        return self.f.read(count * SECTOR)

    def entries(self, lba, size):
        data, i, out = self.sectors(lba, (size + SECTOR - 1) // SECTOR), 0, {}
        while i < len(data):
            n = data[i]
            if n == 0:
                i = (i // SECTOR + 1) * SECTOR
                continue
            rec = data[i:i + n]
            name = rec[33:33 + rec[32]].decode("latin1").split(";")[0]
            out[name] = (struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0])
            i += n
        return out

    def find(self, path):
        lba, size = self.root
        for part in path.split("/"):
            found = self.entries(lba, size).get(part)
            if found is None:
                return None
            lba, size = found
        return lba, size

    def read(self, path):
        lba, size = self.find(path)
        return self.sectors(lba, (size + SECTOR - 1) // SECTOR)[:size]


def tables(dbz4):
    """{archive name: [(offset, sectors), ...]} from BIN/DBZ4.BIN. (The records are not simply one after another:
    each is found by its name, which is 16 bytes into it.)"""
    out = {}
    for n in range(4):
        name = b"pzs4us%d.afs" % n
        pos = dbz4.find(name) - 16
        if pos < 0:
            continue
        _, length, count, _ = struct.unpack_from("<4I", dbz4, pos)
        sizes = struct.unpack_from(f"<{count}H", dbz4, pos + length - 2 * count)
        offset, files = 0, []
        for s in sizes:
            files.append((offset, s))
            offset += s * SECTOR
        out[name.decode()] = files
    return out


# ---- the song names: pictures in the character select's resources (the patch's menu archive, file 0x1C1) ----

def bpe_decode(src):
    """The game's BPE decompression (src/sys/bpe.c): u32 size, u32 packed size, then blocks of pair tables."""
    size, packed = struct.unpack_from("<II", src, 0)
    i, end, out = 8, 8 + packed, bytearray()
    while i != end:
        left, right = list(range(256)), [0] * 256
        count, i, c = src[i], i + 1, 0
        while True:
            if count > 127:
                c, count = c + count - 127, 0
            if c == 256:
                break
            for _ in range(count + 1):
                left[c], i = src[i], i + 1
                if c != left[c]:
                    right[c], i = src[i], i + 1
                c += 1
            if c == 256:
                break
            count, i = src[i], i + 1
        block, i = src[i] * 256 + src[i + 1], i + 2
        stack = []
        while True:
            if stack:
                c = stack.pop()
            elif block == 0:
                break
            else:
                block, c, i = block - 1, src[i], i + 1
            if c == left[c]:
                out.append(c)
            else:
                stack += (right[c], left[c])
    return bytes(out)


def pack_at(pack, n):
    """MPACK_AT: the n-th part of a pack (offsets in its header, from its start)."""
    return (struct.unpack_from("<I", pack, 4 * n)[0] >> 2) << 2


# GS memory order (port/src/gs/gs_core.c): the game uploads each name sheet as a 32-bit image and the GS reads it as an
# 8-bit or a 4-bit texture.
BLOCK32 = [[0, 1, 4, 5, 16, 17, 20, 21], [2, 3, 6, 7, 18, 19, 22, 23], [8, 9, 12, 13, 24, 25, 28, 29], [10, 11, 14, 15, 26, 27, 30, 31]]
BLOCK16 = [[0, 2, 8, 10], [1, 3, 9, 11], [4, 6, 12, 14], [5, 7, 13, 15], [16, 18, 24, 26], [17, 19, 25, 27], [20, 22, 28, 30], [21, 23, 29, 31]]
COL32 = [[0, 1, 4, 5, 8, 9, 12, 13], [2, 3, 6, 7, 10, 11, 14, 15], [16, 17, 20, 21, 24, 25, 28, 29], [18, 19, 22, 23, 26, 27, 30, 31],
         [32, 33, 36, 37, 40, 41, 44, 45], [34, 35, 38, 39, 42, 43, 46, 47], [48, 49, 52, 53, 56, 57, 60, 61], [50, 51, 54, 55, 58, 59, 62, 63]]
COL8 = [[0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54], [8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62],
        [33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23], [41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31]]
COL4 = [[0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106, 4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110],
        [16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122, 20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126],
        [65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43, 69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47],
        [81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59, 85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63]]
_tables = {}


def gs_addresses(kind, bw, w, h):
    """For every pixel of a w x h image in a buffer `bw` (64-pixel units) wide: its place in GS memory, in pixels of
    its own size (kind 32, 8 or 4)."""
    key = (kind, bw, w, h)
    if key not in _tables:
        out = []
        for y in range(h):
            for x in range(w):
                if kind == 32:
                    a = (((y & ~0x1F) * bw + ((x >> 1) & ~0x1F) + BLOCK32[(y >> 3) & 3][(x >> 3) & 7]) << 6) + COL32[y & 7][x & 7]
                else:
                    col = (y & 15) >> 2
                    sx = x ^ 4 if col & 1 else x
                    if kind == 8:
                        a = ((((y >> 1) & ~0x1F) * (bw >> 1) + ((x >> 2) & ~0x1F) + BLOCK32[(y >> 4) & 3][(x >> 4) & 7]) << 8) + \
                            COL8[y & 3][sx & 15] + col * 64
                    else:
                        a = ((((y >> 2) & ~0x1F) * (bw >> 1) + ((x >> 2) & ~0x1F) + BLOCK16[(y >> 4) & 7][(x >> 5) & 3]) << 9) + \
                            COL4[y & 3][sx & 31] + col * 128
                out.append(a)
        _tables[key] = out
    return _tables[key]


def name_sheets(res, part):
    """The 512x256 sheets of a texture file of the resources: {index: RGBA bytes}, through GS memory as the game
    uploads them (a 32-bit image) and the GS reads them (8 or 4 bits through a table of 32-bit colours)."""
    base = pack_at(res, part)
    count, ent = struct.unpack_from("<II", res, base)
    sheets = {}
    for k in range(count):
        e = base + ent * 4 + k * 0x40
        pix_ofs, clut_ofs, pix_size = struct.unpack_from("<IIi", res, e)
        pix_blt = struct.unpack_from("<I", res, e + 0x18)[0]
        tex0 = struct.unpack_from("<Q", res, e + 0x30)[0]
        psm, tbw = (tex0 >> 20) & 0x3F, (tex0 >> 14) & 0x3F
        w, h = 1 << ((tex0 >> 26) & 0xF), 1 << ((tex0 >> 30) & 0xF)
        if (w, h) != (512, 256) or psm not in (0x13, 0x14) or (pix_blt >> 8) & 0x3F:
            continue
        packet = res[base + pix_ofs * 4:base + pix_ofs * 4 + pix_size]
        uw, uh = struct.unpack_from("<II", packet, 0x30)
        mem = bytearray(4 << 20)
        for i, a in enumerate(gs_addresses(32, pix_blt & 0x3F, uw, uh)):
            mem[a * 4:a * 4 + 4] = packet[0x60 + i * 4:0x64 + i * 4]
        clut = res[base + clut_ofs * 4 + 0x60:base + clut_ofs * 4 + 0x60 + 0x400]
        out = bytearray(w * h * 4)
        for i, a in enumerate(gs_addresses(8 if psm == 0x13 else 4, tbw, w, h)):
            if psm == 0x13:
                c = mem[a]
                c = (c & 0xE7) | ((c & 8) << 1) | ((c & 0x10) >> 1)  # the table's order: 8..15 and 16..23 change places
            else:
                c = (mem[a >> 1] >> ((a & 1) * 4)) & 15
            out[i * 4:i * 4 + 4] = clut[c * 4:c * 4 + 4]
        out[3::4] = bytes(min(255, v * 2) for v in out[3::4])  # the GS's alpha: 0x80 is opaque, the overlay's 0xFF
        sheets[k] = bytes(out)
    return sheets


CHARS = None  # --chars=<c>,<c>...: only these of the added characters (default: all)


def character_tables(res):
    """The character select's names, grid and chips, from its resources (section 2 of the menu archive 0x1C1)."""
    text = pack_at(res, 43)
    names = {}
    for c in range(struct.unpack_from("<I", res, text)[0]):
        p = text + (struct.unpack_from("<I", res, text + 4 * (c + 1))[0] >> 2 << 2)
        end = p
        while struct.unpack_from("<H", res, end)[0]:
            end += 2
        names[c] = res[p:end].decode("utf-16-le").lstrip("\ufeff")
    cells = pack_at(res, 42)
    grid = []
    for i in range(struct.unpack_from("<I", res, cells)[0]):
        cid, count, *forms = struct.unpack_from("<9i", res, cells + 0x10 + i * 0x24)
        grid.append(list(forms[:count]) if count else [cid])
    chip = pack_at(res, 45)
    n = struct.unpack_from("<I", res, chip)[0]
    offsets = [chip + (struct.unpack_from("<I", res, chip + 4 * (i + 1))[0] >> 2 << 2) for i in range(n)] + [pack_at(res, 46)]
    chips = {c: res[offsets[c]:offsets[c + 1]] for c in range(n)}
    return names, grid, chips


def main():
    global CHARS
    for a in [a for a in sys.argv[1:] if a.startswith("--chars=")]:
        CHARS = a[len("--chars="):]
        sys.argv.remove(a)
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    iso = Iso(sys.argv[1])
    data = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else "gamedata")
    if iso.find("SLUS_219.78") is None or iso.find("BIN/DBZ4.BIN") is None:
        raise SystemExit(f"{sys.argv[1]}: not the patch SLUS-21978 (no SLUS_219.78 / BIN/DBZ4.BIN)")
    if not (data / "pzs3us2").is_dir():
        raise SystemExit(f"{data}/pzs3us2: the original's data is needed first (install.py)")
    toc = tables(iso.read("BIN/DBZ4.BIN"))
    afs = iso.find("DATA/PZS4US0.AFS")
    files = toc["pzs4us0.afs"]

    def patch0(index):
        offset, sectors = files[index]
        iso.f.seek(afs[0] * SECTOR + offset)
        return iso.f.read(sectors * SECTOR)

    def is_adx(b):
        """An ADX file: its header ends in "(c)CRI" just before the data (far into the file for the music)."""
        if len(b) < 0x20 or b[:2] != b"\x80\x00":
            return False
        start = struct.unpack_from(">H", b, 2)[0] + 4
        return b[start - 6:start] == b"(c)CRI"

    def is_voice(b):
        """An ADX file with sound in it: where the patch has no Spanish recording of a fighter's line it keeps a
        silent file of the same id, and in a fight the original's English is better than a mute fighter."""
        if not is_adx(b):
            return False
        start = struct.unpack_from(">H", b, 2)[0] + 4
        channels, samples = b[7], struct.unpack_from(">I", b, 12)[0]
        data = b[start:start + (samples + 31) // 32 * 18 * channels]
        return any(data[i + 2:i + 18].strip(b"\0") for i in range(0, len(data), 18))  # 18-byte frames: scale, nibbles

    # the patch's number of each original character, by their Japanese lines
    print("matching the characters by their Japanese lines ...")
    theirs = {}
    for c in range(PATCH_CHARAS):
        for line in range(LINES):
            index = PATCH_JP + c * LINES + line
            if index < len(files) and files[index][1]:
                theirs.setdefault(hashlib.md5(patch0(index).rstrip(b"\0")).digest(), []).append((c, line))
    # only the lines that are this character's own (a file no other character of the original has): the forms of a
    # fighter share their shouts and hits
    ours, count = {}, {}
    for c in range(CHARAS):
        for line in range(LINES):
            p = data / "pzs3us2" / f"{BT3_JP + c * LINES + line:05d}.bin"
            if p.exists():
                h = hashlib.md5(p.read_bytes().rstrip(b"\0")).digest()
                ours[(c, line)] = h
                count[h] = count.get(h, 0) + 1
    votes = {}
    for (c, line), h in ours.items():
        if count[h] != 1:
            continue
        for c2, line2 in theirs.get(h, []):
            votes[(c, c2)] = votes.get((c, c2), 0) + 1
    # one to one, the strongest pairs first (forms of one fighter share many lines: a character keeps its own number
    # when that is as strong as any other)
    mapping, taken = {}, set()
    for (c, c2), n in sorted(votes.items(), key=lambda kv: (-kv[1], kv[0][1] != kv[0][0], abs(kv[0][1] - kv[0][0]))):
        if c not in mapping and c2 not in taken:
            mapping[c] = c2
            taken.add(c2)
    # no line of its own matched: the same with all its lines, then its own number
    shared = {}
    for (c, line), h in ours.items():
        if c not in mapping:
            for c2, line2 in theirs.get(h, []):
                if line2 == line and c2 not in taken:
                    shared[(c, c2)] = shared.get((c, c2), 0) + 1
    for (c, c2), n in sorted(shared.items(), key=lambda kv: (-kv[1], kv[0][1] != kv[0][0])):
        if c not in mapping and c2 not in taken:
            mapping[c] = c2
            taken.add(c2)
            print(f"  character {c}: by its shared lines ({n}), {c2} in the patch")
    for c in range(CHARAS):
        if c not in mapping and c not in taken:
            mapping[c] = c
            print(f"  character {c}: not found in the patch, kept as {c}")
    moved = {c: m for c, m in mapping.items() if m != c}
    print(f"  {len(mapping)} of {CHARAS} characters found, {len(moved)} with another number in the patch")

    out = data / "slus21978" / "pzs3us2"
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("*.bin"):  # (a file an earlier version took and this one leaves out)
        old.unlink()
    written = 0
    for c, c2 in sorted(mapping.items()):
        for line in range(LINES):
            b = patch0(PATCH_ES + c2 * LINES + line)
            if is_voice(b):
                (out / f"{BT3_EN + c * LINES + line:05d}.bin").write_bytes(b)
                written += 1
    # the menus' voice (guides, the character select's names, ...): ids 0x87F2..0x8D4D are English in the original
    # (0x55C lower: Japanese) and in the patch Spanish, at the same ids (PZS4US2, index id - 0xD48). Its silent ones
    # are taken as well: the patch leaves most guides mute (their lines about each menu's items, in English and in
    # Japanese alike), and with Spanish chosen the menus are as in the patch rather than half in English.
    guides = toc["pzs4us2.afs"]
    afs2 = iso.find("DATA/PZS4US2.AFS")
    for fid in range(GUIDE_EN[0] - 0xD48, GUIDE_EN[1] - 0xD48):
        offset, sectors = guides[fid]
        iso.f.seek(afs2[0] * SECTOR + offset)
        b = iso.f.read(sectors * SECTOR)
        ours = data / "pzs3us2" / f"{fid:05d}.bin"
        if is_adx(b) and not (ours.exists() and ours.read_bytes().rstrip(b"\0") == b.rstrip(b"\0")):
            (out / f"{fid:05d}.bin").write_bytes(b)
            written += 1
    # the fighters' voice banks (shouts, hits): the patch's are files 0x1D012 (Japanese) / 0x1D10C (Spanish) + its
    # character, in PZS4US0; the original's 0xBDA (Japanese) / 0xC7B (English) + character, partition 1
    banks = data / "slus21978" / "pzs3us1"
    banks.mkdir(parents=True, exist_ok=True)
    same = 0
    for c, c2 in sorted(mapping.items()):
        jp = data / "pzs3us1" / f"{0xBDA - 1 + c:05d}.bin"
        same += jp.exists() and jp.read_bytes().rstrip(b"\0") == patch0(0x1D012 - 0x10CC0 + c2).rstrip(b"\0")
        b = patch0(0x1D10C - 0x10CC0 + c2)
        if len(b.rstrip(b"\0")) > 0x50:
            (banks / f"{0xC7B - 1 + c:05d}.bin").write_bytes(b.rstrip(b"\0"))
            written += 1
    print(f"  voice banks: {same} of {len(mapping)} Japanese banks are the original's (a check of the characters' numbers)")
    # the announcer: 0x38 lines, the patch's at ids 0x939 (Japanese) / 0x971 (Spanish) of PZS4US1 (index id - 1), the
    # original's at 0x10B34 / 0x10B6C (English)
    ann = toc["pzs4us1.afs"]
    afs1 = iso.find("DATA/PZS4US1.AFS")
    def patch1(index):
        offset, sectors = ann[index]
        iso.f.seek(afs1[0] * SECTOR + offset)
        return iso.f.read(sectors * SECTOR)
    same = 0
    for k in range(0x38):
        jp = data / "pzs3us2" / f"{0x10B34 - 0xD48 + k:05d}.bin"
        same += jp.exists() and jp.read_bytes().rstrip(b"\0") == patch1(0x939 - 1 + k).rstrip(b"\0")
        b = patch1(0x971 - 1 + k)
        if is_voice(b):
            (out / f"{0x10B6C - 0xD48 + k:05d}.bin").write_bytes(b)
            written += 1
    print(f"  announcer: {same} of 56 Japanese lines are the original's")
    # the patch's music: stereo tracks at ids 0x816..0x938 of PZS4US1, none of them the original's. Entry n of its music
    # list plays file 0x816 + n (its program and menus have 0x816 where the original has 0x10B16), and its name is row
    # n % 8 of picture n / 8 of the character select's resources (section 2 of the menu archive, file 0x1C1; part 4
    # the names, part 3 the same lit). Each track with a name becomes an added song of the music select
    # (port/src/plat_songs.c), in the order of the patch's own list, then the others; names.rgba holds the pictures
    # (512x32, the names in that order, then the same lit) for the overlay (port/src/gs/ui.cpp).
    songs = data / "slus21978" / "songs"
    songs.mkdir(parents=True, exist_ok=True)
    for old in list(songs.glob("*.adx")) + list(songs.glob("names.rgba")):
        old.unlink()
    menu = patch1(0x1C1 - 1)
    res = bpe_decode(menu[pack_at(menu, 2):])
    names, lit = name_sheets(res, 4), name_sheets(res, 3)
    bgm = pack_at(res, 56)
    order = [v for v in struct.unpack_from(f"<{struct.unpack_from('<I', res, bgm)[0]}i", res, bgm + 0x10) if v >= 0]
    order = list(dict.fromkeys(order)) + [fid - 0x816 for fid in range(0x816, 0x939)]

    def row(sheets, n):
        sheet = sheets.get(n // 8)
        return None if sheet is None else sheet[(n % 8) * 32 * 512 * 4:(n % 8 + 1) * 32 * 512 * 4]

    listing, rows, lit_rows = [], [], []
    for n in dict.fromkeys(order):
        if n in (0x18, 0x19):
            continue  # the list's "random" and "locked" entries (their pictures say so), not songs
        b = patch1(0x816 + n - 1) if 0x816 + n < 0x939 else b""
        name, name_lit = row(names, n), row(lit, n)
        if not (is_voice(b) and b[7] == 2 and struct.unpack_from(">I", b, 12)[0] > 20 * struct.unpack_from(">I", b, 8)[0]):
            continue
        if name is None or not any(name[3::4]):
            continue  # (one track has no name picture: it is left out)
        file = f"slus21978_{0x816 + n:03x}.adx"
        (songs / file).write_bytes(b.rstrip(b"\0"))
        listing.append(f"{file}|SLUS-21978 {len(listing) + 1}")
        rows.append(name)
        lit_rows.append(name_lit if name_lit is not None else name)
    (songs / "songs.txt").write_text("# the music of the patch SLUS-21978, written by port/tools/extract_slus21978.py\n" +
                                     "".join(line + "\n" for line in listing))
    (songs / "names.rgba").write_bytes(struct.pack("<III", 512, 32, 2 * len(rows)) + b"".join(rows) + b"".join(lit_rows))
    print(f"  music: {len(listing)} tracks with their names -> {songs}")
    # the characters the patch adds, as character packages (port/tools/slus21978_chars.py). From the same resources:
    # part 43 their names (a pack of UTF-16 lines, line c the name of character c), part 42 the grid of the select
    # (cells of 0x24 bytes from +0x10: id, number of forms, seven forms), part 45 their chips (entry c + 1)
    import slus21978_chars
    toc3 = {name: (iso.find("DATA/" + name.upper()), toc[name]) for name in toc}

    def read(fid):
        name, index = (("pzs4us1.afs", fid - 1) if fid < 0xD48 else ("pzs4us2.afs", fid - 0xD48) if fid < 0x10CC0 else
                       ("pzs4us0.afs", fid - 0x10CC0) if fid < 0x20000 else ("pzs4us3.afs", fid - 0x20000))
        (lba, _), files = toc3[name]
        offset, sectors = files[index]
        iso.f.seek(lba * SECTOR + offset)
        return iso.f.read(sectors * SECTOR)

    names, grid, chips = character_tables(res)
    which = [int(c) for c in CHARS.split(",")] if CHARS else None
    bt3_of = {m: c for c, m in mapping.items()}
    taken = slus21978_chars.extract(read, bpe_decode, data, names, grid, chips, bt3_of, which)
    print(f"  characters: {taken} -> {data / 'characters'}")
    (data / "slus21978" / "source.txt").write_text(
        "The patch SLUS-21978: Spanish (Latin America) voices and music\n" +
        "".join(f"character {c} = {m}\n" for c, m in sorted(moved.items())))
    print(f"{written} voice lines in Spanish -> {out}")


if __name__ == "__main__":
    main()
