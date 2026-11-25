#include "gfx_font.h"

#include <ctype.h>
#include <string.h>

typedef struct {
    char ch;
    uint8_t rows[5]; // top 3 bits used (bit 2 = left, bit 0 = right)
} glyph_3x5_t;

typedef struct {
    char ch;
    uint8_t rows[6]; // top 4 bits used (bit 3 = left, bit 0 = right)
} glyph_4x6_t;

typedef struct {
    char ch;
    uint8_t rows[7]; // top 5 bits used (bit 4 = left, bit 0 = right)
} glyph_5x7_t;

// Exact match to Clean Slate graphics.py FONT_3X5
static const glyph_3x5_t s_font_3x5[] = {
    {'A', {0b010, 0b101, 0b111, 0b101, 0b101}},
    {'B', {0b110, 0b101, 0b110, 0b101, 0b110}},
    {'C', {0b011, 0b100, 0b100, 0b100, 0b011}},
    {'D', {0b110, 0b101, 0b101, 0b101, 0b110}},
    {'E', {0b111, 0b100, 0b110, 0b100, 0b111}},
    {'F', {0b111, 0b100, 0b110, 0b100, 0b100}},
    {'G', {0b011, 0b100, 0b101, 0b101, 0b011}},
    {'H', {0b101, 0b101, 0b111, 0b101, 0b101}},
    {'I', {0b111, 0b010, 0b010, 0b010, 0b111}},
    {'J', {0b001, 0b001, 0b001, 0b101, 0b010}},
    {'K', {0b101, 0b101, 0b110, 0b101, 0b101}},
    {'L', {0b100, 0b100, 0b100, 0b100, 0b111}},
    {'M', {0b101, 0b111, 0b111, 0b101, 0b101}},
    {'N', {0b111, 0b101, 0b101, 0b101, 0b101}},
    {'O', {0b010, 0b101, 0b101, 0b101, 0b010}},
    {'P', {0b110, 0b101, 0b110, 0b100, 0b100}},
    {'Q', {0b010, 0b101, 0b101, 0b111, 0b011}},
    {'R', {0b110, 0b101, 0b110, 0b101, 0b101}},
    {'S', {0b011, 0b100, 0b010, 0b001, 0b110}},
    {'T', {0b111, 0b010, 0b010, 0b010, 0b010}},
    {'U', {0b101, 0b101, 0b101, 0b101, 0b111}},
    {'V', {0b101, 0b101, 0b101, 0b101, 0b010}},
    {'W', {0b101, 0b101, 0b111, 0b111, 0b101}},
    {'X', {0b101, 0b101, 0b010, 0b101, 0b101}},
    {'Y', {0b101, 0b101, 0b010, 0b010, 0b010}},
    {'Z', {0b111, 0b001, 0b010, 0b100, 0b111}},
    {'0', {0b111, 0b101, 0b101, 0b101, 0b111}},
    {'1', {0b010, 0b110, 0b010, 0b010, 0b111}},
    {'2', {0b111, 0b001, 0b111, 0b100, 0b111}},
    {'3', {0b111, 0b001, 0b111, 0b001, 0b111}},
    {'4', {0b101, 0b101, 0b111, 0b001, 0b001}},
    {'5', {0b111, 0b100, 0b111, 0b001, 0b111}},
    {'6', {0b111, 0b100, 0b111, 0b101, 0b111}},
    {'7', {0b111, 0b001, 0b001, 0b010, 0b010}},
    {'8', {0b111, 0b101, 0b111, 0b101, 0b111}},
    {'9', {0b111, 0b101, 0b111, 0b001, 0b111}},
    {':', {0b000, 0b010, 0b000, 0b010, 0b000}},
    {'-', {0b000, 0b000, 0b111, 0b000, 0b000}},
    {'+', {0b000, 0b010, 0b111, 0b010, 0b000}},
    {'.', {0b000, 0b000, 0b000, 0b000, 0b010}},
    {',', {0b000, 0b000, 0b000, 0b010, 0b100}},
    {'!', {0b010, 0b010, 0b010, 0b000, 0b010}},
    {'?', {0b110, 0b001, 0b010, 0b000, 0b010}},
    {';', {0b000, 0b010, 0b000, 0b010, 0b100}},
    {'"', {0b101, 0b101, 0b000, 0b000, 0b000}},
    {'(', {0b010, 0b100, 0b100, 0b100, 0b010}},
    {')', {0b010, 0b001, 0b001, 0b001, 0b010}},
    {'/', {0b001, 0b001, 0b010, 0b100, 0b100}},
    {'&', {0b010, 0b101, 0b010, 0b101, 0b011}},
    {'@', {0b111, 0b101, 0b111, 0b100, 0b011}},
    {'#', {0b101, 0b111, 0b101, 0b111, 0b101}},
    {'%', {0b101, 0b001, 0b010, 0b100, 0b101}},
    {'\'', {0b010, 0b010, 0b000, 0b000, 0b000}},
    {'|', {0b010, 0b010, 0b010, 0b010, 0b010}},
    {' ', {0b000, 0b000, 0b000, 0b000, 0b000}},
};

// Exact match to Clean Slate graphics.py FONT_4X6
static const glyph_4x6_t s_font_4x6[] = {
    {'0', {0b0110, 0b1001, 0b1001, 0b1001, 0b1001, 0b0110}},
    {'1', {0b0010, 0b0110, 0b0010, 0b0010, 0b0010, 0b0111}},
    {'2', {0b0110, 0b1001, 0b0010, 0b0100, 0b1000, 0b1111}},
    {'3', {0b1110, 0b0001, 0b0110, 0b0001, 0b0001, 0b1110}},
    {'4', {0b1001, 0b1001, 0b1111, 0b0001, 0b0001, 0b0001}},
    {'5', {0b1111, 0b1000, 0b1110, 0b0001, 0b0001, 0b1110}},
    {'6', {0b0111, 0b1000, 0b1110, 0b1001, 0b1001, 0b0110}},
    {'7', {0b1111, 0b0001, 0b0010, 0b0100, 0b0100, 0b0100}},
    {'8', {0b0110, 0b1001, 0b0110, 0b1001, 0b1001, 0b0110}},
    {'9', {0b0110, 0b1001, 0b0111, 0b0001, 0b0001, 0b1110}},
    {'A', {0b0110, 0b1001, 0b1111, 0b1001, 0b1001, 0b1001}},
    {'B', {0b1110, 0b1001, 0b1110, 0b1001, 0b1001, 0b1110}},
    {'C', {0b0111, 0b1000, 0b1000, 0b1000, 0b1000, 0b0111}},
    {'D', {0b1110, 0b1001, 0b1001, 0b1001, 0b1001, 0b1110}},
    {'E', {0b1111, 0b1000, 0b1110, 0b1000, 0b1000, 0b1111}},
    {'F', {0b1111, 0b1000, 0b1110, 0b1000, 0b1000, 0b1000}},
    {'G', {0b0111, 0b1000, 0b1011, 0b1001, 0b1001, 0b0111}},
    {'H', {0b1001, 0b1001, 0b1111, 0b1001, 0b1001, 0b1001}},
    {'I', {0b0111, 0b0010, 0b0010, 0b0010, 0b0010, 0b0111}},
    {'J', {0b0001, 0b0001, 0b0001, 0b1001, 0b1001, 0b0110}},
    {'K', {0b1001, 0b1010, 0b1100, 0b1010, 0b1001, 0b1001}},
    {'L', {0b1000, 0b1000, 0b1000, 0b1000, 0b1000, 0b1111}},
    {'M', {0b1001, 0b1111, 0b1111, 0b1001, 0b1001, 0b1001}},
    {'N', {0b1001, 0b1101, 0b1011, 0b1001, 0b1001, 0b1001}},
    {'O', {0b0110, 0b1001, 0b1001, 0b1001, 0b1001, 0b0110}},
    {'P', {0b1110, 0b1001, 0b1110, 0b1000, 0b1000, 0b1000}},
    {'Q', {0b0110, 0b1001, 0b1001, 0b1011, 0b1001, 0b0111}},
    {'R', {0b1110, 0b1001, 0b1110, 0b1010, 0b1001, 0b1001}},
    {'S', {0b0111, 0b1000, 0b0110, 0b0001, 0b0001, 0b1110}},
    {'T', {0b1111, 0b0100, 0b0100, 0b0100, 0b0100, 0b0100}},
    {'U', {0b1001, 0b1001, 0b1001, 0b1001, 0b1001, 0b0110}},
    {'V', {0b1001, 0b1001, 0b1001, 0b1001, 0b0110, 0b0110}},
    {'W', {0b1001, 0b1001, 0b1001, 0b1111, 0b1111, 0b1001}},
    {'X', {0b1001, 0b1001, 0b0110, 0b0110, 0b1001, 0b1001}},
    {'Y', {0b1001, 0b1001, 0b0110, 0b0010, 0b0010, 0b0010}},
    {'Z', {0b1111, 0b0001, 0b0010, 0b0100, 0b1000, 0b1111}},
    {':', {0b0000, 0b0100, 0b0000, 0b0000, 0b0100, 0b0000}},
    {'-', {0b0000, 0b0000, 0b1111, 0b0000, 0b0000, 0b0000}},
    {'+', {0b0000, 0b0100, 0b1110, 0b0100, 0b0000, 0b0000}},
    {'.', {0b0000, 0b0000, 0b0000, 0b0000, 0b0100, 0b0100}},
    {',', {0b0000, 0b0000, 0b0000, 0b0000, 0b0100, 0b1000}},
    {'!', {0b0100, 0b0100, 0b0100, 0b0100, 0b0000, 0b0100}},
    {'?', {0b0110, 0b1001, 0b0010, 0b0100, 0b0000, 0b0100}},
    {';', {0b0000, 0b0100, 0b0000, 0b0000, 0b0100, 0b1000}},
    {'\'', {0b0100, 0b0100, 0b0000, 0b0000, 0b0000, 0b0000}},
    {'"', {0b1010, 0b1010, 0b0000, 0b0000, 0b0000, 0b0000}},
    {'(', {0b0010, 0b0100, 0b0100, 0b0100, 0b0100, 0b0010}},
    {')', {0b0100, 0b0010, 0b0010, 0b0010, 0b0010, 0b0100}},
    {'/', {0b0001, 0b0010, 0b0010, 0b0100, 0b0100, 0b1000}},
    {'#', {0b0101, 0b1111, 0b0101, 0b0101, 0b1111, 0b0101}},
    {'|', {0b0100, 0b0100, 0b0100, 0b0100, 0b0100, 0b0100}},
    {' ', {0b0000, 0b0000, 0b0000, 0b0000, 0b0000, 0b0000}},
};

// Exact match to Clean Slate graphics.py FONT_5X7
static const glyph_5x7_t s_font_5x7[] = {
    {'0', {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110}},
    {'1', {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'2', {0b01110, 0b10001, 0b00001, 0b00110, 0b01000, 0b10000, 0b11111}},
    {'3', {0b01110, 0b10001, 0b00001, 0b00110, 0b00001, 0b10001, 0b01110}},
    {'4', {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}},
    {'5', {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}},
    {'6', {0b01110, 0b10000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}},
    {'7', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}},
    {'8', {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}},
    {'9', {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00001, 0b01110}},
    {'A', {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'B', {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110}},
    {'C', {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110}},
    {'D', {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110}},
    {'E', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111}},
    {'F', {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'G', {0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111}},
    {'H', {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}},
    {'I', {0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}},
    {'J', {0b00111, 0b00010, 0b00010, 0b00010, 0b10010, 0b10010, 0b01100}},
    {'K', {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001}},
    {'L', {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111}},
    {'M', {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001}},
    {'N', {0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b10001}},
    {'O', {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'P', {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000}},
    {'Q', {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101}},
    {'R', {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001}},
    {'S', {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110}},
    {'T', {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'U', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
    {'V', {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100}},
    {'W', {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b11011, 0b10001}},
    {'X', {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001}},
    {'Y', {0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100}},
    {'Z', {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111}},
    {':', {0b00000, 0b00100, 0b00100, 0b00000, 0b00100, 0b00100, 0b00000}},
    {'-', {0b00000, 0b00000, 0b00000, 0b11111, 0b00000, 0b00000, 0b00000}},
    {'+', {0b00000, 0b00100, 0b00100, 0b11111, 0b00100, 0b00100, 0b00000}},
    {'.', {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00100, 0b00100}},
    {',', {0b00000, 0b00000, 0b00000, 0b00000, 0b00100, 0b00100, 0b01000}},
    {'!', {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00000, 0b00100}},
    {'?', {0b01110, 0b10001, 0b00010, 0b00100, 0b00100, 0b00000, 0b00100}},
    {';', {0b00000, 0b00100, 0b00100, 0b00000, 0b00100, 0b00100, 0b01000}},
    {'"', {0b01010, 0b01010, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000}},
    {'(', {0b00010, 0b00100, 0b01000, 0b01000, 0b01000, 0b00100, 0b00010}},
    {')', {0b01000, 0b00100, 0b00010, 0b00010, 0b00010, 0b00100, 0b01000}},
    {'/', {0b00001, 0b00010, 0b00010, 0b00100, 0b01000, 0b01000, 0b10000}},
    {'&', {0b01100, 0b10010, 0b10100, 0b01000, 0b10101, 0b10010, 0b01101}},
    {'\'', {0b00100, 0b00100, 0b01000, 0b00000, 0b00000, 0b00000, 0b00000}},
    {'#', {0b01010, 0b01010, 0b11111, 0b01010, 0b11111, 0b01010, 0b01010}},
    {'|', {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}},
    {' ', {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000}},
};

static inline void set_pixel(uint8_t *fb, int width, int height,
                             int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return;
    }
    size_t idx = ((size_t)y * (size_t)width + (size_t)x) * 3;
    fb[idx] = r;
    fb[idx + 1] = g;
    fb[idx + 2] = b;
}

static const uint8_t *find_glyph_rows(char c, gfx_font_id_t font, int *out_w, int *out_h)
{
    char up = (char)toupper((unsigned char)c);
    if (font == GFX_FONT_3X5) {
        *out_w = 3;
        *out_h = 5;
        for (size_t i = 0; i < sizeof(s_font_3x5) / sizeof(s_font_3x5[0]); i++) {
            if (s_font_3x5[i].ch == up) return s_font_3x5[i].rows;
        }
        return s_font_3x5[sizeof(s_font_3x5) / sizeof(s_font_3x5[0]) - 1].rows;
    }
    if (font == GFX_FONT_4X6) {
        *out_w = 4;
        *out_h = 6;
        for (size_t i = 0; i < sizeof(s_font_4x6) / sizeof(s_font_4x6[0]); i++) {
            if (s_font_4x6[i].ch == up) return s_font_4x6[i].rows;
        }
        return s_font_4x6[sizeof(s_font_4x6) / sizeof(s_font_4x6[0]) - 1].rows;
    }
    *out_w = 5;
    *out_h = 7;
    for (size_t i = 0; i < sizeof(s_font_5x7) / sizeof(s_font_5x7[0]); i++) {
        if (s_font_5x7[i].ch == up) return s_font_5x7[i].rows;
    }
    return s_font_5x7[sizeof(s_font_5x7) / sizeof(s_font_5x7[0]) - 1].rows;
}

int gfx_measure_text(const char *text, gfx_font_id_t font)
{
    if (!text || !*text) {
        return 0;
    }
    int char_w = (font == GFX_FONT_3X5) ? 3 : ((font == GFX_FONT_4X6) ? 4 : 5);
    int len = (int)strlen(text);
    return len * char_w + (len - 1);
}

void gfx_draw_text_clipped_y(uint8_t *fb_rgb888, int width, int height,
                             int x, int y, const char *text,
                             gfx_font_id_t font, gfx_align_t align,
                             uint8_t r, uint8_t g, uint8_t b,
                             int clip_y0, int clip_y1)
{
    if (!fb_rgb888 || !text || !*text) {
        return;
    }
    if (clip_y0 < 0) {
        clip_y0 = 0;
    }
    if (clip_y1 >= height) {
        clip_y1 = height - 1;
    }
    if (clip_y0 > clip_y1) {
        return;
    }
    int total_w = gfx_measure_text(text, font);
    int cursor_x = x;
    if (align == GFX_ALIGN_CENTER) {
        cursor_x = x - (total_w / 2);
    } else if (align == GFX_ALIGN_RIGHT) {
        cursor_x = x - total_w;
    }

    for (const char *p = text; *p; p++) {
        int cw = 0, ch = 0;
        const uint8_t *rows = find_glyph_rows(*p, font, &cw, &ch);
        if (cursor_x >= width) {
            break;
        }
        if (cursor_x + cw > 0) {
            for (int row = 0; row < ch; row++) {
                int py = y + row;
                if (py < clip_y0 || py > clip_y1) {
                    continue;
                }
                uint8_t bits = rows[row];
                for (int col = 0; col < cw; col++) {
                    if (bits & (1U << (cw - 1 - col))) {
                        set_pixel(fb_rgb888, width, height, cursor_x + col, py, r, g, b);
                    }
                }
            }
        }
        cursor_x += cw + 1;
    }
}

void gfx_draw_text_subpixel_y(uint8_t *fb_rgb888, int width, int height,
                              int x, int y_subpx, const char *text,
                              gfx_font_id_t font, gfx_align_t align,
                              uint8_t r, uint8_t g, uint8_t b,
                              int clip_y0, int clip_y1)
{
    if (!fb_rgb888 || !text || !*text) {
        return;
    }
    if (clip_y0 < 0) {
        clip_y0 = 0;
    }
    if (clip_y1 >= height) {
        clip_y1 = height - 1;
    }
    if (clip_y0 > clip_y1) {
        return;
    }
    int whole_y = (y_subpx >= 0) ? (y_subpx / 4) : -(((-y_subpx) + 3) / 4);
    int frac = y_subpx - whole_y * 4; /* 0..3 */
    if (frac == 0) {
        gfx_draw_text_clipped_y(fb_rgb888, width, height, x, whole_y, text,
                                font, align, r, g, b, clip_y0, clip_y1);
        return;
    }

    /* Gamma-1.5-compensated blend weights for 0/4, 1/4, 2/4, 3/4, 4/4 coverage
     * so total emitted LED brightness stays constant as strokes glide across rows. */
    static const uint8_t k_alpha[5] = { 0, 101, 161, 211, 255 };

    int total_w = gfx_measure_text(text, font);
    int cursor_x = x;
    if (align == GFX_ALIGN_CENTER) {
        cursor_x = x - (total_w / 2);
    } else if (align == GFX_ALIGN_RIGHT) {
        cursor_x = x - total_w;
    }

    for (const char *p = text; *p; p++) {
        int cw = 0, ch = 0;
        const uint8_t *rows = find_glyph_rows(*p, font, &cw, &ch);
        if (cursor_x >= width) {
            break;
        }
        if (cursor_x + cw > 0) {
            for (int row = 0; row <= ch; row++) {
                int py = whole_y + row;
                if (py < clip_y0 || py > clip_y1) {
                    continue;
                }
                uint8_t cur_bits = (row < ch) ? rows[row] : 0U;
                uint8_t prev_bits = (row > 0) ? rows[row - 1] : 0U;
                for (int col = 0; col < cw; col++) {
                    int px = cursor_x + col;
                    if (px < 0 || px >= width) {
                        continue;
                    }
                    uint8_t mask = (uint8_t)(1U << (cw - 1 - col));
                    uint8_t cur_on = (cur_bits & mask) ? 1U : 0U;
                    uint8_t prev_on = (prev_bits & mask) ? 1U : 0U;
                    uint8_t cov = (uint8_t)(cur_on * (4 - frac) + prev_on * frac);
                    if (cov == 0U) {
                        continue;
                    }
                    if (cov >= 4U) {
                        set_pixel(fb_rgb888, width, height, px, py, r, g, b);
                    } else {
                        uint8_t a = k_alpha[cov];
                        uint8_t inv = (uint8_t)(255U - a);
                        size_t idx = ((size_t)py * (size_t)width + (size_t)px) * 3U;
                        fb_rgb888[idx + 0] = (uint8_t)(((uint32_t)fb_rgb888[idx + 0] * inv + (uint32_t)r * a + 127U) / 255U);
                        fb_rgb888[idx + 1] = (uint8_t)(((uint32_t)fb_rgb888[idx + 1] * inv + (uint32_t)g * a + 127U) / 255U);
                        fb_rgb888[idx + 2] = (uint8_t)(((uint32_t)fb_rgb888[idx + 2] * inv + (uint32_t)b * a + 127U) / 255U);
                    }
                }
            }
        }
        cursor_x += cw + 1;
    }
}

void gfx_draw_text_aligned(uint8_t *fb_rgb888, int width, int height,
                           int x, int y, const char *text,
                           gfx_font_id_t font, gfx_align_t align,
                           uint8_t r, uint8_t g, uint8_t b)
{
    gfx_draw_text_clipped_y(fb_rgb888, width, height, x, y, text, font, align,
                            r, g, b, 0, height - 1);
}

void gfx_draw_text(uint8_t *fb_rgb888, int width, int height,
                   int x, int y, const char *text, uint8_t r, uint8_t g, uint8_t b)
{
    gfx_draw_text_aligned(fb_rgb888, width, height, x, y, text,
                          GFX_FONT_5X7, GFX_ALIGN_LEFT, r, g, b);
}

void gfx_fill_rect(uint8_t *fb_rgb888, int width, int height,
                   int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    if (!fb_rgb888) {
        return;
    }
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            set_pixel(fb_rgb888, width, height, x + dx, y + dy, r, g, b);
        }
    }
}

void gfx_rgb565_to_rgb888(uint16_t color565, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t r5 = (uint8_t)((color565 >> 11) & 0x1F);
    uint8_t g6 = (uint8_t)((color565 >> 5) & 0x3F);
    uint8_t b5 = (uint8_t)(color565 & 0x1F);
    *r = (uint8_t)((r5 << 3) | (r5 >> 2));
    *g = (uint8_t)((g6 << 2) | (g6 >> 4));
    *b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

void gfx_rgb565be_to_rgb888(const uint8_t *src_be, size_t pixel_count, uint8_t *dst_rgb888)
{
    for (size_t i = 0; i < pixel_count; i++) {
        uint16_t val = ((uint16_t)src_be[i * 2] << 8) | (uint16_t)src_be[i * 2 + 1];
        gfx_rgb565_to_rgb888(val,
                             &dst_rgb888[i * 3 + 0],
                             &dst_rgb888[i * 3 + 1],
                             &dst_rgb888[i * 3 + 2]);
    }
}
