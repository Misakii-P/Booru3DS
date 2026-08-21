#pragma once
#include <3ds.h>

enum { DL_MORE, DL_DONE, DL_ERR };

typedef struct dl_s dl_t;

/* incremental downloader: pump it a little every frame */
dl_t *dl_start(const char *url);
int dl_pump(dl_t *d);
const u8 *dl_buf(const dl_t *d);
u32 dl_size(const dl_t *d);
void dl_consume(dl_t *d, u32 n); /* drop n leading bytes (streaming to file) */
void dl_abort(dl_t *d);

/* one-shot blocking download (search api) */
Result download(const char *url, u8 **out_buf, u32 *out_size, u32 *status_out);
void url_encode(const char *in, char *out, int outsz);
const char *net_err(void); /* last blocking-download error text */
