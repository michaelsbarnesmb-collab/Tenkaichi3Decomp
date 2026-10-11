/* The screen shown while an online match is being set up (Dragon Net Battle): the picture fades to black and a
   "Connecting..." animation plays until the two games are in step, then the black fades away and the match's
   picture comes back. The wait happens in two places, and both are covered:
     - the lobby (finding the other player; the game keeps drawing, so ui.cpp draws this over the picture each
       frame through frame_build);
     - the handshake (the game thread waits inside net.c's net_start and the game draws nothing at all, so
       Net_ScreenPump presents a black frame plus this overlay from inside that wait).
   The state lives here; the drawing is in ui.cpp (it is Dear ImGui) and is called from both places above.
   A sound file can be given to loop underneath (Net_ScreenSoundSet, or BT3_NET_SOUND in the environment). It is
   put on the same SDL device the game uses (Port_AudioDevice, snd_adx.c), so it goes on playing while the game
   thread is stopped waiting.
   This file owns only the fade, the animation's clock and the looped sound; the lobby (ui.cpp) and the handshake
   (net.c) decide when the screen comes and goes. */
#ifndef NET_SCREEN_H
#define NET_SCREEN_H

#include <stdint.h>

/* 0 nothing, 1 fading in, 2 on (black + animation), 3 fading out. */
int Net_ScreenState(void);
/* Starts the screen: fade to black and begin the looped sound. Calling it again while it is up is harmless. */
void Net_ScreenBegin(void);
/* The other player is there and the two games are in step: the line under the radar reads "Opponent Found!" for a
   moment before the black fades away (a Net_ScreenEnd that arrives first waits for that moment to pass). */
void Net_ScreenFound(void);
int Net_ScreenIsFound(void);
/* Holds the screen showing "Opponent Found!" for its moment, presenting frames: the game is not drawing (the
   session's start-up is run ahead, without picture), so this is what makes the message actually be seen. */
void Net_ScreenHoldFound(void);
/* The match is ready and the game is drawing again: fade the black away and stop the loop. */
void Net_ScreenEnd(void);

/* Where the looped sound comes from: a path to an audio file (anything SDL can decode) or NULL/"" for silence.
   Asking before there is a sound device is fine: it is remembered and used the next time the screen opens. */
void Net_ScreenSoundSet(const char *path);

/* How black the picture is now, 0 (clear) to 255, and the clock for the animation, in seconds. The overlay reads
   both to draw (ui.cpp). */
int Net_ScreenAlpha(void);
float Net_ScreenPhase(void);

/* Presents a black frame plus the overlay while the game thread is stopped waiting (net.c's handshake). Does
   nothing when there is no window (headless tests) or when the renderer is not up. Paced to the refresh rate so
   the handshake loop can call it every iteration. */
void Net_ScreenPump(void);

/* The test trigger (F7 / F8, in ui.cpp's Ui_Event; not during an online match). It plays the whole thing without
   a second player, so the fade, the radar and the sound can be seen and heard on their own:
     - F7: the full cycle on its own — searching, then "Opponent Found!", then the black fading away;
     - F8: the screen by hand: the first press opens it, the next closes it. */
void Net_ScreenDemoStart(void); /* F7 */
void Net_ScreenDemoStop(void);  /* F8: opens it if it is closed, closes it if it is up */
void Net_ScreenDemoTick(void);  /* advances the F7 sequence (called each frame the screen is up) */

#endif /* NET_SCREEN_H */