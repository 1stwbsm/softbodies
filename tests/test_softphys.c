/* SoftPhys behaviour tests. Each test states a physical expectation. */
#include "../src/softphys/softphys.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void run(PhysGrid *g, int ticks) { for (int i = 0; i < ticks; i++) phys_step(g); }

static void test_sand_falls_piles_and_is_conserved(void) {
    PhysGrid *g = phys_new(80, 60);
    phys_paint(g, 40, 10, 6, PE_SAND, true);
    long start = phys_total(g, PE_SAND);
    run(g, 400);
    CHECK(phys_total(g, PE_SAND) == start, "sand count %ld -> %ld", start, phys_total(g, PE_SAND));
    CHECK(phys_get(g, 40, 59) == PE_SAND, "nothing on the floor below the drop");
    CHECK(phys_get(g, 40, 10) == PE_EMPTY, "sand still hanging in the air");
    /* A pile: taller in the middle than at its edge. */
    int mid = 0, side = 0;
    for (int y = 0; y < 60; y++) {
        mid += phys_get(g, 40, y) == PE_SAND;
        side += phys_get(g, 40 + 10, y) == PE_SAND;
    }
    CHECK(mid > side && mid >= 3, "no pile: middle %d, side %d", mid, side);
    phys_free(g);
}

static void test_water_levels_out(void) {
    PhysGrid *g = phys_new(60, 40);
    phys_rect(g, 2, 2, 12, 30, PE_WATER);
    long start = phys_total(g, PE_WATER);
    run(g, 900);
    CHECK(phys_total(g, PE_WATER) == start, "water count %ld -> %ld", start, phys_total(g, PE_WATER));
    int lo = 99, hi = 0;
    for (int x = 0; x < 60; x++) {
        int n = 0;
        for (int y = 0; y < 40; y++) n += phys_get(g, x, y) == PE_WATER;
        lo = n < lo ? n : lo;
        hi = n > hi ? n : hi;
    }
    CHECK(hi - lo <= 2, "surface isn't level: columns from %d to %d", lo, hi);
    phys_free(g);
}

static void test_density_layers_oil_over_water_over_sand(void) {
    PhysGrid *g = phys_new(20, 40);
    phys_rect(g, 0, 0, 19, 4, PE_SAND);
    phys_rect(g, 0, 5, 19, 9, PE_OIL);
    phys_rect(g, 0, 10, 19, 14, PE_WATER);
    run(g, 1500);
    double oil = 0, water = 0, sand = 0, no = 0, nw = 0, ns = 0;
    for (int y = 0; y < 40; y++)
        for (int x = 0; x < 20; x++) {
            int e = phys_get(g, x, y);
            if (e == PE_OIL) { oil += y; no++; }
            if (e == PE_WATER) { water += y; nw++; }
            if (e == PE_SAND) { sand += y; ns++; }
        }
    oil /= no; water /= nw; sand /= ns;
    CHECK(oil < water && water < sand, "layers wrong: oil y %.1f, water y %.1f, sand y %.1f", oil, water, sand);
    phys_free(g);
}

static void test_lava_and_water_make_gravel_and_steam(void) {
    PhysGrid *g = phys_new(40, 30);
    phys_rect(g, 0, 26, 39, 29, PE_WATER);
    phys_rect(g, 15, 10, 24, 18, PE_LAVA);
    long steam = 0;
    for (int i = 0; i < 500; i++) {
        phys_step(g);
        if (phys_total(g, PE_STEAM) > steam) steam = phys_total(g, PE_STEAM);
    }
    run(g, 2500);
    CHECK(steam > 0, "no steam");
    CHECK(phys_total(g, PE_GRAVEL) > 0, "no gravel formed");
    CHECK(phys_total(g, PE_LAVA) == 0, "lava never cooled: %ld left", phys_total(g, PE_LAVA));
    phys_free(g);
}

static void test_water_boils_and_freezes_at_the_right_temperatures(void) {
    PhysGrid *g = phys_new(20, 20);
    phys_rect(g, 0, 15, 19, 19, PE_WATER);
    phys_heat(g, 10, 17, 30, 200);
    run(g, 5);
    CHECK(phys_total(g, PE_STEAM) > 0, "hot water didn't boil");
    phys_clear(g);
    phys_rect(g, 0, 15, 19, 19, PE_WATER);
    phys_heat(g, 10, 17, 30, -80);
    run(g, 5);
    CHECK(phys_total(g, PE_ICE) > 0, "cold water didn't freeze");
    phys_free(g);
}

static void test_wood_burns_to_ash_and_spreads(void) {
    PhysGrid *g = phys_new(60, 30);
    phys_rect(g, 5, 25, 54, 29, PE_WOOD);
    long start = phys_total(g, PE_WOOD);
    phys_set(g, 5, 24, PE_FIRE);
    phys_heat(g, 5, 25, 2, 800);
    run(g, 3000);
    CHECK(phys_total(g, PE_WOOD) < start / 5, "fire didn't spread: %ld of %ld wood left", phys_total(g, PE_WOOD), start);
    CHECK(phys_total(g, PE_ASH) > 0, "no ash");
    phys_free(g);
}

static void test_water_stops_fire(void) {
    PhysGrid *g = phys_new(30, 20);
    phys_rect(g, 0, 19, 29, 19, PE_WALL);
    phys_set(g, 15, 18, PE_FIRE);
    phys_rect(g, 10, 5, 20, 8, PE_WATER);
    run(g, 200);
    CHECK(phys_total(g, PE_FIRE) == 0, "fire survived a soaking: %ld", phys_total(g, PE_FIRE));
    phys_free(g);
}

static void test_powder_explodes_and_reports_it(void) {
    PhysGrid *g = phys_new(60, 40);
    phys_rect(g, 28, 30, 32, 39, PE_POWDER);
    phys_heat(g, 30, 35, 2, 400);
    bool blasted = false;
    for (int i = 0; i < 30 && !blasted; i++) {
        phys_step(g);
        blasted = g->nblasts > 0;
    }
    CHECK(blasted, "heated powder never exploded");
    run(g, 200);
    CHECK(phys_total(g, PE_POWDER) == 0, "chain reaction left %ld powder", phys_total(g, PE_POWDER));
    phys_free(g);
}

static void test_acid_eats_wood_but_not_glass_or_wall(void) {
    PhysGrid *g = phys_new(40, 30);
    phys_rect(g, 0, 29, 39, 29, PE_WALL);
    phys_rect(g, 0, 26, 39, 28, PE_GLASS);
    phys_rect(g, 10, 23, 29, 25, PE_WOOD);
    long wood = phys_total(g, PE_WOOD), glass = phys_total(g, PE_GLASS), wall = phys_total(g, PE_WALL);
    phys_rect(g, 10, 15, 29, 22, PE_ACID);
    run(g, 800);
    CHECK(phys_total(g, PE_WOOD) < wood / 2, "acid barely touched wood: %ld of %ld", phys_total(g, PE_WOOD), wood);
    CHECK(phys_total(g, PE_GLASS) == glass, "glass was eaten: %ld -> %ld", glass, phys_total(g, PE_GLASS));
    CHECK(phys_total(g, PE_WALL) == wall, "wall was eaten");
    phys_free(g);
}

static void test_salt_makes_brine_which_sinks(void) {
    PhysGrid *g = phys_new(20, 30);
    phys_rect(g, 0, 20, 19, 29, PE_WATER);
    phys_rect(g, 5, 15, 14, 16, PE_SALT);
    run(g, 800);
    CHECK(phys_total(g, PE_BRINE) > 0, "no brine");
    double brine = 0, water = 0, nb = 0, nw = 0;
    for (int y = 0; y < 30; y++)
        for (int x = 0; x < 20; x++) {
            if (phys_get(g, x, y) == PE_BRINE) { brine += y; nb++; }
            if (phys_get(g, x, y) == PE_WATER) { water += y; nw++; }
        }
    CHECK(brine / nb > water / nw, "brine floats: brine y %.1f, water y %.1f", brine / nb, water / nw);
    phys_free(g);
}

static void test_heat_flows_through_metal_not_wall(void) {
    PhysGrid *g = phys_new(40, 6);
    phys_rect(g, 0, 1, 39, 1, PE_METAL);
    phys_rect(g, 0, 4, 39, 4, PE_WALL);
    phys_set(g, 0, 1, PE_HEATER);
    phys_set(g, 0, 4, PE_HEATER);
    run(g, 2500);
    CHECK(phys_temp(g, 20, 1) > 80, "metal stayed cold: %.1f", (double)phys_temp(g, 20, 1));
    CHECK(fabsf(phys_temp(g, 20, 4) - PHYS_AMBIENT) < 2, "wall conducted: %.1f", (double)phys_temp(g, 20, 4));
    phys_free(g);
}

static void test_spout_copies_and_drain_swallows(void) {
    PhysGrid *g = phys_new(30, 30);
    phys_set_spout(g, 15, 2, PE_WATER);
    phys_rect(g, 0, 29, 29, 29, PE_DRAIN);
    run(g, 300);
    long water = phys_total(g, PE_WATER);
    CHECK(water > 0, "spout gave nothing");
    run(g, 2000);
    /* Water reaches the drain and is swallowed, so it can't fill the whole box. */
    long held = phys_total(g, PE_WATER);
    run(g, 1000);
    CHECK(phys_total(g, PE_WATER) < 30 * 28, "the box flooded despite the drain: %ld", phys_total(g, PE_WATER));
    CHECK(labs(phys_total(g, PE_WATER) - held) < 200, "level kept climbing: %ld -> %ld", held, phys_total(g, PE_WATER));
    phys_free(g);
}

static void test_liquid_spout_pumps_up_a_shaft(void) {
    PhysGrid *g = phys_new(21, 60);
    for (int x = 0; x < 21; x++) if (x < 8 || x > 12) phys_rect(g, x, 20, x, 59, PE_ROCK);
    for (int x = 8; x <= 12; x++) phys_set_spout(g, x, 58, PE_WATER);
    int top = 59;
    for (int i = 0; i < 1500 && top > 19; i++) {
        phys_step(g);
        for (int y = 0; y < 60; y++) if (phys_get(g, 10, y) == PE_WATER) { top = y < top ? y : top; break; }
    }
    CHECK(top <= 20, "water only reached y=%d in a shaft that tops out at 20", top);
    phys_free(g);
}

static void test_occupied_cells_block_movement(void) {
    PhysGrid *g = phys_new(10, 30);
    for (int x = 0; x < 10; x++) g->occ[20 * 10 + x] = 1;
    phys_rect(g, 0, 2, 9, 4, PE_SAND);
    run(g, 200);
    CHECK(phys_total(g, PE_SAND) > 0 && phys_get(g, 5, 29) == PE_EMPTY, "sand passed through occupied cells");
    CHECK(phys_get(g, 5, 19) == PE_SAND, "sand didn't rest on the occupied row");
    phys_free(g);
}

static void test_resize_keeps_the_floor(void) {
    PhysGrid *g = phys_new(30, 30);
    phys_rect(g, 0, 28, 29, 29, PE_WALL);
    phys_set(g, 5, 27, PE_SAND);
    phys_resize(g, 50, 60);
    CHECK(phys_get(g, 5, 59) == PE_WALL && phys_get(g, 5, 57) == PE_SAND, "grew wrong");
    CHECK(phys_get(g, 40, 59) == PE_EMPTY, "new area isn't empty");
    phys_resize(g, 10, 5);
    CHECK(phys_get(g, 5, 4) == PE_WALL && phys_get(g, 5, 2) == PE_SAND, "shrank wrong");
    phys_step(g);
    phys_free(g);
}

static void test_flood_fill_stays_inside(void) {
    PhysGrid *g = phys_new(30, 30);
    phys_rect(g, 10, 10, 20, 10, PE_WALL);
    phys_rect(g, 10, 20, 20, 20, PE_WALL);
    phys_rect(g, 10, 10, 10, 20, PE_WALL);
    phys_rect(g, 20, 10, 20, 20, PE_WALL);
    phys_flood(g, 15, 15, PE_WATER);
    CHECK(phys_total(g, PE_WATER) == 9 * 9, "filled %ld cells", phys_total(g, PE_WATER));
    CHECK(phys_get(g, 5, 5) == PE_EMPTY, "leaked out");
    phys_free(g);
}

static void test_names_round_trip(void) {
    for (int e = 0; e < PE_COUNT; e++) {
        CHECK(phys_info(e)->name != NULL, "element %d has no name", e);
        CHECK(phys_element_by_name(phys_info(e)->name) == e, "%s doesn't resolve", phys_info(e)->name);
    }
    CHECK(phys_element_by_name("water") == PE_WATER, "names should be case-insensitive");
    CHECK(phys_element_by_name("nope") == -1, "unknown name resolved");
}

static void test_soak_every_element_everywhere_stays_sane(void) {
    PhysGrid *g = phys_new(120, 80);
    srand(11);
    for (int i = 0; i < 300; i++) {
        phys_paint(g, rand() % 120, rand() % 80, 1 + rand() % 5, 1 + rand() % (PE_COUNT - 1), true);
        run(g, 5);
        if (i % 40 == 0) phys_blast(g, rand() % 120, rand() % 80, 10);
        if (i % 70 == 0) g->gravity = (rand() % 3) - 1;
    }
    bool sane = true;
    for (int i = 0; i < 120 * 80; i++)
        if (g->type[i] >= PE_COUNT || !isfinite(g->temp[i]) || fabsf(g->temp[i]) > 20000) sane = false;
    CHECK(sane, "bad cell state after soak");
    phys_free(g);
}

int main(void) {
    struct { const char *name; void (*fn)(void); } tests[] = {
#define T(n) { #n, n }
        T(test_sand_falls_piles_and_is_conserved),
        T(test_water_levels_out),
        T(test_density_layers_oil_over_water_over_sand),
        T(test_lava_and_water_make_gravel_and_steam),
        T(test_water_boils_and_freezes_at_the_right_temperatures),
        T(test_wood_burns_to_ash_and_spreads),
        T(test_water_stops_fire),
        T(test_powder_explodes_and_reports_it),
        T(test_acid_eats_wood_but_not_glass_or_wall),
        T(test_salt_makes_brine_which_sinks),
        T(test_heat_flows_through_metal_not_wall),
        T(test_spout_copies_and_drain_swallows),
        T(test_liquid_spout_pumps_up_a_shaft),
        T(test_occupied_cells_block_movement),
        T(test_resize_keeps_the_floor),
        T(test_flood_fill_stays_inside),
        T(test_names_round_trip),
        T(test_soak_every_element_everywhere_stays_sane),
    };
    int n = sizeof tests / sizeof *tests;
    for (int i = 0; i < n; i++) {
        int before = failures;
        tests[i].fn();
        printf("%s %s\n", failures == before ? "ok  " : "FAIL", tests[i].name);
    }
    printf("%d tests, %d failed checks\n", n, failures);
    return failures ? 1 : 0;
}
