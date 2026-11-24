#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GFX_FONT_3X5 = 0,
    GFX_FONT_4X6,
    GFX_FONT_5X7,
} gfx_font_id_t;

typedef enum {
    GFX_ALIGN_LEFT = 0,
    GFX_ALIGN_CENTER,
    GFX_ALIGN_RIGHT,
} gfx_align_t;

/**
 * Measure rendered pixel width of text in the specified bitmap font (1px spacing).
 */
int gfx_measure_text(const char *text, gfx_font_id_t font);

/**
 * Draw crisp non-antialiased bitmap text onto a 64x32 RGB888 buffer,
 * matching Clean Slate server graphics.py fonts (3x5, 4x6, 5x7) and alignment.
 */
void gfx_draw_text_aligned(uint8_t *fb_rgb888, int width, int height,
                           int x, int y, const char *text,
                           gfx_font_id_t font, gfx_align_t align,
                           uint8_t r, uint8_t g, uint8_t b);

/**
 * Draw crisp bitmap text clipped vertically to [clip_y0, clip_y1] (inclusive).
 */
void gfx_draw_text_clipped_y(uint8_t *fb_rgb888, int width, int height,
                             int x, int y, const char *text,
                             gfx_font_id_t font, gfx_align_t align,
                             uint8_t r, uint8_t g, uint8_t b,
                             int clip_y0, int clip_y1);

/**
 * Draw vertically sub-pixel blended bitmap text (y_subpx in quarter-pixels, 1px = 4)
 * clipped to [clip_y0, clip_y1] for butter-smooth vertical scrolling on 8-bit HUB75.
 */
void gfx_draw_text_subpixel_y(uint8_t *fb_rgb888, int width, int height,
                              int x, int y_subpx, const char *text,
                              gfx_font_id_t font, gfx_align_t align,
                              uint8_t r, uint8_t g, uint8_t b,
                              int clip_y0, int clip_y1);

/**
 * Convenience wrapper for left-aligned 5x7 text (used by local status banners).
 */
void gfx_draw_text(uint8_t *fb_rgb888, int width, int height,
                   int x, int y, const char *text, uint8_t r, uint8_t g, uint8_t b);

/**
 * Fill a solid rectangle in an RGB888 framebuffer.
 */
void gfx_fill_rect(uint8_t *fb_rgb888, int width, int height,
                   int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b);

/**
 * Convert big-endian RGB565 buffer (Clean Slate API format) to RGB888.
 */
void gfx_rgb565be_to_rgb888(const uint8_t *src_be, size_t pixel_count, uint8_t *dst_rgb888);

/**
 * Unpack 16-bit RGB565 value to 8-bit R, G, B channels.
 */
void gfx_rgb565_to_rgb888(uint16_t color565, uint8_t *r, uint8_t *g, uint8_t *b);

#ifdef __cplusplus
}
#endif
