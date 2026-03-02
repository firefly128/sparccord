/*
 * gifload.h - Minimal GIF decoder to X11 Pixmap for Solaris 7
 *
 * Decodes GIF87a/89a images from a memory buffer and creates
 * an X11 Pixmap using the PizzaFool color-cache pattern for
 * graceful handling of 8-bit PseudoColor displays.
 */

#ifndef SPARCCORD_GIFLOAD_H
#define SPARCCORD_GIFLOAD_H

#include <X11/Xlib.h>

typedef struct {
    Pixmap  pixmap;    /* X11 pixmap (None if load failed) */
    int     width;
    int     height;
    int     loaded;    /* 1 if successfully loaded */
} gif_image_t;

/*
 * Initialize the color cache. Call once at startup after opening the display.
 */
void gif_init_colors(Display *dpy, int screen);

/*
 * Decode a GIF image from a memory buffer into an X11 Pixmap.
 * Returns a gif_image_t. Caller should free the pixmap with XFreePixmap().
 */
gif_image_t gif_decode(Display *dpy, int screen, Window win,
                       const unsigned char *data, long data_len);

#endif /* SPARCCORD_GIFLOAD_H */
