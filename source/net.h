#pragma once
#include <3ds.h>

/* API providers, indexed by g_provider. The host is what relative
   preview/sample/file paths (the "/xxx/yyy" form both APIs emit) have to
   be resolved against - hardcoding one of them breaks the other site. */
typedef struct
{
    const char *name; /* shown in the UI */
    const char *host; /* base host for relative paths */
} Provider;

#define PV_COUNT 2
extern const Provider g_providers[PV_COUNT];

/* active provider's host / display name; always return valid strings */
const char *net_host(void);
const char *provider_name(void);

enum { DL_MORE, DL_DONE, DL_ERR };

typedef struct dl_s dl_t;

/* incremental downloader: pump it a little every frame.
   gzip only pays off for JSON - never enable it for image payloads. */
dl_t *dl_start(const char *url, bool gzip);
int dl_pump(dl_t *d);
const u8 *dl_buf(const dl_t *d);
u32 dl_size(const dl_t *d);
void dl_consume(dl_t *d, u32 n); /* drop n leading bytes (streaming to file) */
void dl_abort(dl_t *d);
long dl_code(const dl_t *d); /* final HTTP status, 0 if none */
const char *dl_err(const dl_t *d); /* curl's last error for this handle */

void url_encode(const char *in, char *out, int outsz);

/* tear down the shared multi handle and curl globals. Every dl_t must
   already have been aborted. */
void net_exit(void);
