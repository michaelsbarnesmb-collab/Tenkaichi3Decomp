#ifndef PORT_CONTROL_MODE_H
#define PORT_CONTROL_MODE_H

/* The last pressure byte is unused by the PC pad; keep the layout with its sample. */
#define PORT_CONTROL_MODE_BYTE 17
#define PORT_CONTROL_MODE_WII 0x57
/* The unused battle-button bit keeps the layout with recorded battle input. */
#define PORT_BTL_WII 0x80000000u

#endif
