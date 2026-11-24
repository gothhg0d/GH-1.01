#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspgu.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <png.h>
#include <zlib.h>
#ifdef HAVE_JPEG
#include <setjmp.h>
#include <jpeglib.h>
#endif
#include "library.h"

Game games[2][MAXG];
int ngames[2];

static char selfPath[256];
static volatile int scanReq = 1, scanDone = 0, quit = 0;
static volatile int focusTab = 0, focusSel = 0;
static SceUID loaderTh = -1;

volatile int artState = ART_IDLE;
volatile int artTarget = 0;
Game *volatile artGame = NULL;

/* ================================================================ helpers */
static u32 le32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

static int endsWithCI(const char *s, const char *ext) {
    int ls = strlen(s), le = strlen(ext);
    return ls >= le && strcasecmp(s + ls - le, ext) == 0;
}

static void niceName(const char *file, char *out, int max) {
    snprintf(out, max, "%s", file);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
    for (char *p = out; *p; p++) if (*p == '_') *p = ' ';
}

static Color pickColor(const char *s) {
    static const Color pal[] = {
        0xFFD17000, 0xFFC83C78, 0xFF889600, 0xFF285AE6, 0xFF461EC8,
        0xFF46A028, 0xFF8C3C1E, 0xFF1478B4, 0xFF786464, 0xFFC8AA00,
    };
    u32 h = 5381;
    while (*s) h = h * 33 + (u8)*s++;
    return pal[h % (sizeof(pal) / sizeof(pal[0]))];
}

/* PARAM.SFO value by key (strings only; UTF-8 kept) */
static int sfoGet(const u8 *sfo, u32 len, const char *key, char *out, int max) {
    if (len < 20 || le32(sfo) != 0x46535000) return 0;
    u32 kt = le32(sfo + 8), dt = le32(sfo + 12), n = le32(sfo + 16);
    int kl = strlen(key) + 1;
    for (u32 i = 0; i < n && 20 + i * 16 + 16 <= len; i++) {
        const u8 *e = sfo + 20 + i * 16;
        u32 ko = e[0] | (e[1] << 8), dlen = le32(e + 4), doff = le32(e + 12);
        if (kt + ko + kl > len || dt + doff + dlen > len) continue;
        if (memcmp(sfo + kt + ko, key, kl) == 0) {
            int k = 0;
            const char *t = (const char *)sfo + dt + doff;
            while (k < (int)dlen && k < max - 1 && t[k]) { out[k] = (t[k] == '\n' || t[k] == '\r') ? ' ' : t[k]; k++; }
            out[k] = 0;
            return k > 0;
        }
    }
    return 0;
}

/* ================================================================ LZ4 block */
static int lz4dec(const u8 *src, int slen, u8 *dst, int dlen) {
    const u8 *ip = src, *iend = src + slen;
    u8 *op = dst, *oend = dst + dlen;
    while (ip < iend) {
        u32 tok = *ip++, lit = tok >> 4;
        if (lit == 15) { u8 b; do { if (ip >= iend) return -1; b = *ip++; lit += b; } while (b == 255); }
        if (op + lit > oend || ip + lit > iend) return -1;
        memcpy(op, ip, lit);
        op += lit; ip += lit;
        if (op == oend || ip >= iend) break;
        if (ip + 2 > iend) return -1;
        u32 off = ip[0] | (ip[1] << 8);
        ip += 2;
        if (off == 0 || (u32)(op - dst) < off) return -1;
        u32 ml = tok & 15;
        if (ml == 15) { u8 b; do { if (ip >= iend) return -1; b = *ip++; ml += b; } while (b == 255); }
        ml += 4;
        if (op + ml > oend) return -1;
        const u8 *m = op - off;
        while (ml--) *op++ = *m++;
    }
    return op - dst;
}

/* windowBits: -15 raw deflate, 47 zlib/gzip auto */
static int inflateBuf(const u8 *src, u32 slen, u8 *dst, u32 dlen, int wbits) {
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, wbits) != Z_OK) return 0;
    z.next_in = (Bytef *)src; z.avail_in = slen;
    z.next_out = dst; z.avail_out = dlen;
    int r = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    return r == Z_STREAM_END || z.avail_out == 0;
}

/* ================================================================ disc images */
enum { F_ISO, F_CSO1, F_ZSO1, F_V2, F_DAX, F_JSO };

typedef struct {
    SceUID fd;
    int fmt, align;
    u32 bs, nblocks, idxOfs, daxVer;
    u8 *cbuf, *bbuf;
    int cached;
    u8 sec[2048];
} Disc;

static void discClose(Disc *d) {
    if (d->fd >= 0) sceIoClose(d->fd);
    free(d->cbuf);
    free(d->bbuf);
    d->fd = -1;
    d->cbuf = d->bbuf = NULL;
}

static int discOpen(Disc *d, const char *path) {
    memset(d, 0, sizeof(*d));
    d->cached = -1;
    d->fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (d->fd < 0) return 0;
    u8 h[32];
    if (sceIoRead(d->fd, h, 32) != 32) { discClose(d); return 0; }
    u32 magic = le32(h);
    if (magic == 0x4F534943 || magic == 0x4F53495A) {          /* CISO / ZISO */
        u32 hsize = le32(h + 4);
        u64 total = le32(h + 8) | ((u64)le32(h + 12) << 32);
        d->bs = le32(h + 16);
        d->align = h[21];
        int ver = h[20];
        d->fmt = ver >= 2 ? F_V2 : (magic == 0x4F534943 ? F_CSO1 : F_ZSO1);
        d->idxOfs = (ver >= 2 && hsize >= 24) ? hsize : 24;
        if (d->bs < 2048 || d->bs > 0x40000 || (d->bs % 2048)) { discClose(d); return 0; }
        d->nblocks = (u32)((total + d->bs - 1) / d->bs);
    } else if (magic == 0x00584144) {                         /* DAX */
        d->fmt = F_DAX;
        d->bs = 0x2000;
        d->daxVer = le32(h + 8);
        d->nblocks = (le32(h + 4) + 0x1FFF) / 0x2000;
        d->idxOfs = 32;
    } else if (magic == 0x4F53494A) {                         /* JISO (LZO) - icons not supported */
        discClose(d);
        return 0;
    } else {
        d->fmt = F_ISO;
        return 1;
    }
    d->cbuf = malloc(d->bs * 2 + 64);
    d->bbuf = malloc(d->bs);
    if (!d->cbuf || !d->bbuf) { discClose(d); return 0; }
    return 1;
}

static int readBlock(Disc *d, u32 b) {
    if (b >= d->nblocks) return 0;
    u32 pos, len, top;
    if (d->fmt == F_DAX) {
        u32 off;
        u16 l;
        sceIoLseek(d->fd, d->idxOfs + (SceOff)b * 4, PSP_SEEK_SET);
        if (sceIoRead(d->fd, &off, 4) != 4) return 0;
        sceIoLseek(d->fd, d->idxOfs + (SceOff)d->nblocks * 4 + (SceOff)b * 2, PSP_SEEK_SET);
        if (sceIoRead(d->fd, &l, 2) != 2) return 0;
        pos = off; len = l; top = 0;
    } else {
        u32 idx[2];
        sceIoLseek(d->fd, d->idxOfs + (SceOff)b * 4, PSP_SEEK_SET);
        if (sceIoRead(d->fd, idx, 8) != 8) return 0;
        top = idx[0] & 0x80000000;
        SceOff p0 = (SceOff)(idx[0] & 0x7FFFFFFF) << d->align;
        SceOff p1 = (SceOff)(idx[1] & 0x7FFFFFFF) << d->align;
        if (p1 < p0) return 0;
        pos = (u32)p0;
        len = (u32)(p1 - p0);
        sceIoLseek(d->fd, p0, PSP_SEEK_SET);
    }
    if (d->fmt == F_DAX) sceIoLseek(d->fd, pos, PSP_SEEK_SET);
    int stored = (d->fmt == F_CSO1 || d->fmt == F_ZSO1) ? (top != 0) : (len >= d->bs);
    if (stored) return sceIoRead(d->fd, d->bbuf, d->bs) == (int)d->bs;
    if (len > d->bs * 2 + 64) len = d->bs * 2 + 64;
    if (sceIoRead(d->fd, d->cbuf, len) <= 0) return 0;
    switch (d->fmt) {
    case F_CSO1: return inflateBuf(d->cbuf, len, d->bbuf, d->bs, -15);
    case F_ZSO1: return lz4dec(d->cbuf, len, d->bbuf, d->bs) > 0;
    case F_V2:   return top ? lz4dec(d->cbuf, len, d->bbuf, d->bs) > 0 : inflateBuf(d->cbuf, len, d->bbuf, d->bs, -15);
    case F_DAX:  return inflateBuf(d->cbuf, len, d->bbuf, d->bs, 47) || inflateBuf(d->cbuf, len, d->bbuf, d->bs, -15);
    }
    return 0;
}

static int discRead(Disc *d, u32 lba, u8 *out) {
    if (d->fmt == F_ISO) {
        sceIoLseek(d->fd, (SceOff)lba * 2048, PSP_SEEK_SET);
        return sceIoRead(d->fd, out, 2048) == 2048;
    }
    u64 off = (u64)lba * 2048;
    u32 b = (u32)(off / d->bs), within = (u32)(off % d->bs);
    if ((int)b != d->cached) {
        if (!readBlock(d, b)) { d->cached = -1; return 0; }
        d->cached = b;
    }
    memcpy(out, d->bbuf + within, 2048);
    return 1;
}

static int nameEq(const char *nm, int nl, const char *want) {
    int wl = strlen(want);
    for (int i = 0; i < nl; i++) if (nm[i] == ';') { nl = i; break; }
    if (nl > 0 && nm[nl - 1] == '.') nl--;
    return nl == wl && strncasecmp(nm, want, wl) == 0;
}

static int isoFind(Disc *d, const char *path, u32 *lba, u32 *size) {
    u8 *s = d->sec;
    if (!discRead(d, 16, s) || memcmp(s + 1, "CD001", 5) != 0) return 0;
    u32 dl = le32(s + 156 + 2), ds = le32(s + 156 + 10);
    const char *p = path;
    while (*p) {
        char comp[32];
        int n = 0;
        while (*p && *p != '/' && n < 31) comp[n++] = *p++;
        comp[n] = 0;
        if (*p == '/') p++;
        int found = 0;
        for (u32 off = 0; off < ds && !found; off += 2048) {
            if (!discRead(d, dl + off / 2048, s)) return 0;
            for (int i = 0; i < 2048 - 33;) {
                int len = s[i];
                if (len == 0) break;
                if (nameEq((char *)s + i + 33, s[i + 32], comp)) {
                    dl = le32(s + i + 2);
                    ds = le32(s + i + 10);
                    found = 1;
                    break;
                }
                i += len;
            }
        }
        if (!found) return 0;
    }
    *lba = dl;
    *size = ds;
    return 1;
}

static u8 *isoReadFile(Disc *d, const char *path, u32 *outLen, u32 maxLen) {
    u32 lba, size;
    if (!isoFind(d, path, &lba, &size) || size == 0 || size > maxLen) return NULL;
    u8 *buf = malloc(size);
    if (!buf) return NULL;
    for (u32 off = 0; off < size; off += 2048) {
        if (!discRead(d, lba + off / 2048, d->sec)) { free(buf); return NULL; }
        u32 n = size - off < 2048 ? size - off : 2048;
        memcpy(buf + off, d->sec, n);
    }
    *outLen = size;
    return buf;
}

/* ================================================================ PBP */
#define PBP_SFO   0
#define PBP_ICON0 1
#define PBP_PIC1  4

static u8 *pbpReadSection(const char *path, int sect, u32 *outLen, u32 maxLen) {
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return NULL;
    u32 hdr[10];
    u8 *buf = NULL;
    if (sceIoRead(fd, hdr, sizeof(hdr)) == sizeof(hdr) && hdr[0] == 0x50425000) {
        u32 off = hdr[2 + sect], end = hdr[3 + sect];
        if (end > off && end - off <= maxLen) {
            u32 len = end - off;
            buf = malloc(len);
            if (buf) {
                sceIoLseek32(fd, off, PSP_SEEK_SET);
                if (sceIoRead(fd, buf, len) != (int)len) { free(buf); buf = NULL; }
                else *outLen = len;
            }
        }
    }
    sceIoClose(fd);
    return buf;
}

static u8 *readAsset(const Game *g, const char *isoPath, int pbpSect, u32 *len, u32 maxLen) {
    if (g->type != T_ISO) return pbpReadSection(g->path, pbpSect, len, maxLen);
    Disc d;
    if (!discOpen(&d, g->path)) return NULL;
    u8 *b = isoReadFile(&d, isoPath, len, maxLen);
    discClose(&d);
    return b;
}

/* ================================================================ images */
static u32 *decodePng(const u8 *buf, u32 len, int *w, int *h, int maxDim) {
    png_image img;
    memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&img, buf, len)) return NULL;
    img.format = PNG_FORMAT_RGBA;
    if ((int)img.width > maxDim || (int)img.height > maxDim) { png_image_free(&img); return NULL; }
    u32 *px = malloc(PNG_IMAGE_SIZE(img));
    if (!px) { png_image_free(&img); return NULL; }
    if (!png_image_finish_read(&img, NULL, px, 0, NULL)) { free(px); return NULL; }
    *w = img.width;
    *h = img.height;
    return px; /* RGBA bytes == PSP ABGR u32 */
}

#ifdef HAVE_JPEG
struct jerr { struct jpeg_error_mgr pub; jmp_buf jb; };
static void jexit(j_common_ptr c) { longjmp(((struct jerr *)c->err)->jb, 1); }

static u32 *decodeJpeg(const u8 *buf, u32 len, int *w, int *h, int tw, int th) {
    struct jpeg_decompress_struct ci;
    struct jerr je;
    u32 *volatile px = NULL;
    u8 *volatile row = NULL;
    ci.err = jpeg_std_error(&je.pub);
    je.pub.error_exit = jexit;
    if (setjmp(je.jb)) {
        jpeg_destroy_decompress(&ci);
        free(px);
        free(row);
        return NULL;
    }
    jpeg_create_decompress(&ci);
    jpeg_mem_src(&ci, (unsigned char *)buf, len);
    jpeg_read_header(&ci, TRUE);
    ci.out_color_space = JCS_RGB;
    ci.scale_num = 1;
    ci.scale_denom = 1;
    /* let libjpeg downscale (1/2, 1/4, 1/8) while the result still covers tw x th: much faster */
    while (ci.scale_denom < 8 && ci.image_width / (ci.scale_denom * 2) >= (unsigned)tw && ci.image_height / (ci.scale_denom * 2) >= (unsigned)th)
        ci.scale_denom *= 2;
    jpeg_start_decompress(&ci);
    int ow = ci.output_width, oh = ci.output_height;
    if (ow > 2048 || oh > 2048) longjmp(je.jb, 1);
    px = malloc(ow * oh * 4);
    row = malloc(ow * 3);
    if (!px || !row) longjmp(je.jb, 1);
    while ((int)ci.output_scanline < oh) {
        JSAMPROW r = row;
        int y = ci.output_scanline;
        jpeg_read_scanlines(&ci, &r, 1);
        for (int x = 0; x < ow; x++)
            px[y * ow + x] = 0xFF000000u | (row[x * 3 + 2] << 16) | (row[x * 3 + 1] << 8) | row[x * 3];
    }
    jpeg_finish_decompress(&ci);
    jpeg_destroy_decompress(&ci);
    free(row);
    *w = ow;
    *h = oh;
    return px;
}
#endif

static u32 *decodeImage(const u8 *buf, u32 len, int *w, int *h, int maxDim, int tw, int th) {
    if (len > 8 && buf[0] == 0x89 && buf[1] == 'P') return decodePng(buf, len, w, h, maxDim);
#ifdef HAVE_JPEG
    if (len > 4 && buf[0] == 0xFF && buf[1] == 0xD8) return decodeJpeg(buf, len, w, h, tw, th);
#else
    (void)tw; (void)th;
#endif
    return NULL;
}

static u8 *readWholeFile(const char *path, int *len, int maxLen) {
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return NULL;
    int n = sceIoLseek32(fd, 0, PSP_SEEK_END);
    sceIoLseek32(fd, 0, PSP_SEEK_SET);
    u8 *buf = (n > 0 && n <= maxLen) ? malloc(n) : NULL;
    if (buf && sceIoRead(fd, buf, n) != n) { free(buf); buf = NULL; }
    sceIoClose(fd);
    *len = n;
    return buf;
}

/* bilinear sample, straight RGBA */
static u32 sample(const u32 *px, int w, int h, float fx, float fy) {
    if (fx < 0) fx = 0;
    if (fy < 0) fy = 0;
    int x0 = (int)fx, y0 = (int)fy;
    if (x0 > w - 1) x0 = w - 1;
    if (y0 > h - 1) y0 = h - 1;
    int x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
    int ax = (int)((fx - x0) * 256), ay = (int)((fy - y0) * 256);
    if (ax > 256) ax = 256;
    if (ay > 256) ay = 256;
    u32 c00 = px[y0 * w + x0], c10 = px[y0 * w + x1], c01 = px[y1 * w + x0], c11 = px[y1 * w + x1];
    u32 out = 0;
    for (int s = 0; s < 32; s += 8) {
        int a = (c00 >> s) & 255, b = (c10 >> s) & 255, c = (c01 >> s) & 255, d = (c11 >> s) & 255;
        int top = a + (((b - a) * ax) >> 8), bot = c + (((d - c) * ax) >> 8);
        out |= (u32)((top + (((bot - top) * ay) >> 8)) & 255) << s;
    }
    return out;
}

/* area-average downscale into dst (dw x dh, stride ds); alpha-weighted so
   transparent icons get no dark fringes */
static void resampleBox(const u32 *px, int w, int h, u32 *dst, int dw, int dh, int ds) {
    for (int y = 0; y < dh; y++) {
        int sy0 = y * h / dh, sy1 = (y + 1) * h / dh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < dw; x++) {
            int sx0 = x * w / dw, sx1 = (x + 1) * w / dw;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (w <= dw * 1 && h <= dh * 1) { /* upscale: bilinear */
                dst[y * ds + x] = sample(px, w, h, (x + 0.5f) * w / dw - 0.5f, (y + 0.5f) * h / dh - 0.5f);
                continue;
            }
            u32 r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int yy = sy0; yy < sy1 && yy < h; yy++)
                for (int xx = sx0; xx < sx1 && xx < w; xx++) {
                    u32 c = px[yy * w + xx], ca = c >> 24;
                    r += (c & 255) * ca; g += ((c >> 8) & 255) * ca; b += ((c >> 16) & 255) * ca;
                    a += ca; n++;
                }
            if (a == 0) { dst[y * ds + x] = 0; continue; }
            dst[y * ds + x] = ((a / n) << 24) | ((b / a) << 16) | ((g / a) << 8) | (r / a);
        }
    }
}

static int pow2(int v) { int p = 1; while (p < v) p <<= 1; return p; }

static int loadIcon(Game *g) {
    u32 len = 0;
    u8 *png = readAsset(g, "PSP_GAME/ICON0.PNG", PBP_ICON0, &len, 512 * 1024);
    if (!png) return 0;
    int w, h;
    u32 *px = decodeImage(png, len, &w, &h, 1024, ICON_W, ICON_H);
    free(png);
    if (!px) return 0;
    /* fit inside 144x80 keeping the aspect ratio */
    int dw = ICON_W, dh = h * ICON_W / w;
    if (dh > ICON_H) { dh = ICON_H; dw = w * ICON_H / h; }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int ds = (dw + 7) & ~7;                 /* 16-byte rows for the GPU */
    u32 *tmp = malloc(ds * dh * 4);
    u16 *dst = memalign(16, ds * dh * 2);
    if (!tmp || !dst) { free(px); free(tmp); free(dst); return 0; }
    memset(tmp, 0, ds * dh * 4);
    resampleBox(px, w, h, tmp, dw, dh, ds);
    free(px);
    u32 sr = 0, sg = 0, sb = 0, cnt = 0;
    for (int y = 0; y < dh; y += 2)
        for (int x = 0; x < dw; x += 2) {
            u32 c = tmp[y * ds + x];
            if ((c >> 24) > 160) { sr += c & 255; sg += (c >> 8) & 255; sb += (c >> 16) & 255; cnt++; }
        }
    g->iconAvg = cnt ? RGB(sr / cnt, sg / cnt, sb / cnt) : g->color;
    /* 16-bit RGBA5551 (half the memory and GPU bandwidth of 8888): fully
       transparent pixels stay transparent, soft edges are blended onto the
       tile color the icon is drawn on; ordered dither hides the banding */
    static const int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    u32 bg = colLerp(g->iconAvg, RGB(0, 0, 0), 0.15f);
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < ds; x++) {
            u32 c = tmp[y * ds + x], a = c >> 24;
            if (x >= dw || a < 24) { dst[y * ds + x] = 0; continue; }
            int d = bayer[y & 3][x & 3] >> 1;
            int r = ((c & 255) * a + (bg & 255) * (255 - a)) / 255 + d;
            int gg = (((c >> 8) & 255) * a + ((bg >> 8) & 255) * (255 - a)) / 255 + d;
            int b = (((c >> 16) & 255) * a + ((bg >> 16) & 255) * (255 - a)) / 255 + d;
            if (r > 255) r = 255;
            if (gg > 255) gg = 255;
            if (b > 255) b = 255;
            dst[y * ds + x] = (u16)((r >> 3) | ((gg >> 3) << 5) | ((b >> 3) << 10) | 0x8000);
        }
    free(tmp);
    sceKernelDcacheWritebackRange(dst, ds * dh * 2);
    g->icon.w = dw; g->icon.h = dh;
    g->icon.tw = pow2(ds); g->icon.th = pow2(dh);
    g->icon.stride = ds;
    g->icon.psm = GU_PSM_5551;
    g->icon.data = dst;
    return 1;
}

/* cover-scale RGBA to 480x272 RGB565 with ordered dither (+ optional box blur) */
static void toScreen565(const u32 *px, int w, int h, u16 *dst, int blur) {
    static const int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    float sc = 480.0f / w;
    if (272.0f / h > sc) sc = 272.0f / h;
    float ox = (w * sc - 480) / 2, oy = (h * sc - 272) / 2;
    u32 *tmp = NULL;
    if (blur > 0) tmp = malloc(480 * 272 * 4);
    static u16 row[480];
    u32 *line = tmp;
    static u32 lbuf[480];
    for (int y = 0; y < 272; y++) {
        u32 *L = tmp ? tmp + y * 480 : lbuf;
        for (int x = 0; x < 480; x++) {
            float fx = (x + 0.5f + ox) / sc - 0.5f, fy = (y + 0.5f + oy) / sc - 0.5f;
            L[x] = (sc == 1.0f && ox == 0 && oy == 0) ? px[y * w + x] : sample(px, w, h, fx, fy);
        }
        if (!tmp) {
            for (int x = 0; x < 480; x++) {
                u32 c = L[x];
                int d = bayer[y & 3][x & 3] >> 1;
                int r = (c & 255) + d, g = ((c >> 8) & 255) + (d >> 1), b = ((c >> 16) & 255) + d;
                row[x] = (u16)(((r > 255 ? 255 : r) >> 3) | (((g > 255 ? 255 : g) >> 2) << 5) | (((b > 255 ? 255 : b) >> 3) << 11));
            }
            memcpy(dst + y * 480, row, sizeof(row));
        }
    }
    if (!tmp) return;
    /* separable box blur with running sums: cost does not depend on the
       radius (the old per-pixel loop was ~17x slower at "Сильное");
       2 passes approximate a gaussian */
    int rad = blur == 1 ? 2 : (blur == 2 ? 4 : 8);
    u32 *t2 = malloc(480 * 272 * 4);
    if (t2) {
        for (int pass = 0; pass < 2; pass++) {
            for (int dir = 0; dir < 2; dir++) {
                const u32 *src = dir ? t2 : tmp;
                u32 *out = dir ? tmp : t2;
                int len = dir ? 272 : 480, lines = dir ? 480 : 272;
                int step = dir ? 480 : 1, lstep = dir ? 1 : 480;
                for (int l = 0; l < lines; l++) {
                    const u32 *s = src + l * lstep;
                    u32 *o = out + l * lstep;
                    int r = 0, g = 0, b = 0, n = 0;
                    for (int k = 0; k <= rad && k < len; k++) {
                        u32 c = s[k * step];
                        r += c & 255; g += (c >> 8) & 255; b += (c >> 16) & 255; n++;
                    }
                    for (int i = 0; i < len; i++) {
                        o[i * step] = 0xFF000000u | ((b / n) << 16) | ((g / n) << 8) | (r / n);
                        int add = i + rad + 1, rem = i - rad;
                        if (add < len) { u32 c = s[add * step]; r += c & 255; g += (c >> 8) & 255; b += (c >> 16) & 255; n++; }
                        if (rem >= 0) { u32 c = s[rem * step]; r -= c & 255; g -= (c >> 8) & 255; b -= (c >> 16) & 255; n--; }
                    }
                }
            }
        }
        free(t2);
    }
    for (int y = 0; y < 272; y++) {
        for (int x = 0; x < 480; x++) {
            u32 c = line[y * 480 + x];
            int d = bayer[y & 3][x & 3] >> 1;
            int r = (c & 255) + d, g = ((c >> 8) & 255) + (d >> 1), b = ((c >> 16) & 255) + d;
            row[x] = (u16)(((r > 255 ? 255 : r) >> 3) | (((g > 255 ? 255 : g) >> 2) << 5) | (((b > 255 ? 255 : b) >> 3) << 11));
        }
        memcpy(dst + y * 480, row, sizeof(row));
    }
    free(tmp);
}

static int loadArt(Game *g, int slot) {
    u32 len = 0;
    u8 *png = readAsset(g, "PSP_GAME/PIC1.PNG", PBP_PIC1, &len, 1024 * 1024);
    if (!png) return 0;
    int w, h;
    u32 *px = decodeImage(png, len, &w, &h, 1024, 480, 272);
    free(png);
    if (!px) return 0;
    toScreen565(px, w, h, gfxArtSlotPixels(slot), 0);
    free(px);
    return 1;
}

/* ================================================================ wallpapers */
char wallFiles[MAXWALL][192];
int nwall;
static u16 *wallBuf[2];
static volatile int wallShown = -1, wallSerial = 0, wallBusy = 0, wallReq = 0, wallBlurReq = 0;
static char wallPathReq[192];

static void scanWallDir(const char *dir, int depth) {
    SceUID d = sceIoDopen(dir);
    if (d < 0) return;
    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0 && nwall < MAXWALL) {
        char path[192];
        snprintf(path, sizeof(path), "%s/%s", dir, e.d_name);
        if (FIO_S_ISDIR(e.d_stat.st_mode)) {
            if (depth < 1 && e.d_name[0] != '.') scanWallDir(path, depth + 1);
        } else if (endsWithCI(e.d_name, ".png")
#ifdef HAVE_JPEG
                   || endsWithCI(e.d_name, ".jpg") || endsWithCI(e.d_name, ".jpeg")
#endif
        ) {
            snprintf(wallFiles[nwall++], 192, "%s", path);
        }
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
}

static char appDirL[256];
Texture wallThumb[MAXWALL];
volatile int wallThumbState[MAXWALL];
static volatile int thumbFocus = 0;

static void scanWallpapers(void) {
    char p[300];
    for (int i = 0; i < nwall; i++) { free(wallThumb[i].data); wallThumb[i].data = NULL; wallThumbState[i] = ICON_NONE; }
    nwall = 0;
    snprintf(p, sizeof(p), "%s/wallpapers", appDirL);
    scanWallDir(p, 0);
    scanWallDir("ms0:/PICTURE", 0);
    scanWallDir("ef0:/PICTURE", 0);
    for (int i = 0; i < nwall; i++) wallThumbState[i] = ICON_NONE;
}

void libThumbFocus(int idx) { thumbFocus = idx; }

/* cover-crop + area-average into a 96x54 RGB565 thumbnail */
static int loadThumb(int i) {
    int len, w, h;
    u8 *buf = readWholeFile(wallFiles[i], &len, 8 * 1024 * 1024);
    if (!buf) return 0;
    u32 *px = decodeImage(buf, len, &w, &h, 1600, THUMB_W, THUMB_H);
    free(buf);
    if (!px) return 0;
    u16 *dst = memalign(16, THUMB_W * THUMB_H * 2);
    if (!dst) { free(px); return 0; }
    float sc = (float)THUMB_W / w;
    if ((float)THUMB_H / h > sc) sc = (float)THUMB_H / h;
    float cw = THUMB_W / sc, ch = THUMB_H / sc, cx = (w - cw) / 2, cy = (h - ch) / 2;
    for (int y = 0; y < THUMB_H; y++)
        for (int x = 0; x < THUMB_W; x++) {
            int sx0 = (int)(cx + x * cw / THUMB_W), sx1 = (int)(cx + (x + 1) * cw / THUMB_W);
            int sy0 = (int)(cy + y * ch / THUMB_H), sy1 = (int)(cy + (y + 1) * ch / THUMB_H);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sy1 <= sy0) sy1 = sy0 + 1;
            u32 r = 0, g = 0, b = 0, n = 0;
            for (int yy = sy0; yy < sy1 && yy < h; yy += 1 + (sy1 - sy0) / 4)
                for (int xx = sx0; xx < sx1 && xx < w; xx += 1 + (sx1 - sx0) / 4) {
                    u32 c = px[yy * w + xx];
                    r += c & 255; g += (c >> 8) & 255; b += (c >> 16) & 255; n++;
                }
            if (!n) n = 1;
            dst[y * THUMB_W + x] = (u16)(((r / n) >> 3) | (((g / n) >> 2) << 5) | (((b / n) >> 3) << 11));
        }
    free(px);
    sceKernelDcacheWritebackRange(dst, THUMB_W * THUMB_H * 2);
    Texture *t = &wallThumb[i];
    t->w = THUMB_W; t->h = THUMB_H; t->tw = 128; t->th = 64; t->stride = THUMB_W;
    t->psm = GU_PSM_5650;
    t->data = dst;
    return 1;
}

static int nextThumb(void) {
    int f = thumbFocus;
    for (int d = 0; d < nwall; d++) {
        int a = f + d, b = f - d;
        if (a >= 0 && a < nwall && wallThumbState[a] == ICON_NONE) return a;
        if (b >= 0 && b < nwall && wallThumbState[b] == ICON_NONE) return b;
    }
    return -1;
}

void libRequestWallpaper(const char *path, int blur) {
    snprintf(wallPathReq, sizeof(wallPathReq), "%s", path ? path : "");
    wallBlurReq = blur;
    wallBusy = 1;
    wallReq = 1;
}

int libWallpaper(Texture *t) {
    int s = wallShown;
    if (s < 0 || !wallBuf[s]) return 0;
    t->data = wallBuf[s];
    t->w = 480; t->h = 272; t->tw = 512; t->th = 512; t->stride = 480;
    t->psm = GU_PSM_5650;
    return 1;
}
int libWallpaperSerial(void) { return wallSerial; }
int libWallpaperBusy(void) { return wallBusy; }

static void loadWallpaper(void) {
    char path[192];
    snprintf(path, sizeof(path), "%s", wallPathReq);
    int blur = wallBlurReq;
    wallReq = 0;
    if (!path[0]) { wallShown = -1; wallSerial++; wallBusy = 0; return; }
    int slot = wallShown == 0 ? 1 : 0;
    if (!wallBuf[slot]) wallBuf[slot] = memalign(16, 480 * 272 * 2);
    int len, w, h;
    u8 *buf = wallBuf[slot] ? readWholeFile(path, &len, 8 * 1024 * 1024) : NULL;
    u32 *px = buf ? decodeImage(buf, len, &w, &h, 1600, 480, 272) : NULL;
    free(buf);
    if (px && !wallReq) {
        toScreen565(px, w, h, wallBuf[slot], blur);
        sceKernelDcacheWritebackRange(wallBuf[slot], 480 * 272 * 2);
        wallShown = slot;
        wallSerial++;
    }
    free(px);
    if (!wallReq) wallBusy = 0;
}

/* ================================================================ scanning */
static Game *addGame(int tab, const char *name, const char *path, int type, u32 mb, const char *fmt) {
    if (ngames[tab] >= MAXG) return NULL;
    Game *g = &games[tab][ngames[tab]++];
    memset(g, 0, sizeof(*g));
    snprintf(g->name, sizeof(g->name), "%s", name);
    snprintf(g->path, sizeof(g->path), "%s", path);
    snprintf(g->fmt, sizeof(g->fmt), "%s", fmt);
    g->type = type;
    g->sizeMB = mb;
    g->color = pickColor(name);
    g->iconAvg = g->color;
    return g;
}

static const char *isoExt(const char *n) {
    static const char *ext[] = {".iso", ".cso", ".zso", ".dax", ".jso", ".img"};
    static const char *fmt[] = {"ISO", "CSO", "ZSO", "DAX", "JSO", "ISO"};
    for (int i = 0; i < 6; i++) if (endsWithCI(n, ext[i])) return fmt[i];
    return NULL;
}

static void scanISO(const char *dir, int depth) {
    SceUID d = sceIoDopen(dir);
    if (d < 0) return;
    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0 && !quit) {
        char path[192];
        snprintf(path, sizeof(path), "%s/%s", dir, e.d_name);
        const char *fmt;
        if (FIO_S_ISDIR(e.d_stat.st_mode)) {
            if (depth < 2 && e.d_name[0] != '.') scanISO(path, depth + 1);
        } else if ((fmt = isoExt(e.d_name)) != NULL) {
            char name[96];
            niceName(e.d_name, name, sizeof(name));
            Game *g = addGame(0, name, path, T_ISO, (u32)(e.d_stat.st_size >> 20), fmt);
            if (!g) continue;
            Disc dc;
            if (discOpen(&dc, path)) {
                u32 len = 0, l2, s2;
                u8 *sfo = isoReadFile(&dc, "PSP_GAME/PARAM.SFO", &len, 64 * 1024);
                if (sfo) {
                    char t[96];
                    if (sfoGet(sfo, len, "TITLE", t, sizeof(t))) snprintf(g->name, sizeof(g->name), "%s", t);
                    sfoGet(sfo, len, "DISC_ID", g->discId, sizeof(g->discId));
                    free(sfo);
                }
                g->patched = isoFind(&dc, "PSP_GAME/SYSDIR/EBOOT.OLD", &l2, &s2);
                discClose(&dc);
            }
            g->color = pickColor(g->name);
            g->iconAvg = g->color;
        }
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
}

static void scanEboots(const char *root) {
    SceUID d = sceIoDopen(root);
    if (d < 0) return;
    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0 && !quit) {
        if (FIO_S_ISDIR(e.d_stat.st_mode) && e.d_name[0] != '.') {
            char path[192];
            SceIoStat st;
            snprintf(path, sizeof(path), "%s/%s/EBOOT.PBP", root, e.d_name);
            if (sceIoGetstat(path, &st) >= 0 && strcasecmp(path, selfPath) != 0) {
                char name[96], cat[8] = "", id[16] = "";
                u32 len = 0;
                u8 *sfo = pbpReadSection(path, PBP_SFO, &len, 64 * 1024);
                if (!sfo || !sfoGet(sfo, len, "TITLE", name, sizeof(name))) niceName(e.d_name, name, sizeof(name));
                if (sfo) {
                    sfoGet(sfo, len, "CATEGORY", cat, sizeof(cat));
                    sfoGet(sfo, len, "DISC_ID", id, sizeof(id));
                }
                free(sfo);
                int type = T_APP, tab = 1;
                if (strcmp(cat, "ME") == 0) { type = T_PS1; tab = 0; }
                else if (strcmp(cat, "EG") == 0) { type = T_PSN; tab = 0; }
                Game *g = addGame(tab, name, path, type, (u32)(st.st_size >> 20), "PBP");
                if (g) snprintf(g->discId, sizeof(g->discId), "%s", id);
            }
        }
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
}

static int cmpGame(const void *a, const void *b) {
    return strcasecmp(((const Game *)a)->name, ((const Game *)b)->name);
}

static void freeAll(void) {
    for (int t = 0; t < 2; t++)
        for (int i = 0; i < ngames[t]; i++) {
            free(games[t][i].icon.data);
            games[t][i].icon.data = NULL;
        }
    ngames[0] = ngames[1] = 0;
}

static void scanAll(void) {
    freeAll();
    scanISO("ms0:/ISO", 0);
    scanISO("ef0:/ISO", 0);
    scanEboots("ms0:/PSP/GAME");
    scanEboots("ef0:/PSP/GAME");
    qsort(games[0], ngames[0], sizeof(Game), cmpGame);
    qsort(games[1], ngames[1], sizeof(Game), cmpGame);
    scanWallpapers();
}

/* ================================================================ loader */
static Game *nextIcon(void) {
    int t = focusTab, s = focusSel;
    if (t < 0 || t > 1) return NULL;
    static const int order[] = {0, 1, 2, 3, 4, 5, 6, -1, 7, 8, -2, 9, 10};
    for (unsigned k = 0; k < sizeof(order) / sizeof(order[0]); k++) {
        int i = s + order[k];
        if (i >= 0 && i < ngames[t] && games[t][i].iconState == ICON_NONE) return &games[t][i];
    }
    for (int tt = 0; tt < 2; tt++) {
        int ot = tt == 0 ? t : 1 - t;
        for (int i = 0; i < ngames[ot]; i++)
            if (games[ot][i].iconState == ICON_NONE) return &games[ot][i];
    }
    return NULL;
}

static int loaderThread(SceSize args, void *argp) {
    (void)args; (void)argp;
    while (!quit) {
        if (scanReq) {
            scanReq = 0;
            scanAll();
            scanDone = 1;
            continue;
        }
        if (wallReq) { loadWallpaper(); continue; }
        if (artState == ART_LOADING) {
            Game *g = artGame;
            if (g && loadArt(g, artTarget)) artState = ART_READY;
            else { if (g) g->noArt = 1; artState = ART_FAILED; }
            continue;
        }
        Game *g = scanDone ? nextIcon() : NULL;
        if (g) {
            g->iconState = loadIcon(g) ? ICON_READY : ICON_FAILED;
            continue;
        }
        int t = scanDone ? nextThumb() : -1;
        if (t >= 0) {
            wallThumbState[t] = loadThumb(t) ? ICON_READY : ICON_FAILED;
            continue;
        }
        sceKernelDelayThread(20000);
    }
    return 0;
}

void libStart(const char *self, const char *appDir) {
    snprintf(selfPath, sizeof(selfPath), "%s", self);
    snprintf(appDirL, sizeof(appDirL), "%s", appDir);
    quit = 0;
    scanReq = 1;
    scanDone = 0;
    loaderTh = sceKernelCreateThread("loader", loaderThread, 0x30, 0x20000, PSP_THREAD_ATTR_USER, NULL);
    if (loaderTh >= 0) sceKernelStartThread(loaderTh, 0, NULL);
    else { scanAll(); scanDone = 1; }
}

int libScanDone(void) { return scanDone; }
void libRescan(void) { artState = ART_IDLE; scanDone = 0; scanReq = 1; }
void libFocus(int tab, int sel) { focusTab = tab; focusSel = sel; }
void libRequestArt(Game *g, int slot) { artGame = g; artTarget = slot; artState = ART_LOADING; }
