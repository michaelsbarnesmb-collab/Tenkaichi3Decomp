/* In-game auto-updater: downloads release zips from GitHub and updates binaries cleanly. */
#ifndef PORT_NET_UPDATE_H
#define PORT_NET_UPDATE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PORT_UPDATE_IDLE = 0,
    PORT_UPDATE_DOWNLOADING,
    PORT_UPDATE_EXTRACTING,
    PORT_UPDATE_READY,
    PORT_UPDATE_ERROR
} PortUpdateStatus;

/* Start downloading and installing update for the given release number (e.g. 136 for v0.1.36). */
void PortUpdate_Start(int targetRelease);

/* Current update status. */
PortUpdateStatus PortUpdate_GetStatus(void);

/* Progress fraction from 0.0f to 1.0f. */
float PortUpdate_GetProgress(void);

/* Downloaded bytes and total bytes (0 if unknown). */
void PortUpdate_GetBytes(size_t *downloaded, size_t *total);

/* Error description if status == PORT_UPDATE_ERROR. */
const char *PortUpdate_GetError(void);

/* Launch the updated executable and exit the current process. */
void PortUpdate_Restart(void);

/* Clean up any temporary or .old backup files from a previous update. */
void PortUpdate_Cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* PORT_NET_UPDATE_H */
