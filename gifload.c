/*
 * gifload.c - Minimal GIF87a/89a decoder to X11 Pixmap
 *
 * Decodes the first frame of a GIF image from a memory buffer.
 * Uses LZW decompression. Creates X11 Pixmap with color allocation
 * using the PizzaFool color-cache + nearest-match pattern for
 * graceful 8-bit PseudoColor display handling.
 *
 * No external dependencies (no libgif/libungif).
 */

#include "gifload.h"
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * COLOR CACHE (from PizzaFool pattern)
 * Handles 8-bit colormap exhaustion by finding nearest match.
 * All distance math uses 8-bit values to avoid 32-bit overflow.
 * ================================================================ */

#define MAX_CACHED_COLORS 256

static struct {
    unsigned short r, g, b;  /* 16-bit X11 color values */
    unsigned long pixel;
} color_cache[MAX_CACHED_COLORS];
static int n_cached = 0;
static XColor *cmap_cells = NULL;
static int cmap_ncells = 0;

void gif_init_colors(Display *dpy, int screen)
{
    int i;
    Colormap cmap = DefaultColormap(dpy, screen);
    cmap_ncells = DisplayCells(dpy, screen);
    cmap_cells = (XColor *)malloc(cmap_ncells * sizeof(XColor));
    for (i = 0; i < cmap_ncells; i++)
        cmap_cells[i].pixel = i;
    XQueryColors(dpy, cmap, cmap_cells, cmap_ncells);
    n_cached = 0;
}

static unsigned long alloc_color(Display *dpy, int screen,
                                 unsigned char r8, unsigned char g8,
                                 unsigned char b8)
{
    Colormap cmap = DefaultColormap(dpy, screen);
    XColor xc;
    unsigned short r16 = (unsigned short)r8 << 8 | r8;
    unsigned short g16 = (unsigned short)g8 << 8 | g8;
    unsigned short b16 = (unsigned short)b8 << 8 | b8;
    int i;

    /* Check cache for near-exact match */
    for (i = 0; i < n_cached; i++) {
        int dr = (int)r8 - (int)(color_cache[i].r >> 8);
        int dg = (int)g8 - (int)(color_cache[i].g >> 8);
        int db = (int)b8 - (int)(color_cache[i].b >> 8);
        int dist = dr*dr + dg*dg + db*db;
        if (dist <= 3) return color_cache[i].pixel;
    }

    /* Try allocating a new cell */
    xc.red = r16; xc.green = g16; xc.blue = b16;
    xc.flags = DoRed | DoGreen | DoBlue;
    if (XAllocColor(dpy, cmap, &xc)) {
        if (n_cached < MAX_CACHED_COLORS) {
            color_cache[n_cached].r = r16;
            color_cache[n_cached].g = g16;
            color_cache[n_cached].b = b16;
            color_cache[n_cached].pixel = xc.pixel;
            n_cached++;
        }
        return xc.pixel;
    }

    /* Allocation failed — find nearest in cache */
    {
        unsigned long best_pixel = BlackPixel(dpy, screen);
        int best_dist = 195076;

        for (i = 0; i < n_cached; i++) {
            int dr = (int)r8 - (int)(color_cache[i].r >> 8);
            int dg = (int)g8 - (int)(color_cache[i].g >> 8);
            int db = (int)b8 - (int)(color_cache[i].b >> 8);
            int dist = dr*dr + dg*dg + db*db;
            if (dist < best_dist) {
                best_dist = dist;
                best_pixel = color_cache[i].pixel;
            }
        }

        /* Also search full colormap */
        if (cmap_cells) {
            for (i = 0; i < cmap_ncells; i++) {
                int dr = (int)r8 - (int)(cmap_cells[i].red >> 8);
                int dg = (int)g8 - (int)(cmap_cells[i].green >> 8);
                int db = (int)b8 - (int)(cmap_cells[i].blue >> 8);
                int dist = dr*dr + dg*dg + db*db;
                if (dist < best_dist) {
                    best_dist = dist;
                    best_pixel = cmap_cells[i].pixel;
                }
            }
        }

        return best_pixel;
    }
}

/* ================================================================
 * GIF LZW DECOMPRESSOR
 * ================================================================ */

#define GIF_MAX_LZW_BITS 12
#define GIF_MAX_CODES    (1 << GIF_MAX_LZW_BITS)

typedef struct {
    const unsigned char *data;
    long  len;
    long  pos;
    int   bit_pos;
} bitstream_t;

/* Read next sub-block size byte + data from GIF block stream */
typedef struct {
    const unsigned char *data;
    long  len;
    long  pos;
    /* Sub-block state */
    int   block_remaining;
    int   bits_buf;
    int   bits_count;
} gif_stream_t;

static void gs_init(gif_stream_t *gs, const unsigned char *data, long len, long pos)
{
    gs->data = data;
    gs->len = len;
    gs->pos = pos;
    gs->block_remaining = 0;
    gs->bits_buf = 0;
    gs->bits_count = 0;
}

static int gs_read_byte(gif_stream_t *gs)
{
    while (gs->block_remaining == 0) {
        if (gs->pos >= gs->len) return -1;
        gs->block_remaining = gs->data[gs->pos++];
        if (gs->block_remaining == 0) return -1;  /* Block terminator */
    }
    if (gs->pos >= gs->len) return -1;
    gs->block_remaining--;
    return gs->data[gs->pos++];
}

static int gs_read_bits(gif_stream_t *gs, int nbits)
{
    int val;
    while (gs->bits_count < nbits) {
        int byte = gs_read_byte(gs);
        if (byte < 0) return -1;
        gs->bits_buf |= byte << gs->bits_count;
        gs->bits_count += 8;
    }
    val = gs->bits_buf & ((1 << nbits) - 1);
    gs->bits_buf >>= nbits;
    gs->bits_count -= nbits;
    return val;
}

typedef struct {
    int prefix;
    int suffix;
    int length;
} lzw_entry_t;

static int lzw_decode(gif_stream_t *gs, int min_code_size,
                      unsigned char *output, int max_pixels)
{
    lzw_entry_t table[GIF_MAX_CODES];
    int clear_code = 1 << min_code_size;
    int eoi_code = clear_code + 1;
    int next_code = eoi_code + 1;
    int code_size = min_code_size + 1;
    int old_code = -1;
    int out_pos = 0;
    int code, i;
    unsigned char stack[GIF_MAX_CODES];
    int stack_ptr;

    /* Initialize table */
    for (i = 0; i < clear_code; i++) {
        table[i].prefix = -1;
        table[i].suffix = i;
        table[i].length = 1;
    }

    for (;;) {
        code = gs_read_bits(gs, code_size);
        if (code < 0 || code == eoi_code) break;

        if (code == clear_code) {
            next_code = eoi_code + 1;
            code_size = min_code_size + 1;
            old_code = -1;
            continue;
        }

        /* Decode code to stack */
        stack_ptr = 0;
        if (code < next_code) {
            /* Code in table */
            int c = code;
            while (c >= 0 && stack_ptr < GIF_MAX_CODES) {
                stack[stack_ptr++] = table[c].suffix;
                c = table[c].prefix;
            }
        } else if (code == next_code && old_code >= 0) {
            /* Special case: code not yet in table */
            int c = old_code;
            while (c >= 0 && stack_ptr < GIF_MAX_CODES - 1) {
                stack[stack_ptr++] = table[c].suffix;
                c = table[c].prefix;
            }
            stack[stack_ptr++] = stack[stack_ptr - 1];
        } else {
            break;  /* Invalid code */
        }

        /* Output stack in reverse */
        for (i = stack_ptr - 1; i >= 0; i--) {
            if (out_pos >= max_pixels) goto done;
            output[out_pos++] = stack[i];
        }

        /* Add to table */
        if (old_code >= 0 && next_code < GIF_MAX_CODES) {
            table[next_code].prefix = old_code;
            table[next_code].suffix = stack[stack_ptr - 1]; /* First char of decoded string */
            table[next_code].length = table[old_code].length + 1;
            next_code++;
            if (next_code >= (1 << code_size) && code_size < GIF_MAX_LZW_BITS)
                code_size++;
        }

        old_code = code;
    }

done:
    return out_pos;
}

/* ================================================================
 * GIF DECODER (first frame only)
 * ================================================================ */

gif_image_t gif_decode(Display *dpy, int screen, Window win,
                       const unsigned char *data, long data_len)
{
    gif_image_t result;
    int width, height;
    int has_gct, gct_size, gct_entries;
    int bg_index;
    unsigned char gct[256][3];
    unsigned char *pixels = NULL;
    XImage *ximage = NULL;
    GC gc;
    long pos;
    int i, x, y;

    memset(&result, 0, sizeof(result));
    result.pixmap = None;

    /* Validate header: GIF87a or GIF89a */
    if (data_len < 13) return result;
    if (strncmp((const char *)data, "GIF87a", 6) != 0 &&
        strncmp((const char *)data, "GIF89a", 6) != 0)
        return result;

    /* Logical screen descriptor */
    width  = data[6] | (data[7] << 8);
    height = data[8] | (data[9] << 8);
    has_gct = (data[10] >> 7) & 1;
    gct_size = data[10] & 0x07;
    gct_entries = has_gct ? (1 << (gct_size + 1)) : 0;
    bg_index = data[11];

    if (width <= 0 || height <= 0 || width > 2048 || height > 2048)
        return result;

    pos = 13;

    /* Read Global Color Table */
    memset(gct, 0, sizeof(gct));
    if (has_gct) {
        if (pos + gct_entries * 3 > data_len) return result;
        for (i = 0; i < gct_entries; i++) {
            gct[i][0] = data[pos++];
            gct[i][1] = data[pos++];
            gct[i][2] = data[pos++];
        }
    }

    /* Scan for image descriptor, skip extensions */
    while (pos < data_len) {
        unsigned char block = data[pos++];

        if (block == 0x3B) {
            /* Trailer — end of GIF */
            break;
        } else if (block == 0x21) {
            /* Extension block — skip it */
            if (pos >= data_len) break;
            pos++;  /* extension type */
            /* Skip sub-blocks */
            while (pos < data_len) {
                int bsz = data[pos++];
                if (bsz == 0) break;
                pos += bsz;
            }
        } else if (block == 0x2C) {
            /* Image descriptor */
            int img_x, img_y, img_w, img_h;
            int has_lct, lct_size, lct_entries;
            int interlaced;
            unsigned char lct[256][3];
            unsigned char (*palette)[3];
            int min_code_size;
            gif_stream_t gs;
            int n_decoded;

            if (pos + 9 > data_len) break;
            img_x = data[pos] | (data[pos+1] << 8);
            img_y = data[pos+2] | (data[pos+3] << 8);
            img_w = data[pos+4] | (data[pos+5] << 8);
            img_h = data[pos+6] | (data[pos+7] << 8);
            has_lct = (data[pos+8] >> 7) & 1;
            interlaced = (data[pos+8] >> 6) & 1;
            lct_size = data[pos+8] & 0x07;
            lct_entries = has_lct ? (1 << (lct_size + 1)) : 0;
            pos += 9;

            (void)img_x; (void)img_y; (void)interlaced;

            /* Read Local Color Table if present */
            memset(lct, 0, sizeof(lct));
            if (has_lct) {
                if (pos + lct_entries * 3 > data_len) break;
                for (i = 0; i < lct_entries; i++) {
                    lct[i][0] = data[pos++];
                    lct[i][1] = data[pos++];
                    lct[i][2] = data[pos++];
                }
                palette = lct;
            } else {
                palette = gct;
            }

            /* LZW minimum code size */
            if (pos >= data_len) break;
            min_code_size = data[pos++];
            if (min_code_size < 2 || min_code_size > 11) break;

            /* Decode LZW data */
            pixels = (unsigned char *)malloc(img_w * img_h);
            if (!pixels) break;
            memset(pixels, bg_index, img_w * img_h);

            gs_init(&gs, data, data_len, pos);
            n_decoded = lzw_decode(&gs, min_code_size, pixels, img_w * img_h);

            if (n_decoded <= 0) {
                free(pixels);
                pixels = NULL;
                break;
            }

            /* Build XImage from decoded pixels */
            {
                int depth = DefaultDepth(dpy, screen);
                Visual *vis = DefaultVisual(dpy, screen);
                int bpp;
                char *imgdata;

                /* Determine bytes per pixel from depth */
                if (depth <= 8) bpp = 1;
                else if (depth <= 16) bpp = 2;
                else bpp = 4;

                imgdata = (char *)malloc(img_w * img_h * bpp);
                if (!imgdata) { free(pixels); break; }

                ximage = XCreateImage(dpy, vis, depth, ZPixmap, 0,
                                      imgdata, img_w, img_h, bpp * 8,
                                      img_w * bpp);
                if (!ximage) {
                    free(imgdata);
                    free(pixels);
                    break;
                }

                /* Map each pixel through palette → alloc_color */
                for (y = 0; y < img_h; y++) {
                    for (x = 0; x < img_w; x++) {
                        int idx = pixels[y * img_w + x];
                        unsigned long px = alloc_color(dpy, screen,
                                                       palette[idx][0],
                                                       palette[idx][1],
                                                       palette[idx][2]);
                        XPutPixel(ximage, x, y, px);
                    }
                }
            }

            free(pixels);

            /* Create Pixmap from XImage */
            result.width = img_w;
            result.height = img_h;
            result.pixmap = XCreatePixmap(dpy, win, img_w, img_h,
                                           DefaultDepth(dpy, screen));
            gc = XCreateGC(dpy, result.pixmap, 0, NULL);
            XPutImage(dpy, result.pixmap, gc, ximage, 0, 0, 0, 0,
                      img_w, img_h);
            XFreeGC(dpy, gc);
            XDestroyImage(ximage);  /* Also frees imgdata */
            result.loaded = 1;
            break;  /* Only decode first frame */
        }
    }

    return result;
}
