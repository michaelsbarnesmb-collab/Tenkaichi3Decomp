"""The characters the patch SLUS-21978 adds, written as character packages (docs/port/character_packages.md): one
folder per character in data_dir/characters, "SLUS-21978 <its name>", with the patch's files laid out as the
original's. port/tools/extract_slus21978.py calls it; the game reads the folders as any other package.

How the patch keeps a character (worked out from its program, nothing documented); c is the patch's own number,
its added characters 165..249 (161..164 are the select's own cells):
- Its character table (file 4, part 1) has 250 entries of the original's 0x3C bytes; the number of costumes is also
  at +0x28. Its fighters' numbers (file 2, part 8): 250 x 16 bytes (health, starting ki, -, blast stocks).
- Models follow each other from file 0x1612: character c's costume k is 0x1612 + 2 * (costumes of 0..c-1) + k, its
  damaged models the next `costumes` files.
- A model has 52 parts. The original's model has 252; the patch keeps the rest in other files:
    part 0                      0x1036 + c, entry 1
    parts 17..21, 23, 24, 26, 27  0x1036 + c, entry 2 (one after the other, fixed sizes; 17 the parameter table)
    parts 40..42                0x1036 + c, entry 3 (a pack)
    part 44 (the moves' names)   0x1130 + c (one language; the original has one part per language, 43..51)
    part 35                     entry 0 of the animations 0x141E + c, BPE-packed
    parts 52..151 / 152..251     0x122A + c / 0x1324 + c (lip tracks of the two voice languages)
  Its parts 25 and 51 are new (the original reads neither) and are left out.
- Animations 0x141E + c and effects 0x1518 + c (the original's 0x598 / 0x599 + c * 10); the large picture
  0x71C + c; voice banks 0x1D012 (Japanese) / 0x1D10C (Spanish) + c; spoken lines 0x10CC2 (Japanese) / 0x16E6A
  (Spanish) + c * 100 + line; its chip: entry c + 1 of part 45 of the character select's resources.
- Its select's grid (part 42 of those resources) puts the forms of a fighter in one cell.
- The parameter table names forms and fusions by the patch's numbers: they become the package's links."""
import struct

FIRST = 165  # the patch's first added character
COSTUMES = 4  # the original's room per character
TABLE = 0x3C
PREFIX = "SLUS-21978 "


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
    # would not fit the original's model buffer)
    parts[35] = bpe_decode(entries(read(0x141E + c))[0])
    for first, base in ((52, 0x122A), (152, 0x1324)):
        lips = read(base + c)
        ln = struct.unpack_from("<I", lips, 0)[0]
        lo = list(struct.unpack_from(f"<{ln + 1}I", lips, 0)) + [len(lips)]
        for i in range(100):
            parts[first + i] = lips[lo[i + 1]:lo[i + 2]] if lo[i + 1] != lo[i + 2] else b""
    return pack(parts)


def is_voice(b):
    """An ADX file with sound in it (the patch keeps a silent file where it has no recording)."""
    if len(b) < 0x20 or b[:2] != b"\x80\x00":
        return False
    start = struct.unpack_from(">H", b, 2)[0] + 4
    if b[start - 6:start] != b"(c)CRI":
        return False
    channels, samples = b[7], struct.unpack_from(">I", b, 12)[0]
    data = b[start:start + (samples + 31) // 32 * 18 * channels]
    return any(data[i + 2:i + 18].strip(b"\0") for i in range(0, len(data), 18))


def folder_name(name):
    clean = "".join("-" if ch in '/\\:*?"<>|' or ord(ch) < 32 else ch for ch in name).strip(" .")
    return (PREFIX + clean)[:63].rstrip(" .")


def extract(read, bpe_decode, data, names, grid, chips, bt3_of, which=None):
    """read(id): a file of the patch; names: its character names (by number); grid: its select's cells (lists of
    character numbers); chips: its chip pack entries (by number); bt3_of: the original's number of each of the
    original's characters, by the patch's number. which: the added characters to take (default: every one the
    patch's select has a cell for)."""
    import shutil
    out = data / "characters"
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob(PREFIX + "*"):  # (what an earlier run wrote: the patch's packages are made afresh)
        shutil.rmtree(old)
    tbl = entries(read(4))[0]
    numbers = entries(read(2))[7]
    count = len(tbl) // TABLE
    costumes = [struct.unpack_from("<H", tbl, c * TABLE + 0x28)[0] for c in range(count)]
    on_grid = [c for cell in grid for c in cell if FIRST <= c < count]
    take = [c for c in (which or dict.fromkeys(on_grid)) if FIRST <= c < count and costumes[c] and names.get(c)]
    folder = {}
    for c in take:  # (two characters of one name: the second gets its number)
        folder[c] = folder_name(names[c]) if folder_name(names[c]) not in folder.values() else folder_name(f"{names[c]} {c}")

    def link(t):
        if t == 0xFF:
            return None
        if t in folder:
            return folder[t]
        if t in bt3_of:
            return f"disc:{bt3_of[t]}"
        return "none"  # (a character of the patch that is not taken: that form is left out, with a remark)

    def base_of(c):
        for cell in grid:
            if c in cell:
                own = [bt3_of[x] for x in cell if x in bt3_of]
                if own:
                    return own[0]
        return 0

    for c in take:
        d = out / folder[c]
        d.mkdir()
        stem = folder[c][len(PREFIX):].replace(" ", "_") or str(c)
        first = 0x1612 + 2 * sum(costumes[:c])
        param = b""
        for k in range(min(costumes[c], COSTUMES)):
            m = model(read, bpe_decode, c, first + k)
            (d / f"{stem}_{k + 1}p.pak").write_bytes(m)
            (d / f"{stem}_{k + 1}p_dmg.pak").write_bytes(model(read, bpe_decode, c, first + costumes[c] + k))
            if k == 0:
                param = entries(m)[17]  # (the parameter table: BtlParam, forms at +0x98, fusions at +0xB4)
        (d / f"{stem}_anm.pak").write_bytes(read(0x141E + c))
        (d / f"{stem}_eff.pak").write_bytes(read(0x1518 + c))
        (d / f"{stem}_voice_us.pak").write_bytes(read(0x1D10C + c).rstrip(b"\0"))  # (Spanish, in the English place)
        (d / f"{stem}_voice_jp.pak").write_bytes(read(0x1D012 + c).rstrip(b"\0"))
        for sub, base in (("voice_us", 0x16E6A), ("voice_jp", 0x10CC2)):
            for line in range(100):
                b = read(base + c * 100 + line)
                if is_voice(b):
                    (d / sub).mkdir(exist_ok=True)
                    (d / sub / f"{line:03d}.adx").write_bytes(b.rstrip(b"\0"))
        (d / "portrait.bin").write_bytes(read(0x71C + c))
        if chips.get(c):
            (d / "chip.bin").write_bytes(chips[c])
        health, ki, _, blast = struct.unpack_from("<4i", numbers, c * 16)
        lines = ["format=1", f"name={names[c]}", f"base={base_of(c)}", "author=SLUS-21978 (from the player's own disc image)",
                 f"about=Taken from the patch SLUS-21978, its character {c}, by port/tools/extract_slus21978.py.",
                 f"health={min(max(health, 10000), 150000)}", f"ki={min(max(ki, 0), 100000)}", f"blast={min(max(blast, 0), 7)}"]
        if len(param) >= 0xC6:
            for key, t in zip(("left", "up", "right", "down"), param[0x98:0x9C]):
                if link(t):
                    lines.append(f"{key}={link(t)}")
            for k in range(3):
                result = link(param[0xB4 + k])
                if result:
                    lines.append(f"fusion{k + 1}={result}")
                    partners = [link(t) for t in param[0xBA + 4 * k:0xBE + 4 * k] if link(t)]
                    if partners:
                        lines.append(f"partner{k + 1}={','.join(partners)}")
        (d / "character.txt").write_text("".join(line + "\n" for line in lines), encoding="utf-8")
        print(f"  character {c}: {names[c]} -> {d.name}")
    return len(take)
