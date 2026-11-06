/* SoftWin 1-bit graphics: every pixel written here is black or white. */
#include "softwin.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INK_PX 0x00FFFFFFu
#define PAPER_PX 0x00000000u

static inline uint32_t col(SwColor c) { return c == SW_INK ? INK_PX : PAPER_PX; }

/* 4x4 Bayer matrix. A pixel is ink when shade > threshold. */
static const uint8_t BAYER[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

static inline uint32_t shade_px(const SwCanvas *c, int x, int y, int shade) {
    int p = c->pat > 0 ? c->pat : 1;
    return shade > BAYER[(y / p) & 3][(x / p) & 3] ? INK_PX : PAPER_PX;
}

void sw_canvas(SwCanvas *c, uint32_t *px, int w, int h) {
    c->px = px; c->w = w; c->h = h;
    if (c->pat < 1) c->pat = 1;
    sw_noclip(c);
}

void sw_noclip(SwCanvas *c) { c->cx0 = 0; c->cy0 = 0; c->cx1 = c->w; c->cy1 = c->h; }

void sw_clip(SwCanvas *c, int x, int y, int w, int h) {
    c->cx0 = x < 0 ? 0 : x;
    c->cy0 = y < 0 ? 0 : y;
    c->cx1 = x + w > c->w ? c->w : x + w;
    c->cy1 = y + h > c->h ? c->h : y + h;
    if (c->cx1 < c->cx0) c->cx1 = c->cx0;
    if (c->cy1 < c->cy0) c->cy1 = c->cy0;
}

void sw_clear(SwCanvas *c, SwColor color) {
    uint32_t v = col(color);
    int n = c->w * c->h;
    for (int i = 0; i < n; i++) c->px[i] = v;
}

void sw_pixel(SwCanvas *c, int x, int y, SwColor color) {
    if (x < c->cx0 || y < c->cy0 || x >= c->cx1 || y >= c->cy1) return;
    c->px[y * c->w + x] = col(color);
}

void sw_fill(SwCanvas *c, int x, int y, int w, int h, SwColor color) {
    int x0 = x < c->cx0 ? c->cx0 : x, y0 = y < c->cy0 ? c->cy0 : y;
    int x1 = x + w > c->cx1 ? c->cx1 : x + w, y1 = y + h > c->cy1 ? c->cy1 : y + h;
    uint32_t v = col(color);
    for (int j = y0; j < y1; j++) {
        uint32_t *row = c->px + j * c->w;
        for (int i = x0; i < x1; i++) row[i] = v;
    }
}

void sw_fill_shade(SwCanvas *c, int x, int y, int w, int h, int shade) {
    int x0 = x < c->cx0 ? c->cx0 : x, y0 = y < c->cy0 ? c->cy0 : y;
    int x1 = x + w > c->cx1 ? c->cx1 : x + w, y1 = y + h > c->cy1 ? c->cy1 : y + h;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = c->px + j * c->w;
        for (int i = x0; i < x1; i++) row[i] = shade_px(c, i, j, shade);
    }
}

void sw_stipple(SwCanvas *c, int x, int y, int w, int h, int shade) {
    int x0 = x < c->cx0 ? c->cx0 : x, y0 = y < c->cy0 ? c->cy0 : y;
    int x1 = x + w > c->cx1 ? c->cx1 : x + w, y1 = y + h > c->cy1 ? c->cy1 : y + h;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = c->px + j * c->w;
        for (int i = x0; i < x1; i++)
            if (shade_px(c, i, j, shade) == INK_PX) row[i] = INK_PX;
    }
}

void sw_frame(SwCanvas *c, int x, int y, int w, int h, int th, SwColor color) {
    sw_fill(c, x, y, w, th, color);
    sw_fill(c, x, y + h - th, w, th, color);
    sw_fill(c, x, y + th, th, h - 2 * th, color);
    sw_fill(c, x + w - th, y + th, th, h - 2 * th, color);
}

/* Clips to the clip rect with Cohen-Sutherland style rejection, so a wild
 * coordinate cannot make Bresenham walk millions of pixels. */
static bool clip_segment(const SwCanvas *c, int pad, float *x0, float *y0, float *x1, float *y1) {
    float xmin = c->cx0 - pad, ymin = c->cy0 - pad, xmax = c->cx1 + pad, ymax = c->cy1 + pad;
    float t0 = 0, t1 = 1, dx = *x1 - *x0, dy = *y1 - *y0;
    float p[4] = { -dx, dx, -dy, dy };
    float q[4] = { *x0 - xmin, xmax - *x0, *y0 - ymin, ymax - *y0 };
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0) {
            if (q[i] < 0) return false;
        } else {
            float r = q[i] / p[i];
            if (p[i] < 0) { if (r > t1) return false; if (r > t0) t0 = r; }
            else { if (r < t0) return false; if (r < t1) t1 = r; }
        }
    }
    float ox = *x0, oy = *y0;
    *x0 = ox + dx * t0; *y0 = oy + dy * t0;
    *x1 = ox + dx * t1; *y1 = oy + dy * t1;
    return true;
}

static void line_core(SwCanvas *c, int x0, int y0, int x1, int y1, int th, int on, int off, SwColor color) {
    float fx0 = x0, fy0 = y0, fx1 = x1, fy1 = y1;
    if (!clip_segment(c, th + 1, &fx0, &fy0, &fx1, &fy1)) return;
    x0 = (int)fx0; y0 = (int)fy0; x1 = (int)fx1; y1 = (int)fy1;
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, count = 0, period = on + off;
    int lo = -(th - 1) / 2;
    for (;;) {
        if (!period || count % period < on) {
            if (th <= 1) sw_pixel(c, x0, y0, color);
            else sw_fill(c, x0 + lo, y0 + lo, th, th, color);
        }
        count++;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void sw_line(SwCanvas *c, int x0, int y0, int x1, int y1, int th, SwColor color) {
    line_core(c, x0, y0, x1, y1, th, 0, 0, color);
}

void sw_dash(SwCanvas *c, int x0, int y0, int x1, int y1, int th, int on, int off, SwColor color) {
    line_core(c, x0, y0, x1, y1, th, on, off, color);
}

void sw_tri(SwCanvas *c, float x0, float y0, float x1, float y1, float x2, float y2, int shade) {
    float t;
    if (y1 < y0) { t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
    if (y2 < y0) { t = y0; y0 = y2; y2 = t; t = x0; x0 = x2; x2 = t; }
    if (y2 < y1) { t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }
    if (y2 - y0 < 0.01f) return;
    int ys = (int)ceilf(y0 - 0.5f), ye = (int)ceilf(y2 - 0.5f);
    if (ys < c->cy0) ys = c->cy0;
    if (ye > c->cy1) ye = c->cy1;
    for (int y = ys; y < ye; y++) {
        float fy = y + 0.5f;
        float xa = x0 + (x2 - x0) * (fy - y0) / (y2 - y0), xb;
        if (fy < y1) xb = x0 + (x1 - x0) * (fy - y0) / (y1 - y0 > 1e-4f ? y1 - y0 : 1e-4f);
        else xb = x1 + (x2 - x1) * (fy - y1) / (y2 - y1 > 1e-4f ? y2 - y1 : 1e-4f);
        if (xa > xb) { t = xa; xa = xb; xb = t; }
        int xs = (int)ceilf(xa - 0.5f), xe = (int)ceilf(xb - 0.5f);
        if (xs < c->cx0) xs = c->cx0;
        if (xe > c->cx1) xe = c->cx1;
        uint32_t *row = c->px + y * c->w;
        for (int x = xs; x < xe; x++) row[x] = shade_px(c, x, y, shade);
    }
}

void sw_disc(SwCanvas *c, int cx, int cy, int r, int shade) {
    if (r <= 0) return;
    int y0 = cy - r < c->cy0 ? c->cy0 : cy - r, y1 = cy + r + 1 > c->cy1 ? c->cy1 : cy + r + 1;
    for (int y = y0; y < y1; y++) {
        int dy = y - cy, hw = (int)sqrtf((float)(r * r - dy * dy) + 0.25f);
        int xs = cx - hw < c->cx0 ? c->cx0 : cx - hw, xe = cx + hw + 1 > c->cx1 ? c->cx1 : cx + hw + 1;
        uint32_t *row = c->px + y * c->w;
        for (int x = xs; x < xe; x++) row[x] = shade_px(c, x, y, shade);
    }
}

void sw_ring(SwCanvas *c, int cx, int cy, int r, int th, SwColor color) {
    int ro2 = r * r, ri = r - th, ri2 = ri > 0 ? ri * ri : 0;
    /* Only walk the visible part: blast rings get far bigger than the canvas. */
    int ys = cy - r < c->cy0 ? c->cy0 : cy - r, ye = cy + r >= c->cy1 ? c->cy1 - 1 : cy + r;
    int xs = cx - r < c->cx0 ? c->cx0 : cx - r, xe = cx + r >= c->cx1 ? c->cx1 - 1 : cx + r;
    for (int y = ys; y <= ye; y++)
        for (int x = xs; x <= xe; x++) {
            int d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
            if (d <= ro2 && d >= ri2) sw_pixel(c, x, y, color);
        }
}

void sw_xor(SwCanvas *c, int x, int y, int w, int h) {
    int x0 = x < c->cx0 ? c->cx0 : x, y0 = y < c->cy0 ? c->cy0 : y;
    int x1 = x + w > c->cx1 ? c->cx1 : x + w, y1 = y + h > c->cy1 ? c->cy1 : y + h;
    for (int j = y0; j < y1; j++) {
        uint32_t *row = c->px + j * c->w;
        for (int i = x0; i < x1; i++) row[i] ^= INK_PX;
    }
}

void sw_invert_all(SwCanvas *c) {
    int n = c->w * c->h;
    for (int i = 0; i < n; i++) c->px[i] ^= INK_PX;
}

/* ---- font: 5x7, rows top to bottom, bit 4 is the left column ------------ */

static const uint8_t FONT[64][7] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* space */
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, /* ! */
    {0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00}, /* " */
    {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A}, /* # */
    {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}, /* $ */
    {0x18,0x19,0x02,0x04,0x08,0x13,0x03}, /* % */
    {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D}, /* & */
    {0x0C,0x04,0x08,0x00,0x00,0x00,0x00}, /* ' */
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, /* ( */
    {0x08,0x04,0x02,0x02,0x02,0x04,0x08}, /* ) */
    {0x00,0x04,0x15,0x0E,0x15,0x04,0x00}, /* * */
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, /* + */
    {0x00,0x00,0x00,0x00,0x0C,0x04,0x08}, /* , */
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, /* . */
    {0x00,0x01,0x02,0x04,0x08,0x10,0x00}, /* / */
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* 0 */
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* 2 */
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, /* 3 */
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* 4 */
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, /* 5 */
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /* 6 */
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* 8 */
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, /* 9 */
    {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}, /* : */
    {0x00,0x0C,0x0C,0x00,0x0C,0x04,0x08}, /* ; */
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, /* < */
    {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}, /* = */
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, /* > */
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}, /* ? */
    {0x0E,0x11,0x01,0x0D,0x15,0x15,0x0E}, /* @ */
    {0x0E,0x11,0x11,0x11,0x1F,0x11,0x11}, /* A */
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /* B */
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* C */
    {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}, /* D */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* E */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, /* F */
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, /* G */
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /* H */
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* I */
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, /* J */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* K */
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, /* L */
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, /* M */
    {0x11,0x11,0x19,0x15,0x13,0x11,0x11}, /* N */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* O */
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* P */
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, /* Q */
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /* R */
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, /* S */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* U */
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, /* V */
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, /* W */
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /* X */
    {0x11,0x11,0x11,0x0A,0x04,0x04,0x04}, /* Y */
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, /* Z */
    {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}, /* [ */
    {0x00,0x10,0x08,0x04,0x02,0x01,0x00}, /* \ */
    {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}, /* ] */
    {0x04,0x0A,0x11,0x00,0x00,0x00,0x00}, /* ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}, /* _ */
};

const uint8_t *sw_glyph(char ch) {
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    if (ch < 32 || ch > 95) return NULL;
    return FONT[ch - 32];
}

void sw_text(SwCanvas *c, int x, int y, const char *s, int dot, SwColor color) {
    for (; *s; s++, x += SW_CHAR_W * dot) {
        const uint8_t *g = sw_glyph(*s);
        if (!g) continue;
        for (int r = 0; r < 7; r++)
            for (int b = 0; b < 5; b++)
                if (g[r] & (0x10 >> b)) sw_fill(c, x + b * dot, y + r * dot, dot, dot, color);
    }
}

int sw_text_w(const char *s, int dot) {
    int n = (int)strlen(s);
    return n ? (n * SW_CHAR_W - 1) * dot : 0;
}

/* ---- PNG writer: stored deflate blocks, so no compression code needed --- */

static uint32_t crc_table[256];
static void crc_init(void) {
    if (crc_table[1]) return;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
static uint32_t crc_add(uint32_t crc, const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; i++) crc = crc_table[(crc ^ d[i]) & 255] ^ (crc >> 8);
    return crc;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void png_chunk(FILE *f, const char *type, const uint8_t *data, size_t n) {
    uint8_t b[4];
    be32(b, (uint32_t)n);
    fwrite(b, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t crc = crc_add(0xFFFFFFFFu, (const uint8_t *)type, 4);
    crc = crc_add(crc, data, n) ^ 0xFFFFFFFFu;
    be32(b, crc);
    fwrite(b, 1, 4, f);
}

bool sw_write_png(const char *path, const uint32_t *px, int w, int h) {
    crc_init();
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, w); be32(ihdr + 4, h);
    ihdr[8] = 1; ihdr[9] = 0; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0; /* 1-bit greyscale */
    png_chunk(f, "IHDR", ihdr, 13);

    size_t stride = (size_t)(w + 7) / 8 + 1, raw_n = stride * h;
    uint8_t *raw = calloc(raw_n, 1);
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + y * stride + 1;
        for (int x = 0; x < w; x++)
            if (px[y * w + x] & 0xFF) row[x >> 3] |= 0x80 >> (x & 7);
    }
    size_t blocks = raw_n / 65535 + 1;
    uint8_t *z = malloc(raw_n + blocks * 5 + 6), *o = z;
    *o++ = 0x78; *o++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw_n || off == 0;) {
        size_t n = raw_n - off > 65535 ? 65535 : raw_n - off;
        *o++ = off + n >= raw_n ? 1 : 0;
        *o++ = n & 255; *o++ = n >> 8;
        *o++ = ~n & 255; *o++ = (~n >> 8) & 255;
        memcpy(o, raw + off, n);
        o += n; off += n;
        if (!n) break;
    }
    be32(o, (b << 16) | a);
    o += 4;
    png_chunk(f, "IDAT", z, (size_t)(o - z));
    png_chunk(f, "IEND", NULL, 0);
    bool ok = !ferror(f);
    fclose(f);
    free(raw);
    free(z);
    return ok;
}
