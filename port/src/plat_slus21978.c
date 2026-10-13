/*
 * Extras from the fan patch SLUS-21978, for a player who also has its disc image: taken from it by
 * port/tools/extract_slus21978.py into <data>/slus21978/. Without that folder nothing here does anything and the game is the
 * original. So far: its Latin-American Spanish voices, a third choice of the options' voice language.
 *
 * The choice is not in the save (that keeps the original's two languages, one bit): the save says English, and the
 * setting voice_latino=1 (bt3_settings.txt) plays the Spanish file of each English line that has one
 * (port/src/gs/snd_adx.c). Online the game computes with English on both sides anyway; what a player hears is his.
 */
#include <stdio.h>
#include <sys/stat.h>

extern const char *Port_FileRoot(void);              /* plat_file.c: BT3_DATA or "gamedata" */
extern int Port_Setting(const char *name, int def);  /* plat_settings.c */
extern void Port_SettingSave(const char *name, int value);
extern void Port_SettingsWrite(void);

/* 1 when the patch's voices were taken (the folder of their files is there). Looked at once. */
int Port_Slus21978Voices(void) {
    static int known = -1;

    if (known < 0) {
        char path[512];
        struct stat st;
        snprintf(path, sizeof(path), "%s/slus21978/pzs3us2", Port_FileRoot());
        known = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
        if (known) {
            fprintf(stderr, "bt3: the voices of the patch SLUS-21978 are there (%s)\n", path);
        }
    }
    return known;
}

/* The voices in Spanish (Latin America) were chosen, and are there. */
int Port_VoiceLatino(void) {
    return Port_Slus21978Voices() && Port_Setting("voice_latino", 0) != 0;
}

void Port_SetVoiceLatino(int on) {
    if ((Port_Setting("voice_latino", 0) != 0) != (on != 0)) {
        Port_SettingSave("voice_latino", on != 0);
        Port_SettingsWrite();
    }
}

/* The files the patch has in its own version are read from <data>/slus21978/ instead of the original's (plat_fcache.c):
   the fighters' voice banks (their shouts and hits) in Spanish. Not online, where the game's memory has to be the
   same on both sides (a bank is loaded into it); the streamed lines are another matter (snd_adx.c). */
int Port_Slus21978Files(void) {
    extern int Port_NetSession(void); /* gs/net.c */
    return Port_VoiceLatino() && !Port_NetSession();
}

/* The Spanish file of a voice line of partition 2 (file number `fid`), into `buf`; 0 when it has none. */
int Port_Slus21978VoicePath(int fid, char *buf, int size) {
    FILE *fp;

    snprintf(buf, (size_t)size, "%s/slus21978/pzs3us2/%05d.bin", Port_FileRoot(), fid);
    fp = fopen(buf, "rb");
    if (fp == NULL) {
        return 0;
    }
    fclose(fp);
    return 1;
}
