/* aasset_patch.c -- NDK AAssetManager for Geometry Wars 3, backed by the
 * game's packed OBB ("DAWL" WAD container) and, as a fallback, the local
 * filesystem under g_data_path.
 *
 * The engine reads every data file through AAssetManager_open / AAsset_read /
 * AAsset_close / AAsset_getLength / AAsset_getRemainingLength / AAsset_seek /
 * AAssetManager_fromJava. On Android these come from libandroid.so and are
 * served out of the APK assets + OBB expansion file. This port ships no APK
 * assets, so everything lives in the OBB.
 *
 * OBB / WAD format (reverse-engineered from libgwnext.so WadArchive):
 *   - The whole file is obfuscated with a 2-byte, offset-parity XOR key:
 *       plain[i] = file[i] ^ (i & 1 ? 0x66 : 0xAF)
 *   - Header @ 0x000:  "DAWL" | u32 version(1) | u32 format(1) | u32 (0)
 *   - FAT    @ 0x800:  u32 numDirs
 *                      numDirs   * { u32 parent, u32 nameOff }      (8 bytes)
 *                      u32 numFiles
 *                      numFiles  * WadFatEntry                      (48 bytes)
 *                      u32 nameTableSize                            (0 in retail)
 *   - WadFatEntry (48 bytes, little-endian):
 *       +0x00 u32  hash        = CRC-32 of the lower-cased relative path
 *                               (poly 0xEDB88320, seed 0xFFFFFFFF, NO final XOR)
 *       +0x04 u32  ?           (platform/flags -- unused here)
 *       +0x08 u32  ?
 *       +0x0c u32  ?
 *       +0x10 u64  dataOffset  (absolute, into the de-obfuscated file)
 *       +0x18 u64  size
 *       +0x20 u64  size again  (stored uncompressed == compressed)
 *       +0x28 u64  0
 *   Lookups are pure hash: GetFatEntry(name) lower-cases name, CRC-32s it and
 *   binary-searches the FAT (which the packer sorts by hash). Paths use '/'
 *   and are relative with no leading slash, e.g. "common/data.bin".
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <dirent.h>

#include "aasset_patch.h"
#include "qol.h"

extern char g_data_path[512];

typedef struct AAssetManager AAssetManager;
typedef struct AAsset        AAsset;

/* ── WAD container ───────────────────────────────────────────────────────── */

static inline uint8_t wad_key(uint64_t i) { return (i & 1) ? 0x66 : 0xAF; }

typedef struct {
    uint32_t hash;
    uint64_t off;
    uint64_t size;
} wad_entry_t;

static const uint8_t *g_wad_map = NULL;   /* mmap of the whole .obb (obfuscated) */
static size_t          g_wad_len = 0;
static wad_entry_t     *g_wad_ent = NULL;
static uint32_t         g_wad_n   = 0;

static uint32_t g_crc_tab[256];

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_tab[i] = c;
    }
}

/* Engine's WAD hash: running CRC-32 seeded with 0xFFFFFFFF, no final complement. */
static uint32_t wad_hash(const char *s) {
    uint32_t h = 0xFFFFFFFFu;
    for (; *s; ++s) {
        unsigned char b = (unsigned char)tolower((unsigned char)*s);
        h = g_crc_tab[(h ^ b) & 0xFF] ^ (h >> 8);
    }
    return h;
}

/* Read little-endian words through the de-obfuscation XOR. */
static uint32_t wad_u32(uint64_t i) {
    return  (uint32_t)(g_wad_map[i + 0] ^ wad_key(i + 0))
         | ((uint32_t)(g_wad_map[i + 1] ^ wad_key(i + 1)) << 8)
         | ((uint32_t)(g_wad_map[i + 2] ^ wad_key(i + 2)) << 16)
         | ((uint32_t)(g_wad_map[i + 3] ^ wad_key(i + 3)) << 24);
}
static uint64_t wad_u64(uint64_t i) {
    return (uint64_t)wad_u32(i) | ((uint64_t)wad_u32(i + 4) << 32);
}

static int wad_cmp(const void *a, const void *b) {
    uint32_t x = ((const wad_entry_t *)a)->hash;
    uint32_t y = ((const wad_entry_t *)b)->hash;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static const wad_entry_t *wad_find(const char *norm_name) {
    if (!g_wad_ent) return NULL;
    wad_entry_t key = { wad_hash(norm_name), 0, 0 };
    return (const wad_entry_t *)bsearch(&key, g_wad_ent, g_wad_n,
                                       sizeof(*g_wad_ent), wad_cmp);
}

/* Lower-case, '\'->'/', drop a leading "./" or "/". Result fits in out[512]. */
static void wad_normalise(const char *in, char *out, size_t outsz) {
    while (*in == '/' || (in[0] == '.' && in[1] == '/')) in += (*in == '/') ? 1 : 2;
    size_t j = 0;
    for (; in[0] && j + 1 < outsz; ++in) {
        char c = in[0];
        if (c == '\\') c = '/';
        out[j++] = (char)tolower((unsigned char)c);
    }
    out[j] = 0;
}

static void wad_mount(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "wad: cannot open %s\n", path); return; }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0x820) {
        fprintf(stderr, "wad: %s too small\n", path);
        close(fd);
        return;
    }

    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { fprintf(stderr, "wad: mmap %s failed\n", path); return; }

    g_wad_map = (const uint8_t *)m;
    g_wad_len = (size_t)st.st_size;

    if (wad_u32(0) != 0x4C574144u /* "DAWL" */) {
        fprintf(stderr, "wad: %s bad magic %08x\n", path, wad_u32(0));
        munmap(m, g_wad_len);
        g_wad_map = NULL;
        return;
    }

    uint64_t off = 0x800;
    uint32_t num_dirs = wad_u32(off); off += 4;
    off += (uint64_t)num_dirs * 8;
    if (off + 4 > g_wad_len) { fprintf(stderr, "wad: FAT truncated\n"); goto fail; }
    uint32_t num_files = wad_u32(off); off += 4;
    if (off + (uint64_t)num_files * 48 > g_wad_len) {
        fprintf(stderr, "wad: FAT truncated (%u files)\n", num_files);
        goto fail;
    }

    g_wad_ent = calloc(num_files ? num_files : 1, sizeof(*g_wad_ent));
    if (!g_wad_ent) goto fail;

    for (uint32_t i = 0; i < num_files; i++) {
        uint64_t e = off + (uint64_t)i * 48;
        g_wad_ent[i].hash = wad_u32(e + 0x00);
        g_wad_ent[i].off  = wad_u64(e + 0x10);
        g_wad_ent[i].size = wad_u64(e + 0x18);
    }
    g_wad_n = num_files;
    qsort(g_wad_ent, g_wad_n, sizeof(*g_wad_ent), wad_cmp);

    fprintf(stderr, "wad: mounted %s (%u dirs, %u files)\n", path, num_dirs, g_wad_n);
    return;

fail:
    if (g_wad_map) { munmap((void *)g_wad_map, g_wad_len); g_wad_map = NULL; }
    free(g_wad_ent); g_wad_ent = NULL; g_wad_n = 0;
}

/* GW3 loads "<privateFilesDir>savegame.dat" through AAssetManager on startup
 * and dereferences the result WITHOUT a NULL check (crash at libgwnext+0x236b10,
 * `this` == NULL, reached via sub_0x2260a0).  On Android the file is created by
 * the Java layer before the native side ever asks for it; here nothing does, so
 * on a fresh install the very first frame segfaults.  Touch an empty one — the
 * engine treats a zero-length save as "no progress yet" and boots normally. */
static void ensure_savegame(void) {
    char path[640];
    snprintf(path, sizeof(path), "%s/savegame.dat", g_data_path);
    if (access(path, F_OK) == 0) return;
    FILE *fp = fopen(path, "wb");
    if (fp) { fclose(fp); fprintf(stderr, "savegame: created empty %s\n", path); }
    else     fprintf(stderr, "savegame: CANNOT create %s\n", path);
}

/* main.<build>.com.activision.gw3.dimensions.obb: the newest build present
 * (101 = 1.3.0 "Evolved", 35/41 = 1.0.x), else the first *.obb found. */
static void wad_mount_auto(void) {
    ensure_savegame();
    char path[640], best[300] = "";
    int best_build = -1;
    DIR *d = opendir(g_data_path);
    if (!d) { fprintf(stderr, "wad: no OBB under %s\n", g_data_path); return; }
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t n = strlen(de->d_name);
        if (n <= 4 || strcasecmp(de->d_name + n - 4, ".obb") != 0) continue;
        int build = -1;
        if (sscanf(de->d_name, "main.%d.", &build) != 1) build = 0;
        if (build > best_build) { best_build = build; snprintf(best, sizeof best, "%s", de->d_name); }
    }
    closedir(d);
    if (!best[0]) { fprintf(stderr, "wad: no OBB under %s\n", g_data_path); return; }
    snprintf(path, sizeof(path), "%s/%s", g_data_path, best);
    wad_mount(path);
}

/* A whole file from the OBB, de-obfuscated into a malloc'd buffer. */
int aasset_load(const char *name, uint8_t **out, size_t *len) {
    char norm[512];
    wad_normalise(name, norm, sizeof(norm));
    const wad_entry_t *e = wad_find(norm);
    if (!e || e->off + e->size > g_wad_len) return 0;
    uint8_t *buf = malloc(e->size ? e->size : 1);
    if (!buf) return 0;
    for (uint64_t j = 0; j < e->size; j++) buf[j] = g_wad_map[e->off + j] ^ wad_key(e->off + j);
    *out = buf; *len = (size_t)e->size;
    return 1;
}

/* ── AAssetManager ──────────────────────────────────────────────────────── */

typedef struct {
    FILE          *fp;      /* filesystem-backed                              */
    const uint8_t *mem;     /* in-memory: wad map, or owned buffer            */
    void          *owned;   /* malloc'd buffer to free on close (mem == owned)*/
    uint64_t       base;    /* absolute file offset of mem[0] (XOR parity)    */
    int            xor_on;  /* de-obfuscate on read (raw wad extent)          */
    long           length;
    long           pos;
    char           path[1024];
} aasset_t;

/* Strip colour-management / metadata chunks from a PNG so the engine's
 * statically-linked libpng never takes its (broken) benign-error path on
 * e.g. "iCCP: known incorrect sRGB profile". Keeps only the chunks needed to
 * decode pixels. Returns a malloc'd buffer (caller frees) or NULL to fall back
 * to the raw bytes. in[] must already be de-obfuscated. */
static uint8_t *png_strip_ancillary(const uint8_t *in, size_t in_len, size_t *out_len) {
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if (in_len < 8 + 12 || memcmp(in, sig, 8) != 0) return NULL;

    uint8_t *out = malloc(in_len);
    if (!out) return NULL;
    memcpy(out, sig, 8);
    size_t w = 8, r = 8;

    while (r + 8 <= in_len) {
        uint32_t clen = (uint32_t)in[r] << 24 | (uint32_t)in[r + 1] << 16
                      | (uint32_t)in[r + 2] << 8 | (uint32_t)in[r + 3];
        const uint8_t *type = in + r + 4;
        size_t chunk = 8 + (size_t)clen + 4;           /* len + type + data + crc */
        if (clen > in_len || r + chunk > in_len) break;

        int keep = !memcmp(type, "IHDR", 4) || !memcmp(type, "PLTE", 4)
                || !memcmp(type, "IDAT", 4) || !memcmp(type, "IEND", 4)
                || !memcmp(type, "tRNS", 4);
        if (keep) { memcpy(out + w, in + r, chunk); w += chunk; }

        r += chunk;
        if (!memcmp(type, "IEND", 4)) break;
    }

    if (w < 8 + 12) { free(out); return NULL; }
    *out_len = w;
    return out;
}

static int name_has_suffix(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && strcasecmp(s + ls - lf, suf) == 0;
}

AAssetManager *AAssetManager_fromJava(void *env, void *jassetmanager) {
    (void)env; (void)jassetmanager;
    static int marker = 0x4141;
    return (AAssetManager *)&marker;
}

/* filesystem-backed asset: the path as given, then under the data dir */
static int open_file(aasset_t *a, const char *filename) {
    FILE *fp = NULL;
    if (filename[0] == '/') {
        strncpy(a->path, filename, sizeof(a->path) - 1);
        fp = fopen(a->path, "rb");
    } else {
        snprintf(a->path, sizeof(a->path), "%s/%s", g_data_path, filename);
        fp = fopen(a->path, "rb");
        if (!fp) { strncpy(a->path, filename, sizeof(a->path) - 1); fp = fopen(a->path, "rb"); }
    }
    if (!fp) return 0;
    fseek(fp, 0, SEEK_END);
    a->length = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    a->fp = fp;
    return 1;
}

/* mode: AASSET_MODE_UNKNOWN=0, RANDOM=1, STREAMING=2, BUFFER=3 (ignored) */
AAsset *AAssetManager_open(AAssetManager *mgr, const char *filename, int mode) {
    (void)mgr; (void)mode;
    if (!filename) return NULL;

    aasset_t *a = calloc(1, sizeof(aasset_t));
    if (!a) return NULL;

    static int trace = -1;
    if (trace < 0) trace = getenv("GW3_TRACE_ASSETS") != NULL;

    /* 0) our own prompt images / the achievements list (qol.c) */
    if (filename[0] != '/') {
        char norm[512];
        const uint8_t *data; long len;
        wad_normalise(filename, norm, sizeof(norm));
        if (wad_find(norm) && qol_asset(norm, &data, &len)) {
            a->mem = data; a->length = len;
            if (trace) fprintf(stderr, "asset: %s (replaced, %ld bytes)\n", norm, len);
            return (AAsset *)a;
        }
    }

    /* 1) absolute paths (the save) come from disk; relative ones from the OBB
     *    first, then from the data dir. */
    if (filename[0] == '/' && open_file(a, filename)) return (AAsset *)a;

    /* 2) the packed OBB / WAD. */
    if (g_wad_ent) {
        char norm[512];
        wad_normalise(filename, norm, sizeof(norm));
        const wad_entry_t *e = wad_find(norm);
        if (e && e->off + e->size <= g_wad_len) {
            /* PNGs: hand libpng a copy with colour/metadata chunks removed. */
            if (name_has_suffix(norm, ".png") && e->size > 8) {
                uint8_t *raw = malloc(e->size);
                if (raw) {
                    for (uint64_t j = 0; j < e->size; j++)
                        raw[j] = g_wad_map[e->off + j] ^ wad_key(e->off + j);
                    size_t olen = 0;
                    uint8_t *clean = png_strip_ancillary(raw, e->size, &olen);
                    free(raw);
                    if (clean) {
                        a->mem    = clean;
                        a->owned  = clean;
                        a->xor_on = 0;
                        a->length = (long)olen;
                        a->pos    = 0;
                        if (trace) fprintf(stderr, "wad: open %s (%ld bytes, png-stripped from %llu)\n",
                                           norm, a->length, (unsigned long long)e->size);
                        return (AAsset *)a;
                    }
                }
            }
            a->mem    = g_wad_map + e->off;
            a->base   = e->off;
            a->xor_on = 1;
            a->length = (long)e->size;
            a->pos    = 0;
            if (trace) fprintf(stderr, "wad: open %s (%ld bytes)\n", norm, a->length);
            return (AAsset *)a;
        }
    }

    if (filename[0] != '/' && open_file(a, filename)) return (AAsset *)a;
    /* the engine probes Android/, Console/ and Common/ for most files: only
     * absolute paths are worth a log line */
    if (trace || filename[0] == '/') fprintf(stderr, "AAssetManager_open: missing %s\n", filename);
    free(a);
    return NULL;
}

/* ── AAsset ─────────────────────────────────────────────────────────────── */

int AAsset_read(AAsset *asset, void *buf, int count) {
    if (!asset || count <= 0) return 0;
    aasset_t *a = (aasset_t *)asset;

    if (a->mem) {
        long remain = a->length - a->pos;
        if (remain <= 0) return 0;
        int n = (count < remain) ? count : (int)remain;
        uint8_t *dst = (uint8_t *)buf;
        if (a->xor_on) {
            for (int j = 0; j < n; j++) {
                uint64_t idx = a->base + (uint64_t)a->pos + j;
                dst[j] = a->mem[a->pos + j] ^ wad_key(idx);
            }
        } else {
            memcpy(dst, a->mem + a->pos, (size_t)n);
        }
        a->pos += n;
        return n;
    }

    if (!a->fp) return 0;
    size_t n = fread(buf, 1, (size_t)count, a->fp);
    a->pos += (long)n;
    return (int)n;
}

void AAsset_close(AAsset *asset) {
    if (!asset) return;
    aasset_t *a = (aasset_t *)asset;
    if (a->fp) fclose(a->fp);
    free(a->owned);
    free(a);
}

long AAsset_getLength(AAsset *asset) {
    if (!asset) return 0;
    return ((aasset_t *)asset)->length;
}

long AAsset_getRemainingLength(AAsset *asset) {
    if (!asset) return 0;
    aasset_t *a = (aasset_t *)asset;
    long r = a->length - a->pos;
    return (r < 0) ? 0 : r;
}

/* whence: SEEK_SET=0, SEEK_CUR=1, SEEK_END=2; returns the new position or -1. */
long AAsset_seek(AAsset *asset, long offset, int whence) {
    if (!asset) return -1;
    aasset_t *a = (aasset_t *)asset;

    long np;
    switch (whence) {
        case SEEK_SET: np = offset;            break;
        case SEEK_CUR: np = a->pos + offset;   break;
        case SEEK_END: np = a->length + offset; break;
        default: return -1;
    }
    if (np < 0) return -1;
    if (np > a->length) np = a->length;

    if (a->fp && fseek(a->fp, np, SEEK_SET) != 0) return -1;
    a->pos = np;
    return np;
}

void aasset_patch_init(void) {
    crc_init();
    wad_mount_auto();
}
