/* Characters added past the original's 161: those of the patch SLUS-21978, when the player took them from their
   disc image (port/tools/slus21978_chars.py writes <data>/slus21978/chars/<c>/ and chars.txt). A character keeps
   the patch's own number (165..249).

   The original works out a character's files from its number (model 0x590 + c * 10 + k, ...); past 160 those land
   on other files. The game asks Port_CharaFile instead where it loads one (#ifdef PORT): for an added character
   it gets a file id past the disc's (70000.. are the added stages, 73000.. the added songs, 80000.. these) that
   the file layer serves from the character's folder (PortChars_Alias). Its entries of the character table and of
   the fighters' parameters are in tables of the port's own (Port_ChrTblSection, Port_ParamTbl), the original's
   161 / 164 entries copied, so the game's own files and buffers stay as they are.

   Nothing of this is used without that folder, or in an online session (both copies have to be the same game): the
   numbers and tables are then the original's. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHARAS 161          /* the original's */
#define CHARA_MAX 256       /* a character's number is a byte in the game's records */
#define TABLE_SIZE 0x3C     /* an entry of the character table (common file 4, part 1) */
#define PARAM_SIZE 16       /* an entry of the fighters' parameters (common file 2, part 8) */
#define PARAM_ORIGINAL 164  /* how many the original has */
#define FILE_INDEX 80000    /* the second archive's file 80000 (id 0xD48 + 80000) is the first added character's first */
#define FILE_SLOTS 16       /* per character: 0..7 models (costume, + 4 damaged), then the PORT_CHARA_* below */

extern void *Port_LowAlloc(size_t size); /* plat_mem.c: an address the game can keep in 4 bytes */
extern int Port_NetSession(void);        /* gs/net.c */

static const char *sSlotFile[FILE_SLOTS] = {
    "model0.bin", "model1.bin", "model2.bin", "model3.bin", "model4.bin", "model5.bin", "model6.bin", "model7.bin",
    "anim8.bin", "anim9.bin", "data0.bin", "data1.bin", "portrait.bin", "bank.bin", "bank_jp.bin", NULL,
};

static int sDone;
static char sDir[256];
static unsigned char sAdded[CHARA_MAX];
static unsigned char sTable[CHARA_MAX][TABLE_SIZE + PARAM_SIZE];
static char sName[CHARA_MAX][48];
static int sCount;

static void init(void) {
    char path[320], line[128];
    FILE *fp;

    if (sDone) {
        return;
    }
    sDone = 1;
    {
        extern const char *Port_FileRoot(void); /* plat_file.c */
        snprintf(sDir, sizeof(sDir), "%s/slus21978/chars", Port_FileRoot());
    }
    snprintf(path, sizeof(path), "%s/chars.txt", sDir);
    if ((fp = fopen(path, "rb")) == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *bar = strchr(line, '|'), *end;
        int c = (int)strtol(line, &end, 10);
        FILE *t;
        if (line[0] == '#' || bar == NULL || end != bar || c < CHARAS || c >= CHARA_MAX) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%d/table.bin", sDir, c);
        if ((t = fopen(path, "rb")) == NULL) {
            continue;
        }
        if (fread(sTable[c], 1, sizeof(sTable[c]), t) == sizeof(sTable[c])) {
            sAdded[c] = 1;
            bar[strcspn(bar, "\r\n")] = '\0';
            snprintf(sName[c], sizeof(sName[c]), "%s", bar + 1);
            sCount++;
        }
        fclose(t);
    }
    fclose(fp);
    if (sCount > 0) {
        fprintf(stderr, "bt3: characters: %d of the patch SLUS-21978 added from %s\n", sCount, sDir);
    }
}

/* Whether `chara` is an added character the game may use now. */
int Port_CharaAdded(int chara) {
    init();
    return chara >= CHARAS && chara < CHARA_MAX && sAdded[chara] && !Port_NetSession();
}

/* The file id the game loads for `chara`'s file `slot` (PORT_CHARA_* in the game's headers; a model: its costume,
   + 4 for the damaged one): `id`, the original's, unless `chara` is an added character. */
int Port_CharaFile(int id, int chara, int slot) {
    if (!Port_CharaAdded(chara) || slot < 0 || slot >= FILE_SLOTS || sSlotFile[slot] == NULL) {
        return id;
    }
    return 0xD48 + FILE_INDEX + (chara - CHARAS) * FILE_SLOTS + slot;
}

/* The character an added character's file id stands for (the game works a model's character out of its file id),
   or -1. */
int Port_CharaOfFile(int id) {
    int i = id - 0xD48 - FILE_INDEX;
    return i >= 0 && i < (CHARA_MAX - CHARAS) * FILE_SLOTS ? CHARAS + i / FILE_SLOTS : -1;
}

/* The file layer: the host file of an added character's file (its path under the data root, "pzs3us2/NNNNN.bin"). */
int PortChars_Alias(const char *rel, char *out, unsigned n) {
    int index, chara, slot;

    /* (also with no folder: the demo battle (BT3_DEMO) loads before the game has mounted the second archive) */
    const char *num = strncmp(rel, "pzs3us2/", 8) == 0 ? rel + 8 : rel[0] == '/' ? rel + 1 : NULL;

    if (num == NULL || sscanf(num, "%d", &index) != 1 || index < FILE_INDEX) {
        return 0;
    }
    init();
    chara = CHARAS + (index - FILE_INDEX) / FILE_SLOTS;
    slot = (index - FILE_INDEX) % FILE_SLOTS;
    if (chara >= CHARA_MAX || !sAdded[chara] || sSlotFile[slot] == NULL) {
        return 0;
    }
    snprintf(out, n, "%s/%d/%s", sDir, chara, sSlotFile[slot]);
    return 1;
}

/* The character table (common file 4, part 1) as the game reads it: `section` is where a reader found a part of
   that file; for the characters' part, with added characters, the port's copy with room for them. */
void *Port_ChrTblSection(void *file, void *section) {
    static unsigned char *table;
    static void *from;

    init();
    if (sCount == 0 || Port_NetSession() || file == NULL ||
        (unsigned char *)section != (unsigned char *)file + (((uint32_t *)file)[1] >> 2 << 2)) {
        return section;
    }
    if (table == NULL) {
        table = Port_LowAlloc(CHARA_MAX * TABLE_SIZE);
    }
    if (from != section) { /* (the game's buffer: the same file each time it is read) */
        int c;
        from = section;
        memset(table, 0, CHARA_MAX * TABLE_SIZE);
        memcpy(table, section, CHARAS * TABLE_SIZE);
        for (c = CHARAS; c < CHARA_MAX; c++) {
            if (sAdded[c]) {
                memcpy(table + c * TABLE_SIZE, sTable[c], TABLE_SIZE);
            }
        }
    }
    return table;
}

/* The fighters' parameters (common file 2, part 8: health, ki, blast stocks), the same way. */
void *Port_ParamTbl(void *section) {
    static unsigned char *table;
    static void *from;

    init();
    if (sCount == 0 || Port_NetSession() || section == NULL) {
        return section;
    }
    if (table == NULL) {
        table = Port_LowAlloc(CHARA_MAX * PARAM_SIZE);
    }
    if (from != section) {
        int c;
        from = section;
        memset(table, 0, CHARA_MAX * PARAM_SIZE);
        memcpy(table, section, PARAM_ORIGINAL * PARAM_SIZE);
        for (c = CHARAS; c < CHARA_MAX; c++) {
            if (sAdded[c]) {
                memcpy(table + c * PARAM_SIZE, sTable[c] + TABLE_SIZE, PARAM_SIZE);
            }
        }
    }
    return table;
}

/* An added character's name ("" for another). */
const char *Port_CharaName(int chara) {
    init();
    return chara >= 0 && chara < CHARA_MAX && sAdded[chara] ? sName[chara] : "";
}
