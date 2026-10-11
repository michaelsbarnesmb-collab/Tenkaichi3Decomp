/* The fade, the animation's clock and the looped sound of the "Connecting..." screen (see net_screen.h). The
   drawing is Dear ImGui and lives in ui.cpp; the presenting during the game thread's wait lives in gs_draw.c
   (Gs_PresentIdle). */
#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "net_screen.h"

extern int GsGpu_Enabled(void);               /* gs_core.c */
extern void Gs_PresentIdle(void);             /* gs_draw.c: a black frame plus the overlay, no frame of the game */
extern SDL_AudioDeviceID Port_AudioDevice(void); /* snd_adx.c */
extern int Ui_Event(const SDL_Event *ev);     /* ui.cpp */
extern int PortExtras_Dir(const char *kind, const char *env, char *out, unsigned size); /* plat_extras.c */

static void loop_start(void);
static void loop_stop(void);
static void sound_find(void);
void Net_ScreenPump(void);

/* ---- the fade ---- */

enum { OFF = 0, IN = 1, ON = 2, OUT = 3 };
#define FADE_IN_MS 300u  /* how long the picture takes to go black */
#define FADE_OUT_MS 450u /* and to come back */
#define FOUND_MS 1400u   /* how long "Opponent Found!" is held before the black fades away */

static volatile int sState; /* OFF / IN / ON / OUT */
static uint32_t sSince;     /* when the state last changed */
static int sAlpha;          /* how black, 0..255 */
static int sFound;          /* the match is ready (Net_ScreenFound): the line under the radar says so */
static uint32_t sFoundAt;   /* when it was found */
static int sEndReq;         /* Net_ScreenEnd arrived while "Opponent Found!" was still being held */

static uint32_t now_ms(void) {
    return SDL_GetTicks();
}

int Net_ScreenState(void) {
    return sState;
}

float Net_ScreenPhase(void) {
    /* the animation's clock: seconds since the screen opened, so it moves while it is up */
    return sState == OFF ? 0.0f : (float)(now_ms() - sSince) / 1000.0f;
}

static void set_state(int st) {
    sState = st;
    sSince = now_ms();
    if (st == OFF) {
        sAlpha = 0;
        sFound = 0;
        sEndReq = 0;
        loop_stop();
    }
}

/* Moves the fade on with the clock. The overlay reads the alpha every frame (lobby) and the pump reads it while the
   game thread waits (handshake), so either path animates it. */
static void advance(void) {
    uint32_t d = now_ms() - sSince;

    if (sState == IN) {
        sAlpha = d >= FADE_IN_MS ? 255 : (int)((uint64_t)d * 255u / FADE_IN_MS);
        if (d >= FADE_IN_MS) {
            sState = ON;
        }
    } else if (sState == OUT) {
        sAlpha = d >= FADE_OUT_MS ? 0 : 255 - (int)((uint64_t)d * 255u / FADE_OUT_MS);
        if (d >= FADE_OUT_MS) {
            set_state(OFF);
        }
    } else if (sState == ON) {
        sAlpha = 255;
    }
}

int Net_ScreenAlpha(void) {
    /* the end that was asked for while "Opponent Found!" was on runs once that moment has passed */
    if (sEndReq && (!sFound || now_ms() - sFoundAt >= FOUND_MS)) {
        sEndReq = 0;
        sAlpha = 255;
        set_state(OUT);
    }
    advance();
    return sAlpha;
}

void Net_ScreenFound(void) {
    if (sState == OFF || sFound) {
        return;
    }
    sFound = 1;
    sFoundAt = now_ms();
}

int Net_ScreenIsFound(void) {
    return sFound;
}

/* The game is not drawing at this point (the session's start-up is run ahead, without picture), so the moment
   "Opponent Found!" is on is the one this makes: it presents the screen with the radar still going. */
void Net_ScreenHoldFound(void) {
    uint32_t until = now_ms() + FOUND_MS;

    while (now_ms() < until) {
        Net_ScreenPump();
        SDL_Delay(4);
    }
}

void Net_ScreenBegin(void) {
    sound_find();
    sFound = 0;
    sEndReq = 0;
    if (sState == OFF) {
        set_state(IN);
        loop_start();
    } else if (sState == OUT) {
        /* it was closing (the match came up and then went away again): keep the black and go on waiting */
        sState = ON;
        sAlpha = 255;
    } else {
        advance();
    }
}

void Net_ScreenEnd(void) {
    if (sState == OFF || sState == OUT) {
        return; /* nothing to close, or already closing */
    }
    if (sFound && now_ms() - sFoundAt < FOUND_MS) {
        sEndReq = 1; /* let "Opponent Found!" be seen before the black fades away */
        return;
    }
    /* Snap to black first (it may have been only half faded in), then fade away from there: set_state(OUT)
       restarts the clock, so the fade-out begins at full black whatever the alpha was. */
    sAlpha = 255;
    set_state(OUT);
}

/* ---- the looped sound ---- */

static char sSound[1024];
static SDL_AudioStream *sLoop;
static uint8_t *sLoopData;
static uint32_t sLoopLen, sLoopPos;

/* SDL's audio thread wants `additional` more bytes: the file goes round and round. It is bound to the same device
   the game uses, so it is heard with whatever else is playing and it keeps going while the game thread is stopped
   (the stream is fed from SDL's audio thread, not from the game's vertical blank). */
static void SDLCALL loop_feed(void *user, SDL_AudioStream *stream, int additional, int total) {
    (void)user;
    (void)total;
    while (additional > 0 && sLoopData != NULL) {
        uint32_t n;
        if (sLoopPos >= sLoopLen) {
            sLoopPos = 0;
        }
        n = sLoopLen - sLoopPos;
        if (n > (uint32_t)additional) {
            n = (uint32_t)additional;
        }
        SDL_PutAudioStreamData(stream, sLoopData + sLoopPos, (int)n);
        sLoopPos += n;
        additional -= (int)n;
    }
}

static void loop_stop(void) {
    if (sLoop != NULL) {
        SDL_DestroyAudioStream(sLoop); /* waits for a running callback */
        sLoop = NULL;
    }
    free(sLoopData);
    sLoopData = NULL;
    sLoopLen = sLoopPos = 0;
}

static void loop_start(void) {
    SDL_AudioSpec spec;
    SDL_AudioDeviceID dev;

    if (sLoop != NULL || sSound[0] == '\0') {
        return; /* already on, or asked for silence */
    }
    dev = Port_AudioDevice();
    if (dev == 0) {
        return; /* no sound (BT3_NOSOUND, or it has not been opened yet); the screen still fades */
    }
    if (!SDL_LoadWAV(sSound, &spec, &sLoopData, &sLoopLen)) {
        fprintf(stderr, "bt3: net: %s is not a sound file SDL can read: %s\n", sSound, SDL_GetError());
        return;
    }
    if (sLoopLen == 0 || spec.channels <= 0 || spec.freq <= 0) {
        free(sLoopData);
        sLoopData = NULL;
        return;
    }
    sLoopPos = 0;
    sLoop = SDL_CreateAudioStream(&spec, NULL);
    if (sLoop == NULL) {
        fprintf(stderr, "bt3: net: sound stream: %s\n", SDL_GetError());
        free(sLoopData);
        sLoopData = NULL;
        return;
    }
    SDL_SetAudioStreamGetCallback(sLoop, loop_feed, NULL);
    if (!SDL_BindAudioStream(dev, sLoop)) {
        fprintf(stderr, "bt3: net: sound stream: %s\n", SDL_GetError());
        loop_stop();
        return;
    }
    if (getenv("BT3_NET_SOUND_LOG") != NULL) {
        fprintf(stderr, "bt3: net: looping %s: %u ch, %d Hz, %.2f s\n", sSound, spec.channels, spec.freq,
                (double)sLoopLen / (double)(spec.channels * 4 * spec.freq));
    }
}

void Net_ScreenSoundSet(const char *path) {
    int was;

    if (path == NULL || path[0] == '\0') {
        sSound[0] = '\0';
        loop_stop();
        return;
    }
    if (strcmp(path, sSound) == 0) {
        return;
    }
    snprintf(sSound, sizeof(sSound), "%s", path);
    was = sLoop != NULL;
    loop_stop();
    if (was) {
        loop_start(); /* a new file while the screen is up: swap it straight away */
    }
}

/* Where the loop comes from, read once: BT3_NET_SOUND says the file outright; otherwise it is net.wav in the
   folder the added sounds live in (the same place the added stages and songs are looked for: the "sounds" folder,
   or whatever BT3_SOUNDS / BT3_DATA point at). */
static void sound_find(void) {
    static int read;
    const char *env;
    char dir[512], path[640];
    FILE *fp;

    if (read) {
        return;
    }
    read = 1;
    env = getenv("BT3_NET_SOUND");
    if (env != NULL && env[0] != '\0') {
        Net_ScreenSoundSet(env);
        return;
    }
    if (!PortExtras_Dir("sounds", "BT3_SOUNDS", dir, sizeof(dir))) {
        return; /* no folder of added sounds: the screen is silent unless BT3_NET_SOUND says otherwise */
    }
    snprintf(path, sizeof(path), "%s/net.wav", dir);
    fp = fopen(path, "rb");
    if (fp == NULL) {
        return;
    }
    fclose(fp);
    Net_ScreenSoundSet(path);
    if (getenv("BT3_NET_SOUND_LOG") != NULL) {
        fprintf(stderr, "bt3: net: the connecting sound is %s\n", path);
    }
}

/* ---- presenting while the game thread waits ---- */

void Net_ScreenPump(void) {
    static uint32_t last;
    uint32_t now;

    if (sState == OFF || !GsGpu_Enabled()) {
        return; /* nothing to show (headless tests, or the screen never opened) */
    }
    now = now_ms();
    if (last != 0 && now - last < 16) {
        return; /* the handshake loop calls this every iteration; about 60 a second is enough */
    }
    last = now;
    advance();
    Gs_PresentIdle();
}