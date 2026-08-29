#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include <3ds.h>
#include <curl/curl.h>

#include "net.h"

/* real-hardware 3DS: the system SSL module's TLS handshake gets 403'd by
   Cloudflare (JA3 fingerprinting). libcurl+mbedTLS does TLS in software
   with a different fingerprint, which passes. */
#define HTTP_USER_AGENT \
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:109.0) Gecko/20100101 Firefox/115.0"

/* hard cap per buffered transfer (save streams via dl_consume, so it never
   comes close); full-size file_url images are written to SD incrementally */
#define DL_MAX_BUF (4 * 1024 * 1024)

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

    if (d->size + total > d->cap) {
        u32 cap = d->cap ? d->cap : 32 * 1024;
        while (d->size + total > cap)
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
    curl_easy_setopt(e, CURLOPT_ACCEPT_ENCODING, "");
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

dl_t *dl_start(const char *url)
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

static char s_neterr[CURL_ERROR_SIZE] = "";

const char *net_err(void)
{
    return s_neterr;
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

/* ------------------------------------------------------------------ */
/* one-shot blocking download (search api)                             */
/* ------------------------------------------------------------------ */

void url_encode(const char *in, char *out, int outsz)
{
    static const char hex[] = "0123456789ABCDEF";
    int o = 0;
    for (; *in && o < outsz - 4; in++) {
        unsigned char c = *in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~')
            out[o++] = c;
        else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

Result download(const char *url, u8 **out_buf, u32 *out_size, u32 *status_out)
{
    char errbuf[CURL_ERROR_SIZE];
    errbuf[0] = 0;

    curl_ensure();

    Result ret = -3;
    dl_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.cap = 32 * 1024;
    tmp.buf = (u8 *)malloc(tmp.cap);

    CURL *e = curl_easy_init();
    if (!tmp.buf || !e) {
        free(tmp.buf);
        if (e) curl_easy_cleanup(e);
        return -1;
    }

    curl_easy_setopt(e, CURLOPT_URL, url);
    set_common_opts(e, &tmp);
    curl_easy_setopt(e, CURLOPT_ERRORBUFFER, errbuf);

    CURLcode rc = curl_easy_perform(e);

    long code = 0;
    curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &code);
    if (status_out)
        *status_out = (u32)code;

    snprintf(s_neterr, sizeof(s_neterr), "%s",
             errbuf[0] ? errbuf : curl_easy_strerror(rc));
    if (rc == CURLE_OK && code == 200 && tmp.size > 0) {
        u8 *shrunk = (u8 *)realloc(tmp.buf, tmp.size + 1);
        if (shrunk) {
            tmp.buf = shrunk;
            tmp.buf[tmp.size] = 0;
            *out_buf = tmp.buf;
            *out_size = tmp.size;
            ret = 0;
        } else {
            free(tmp.buf);
            ret = -1;
        }
    } else {
        free(tmp.buf);
        ret = rc != CURLE_OK ? (Result)rc : -2;
    }
    curl_easy_cleanup(e);
    return ret;
}
