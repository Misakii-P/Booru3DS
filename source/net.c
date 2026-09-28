#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include <3ds.h>
#include <curl/curl.h>

#include "app.h"
#include "net.h"

/* real-hardware 3DS: the system SSL module's TLS handshake gets 403'd by
   Cloudflare (JA3 fingerprinting). libcurl+mbedTLS does TLS in software
   with a different fingerprint, which passes. */
#define HTTP_USER_AGENT \
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:109.0) Gecko/20100101 Firefox/115.0"

/* hard cap per buffered transfer (save streams via dl_consume, so it never
   comes close); full-size file_url images are written to SD incrementally */
#define DL_MAX_BUF (4 * 1024 * 1024)

const Provider g_providers[PV_COUNT] = {
    { "safebooru.org", "safebooru.org" },
    { "konachan.net",  "konachan.net"  },
};

static int provider_index(void)
{
    int i = g_provider;
    if (i < 0 || i >= PV_COUNT)
        i = 0;
    return i;
}

const char *net_host(void)
{
    return g_providers[provider_index()].host;
}

const char *provider_name(void)
{
    return g_providers[provider_index()].name;
}

static CURLM *s_multi = NULL;
static bool s_global = false;

static void curl_ensure(void)
{
    if (!s_global) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        s_global = true;
    }
}

struct dl_s
{
    CURL *easy;
    long code;
    u8 *buf;
    u32 size, cap;
    bool done;
    bool failed;
    char err[CURL_ERROR_SIZE];
};

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    dl_t *d = (dl_t *)userdata;
    size_t total = size * nmemb;

    if ((size_t)d->size + total > d->cap) {
        u32 cap = d->cap ? d->cap : 32 * 1024;
        while ((size_t)d->size + total > cap)
            cap *= 2;
        if (cap > DL_MAX_BUF)
            return 0; /* aborts the transfer */
        u8 *nb = (u8 *)realloc(d->buf, cap);
        if (!nb)
            return 0;
        d->buf = nb;
        d->cap = cap;
    }
    memcpy(d->buf + d->size, ptr, total);
    d->size += (u32)total;
    return total;
}

static void set_common_opts(CURL *e, dl_t *d)
{
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, d);
    curl_easy_setopt(e, CURLOPT_ERRORBUFFER, d ? d->err : NULL);
    curl_easy_setopt(e, CURLOPT_USERAGENT, HTTP_USER_AGENT);
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(e, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(e, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(e, CURLOPT_PRIVATE, d);
}

/* ------------------------------------------------------------------ */
/* incremental downloader                                              */
/* ------------------------------------------------------------------ */

dl_t *dl_start(const char *url, bool gzip)
{
    curl_ensure();

    if (!s_multi)
        s_multi = curl_multi_init();
    if (!s_multi)
        return NULL;

    dl_t *d = (dl_t *)calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->cap = 32 * 1024;
    d->buf = (u8 *)malloc(d->cap);
    d->easy = curl_easy_init();
    if (!d->buf || !d->easy) {
        free(d->buf);
        if (d->easy) curl_easy_cleanup(d->easy);
        free(d);
        return NULL;
    }

    curl_easy_setopt(d->easy, CURLOPT_URL, url);
    set_common_opts(d->easy, d);
    if (gzip)
        curl_easy_setopt(d->easy, CURLOPT_ACCEPT_ENCODING, "");

    if (curl_multi_add_handle(s_multi, d->easy) != CURLM_OK) {
        curl_easy_cleanup(d->easy);
        free(d->buf);
        free(d);
        return NULL;
    }
    return d;
}

int dl_pump(dl_t *d)
{
    if (!d || !d->easy)
        return DL_ERR;
    if (d->done || d->failed)
        return d->failed ? DL_ERR : DL_DONE;

    int running = 0;
    /* pump multiple times per frame to keep curl progressing on slow CPUs */
    for (int i = 0; i < 3; i++) {
        curl_multi_perform(s_multi, &running);
        if (running == 0)
            break;
    }

    /* the multi handle is shared, so this also harvests completions for
       the other in-flight transfers; CURLOPT_PRIVATE routes each message
       back to its own dl_t */
    CURLMsg *msg;
    int msgs_left = 0;
    while ((msg = curl_multi_info_read(s_multi, &msgs_left)) != NULL) {
        if (msg->msg == CURLMSG_DONE) {
            dl_t *fd = NULL;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &fd);
            if (fd) {
                fd->done = true;
                fd->failed = msg->data.result != CURLE_OK;
                curl_easy_getinfo(msg->easy_handle,
                                  CURLINFO_RESPONSE_CODE, &fd->code);
            }
            curl_multi_remove_handle(s_multi, msg->easy_handle);
        }
    }

    if (d->done) {
        if (!d->failed && d->code == 200)
            return DL_DONE;
        return DL_ERR;
    }
    return DL_MORE;
}

const u8 *dl_buf(const dl_t *d)
{
    return d ? d->buf : NULL;
}

u32 dl_size(const dl_t *d)
{
    return d ? d->size : 0;
}

long dl_code(const dl_t *d)
{
    return d ? d->code : 0;
}

const char *dl_err(const dl_t *d)
{
    if (!d || !d->err[0])
        return "unknown error";
    return d->err;
}

void dl_consume(dl_t *d, u32 n)
{
    if (!d || n == 0)
        return;
    if (n >= d->size) {
        d->size = 0;
        /* shrink buffer to free heap after streaming is done */
        if (d->cap > 64 * 1024) {
            u8 *shrunk = (u8 *)realloc(d->buf, 32 * 1024);
            if (shrunk) {
                d->buf = shrunk;
                d->cap = 32 * 1024;
            }
        }
        return;
    }
    memmove(d->buf, d->buf + n, d->size - n);
    d->size -= n;
    /* shrink if buffer is mostly empty */
    if (d->size > 0 && d->cap > 64 * 1024 && d->size < d->cap / 4) {
        u32 new_cap = d->cap / 2;
        if (new_cap < d->size * 2)
            new_cap = d->size * 2;
        if (new_cap < 32 * 1024)
            new_cap = 32 * 1024;
        u8 *shrunk = (u8 *)realloc(d->buf, new_cap);
        if (shrunk) {
            d->buf = shrunk;
            d->cap = new_cap;
        }
    }
}

void dl_abort(dl_t *d)
{
    if (!d)
        return;
    if (d->easy) {
        /* clear private so a pending DONE message for this handle
           won't dereference freed memory on the next pump */
        curl_easy_setopt(d->easy, CURLOPT_PRIVATE, NULL);
        if (s_multi)
            curl_multi_remove_handle(s_multi, d->easy);
        curl_easy_cleanup(d->easy);
    }
    free(d->buf);
    free(d);
}

void net_exit(void)
{
    if (s_multi) {
        curl_multi_cleanup(s_multi);
        s_multi = NULL;
    }
    if (s_global) {
        curl_global_cleanup();
        s_global = false;
    }
}

/* ------------------------------------------------------------------ */

void url_encode(const char *in, char *out, int outsz)
{
    static const char hex[] = "0123456789ABCDEF";
    int o = 0;
    for (; *in; in++) {
        /* worst case a character costs 3 bytes, plus the terminator */
        if (o + 4 > outsz)
            break;
        unsigned char c = (unsigned char)*in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~')
            out[o++] = (char)c;
        else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    if (outsz > 0)
        out[o] = 0;
}
