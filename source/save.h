#pragma once
#include <3ds.h>

typedef enum { SAVE_IDLE, SAVE_ACTIVE, SAVE_OK, SAVE_ERR } SaveState;
typedef enum { SAVE_DEST_BOORU, SAVE_DEST_CAMERA } SaveDest;

void save_init(void);
void save_exit(void);
void save_reset(void);
void save_request(int post, SaveDest dest);
void save_pump(void);
SaveState save_state(void);
u32 save_bytes(void);
bool save_take_cam_notice(void); /* consumed on first camera success */
