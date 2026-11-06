/* SoftPhys simulation. See softphys.h for the model. */
#include "softphys.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define F_BURNING 1

static const PhysInfo INFO[PE_COUNT] = {
    /*             name       about                                kind       dens  cond   temp   ignite */
    [PE_EMPTY]   = { "EMPTY",   "NOTHING. AIR.",                     PK_EMPTY,  0.0f, 0.001f, 22,   0 },
    [PE_WALL]    = { "WALL",    "NOTHING GETS THROUGH THIS.",        PK_SOLID,  99,   0.00f, 22,    0 },
    [PE_ROCK]    = { "ROCK",    "SOLID GROUND. ACID EATS IT SLOWLY.", PK_SOLID,  99,   0.06f, 22,    0 },
    [PE_METAL]   = { "METAL",   "CARRIES HEAT VERY WELL.",           PK_SOLID,  99,   0.24f, 22,    0 },
    [PE_WOOD]    = { "WOOD",    "BURNS SLOWLY, LEAVES ASH.",         PK_SOLID,  99,   0.02f, 22,    280 },
    [PE_ICE]     = { "ICE",     "MELTS. FREEZES WATER IT TOUCHES.",  PK_SOLID,  99,   0.12f, -20,   0 },
    [PE_PLANT]   = { "PLANT",   "DRINKS WATER AND GROWS. BURNS.",    PK_SOLID,  99,   0.03f, 22,    180 },
    [PE_GLASS]   = { "GLASS",   "ACID CANNOT EAT THIS.",             PK_SOLID,  99,   0.04f, 22,    0 },
    [PE_HEATER]  = { "HEATER",  "ALWAYS 1200 DEGREES.",              PK_SOLID,  99,   0.24f, 1200,  0 },
    [PE_COOLER]  = { "COOLER",  "ALWAYS -60 DEGREES.",               PK_SOLID,  99,   0.24f, -60,   0 },
    [PE_SPOUT]   = { "SPOUT",   "COPIES WHAT TOUCHES IT, FOREVER.",  PK_SOLID,  99,   0.00f, 22,    0 },
    [PE_DRAIN]   = { "DRAIN",   "SWALLOWS ANYTHING LOOSE.",          PK_SOLID,  99,   0.00f, 22,    0 },
    [PE_SAND]    = { "SAND",    "PILES UP. MELTS INTO GLASS.",       PK_POWDER, 5.0f, 0.05f, 22,    0 },
    [PE_SALT]    = { "SALT",    "DISSOLVES IN WATER, MELTS ICE.",    PK_POWDER, 4.5f, 0.05f, 22,    0 },
    [PE_ASH]     = { "ASH",     "WHAT IS LEFT OF A FIRE.",           PK_POWDER, 1.5f, 0.03f, 22,    0 },
    [PE_COAL]    = { "COAL",    "BURNS LONG AND VERY HOT.",          PK_POWDER, 4.0f, 0.04f, 22,    380 },
    [PE_POWDER]  = { "POWDER",  "EXPLODES WHEN HOT.",                PK_POWDER, 4.0f, 0.05f, 22,    170 },
    [PE_SEED]    = { "SEED",    "LANDS AND SPROUTS NEAR WATER.",     PK_POWDER, 2.0f, 0.03f, 22,    200 },
    [PE_SNOW]    = { "SNOW",    "LIGHT AND COLD. MELTS EASILY.",     PK_POWDER, 1.2f, 0.08f, -10,   0 },
    [PE_GRAVEL]  = { "GRAVEL",  "COOLED LAVA. RE-MELTS WHEN HOT.",   PK_POWDER, 7.0f, 0.08f, 22,    0 },
    [PE_WATER]   = { "WATER",   "BOILS, FREEZES, PUTS OUT FIRE.",    PK_LIQUID, 3.0f, 0.14f, 22,    0 },
    [PE_BRINE]   = { "BRINE",   "SALTY WATER. SINKS UNDER WATER.",   PK_LIQUID, 3.4f, 0.14f, 22,    0 },
    [PE_OIL]     = { "OIL",     "FLOATS ON WATER. VERY FLAMMABLE.",  PK_LIQUID, 2.4f, 0.06f, 22,    200 },
    [PE_ACID]    = { "ACID",    "EATS NEARLY EVERYTHING.",           PK_LIQUID, 3.2f, 0.08f, 22,    0 },
    [PE_LAVA]    = { "LAVA",    "1400 DEGREES. COOLS TO GRAVEL.",    PK_LIQUID, 6.0f, 0.10f, 1400,  0 },
    [PE_STEAM]   = { "STEAM",   "RISES, COOLS, RAINS BACK DOWN.",    PK_GAS,    0.2f, 0.03f, 130,   0 },
    [PE_SMOKE]   = { "SMOKE",   "RISES AND FADES.",                  PK_GAS,    0.3f, 0.02f, 90,    0 },
    [PE_FIRE]    = { "FIRE",    "HOT, BRIEF, AND IT SPREADS.",       PK_GAS,    0.1f, 0.20f, 800,   0 },
    [PE_METHANE] = { "METHANE", "A RISING GAS THAT GOES BANG.",      PK_GAS,    0.15f, 0.02f, 22,   120 },
};

const PhysInfo *phys_info(int e) { return &INFO[e >= 0 && e < PE_COUNT ? e : PE_EMPTY]; }

int phys_element_by_name(const char *name) {
    for (int e = 0; e < PE_COUNT; e++) {
        const char *a = INFO[e].name, *b = name;
        while (*a && *b && *a == toupper((unsigned char)*b)) { a++; b++; }
        if (!*a && !*b) return e;
    }
    return -1;
}

/* ---- storage ---- */

static void alloc_grid(PhysGrid *g, int w, int h) {
    size_t n = (size_t)w * h;
    g->w = w;
    g->h = h;
    g->type = calloc(n, 1);
    g->life = calloc(n, 1);
    g->flags = calloc(n, 1);
    g->stamp = calloc(n, 1);
    g->occ = calloc(n, 1);
    g->temp = malloc(n * sizeof(float));
    for (size_t i = 0; i < n; i++) g->temp[i] = PHYS_AMBIENT;
}

static void free_arrays(PhysGrid *g) {
    free(g->type); free(g->life); free(g->flags); free(g->stamp); free(g->occ); free(g->temp);
}

PhysGrid *phys_new(int w, int h) {
    PhysGrid *g = calloc(1, sizeof *g);
    g->rng = 0x6B1D3A57u;
    g->gravity = 1;
    alloc_grid(g, w < 1 ? 1 : w, h < 1 ? 1 : h);
    return g;
}

void phys_free(PhysGrid *g) {
    if (!g) return;
    free_arrays(g);
    free(g);
}

void phys_clear(PhysGrid *g) {
    size_t n = (size_t)g->w * g->h;
    memset(g->type, 0, n);
    memset(g->life, 0, n);
    memset(g->flags, 0, n);
    memset(g->occ, 0, n);
    for (size_t i = 0; i < n; i++) g->temp[i] = PHYS_AMBIENT;
    g->nblasts = 0;
    memset(g->counts, 0, sizeof g->counts);
}

void phys_resize(PhysGrid *g, int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w == g->w && h == g->h) return;
    PhysGrid old = *g;
    alloc_grid(g, w, h);
    int cw = w < old.w ? w : old.w, ch = h < old.h ? h : old.h;
    for (int y = 0; y < ch; y++) {
        /* Row 0 of this copy is the floor of each grid. */
        size_t to = (size_t)(h - 1 - y) * w, from = (size_t)(old.h - 1 - y) * old.w;
        memcpy(g->type + to, old.type + from, (size_t)cw);
        memcpy(g->life + to, old.life + from, (size_t)cw);
        memcpy(g->flags + to, old.flags + from, (size_t)cw);
        memcpy(g->temp + to, old.temp + from, (size_t)cw * sizeof(float));
    }
    free_arrays(&old);
}

/* ---- small helpers ---- */

static inline unsigned rnd(PhysGrid *g) {
    g->rng ^= g->rng << 13;
    g->rng ^= g->rng >> 17;
    g->rng ^= g->rng << 5;
    return g->rng;
}
static inline bool chance(PhysGrid *g, unsigned per_1000) { return rnd(g) % 1000 < per_1000; }
static inline bool in_grid(const PhysGrid *g, int x, int y) { return x >= 0 && y >= 0 && x < g->w && y < g->h; }
static inline int at(const PhysGrid *g, int x, int y) { return y * g->w + x; }

int phys_get(const PhysGrid *g, int x, int y) { return in_grid(g, x, y) ? g->type[at(g, x, y)] : PE_WALL; }
float phys_temp(const PhysGrid *g, int x, int y) { return in_grid(g, x, y) ? g->temp[at(g, x, y)] : PHYS_AMBIENT; }

static void put(PhysGrid *g, int i, int e) {
    g->type[i] = (uint8_t)e;
    g->flags[i] = 0;
    g->life[i] = 0;
    g->temp[i] = INFO[e].start_temp;
    g->stamp[i] = g->tick;
    if (e == PE_FIRE) g->life[i] = (uint8_t)(20 + rnd(g) % 40);
    else if (e == PE_SMOKE) g->life[i] = (uint8_t)(60 + rnd(g) % 120);
    else if (e == PE_STEAM) g->life[i] = (uint8_t)(120 + rnd(g) % 120);
    else if (e == PE_PLANT) g->life[i] = 4;
}

/* Changes element while keeping the heat the cell already has. */
static void become(PhysGrid *g, int i, int e) {
    float t = g->temp[i];
    put(g, i, e);
    if (e != PE_EMPTY && e != PE_FIRE) g->temp[i] = t;
}

static inline void swap_cells(PhysGrid *g, int a, int b) {
    uint8_t t = g->type[a]; g->type[a] = g->type[b]; g->type[b] = t;
    t = g->life[a]; g->life[a] = g->life[b]; g->life[b] = t;
    t = g->flags[a]; g->flags[a] = g->flags[b]; g->flags[b] = t;
    float f = g->temp[a]; g->temp[a] = g->temp[b]; g->temp[b] = f;
    g->stamp[a] = g->tick;
    g->stamp[b] = g->tick;
}

void phys_set(PhysGrid *g, int x, int y, int e) {
    if (in_grid(g, x, y)) put(g, at(g, x, y), e);
}

void phys_paint(PhysGrid *g, int x, int y, int r, int e, bool overwrite) {
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++) {
            if (i * i + j * j > r * r + r / 2 || !in_grid(g, x + i, y + j)) continue;
            int c = at(g, x + i, y + j);
            if (g->occ[c]) continue;
            if (overwrite || g->type[c] == PE_EMPTY) put(g, c, e);
        }
}

void phys_line(PhysGrid *g, int x0, int y0, int x1, int y1, int r, int e, bool overwrite) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        phys_paint(g, x0, y0, r, e, overwrite);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void phys_rect(PhysGrid *g, int x0, int y0, int x1, int y1, int e) {
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            phys_set(g, x, y, e);
}

void phys_flood(PhysGrid *g, int x, int y, int e) {
    if (!in_grid(g, x, y)) return;
    int from = g->type[at(g, x, y)];
    if (from == e) return;
    size_t cap = (size_t)g->w * g->h + 1, top = 0;
    int *stack = malloc(cap * sizeof(int));
    stack[top++] = at(g, x, y);
    g->type[at(g, x, y)] = (uint8_t)PE_COUNT; /* marks it as queued */
    while (top) {
        int c = stack[--top], cx = c % g->w, cy = c / g->w;
        put(g, c, e);
        static const int nx[4] = { 1, -1, 0, 0 }, ny[4] = { 0, 0, 1, -1 };
        for (int k = 0; k < 4; k++) {
            int ax = cx + nx[k], ay = cy + ny[k];
            if (!in_grid(g, ax, ay)) continue;
            int n = at(g, ax, ay);
            if (g->type[n] != from || g->occ[n]) continue;
            g->type[n] = (uint8_t)PE_COUNT;
            stack[top++] = n;
        }
    }
    free(stack);
}

void phys_heat(PhysGrid *g, int x, int y, int r, float amount) {
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++) {
            if (i * i + j * j > r * r || !in_grid(g, x + i, y + j)) continue;
            float *t = &g->temp[at(g, x + i, y + j)];
            *t += amount;
            if (*t < -200) *t = -200;
            if (*t > 3000) *t = 3000;
        }
}

static inline bool loose(int e) { return INFO[e].kind == PK_POWDER || INFO[e].kind == PK_LIQUID || INFO[e].kind == PK_GAS; }

void phys_push(PhysGrid *g, int x, int y, int r, int dx, int dy) {
    if (!dx && !dy) return;
    /* Walk from the leading edge, so a block of cells moves as a block. */
    int sx = dx > 0 ? -1 : 1, sy = dy > 0 ? -1 : 1;
    for (int j = sy > 0 ? -r : r; j >= -r && j <= r; j += sy)
        for (int i = sx > 0 ? -r : r; i >= -r && i <= r; i += sx) {
            if (i * i + j * j > r * r) continue;
            int ax = x + i, ay = y + j, bx = ax + dx, by = ay + dy;
            if (!in_grid(g, ax, ay) || !in_grid(g, bx, by)) continue;
            int a = at(g, ax, ay), b = at(g, bx, by);
            if (loose(g->type[a]) && g->type[b] == PE_EMPTY && !g->occ[b]) swap_cells(g, a, b);
        }
}

/* Moves one cell along one axis until something stops it. */
static void shove_cell(PhysGrid *g, int x, int y, int sx, int sy, int reach) {
    int c = at(g, x, y);
    if (!loose(g->type[c])) return;
    for (int s = 0; s < reach; s++) {
        x += sx; y += sy;
        if (!in_grid(g, x, y)) return;
        int n = at(g, x, y);
        if (g->type[n] != PE_EMPTY || g->occ[n]) return;
        swap_cells(g, c, n);
        c = n;
    }
}

void phys_shove(PhysGrid *g, int dx, int dy) {
    /* From the leading edge, so a block of cells moves as a block. */
    if (dx) {
        int sx = dx > 0 ? 1 : -1;
        for (int y = 0; y < g->h; y++)
            for (int q = 0; q < g->w; q++)
                shove_cell(g, sx > 0 ? g->w - 1 - q : q, y, sx, 0, abs(dx));
    }
    if (dy) {
        int sy = dy > 0 ? 1 : -1;
        for (int q = 0; q < g->h; q++)
            for (int x = 0; x < g->w; x++)
                shove_cell(g, x, sy > 0 ? g->h - 1 - q : q, 0, sy, abs(dy));
    }
}

void phys_set_spout(PhysGrid *g, int x, int y, int e) {
    if (!in_grid(g, x, y)) return;
    int c = at(g, x, y);
    put(g, c, PE_SPOUT);
    g->life[c] = (uint8_t)e;
}

long phys_total(const PhysGrid *g, int e) {
    long n = 0;
    size_t size = (size_t)g->w * g->h;
    for (size_t i = 0; i < size; i++) n += g->type[i] == e;
    return n;
}

void phys_blast(PhysGrid *g, int x, int y, int r) {
    if (g->nblasts < PHYS_MAX_BLASTS) g->blasts[g->nblasts++] = (PhysBlast){ x, y, r };
    /* Outer rings go first, so inner matter has somewhere to be thrown. */
    for (int ring = r; ring >= 1; ring--) {
        for (int j = -ring; j <= ring; j++)
            for (int i = -ring; i <= ring; i++) {
                int d2 = i * i + j * j;
                if (d2 > ring * ring || d2 <= (ring - 1) * (ring - 1)) continue;
                int ax = x + i, ay = y + j;
                if (!in_grid(g, ax, ay)) continue;
                int c = at(g, ax, ay), e = g->type[c];
                float k = 1 - (float)ring / (float)(r + 1);
                g->temp[c] += 900 * k;
                if (e == PE_WOOD || e == PE_ICE || e == PE_PLANT || e == PE_GLASS) {
                    if (chance(g, (unsigned)(700 * k))) { become(g, c, e == PE_WOOD ? PE_ASH : PE_EMPTY); e = g->type[c]; }
                }
                if (!loose(e)) continue;
                float d = sqrtf((float)d2);
                int reach = (int)((float)(r + 2 - ring) * 1.2f) + 1;
                int tx = ax, ty = ay;
                for (int s = 1; s <= reach; s++) {
                    int nx = ax + (int)roundf((float)i / d * (float)s), ny = ay + (int)roundf((float)j / d * (float)s);
                    if (!in_grid(g, nx, ny) || g->type[at(g, nx, ny)] != PE_EMPTY || g->occ[at(g, nx, ny)]) break;
                    tx = nx; ty = ny;
                }
                if (tx != ax || ty != ay) swap_cells(g, c, at(g, tx, ty));
            }
    }
    /* The space left behind fills with fire. */
    int inner = r / 2 + 1;
    for (int j = -inner; j <= inner; j++)
        for (int i = -inner; i <= inner; i++) {
            if (i * i + j * j > inner * inner || !in_grid(g, x + i, y + j)) continue;
            int c = at(g, x + i, y + j);
            if (g->type[c] == PE_EMPTY && !g->occ[c] && chance(g, 700)) put(g, c, PE_FIRE);
        }
}

/* ---- movement ---- */

/* Can a cell of element e move into this spot, swapping with what's there? */
static inline bool can_enter(const PhysGrid *g, int x, int y, int e) {
    if (!in_grid(g, x, y)) return false;
    int c = at(g, x, y);
    if (g->occ[c]) return false;
    int o = g->type[c];
    if (o == PE_EMPTY) return true;
    if (!loose(o)) return false;
    return INFO[e].density > INFO[o].density;
}

static void move_powder(PhysGrid *g, int x, int y, int e) {
    int dy = g->gravity;
    if (!dy) return;
    int i = at(g, x, y);
    bool moved = false;
    /* Up to three cells a tick, so a long fall has some speed. */
    for (int s = 0; s < 3; s++) {
        if (!can_enter(g, x, y + dy, e)) break;
        int n = at(g, x, y + dy);
        bool into_empty = g->type[n] == PE_EMPTY;
        swap_cells(g, i, n);
        y += dy;
        i = n;
        moved = true;
        if (!into_empty) return; /* sinking through a liquid is slow */
    }
    if (moved) return;
    int dir = (rnd(g) & 1) ? 1 : -1;
    if (e == PE_GRAVEL && chance(g, 500)) return; /* heavy, chunky piles stand steeper */
    for (int k = 0; k < 2; k++, dir = -dir)
        if (can_enter(g, x + dir, y + dy, e) && g->type[at(g, x + dir, y)] == PE_EMPTY) {
            swap_cells(g, i, at(g, x + dir, y + dy));
            return;
        }
}

static void move_liquid(PhysGrid *g, int x, int y, int e) {
    int dy = g->gravity, i = at(g, x, y);
    if (dy) {
        bool moved = false;
        for (int s = 0; s < 3; s++) {
            if (!can_enter(g, x, y + dy, e)) break;
            int n = at(g, x, y + dy);
            bool into_empty = g->type[n] == PE_EMPTY;
            swap_cells(g, i, n);
            y += dy;
            i = n;
            moved = true;
            if (!into_empty) return;
        }
        if (moved) return;
        int dir = (rnd(g) & 1) ? 1 : -1;
        for (int k = 0; k < 2; k++, dir = -dir)
            if (can_enter(g, x + dir, y + dy, e)) {
                swap_cells(g, i, at(g, x + dir, y + dy));
                return;
            }
    }
    /* Spreads sideways: thin liquids run, lava creeps. */
    int flow = e == PE_LAVA ? 1 : e == PE_OIL ? 4 : 7;
    if (e == PE_LAVA && chance(g, 600)) return;
    int dir = (rnd(g) & 1) ? 1 : -1, best = 0;
    for (int s = 1; s <= flow; s++) {
        if (!in_grid(g, x + dir * s, y)) break;
        int c = at(g, x + dir * s, y);
        /* Sideways only into air: pushing past another liquid never settles. */
        if (g->type[c] != PE_EMPTY || g->occ[c]) break;
        best = s;
        if (dy && can_enter(g, x + dir * s, y + dy, e)) break;
    }
    if (best) swap_cells(g, i, at(g, x + dir * best, y));
}

static void move_gas(PhysGrid *g, int x, int y, int e) {
    int up = g->gravity ? -g->gravity : 0, i = at(g, x, y);
    /* Rising is random enough to billow rather than march. */
    if (up && chance(g, 780)) {
        int dx = (int)(rnd(g) % 3) - 1;
        int tx = x + dx, ty = y + up;
        if (in_grid(g, tx, ty)) {
            int c = at(g, tx, ty);
            /* A gas climbs through liquid, as the liquid sinks past it. */
            if (!g->occ[c] && (g->type[c] == PE_EMPTY || (loose(g->type[c]) && INFO[g->type[c]].kind == PK_LIQUID))) {
                swap_cells(g, i, c);
                return;
            }
        }
    }
    int dir = (rnd(g) & 1) ? 1 : -1;
    int dy = up ? 0 : (int)(rnd(g) % 3) - 1;
    if (in_grid(g, x + dir, y + dy)) {
        int c = at(g, x + dir, y + dy);
        if (g->type[c] == PE_EMPTY && !g->occ[c]) swap_cells(g, i, c);
    }
    (void)e;
}

/* ---- reactions ---- */

static bool random_neighbour(PhysGrid *g, int x, int y, int *out) {
    unsigned r = rnd(g);
    int nx = x + (int)(r % 3) - 1, ny = y + (int)((r >> 4) % 3) - 1;
    if ((nx == x && ny == y) || !in_grid(g, nx, ny)) return false;
    *out = at(g, nx, ny);
    return true;
}

static void explode(PhysGrid *g, int x, int y, int r) {
    put(g, at(g, x, y), PE_FIRE);
    phys_blast(g, x, y, r);
}

static void ignite(PhysGrid *g, int x, int y) {
    int c = at(g, x, y), e = g->type[c];
    switch (e) {
    case PE_POWDER: explode(g, x, y, 5); break;
    case PE_METHANE: explode(g, x, y, 3); break;
    case PE_OIL: put(g, c, PE_FIRE); g->life[c] = (uint8_t)(60 + rnd(g) % 60); break;
    case PE_SEED: put(g, c, PE_FIRE); break;
    case PE_WOOD: case PE_PLANT: case PE_COAL:
        if (!(g->flags[c] & F_BURNING)) {
            g->flags[c] |= F_BURNING;
            g->life[c] = (uint8_t)(e == PE_COAL ? 220 : e == PE_WOOD ? 90 + rnd(g) % 60 : 12 + rnd(g) % 12);
        }
        break;
    default: break;
    }
}

static void update_burning(PhysGrid *g, int x, int y, int e) {
    int c = at(g, x, y), n;
    float hot = e == PE_COAL ? 1000.0f : 650.0f;
    if (g->temp[c] < hot) g->temp[c] = hot;
    if (random_neighbour(g, x, y, &n)) {
        if (g->type[n] == PE_EMPTY && !g->occ[n] && chance(g, 500)) put(g, n, PE_FIRE);
        else if (g->type[n] == PE_WATER) {
            /* Water puts it out, and is boiled for its trouble. */
            become(g, n, PE_STEAM);
            g->temp[n] = INFO[PE_STEAM].start_temp;
            g->flags[c] &= (uint8_t)~F_BURNING;
            g->temp[c] = 90;
            return;
        }
    }
    if (g->life[c] > 0) { g->life[c]--; return; }
    if (e == PE_WOOD || e == PE_COAL) become(g, c, chance(g, 600) ? PE_ASH : PE_FIRE);
    else become(g, c, chance(g, 150) ? PE_ASH : PE_FIRE);
}

/* Returns false when the cell is no longer there to move. */
static bool react(PhysGrid *g, int x, int y, int e) {
    int c = at(g, x, y), n;
    float t = g->temp[c];
    const PhysInfo *info = &INFO[e];

    if (info->ignite_at > 0 && t >= info->ignite_at) {
        ignite(g, x, y);
        if (g->type[c] != e) return false;
    }
    if (g->flags[c] & F_BURNING) {
        update_burning(g, x, y, e);
        return g->type[c] == e && info->kind != PK_SOLID;
    }

    switch (e) {
    case PE_HEATER: g->temp[c] = 1200; return false;
    case PE_COOLER: g->temp[c] = -60; return false;
    case PE_SPOUT:
        g->temp[c] = PHYS_AMBIENT;
        for (int k = 0; k < 4; k++) {
            if (!random_neighbour(g, x, y, &n)) continue;
            if (!g->life[c]) {
                int o = g->type[n];
                if (o != PE_EMPTY && o != PE_SPOUT && o != PE_DRAIN && o != PE_WALL) g->life[c] = (uint8_t)o;
            } else if (g->type[n] == PE_EMPTY && !g->occ[n]) {
                put(g, n, g->life[c]);
            }
        }
        /* Liquids have no pressure of their own, so a spout pumps: new matter
         * appears at the top of the column it's already filled. */
        if (g->life[c] && INFO[g->life[c]].kind == PK_LIQUID && chance(g, 800)) {
            int up = g->gravity >= 0 ? -1 : 1, ty = y + up;
            while (in_grid(g, x, ty) && g->type[at(g, x, ty)] == g->life[c]) ty += up;
            if (in_grid(g, x, ty) && g->type[at(g, x, ty)] == PE_EMPTY && !g->occ[at(g, x, ty)])
                put(g, at(g, x, ty), g->life[c]);
        }
        return false;
    case PE_DRAIN:
        for (int k = 0; k < 2; k++)
            if (random_neighbour(g, x, y, &n) && loose(g->type[n])) put(g, n, PE_EMPTY);
        return false;
    case PE_ICE:
        if (t > 4) { become(g, c, PE_WATER); return false; }
        if (random_neighbour(g, x, y, &n)) {
            if (g->type[n] == PE_WATER && g->temp[n] < 20 && chance(g, 12)) { become(g, n, PE_ICE); g->temp[n] = -5; }
            else if (g->type[n] == PE_SALT && chance(g, 80)) { become(g, c, PE_WATER); return false; }
        }
        return false;
    case PE_PLANT:
        if (random_neighbour(g, x, y, &n) && (g->type[n] == PE_WATER || g->type[n] == PE_BRINE) && chance(g, 200)) {
            put(g, n, PE_EMPTY);
            if (g->life[c] < 250) g->life[c] = (uint8_t)(g->life[c] + 24);
        }
        if (g->life[c] > 0 && chance(g, 40)) {
            /* Reaches against gravity, mostly straight, sometimes branching. */
            int up = g->gravity ? -g->gravity : -1;
            int dx = (int)(rnd(g) % 5) - 2, ty = y + (chance(g, 150) ? 0 : up);
            dx = dx > 1 ? 1 : dx < -1 ? -1 : dx;
            if (in_grid(g, x + dx, ty)) {
                int m = at(g, x + dx, ty);
                if (g->type[m] == PE_EMPTY && !g->occ[m]) {
                    put(g, m, PE_PLANT);
                    g->life[m] = (uint8_t)(g->life[c] / 2);
                    g->life[c] = (uint8_t)(g->life[c] / 2);
                }
            }
        }
        return false;
    case PE_SAND:
        if (t > 1100) { become(g, c, PE_GLASS); return false; }
        break;
    case PE_GRAVEL:
        if (t > 1300) { become(g, c, PE_LAVA); return false; }
        break;
    case PE_SALT:
        if (random_neighbour(g, x, y, &n) && g->type[n] == PE_WATER && chance(g, 80)) {
            become(g, n, PE_BRINE);
            put(g, c, PE_EMPTY);
            return false;
        }
        break;
    case PE_SNOW:
        if (t > 3) { become(g, c, PE_WATER); return false; }
        break;
    case PE_SEED: {
        int dy = g->gravity ? g->gravity : 1, below = phys_get(g, x, y + dy);
        if (below != PE_EMPTY && INFO[below].kind != PK_LIQUID && random_neighbour(g, x, y, &n)
            && (g->type[n] == PE_WATER || below == PE_SAND || below == PE_ASH) && chance(g, 40)) {
            if (g->type[n] == PE_WATER) put(g, n, PE_EMPTY);
            become(g, c, PE_PLANT);
            g->life[c] = 50;
            return false;
        }
        break;
    }
    case PE_WATER:
        if (t >= 100) { become(g, c, PE_STEAM); g->temp[c] = INFO[PE_STEAM].start_temp; return false; }
        if (t < 0) { become(g, c, PE_ICE); return false; }
        break;
    case PE_BRINE:
        if (t >= 110) { /* boiling leaves the salt behind */
            become(g, c, PE_SALT);
            if (random_neighbour(g, x, y, &n) && g->type[n] == PE_EMPTY) put(g, n, PE_STEAM);
            return false;
        }
        if (t < -15) { become(g, c, PE_ICE); return false; }
        break;
    case PE_ACID:
        if (random_neighbour(g, x, y, &n)) {
            int o = g->type[n];
            if (o != PE_EMPTY && o != PE_ACID && o != PE_WALL && o != PE_GLASS && o != PE_SMOKE && o != PE_HEATER
                && o != PE_COOLER && o != PE_SPOUT && o != PE_DRAIN && chance(g, 90)) {
                put(g, n, chance(g, 300) ? PE_SMOKE : PE_EMPTY);
                if (++g->life[c] >= 4) { put(g, c, PE_SMOKE); return false; }
            }
        }
        break;
    case PE_LAVA:
        if (t < 650) { become(g, c, PE_GRAVEL); return false; }
        if (random_neighbour(g, x, y, &n)) {
            int o = g->type[n];
            if (o == PE_WATER || o == PE_BRINE) {
                become(g, n, PE_STEAM);
                g->temp[n] = INFO[PE_STEAM].start_temp;
                g->temp[c] -= 120;
            } else if (o == PE_ICE || o == PE_SNOW) {
                become(g, n, PE_WATER);
                g->temp[n] = 60;
                g->temp[c] -= 40;
            }
        }
        /* Only radiates where it's exposed. A deep pool stays molten, and a thin
         * flow sets. Solid walls just take its heat by conduction. */
        if (random_neighbour(g, x, y, &n) && g->type[n] == PE_EMPTY) g->temp[c] -= 3.0f;
        break;
    case PE_FIRE:
        g->temp[c] = t > 800 ? t : 800;
        if (random_neighbour(g, x, y, &n)) {
            int o = g->type[n];
            if (o == PE_WATER || o == PE_BRINE) {
                become(g, n, PE_STEAM);
                g->temp[n] = INFO[PE_STEAM].start_temp;
                put(g, c, chance(g, 500) ? PE_SMOKE : PE_EMPTY);
                return false;
            }
            if (INFO[o].ignite_at > 0 && chance(g, 180)) ignite(g, n % g->w, n / g->w);
        }
        if (g->life[c] == 0) {
            put(g, c, chance(g, 250) ? PE_SMOKE : PE_EMPTY);
            return false;
        }
        g->life[c]--;
        break;
    case PE_SMOKE:
        if (g->life[c] == 0) { put(g, c, PE_EMPTY); return false; }
        g->life[c]--;
        break;
    case PE_STEAM:
        if (t < 92 && chance(g, 25)) { become(g, c, PE_WATER); g->temp[c] = 70; return false; }
        if (g->life[c] > 0) g->life[c]--;
        else { g->temp[c] -= 3; }
        break;
    default:
        break;
    }
    return true;
}

/* ---- heat ---- */

static void diffuse_heat(PhysGrid *g) {
    int w = g->w, h = g->h;
    /* Half the cells each tick, in a checkerboard. Heat flows between neighbours,
     * so it's conserved except where air quietly returns to room temperature. */
    int odd = g->tick & 1;
    for (int y = 0; y < h; y++) {
        for (int x = (y + odd) & 1; x < w; x += 2) {
            int c = y * w + x, ec = g->type[c];
            float cc = INFO[ec].conduct;
            if (x + 1 < w) {
                int n = c + 1;
                float k = cc < INFO[g->type[n]].conduct ? cc : INFO[g->type[n]].conduct;
                float f = (g->temp[n] - g->temp[c]) * k;
                g->temp[c] += f;
                g->temp[n] -= f;
            }
            if (y + 1 < h) {
                int n = c + w;
                float k = cc < INFO[g->type[n]].conduct ? cc : INFO[g->type[n]].conduct;
                float f = (g->temp[n] - g->temp[c]) * k;
                g->temp[c] += f;
                g->temp[n] -= f;
            }
            if (ec == PE_EMPTY) g->temp[c] += (PHYS_AMBIENT - g->temp[c]) * 0.08f;
            else g->temp[c] += (PHYS_AMBIENT - g->temp[c]) * 0.0006f;
        }
    }
}

/* ---- the tick ---- */

void phys_step(PhysGrid *g) {
    g->tick++;
    g->nblasts = 0;
    memset(g->counts, 0, sizeof g->counts);
    int w = g->w, h = g->h;
    int rows_down = g->gravity >= 0;
    /* Falling things are processed lowest first, so a column moves as a column. */
    for (int r = 0; r < h; r++) {
        int y = rows_down ? h - 1 - r : r;
        bool rightward = (g->tick + (unsigned)r) & 1;
        for (int q = 0; q < w; q++) {
            int x = rightward ? q : w - 1 - q;
            int c = y * w + x, e = g->type[c];
            if (e == PE_EMPTY || g->stamp[c] == g->tick) continue;
            g->counts[e]++;
            if (!react(g, x, y, e)) continue;
            /* The reaction might have changed it, or blown it somewhere. */
            if (g->type[c] != e || g->stamp[c] == g->tick) continue;
            switch (INFO[e].kind) {
            case PK_POWDER: move_powder(g, x, y, e); break;
            case PK_LIQUID: move_liquid(g, x, y, e); break;
            case PK_GAS: move_gas(g, x, y, e); break;
            default: break;
            }
        }
    }
    diffuse_heat(g);
}
