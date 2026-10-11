/* Autosave: the game asks, at the end of a chapter and in a few other places, whether to save, and waits for an
   answer. On a PC that question is noise: the save is wanted every time. With this on, the questions are answered
   for the player and the flow goes straight to the write, which shows "Auto Saving" while it happens (the text of
   the game's own messages 0xA and 0x10, which only ever show while saving).

   The game itself is not changed beyond three small `#ifdef PORT` branches in src/sys/mcflow_a.c (the three
   states of the save flow that wait for an answer), each of which jumps where a "Yes" would have taken it.

   On by default. Off with BT3_AUTOSAVE=0, or with the `autosave` setting at 0 (the port's settings file). */
#include <stdlib.h>

extern int Port_Setting(const char *name, int def);  /* plat_settings.c */
extern void Port_SettingSave(const char *name, int value);
extern void Port_SettingsWrite(void);

static int sOn = -1;

int Port_Autosave(void) {
    const char *env;

    if (sOn >= 0) {
        return sOn;
    }
    env = getenv("BT3_AUTOSAVE");
    sOn = env != NULL ? atoi(env) != 0 : Port_Setting("autosave", 1) != 0;
    return sOn;
}

/* From the settings window (F1): changes it for this run and keeps it for the next. */
void Port_AutosaveSet(int on) {
    sOn = on != 0;
    Port_SettingSave("autosave", sOn);
    Port_SettingsWrite();
}