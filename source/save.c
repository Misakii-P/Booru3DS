#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <utime.h>
#include <dirent.h>
#include <sys/stat.h>
#include <turbojpeg.h>

#include <3ds.h>

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "net.h"
#include "posts.h"
#include "save.h"

#define SAVE_DIR_A "sdmc:/3ds"
#define SAVE_DIR_B "sdmc:/3ds/booru"

/* camera saves: decode + re-encode as baseline jpeg the 3DS camera accepts.
   the camera ONLY indexes images up to 640x480 - anything larger is
   silently ignored during its SD scan. */
#define CAM_MAX_W 640
#define CAM_MAX_H 480

static struct
{
    SaveState st;
    SaveDest dest;
    dl_t *dl;
    FILE *fp;
    u32 bytes;  /* booru: total written | camera: buffered download size */
    char path[256];
} s = { SAVE_IDLE, SAVE_DEST_BOORU, NULL, NULL, 0, "" };

/* latched on the first successful camera install of this session */
static bool s_cam_notice = false;

bool save_take_cam_notice(void)
{
    bool v = s_cam_notice;
    s_cam_notice = false;
    return v;
}

void save_init(void)
{
    mkdir(SAVE_DIR_A, 0777);
    mkdir(SAVE_DIR_B, 0777);
}

static void finish(bool keep)
{
    if (s.fp) {
        fclose(s.fp);
        s.fp = NULL;
    }
    if (s.dl) {
        dl_abort(s.dl);
        s.dl = NULL;
    }
    if (!keep && s.path[0])
        remove(s.path);
}

void save_exit(void)
{
    finish(true);
    s.st = SAVE_IDLE;
}

void save_reset(void)
{
    if (s.st == SAVE_ACTIVE)
        finish(false);
    s.st = SAVE_IDLE;
}

SaveState save_state(void)
{
    return s.st;
}

u32 save_bytes(void)
{
    return s.bytes;
}

/* ------------------------------------------------------------------ */
/* booru folder target                                                 */
/* ------------------------------------------------------------------ */

static void build_booru_path(int post, char *out, int outsz)
{
    Post *p = &posts[post];
    const char *img = p->image;
    const char *slash = strrchr(img, '/');
    if (slash)
        img = slash + 1;

    char name[160];
    int o = 0;
    for (; *img && o < (int)sizeof(name) - 1; img++) {
        unsigned char c = *img;
        if (c == '/' || c == '\\' || c < 0x20 || c == '?' || c == '*' ||
            c == ':' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
        name[o++] = (char)c;
    }
    if (o == 0)
        o = snprintf(name, sizeof(name), "%u.dat", p->id);
    name[o] = 0;

    snprintf(out, outsz, SAVE_DIR_B "/%s", name);
}

/* ------------------------------------------------------------------ */
/* 3DS camera target: DCIM/<NNNNIN03>/HNI_NNNN.JPG                     */
/* (folder/file convention verified against SCR2JPG, which demonstrably */
/*  produces photos the camera indexes)                                 */
/* ------------------------------------------------------------------ */

static bool build_camera_path(char *out, int outsz)
{
    mkdir("sdmc:/DCIM", 0777);

    /* first ###NIN03 folder with a free slot, starting at 100 */
    for (int dirIdx = 100; dirIdx <= 999; dirIdx++) {
        char dir[128];
        snprintf(dir, sizeof(dir), "sdmc:/DCIM/%03dNIN03", dirIdx);
        mkdir(dir, 0777); /* succeeds silently when it already exists */

        int mx = 0, count = 0;
        DIR *d = opendir(dir);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            const char *n = e->d_name;
            if (!strncmp(n, "HNI_", 4)) {
                count++;
                int v = atoi(n + 4);
                if (v > mx && v < 10000)
                    mx = v;
            }
        }
        if (d)
            closedir(d);

        if (count >= 100)
            continue; /* camera folders hold 100 photos each */

        snprintf(out, outsz, "%s/HNI_%04d.JPG", dir, (mx % 9999) + 1);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Nintendo-style EXIF (Make/Model/Software/DateTime + Exif            */
/* DateTimeOriginal), built big-endian and spliced after SOI           */
/* ------------------------------------------------------------------ */

/* EXIF layout v2: mirrors the SCR2JPG template (proven to be read by the
   3DS camera) - IFD0 gains an Artist tag, SubIFD gains UserComment.
   Offsets (tiff-relative): make=86 model=96 soft=110 dt=120 artist=140
   subifd=156 dto=198 usercomment=218, payload total 233. */
static size_t build_exif(unsigned char *b, const char *dt)
{
    size_t p = 0;
#define P8(x) b[p++] = (unsigned char)(x)
#define P16(x) do { b[p++] = (unsigned char)((x) >> 8); \
                    b[p++] = (unsigned char)(x); } while (0)
#define P32(x) do { b[p++] = (unsigned char)((x) >> 24); \
                    b[p++] = (unsigned char)((x) >> 16); \
                    b[p++] = (unsigned char)((x) >> 8); \
                    b[p++] = (unsigned char)(x); } while (0)

    memcpy(b + p, "Exif\0\0", 6); p += 6;

    /* TIFF header, big-endian, IFD0 at offset 8 */
    P8('M'); P8('M'); P16(42); P32(8);

    /* IFD0: 6 entries */
    P16(6);
    P16(0x010F); P16(2); P32(9);  P32(86);   /* Make     "Nintendo"       */
    P16(0x0110); P16(2); P32(13); P32(96);   /* Model    "Nintendo 3DS"   */
    P16(0x0131); P16(2); P32(9);  P32(110);  /* Software "Booru3DS"       */
    P16(0x0132); P16(2); P32(20); P32(120);  /* DateTime                  */
    P16(0x013B); P16(2); P32(16); P32(140);  /* Artist  "Booru3DS client" */
    P16(0x8769); P16(4); P32(1);  P32(156);  /* Exif SubIFD               */
    P32(0);

    /* data area, offsets relative to TIFF start */
    memcpy(b + p, "Nintendo\0", 9);      p += 9;   /*  86..94 */
    P8(0);                                          /*  95     */
    memcpy(b + p, "Nintendo 3DS\0", 13); p += 13;   /*  96..108*/
    P8(0);                                          /* 109     */
    memcpy(b + p, "Booru3DS\0", 9);      p += 9;   /* 110..118*/
    P8(0);                                          /* 119     */

    memcpy(b + p, dt, 20);               p += 20;  /* 120..139*/

    memcpy(b + p, "Booru3DS client", 15); p += 15;  /* 140..154*/
    P8(0);                                          /* 155     */

    /* 156: SubIFD, 3 entries */
    P16(3);
    P16(0x9000); P16(7); P32(4);
    P8('0'); P8('2'); P8('3'); P8('0');             /* ExifVersion inline */
    P16(0x9003); P16(2); P32(20); P32(198);         /* DateTimeOriginal   */
    P16(0x9286); P16(7); P32(9); P32(218);          /* UserComment        */
    P32(0);

    memcpy(b + p, dt, 20);               p += 20;  /* 198..217 */

    /* 218: UserComment = charset header + value */
    memcpy(b + p, "ASCII\0\0\0", 8);     p += 8;   /* 218..225 */
    P8('1');                                         /* 226     */

#undef P8
#undef P16
#undef P32
    return p; /* 233 */
}

static bool write_camera_jpeg(FILE *f, const unsigned char *jpg,
                              unsigned long sz)
{
    char dt[64] = "2026:01:01 00:00:00";
    time_t t = time(NULL);
    struct tm *tmv = localtime(&t);
    if (tmv && tmv->tm_year >= 110)
        snprintf(dt, sizeof(dt), "%04d:%02d:%02d %02d:%02d:%02d",
                 tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday,
                 tmv->tm_hour, tmv->tm_min, tmv->tm_sec);

    unsigned char exif[280];
    size_t exiflen = build_exif(exif, dt);

    /* JPEG: SOI(2) [APP0 JFIF] ...  Insert EXIF APP1 after APP0 if present,
       otherwise right after SOI. 3DS camera is picky about order. */
    size_t ins = 2;
    if (sz >= 6 && jpg[2] == 0xFF && jpg[3] == 0xE0) {
        unsigned int app0len = ((unsigned int)jpg[4] << 8) | jpg[5];
        if (app0len >= 2 && 2 + 2 + app0len <= sz)
            ins = 2 + 2 + app0len;
    }
    if (fwrite(jpg, 1, ins, f) != ins)
        return false;
    unsigned char hdr[3] = {
        0xE1,
        (unsigned char)(((exiflen + 2) >> 8) & 0xFF),
        (unsigned char)((exiflen + 2) & 0xFF),
    };
    if (fwrite(hdr, 1, 3, f) != 3) return false;
    if (fwrite(exif, 1, exiflen, f) != exiflen) return false;
    return fwrite(jpg + ins, 1, sz - ins, f) == sz - ins;
}

/* decode ANY input and produce a small baseline jpeg the camera accepts.
   no passthrough: progressive/huge jpegs are exactly what the camera
   silently refuses to index. */
static bool camera_encode(u8 *jpg, u32 sz, const char *path)
{
    static tjhandle s_dec = NULL, s_enc = NULL;
    if (!s_dec)
        s_dec = tjInitDecompress();
    if (!s_enc)
        s_enc = tjInitCompress();
    if (!s_dec || !s_enc)
        return false;

    static u8 s_rgba[CAM_MAX_W * CAM_MAX_H * 4];
    const u8 *enc_src = s_rgba;
    int W = 0, H = 0, dw = 0, dh = 0;

    if (sz >= 3 && jpg[0] == 0xFF && jpg[1] == 0xD8 && jpg[2] == 0xFF) {
        /* jpeg input: scale-decode (also converts progressive -> baseline) */
        if (tjDecompressHeader(s_dec, jpg, sz, &W, &H) || W <= 0 || H <= 0)
            return false;

        int num = 1, den = 1, nsf = 0;
        tjscalingfactor *sf = tjGetScalingFactors(&nsf);
        for (int i = 0; sf && i < nsf; i++) {
            if (sf[i].num > sf[i].denom)
                continue;
            int tw = (W * sf[i].num + sf[i].denom - 1) / sf[i].denom;
            int th = (H * sf[i].num + sf[i].denom - 1) / sf[i].denom;
            if (tw <= CAM_MAX_W && th <= CAM_MAX_H) {
                num = sf[i].num;
                den = sf[i].denom;
                break;
            }
        }
        dw = (W * num + den - 1) / den;
        dh = (H * num + den - 1) / den;
        if (tjDecompress2(s_dec, jpg, sz, s_rgba, dw, 0, dh, TJPF_RGBA,
                          TJFLAG_FASTDCT))
            return false;
    } else {
        /* png/bmp/etc via stb_image, then shrink to fit 640x480 */
        int comp = 0;

        /* header pre-check: refuse monsters BEFORE stb tries to allocate
           W*H*4 bytes (a 4500px wallpaper would attempt ~56MB) */
        {
            int pw = 0, ph = 0, pc = 0;
            if (!stbi_info_from_memory(jpg, (int)sz, &pw, &ph, &pc) ||
                pw <= 0 || ph <= 0 ||
                (long long)pw * ph > 10000000LL)
                return false;
        }

        u8 *src = stbi_load_from_memory(jpg, (int)sz, &W, &H, &comp, 4);
        if (!src || W <= 0 || H <= 0) {
            free(src);
            return false;
        }

        dw = W; dh = H;
        if (dw > CAM_MAX_W || dh > CAM_MAX_H) {
            int sw = (CAM_MAX_W * 256) / dw;
            int sh = (CAM_MAX_H * 256) / dh;
            int scale = sw < sh ? sw : sh;
            dw = (W * scale) / 256;
            dh = (H * scale) / 256;
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
            for (int y = 0; y < dh; y++) {
                int sy = y * H / dh;
                for (int x = 0; x < dw; x++) {
                    int sx = x * W / dw;
                    memcpy(s_rgba + ((size_t)y * dw + x) * 4,
                           src + ((size_t)sy * W + sx) * 4, 4);
                }
            }
            enc_src = s_rgba;
        } else {
            enc_src = src;
        }
        stbi_image_free(src);
    }

    unsigned long outcap = tjBufSize(dw, dh, TJSAMP_420);
    unsigned char *out = tjAlloc(outcap);
    bool ok = false;
    if (out) {
        unsigned long outsize = outcap;
        if (tjCompress2(s_enc, enc_src, dw, 0, dh, TJPF_RGBA,
                        &out, &outsize, TJSAMP_420, 85,
                        TJFLAG_FASTDCT) == 0) {
            FILE *f = fopen(path, "wb");
            if (f) {
                ok = write_camera_jpeg(f, out, outsize);
                fclose(f);
            }
            /* the camera may fall back to the file's modification date:
               stamp it explicitly so imported photos show download time.
               time() can return -1 on 3DS if RTC is unset, which would
               clamp to the FAT minimum (1980/2001) - use a sane fallback. */
            if (ok) {
                time_t now = time(NULL);
                if (now == (time_t)-1 || now < 1262304000)
                    now = 1704067200; /* 2024-01-01 */
                struct utimbuf ub;
                ub.actime = ub.modtime = now;
                utime(path, &ub);
            }
        }
        tjFree(out);
    }

    return ok;
}

/* ------------------------------------------------------------------ */

static void build_url(int post, char *out, int outsz)
{
    Post *p = &posts[post];
    if (p->file[0]) {
        const char *cand = p->file;
        if (!strncmp(cand, "//", 2))
            snprintf(out, outsz, "https:%s", cand);
        else if (cand[0] == '/')
            snprintf(out, outsz, "https://safebooru.org%s", cand);
        else
            snprintf(out, outsz, "%s", cand);
        return;
    }
    if (p->image[0]) {
        snprintf(out, outsz,
                 "https://safebooru.org/images/%s/%s",
                 p->directory, p->image);
        return;
    }
    out[0] = 0;
}

void save_request(int post, SaveDest dest)
{
    if (s.st == SAVE_ACTIVE || post < 0 || post >= post_count)
        return;

    char url[512];
    build_url(post, url, sizeof(url));
    if (!url[0]) {
        s.st = SAVE_ERR;
        return;
    }

    if (dest == SAVE_DEST_BOORU) {
        build_booru_path(post, s.path, sizeof(s.path));
        s.fp = fopen(s.path, "wb");
        if (!s.fp) {
            s.st = SAVE_ERR;
            return;
        }
    } else {
        build_camera_path(s.path, sizeof(s.path));
        s.fp = NULL;
    }

    s.dest = dest;
    s.bytes = 0;
    s.dl = dl_start(url);
    if (!s.dl) {
        finish(false);
        s.st = SAVE_ERR;
        return;
    }
    s.st = SAVE_ACTIVE;
}

void save_pump(void)
{
    if (s.st != SAVE_ACTIVE || !s.dl)
        return;

    int r = dl_pump(s.dl);
    u32 total = dl_size(s.dl);

    if (s.dest == SAVE_DEST_BOORU) {
        const u8 *buf = dl_buf(s.dl);
        if (total > 0 && s.fp) {
            fwrite(buf, 1, total, s.fp);
            s.bytes += total;
            /* stream to SD so huge files never grow the heap */
            dl_consume(s.dl, total);
        }

        if (r == DL_DONE) {
            finish(true);
            s.st = SAVE_OK;
        } else if (r == DL_ERR) {
            finish(false); /* remove partial file */
            s.st = SAVE_ERR;
        }
        return;
    }

    /* camera: buffer whole file, then decode+re-encode once complete */
    s.bytes = total;
    if (r == DL_DONE) {
        u8 *buf = (u8 *)dl_buf(s.dl);
        u32 sz = dl_size(s.dl);
        bool ok = sz >= 16 && camera_encode(buf, sz, s.path);
        dl_abort(s.dl);
        s.dl = NULL;
        if (!ok)
            remove(s.path); /* don't leave a broken photo in the roll */
        s.st = ok ? SAVE_OK : SAVE_ERR;
        if (ok)
            s_cam_notice = true;
    } else if (r == DL_ERR) {
        finish(false);
        s.st = SAVE_ERR;
    }
}
