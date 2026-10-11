// The setup's own installer, for a release folder (Tenkaichi3Decomp, Tenkaichi3Decomp.dat and Tenkaichi3Decomp-setup together, no build tools):
// reads the user's disc image directly, checks it, unpacks the game data and runs the self-test. No Python, no 7z.
// It reports through the same event lines as install.py (@step, @note, @ok, @skip, @fail, @stopped, @done), so the
// window (setup.cpp) shows both the same way.
//
//   disc image   ISO 9660: the volume descriptor at sector 16, directory records from the root
//   checks       SHA-1 of BIN/DBZP.BIN and of SLUS_216.78 laid out as a flat image from 0x100000 (the values of
//                config/DBZP.yaml and config/SLUS_216.78.yaml)
//   game data    gamedata/disc/<loose files>, and every DATA/*.AFS archive split into gamedata/<name>/00000.bin ...
//                (the layout of port/tools/extract_disc.py)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include "native.h"

namespace {

const char *kRomSha1 = "caee6c2269bba89fc51eea9e7beac3adbec9dc52";
const char *kDbzpSha1 = "4f910969e05d9b25c83af7642b60da9949a4348b";
const Uint32 kRomBase = 0x100000, kRomEnd = 0x2FF180;
#ifdef _WIN32
const char *kGame = "Tenkaichi3Decomp.exe";
#else
const char *kGame = "Tenkaichi3Decomp";
#endif

std::mutex sLock;
std::vector<std::string> sLines;
std::thread sThread;
std::atomic<bool> sRunning(false), sCancel(false);

void say(const std::string &line) {
    std::lock_guard<std::mutex> lock(sLock);
    sLines.push_back(line);
}

struct Stop { std::string why; };

// ------------------------------------------------------------------------------------------------------- SHA-1
struct Sha1 {
    Uint32 h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    Uint8 block[64];
    Uint64 total = 0;
    size_t fill = 0;

    static Uint32 rol(Uint32 v, int n) { return (v << n) | (v >> (32 - n)); }
    void compress() {
        Uint32 w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 16; i++) {
            w[i] = (Uint32)block[i * 4] << 24 | (Uint32)block[i * 4 + 1] << 16 | (Uint32)block[i * 4 + 2] << 8 | block[i * 4 + 3];
        }
        for (int i = 16; i < 80; i++) {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        for (int i = 0; i < 80; i++) {
            Uint32 f = i < 20 ? (b & c) | (~b & d) : i < 40 ? b ^ c ^ d : i < 60 ? (b & c) | (b & d) | (c & d) : b ^ c ^ d;
            Uint32 k = i < 20 ? 0x5A827999 : i < 40 ? 0x6ED9EBA1 : i < 60 ? 0x8F1BBCDC : 0xCA62C1D6;
            Uint32 t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    void add(const Uint8 *p, size_t n) {
        total += n;
        while (n > 0) {
            size_t take = 64 - fill < n ? 64 - fill : n;
            memcpy(block + fill, p, take);
            fill += take; p += take; n -= take;
            if (fill == 64) {
                compress();
                fill = 0;
            }
        }
    }
    std::string hex() {
        Uint64 bits = total * 8;
        Uint8 pad = 0x80, zero = 0, len[8];
        char out[41];
        add(&pad, 1);
        while (fill != 56) {
            add(&zero, 1);
        }
        for (int i = 0; i < 8; i++) {
            len[i] = (Uint8)(bits >> (56 - i * 8));
        }
        add(len, 8);
        for (int i = 0; i < 5; i++) {
            snprintf(out + i * 8, 9, "%08x", h[i]);
        }
        return out;
    }
};

// ------------------------------------------------------------------------------------------------- the disc image
struct IsoFile { std::string path; Uint64 offset, size; };

struct Iso {
    SDL_IOStream *io = NULL;
    std::vector<IsoFile> files;

    void read(Uint64 offset, void *buf, size_t n) {
        if (SDL_SeekIO(io, (Sint64)offset, SDL_IO_SEEK_SET) < 0 || SDL_ReadIO(io, buf, n) != n) {
            throw Stop{"The disc image could not be read to its end. It may be incomplete."};
        }
    }
    void walk(Uint32 lba, Uint32 size, const std::string &dir, int depth) {
        std::vector<Uint8> d(size);
        read((Uint64)lba * 2048, d.data(), size);
        for (size_t pos = 0; pos + 34 <= size;) {
            Uint8 len = d[pos];
            if (len == 0) { // records do not cross sectors: the rest of this one is empty
                pos = (pos / 2048 + 1) * 2048;
                continue;
            }
            if (pos + len > size) {
                break;
            }
            Uint32 ext, bytes;
            memcpy(&ext, &d[pos + 2], 4);
            memcpy(&bytes, &d[pos + 10], 4);
            Uint8 flags = d[pos + 25], nameLen = d[pos + 32];
            std::string name((const char *)&d[pos + 33], nameLen);
            pos += len;
            if (nameLen == 1 && (name[0] == 0 || name[0] == 1)) {
                continue; // this directory and its parent
            }
            size_t semi = name.find(';');
            if (semi != std::string::npos) {
                name.resize(semi);
            }
            if (!name.empty() && name.back() == '.') {
                name.pop_back();
            }
            if (flags & 2) {
                if (depth < 8) {
                    walk(ext, bytes, dir + name + "/", depth + 1);
                }
            } else if (!files.empty() && files.back().path == dir + name) {
                files.back().size += bytes; // a file in several extents: they follow each other
            } else {
                files.push_back({dir + name, (Uint64)ext * 2048, bytes});
            }
        }
    }
    void open(const std::string &path) {
        Uint8 pvd[2048];
        Uint32 lba, size;
        io = SDL_IOFromFile(path.c_str(), "rb");
        if (io == NULL) {
            throw Stop{"The disc image could not be opened: " + path};
        }
        if (SDL_SeekIO(io, 16 * 2048, SDL_IO_SEEK_SET) < 0 || SDL_ReadIO(io, pvd, sizeof(pvd)) != sizeof(pvd) || pvd[0] != 1 ||
            memcmp(pvd + 1, "CD001", 5) != 0) {
            throw Stop{"This file is not a disc image (.iso). Compressed images (.chd, .cso, .gz) have to be converted to .iso first."};
        }
        memcpy(&lba, pvd + 156 + 2, 4);
        memcpy(&size, pvd + 156 + 10, 4);
        walk(lba, size, "", 0);
    }
    const IsoFile *find(const char *path) {
        for (auto &f : files) {
            if (SDL_strcasecmp(f.path.c_str(), path) == 0) {
                return &f;
            }
        }
        return NULL;
    }
    ~Iso() {
        if (io != NULL) {
            SDL_CloseIO(io);
        }
    }
};

std::string lower(std::string s) {
    for (auto &c : s) {
        c = (char)tolower((unsigned char)c);
    }
    return s;
}

std::string upper(std::string s) {
    for (auto &c : s) {
        c = (char)toupper((unsigned char)c);
    }
    return s;
}

void check_cancel() {
    if (sCancel) {
        throw Stop{"Cancelled. Install continues from where it stopped."};
    }
}

// Copies `size` bytes of the image at `offset` into a new file.
void copy_out(Iso &iso, Uint64 offset, Uint64 size, const std::string &path, std::vector<Uint8> &buf) {
    SDL_IOStream *out = SDL_IOFromFile(path.c_str(), "wb");
    if (out == NULL) {
        throw Stop{"Could not write " + path + ". Is the disk full or the folder read-only?"};
    }
    while (size > 0) {
        size_t n = size < buf.size() ? (size_t)size : buf.size();
        iso.read(offset, buf.data(), n);
        if (SDL_WriteIO(out, buf.data(), n) != n) {
            SDL_CloseIO(out);
            throw Stop{"Could not write " + path + ". Is the disk full?"};
        }
        offset += n;
        size -= n;
    }
    if (!SDL_CloseIO(out)) {
        throw Stop{"Could not write " + path + ". Is the disk full?"};
    }
}

void step_disc(Iso &iso, const std::string &path) {
    say("@step 1 4 Disc image");
    iso.open(path);
    const IsoFile *slus = iso.find("SLUS_216.78"), *dbzp = iso.find("BIN/DBZP.BIN");
    if (slus == NULL || dbzp == NULL) {
        throw Stop{"This is not a disc image of the USA release of Budokai Tenkaichi 3 (no SLUS_216.78 on it).\n"
                   "Other regions and the Wii version have different programs and are not supported."};
    }
    say("@note checking");
    std::vector<Uint8> elf((size_t)slus->size), rom(kRomEnd - kRomBase, 0), menu((size_t)dbzp->size);
    iso.read(slus->offset, elf.data(), elf.size());
    iso.read(dbzp->offset, menu.data(), menu.size());
    if (elf.size() > 0x34 && memcmp(elf.data(), "\177ELF", 4) == 0) {
        Uint32 shoff;
        Uint16 shentsize, shnum;
        memcpy(&shoff, &elf[0x20], 4);
        memcpy(&shentsize, &elf[0x2E], 2);
        memcpy(&shnum, &elf[0x30], 2);
        for (Uint32 i = 0; i < shnum && (size_t)shoff + (size_t)(i + 1) * shentsize <= elf.size(); i++) {
            Uint32 sh[6]; // name, type, flags, address, offset, size
            memcpy(sh, &elf[shoff + (size_t)i * shentsize], sizeof(sh));
            if ((sh[2] & 2) && sh[1] != 8 && sh[5] != 0 && sh[3] >= kRomBase && sh[3] + sh[5] <= kRomEnd && (size_t)sh[4] + sh[5] <= elf.size()) {
                memcpy(&rom[sh[3] - kRomBase], &elf[sh[4]], sh[5]);
            }
        }
    }
    Sha1 a, b;
    a.add(rom.data(), rom.size());
    b.add(menu.data(), menu.size());
    if (a.hex() != kRomSha1 || b.hex() != kDbzpSha1) {
        throw Stop{"The game's programs on this disc do not have the expected checksums.\n"
                   "The port needs the unmodified USA release (SLUS-21678); a patched or damaged image will not work."};
    }
    say("@ok the USA release, checksums match");
}

void step_data(Iso &iso, const std::string &root) {
    std::string data = root + "gamedata/", marker = data + ".installed";
    SDL_PathInfo info;
    say("@step 2 4 Game data");
    if (SDL_GetPathInfo(marker.c_str(), &info)) {
        say("@skip already unpacked");
        return;
    }
    std::vector<const IsoFile *> take;
    Uint64 total = 0, done = 0;
    for (auto &f : iso.files) {
        std::string u = upper(f.path);
        if (u == "SLUS_216.78" || u.rfind("BIN/", 0) == 0 || u.rfind("DATA/", 0) == 0) {
            take.push_back(&f);
            total += f.size;
        }
    }
    std::vector<Uint8> buf(1 << 20);
    int files = 0, lastPercent = -1;
    auto progress = [&](Uint64 n) {
        done += n;
        int percent = total ? (int)(done * 100 / total) : 0;
        if (percent != lastPercent) {
            char line[64];
            snprintf(line, sizeof(line), "@note %d%%, %d files", percent, files);
            say(line);
            lastPercent = percent;
        }
        check_cancel();
    };
    for (const IsoFile *f : take) {
        std::string u = upper(f->path);
        bool afs = u.rfind("DATA/", 0) == 0 && u.size() > 4 && u.compare(u.size() - 4, 4, ".AFS") == 0;
        if (!afs) {
            std::string out = data + "disc/" + u; // upper case, as the game asks for them
            SDL_CreateDirectory(out.substr(0, out.rfind('/')).c_str());
            copy_out(iso, f->offset, f->size, out, buf);
            files++;
            progress(f->size);
            continue;
        }
        // an AFS archive: "AFS\0", the number of entries, then (offset, size) of each; one loose file per entry
        std::string stem = lower(u.substr(5, u.size() - 9)), dir = data + stem + "/";
        Uint8 head[8];
        Uint32 count;
        SDL_CreateDirectory(dir.c_str());
        iso.read(f->offset, head, 8);
        memcpy(&count, head + 4, 4);
        if (memcmp(head, "AFS\0", 4) != 0 || (Uint64)count * 8 + 8 > f->size) {
            throw Stop{"An archive on the disc (" + f->path + ") is damaged."};
        }
        std::vector<Uint32> table((size_t)count * 2);
        iso.read(f->offset + 8, table.data(), table.size() * 4);
        Uint64 inArchive = 0;
        for (Uint32 i = 0; i < count; i++) {
            char name[32];
            snprintf(name, sizeof(name), "%05u.bin", i);
            if ((Uint64)table[i * 2] + table[i * 2 + 1] > f->size) {
                throw Stop{"An archive on the disc (" + f->path + ") is damaged."};
            }
            copy_out(iso, f->offset + table[i * 2], table[i * 2 + 1], dir + name, buf);
            files++;
            inArchive += table[i * 2 + 1];
            progress(table[i * 2 + 1]);
        }
        progress(f->size > inArchive ? f->size - inArchive : 0); // the archive's table and padding
    }
    SDL_IOStream *m = SDL_IOFromFile(marker.c_str(), "wb");
    if (m != NULL) {
        SDL_CloseIO(m);
    }
    char line[64];
    snprintf(line, sizeof(line), "@ok %d files in gamedata/", files);
    say(line);
}

// ------------------------------------------------------------------------------ the save-file dialogs

// The game's save dialogs say a memory card has to be inserted, formatted, or has no space left. On a PC there
// is no card, so they are rewritten to talk about the save file instead. The result goes to gamedata/mods/,
// which the game prefers over the disc's copy (port/src/plat_fcache.c); nothing is written over the original.
//
// The texts are found by their place in the list of the texts that mention the card, which is the same 31 in
// the same order in every file that has them. That is why neither this file nor port/tools/patch_texts.py (the
// same job for a source checkout) carries a single word of the game's own dialogs: only the 31 that go in.
static const char *kNewTexts[] = {
    "",                                                                             // the three slot names: the dialog draws no title
    "",
    "",
    "Checking Save File",
    "Save file not found.\nProgress will not be saved.\nContinue anyway?",
    "Not enough free space.\nAt least 61 KB is required.\nContinue anyway?",
    "Do you wish to create a save file?",
    "Auto Saving",
    "Save game data?",
    "Auto Saving",
    "Save failed.\nPlease try again.",
    "Loading data.",
    "Load failed.\nPlease try again.",
    "",                                                                             // the unformatted-card prompt, which should not come up
    "Formatting save file.",
    "Format failed!\nPlease try again.",
    "No save file found.",
    "Game data not found.",
    "Load failed.\nData may be corrupt.",
    "Not enough free space.\nAt least 61 KB is required.",
    "Checking Save File",
    "No replay data found.\nAt least 160 KB is required.\nContinue anyway?",
    "Not enough free space.\nAt least 160 KB is required.\nContinue anyway?",
    "Create Replay data?",
    "Auto Saving",
    "Save replay data?",
    "Auto Saving",
    "Replay data save failed.\nPlease try again.",
    "Loading replay data.",
    "Replay data load failed.\nPlease try again.",
    "Not enough free space.\nAt least 160 KB is required.",
};
const int kTextCount = (int)(sizeof(kNewTexts) / sizeof(kNewTexts[0]));
const char *kTextFiles[] = {"00448", "00449", "00453", "00460", "00472", "00473"};

struct Section { int index; size_t off; };

static Uint32 rd32(const std::vector<Uint8> &d, size_t at) {
    return (Uint32)d[at] | ((Uint32)d[at + 1] << 8) | ((Uint32)d[at + 2] << 16) | ((Uint32)d[at + 3] << 24);
}
static void wr32(std::vector<Uint8> &d, size_t at, Uint32 v) {
    d[at] = (Uint8)v; d[at + 1] = (Uint8)(v >> 8); d[at + 2] = (Uint8)(v >> 16); d[at + 3] = (Uint8)(v >> 24);
}
static size_t part(const std::vector<Uint8> &d, int i) { return (size_t)((rd32(d, (size_t)i * 4) >> 2) << 2); }

/* The file's sections: the first words are their offsets, and they come in order. The running offset is kept
   signed on purpose: the first section's predecessor is "none", and comparing that as an unsigned size would
   wrap round to the largest one and stop the walk on its first step. */
static std::vector<Section> sections(const std::vector<Uint8> &d) {
    std::vector<Section> out;
    long prev = -1;
    for (int i = 0; i < 80; i++) {
        if ((size_t)i * 4 + 4 > d.size()) break;
        long o = (long)part(d, i);
        if (!(o > prev && o >= 0 && (size_t)o < d.size())) break;
        out.push_back({i, (size_t)o});
        prev = o;
    }
    return out;
}

/* The game's BPE decoder (src/sys/bpe.c), the same expansion the file loader does. */
static bool bpe_decode(const std::vector<Uint8> &d, size_t off, std::vector<Uint8> &out, Uint32 &declared) {
    if (off + 8 > d.size()) return false;
    Uint32 size = rd32(d, off), packed = rd32(d, off + 4);
    declared = size;
    size_t p = off + 8, end = p + packed;
    if (end > d.size()) return false;
    out.clear();
    Uint8 left[256], right[256], stack[256];
    while (p < end) {
        int count = d[p++];
        for (int i = 0; i < 256; i++) left[i] = (Uint8)i;
        int c = 0;
        for (;;) {
            if (count > 127) { c += count - 127; count = 0; }
            // c == 256 is the table filled up, which ends the reading of it; anything past that means these
            // bytes are not a BPE table at all (the file has sections that are plain data), and the caller
            // moves on to the next one
            if (c > 256) return false;
            if (c == 256) break;
            int i = 0;
            do {
                if (p >= end || c >= 256) return false;
                left[c] = d[p++];
                if (c != left[c]) { if (p >= end) return false; right[c] = d[p++]; }
                i++; c++;
            } while (i <= count);
            if (c == 256) break;
            if (p >= end) return false;
            count = d[p++];
        }
        if (p + 1 >= end) return false;
        // the size of the block comes first with its high byte, as bpeenc writes it and src/sys/bpe.c reads it
        Uint32 blocksize = (Uint32)d[p + 1] + ((Uint32)d[p] << 8);
        p += 2;
        int i = 0;
        for (;;) {
            int sym;
            if (i != 0) { i--; sym = stack[i]; }
            else {
                if (blocksize == 0) break;
                blocksize--;
                if (p >= end) return false;
                sym = d[p++];
            }
            if (sym == left[sym]) out.push_back((Uint8)sym);
            else {
                if ((size_t)i + 1 >= sizeof(stack)) return false;
                stack[i++] = right[sym];
                stack[i++] = left[sym];
            }
        }
    }
    return true;
}

/* The same format from an encoder that only writes literals: the table is the identity, so every byte stands
   for itself and the decoder above reproduces it exactly. It is larger than what it replaces, which is why
   the sections after it have to move along. blockSize is 16 bits, so it is written in pieces. */
static std::vector<Uint8> bpe_encode_literal(const std::vector<Uint8> &plain) {
    std::vector<Uint8> body;
    for (size_t off = 0; off < plain.size(); off += 65535) {
        size_t n = plain.size() - off < 65535 ? plain.size() - off : 65535;
        body.push_back(255);              // count > 127: skip 128 symbols of the identity table
        body.push_back(128);              // left[128] = 128, which equals itself, so no right byte
        body.push_back(126);              // count, which fills c = 129..255
        for (int c = 129; c < 256; c++) body.push_back((Uint8)c);
        body.push_back((Uint8)(n >> 8));  // blockSize
        body.push_back((Uint8)(n & 0xFF));
        body.insert(body.end(), plain.begin() + off, plain.begin() + off + n);
    }
    std::vector<Uint8> out;
    out.insert(out.end(), {(Uint8)(plain.size() & 0xFF), (Uint8)((plain.size() >> 8) & 0xFF),
                           (Uint8)((plain.size() >> 16) & 0xFF), (Uint8)((plain.size() >> 24) & 0xFF),
                           (Uint8)(body.size() & 0xFF), (Uint8)((body.size() >> 8) & 0xFF),
                           (Uint8)((body.size() >> 16) & 0xFF), (Uint8)((body.size() >> 24) & 0xFF)});
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

/* One text of a decompressed section: where it is, and whether it is one of the dialogs about the card. */
struct Spot { size_t start, end; bool card; std::string text; };

static std::vector<Spot> texts(const std::vector<Uint8> &buf) {
    std::vector<Spot> out;
    size_t i = 0;
    while (i + 5 < buf.size()) {
        if (buf[i] != 0xFF || buf[i + 1] != 0xFE) { i += 2; continue; }
        std::string s;
        size_t p = i + 2;
        for (;;) {
            if (p + 1 >= buf.size()) break;
            Uint16 ch = (Uint16)(buf[p] | (buf[p + 1] << 8));
            p += 2;
            if (ch == 0) break;
            if (ch == 0xFEFF) continue;          // the byte-order mark each line of the game carries
            s += ch == 10 ? '\n' : ch < 128 ? (char)ch : '?';
        }
        while (!s.empty() && (s.front() == ' ' || s.front() == '\n')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '\n')) s.pop_back();
        if (!s.empty()) {
            // two of the texts spell it across a line break ("memory \ncard"), so every run of white space is
            // made one space before looking, the way a search over the text would see it
            std::string flat;
            for (size_t k = 0; k < s.size(); k++) {
                char c = (char)tolower((unsigned char)s[k]);
                if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                if (c == ' ' && !flat.empty() && flat.back() == ' ') continue;
                flat += c;
            }
            out.push_back({i, p, flat.find("memory card") != std::string::npos, s});
        }
        i = p;
    }
    return out;
}

/* One of these files, rewritten: its section decoded, the 31 texts replaced, compressed again and put back with
   the sections after it moved along. Empty when the file is not one of these. */
static std::vector<Uint8> patch_file(const std::vector<Uint8> &original, int &sectionOut) {
    std::vector<Uint8> plain, out;
    Uint32 declared = 0;
    int found = -1;
    for (const Section &s : sections(original)) {
        if (!bpe_decode(original, s.off, plain, declared) || plain.size() != declared) continue;
        int n = 0;
        for (const Spot &t : texts(plain)) n += t.card;
        if (n != kTextCount) continue;
        if (found >= 0) return std::vector<Uint8>();   // two of them: not a file we know how to read
        found = s.index;
    }
    if (found < 0) return std::vector<Uint8>();
    for (const Section &s : sections(original)) {
        if (s.index != found) continue;
        if (!bpe_decode(original, s.off, plain, declared)) return std::vector<Uint8>();
    }
    std::vector<Spot> spots = texts(plain);
    std::vector<Spot> cards;
    for (const Spot &t : spots) if (t.card) cards.push_back(t);
    if ((int)cards.size() != kTextCount) return std::vector<Uint8>();
    for (int k = 0; k < kTextCount; k++) {
        size_t start = cards[k].start, room = cards[k].end - start;
        std::string text = kNewTexts[k];
        text += '\0';                            // UTF-16 as the game stores it: ASCII, \n kept as 10
        if (text.size() * 2 + 2 > room) return std::vector<Uint8>();   // every new text is shorter; this says otherwise
        size_t at = start + 2;
        for (size_t j = 0; j < text.size(); j++) plain[at + j * 2] = (Uint8)text[j];
        for (size_t j = at + text.size() * 2; j < cards[k].end; j++) plain[j] = 0;
    }
    std::vector<Uint8> enc = bpe_encode_literal(plain);
    size_t off = part(original, found), tail = original.size();
    for (const Section &s : sections(original)) if (s.off > off && s.off < tail) tail = s.off;
    long delta = (long)enc.size() - (long)(tail - off);
    out.insert(out.end(), original.begin(), original.begin() + off);
    out.insert(out.end(), enc.begin(), enc.end());
    out.insert(out.end(), original.begin() + tail, original.end());
    for (const Section &s : sections(original)) {
        if (s.off > off) wr32(out, (size_t)s.index * 4, (Uint32)((((long)s.off + delta) >> 2) << 2));
    }
    // read it back the way the game will: the section has to decode, and nothing may still mention the card
    if (!bpe_decode(out, part(out, found), plain, declared) || plain.size() != declared) return std::vector<Uint8>();
    for (const Spot &t : texts(plain)) if (t.card) return std::vector<Uint8>();
    sectionOut = found;
    return out;
}

void step_texts(const std::string &root) {
    std::string data = root + "gamedata/", out = data + "mods/pzs3us1/";
    SDL_PathInfo info;
    say("@step 3 4 Save-file texts");
    if (SDL_GetPathInfo((out + "00473.bin").c_str(), &info)) {
        say("@skip already rewritten");
        return;
    }
    if (!SDL_GetPathInfo((data + "pzs3us1").c_str(), &info)) {
        say("@skip no game data");
        return;
    }
    int files = 0;
    for (const char *name : kTextFiles) {
        size_t size = 0;
        char *raw = (char *)SDL_LoadFile((data + "pzs3us1/" + name + ".bin").c_str(), &size);
        if (raw == NULL) continue;
        std::vector<Uint8> original(raw, raw + size);
        SDL_free(raw);
        int section = -1;
        std::vector<Uint8> patched = patch_file(original, section);
        if (patched.empty()) {
            throw Stop{std::string("The file ") + name + ".bin of the game data is not one this setup knows how to"
                                   " read, so its save-file texts were left as the disc has them.\n"
                                   "The game will still work; its dialogs will keep talking about the card."};
        }
        std::string path = out + name + ".bin";
        SDL_CreateDirectory(out.c_str());
        SDL_IOStream *f = SDL_IOFromFile(path.c_str(), "wb");
        if (f == NULL || !SDL_WriteIO(f, patched.data(), patched.size())) {
            if (f != NULL) SDL_CloseIO(f);
            throw Stop{"The rewritten game file could not be written:\n" + path};
        }
        SDL_CloseIO(f);
        files++;
    }
    if (files == 0) {
        throw Stop{"None of the game files with the save-file texts were found in gamedata/pzs3us1/."};
    }
    // The list the game reads to know which files the setup put in mods/, so they are not counted as game
    // data the player changed (port/src/plat_verify.c).
    SDL_IOStream *list = SDL_IOFromFile((out + "installed.txt").c_str(), "wb");
    if (list != NULL) {
        for (const char *name : kTextFiles) {
            std::string line = std::string("pzs3us1/") + name + ".bin\n";
            SDL_WriteIO(list, line.data(), line.size());
        }
        SDL_CloseIO(list);
    }
    char line[128];
    snprintf(line, sizeof(line), "@ok %d files, in gamedata/mods/", files);
    say(line);
}

void step_test(const std::string &root) {
    std::string exe = root + kGame, saves = root + "gamedata/.selftest";
    const char *args[2] = {exe.c_str(), NULL};
    say("@step 4 4 Self-test");
    say("@note the game's demo fight, no window");
    SDL_Environment *env = SDL_CreateEnvironment(true);
    SDL_SetEnvironmentVariable(env, "BT3_DEMO", "1", true);
    SDL_SetEnvironmentVariable(env, "BT3_SETTINGS", "", true);
    SDL_SetEnvironmentVariable(env, "BT3_SAVES", saves.c_str(), true);
    SDL_SetEnvironmentVariable(env, "BT3_GS", "none", true); // no window: a release program opens one by default
    for (const char *name : {"BT3_REPLAY", "BT3_PAD_PLAY", "BT3_PAD_REC", "BT3_DATA"}) {
        SDL_UnsetEnvironmentVariable(env, name);
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
    SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, root.c_str());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    SDL_Process *proc = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    SDL_DestroyEnvironment(env);
    if (proc == NULL) {
        throw Stop{std::string("The game could not be started: ") + SDL_GetError()};
    }
    size_t size = 0;
    int code = -1;
    char *out = (char *)SDL_ReadProcess(proc, &size, &code);
    std::string text = out != NULL ? std::string(out, size) : "";
    SDL_free(out);
    SDL_DestroyProcess(proc);
    // How the game ended goes into the log too: when it never got as far as printing anything (the system refused
    // to start it, or something stopped it), the exit code is all there is to go on.
    char ended[160];
    const char *why = "";
    switch ((unsigned)code) {
    case 0xC0000135u: why = " (a DLL the program needs was not found)"; break;
    case 0xC0000018u: why = " (the program could not be loaded at its fixed address)"; break;
    case 0xC0000022u: why = " (access denied: often security software blocking the program)"; break;
    case 0xC000007Bu: why = " (a DLL of the wrong kind was loaded)"; break;
    case 0xC0000005u: why = " (crashed: bad memory access)"; break;
    case 0xC0000409u: why = " (stopped by a security check)"; break;
    default: break;
    }
    snprintf(ended, sizeof(ended), "\n[setup] the game exited with code %d (0x%08X)%s\n", code, (unsigned)code, why);
    text += ended;
    {   // a crash report the game wrote itself
        size_t n = 0;
        char *crash = (char *)SDL_LoadFile((root + "bt3_crash.txt").c_str(), &n);
        if (crash != NULL) {
            text += "[setup] bt3_crash.txt:\n" + std::string(crash, n);
            SDL_free(crash);
        }
    }
    SDL_IOStream *log = SDL_IOFromFile((root + "install.log").c_str(), "wb");
    if (log != NULL) {
        SDL_WriteIO(log, text.data(), text.size());
        SDL_CloseIO(log);
    }
    if (text.find("battle finished") == std::string::npos) {
        size_t end = text.find_last_not_of("\r\n"), begin = end == std::string::npos ? std::string::npos : text.rfind('\n', end);
        std::string last = end == std::string::npos ? "no output" : text.substr(begin == std::string::npos ? 0 : begin + 1, end - (begin == std::string::npos ? 0 : begin + 1) + 1);
        throw Stop{"The game's demo fight did not finish. Its last line was:\n" + last + "\nThe full output is in install.log."};
    }
    say("@ok the demo fight ran to its end");
}

void work(std::string root, std::string path) {
    try {
        Iso iso;
        step_disc(iso, path);
        check_cancel();
        step_data(iso, root);
        check_cancel();
        step_texts(root);
        check_cancel();
        step_test(root);
        say("@done " + root + kGame);
    } catch (const Stop &s) {
        std::string why = s.why;
        for (size_t i = 0; (i = why.find('\n', i)) != std::string::npos;) {
            why.replace(i, 1, "\\n");
        }
        say("@fail");
        say("@stopped " + why);
    }
    sRunning = false;
}

} // namespace

void Native_Start(const std::string &root, const std::string &iso) {
    if (sThread.joinable()) {
        sThread.join();
    }
    sCancel = false;
    sRunning = true;
    sThread = std::thread(work, root, iso);
}

bool Native_Poll(std::string &line) {
    std::lock_guard<std::mutex> lock(sLock);
    if (sLines.empty()) {
        return false;
    }
    line = sLines.front();
    sLines.erase(sLines.begin());
    return true;
}

bool Native_Running() { return sRunning; }

void Native_Cancel() { sCancel = true; }

void Native_Join() {
    sCancel = true;
    if (sThread.joinable()) {
        sThread.join();
    }
}

bool Native_Installed(const std::string &root) {
    SDL_PathInfo info;
    return SDL_GetPathInfo((root + "gamedata/.installed").c_str(), &info);
}
