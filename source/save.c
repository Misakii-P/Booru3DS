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
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "net.h"
#include "posts.h"
#include "save.h"
#include "scaledec.h"

#define SAVE_DIR_A "sdmc:/3ds"
#define SAVE_DIR_B "sdmc:/3ds/booru"

/* camera saves: decode + re-encode as baseline jpeg the 3DS camera accepts.
   the camera ONLY indexes images up to 640x480 - anything larger is
   silently ignored during its SD scan. */
#define CAM_MAX_W 640
#define CAM_MAX_H 480

static tjhandle s_dec, s_enc;

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

/* Why the last save failed. The UI showed a bare "save failed" for every
   distinct problem, which made them indistinguishable from the outside -
   a 4MB transfer abort and a full camera roll looked identical. */
/* "saved!" / "save failed" are a 3s toast, not a sticky state - a message
   that never goes away is just permanent noise once you have read it. */
#define SAVE_MSG_MS 3000
static u32 s_done_ms = 0;

static u32 now_ms(void) { return (u32)(svcGetSystemTick() / 268123); }
static void mark_done(void) { s_done_ms = now_ms(); }

static char s_err[96] = "";
static void set_err(const char *why) { snprintf(s_err, sizeof(s_err), "%s", why); }

const char *save_error(void) { return s_err; }

bool save_status_live(void)
{
    if (s.st != SAVE_OK && s.st != SAVE_ERR)
        return false;
    return (u32)(now_ms() - s_done_ms) < SAVE_MSG_MS;
}

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
    if (!s_dec) s_dec = tjInitDecompress();
    if (!s_enc) s_enc = tjInitCompress();
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
    if (s_dec) {
        tjDestroy(s_dec);
        s_dec = NULL;
    }
    if (s_enc) {
        tjDestroy(s_enc);
        s_enc = NULL;
    }
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
    char dt[64];
    time_t t = time(NULL);
    struct tm *tmv = localtime(&t);
    if (tmv && tmv->tm_year >= 110)
        snprintf(dt, sizeof(dt), "%04d:%02d:%02d %02d:%02d:%02d",
                 tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday,
                 tmv->tm_hour, tmv->tm_min, tmv->tm_sec);
    else
        snprintf(dt, sizeof(dt), "2024:01:01 00:00:00");

    unsigned char exif[280];
    size_t exiflen = build_exif(exif, dt);

    /* JPEG: SOI(2) [APP0 JFIF] [APP1 old EXIF] ...  Insert new EXIF
       after APP0, replacing any existing APP1 so ours is the first one
       the camera sees. 3DS camera is picky about order and duplicates. */
    size_t ins = 2;
    size_t copyFrom = 2;
    if (sz >= 6 && jpg[2] == 0xFF && jpg[3] == 0xE0) {
        unsigned int app0len = ((unsigned int)jpg[4] << 8) | jpg[5];
        if (app0len >= 2 && 2 + 2 + app0len <= sz) {
            ins = 2 + 2 + app0len;
            copyFrom = ins;
            if (sz >= ins + 4 && jpg[ins] == 0xFF && jpg[ins+1] == 0xE1) {
                unsigned int oldExifLen = ((unsigned int)jpg[ins+2] << 8) | jpg[ins+3];
                if (oldExifLen >= 2 && ins + 2 + oldExifLen <= sz)
                    copyFrom = ins + 2 + oldExifLen;
            }
        }
    } else if (sz >= 4 && jpg[2] == 0xFF && jpg[3] == 0xE1) {
        unsigned int oldExifLen = ((unsigned int)jpg[4] << 8) | jpg[5];
        if (oldExifLen >= 2 && 2 + 2 + oldExifLen <= sz)
            copyFrom = 2 + 2 + oldExifLen;
    }
    if (fwrite(jpg, 1, ins, f) != ins)
        return false;
    /* APP1 is a two-byte marker: 0xFF 0xE1, then the segment length.
       Writing only 0xE1 produced a file that still decoded as an image
       but that the camera's EXIF reader skipped entirely, so every photo
       came out dated at the camera's floor date. */
    unsigned char hdr[4] = {
        0xFF, 0xE1,
        (unsigned char)(((exiflen + 2) >> 8) & 0xFF),
        (unsigned char)((exiflen + 2) & 0xFF),
    };
    if (fwrite(hdr, 1, 4, f) != 4) return false;
    if (fwrite(exif, 1, exiflen, f) != exiflen) return false;
    return fwrite(jpg + copyFrom, 1, sz - copyFrom, f) == sz - copyFrom;
}

/* decode ANY input and produce a small baseline jpeg the camera accepts.
   no passthrough: progressive/huge jpegs are exactly what the camera
   silently refuses to index. */
/* Decode the file at path in place and overwrite it with a baseline JPEG
   the 3DS camera will index. Reading from disk rather than from the
   transfer buffer means the compressed original only has to be resident
   once, and only for the formats that genuinely need it. */
static bool camera_encode(const char *path)
{
    if (!s_dec || !s_enc) {
        set_err("jpeg encoder unavailable");
        return false;
    }

    u8 *rgba = NULL;
    u8 *raw = NULL;   /* jpeg source, only held while turbojpeg needs it */
    FILE *rf = fopen(path, "rb");
    if (!rf) {
        set_err("staged file vanished");
        return false;
    }
    fseek(rf, 0, SEEK_END);
    long fsz = ftell(rf);
    fseek(rf, 0, SEEK_SET);
    if (fsz < 16) {
        fclose(rf);
        set_err("download was too small to be an image");
        return false;
    }

    const u8 *enc_src = NULL;
    int W = 0, H = 0, dw = 0, dh = 0;
    bool ok = false;
    bool is_jpeg = false;

    /* peek the magic without loading the file */
    unsigned char magic[3] = { 0, 0, 0 };
    if (fread(magic, 1, 3, rf) != 3) {
        fclose(rf);
        set_err("could not read staged file");
        return false;
    }
    is_jpeg = (magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF);
    fseek(rf, 0, SEEK_SET);

    if (is_jpeg) {
        /* turbojpeg has no from-file entry point, so this one format does
           need the compressed data in RAM. Sized exactly, with no
           doubling slack, and released as soon as the pixels exist. */
        raw = (u8 *)malloc((size_t)fsz);
        if (!raw) {
            set_err("out of memory reading the original");
            goto done;
        }
        if (fread(raw, 1, (size_t)fsz, rf) != (size_t)fsz) {
            set_err("short read on the staged original");
            goto done;
        }

        if (tjDecompressHeader(s_dec, raw, (unsigned long)fsz, &W, &H) ||
            W <= 0 || H <= 0) {
            set_err("not a decodable jpeg");
            goto done;
        }

        if (!scale_fit(W, H, CAM_MAX_W, CAM_MAX_H, &dw, &dh)) {
            set_err("image too large for 640x480");
            goto done;
        }

        rgba = (u8 *)malloc((size_t)dw * dh * 4);
        if (!rgba) {
            set_err("out of memory decoding");
            goto done;
        }
        if (tjDecompress2(s_dec, raw, (unsigned long)fsz, rgba, dw, 0, dh,
                          TJPF_RGBA, TJFLAG_FASTDCT)) {
            set_err("jpeg too large to scale down");
            goto done;
        }
        free(raw);
        raw = NULL;
        enc_src = rgba;
    } else {
        /* png/bmp/etc: stb reads the file itself, so the compressed data
           never has to be in RAM at all */
        int comp = 0;

        /* Sanity bound only. This used to reject anything over
           CAM_MAX_W*CAM_MAX_H pixels, which threw away every PNG bigger
           than ~0.3MP and made the downscale block below it dead code -
           that is what "save failed" above roughly 1MB actually was. The
           only thing worth refusing here is an allocation that would
           itself be unreasonable (a 4500px wallpaper is ~56MB decoded). */
        {
            int pw = 0, ph = 0, pc = 0;
            fseek(rf, 0, SEEK_SET);
            if (!stbi_info_from_file(rf, &pw, &ph, &pc)) {
                set_err("unsupported format (not png/jpeg/bmp)");
                goto done;
            }
            if (pw <= 0 || ph <= 0) {
                set_err("bad image header");
                goto done;
            }
            /* stb expands to W*H*4 before we can downscale, so this is
               a hard RAM requirement, not a politeness limit. ~8MB is
               about what old3DS can spare; anything larger fails as an
               opaque "out of memory" from inside stb. */
            if ((long long)pw * ph * 4 > 8LL * 1024 * 1024) {
                snprintf(s_err, sizeof(s_err),
                         "image too large to decode (%dx%d, needs %lldMB)",
                         pw, ph, (long long)pw * ph * 4 / (1024 * 1024));
                goto done;
            }
        }

        fseek(rf, 0, SEEK_SET);
        u8 *src = stbi_load_from_file(rf, &W, &H, &comp, 4);
        if (!src) {
            const char *why = stbi_failure_reason();
            set_err(why ? why : "stb could not decode this image");
            goto done;
        }
        if (W <= 0 || H <= 0) {
            stbi_image_free(src);
            set_err("bad image dimensions");
            goto done;
        }

        dw = W; dh = H;
        if (dw > CAM_MAX_W || dh > CAM_MAX_H) {
            rgba = (u8 *)malloc((size_t)dw * dh * 4);
            if (!rgba) {
                stbi_image_free(src);
                set_err("out of memory downscaling");
                goto done;
            }
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
                    memcpy(rgba + ((size_t)y * dw + x) * 4,
                           src + ((size_t)sy * W + sx) * 4, 4);
                }
            }
            enc_src = rgba;
        } else {
            enc_src = src;
        }
        stbi_image_free(src);
    }
    fclose(rf);
    rf = NULL;

    if (enc_src) {
        unsigned long outcap = tjBufSize(dw, dh, TJSAMP_420);
        unsigned char *out = tjAlloc(outcap);
        if (out) {
            unsigned long outsize = outcap;
            if (tjCompress2(s_enc, enc_src, dw, 0, dh, TJPF_RGBA,
                            &out, &outsize, TJSAMP_420, 85,
                            TJFLAG_FASTDCT) == 0) {
                FILE *f = fopen(path, "wb");
                if (!f) {
                    set_err("cannot rewrite the staged file");
                } else {
                    ok = write_camera_jpeg(f, out, outsize);
                    if (!ok) set_err("exif splice failed");
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
    }

done:
    if (rf) fclose(rf);
    free(raw);
    free(rgba);
    return ok;
}

/* ------------------------------------------------------------------ */

static void resolve(const char *cand, char *out, int outsz)
{
    if (!strncmp(cand, "//", 2))
        snprintf(out, outsz, "https:%s", cand);
    else if (cand[0] == '/')
        snprintf(out, outsz, "https://%s%s", net_host(), cand);
    else
        snprintf(out, outsz, "%s", cand);
}

/* The booru folder gets the untouched original - that is the whole point
   of that target, and it streams to SD so size costs nothing. */
static void build_url(int post, char *out, int outsz)
{
    Post *p = &posts[post];
    if (p->file[0]) {
        resolve(p->file, out, outsz);
        return;
    }
    if (p->image[0]) {
        snprintf(out, outsz,
                 "https://%s/images/%s/%s",
                 net_host(), p->directory, p->image);
        return;
    }
    out[0] = 0;
}

/* The camera target is different: the output is capped at 640x480 because
   that is all the 3DS camera indexes, so the original's extra resolution
   is thrown away anyway - while costing us the download AND, fatally, the
   decode. stb expands to W*H*4 before any downscale, and across a 420
   post live sample the originals need a median 12MB and up to 193MB to
   decode, which old3DS cannot do. The provider sample is at most ~850px
   (safebooru) or 1500px (konachan): 6.8MB worst case, and visually
   identical once it lands in a 640x480 JPEG. */
static void build_camera_url(int post, char *out, int outsz)
{
    Post *p = &posts[post];
    if (p->sample_ok && p->sample[0]) { resolve(p->sample, out, outsz); return; }
    if (p->preview[0])                { resolve(p->preview, out, outsz); return; }
    if (p->file[0])                   { resolve(p->file, out, outsz); return; }
    out[0] = 0;
}

void save_request(int post, SaveDest dest)
{
    if (s.st == SAVE_ACTIVE || post < 0 || post >= post_count)
        return;

    char url[512];
    if (dest == SAVE_DEST_CAMERA)
        build_camera_url(post, url, sizeof(url));
    else
        build_url(post, url, sizeof(url));
    if (!url[0]) {
        set_err("no download url for this post");
        s.st = SAVE_ERR;
        return;
    }

    /* never inherit a handle or a path from an earlier save: a stale path
       would silently overwrite the previously installed photo */
    if (s.fp) {
        fclose(s.fp);
        s.fp = NULL;
    }
    s.path[0] = 0;

    if (dest == SAVE_DEST_BOORU) {
        build_booru_path(post, s.path, sizeof(s.path));
        s.fp = fopen(s.path, "wb");
        if (!s.fp) {
            set_err("cannot write to sdmc:/DCIM");
            s.st = SAVE_ERR;
            return;
        }
    } else {
        /* all 900 camera folders full: bail out rather than fall through
           with a path we never filled in */
        if (!build_camera_path(s.path, sizeof(s.path))) {
            set_err("camera roll is full (900 folders)");
            s.st = SAVE_ERR;
            return;
        }
        /* the original is streamed here first and then overwritten in
           place with the converted JPEG */
        s.fp = fopen(s.path, "wb");
        if (!s.fp) {
            set_err("cannot write to sdmc:/DCIM");
            s.st = SAVE_ERR;
            return;
        }
    }

    s.dest = dest;
    s.bytes = 0;
    s_err[0] = 0;
    s.dl = dl_start(url, false);
    if (!s.dl) {
        set_err("download could not start");
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
            if (fwrite(buf, 1, total, s.fp) != total) {
                set_err("SD write failed");
                finish(false);
                s.st = SAVE_ERR;
                mark_done();
                return;
            }
            s.bytes += total;
            /* stream to SD so huge files never grow the heap */
            dl_consume(s.dl, total);
        }

        if (r == DL_DONE) {
            finish(true);
            s.st = SAVE_OK;
            mark_done();
        } else if (r == DL_ERR) {
            set_err(dl_err(s.dl));
            finish(false); /* remove partial file */
            s.st = SAVE_ERR;
            mark_done();
        }
        return;
    }

    /* Camera: stream the original to disk first, exactly like the booru
       target does, then decode it from the file. Buffering it in RAM was
       the reason big originals failed - the transfer buffer grows by
       doubling so it can hold nearly twice the file, on top of the
       decode scratch, and old3DS does not have that much spare. On disk
       costs nothing and the dl buffer stays at one chunk. */
    if (s.fp) {
        const u8 *buf = dl_buf(s.dl);
        if (total > 0) {
            if (fwrite(buf, 1, total, s.fp) != total) {
                set_err("SD write failed");
                finish(false);
                s.st = SAVE_ERR;
                mark_done();
                return;
            }
            s.bytes += total;
            dl_consume(s.dl, total);
        }
    }
    if (r == DL_DONE) {
        if (s.fp) {
            fclose(s.fp);
            s.fp = NULL;
        }
        bool ok = camera_encode(s.path);
        if (!ok && !s_err[0])
            set_err("decode/encode failed (unsupported or corrupt)");
        if (s.dl) {
            dl_abort(s.dl);
            s.dl = NULL;
        }
        if (!ok)
            remove(s.path); /* don't leave a broken photo in the roll */
        s.st = ok ? SAVE_OK : SAVE_ERR;
        mark_done();
        if (ok)
            s_cam_notice = true;
    } else if (r == DL_ERR) {
        set_err(dl_err(s.dl));
        finish(false);
        s.st = SAVE_ERR;
        mark_done();
    }
}
