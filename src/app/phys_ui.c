/* SoftPhys as a toy: drawing cells in 1-bit, its panels, tools, scenes, and the
 * link between the grid and SoftPute's soft bodies. */
#include "app.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Tools and their names, also used by the terminal. */
static const char *const PT_NAMES[PT_COUNT] = {
    "DRAW", "ERASE", "LINE", "FILL", "HEAT", "COOL", "PUSH", "BLAST", "GRAB", "THROW", "NUKE"
};
const char *phys_tool_name(int t) { return PT_NAMES[t]; }
static const char *const SCENE_NAMES[PHYS_NSCENES] = {
    "1 SANDBOX", "2 HOURGLASS", "3 VOLCANO", "4 FOREST", "5 DAM", "6 REFINERY", "7 GLACIER", "8 FUSE"
};
const char *phys_scene_name(int i) { return SCENE_NAMES[i]; }

/* ---- drawing: every element gets a texture you can tell apart without colour ---- */

static inline unsigned cell_hash(unsigned x, unsigned y) {
    unsigned h = x * 73856093u ^ y * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    return h ^ (h >> 15);
}

/* A 3x3 mask for a cell: bit 0 is top left, bit 8 is bottom right. */
static unsigned cell_mask(int e, unsigned x, unsigned y, unsigned tick, unsigned life, bool surface, bool burning) {
    unsigned h = cell_hash(x, y), f = cell_hash(x + tick / 3, y + tick);
    unsigned m = 0;
#define ROW(n) (0x7u << ((n) * 3))
    switch (e) {
    case PE_WALL: m = 0x1FF; break;
    case PE_ROCK: m = (h & 7) == 0 ? 0x0FF : ((x + y / 2) % 5 == 0 ? 0x1B6 : 0x1FF); break;
    case PE_METAL: m = (x + y) % 4 == 0 ? 0x0EE : 0x1FF; break;
    case PE_WOOD: m = 0x16D ^ ((h & 31) == 0 ? 0x092 : 0); break;
    case PE_ICE: m = 0x111 << 0 | ((x + y) & 1 ? 0x054 : 0); break;
    case PE_PLANT: m = (h & 0x1FF) | 0x0BA; break;
    case PE_GLASS: m = (x + y) % 3 == 0 ? 0x001 : 0; m |= (h & 63) == 0 ? 0x111 : 0; break;
    case PE_HEATER: m = x % 2 == y % 2 ? 0x0BA : 0x000; break;
    case PE_COOLER: m = y % 2 ? 0x038 : 0x000; break;
    case PE_SPOUT: m = 0x1EF; break;
    case PE_DRAIN: m = 0x145; break;
    case PE_SAND: m = h & 0x1FF & (h >> 9 | 0x155); break;
    case PE_SALT: m = (h | h >> 9) & 0x1FF; break;
    case PE_ASH: m = h & (h >> 9) & (h >> 18) & 0x1FF; break;
    case PE_COAL: m = 0x1FF & ~(h & (h >> 9)); break;
    case PE_POWDER: m = 0x155 ^ (h & 0x011); break;
    case PE_SEED: m = 0x0BA; break;
    case PE_SNOW: m = (h | 0x0AA) & 0x1FF; break;
    case PE_GRAVEL: m = ((x / 2 + y / 2) & 1) ? 0x1FF & ~(h & 7) : 0x010; break;
    case PE_WATER: m = (y + x / 6 + tick / 24) % 3 == 0 ? ROW((tick / 9 + x / 3) % 3) : 0; break;
    case PE_BRINE: m = (y + tick / 40) % 2 == 0 ? ROW(1) & 0x1C0 >> 3 & (x % 2 ? 0x038 : 0x008) : 0; m |= (h & 15) == 0 ? 0x010 : 0; break;
    case PE_OIL: m = (x + tick / 30) % 3 == 0 ? 0x049 : 0; break;
    case PE_ACID: m = (x + y + tick / 6) % 3 == 0 ? 0x111 : 0; break;
    case PE_LAVA: m = (f | 0x155) & 0x1FF; break;
    case PE_STEAM: m = f & (f >> 9) & 0x1FF; break;
    case PE_SMOKE: m = (x + y + (tick >> 3)) % 4 == 0 ? (h & 0x155) : 0; break;
    case PE_FIRE: m = life > 12 ? (f | f >> 7) & 0x1FF : f & 0x1FF; break;
    case PE_METHANE: m = (x + tick / 12) % 3 == 0 && (y % 3 == 0) ? 0x010 : 0; break;
    default: break;
    }
    if (surface) m |= 0x007;  /* a hard top edge, so a body of liquid reads as one */
    if (burning) m ^= f & 0x1FF;
    return m;
}

void phys_draw(SwCanvas *c, const PhysGrid *g, int ox, int oy, int cell, bool temp_view) {
    int cx0 = c->cx0, cy0 = c->cy0, cx1 = c->cx1, cy1 = c->cy1;
    const uint32_t ink = 0x00FFFFFF;
    unsigned tick = g->tick;
    for (int y = 0; y < g->h; y++) {
        int py = oy + y * cell;
        if (py >= cy1 || py + cell <= cy0) continue;
        for (int x = 0; x < g->w; x++) {
            int i = y * g->w + x, e = g->type[i];
            int px = ox + x * cell;
            if (px >= cx1 || px + cell <= cx0) continue;
            if (temp_view) {
                float t = g->temp[i];
                if (e == PE_EMPTY && fabsf(t - PHYS_AMBIENT) < 12) continue;
                int shade = t < PHYS_AMBIENT ? (int)((PHYS_AMBIENT - t) / 8) : (int)((t - PHYS_AMBIENT) / 60) + 1;
                shade = shade > 16 ? 16 : shade;
                sw_fill_shade(c, px, py, cell, cell, shade);
                continue;
            }
            if (e == PE_EMPTY) continue;
            bool surface = false;
            const PhysInfo *inf = phys_info(e);
            if (inf->kind == PK_LIQUID && y > 0 && g->type[i - g->w] == PE_EMPTY) surface = true;
            unsigned m = cell_mask(e, (unsigned)x, (unsigned)y, tick, g->life[i], surface, g->flags[i] & 1);
            if (!m) continue;
            for (int j = 0; j < cell; j++) {
                int ry = py + j;
                if (ry < cy0 || ry >= cy1) continue;
                uint32_t *row = c->px + ry * c->w;
                unsigned sub_y = (unsigned)(j * 3 / cell);
                for (int k = 0; k < cell; k++) {
                    int rx = px + k;
                    if (rx < cx0 || rx >= cx1) continue;
                    if (m & (1u << (sub_y * 3 + (unsigned)(k * 3 / cell)))) row[rx] = ink;
                }
            }
        }
    }
}

static void draw_swatch(App *a, int x, int y, int size, int e) {
    SwCanvas *c = &a->canvas;
    sw_fill(c, x, y, size, size, SW_PAPER);
    unsigned tick = a->phys.g ? a->phys.g->tick : 0;
    int cells = 4, cell = size / cells;
    if (cell < 1) cell = 1;
    for (int j = 0; j < cells; j++)
        for (int i = 0; i < cells; i++) {
            unsigned m = cell_mask(e, (unsigned)i * 7 + 3, (unsigned)j * 5 + 2, tick, 30, false, false);
            for (int b = 0; b < 9; b++)
                if (m & (1u << b))
                    sw_fill(c, x + i * cell + (b % 3) * cell / 3, y + j * cell + (b / 3) * cell / 3,
                            cell / 3 > 0 ? cell / 3 : 1, cell / 3 > 0 ? cell / 3 : 1, SW_INK);
        }
    sw_frame(c, x, y, size, size, 1, SW_INK);
}

/* ---- the grid as a medium for soft bodies ---- */

static float world_per_cell(const App *a) { return (float)a->phys.cell / a->vs; }

static bool phys_medium(void *user, float x, float y, SpMediumSample *out) {
    App *a = user;
    const PhysGrid *g = a->phys.g;
    float wpc = a->phys.world_per_cell;
    int gx = (int)(x / wpc), gy = (int)(y / wpc);
    if (gx < 0 || gy < 0 || gx >= g->w || gy >= g->h) return false;
    int e = g->type[gy * g->w + gx];
    if (e == PE_EMPTY) return false;
    const PhysInfo *inf = phys_info(e);
    if (inf->kind == PK_GAS) return false;
    if (inf->kind == PK_LIQUID) {
        /* Lift in proportion to how dense the liquid is. Soft bodies count as 2. */
        float lift = a->world->gy * inf->density / 2.0f;
        out->accel_y = -lift;
        out->drag = 7;
        return true;
    }
    /* Solid or powder: out, away from what's filled around here. */
    float nx = 0, ny = 0;
    for (int j = -2; j <= 2; j++)
        for (int i = -2; i <= 2; i++) {
            int ax = gx + i, ay = gy + j;
            if (ax < 0 || ay < 0 || ax >= g->w || ay >= g->h) continue;
            int o = g->type[ay * g->w + ax];
            PhysKind k = phys_info(o)->kind;
            if (o == PE_EMPTY || k == PK_GAS || k == PK_LIQUID) { nx += (float)i; ny += (float)j; }
        }
    float len = sqrtf(nx * nx + ny * ny);
    if (len < 0.01f) { nx = 0; ny = a->world->gy >= 0 ? -1.0f : 1.0f; len = 1; }
    float pen = wpc * 0.45f;
    out->push_x = nx / len * pen;
    out->push_y = ny / len * pen;
    out->friction = 0.9f;
    return true;
}

/* Marks the cells inside each closed body, and moves loose matter out of them. */
static void displace_cell(PhysGrid *g, int x, int y) {
    int c = y * g->w + x;
    int e = g->type[c];
    g->occ[c] = 1;
    PhysKind k = phys_info(e)->kind;
    if (e == PE_EMPTY || k == PK_SOLID) return;
    int up = g->gravity >= 0 ? -1 : 1;
    for (int s = 1; s <= 30; s++) {
        int ty = y + up * s;
        if (ty < 0 || ty >= g->h) return;
        int t = ty * g->w + x;
        if (g->type[t] != PE_EMPTY || g->occ[t]) continue;
        /* phys_set would reset the cell, and this is the same cell moved. */
        g->type[t] = g->type[c]; g->life[t] = g->life[c]; g->flags[t] = g->flags[c]; g->temp[t] = g->temp[c];
        g->type[c] = PE_EMPTY; g->life[c] = 0; g->flags[c] = 0;
        return;
    }
}

static void fill_tri(PhysGrid *g, float wpc, float x0, float y0, float x1, float y1, float x2, float y2) {
    float minx = fminf(x0, fminf(x1, x2)) / wpc, maxx = fmaxf(x0, fmaxf(x1, x2)) / wpc;
    float miny = fminf(y0, fminf(y1, y2)) / wpc, maxy = fmaxf(y0, fmaxf(y1, y2)) / wpc;
    int ax = (int)minx, bx = (int)maxx + 1, ay = (int)miny, by = (int)maxy + 1;
    if (ax < 0) ax = 0;
    if (ay < 0) ay = 0;
    if (bx >= g->w) bx = g->w - 1;
    if (by >= g->h) by = g->h - 1;
    float d = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    if (fabsf(d) < 1e-3f) return;
    for (int y = ay; y <= by; y++)
        for (int x = ax; x <= bx; x++) {
            float px = ((float)x + 0.5f) * wpc, py = ((float)y + 0.5f) * wpc;
            float l0 = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / d;
            float l1 = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / d;
            if (l0 >= 0 && l1 >= 0 && l0 + l1 <= 1) displace_cell(g, x, y);
        }
}

static void bodies_into_grid(App *a) {
    PhysGrid *g = a->phys.g;
    SpWorld *w = a->world;
    float wpc = a->phys.world_per_cell;
    memset(g->occ, 0, (size_t)g->w * g->h);
    for (int s = 0; s < SP_MAX_BODIES; s++) {
        const SpBody *b = &w->bodies[s];
        if (!b->alive || !b->closed) continue;
        int f = b->first;
        if (b->ntris) {
            for (int t = 0; t < b->ntris; t++) {
                const SpTri *tr = &b->tris[t];
                fill_tri(g, wpc, w->x[f + tr->a], w->y[f + tr->a], w->x[f + tr->b], w->y[f + tr->b],
                         w->x[f + tr->c], w->y[f + tr->c]);
            }
        } else {
            float cx, cy;
            sp_body_centroid(w, b, &cx, &cy);
            for (int e = 0; e < b->nedges; e++) {
                int p = f + b->edges[e].a, q = f + b->edges[e].b;
                fill_tri(g, wpc, cx, cy, w->x[p], w->y[p], w->x[q], w->y[q]);
            }
        }
    }
}

/* Soft things burn. */
static void scorch_bodies(App *a, float dt) {
    PhysGrid *g = a->phys.g;
    SpWorld *w = a->world;
    float wpc = a->phys.world_per_cell;
    for (int s = 0; s < SP_MAX_BODIES; s++) {
        SpBody *b = &w->bodies[s];
        if (!b->alive || b->kind == SP_GOO) continue;
        int hot = 0;
        for (int i = b->first; i < b->first + b->count; i += 2) {
            int gx = (int)(w->x[i] / wpc), gy = (int)(w->y[i] / wpc);
            if (phys_temp(g, gx, gy) > 450) hot++;
        }
        Toy *t = &a->toys[s];
        if (hot) { t->scorch += dt; t->dizzy = 1; }
        else if (t->scorch > 0) t->scorch -= dt * 0.25f;
        if (t->scorch > 1.3f) {
            float cx, cy;
            sp_body_centroid(w, b, &cx, &cy);
            int gx = (int)(cx / wpc), gy = (int)(cy / wpc), r = (int)(b->radius / wpc);
            sp_body_remove(w, s);
            phys_paint(g, gx, gy, r > 12 ? 12 : r, PE_ASH, false);
            fx_text(a, cx - 24, cy - 8, "TOAST");
        }
    }
}

/* ---- ticking ---- */

void phys_tick(App *a, float dt) {
    PhysUi *p = &a->phys;
    PhysGrid *g = p->g;
    if (!g) return;
    p->world_per_cell = world_per_cell(a);
    bodies_into_grid(a);
    int n = (int)(p->speed + 0.5f);
    if (n < 1) n = 1;
    for (int i = 0; i < n; i++) {
        phys_step(g);
        for (int b = 0; b < g->nblasts; b++) {
            const PhysBlast *e = &g->blasts[b];
            float x = ((float)e->x + 0.5f) * p->world_per_cell, y = ((float)e->y + 0.5f) * p->world_per_cell;
            float r = (float)e->r * p->world_per_cell;
            sp_blast(a->world, x, y, r * 4, 700);
            if (e->r >= 5) fx_ring(a, x, y, r * 2, 0.22f);
        }
    }
    scorch_bodies(a, dt);
}

/* ---- scenes ---- */

static void floor_and_walls(PhysGrid *g) {
    phys_rect(g, 0, g->h - 3, g->w - 1, g->h - 1, PE_WALL);
}

void phys_load_scene(App *a, int index) {
    PhysUi *p = &a->phys;
    if (index < 0 || index >= PHYS_NSCENES) return;
    p->scene = index;
    phys_clear_world(a);
    PhysGrid *g = p->g;
    phys_clear(g);
    int W = g->w, H = g->h, cx = W / 2;
    float wpc = world_per_cell(a);
#define SPAWN(item, gx, gy, size) toy_spawn(a, (item), (float)(gx) * wpc, (float)(gy) * wpc, (size))
    floor_and_walls(g);
    switch (index) {
    case 0: /* SANDBOX: a bit of everything to poke */
        for (int i = 0; i < 40; i++) phys_line(g, cx - 40 + i, H - 4 - i / 2, cx + 40 - i, H - 4 - i / 2, 0, PE_SAND, true);
        phys_rect(g, cx - 110, H - 40, cx - 106, H - 4, PE_WOOD);
        phys_rect(g, cx - 72, H - 40, cx - 68, H - 4, PE_WOOD);
        phys_rect(g, cx - 106, H - 30, cx - 72, H - 4, PE_WATER);
        phys_set_spout(g, cx + 80, H / 3, PE_SAND);
        SPAWN(ITEM_BLOB, cx, H / 2, 24);
        SPAWN(ITEM_BOX, cx - 90, H / 2, 18);
        break;
    case 1: { /* HOURGLASS */
        int top = H / 8, mid = H / 2, bot = H - 4, hw = H / 3;
        phys_line(g, cx - hw, top, cx - 2, mid, 1, PE_GLASS, true);
        phys_line(g, cx + hw, top, cx + 2, mid, 1, PE_GLASS, true);
        phys_line(g, cx - 2, mid, cx - hw, bot, 1, PE_GLASS, true);
        phys_line(g, cx + 2, mid, cx + hw, bot, 1, PE_GLASS, true);
        phys_line(g, cx - hw, top, cx + hw, top, 1, PE_GLASS, true);
        phys_flood(g, cx, top + 3, PE_EMPTY);
        for (int y = top + 3; y < mid - 12; y++) phys_line(g, cx - hw, y, cx + hw, y, 0, PE_SAND, false);
        /* Trim what spilled outside the glass. */
        for (int y = top + 3; y < mid - 12; y++) {
            int edge = hw - (y - top) * (hw - 2) / (mid - top) - 2;
            for (int x = cx - hw - 2; x < cx - edge - 1; x++) if (phys_get(g, x, y) == PE_SAND) phys_set(g, x, y, PE_EMPTY);
            for (int x = cx + edge + 2; x <= cx + hw + 2; x++) if (phys_get(g, x, y) == PE_SAND) phys_set(g, x, y, PE_EMPTY);
        }
        break;
    }
    case 2: { /* VOLCANO */
        int peak = H / 2, base = H - 4, hw = (base - peak) * 3 / 2;
        for (int y = peak; y <= base; y++) {
            int w = (y - peak) * hw / (base - peak) + 2;
            phys_line(g, cx - w, y, cx + w, y, 0, PE_ROCK, true);
        }
        /* A vent up the middle, with a magma chamber feeding it. */
        phys_rect(g, cx - 3, peak, cx + 3, base - 3, PE_EMPTY);
        for (int x = cx - 3; x <= cx + 3; x++) phys_set_spout(g, x, base - 3, PE_LAVA);
        phys_rect(g, 0, base - 12, cx - hw - 4, base, PE_WATER);
        phys_rect(g, cx + hw + 4, base - 12, W - 1, base, PE_WATER);
        SPAWN(ITEM_BOX, (cx - hw) / 2, base - 40, 16);
        SPAWN(ITEM_BLOB, cx + hw + (W - cx - hw) / 2, base - 40, 18);
        break;
    }
    case 3: { /* FOREST */
        int ground = H - 12;
        phys_rect(g, 0, ground, W - 1, H - 4, PE_SAND);
        for (int x = 14; x < W - 14; x += 22 + (x * 7) % 12) {
            int top = ground - 24 - (x * 13) % 24;
            phys_rect(g, x, top, x + 1, ground - 1, PE_WOOD);
            phys_paint(g, x, top - 2, 7 + (x * 3) % 5, PE_PLANT, false);
        }
        phys_paint(g, 8, ground - 6, 3, PE_FIRE, false);
        phys_heat(g, 16, ground - 28, 6, 500);
        SPAWN(ITEM_HOPPER, cx, ground - 40, 18);
        break;
    }
    case 4: { /* DAM */
        int bx = W / 3;
        phys_rect(g, bx, H / 3, bx + 3, H - 4, PE_WOOD);
        phys_rect(g, 0, H / 3 + 8, bx - 1, H - 4, PE_WATER);
        for (int y = H / 3 + 6; y < H - 8; y += 12) phys_set(g, bx + 4, y, PE_POWDER);
        phys_rect(g, bx + 4, H - 8, bx + 8, H - 4, PE_POWDER);
        phys_rect(g, W - 24, H - 30, W - 21, H - 4, PE_METAL);
        SPAWN(ITEM_BOX, bx / 2, H / 3 - 8, 18);
        SPAWN(ITEM_BLOB, bx / 3, H / 3 - 8, 16);
        SPAWN(ITEM_BLOB, bx * 2 / 3 + 2, H / 2 - 8, 14);
        app_flash(a, "HEAT THE POWDER AT THE FOOT OF THE DAM");
        break;
    }
    case 5: { /* REFINERY: layers cooked from below */
        int tank = W / 4;
        phys_rect(g, cx - tank - 3, H / 3, cx - tank, H - 4, PE_METAL);
        phys_rect(g, cx + tank, H / 3, cx + tank + 3, H - 4, PE_METAL);
        phys_rect(g, cx - tank, H - 7, cx + tank, H - 4, PE_METAL);
        phys_rect(g, cx - tank + 6, H - 3, cx - tank + 18, H - 3, PE_HEATER);
        phys_rect(g, cx - tank + 1, H - 22, cx + tank - 1, H - 8, PE_WATER);
        phys_rect(g, cx - tank + 1, H - 34, cx + tank - 1, H - 23, PE_OIL);
        phys_rect(g, cx - tank + 1, H - 42, cx + tank - 1, H - 35, PE_SALT);
        break;
    }
    case 6: { /* GLACIER */
        for (int y = H / 3; y < H - 3; y++) {
            int w = (y - H / 3) * 3 / 2 + 8;
            phys_line(g, cx - w, y, cx + w, y, 0, PE_ICE, true);
        }
        phys_rect(g, cx - 10, H / 3 - 14, cx + 10, H / 3 - 2, PE_SNOW);
        phys_rect(g, 0, H - 6, 16, H - 4, PE_HEATER);
        phys_rect(g, W - 17, H - 6, W - 1, H - 4, PE_HEATER);
        SPAWN(ITEM_BLOB, cx, H / 4, 20);
        break;
    }
    case 7: { /* FUSE: a line of powder feeding bigger and bigger charges */
        int y = H - 6;
        for (int i = 0; i < 5; i++) {
            int x = W / 8 + i * W / 7, size = 3 + i * 2;
            phys_rect(g, x - size, y - size, x + size, y, PE_POWDER);
            phys_rect(g, x - size, y - size * 3 - 4, x + size, y - size - 2, PE_WOOD);
            SPAWN(ITEM_BOX, x, y - size * 3 - 20, 12 + i);
        }
        phys_rect(g, 0, y, W - 1, y, PE_POWDER);
        phys_paint(g, 2, y - 2, 1, PE_FIRE, false);
        break;
    }
    }
}

/* ---- panels ---- */

static bool swatch_button(App *a, int e, bool second) {
    SwDesk *d = &a->desk;
    int y = d->cy, gap = d->dot, w = (d->cw - gap) / 2;
    int x = second ? d->cx + w + gap : d->cx;
    bool picked = sw_button_half(d, phys_info(e)->name, a->phys.element == e && a->phys.tool == PT_DRAW, second);
    draw_swatch(a, x + d->line + 1, y + d->line + 1, d->row - d->line * 2 - 2, e);
    return picked;
}

static void element_group(App *a, const char *title, const int *list, int n) {
    SwDesk *d = &a->desk;
    sw_heading(d, title);
    for (int i = 0; i < n; i++)
        if (swatch_button(a, list[i], i & 1)) {
            a->phys.element = list[i];
            a->phys.tool = PT_DRAW;
        }
    if (n & 1) d->cy += d->row + d->dot;
    sw_space(d, 1);
}

void phys_panels(App *a, SwPanel *p) {
    SwDesk *d = &a->desk;
    PhysUi *u = &a->phys;
    PhysGrid *g = u->g;
    if (p == &a->p_elements) {
        if (!sw_panel_begin(d, p)) return;
        int saved = d->row;
        d->row = d->dot * 9;
        static const int powders[] = { PE_SAND, PE_SALT, PE_ASH, PE_COAL, PE_POWDER, PE_SEED, PE_SNOW, PE_GRAVEL };
        static const int liquids[] = { PE_WATER, PE_BRINE, PE_OIL, PE_ACID, PE_LAVA };
        static const int gases[] = { PE_STEAM, PE_SMOKE, PE_FIRE, PE_METHANE };
        static const int solids[] = { PE_WALL, PE_ROCK, PE_METAL, PE_WOOD, PE_ICE, PE_PLANT, PE_GLASS, PE_HEATER, PE_COOLER, PE_SPOUT, PE_DRAIN };
        element_group(a, "POWDER", powders, 8);
        element_group(a, "LIQUID", liquids, 5);
        element_group(a, "GAS", gases, 4);
        element_group(a, "SOLID", solids, 11);
        d->row = saved;
        sw_panel_end(d);
    } else if (p == &a->p_lab) {
        if (!sw_panel_begin(d, p)) return;
        int saved = d->row;
        d->row = d->dot * 9;
        sw_heading(d, "TOOL");
        for (int t = 0; t < PT_COUNT; t++) {
            /* NUKE takes a full row to itself. */
            if (t == PT_NUKE) {
                if (sw_button(d, PT_NAMES[t], (int)u->tool == t)) u->tool = (PhysTool)t;
            } else if (sw_button_half(d, PT_NAMES[t], (int)u->tool == t, t & 1)) {
                u->tool = (PhysTool)t;
            }
        }
        d->row = saved;
        sw_space(d, 1);
        sw_slider(d, "BRUSH", &u->brush, 0, 24, "%.0f");
        sw_slider(d, "SPEED", &u->speed, 1, 4, "%.0f");
        float grav = (float)g->gravity;
        if (sw_slider(d, "GRAVITY", &grav, -1, 1, "%.0f")) {
            g->gravity = grav > 0.33f ? 1 : grav < -0.33f ? -1 : 0;
            a->world->gy = (float)g->gravity * 900;
        }
        if (sw_button_half(d, a->paused ? "RUN" : "PAUSE", a->paused, false)) a->paused = !a->paused;
        if (sw_button_half(d, "STEP", false, true)) { a->paused = true; a->step_once++; }
        if (sw_button_half(d, "RESET", false, false)) phys_load_scene(a, u->scene);
        if (sw_button_half(d, "CLEAR", false, true)) { phys_clear(g); phys_clear_world(a); }
        sw_panel_end(d);
    } else if (p == &a->p_info) {
        if (!sw_panel_begin(d, p)) return;
        char buf[64];
        int gx = u->hover_x, gy = u->hover_y;
        if (u->hover_ok) {
            snprintf(buf, sizeof buf, "CELL  %d,%d", gx, gy);
            sw_label(d, buf);
            snprintf(buf, sizeof buf, "HOLDS %s", phys_info(phys_get(g, gx, gy))->name);
            sw_label(d, buf);
            snprintf(buf, sizeof buf, "TEMP  %.0f", (double)phys_temp(g, gx, gy));
            sw_label(d, buf);
        } else {
            sw_label(d, "CELL  -");
            sw_label(d, "HOLDS -");
            sw_label(d, "TEMP  -");
        }
        sw_space(d, 2);
        long total = 0;
        for (int e = 1; e < PE_COUNT; e++) total += g->counts[e];
        snprintf(buf, sizeof buf, "CELLS  %ld", total);
        sw_label(d, buf);
        snprintf(buf, sizeof buf, "BODIES %d", sp_count_bodies(a->world));
        sw_label(d, buf);
        sw_panel_end(d);
    }
}

/* ---- input ---- */

void phys_input(App *a) {
    PhysUi *u = &a->phys;
    SwDesk *d = &a->desk;
    const SwInput *in = &d->in;
    PhysGrid *g = u->g;
    SpWorld *w = a->world;
    float wx = (float)(in->mx - a->ax) / a->vs, wy = (float)(in->my - a->ay) / a->vs;
    bool free_mouse = !d->mouse_on_ui && in->inside;
    u->world_per_cell = world_per_cell(a);
    int cx = (in->mx - a->ax) / u->cell, cy = (in->my - a->ay) / u->cell;
    bool here = free_mouse && cx >= 0 && cy >= 0 && cx < g->w && cy < g->h;
    u->hover_ok = here;
    u->hover_x = cx;
    u->hover_y = cy;
    a->mouse_x = wx;
    a->mouse_y = wy;
    a->mouse_in_arena = here;
    int brush = (int)(u->brush + 0.5f);

    if (u->grabbing) {
        sp_grab_move(w, wx, wy);
        if (!in->down[0]) { sp_grab_end(w); u->grabbing = false; }
    }
    if (here && in->wheel) {
        u->brush -= (float)in->wheel;
        u->brush = u->brush < 0 ? 0 : u->brush > 24 ? 24 : u->brush;
    }
    if (here && in->pressed[1]) {
        phys_blast(g, cx, cy, 8);
    }
    bool press = here && in->pressed[0], held = here && in->down[0];
    if (!in->down[0] && u->tool != PT_LINE) u->stroke = false;
    /* Fills gaps between frames with a line, so fast strokes stay unbroken. */
    int lx = u->stroke ? u->last_x : cx, ly = u->stroke ? u->last_y : cy;
    switch (u->tool) {
    case PT_DRAW:
        if (held) phys_line(g, lx, ly, cx, cy, brush, u->element, false);
        break;
    case PT_ERASE:
        if (held) phys_line(g, lx, ly, cx, cy, brush, PE_EMPTY, true);
        break;
    case PT_LINE:
        if (press) { u->line_on = true; u->line_x = cx; u->line_y = cy; }
        if (u->line_on && !in->down[0]) {
            u->line_on = false;
            phys_line(g, u->line_x, u->line_y, cx, cy, brush, u->element, false);
        }
        break;
    case PT_FILL:
        if (press) phys_flood(g, cx, cy, u->element);
        break;
    case PT_HEAT:
        if (held) phys_heat(g, cx, cy, brush + 1, 60);
        break;
    case PT_COOL:
        if (held) phys_heat(g, cx, cy, brush + 1, -30);
        break;
    case PT_PUSH:
        if (held && u->stroke) {
            int dx = cx - lx, dy = cy - ly;
            dx = dx > 2 ? 2 : dx < -2 ? -2 : dx;
            dy = dy > 2 ? 2 : dy < -2 ? -2 : dy;
            phys_push(g, cx, cy, brush + 2, dx, dy);
        }
        break;
    case PT_BLAST:
        if (press) phys_blast(g, cx, cy, 6 + brush);
        break;
    case PT_GRAB:
        if (press) {
            sp_grab_begin(w, wx, wy, 26);
            u->grabbing = w->ngrabs > 0;
        }
        break;
    case PT_BODY:
        if (press) toy_spawn(a, u->body_item, wx, wy, 14 + (float)brush);
        break;
    case PT_NUKE:
        if (press) nuke_launch(a, wx, wy);
        break;
    default: break;
    }
    if (held) { u->stroke = true; u->last_x = cx; u->last_y = cy; }
}

void phys_key(App *a, int key) {
    PhysUi *u = &a->phys;
    if (key >= SW_KEY_1 && key < SW_KEY_1 + PHYS_NSCENES) { phys_load_scene(a, key - SW_KEY_1); return; }
    switch (key) {
    case SW_KEY_D: u->tool = PT_DRAW; break;
    case SW_KEY_E: u->tool = PT_ERASE; break;
    case SW_KEY_L: u->tool = PT_LINE; break;
    case SW_KEY_F: u->tool = PT_FILL; break;
    case SW_KEY_H: u->tool = PT_HEAT; break;
    case SW_KEY_O: u->tool = PT_COOL; break;
    case SW_KEY_U: u->tool = PT_PUSH; break;
    case SW_KEY_B: u->tool = PT_BLAST; break;
    case SW_KEY_G: u->tool = PT_GRAB; break;
    case SW_KEY_K: u->tool = PT_NUKE; break;
    case SW_KEY_T: u->body_item = (Item)((u->body_item + 1) % 3); u->tool = PT_BODY;
        app_flash(a, item_name(u->body_item)); break;
    case SW_KEY_V: u->temp_view = !u->temp_view; break;
    case SW_KEY_R: phys_load_scene(a, u->scene); break;
    case SW_KEY_C: phys_clear(u->g); phys_clear_world(a); break;
    default: break;
    }
}

const char *phys_status(App *a, char *buf, int n) {
    PhysUi *u = &a->phys;
    if (u->tool == PT_DRAW || u->tool == PT_LINE || u->tool == PT_FILL)
        snprintf(buf, (size_t)n, "%s %s: %s", PT_NAMES[u->tool], phys_info(u->element)->name, phys_info(u->element)->about);
    else if (u->tool == PT_BODY)
        snprintf(buf, (size_t)n, "THROW %s  PRESS T TO CHANGE", item_name(u->body_item));
    else
        snprintf(buf, (size_t)n, "%s  BRUSH %.0f", PT_NAMES[u->tool], (double)u->brush);
    return buf;
}

/* ---- the whole arena ---- */

void phys_render(App *a) {
    PhysUi *u = &a->phys;
    SwCanvas *c = &a->canvas;
    render_begin(a);
    phys_draw(c, u->g, a->ax + (int)a->shake_x, a->ay + (int)a->shake_y, u->cell, u->temp_view);
    render_matter(a);
    if (a->mouse_in_arena) {
        const SwInput *in = &a->desk.in;
        int r = (int)((u->brush + 0.5f) * (float)u->cell);
        int lw = a->desk.line;
        if (u->tool == PT_NUKE) {
            sw_ring(c, in->mx, in->my, (int)(NUKE_FIREBALL * a->vs), lw, SW_INK);
        } else if (u->tool == PT_BODY) {
            sw_ring(c, in->mx, in->my, (int)((14 + u->brush) * a->vs), lw, SW_INK);
        } else if (u->tool != PT_FILL && u->tool != PT_GRAB) {
            sw_ring(c, in->mx, in->my, r > 1 ? r : 1, lw, SW_INK);
        }
        if (u->line_on) {
            int x0 = a->ax + u->line_x * u->cell, y0 = a->ay + u->line_y * u->cell;
            sw_dash(c, x0, y0, in->mx, in->my, lw, lw * 3, lw * 3, SW_INK);
        }
    }
    render_end(a);
}

void phys_layout(App *a) {
    PhysUi *u = &a->phys;
    int cell = (int)(3 * a->vs + 0.5f);
    u->cell = cell < 2 ? 2 : cell;
    u->world_per_cell = world_per_cell(a);
    int w = a->aw / u->cell, h = a->ah / u->cell;
    if (u->g) phys_resize(u->g, w, h);
}

void phys_clear_world(App *a) {
    SpWorld *w = a->world_phys;
    sp_world_clear(w);
    brk_world_cleared(a, w);
    memset(a->toys_phys, 0, sizeof a->toys_phys);
    memset(a->nukes, 0, sizeof a->nukes);
    a->shake = 0;
}

void phys_start(App *a) {
    PhysUi *u = &a->phys;
    if (u->g) return;
    phys_layout(a);
    u->g = phys_new(a->aw / u->cell, a->ah / u->cell);
    u->element = PE_SAND;
    u->tool = PT_DRAW;
    u->brush = 3;
    u->speed = 1;
    u->body_item = ITEM_BLOB;
    a->world_phys->medium = phys_medium;
    a->world_phys->medium_user = a;
    phys_load_scene(a, 0);
}
