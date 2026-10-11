/* Discord Rich Presence: what the player is doing, shown on their Discord profile.
 *
 * How it works. A Discord desktop client running on this machine opens a local socket -- a unix domain socket
 * on Linux and macOS, a named pipe on Windows -- and a program connects to it and sends JSON commands. There is
 * no internet connection, no account and no API key involved: everything stays on this machine. Discord's own
 * documentation calls writing this yourself the "hard mode" and recommends it for Rich Presence, and that is
 * what this is. The Game SDK was archived and discord-rpc was deprecated in 2019; neither is used here.
 *
 * Each message is a frame: a 32-bit little-endian opcode, a 32-bit little-endian length, then that many bytes
 * of JSON. Opcode 0 is the handshake, 1 carries the commands, 3 is a ping that must be answered with 4. The
 * client may be on any socket from discord-ipc-0 to discord-ipc-9 (several can run at once), so each is tried.
 *
 * The presence needs a Discord application id, an 18-digit number the application is registered under. It is
 * read from $BT3_DISCORD_ID; without it this file does nothing at all and the game is unaffected. The name
 * Discord shows at the top of the card is the application's, not ours, so it is whatever the application was
 * created with; the two lines below it are the activity's `details` and `state`, which are ours.
 *
 * All the socket work is on this module's own thread: connecting can block, and the game thread must not wait
 * for anything. It is told what to show through Port_DiscordTick, which the game calls each frame
 * (headless.c's __wrap_Progress_Main) and which only stores two numbers.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

/* What the player is doing. Battle_GetWork()->running is 1 between Battle_Main's entry and its exit, and
   gProgress->mode is the screen the menu dispatcher is on (6..10 is the story, menu_a_b.c). Both are the
   game's own, read here as the structures are declared there rather than by including them: this file must
   keep building whatever the game's headers grow. */
typedef struct { char pad[0x18]; int32_t mode; } PortProgress;
typedef struct { char pad[0x19F8]; int32_t running; } PortBattleWork;

extern int Port_Setting(const char *name, int def);          /* plat_settings.c */
extern void Port_SettingSave(const char *name, int value);
extern const char *Port_SettingStr(const char *name, const char *def);
extern void Port_SettingSaveStr(const char *name, const char *value);
extern void Port_SettingsWrite(void);
extern void *gProgress;                  /* menu_a.h: MenuProgress * */
extern void *Battle_GetWork(void);       /* battle_work.c */
extern uint32_t Port_Pid(void);          /* plat_sys.c, or the host's own on Windows */

#define DISCORD_NAME "DBZ BT3 Decomp"
#define DISCORD_SEND_MS 15000u            /* Discord allows 5 updates per 20 s; stay well inside that */

/* what to show: the three the game distinguishes, in the order they can change */
static const char *kDetails[3] = { "En los men\u00fas", "En la historia", "En una pelea" };
#define D_MENU 0
#define D_STORY 1
#define D_FIGHT 2

/* ---------------------------------------------------------------------------------------------------- the link */

static volatile int sWant = -1;      /* what to show next, -1 = nothing yet */
static volatile int sRunning;
static volatile int sQuit;
static char sClientId[32];
static int sLog;                     /* BT3_DISCORD_LOG=1: say what happens */
static volatile int sEnabled;        /* F1 > Controls: show the presence or not */
static void note(const char *what) {
    if (sLog) {
        fprintf(stderr, "bt3: discord: %s\n", what);
    }
}

#ifdef _WIN32
typedef HANDLE PortIpc;
#define kBadPipe INVALID_HANDLE_VALUE
static PortIpc ipc_open(int n) {
    char name[64];
    HANDLE h;
    snprintf(name, sizeof(name), "\\\\.\\pipe\\discord-ipc-%d", n);
    h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    return h;
}
static int ipc_send(PortIpc h, const void *buf, int n) {
    DWORD got = 0;
    return WriteFile(h, buf, (DWORD)n, &got, NULL) && got == (DWORD)n;
}
static int ipc_recv(PortIpc h, void *buf, int n) {
    DWORD got = 0;
    if (!ReadFile(h, buf, (DWORD)n, &got, NULL)) {
        return 0;
    }
    return got == (DWORD)n;
}
static void ipc_close(PortIpc h) { CloseHandle(h); }
#else
typedef int PortIpc;
#define kBadPipe (-1)
static PortIpc ipc_open(int n) {
    /* where the client puts it: XDG_RUNTIME_DIR first (a normal run), then TMPDIR and /tmp (Flatpak, Snap,
       older clients). */
    const char *dirs[3];
    char buf[256];
    int fd, i;

    dirs[0] = getenv("XDG_RUNTIME_DIR");
    dirs[1] = getenv("TMPDIR");
    dirs[2] = "/tmp";
    for (i = 0; i < 3; i++) {
        if (dirs[i] == NULL || dirs[i][0] == '\0') {
            continue;
        }
        snprintf(buf, sizeof(buf), "%s/discord-ipc-%d", dirs[i], n);
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            return kBadPipe;
        }
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", buf);
        if (connect(fd, (struct sockaddr *)&sa, (socklen_t)sizeof(sa)) == 0) {
            return fd;
        }
        close(fd);
    }
    return kBadPipe;
}
static int ipc_send(PortIpc fd, const void *buf, int n) {
    return send(fd, buf, (size_t)n, 0) == n;
}
static int ipc_recv(PortIpc fd, void *buf, int n) {
    return recv(fd, buf, (size_t)n, 0) == n;
}
static void ipc_close(PortIpc fd) { close(fd); }
#endif

/* the frames go out little-endian whatever the machine is, as the protocol says */
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#define OP_HANDSHAKE 0
#define OP_FRAME 1
#define OP_CLOSE 2
#define OP_PING 3
#define OP_PONG 4

static int send_frame(PortIpc ipc, int op, const char *json) {
    uint8_t head[8];
    size_t n = strlen(json);
    put32(head, (uint32_t)op);
    put32(head + 4, (uint32_t)n);
    return ipc_send(ipc, head, 8) && ipc_send(ipc, json, (int)n);
}

/* The activity. `details` is the line under the name; `state` is left out rather than filled with something
   that would be wrong half the time. instance keeps it as its own entry instead of merging it into Discord's. */
static void send_activity(PortIpc ipc, int what) {
    char json[512];
    int n = snprintf(json, sizeof(json),
        "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%u,\"activity\":{\"details\":\"%s\","
        "\"type\":0,\"instance\":true}},\"nonce\":\"bt3\"}",
#ifdef _WIN32
        (unsigned)GetCurrentProcessId(),
#else
        (unsigned)getpid(),
#endif
        kDetails[what < 0 || what > 2 ? 0 : what]);
    (void)n;
    if (!send_frame(ipc, OP_FRAME, json)) {
        note("the presence could not be sent; the link is broken");
    }
}

static PortIpc connect_discord(void) {
    char hello[128];
    uint8_t head[8];
    PortIpc ipc;
    int n;

    for (n = 0; n < 10; n++) {          /* the client can be on any of discord-ipc-0 .. 9 */
        ipc = ipc_open(n);
        if (ipc != kBadPipe) {
            snprintf(hello, sizeof(hello), "{\"v\":1,\"client_id\":\"%s\"}", sClientId);
            if (!send_frame(ipc, OP_HANDSHAKE, hello)) {
                ipc_close(ipc);
                continue;
            }
            if (!ipc_recv(ipc, head, 8)) {
                ipc_close(ipc);
                continue;
            }
            note("connected to Discord");
            return ipc;
        }
    }
    return kBadPipe;
}

static uint32_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000u + t.tv_nsec / 1000000u);
}

static void *discord_thread(void *arg) {
    PortIpc ipc = kBadPipe;
    uint32_t last = 0;
    int shown = -1;

    (void)arg;
    while (!sQuit) {
        if (ipc == kBadPipe && (!sEnabled || sClientId[0] == '\0')) {
            uint32_t until = now_ms() + 1000; /* off: look again in a second, the settings may turn it on */
            while (!sQuit && now_ms() < until) {
                usleep(100000);
            }
            continue;
        }
        if (ipc == kBadPipe) {
            ipc = connect_discord();
            if (ipc == kBadPipe) { /* not running: look again in a few seconds and carry on with the game */
                uint32_t until = now_ms() + 5000;
                while (!sQuit && now_ms() < until) {
                    usleep(100000);
                }
                continue;
            }
            shown = -1;
            last = 0;
        }
        {
            uint8_t head[8];
            int want = sWant;

            /* a ping has to be answered or the client drops the link */
            if (ipc_recv(ipc, head, 8)) {
                int op = (int)get32(head);
                uint32_t len = get32(head + 4);
                if (op == OP_PING) {
                    uint8_t out[8];
                    put32(out, OP_PONG);
                    put32(out + 4, len);
                    if (!ipc_send(ipc, out, 8)) {
                        ipc_close(ipc);
                        ipc = kBadPipe;
                        continue;
                    }
                } else if (op == OP_CLOSE) {
                    ipc_close(ipc);
                    ipc = kBadPipe;
                    continue;
                }
                while (len > 0) { /* whatever else it sent, the socket has to be drained */
                    char drop[4096];
                    int take = len > sizeof(drop) ? (int)sizeof(drop) : (int)len;
                    if (!ipc_recv(ipc, drop, take)) {
                        break;
                    }
                    len -= (uint32_t)take;
                }
            }
            if (want >= 0 && want != shown && now_ms() - last >= DISCORD_SEND_MS) {
                send_activity(ipc, want);
                shown = want;
                last = now_ms();
            }
        }
        usleep(250000);
    }
    if (ipc != kBadPipe) {
        ipc_close(ipc);
    }
    return NULL;
}

/* The switch and the id, as the settings window has them (F1 > Controls). The thread is always there and only
   connects when both are in place, so turning it on during a game works without restarting it. */
int Port_DiscordEnabled(void) {
    return sEnabled;
}

void Port_DiscordEnabledSet(int on) {
    sEnabled = on != 0;
    Port_SettingSave("discord", sEnabled);
    Port_SettingsWrite();
}

const char *Port_DiscordClientId(void) {
    return sClientId;
}

/* An application id is digits only; whatever else is in the field is dropped rather than written out. */
void Port_DiscordClientIdSet(const char *id) {
    char clean[32];
    size_t n = 0;

    for (; id != NULL && id[n] != '\0' && n < sizeof(clean) - 1; n++) {
        if (id[n] >= '0' && id[n] <= '9') {
            clean[n] = id[n];
        }
    }
    clean[n] = '\0';
    if (strcmp(clean, sClientId) != 0) {
        snprintf(sClientId, sizeof(sClientId), "%s", clean);
        Port_SettingSaveStr("discord_id", sClientId);
        Port_SettingsWrite();
        note(sClientId[0] == '\0' ? "the application id was cleared" : "the application id changed");
    }
}

void Port_DiscordStart(void) {
    const char *env = getenv("BT3_DISCORD_ID");
    pthread_t th;
    pthread_attr_t attr;
    static int once;

    if (once) {
        return;
    }
    once = 1;
    sLog = getenv("BT3_DISCORD_LOG") != NULL;
    sEnabled = Port_Setting("discord", 0);
    snprintf(sClientId, sizeof(sClientId), "%s", Port_SettingStr("discord_id", ""));
    if (env != NULL && env[0] != '\0') { /* the environment wins over the file, for one run */
        sEnabled = 1;
        snprintf(sClientId, sizeof(sClientId), "%s", env);
    }
    sQuit = 0;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, discord_thread, NULL) == 0) {
        sRunning = 1;
        note("looking for Discord");
    }
    pthread_attr_destroy(&attr);
}

/* Called by the game every frame (headless.c). It only reads what the game is doing and leaves it for the
   thread above; nothing here can make the game wait. */
void Port_DiscordTick(void) {
    void *prog = gProgress;
    void *work;
    int what;

    if (!sRunning || prog == NULL) {
        return;
    }
    work = Battle_GetWork();
    if (work != NULL && ((PortBattleWork *)work)->running != 0) {
        what = D_FIGHT;
    } else if (((PortProgress *)prog)->mode >= 6 && ((PortProgress *)prog)->mode <= 10) {
        what = D_STORY;                                  /* 6..10 is the story, menu_a_b.c's Hist_Main */
    } else {
        what = D_MENU;
    }
    sWant = what;
}