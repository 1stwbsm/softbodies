/* SoftPhys - a cellular powder, fluid, gas and heat simulation.
 *
 * Every cell holds one element and a temperature. Each tick, cells move by their
 * kind (powders pile, liquids level, gases rise), heat diffuses, and elements react
 * with their neighbours. No dependencies beyond libc and libm, and deterministic for
 * a given seed.
 */
#ifndef SOFTPHYS_H
#define SOFTPHYS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PE_EMPTY,
    /* solids */
    PE_WALL, PE_ROCK, PE_METAL, PE_WOOD, PE_ICE, PE_PLANT, PE_GLASS, PE_HEATER, PE_COOLER, PE_SPOUT, PE_DRAIN,
    /* powders */
    PE_SAND, PE_SALT, PE_ASH, PE_COAL, PE_POWDER, PE_SEED, PE_SNOW, PE_GRAVEL,
    /* liquids */
    PE_WATER, PE_BRINE, PE_OIL, PE_ACID, PE_LAVA,
    /* gases */
    PE_STEAM, PE_SMOKE, PE_FIRE, PE_METHANE,
    PE_COUNT
} PhysElement;

typedef enum { PK_EMPTY, PK_SOLID, PK_POWDER, PK_LIQUID, PK_GAS } PhysKind;

typedef struct {
    const char *name;
    const char *about;
    PhysKind kind;
    float density;      /* heavier sinks through lighter */
    float conduct;      /* 0..0.24 heat passed per tick */
    float start_temp;
    float ignite_at;    /* 0 = never burns */
} PhysInfo;

typedef struct { int x, y, r; } PhysBlast;
#define PHYS_MAX_BLASTS 32
#define PHYS_AMBIENT 22.0f

typedef struct {
    int w, h;
    uint8_t *type, *life, *flags, *stamp;
    uint8_t *occ;       /* cells taken by something outside the grid, such as a soft body */
    float *temp;
    uint8_t tick;
    unsigned rng;
    int gravity;        /* 1 down, -1 up, 0 none */
    long counts[PE_COUNT];
    PhysBlast blasts[PHYS_MAX_BLASTS]; /* output: explosions since the last step */
    int nblasts;
} PhysGrid;

const PhysInfo *phys_info(int element);
int phys_element_by_name(const char *name); /* -1 when unknown, case-insensitive */

PhysGrid *phys_new(int w, int h);
void phys_free(PhysGrid *g);
void phys_clear(PhysGrid *g);
void phys_resize(PhysGrid *g, int w, int h); /* keeps the contents, anchored to the floor */
void phys_step(PhysGrid *g);

int phys_get(const PhysGrid *g, int x, int y); /* PE_WALL outside the grid */
float phys_temp(const PhysGrid *g, int x, int y);
void phys_set(PhysGrid *g, int x, int y, int element);
void phys_paint(PhysGrid *g, int x, int y, int radius, int element, bool overwrite);
void phys_line(PhysGrid *g, int x0, int y0, int x1, int y1, int radius, int element, bool overwrite);
void phys_rect(PhysGrid *g, int x0, int y0, int x1, int y1, int element);
void phys_flood(PhysGrid *g, int x, int y, int element);
void phys_heat(PhysGrid *g, int x, int y, int radius, float amount);
void phys_push(PhysGrid *g, int x, int y, int radius, int dx, int dy);
void phys_blast(PhysGrid *g, int x, int y, int radius);
/* Throws everything loose in the grid by up to this many cells, as far as it can go. */
void phys_shove(PhysGrid *g, int dx, int dy);
void phys_set_spout(PhysGrid *g, int x, int y, int element);
long phys_total(const PhysGrid *g, int element);

#endif
