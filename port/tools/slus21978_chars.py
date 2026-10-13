"""The characters the patch SLUS-21978 adds, written as the original's files (port/tools/extract_slus21978.py calls
it). For each added character c (the patch's own number, 165..249) data_dir/slus21978/chars/<c>/ gets:

    model<k>.bin    the model of costume k (k + 4: damaged), laid out as the original's (file 0x590 + c * 10 + k)
    anim8.bin       its animations                                     (file 0x598 + c * 10)
    anim9.bin       its effects                                        (file 0x599 + c * 10)
    data<s>.bin     its per-fighter block on side s (the gauge's face)  (file 8 + c * 2 + s)
    portrait.bin    its large picture                                  (file 0x2F9 + c)
    bank.bin        its voice bank, Spanish; bank_jp.bin: Japanese     (file 0xC7B + c; 0xBDA + c)
    table.bin       its entry of the character table (0x3C bytes) and its parameters (16 bytes)

and data_dir/slus21978/chars/chars.txt lists them ("<c>|<name>"). The port serves these files for a character past
the original's 161 (port/src/plat_chars.c).

How the patch keeps a character (worked out from its program, nothing documented):
- Its character table (file 4, part 1) has 250 entries of the original's 0x3C bytes; the number of costumes is also
  at +0x28. Its parameters (file 2, part 8): 250 x 16 bytes.
- Models follow each other from file 0x1612: character c's costume k is 0x1612 + 2 * (costumes of 0..c-1) + k, its
  damaged models the next `costumes` files.
- A model has 52 parts. The original's model has 252; the patch keeps the rest in other files:
    part 0                      0x1036 + c, entry 1
    parts 17..21, 23, 24, 26, 27  0x1036 + c, entry 2 (one after the other, fixed sizes)
    parts 40..42                0x1036 + c, entry 3 (a pack)
    part 44 (the moves' names)   0x1130 + c (one language; the original has one part per language, 43..51)
    part 35                     entry 0 of the animations 0x141E + c, BPE-packed
    parts 52..151 / 152..251     0x122A + c / 0x1324 + c (lip tracks of the two voice languages)
  Its parts 25 and 51 are new (the original reads neither) and are left out.
- Animations 0x141E + c and 0x1518 + c (the original's 0x598 / 0x599 + c * 10); the per-fighter block
  0xD48 + c * 2 + side; the large picture 0x71C + c; voice banks 0x1D012 (Japanese) / 0x1D10C (Spanish) + c."""
import struct

FIRST = 165  # the patch's first added character (161..164 are the select's own cells)
COSTUMES = 4  # the original's room per character: costumes 0..3, damaged 4..7
TABLE = 0x3C


def entries(d):
    """The entries of a pack: word 0 is their count, then each one's offset; the last one ends where the next
    offset word (or the file) says."""
    n = struct.unpack_from("<I", d, 0)[0]
    o = [v >> 2 << 2 for v in struct.unpack_from(f"<{n + 1}I", d, 0)] + [len(d)]
    return [d[o[i + 1]:o[i + 2]] for i in range(n)]


def pack(parts):
    """A pack as the original's model files are: offsets 64-byte aligned, then the end."""
    out = bytearray((4 * (len(parts) + 2) + 0x3F) & ~0x3F)
    offsets = []
    for p in parts:
        offsets.append(len(out))
        out += p
        out += b"\0" * (-len(out) & 0x3F)
    struct.pack_into(f"<{len(parts) + 2}I", out, 0, len(parts), *offsets, len(out))
    return bytes(out)


BLOCK = [(17, 0x100), (18, 0x2140), (19, 0x2C0), (20, 0xC0), (21, 0x410), (23, 0x280), (24, 0x100), (26, 0x200), (27, None)]


def model(read, bpe_decode, c, file):
    parts = [b""] * 252
    for i, p in enumerate(entries(read(file))[:52]):
        parts[i] = p
    extra = read(0x1036 + c)
    n = struct.unpack_from("<I", extra, 0)[0]
    o = [v >> 2 << 2 for v in struct.unpack_from(f"<{n + 1}I", extra, 0)] + [len(extra)]
    entry = lambda j: extra[o[j]:o[j + 1]] if j <= n else b""
    parts[0] = entry(1)
    block, pos = entry(2), 0
    for i, size in BLOCK:
        parts[i] = block[pos:pos + size] if size else block[pos:]
        pos += size or 0
    sub = entry(3)
    if sub:
        for j, p in enumerate(entries(sub)[:3]):
            parts[40 + j] = p
    parts[25] = b""
    parts[43:52] = [b""] * 9
    parts[44] = read(0x1130 + c).rstrip(b"\0") + b"\0\0"  # (the only language: in the others' room the model
    # would not fit the original's model buffer, 0xCE000 bytes)
    parts[35] = bpe_decode(entries(read(0x141E + c))[0])
    for first, base in ((52, 0x122A), (152, 0x1324)):
        lips = read(base + c)
        ln = struct.unpack_from("<I", lips, 0)[0]
        lo = list(struct.unpack_from(f"<{ln + 1}I", lips, 0)) + [len(lips)]
        for i in range(100):
            parts[first + i] = lips[lo[i + 1]:lo[i + 2]] if lo[i + 1] != lo[i + 2] else b""
    return pack(parts)


def extract(read, bpe_decode, data, names, which=None):
    """read(id): a file of the patch; names: the patch's character names (by number). which: the characters to
    take (default: every added one that has a model)."""
    out = data / "slus21978" / "chars"
    out.mkdir(parents=True, exist_ok=True)
    tbl = entries(read(4))[0]
    params = entries(read(2))[7]
    count = len(tbl) // TABLE
    costumes = [struct.unpack_from("<H", tbl, c * TABLE + 0x28)[0] for c in range(count)]
    listing = []
    for c in which or range(FIRST, count):
        if costumes[c] == 0 or not names.get(c):
            continue
        d = out / str(c)
        d.mkdir(exist_ok=True)
        first = 0x1612 + 2 * sum(costumes[:c])
        for k in range(min(costumes[c], COSTUMES)):
            (d / f"model{k}.bin").write_bytes(model(read, bpe_decode, c, first + k))
            (d / f"model{k + 4}.bin").write_bytes(model(read, bpe_decode, c, first + costumes[c] + k))
        (d / "anim8.bin").write_bytes(read(0x141E + c))
        (d / "anim9.bin").write_bytes(read(0x1518 + c))
        for side in (0, 1):
            (d / f"data{side}.bin").write_bytes(read(0xD48 + c * 2 + side))
        (d / "portrait.bin").write_bytes(read(0x71C + c))
        (d / "bank_jp.bin").write_bytes(read(0x1D012 + c).rstrip(b"\0"))
        (d / "bank.bin").write_bytes(read(0x1D10C + c).rstrip(b"\0"))
        entry = bytearray(tbl[c * TABLE:(c + 1) * TABLE])
        entry[0x0A:0x0C] = struct.pack("<H", min(costumes[c], COSTUMES))
        entry[0x2C:0x30] = b"\xff" * 4  # (its other forms: the patch's numbers, not taken here)
        (d / "table.bin").write_bytes(bytes(entry) + params[c * 16:(c + 1) * 16])
        listing.append(f"{c}|{names[c]}")
        print(f"  character {c}: {names[c]}, {min(costumes[c], COSTUMES)} costumes")
    (out / "chars.txt").write_text("# the characters of the patch SLUS-21978, written by port/tools/extract_slus21978.py\n" +
                                   "".join(line + "\n" for line in listing))
    return len(listing)
