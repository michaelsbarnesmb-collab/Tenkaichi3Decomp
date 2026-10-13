/* Characters added past the original's 161 (those of the patch SLUS-21978: port/src/plat_chars.c). Where the game
   works out a character's file from its number it asks Port_CharaFile, which leaves the original's id for the
   original's characters and gives an added character's own. */
#ifndef PORT_PLAT_CHARS_H
#define PORT_PLAT_CHARS_H

/* A character's files (`slot` of Port_CharaFile); a model is its costume, + 4 for the damaged one. */
#define PORT_CHARA_MODEL 0     /* 0x590 + chara * 10 + costume (+ 4) */
#define PORT_CHARA_ANIM 8      /* 0x598 + chara * 10 */
#define PORT_CHARA_EFFECTS 9   /* 0x599 + chara * 10 */
#define PORT_CHARA_DATA 10     /* 8 + chara * 2 + side (+ side) */
#define PORT_CHARA_PORTRAIT 12 /* 0x2F9 + chara */
#define PORT_CHARA_BANK 13     /* 0xC7B + chara (the patch has no English: its Spanish) */
#define PORT_CHARA_BANK_JP 14  /* 0xBDA + chara */

int Port_CharaAdded(int chara);
int Port_CharaFile(int id, int chara, int slot);
int Port_CharaOfFile(int id);
void *Port_ChrTblSection(void *file, void *section);
void *Port_ParamTbl(void *section);

#endif
