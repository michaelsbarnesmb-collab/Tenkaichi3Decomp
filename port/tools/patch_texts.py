#!/usr/bin/env python3
"""The game's save-file dialogs, with the words about the memory card taken out.

On a PC there is no card to insert, take out, format or run out of space on, so the dialogs that talk about it
are rewritten to talk about the save file instead. The game asks whether to save; the answers are handled by
port/src/plat_autosave.c, which leaves only the two "Save game data?" prompts on screen for a moment.

The text lives in the game's own resource files, one per menu, so it is patched where the game reads it:
`gamedata/mods/<part>/<file>.bin`, which the file cache prefers over the disc's copy (port/src/plat_fcache.c).
Nothing is written over the original, and deleting the patched files puts the game's own words back.

HOW THE TEXT IS FOUND, WITHOUT CARRYING ANY OF IT IN HERE
    Every file has the same 31 dialog texts that mention the memory card, in the same order (checked across the
    six of them). So the texts are found by position in that list rather than by matching what they say: this
    file holds only the 31 texts that go *in*, none of the ones that come out. A file that does not present
    exactly 31 is left alone and reported, so a different disc or a changed resource fails loudly here instead
    of quietly writing something wrong into the game.

    Two of the texts spell it across a line break ("memory \ncard"), so whitespace is collapsed before looking.

THE FILES
    Each one is a container of sections (the first words are the sections' offsets); the text is in a section
    compressed with the game's own BPE, so the section is decoded, edited, and compressed again. That encoder
    only writes literal bytes, so its output is larger than what it replaces; the sections that follow are moved
    along and their offsets in the table are corrected. The result is checked before it is written.

    python3 port/tools/patch_texts.py [--gamedata gamedata] [--check]

Standard library only; Linux for now."""
import argparse, pathlib, re, struct, sys

# ---------------------------------------------------------------------------------------------- the 31 texts that go in
#
# In the order the game's own texts appear in each file. Empty means the line the dialog draws as its title
# is left blank, which is what the game did for a dialog with no title to give.
NEW = [
    "",                                                                 # 0
    "",                                                                 # 1
    "",                                                                 # 2
    "Checking Save File",                                               # 3
    "Save file not found.\nProgress will not be saved.\nContinue anyway?",   # 4
    "Not enough free space.\nAt least 61 KB is required.\nContinue anyway?",   # 5
    "Do you wish to create a save file?",                                # 6
    "Auto Saving",                                                      # 7
    "Save game data?",                                                  # 8
    "Auto Saving",                                                      # 9
    "Save failed.\nPlease try again.",                                  # 10
    "Loading data.",                                                    # 11
    "Load failed.\nPlease try again.",                                  # 12
    "",                                                                 # 13
    "Formatting save file.",                                            # 14
    "Format failed!\nPlease try again.",                                # 15
    "No save file found.",                                              # 16
    "Game data not found.",                                             # 17
    "Load failed.\nData may be corrupt.",                               # 18
    "Not enough free space.\nAt least 61 KB is required.",              # 19
    "Checking Save File",                                               # 20
    "No replay data found.\nAt least 160 KB is required.\nContinue anyway?",  # 21
    "Not enough free space.\nAt least 160 KB is required.\nContinue anyway?",   # 22
    "Create Replay data?",                                              # 23
    "Auto Saving",                                                      # 24
    "Save replay data?",                                                # 25
    "Auto Saving",                                                      # 26
    "Replay data save failed.\nPlease try again.",                      # 27
    "Loading replay data.",                                             # 28
    "Replay data load failed.\nPlease try again.",                      # 29
    "Not enough free space.\nAt least 160 KB is required.",              # 30
]
COUNT = len(NEW)
FILES = ("00448", "00449", "00453", "00460", "00472", "00473")  # every file that carries the texts


# ----------------------------------------------------------------------------------------------------------- the format

def part(base, index):
    """MPACK_AT: the start of section `index`, which is its offset with the low bits cleared."""
    return (struct.unpack_from("<I", base, index * 4)[0] >> 2) << 2


def sections(data):
    """The sections of the file, as (index, offset), in the order they are stored."""
    out, prev = [], -1
    for i in range(80):
        try:
            off = part(data, i)
        except struct.error:
            break
        if not (prev < off < len(data)):
            break
        out.append((i, off))
        prev = off
    return out


def bpe_decode(buf, off):
    """The BPE section at `off` (the game's own decoder, src/sys/bpe.c). Returns (plain, declared size)."""
    size, packsize = struct.unpack_from("<ii", buf, off)
    p = off + 8
    out = bytearray()
    end = p + packsize
    left = bytearray(256)
    right = bytearray(256)
    stack = bytearray(256)   # fixed at 256, like the C decoder's u8 stack[256]
    while p < end:
        count = buf[p]; p += 1
        for i in range(256):
            left[i] = i
        c = 0
        while True:
            if count > 127:
                c += count - 127
                count = 0
            if c == 256:
                break
            i = 0
            while i <= count:
                left[c] = buf[p]; p += 1
                if c != left[c]:
                    right[c] = buf[p]; p += 1
                i += 1; c += 1
            if c == 256:
                break
            count = buf[p]; p += 1
        hi = buf[p]; p += 1
        blocksize = buf[p] + (hi << 8); p += 1
        i = 0
        while True:
            if i != 0:
                i -= 1; c = stack[i]
            else:
                if blocksize == 0:
                    break
                blocksize -= 1; c = buf[p]; p += 1
            if c == left[c]:
                out.append(c)
            else:
                stack[i] = right[c]; i += 1
                stack[i] = left[c]; i += 1
    return bytes(out), size


def bpe_encode_literal(data):
    """The same format from an encoder that only writes literals: the table is the identity (every byte stands
    for itself), which the decoder above reproduces byte for byte. It is larger than what it replaces, never
    smaller, which is why the sections that follow have to move along. blockSize is 16 bits, so a section is
    written in pieces of up to 65535."""
    body = bytearray()
    for off in range(0, len(data), 65535):
        chunk = data[off:off + 65535]
        body.append(255)                          # count > 127: skip 128 symbols of the identity table
        body.append(128)                          # left[128] = 128  (c == left[c], so no right byte)
        body.append(126)                          # count: fills c = 129..255
        for c in range(129, 256):
            body.append(c)                        # left[c] = c, no right byte
        body.append((len(chunk) >> 8) & 0xFF)     # blockSize, high byte
        body.append(len(chunk) & 0xFF)            # and low byte
        body += chunk                             # the symbols, each expanding to itself
    return struct.pack("<ii", len(data), len(body)) + bytes(body)


# ------------------------------------------------------------------------------------------------------ finding the text

def strings(buf):
    """Every text in a decompressed section: UTF-16 with a BOM at the front, ending in a 0. (offset, text)."""
    out, i = [], 0
    while i < len(buf) - 5:
        if buf[i] == 0xFF and buf[i + 1] == 0xFE:
            text, p = [], i + 2
            while p < len(buf) - 1:
                ch = struct.unpack_from("<H", buf, p)[0]
                p += 2
                if ch == 0:
                    break
                text.append("\n" if ch == 10 else chr(ch))
            s = "".join(text).replace("﻿", "").strip()
            if s:
                out.append((i, p, s))
            i = p
        else:
            i += 2
    return out


def about_card(s):
    """Whether a text is one of the dialogs that talk about the memory card."""
    return "memory card" in re.sub(r"\s+", " ", s).lower()


def find_section(data):
    """The one section holding the 31 dialogs: (index, offset, decompressed). None when there is no such
    section, or more than one, which would mean this is not the file it is expected to be."""
    found = []
    for idx, off in sections(data):
        try:
            plain, declared = bpe_decode(data, off)
        except (struct.error, IndexError):
            continue
        if len(plain) != declared:
            continue
        if len([s for _, _, s in strings(plain) if about_card(s)]) == COUNT:
            found.append((idx, off, plain))
    return found[0] if len(found) == 1 else None


# ------------------------------------------------------------------------------------------------------- rewriting a section

def rewrite(plain):
    """The 31 texts of `plain`, each replaced by the one that goes in its place. Returns (new, how many)."""
    buf = bytearray(plain)
    spots = [(start, end) for start, end, s in strings(buf) if about_card(s)]
    if len(spots) != COUNT:
        raise SystemExit(f"expected {COUNT} texts about the card, found {len(spots)}")
    for (start, end), new in zip(spots, NEW):
        # The BOM stays where it is; the text and its ending 0 go over the old ones. Every new text is
        # shorter than the one it replaces, which is what leaves room to write it in place.
        data = new.encode("utf-16-le") + b"\x00\x00"
        room = end - start
        if len(data) > room:
            raise SystemExit(f"a new text does not fit: {new!r} needs {len(data)} of {room} bytes")
        buf[start + 2:start + 2 + len(data)] = data
        for k in range(start + 2 + len(data), end):
            buf[k] = 0
    return bytes(buf), len(spots)


def rebuild(data, idx, off, encoded):
    """The file with the section at `off` replaced by `encoded`: the sections after it move along, and the
    table of offsets at the front is corrected to match."""
    secs = sections(data)
    after = [(i, o) for i, o in secs if o > off]
    tail_start = after[0][1] if after else len(data)
    delta = len(encoded) - (tail_start - off)
    out = bytearray(data[:off]) + bytearray(encoded) + bytearray(data[tail_start:])
    for i, o in secs:
        if o > off:
            struct.pack_into("<I", out, i * 4, ((o + delta) >> 2) << 2)
    return bytes(out), idx, delta


def check(patched, idx):
    """Reads the finished file back: the section has to still decode, and none of its texts may talk about the
    card any more. The section is looked up by the number it has in the table, which the rebuild kept."""
    try:
        off = part(patched, idx)
    except struct.error:
        return "the table of offsets does not reach the section"
    try:
        plain, declared = bpe_decode(patched, off)
    except (struct.error, IndexError):
        return "the section does not decode any more"
    if len(plain) != declared:
        return f"the section decodes to {len(plain)} of {declared} bytes"
    left = [s for _, _, s in strings(plain) if about_card(s)]
    if left:
        return f"{len(left)} of the texts still talk about the card"
    # the titles that are emptied leave only their BOM behind, so what is left is checked against what went in
    got = [s for _, _, s in strings(plain)]
    for new in NEW:
        if new and new not in got:
            return f"a text that should be there is not: {new.splitlines()[0]!r}"
    return None


# --------------------------------------------------------------------------------------------------------- the files to patch

def run(gamedata, only_check, force):
    data = pathlib.Path(gamedata)
    todo = [f for f in FILES if (data / "pzs3us1" / f"{f}.bin").is_file()]
    if not todo:
        print(f"no game files in {data}/pzs3us1 (run the setup first)", file=sys.stderr)
        return 1
    for name in todo:
        src = data / "pzs3us1" / f"{name}.bin"
        dst = data / "mods" / "pzs3us1" / f"{name}.bin"
        original = src.read_bytes()
        hit = find_section(original)
        if hit is None:
            print(f"{name}.bin  no section with the {COUNT} dialogs: left as it is", file=sys.stderr)
            continue
        if only_check:
            print(f"{name}.bin  section {hit[0]} holds the {COUNT} dialogs")
            continue
        if dst.is_file() and not force:
            print(f"{name}.bin  already patched")
            continue
        plain, _ = rewrite(hit[2])
        patched, idx, delta = rebuild(original, hit[0], hit[1], bpe_encode_literal(plain))
        problem = check(patched, idx)
        if problem:
            print(f"{name}.bin  not written: {problem}", file=sys.stderr)
            return 1
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(patched)
        print(f"{name}.bin  {COUNT} texts  (section {idx}, {delta:+d} bytes)  ->  mods/")
    return 0


def main():
    ap = argparse.ArgumentParser(description="Rewrites the game's save-file dialogs so they stop talking "
                                                 "about a memory card. Part of the setup; run it on its own only "
                                                 "to look at what it would do.")
    ap.add_argument("--gamedata", default="gamedata", help="the folder the setup unpacked the disc into")
    ap.add_argument("--check", action="store_true", help="only say which files hold the texts")
    ap.add_argument("--force", action="store_true", help="patch again over files already patched")
    args = ap.parse_args()
    return run(args.gamedata, args.check, args.force)


if __name__ == "__main__":
    sys.exit(main())