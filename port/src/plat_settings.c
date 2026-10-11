/*
 * The player's settings, kept between runs in a small text file of `name=number` lines:
 *
 *     bt3_settings.txt in the current directory, or the path in $BT3_SETTINGS (an empty $BT3_SETTINGS: no file)
 *
 * The settings overlay (port/src/gs/gs_gpu.c) writes it whenever a setting changes. At start an environment
 * variable (BT3_SCALE, BT3_ASPECT, ...) still wins over the file, so a single run can be started differently.
 * Names: scale, aspect_milli, fullscreen, fx_off, glow, music, effects, display; the bindings of port/src/gs/gs_input.c
 * (key_p1_cross = an SDL scancode, pad_p1_cross = a controller button, padslot_p1 = which controller).
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SETTINGS 200
#define MAX_STRINGS 16
static struct { char name[24]; int value; } sSet[MAX_SETTINGS];
/* A few settings are text, not a number: a Discord application id is eighteen digits, which is wider than any
   integer here. They are kept apart so the number ones stay numbers in the file, and written after them. */
static struct { char name[24]; char value[64]; } sStr[MAX_STRINGS];
static int sStrCount;
static int sDirty;
static int sCount;
static pthread_once_t sOnce = PTHREAD_ONCE_INIT;
static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;

static const char *path(void) {
    const char *p = getenv("BT3_SETTINGS");
    return p == NULL ? "bt3_settings.txt" : p[0] == '\0' ? NULL : p;
}

static void load(void) {
    char line[128], name[24];
    int value;
    FILE *fp = path() != NULL ? fopen(path(), "r") : NULL;

    while (fp != NULL && fgets(line, sizeof(line), fp) != NULL && sCount < MAX_SETTINGS) {
        if (sscanf(line, " %23[a-z0-9_]=%d", name, &value) == 2) {
            strcpy(sSet[sCount].name, name);
            sSet[sCount++].value = value;
        } else if (sStrCount < MAX_STRINGS && sscanf(line, " %23[a-z0-9_]=%63[^\n]", name, sStr[sStrCount].value) == 2) {
            /* a line that is not a number: name=text, the Discord application id among them */
            strcpy(sStr[sStrCount].name, name);
            sStrCount++;
        }
    }
    if (fp != NULL) {
        fclose(fp);
    }
}

/* The saved value of a setting, or `def` when the file does not have it. */
int Port_Setting(const char *name, int def) {
    int i, v = def;

    pthread_once(&sOnce, load);
    pthread_mutex_lock(&sLock);
    for (i = 0; i < sCount; i++) {
        if (strcmp(sSet[i].name, name) == 0) {
            v = sSet[i].value;
        }
    }
    pthread_mutex_unlock(&sLock);
    return v;
}

/* Stores a setting; Port_SettingsWrite puts the changes into the file. */
void Port_SettingSave(const char *name, int value) {
    int i;

    pthread_once(&sOnce, load);
    pthread_mutex_lock(&sLock);
    for (i = 0; i < sCount && strcmp(sSet[i].name, name) != 0; i++) {
    }
    if (i < MAX_SETTINGS) {
        if (i == sCount) {
            snprintf(sSet[sCount++].name, sizeof(sSet[0].name), "%s", name);
            sDirty = 1;
        }
        if (sSet[i].value != value) {
            sSet[i].value = value;
            sDirty = 1;
        }
    }
    pthread_mutex_unlock(&sLock);
}

/* A saved text setting, or `def`. The returned string is the settings' own; it is read under the lock and
   stays valid until the next call that changes it. */
const char *Port_SettingStr(const char *name, const char *def) {
    static char got[64];
    int i;

    pthread_once(&sOnce, load);
    pthread_mutex_lock(&sLock);
    snprintf(got, sizeof(got), "%s", def);
    for (i = 0; i < sStrCount; i++) {
        if (strcmp(sStr[i].name, name) == 0) {
            snprintf(got, sizeof(got), "%s", sStr[i].value);
        }
    }
    pthread_mutex_unlock(&sLock);
    return got;
}

/* Stores a text setting; Port_SettingsWrite puts it in the file. */
void Port_SettingSaveStr(const char *name, const char *value) {
    int i;

    pthread_once(&sOnce, load);
    pthread_mutex_lock(&sLock);
    for (i = 0; i < sStrCount && strcmp(sStr[i].name, name) != 0; i++) {
    }
    if (i < MAX_STRINGS) {
        if (i == sStrCount) {
            snprintf(sStr[sStrCount].name, sizeof(sStr[sStrCount].name), "%s", name);
            sStrCount++;
        }
        if (strcmp(sStr[i].value, value) != 0) {
            snprintf(sStr[i].value, sizeof(sStr[i].value), "%s", value);
            sDirty = 1;
        }
    }
    pthread_mutex_unlock(&sLock);
}

void Port_SettingsWrite(void) {
    FILE *fp;
    int i;

    pthread_mutex_lock(&sLock);
    fp = sDirty && path() != NULL ? fopen(path(), "w") : NULL;
    if (fp != NULL) {
        for (i = 0; i < sCount; i++) {
            fprintf(fp, "%s=%d\n", sSet[i].name, sSet[i].value);
        }
        for (i = 0; i < sStrCount; i++) {
            fprintf(fp, "%s=%s\n", sStr[i].name, sStr[i].value);
        }
        fclose(fp);
    }
    sDirty = 0;
    pthread_mutex_unlock(&sLock);
}
