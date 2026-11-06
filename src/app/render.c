/* Draws the simulation. Every shade is a dither of pure black and white. */
#include "app.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SX(a, v) ((a)->ax + (a)->shake_x + (v) * (a)->vs)
#define SY(a, v) ((a)->ay + (a)->shake_y + (v) * (a)->vs)

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void draw_statics(App *a) {
    SwCanvas *c = &a->canvas;
    const SpWorld *w = a->world;
    for (int i = 0; i < w->nstatics; i++) {
        const SpStatic *s = &w->statics[i];
        if (!s->alive || s->kind != SP_STATIC_CAPSULE) continue;
        int x0 = (int)SX(a, s->x0), y0 = (int)SY(a, s->y0), x1 = (int)SX(a, s->x1), y1 = (int)SY(a, s->y1);
        int r = (int)(s->radius * a->vs);
        if (s->x0 == s->x1 && s->y0 == s->y1) {
            sw_disc(c, x0, y0, r, 16);
            sw_disc(c, x0, y0, r / 2, 0);
        } else {
            sw_line(c, x0, y0, x1, y1, r * 2, SW_INK);
            sw_dash(c, x0, y0, x1, y1, r > 2 ? r : 1, r * 2 + 2, r * 2 + 2, SW_PAPER);
        }
    }
}

/* Goo: splat particles into a coarse field and threshold it. The chunky cells
 * are deliberate, they keep the edges sharp. */
static void draw_goo(App *a) {
    const SpWorld *w = a->world;
    bool any = false;
    for (int s = 0; s < SP_MAX_BODIES; s++)
        if (w->bodies[s].alive && w->bodies[s].kind == SP_GOO) { any = true; break; }
    if (!any) return;

    int cell = (int)(3 * a->vs + 0.5f);
    if (cell < 2) cell = 2;
    /* The field covers what's being drawn: the arena, or a patch of the desktop. */
    const SwCanvas *clip = &a->canvas;
    int gx0 = (int)floorf((float)(clip->cx0 - a->ax) / (float)cell) - 1;
    int gy0 = (int)floorf((float)(clip->cy0 - a->ay) / (float)cell) - 1;
    int gw = (clip->cx1 - clip->cx0) / cell + 4, gh = (clip->cy1 - clip->cy0) / cell + 4;
    static float *field;
    static int cap;
    if (gw * gh > cap) {
        cap = gw * gh;
        free(field);
        field = malloc(sizeof(float) * (size_t)cap);
    }
    memset(field, 0, sizeof(float) * (size_t)(gw * gh));
    float R = 8.5f * a->vs / (float)cell;
    int ir = (int)R + 1;
    for (int s = 0; s < SP_MAX_BODIES; s++) {
        const SpBody *b = &w->bodies[s];
        if (!b->alive || b->kind != SP_GOO) continue;
        for (int i = b->first; i < b->first + b->count; i++) {
            float gx = w->x[i] * a->vs / (float)cell - (float)gx0, gy = w->y[i] * a->vs / (float)cell - (float)gy0;
            if (gx < -R || gy < -R || gx > (float)gw + R || gy > (float)gh + R) continue;
            int x0 = (int)gx, y0 = (int)gy;
            for (int y = y0 - ir; y <= y0 + ir; y++) {
                if (y < 0 || y >= gh) continue;
                for (int x = x0 - ir; x <= x0 + ir; x++) {
                    if (x < 0 || x >= gw) continue;
                    float dx = (float)x + 0.5f - gx, dy = (float)y + 0.5f - gy;
                    float q = 1 - (dx * dx + dy * dy) / (R * R);
                    if (q > 0) field[y * gw + x] += q * q;
                }
            }
        }
    }
    SwCanvas *c = &a->canvas;
    for (int y = 0; y < gh; y++)
        for (int x = 0; x < gw; x++) {
            float v = field[y * gw + x];
            if (v < 0.75f) continue;
            int px = a->ax + (int)a->shake_x + (x + gx0) * cell, py = a->ay + (int)a->shake_y + (y + gy0) * cell;
            if (v >= 1.15f) sw_fill(c, px, py, cell, cell, SW_INK);
            else sw_fill_shade(c, px, py, cell, cell, 8);
        }
    if (a->view == VIEW_WIRE) {
        for (int s = 0; s < SP_MAX_BODIES; s++) {
            const SpBody *b = &w->bodies[s];
            if (!b->alive || b->kind != SP_GOO) continue;
            for (int i = b->first; i < b->first + b->count; i++)
                sw_xor(c, (int)SX(a, w->x[i]) - 1, (int)SY(a, w->y[i]) - 1, 3, 3);
        }
    }
}

static float tri_rest_area(const SpBody *b, const SpTri *t) {
    float ax = b->q0x[t->b] - b->q0x[t->a], ay = b->q0y[t->b] - b->q0y[t->a];
    float bx = b->q0x[t->c] - b->q0x[t->a], by = b->q0y[t->c] - b->q0y[t->a];
    return 0.5f * (ax * by - ay * bx);
}

static void draw_outline(App *a, const SpBody *b, int th) {
    const SpWorld *w = a->world;
    SwCanvas *c = &a->canvas;
    for (int e = 0; e < b->nedges; e++) {
        int i = b->first + b->edges[e].a, j = b->first + b->edges[e].b;
        sw_line(c, (int)SX(a, w->x[i]), (int)SY(a, w->y[i]), (int)SX(a, w->x[j]), (int)SY(a, w->y[j]), th, SW_INK);
    }
}

static void draw_links(App *a, int slot, float maxrest, bool dashed) {
    const SpWorld *w = a->world;
    SwCanvas *c = &a->canvas;
    for (int i = 0; i < w->nlinks; i++) {
        const SpLink *l = &w->links[i];
        if (l->dead || l->body != slot || l->rest > maxrest) continue;
        int x0 = (int)SX(a, w->x[l->a]), y0 = (int)SY(a, w->y[l->a]);
        int x1 = (int)SX(a, w->x[l->b]), y1 = (int)SY(a, w->y[l->b]);
        if (dashed) sw_dash(c, x0, y0, x1, y1, 1, 2, 2, SW_INK);
        else sw_line(c, x0, y0, x1, y1, 1, SW_INK);
    }
}

static void draw_jelly(App *a, int slot, const SpBody *b, const Toy *t) {
    const SpWorld *w = a->world;
    SwCanvas *c = &a->canvas;
    int lw = a->desk.line;
    if (a->view == VIEW_WIRE) {
        for (int k = 0; k < b->ntris; k++) {
            const SpTri *tr = &b->tris[k];
            float x0 = SX(a, w->x[b->first + tr->a]), y0 = SY(a, w->y[b->first + tr->a]);
            float x1 = SX(a, w->x[b->first + tr->b]), y1 = SY(a, w->y[b->first + tr->b]);
            float x2 = SX(a, w->x[b->first + tr->c]), y2 = SY(a, w->y[b->first + tr->c]);
            sw_tri(c, x0, y0, x1, y1, x2, y2, 0);
        }
        draw_links(a, slot, 1e9f, false);
    } else {
        for (int k = 0; k < b->ntris; k++) {
            const SpTri *tr = &b->tris[k];
            int pa = b->first + tr->a, pb = b->first + tr->b, pc = b->first + tr->c;
            float x0 = w->x[pa], y0 = w->y[pa], x1 = w->x[pb], y1 = w->y[pb], x2 = w->x[pc], y2 = w->y[pc];
            int shade = t->shade;
            if (a->view == VIEW_STRESS) {
                /* Compressed matter goes solid, stretched matter thins out. */
                float rest = fabsf(tri_rest_area(b, tr));
                float now = fabsf(0.5f * ((x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0)));
                float ratio = rest > 0.01f ? now / rest : 1;
                shade = clampi((int)(8 + (1 - ratio) * 26), 1, 16);
            }
            sw_tri(c, SX(a, x0), SY(a, y0), SX(a, x1), SY(a, y1), SX(a, x2), SY(a, y2), shade);
        }
    }
    draw_outline(a, b, lw + 1);
}

static void draw_balloon(App *a, int slot, const SpBody *b, const Toy *t) {
    const SpWorld *w = a->world;
    SwCanvas *c = &a->canvas;
    if (b->popped) {
        draw_links(a, slot, 1e9f, false);
        return;
    }
    float cx, cy;
    sp_body_centroid(w, b, &cx, &cy);
    /* Tension shows as density: the closer to bursting, the darker. */
    int shade = t->shade;
    if (a->view == VIEW_STRESS || b->stretch_peak > 1.2f) {
        float k = (b->stretch_peak - 1) / (b->burst - 1);
        shade = clampi((int)(k * 14), t->shade, 15);
    }
    int fill = a->view == VIEW_WIRE ? 0 : shade;
    for (int e = 0; e < b->nedges; e++) {
        int i = b->first + b->edges[e].a, j = b->first + b->edges[e].b;
        sw_tri(c, SX(a, cx), SY(a, cy), SX(a, w->x[i]), SY(a, w->y[i]), SX(a, w->x[j]), SY(a, w->y[j]), fill);
    }
    if (a->view == VIEW_WIRE) draw_links(a, slot, 1e9f, true);
    draw_outline(a, b, a->desk.line + 1);
    /* A square glint and a knot: the only decoration a balloon needs. */
    int r = (int)(b->radius * a->vs);
    int g = r / 6 > 1 ? r / 6 : 2;
    sw_fill(c, (int)SX(a, cx) - r * 5 / 8, (int)SY(a, cy) - r * 5 / 8, g, g, SW_INK);
    int low = b->first + b->count / 4;
    int kx = (int)SX(a, w->x[low]), ky = (int)SY(a, w->y[low]), ks = (int)(3 * a->vs);
    sw_fill(c, kx - ks, ky, ks * 2, ks, SW_INK);
}

static void draw_cloth(App *a, int slot, const SpBody *b, const Toy *t) {
    const SpWorld *w = a->world;
    SwCanvas *c = &a->canvas;
    if (a->view != VIEW_WIRE) {
        int nx = t->cloth_nx > 1 ? t->cloth_nx : 2;
        for (int k = 0; k < b->ntris; k++) {
            const SpTri *tr = &b->tris[k];
            if (!sp_tri_alive(w, tr)) continue;
            int pa = b->first + tr->a, pb = b->first + tr->b, pc = b->first + tr->c;
            float x0 = w->x[pa], y0 = w->y[pa], x1 = w->x[pb], y1 = w->y[pb], x2 = w->x[pc], y2 = w->y[pc];
            /* Gingham: two dither weights in blocks of two cells. */
            int cell = k / 2, ci = cell % (nx - 1), cj = cell / (nx - 1);
            int shade = ((ci / 2 + cj / 2) & 1) ? 10 : 4;
            if (a->view == VIEW_STRESS) {
                float now = fabsf(0.5f * ((x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0)));
                shade = clampi((int)(2 + (now / 60.5f - 1) * 18), 1, 16);
            }
            sw_tri(c, SX(a, x0), SY(a, y0), SX(a, x1), SY(a, y1), SX(a, x2), SY(a, y2), shade);
        }
    }
    draw_links(a, slot, 12.5f, false);
}

static void draw_rope(App *a, const SpBody *b) {
    const SpWorld *w = a->world;
    SwCanvas *c = &a->canvas;
    const Toy *t = &a->toys[b - w->bodies];
    int th = (int)((float)(t->rope_width ? t->rope_width : 2) * a->vs + 0.5f);
    if (th < 2) th = 2;
    for (int i = 0; i < w->nlinks; i++) {
        const SpLink *l = &w->links[i];
        if (l->dead || l->a < b->first || l->a >= b->first + b->count) continue;
        if (l->b != l->a + 1) continue;
        sw_line(c, (int)SX(a, w->x[l->a]), (int)SY(a, w->y[l->a]), (int)SX(a, w->x[l->b]), (int)SY(a, w->y[l->b]), th, SW_INK);
    }
}

static void face_point(float ca, float sa, float lx, float ly, float *x, float *y) {
    *x = ca * lx - sa * ly;
    *y = sa * lx + ca * ly;
}

static void draw_face(App *a, int slot, const SpBody *b, const Toy *t) {
    (void)slot;
    const SpWorld *w = a->world;
    SwCanvas *c = &a->canvas;
    float cx, cy;
    if (b->center >= 0) { cx = w->x[b->first + b->center]; cy = w->y[b->first + b->center]; }
    else sp_body_centroid(w, b, &cx, &cy);
    /* Faces stay within 40 degrees of upright, so tumbling reads as tilting. */
    float ang = b->kind == SP_JELLY ? atan2f(sinf(b->angle), cosf(b->angle)) : 0;
    if (ang > 0.7f) ang = 0.7f;
    if (ang < -0.7f) ang = -0.7f;
    float ca = cosf(ang), sa = sinf(ang), R = b->radius;
    float er = R * 0.2f;
    if (er < 3.2f) er = 3.2f;
    float ex = R * 0.34f, ey = -R * 0.12f;
    bool dizzy = t->dizzy > 0, blink = t->blink < 0;

    float dirx = a->mouse_x - cx, diry = a->mouse_y - cy, dl = hypotf(dirx, diry);
    if (dl > 1) { dirx /= dl; diry /= dl; } else { dirx = 0; diry = 0; }
    if (!a->mouse_in_arena) { dirx = 0; diry = 0; }

    int lw = a->desk.line;
    for (int side = -1; side <= 1; side += 2) {
        float ox, oy;
        face_point(ca, sa, side * ex, ey, &ox, &oy);
        int px = (int)SX(a, cx + ox), py = (int)SY(a, cy + oy);
        int rr = (int)(er * a->vs);
        sw_disc(c, px, py, rr + lw, 0);
        if (dizzy) {
            int m = rr - lw;
            sw_line(c, px - m, py - m, px + m, py + m, lw + 1, SW_INK);
            sw_line(c, px + m, py - m, px - m, py + m, lw + 1, SW_INK);
        } else if (blink) {
            sw_fill(c, px - rr, py - lw, rr * 2, lw * 2, SW_INK);
        } else {
            sw_disc(c, px, py, rr, 16);
            int pr = rr / 2 > 1 ? rr / 2 : 1;
            sw_disc(c, px + (int)(dirx * (float)(rr - pr)), py + (int)(diry * (float)(rr - pr)), pr, 0);
        }
    }
    float mx, my;
    face_point(ca, sa, 0, R * 0.36f, &mx, &my);
    int qx = (int)SX(a, cx + mx), qy = (int)SY(a, cy + my);
    int mw = (int)(R * 0.24f * a->vs);
    if (mw < 3) mw = 3;
    if (dizzy || b->stretch_peak > 1.2f) {
        sw_disc(c, qx, qy, mw * 2 / 3 + lw, 16);
        sw_disc(c, qx, qy, mw * 2 / 3, 0);
    } else {
        /* A three-segment smile, drawn on a halo so it reads on any dither. */
        int d = mw / 2;
        int xs[4] = { qx - mw, qx - d, qx + d, qx + mw };
        int ys[4] = { qy - d, qy, qy, qy - d };
        for (int i = 0; i < 3; i++) sw_line(c, xs[i], ys[i], xs[i + 1], ys[i + 1], lw + 3, SW_INK);
        for (int i = 0; i < 3; i++) sw_line(c, xs[i], ys[i], xs[i + 1], ys[i + 1], lw + 1, SW_PAPER);
    }
}

/* Eyes on the end of a rope, looking where it's headed. */
static void draw_snake_head(App *a, const SpBody *b) {
    const SpWorld *w = a->world;
    SwCanvas *c = &a->canvas;
    int head = b->first, next = b->first + 1;
    float dx = w->x[head] - w->x[next], dy = w->y[head] - w->y[next], d = hypotf(dx, dy) + 0.01f;
    dx /= d; dy /= d;
    float hx = w->x[head], hy = w->y[head];
    int rr = (int)(7 * a->vs);
    sw_disc(c, (int)SX(a, hx), (int)SY(a, hy), rr + a->desk.line, 0);
    sw_disc(c, (int)SX(a, hx), (int)SY(a, hy), rr, 16);
    for (int side = -1; side <= 1; side += 2) {
        int ex = (int)SX(a, hx + dx * 2 - dy * 3.5f * (float)side), ey = (int)SY(a, hy + dy * 2 + dx * 3.5f * (float)side);
        sw_disc(c, ex, ey, (int)(2 * a->vs), 0);
    }
}

/* Links between bodies, made by the GLUE tool or tying on a balloon's string. */
static void draw_glue(App *a) {
    const SpWorld *w = a->world;
    SwCanvas *c = &a->canvas;
    for (int i = 0; i < w->nlinks; i++) {
        const SpLink *l = &w->links[i];
        if (l->dead || l->body >= 0) continue;
        sw_line(c, (int)SX(a, w->x[l->a]), (int)SY(a, w->y[l->a]), (int)SX(a, w->x[l->b]), (int)SY(a, w->y[l->b]), a->desk.line + 1, SW_INK);
    }
}

static void draw_fx(App *a) {
    SwCanvas *c = &a->canvas;
    for (int i = 0; i < MAX_FX; i++) {
        const Fx *f = &a->fx[i];
        if (f->ttl <= 0) continue;
        float k = f->t / f->ttl;
        if (f->kind == FX_RING) {
            int r = (int)(f->r * a->vs * sqrtf(k));
            int th = (int)((1 - k) * 5 * a->vs) + 1;
            sw_ring(c, (int)SX(a, f->x), (int)SY(a, f->y), r, th, SW_INK);
        } else {
            int x = (int)SX(a, f->x), y = (int)SY(a, f->y - k * 30);
            int d = a->desk.dot, w = sw_text_w(f->text, d);
            /* Hard knockout, so the word stays legible over anything. */
            sw_fill(c, x - d, y - d, w + d * 2, 7 * d + d * 2, SW_PAPER);
            sw_text(c, x, y, f->text, d, SW_INK);
        }
    }
}

static float nuke_hash(unsigned seed, int i) {
    unsigned h = (seed + (unsigned)i * 2654435761u) * 2246822519u;
    h ^= h >> 15;
    h *= 3266489917u;
    h ^= h >> 13;
    return (float)(h & 0xFFFF) / 65536.0f;
}

static void draw_warhead(App *a, const Nuke *n) {
    SwCanvas *c = &a->canvas;
    float k = n->t / NUKE_FALL;
    float wy = -40 + (n->y + 40) * k * k;
    int x = (int)SX(a, n->x), y = (int)SY(a, wy), u = (int)(2 * a->vs + 0.5f);
    sw_tri(c, (float)(x - 5 * u), (float)(y - 2 * u), (float)(x + 5 * u), (float)(y - 2 * u), (float)x, (float)(y + 9 * u), 16);
    sw_fill(c, x - 5 * u, y - 14 * u, 10 * u, 12 * u, SW_INK);
    sw_tri(c, (float)(x - 5 * u), (float)(y - 14 * u), (float)(x - 9 * u), (float)(y - 20 * u), (float)(x - 5 * u), (float)(y - 8 * u), 16);
    sw_tri(c, (float)(x + 5 * u), (float)(y - 14 * u), (float)(x + 9 * u), (float)(y - 20 * u), (float)(x + 5 * u), (float)(y - 8 * u), 16);
    sw_fill(c, x - 3 * u, y - 9 * u, 6 * u, 2 * u, SW_PAPER);
    /* Where it's going to land. */
    int tx = (int)SX(a, n->x), ty = (int)SY(a, n->y), s = 8 * u;
    sw_frame(c, tx - s, ty - s, s * 2, s * 2, a->desk.line, SW_INK);
}

/* A puff has a hard black edge, so overlapping puffs read as a billowing mass. */
static void puff(App *a, float x, float y, float r, int shade) {
    if (r < 1) return;
    SwCanvas *c = &a->canvas;
    int px = (int)SX(a, x), py = (int)SY(a, y), rr = (int)(r * a->vs);
    sw_disc(c, px, py, rr + a->desk.line * 2, 0);
    sw_disc(c, px, py, rr, shade);
}

static void draw_cloud(App *a, const Nuke *n) {
    SwCanvas *c = &a->canvas;
    float e = n->t;
    float x = n->x, y = n->y;

    /* Rises fast, then coasts. Puffs shrink away at the end, instead of a grey
     * fade, so the edges stay sharp right up until it's gone. */
    float rise_k = e / 2.4f > 1 ? 1 : e / 2.4f;
    float rise = (1 - (1 - rise_k) * (1 - rise_k)) * 230;
    float grow = e / 1.4f > 1 ? 1 : e / 1.4f;
    float fade = e < NUKE_LIFE - 1.4f ? 1 : (NUKE_LIFE - e) / 1.4f;
    if (fade < 0) fade = 0;
    float capy = y - rise;

    /* Fallout: specks that drift down across the arena. */
    if (e > 0.8f) {
        int s = (int)(2 * a->vs + 0.5f);
        for (int i = 0; i < 70 * fade; i++) {
            float ax0 = x + (nuke_hash(n->seed, i) - 0.5f) * 760;
            float ay0 = capy + (nuke_hash(n->seed, i + 200) - 0.3f) * 160;
            float fall = (e - 0.8f) * (24 + nuke_hash(n->seed, i + 400) * 46);
            float sway = sinf(e * 1.6f + (float)i) * 9;
            sw_fill(c, (int)SX(a, ax0 + sway), (int)SY(a, ay0 + fall), s, s, SW_INK);
        }
    }

    /* Stem */
    if (rise > 10) {
        int count = 12;
        for (int i = 0; i <= count; i++) {
            float k = (float)i / (float)count;
            float wobble = sinf(k * 7 + e * 2.2f + (float)n->seed) * 7 * k;
            float r = (15 + 9 * (1 - k) * (1 - k) + nuke_hash(n->seed, i) * 5) * fade;
            puff(a, x + wobble, y + (capy - y) * k, r, (i & 1) ? 6 : 9);
        }
    }

    /* Cap: a rolling torus, far side first. */
    int cap = 30;
    for (int layer = 0; layer < 2; layer++) {
        for (int i = 0; i < cap; i++) {
            float ang = (float)i * 6.2831853f / (float)cap + e * 0.7f;
            bool front = sinf(ang) > 0;
            if (front != (layer == 1)) continue;
            float rx = (66 + nuke_hash(n->seed, i + 30) * 58) * grow;
            float ry = 22 * grow;
            float r = (25 + nuke_hash(n->seed, i + 60) * 17) * grow * fade;
            float lift = -nuke_hash(n->seed, i + 90) * 26 * grow;
            puff(a, x + cosf(ang) * rx, capy + sinf(ang) * ry + lift, r, front ? 5 + i % 3 * 3 : 3);
        }
    }
    /* Crown */
    for (int i = 0; i < 9; i++) {
        float ang = (float)i * 0.698f + e * 0.3f;
        float r = (24 + nuke_hash(n->seed, i + 120) * 12) * grow * fade;
        puff(a, x + cosf(ang) * 46 * grow, capy - 34 * grow + sinf(ang) * 12 * grow, r, 16 - i % 3 * 3);
    }

    /* Fireball: hot and solid, swelling, then swallowed by its own cloud. */
    if (e < 0.75f) {
        float k = e / 0.75f;
        float r = NUKE_FIREBALL * sqrtf(k < 0.3f ? k / 0.3f : 1) * (k < 0.3f ? 1 : 1 - (k - 0.3f) / 0.7f);
        sw_disc(c, (int)SX(a, x), (int)SY(a, y - rise * 0.5f), (int)(r * a->vs), 16);
    }
}

static void draw_nukes(App *a) {
    SwCanvas *c = &a->canvas;
    for (int i = 0; i < MAX_NUKES; i++) {
        const Nuke *n = &a->nukes[i];
        if (!n->active) continue;
        if (!n->exploded) { draw_warhead(a, n); continue; }
        draw_cloud(a, n);
        /* The shock front, visible while it's on screen. */
        if (n->front < hypotf(a->world_w, a->world_h)) {
            int r = (int)(n->front * a->vs), th = (int)(7 * a->vs) + 1;
            sw_ring(c, (int)SX(a, n->x), (int)SY(a, n->y), r, th, SW_INK);
            sw_ring(c, (int)SX(a, n->x), (int)SY(a, n->y), r - th * 3, a->desk.line, SW_INK);
        }
    }
}

/* A single white-out that dissolves. It never pulses. */
static void draw_blast_flash(App *a) {
    if (!a->nuke_flash) return;
    double k = (a->sim_time - a->blast_flash_at) / 0.7;
    if (k < 0 || k >= 1) return;
    int shade = (int)(17 * (1 - k));
    if (shade > 0) sw_stipple(&a->canvas, a->ax, a->ay, a->aw, a->ah, shade);
}

static void draw_trefoil(App *a, int mx, int my) {
    SwCanvas *c = &a->canvas;
    float r0 = 7 * a->vs, r1 = 19 * a->vs;
    for (int k = 0; k < 3; k++) {
        float mid = -1.5708f + (float)k * 2.0944f, half = 0.5236f;
        float steps = 4;
        for (int j = 0; j < (int)steps; j++) {
            float t0 = mid - half + (float)j * 2 * half / steps, t1 = t0 + 2 * half / steps;
            float ax0 = (float)mx + cosf(t0) * r0, ay0 = (float)my + sinf(t0) * r0;
            float ax1 = (float)mx + cosf(t1) * r0, ay1 = (float)my + sinf(t1) * r0;
            float bx0 = (float)mx + cosf(t0) * r1, by0 = (float)my + sinf(t0) * r1;
            float bx1 = (float)mx + cosf(t1) * r1, by1 = (float)my + sinf(t1) * r1;
            sw_tri(c, ax0, ay0, bx0, by0, bx1, by1, 16);
            sw_tri(c, ax0, ay0, bx1, by1, ax1, ay1, 16);
        }
    }
    sw_disc(c, mx, my, (int)(4 * a->vs), 16);
    /* What the fireball will vaporize. */
    sw_ring(c, mx, my, (int)(NUKE_FIREBALL * a->vs), a->desk.line, SW_INK);
}

static void draw_grabs(App *a) {
    SwCanvas *c = &a->canvas;
    const SpWorld *w = a->world;
    int lw = a->desk.line;
    int mx = (int)SX(a, a->mouse_x), my = (int)SY(a, a->mouse_y);
    for (int g = 0; g < w->ngrabs; g++) {
        int i = w->grabs[g].particle;
        sw_fill(c, (int)SX(a, w->x[i]) - lw, (int)SY(a, w->y[i]) - lw, lw * 2 + 1, lw * 2 + 1, SW_INK);
    }
    if (w->ngrabs) {
        int i = w->grabs[0].particle;
        sw_dash(c, mx, my, (int)SX(a, w->x[i]), (int)SY(a, w->y[i]), lw, lw * 3, lw * 3, SW_INK);
    }
}

static void draw_tool_overlay(App *a) {
    SwCanvas *c = &a->canvas;
    const SpWorld *w = a->world;
    int lw = a->desk.line;
    int mx = (int)SX(a, a->mouse_x), my = (int)SY(a, a->mouse_y);
    if (a->glue_from >= 0 && a->glue_from < w->np)
        sw_dash(c, (int)SX(a, w->x[a->glue_from]), (int)SY(a, w->y[a->glue_from]), mx, my, lw, lw * 3, lw * 3, SW_INK);
    if (!a->mouse_in_arena) return;
    if (a->tool == TOOL_SPAWN) {
        int r = (int)(a->size * a->vs);
        sw_ring(c, mx, my, r, lw, SW_INK);
    } else if ((a->tool == TOOL_BLOW || a->tool == TOOL_MAGNET) && w->field_on) {
        int r = (int)(w->field_radius * a->vs);
        for (int i = 0; i < 12; i++) {
            float t = (float)i * 6.2831853f / 12 + (float)a->sim_time * 3;
            sw_dash(c, mx + (int)(cosf(t) * r * 0.25f), my + (int)(sinf(t) * r * 0.25f),
                    mx + (int)(cosf(t) * r * 0.8f), my + (int)(sinf(t) * r * 0.8f), lw, lw * 3, lw * 4, SW_INK);
        }
    } else if (a->tool == TOOL_BOMB) {
        sw_ring(c, mx, my, (int)(150 * a->vs), lw, SW_INK);
    } else if (a->tool == TOOL_WALL && a->walling) {
        sw_dash(c, (int)SX(a, a->wall_x), (int)SY(a, a->wall_y), mx, my, lw * 2, lw * 4, lw * 3, SW_INK);
    } else if (a->tool == TOOL_CUT && a->cutting) {
        sw_line(c, (int)SX(a, a->cut_x), (int)SY(a, a->cut_y), mx, my, lw * 2, SW_INK);
    } else if (a->tool == TOOL_NUKE) {
        draw_trefoil(a, mx, my);
    } else if (a->tool == TOOL_TYPE) {
        int d = (int)(10 * a->vs);
        int tx = (int)SX(a, a->type_x), ty = (int)SY(a, a->type_y);
        sw_frame(c, tx, ty, d * 5, d * 7, lw, SW_INK);
    }
}

void render_begin(App *a) {
    SwCanvas *c = &a->canvas;
    sw_clip(c, a->ax, a->ay, a->aw, a->ah);
    sw_fill(c, a->ax, a->ay, a->aw, a->ah, SW_PAPER);
    /* Everything in the arena jumps together. Frozen when paused. */
    a->shake_x = a->shake_y = 0;
    if (a->shake > 0 && !a->paused) {
        a->shake_x = (app_rand(a) - 0.5f) * 2 * a->shake * a->vs;
        a->shake_y = (app_rand(a) - 0.5f) * 2 * a->shake * a->vs;
    }
}

/* The SoftPute world and what happens to it, whichever toy it's in. */
void render_matter(App *a) {
    SwCanvas *c = &a->canvas;
    const SpWorld *w = a->world;
    draw_statics(a);
    for (int s = 0; s < SP_MAX_BODIES; s++) {
        const SpBody *b = &w->bodies[s];
        if (!b->alive) continue;
        if (b->kind == SP_CLOTH) draw_cloth(a, s, b, &a->toys[s]);
    }
    for (int s = 0; s < SP_MAX_BODIES; s++) {
        const SpBody *b = &w->bodies[s];
        if (!b->alive) continue;
        if (b->kind == SP_ROPE) draw_rope(a, b);
    }
    draw_glue(a);
    draw_goo(a);
    for (int s = 0; s < SP_MAX_BODIES; s++) {
        const SpBody *b = &w->bodies[s];
        if (!b->alive) continue;
        const Toy *t = &a->toys[s];
        if (t->elem) continue; /* a widget, which draws itself */
        if (b->kind == SP_JELLY) draw_jelly(a, s, b, t);
        else if (b->kind == SP_BALLOON) draw_balloon(a, s, b, t);
        if (t->snake) draw_snake_head(a, b);
        else if (t->face && a->show_faces && a->view != VIEW_WIRE && !b->popped) draw_face(a, s, b, t);
    }
    if (a->show_points) {
        for (int i = 0; i < w->np; i++)
            sw_xor(c, (int)SX(a, w->x[i]) - 1, (int)SY(a, w->y[i]) - 1, 3, 3);
    }
    /* Pins are always visible: small hard squares. */
    for (int i = 0; i < w->np; i++) {
        if (w->w[i] > 0) continue;
        int px = (int)SX(a, w->x[i]), py = (int)SY(a, w->y[i]), r = (int)(3 * a->vs);
        sw_fill(c, px - r, py - r, r * 2 + 1, r * 2 + 1, SW_PAPER);
        sw_frame(c, px - r, py - r, r * 2 + 1, r * 2 + 1, a->desk.line, SW_INK);
    }
    draw_nukes(a);
    draw_fx(a);
    draw_grabs(a);
}

void render_end(App *a) {
    draw_blast_flash(a);
    a->shake_x = a->shake_y = 0;
    sw_noclip(&a->canvas);
}

void render_world(App *a) {
    SwCanvas *c = &a->canvas;
    render_begin(a);
    /* A hairline floor grid at low density gives motion a reference. */
    int grid = (int)(48 * a->vs);
    for (int y = a->ay + grid; y < a->ay + a->ah; y += grid)
        for (int x = a->ax + grid; x < a->ax + a->aw; x += grid)
            sw_pixel(c, x, y, SW_INK);
    render_matter(a);
    draw_tool_overlay(a);
    render_end(a);
}

/* Cursors are XOR boxes, so they stay visible over any pattern. */
void render_cursor(App *a) {
    SwCanvas *c = &a->canvas;
    const SwInput *in = &a->desk.in;
    if (!in->inside) return;
    int x = in->mx, y = in->my, u = a->desk.line, s = a->desk.dot * 5;
    if (a->desk.cursor_hint) {
        /* Resize: a diagonal of squares. */
        for (int i = -3; i <= 3; i++) sw_xor(c, x + i * u * 2, y + i * u * 2, u * 2, u * 2);
        return;
    }
    if (a->desk.mouse_on_ui) {
        /* Arrow: stepped rows. */
        for (int r = 0; r < 11; r++) sw_xor(c, x, y + r * u, (r < 8 ? r + 1 : 11 - r) * u, u);
        return;
    }
    switch (a->mode == MODE_BODIES ? a->tool : TOOL_GRAB) {
    case TOOL_CUT:
        for (int i = -3; i <= 3; i++) {
            sw_xor(c, x + i * u * 2, y + i * u * 2, u * 2, u * 2);
            if (i) sw_xor(c, x - i * u * 2, y + i * u * 2, u * 2, u * 2);
        }
        break;
    case TOOL_ERASE:
        sw_xor(c, x - s, y - s, s * 2, u);
        sw_xor(c, x - s, y + s - u, s * 2, u);
        sw_xor(c, x - s, y - s + u, u, s * 2 - 2 * u);
        sw_xor(c, x + s - u, y - s + u, u, s * 2 - 2 * u);
        break;
    default:
        /* A crosshair with a hollow middle. */
        sw_xor(c, x - s, y, s - u * 2, u);
        sw_xor(c, x + u * 3, y, s - u * 2, u);
        sw_xor(c, x, y - s, u, s - u * 2);
        sw_xor(c, x, y + u * 3, u, s - u * 2);
        break;
    }
}
