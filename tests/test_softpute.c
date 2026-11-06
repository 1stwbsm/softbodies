/* SoftPute behaviour tests. Each test states a physical expectation. */
#include "../src/softpute/softpute.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void run(SpWorld *w, float seconds) {
    int n = (int)(seconds * 60);
    for (int i = 0; i < n; i++) sp_step(w, 1.0f / 60);
}

static bool all_finite(const SpWorld *w) {
    for (int i = 0; i < w->np; i++)
        if (!isfinite(w->x[i]) || !isfinite(w->y[i]) || !isfinite(w->vx[i])) return false;
    return true;
}

static bool all_in_bounds(const SpWorld *w, float slack) {
    for (int i = 0; i < w->np; i++)
        if (w->x[i] < w->bx0 - slack || w->x[i] > w->bx1 + slack ||
            w->y[i] < w->by0 - slack || w->y[i] > w->by1 + slack) return false;
    return true;
}

static int add_square(SpWorld *w, float cx, float cy, float half) {
    float p[8] = { cx - half, cy - half, cx + half, cy - half, cx + half, cy + half, cx - half, cy + half };
    return sp_add_polygon(w, p, 4, NULL);
}

static int add_circle(SpWorld *w, float cx, float cy, float r) {
    float p[64];
    for (int i = 0; i < 32; i++) {
        p[i * 2] = cx + cosf(i * 6.2831853f / 32) * r;
        p[i * 2 + 1] = cy + sinf(i * 6.2831853f / 32) * r;
    }
    return sp_add_polygon(w, p, 32, NULL);
}

static void test_free_fall_matches_gravity(void) {
    SpWorld *w = sp_world_new();
    sp_set_bounds(w, 0, 0, 2000, 100000, true);
    w->air = 0;
    int s = add_circle(w, 1000, 100, 30);
    float y0, yt, x;
    sp_body_centroid(w, &w->bodies[s], &x, &y0);
    run(w, 1.0f);
    sp_body_centroid(w, &w->bodies[s], &x, &yt);
    float expected = 0.5f * w->gy;
    CHECK(fabsf((yt - y0) - expected) < expected * 0.03f, "fell %.1f, expected %.1f", yt - y0, expected);
    sp_world_free(w);
}

static void test_jelly_rests_on_floor_and_keeps_shape(void) {
    SpWorld *w = sp_world_new();
    sp_set_bounds(w, 0, 0, 800, 600, true);
    int s = add_square(w, 400, 300, 40);
    run(w, 4);
    SpBody *b = &w->bodies[s];
    float cx, cy;
    sp_body_centroid(w, b, &cx, &cy);
    CHECK(all_finite(w), "non finite state");
    CHECK(all_in_bounds(w, 1), "left the arena");
    CHECK(cy > 540 && cy < 575, "resting centre y=%.1f", cy);
    CHECK(w->kinetic < 40, "still moving, ke=%.1f", w->kinetic);
    CHECK(b->maxx - b->minx > 70 && b->maxx - b->minx < 105, "width collapsed to %.1f", b->maxx - b->minx);
    sp_world_free(w);
}

static void test_stack_does_not_interpenetrate(void) {
    SpWorld *w = sp_world_new();
    sp_set_bounds(w, 0, 0, 800, 600, true);
    /* Placed almost touching: hard off-centre drops may rightly topple. */
    int a = add_square(w, 400, 555, 40);
    int b = add_square(w, 400, 465, 40);
    int c = add_square(w, 400, 375, 40);
    run(w, 5);
    float x, ya, yb, yc;
    sp_body_centroid(w, &w->bodies[a], &x, &ya);
    sp_body_centroid(w, &w->bodies[b], &x, &yb);
    sp_body_centroid(w, &w->bodies[c], &x, &yc);
    CHECK(ya > yb && yb > yc, "order changed: %.1f %.1f %.1f", ya, yb, yc);
    CHECK(ya - yb > 55 && yb - yc > 55, "merged: gaps %.1f %.1f", ya - yb, yb - yc);
    sp_world_free(w);
}

static void test_fast_body_does_not_tunnel_through_jelly(void) {
    SpWorld *w = sp_world_new();
    sp_set_bounds(w, 0, 0, 800, 800, true);
    int big = add_square(w, 400, 700, 70);
    int small = add_circle(w, 400, 200, 24);
    for (int i = w->bodies[small].first; i < w->bodies[small].first + w->bodies[small].count; i++)
        w->vy[i] = 2200;
    run(w, 3);
    float x, yb, ys;
    sp_body_centroid(w, &w->bodies[big], &x, &yb);
    sp_body_centroid(w, &w->bodies[small], &x, &ys);
    CHECK(ys < yb, "small body ended below big body: %.1f vs %.1f", ys, yb);
    sp_world_free(w);
}

static void test_balloon_holds_area_and_overpressure_bursts(void) {
    SpWorld *w = sp_world_new();
    sp_set_bounds(w, 0, 0, 800, 600, true);
    int s = sp_add_balloon(w, 400, 300, 50, NULL);
    float area0 = w->bodies[s].area0;
    run(w, 3);
    float a = sp_body_area(w, &w->bodies[s]);
    CHECK(fabsf(a) > area0 * 0.75f && fabsf(a) < area0 * 1.25f, "area %.0f vs rest %.0f", a, area0);
    CHECK(!w->bodies[s].popped, "burst at normal pressure");
    w->pressure = 3.5f;
    int pops = 0;
    for (int i = 0; i < 240 && !pops; i++) {
        sp_step(w, 1.0f / 60);
        for (int e = 0; e < w->nevents; e++) pops += w->events[e].kind == SP_EVENT_POP;
    }
    CHECK(pops == 1, "pop events: %d", pops);
    CHECK(w->bodies[s].popped, "survived 3.5x pressure");
    sp_world_free(w);
}

static void test_cloth_hangs_and_blade_cuts(void) {
    SpWorld *w = sp_world_new();
    sp_set_bounds(w, 0, 0, 800, 800, true);
    int s = sp_add_cloth(w, 200, 50, 20, 14, NULL, 3);
    int links = sp_count_links(w);
    run(w, 2);
    CHECK(all_finite(w), "non finite");
    CHECK(sp_count_links(w) == links, "tore under its own weight: %d -> %d", links, sp_count_links(w));
    SpBody *b = &w->bodies[s];
    CHECK(b->maxy < 50 + 14 * 11 * 1.35f, "overstretched, max y=%.1f", b->maxy);
    sp_cut(w, 150, b->miny + 60, 650, b->miny + 60);
    CHECK(sp_count_links(w) < links, "blade cut nothing");
    sp_world_free(w);
}

static void test_cloth_tears_under_load(void) {
    SpWorld *w = sp_world_new();
    sp_set_bounds(w, 0, 0, 800, 800, true);
    sp_add_cloth(w, 200, 50, 20, 14, NULL, 3);
    int before = sp_count_links(w);
    sp_grab_begin(w, 310, 190, 40);
    sp_grab_move(w, 310, 790);
    run(w, 2);
    sp_grab_end(w);
    CHECK(sp_count_links(w) < before, "yanking did not tear");
    sp_world_free(w);
}

static void test_rope_pinned_swings_below_pin(void) {
    SpWorld *w = sp_world_new();
    sp_set_bounds(w, 0, 0, 800, 800, true);
    int s = sp_add_rope(w, 200, 100, 500, 100, NULL, true);
    run(w, 6);
    SpBody *b = &w->bodies[s];
    int last = b->first + b->count - 1;
    CHECK(w->x[b->first] == 200 && w->y[b->first] == 100, "pin moved");
    CHECK(w->y[last] > 250, "end did not swing down: %.1f", w->y[last]);
    sp_world_free(w);
}

static void test_goo_spreads_without_collapsing_or_exploding(void) {
    SpWorld *w = sp_world_new();
    sp_set_bounds(w, 0, 0, 400, 400, true);
    int s = sp_add_goo(w, 200, 200, 60, 300);
    run(w, 5);
    SpBody *b = &w->bodies[s];
    CHECK(all_finite(w), "non finite");
    CHECK(all_in_bounds(w, 1), "left arena");
    CHECK(b->maxy - b->miny < 200, "flying everywhere, height %.1f", b->maxy - b->miny);
    /* Volume: 300 particles should not condense to a thin sliver. */
    CHECK(b->maxy - b->miny > 15, "collapsed to height %.1f", b->maxy - b->miny);
    CHECK(w->kinetic < 4000, "boiling, ke=%.1f", w->kinetic);
    sp_world_free(w);
}

static void test_static_box_carries_and_pushes(void) {
    SpWorld *w = sp_world_new();
    sp_set_bounds(w, 0, 0, 800, 800, true);
    int box = sp_add_box(w, 300, 400, 500, 450, 0);
    int s = add_circle(w, 400, 300, 30);
    run(w, 2);
    float x, y;
    sp_body_centroid(w, &w->bodies[s], &x, &y);
    CHECK(y < 400 && y > 330, "should rest on box, y=%.1f", y);
    /* Lift the box quickly: the jelly must rise with it, not fall through. */
    for (int i = 0; i < 60; i++) {
        float top = 400 - i * 3.0f;
        sp_static_move(w, box, 300, top, 500, top + 50);
        sp_step(w, 1.0f / 60);
    }
    sp_body_centroid(w, &w->bodies[s], &x, &y);
    CHECK(y < 225, "not carried up, y=%.1f", y);
    sp_world_free(w);
}

static void test_peg_does_not_enter_jelly(void) {
    SpWorld *w = sp_world_new();
    sp_set_bounds(w, 0, 0, 800, 800, true);
    int s = add_circle(w, 400, 200, 40);
    sp_add_capsule(w, 400, 500, 400, 500, 6, 0);
    run(w, 3);
    CHECK(sp_body_at(w, 400, 500) != s, "peg ended inside the body");
    sp_world_free(w);
}

static void test_shrinking_walls_squeeze_without_escape(void) {
    SpWorld *w = sp_world_new();
    sp_set_bounds(w, 0, 0, 800, 600, true);
    for (int i = 0; i < 6; i++) add_circle(w, 150 + i * 100, 400, 40);
    run(w, 1);
    sp_set_bounds(w, 200, 200, 600, 600, false);
    run(w, 3);
    CHECK(all_finite(w), "non finite");
    CHECK(all_in_bounds(w, 2), "escaped through a moving wall");
    sp_world_free(w);
}

static void test_removal_keeps_other_bodies_consistent(void) {
    SpWorld *w = sp_world_new();
    int a = add_circle(w, 200, 200, 30);
    int b = add_square(w, 400, 200, 30);
    int c = sp_add_rope(w, 500, 50, 700, 50, NULL, true);
    run(w, 0.5f);
    float bx, by;
    sp_body_centroid(w, &w->bodies[b], &bx, &by);
    sp_body_remove(w, a);
    float bx2, by2;
    sp_body_centroid(w, &w->bodies[b], &bx2, &by2);
    CHECK(fabsf(bx - bx2) < 0.001f && fabsf(by - by2) < 0.001f, "survivor moved on removal");
    for (int i = 0; i < w->nlinks; i++) {
        CHECK(w->links[i].a < w->np && w->links[i].b < w->np, "stale link index");
        CHECK(w->body[w->links[i].a] == w->links[i].body, "link owner mismatch");
    }
    int s = add_circle(w, 100, 100, 20);
    CHECK(s == a, "slot was not reused");
    run(w, 1);
    CHECK(all_finite(w), "non finite after reuse");
    CHECK(w->bodies[c].alive, "rope lost");
    sp_world_free(w);
}

static void test_vaporize_only_affects_matter_in_range(void) {
    SpWorld *w = sp_world_new();
    sp_set_bounds(w, 0, 0, 1000, 600, true);
    int near_jelly = add_circle(w, 400, 300, 30);
    int far_jelly = add_circle(w, 900, 300, 30);
    int balloon = sp_add_balloon(w, 300, 300, 30, NULL);
    int splats = sp_vaporize(w, 350, 300, 150);
    CHECK(splats == 1, "splats: %d", splats);
    CHECK(w->nevents == 2, "events: %d", w->nevents);
    CHECK(w->bodies[balloon].popped, "balloon in range survived");
    int splat_body = -1;
    for (int e = 0; e < w->nevents; e++)
        if (w->events[e].kind == SP_EVENT_SPLAT) splat_body = w->events[e].body;
    CHECK(splat_body == near_jelly, "wrong jelly splatted: %d", splat_body);
    CHECK(far_jelly != splat_body, "far jelly splatted");
    sp_world_free(w);
}

static void test_shatter_cuts_cloth_and_pulls_pins_in_range_only(void) {
    SpWorld *w = sp_world_new();
    sp_set_bounds(w, 0, 0, 1000, 800, true);
    int cloth = sp_add_cloth(w, 100, 100, 10, 8, NULL, 1);
    int rope = sp_add_rope(w, 800, 100, 950, 100, NULL, true);
    int jelly = add_circle(w, 150, 150, 30);
    int before = sp_count_links(w);
    int jelly_links = 0;
    for (int i = 0; i < w->nlinks; i++) jelly_links += w->links[i].body == jelly;
    sp_shatter(w, 150, 120, 80);
    CHECK(sp_count_links(w) < before, "nothing broke");
    int alive = 0;
    for (int i = 0; i < w->nlinks; i++) alive += w->links[i].body == jelly && !w->links[i].dead;
    CHECK(alive == jelly_links, "jelly links were cut: %d of %d", alive, jelly_links);
    CHECK(w->w[w->bodies[cloth].first] > 0, "pin in range was not pulled");
    CHECK(w->w[w->bodies[rope].first] == 0, "pin out of range was pulled");
    sp_world_free(w);
}

static void test_shockwave_front_pushes_outward_once(void) {
    SpWorld *w = sp_world_new();
    sp_set_bounds(w, 0, 0, 2000, 2000, true);
    w->gy = 0;
    int s = add_circle(w, 1300, 1000, 30);
    SpBody *b = &w->bodies[s];
    float total = 0;
    for (float r = 0; r < 1500; r += 25) sp_shockwave(w, 1000, 1000, r, r + 25, 1000, 100);
    for (int i = b->first; i < b->first + b->count; i++) total += w->vx[i];
    float mean = total / b->count, expected = 1000 / (1 + 300.0f / 100);
    CHECK(fabsf(mean - expected) < expected * 0.15f, "mean push %.1f, expected about %.1f", mean, expected);
    sp_world_free(w);
}

/* A pretend medium: solid below y=400, and a buoyant fluid above that from y=200. */
static bool test_medium(void *user, float x, float y, SpMediumSample *out) {
    (void)user; (void)x;
    if (y >= 400) { out->push_y = -1.5f; out->friction = 1; return true; }
    if (y >= 200) { out->accel_y = -1800; out->drag = 5; return true; }
    return false;
}

static void test_medium_supports_and_floats_bodies(void) {
    SpWorld *w = sp_world_new();
    sp_set_bounds(w, 0, 0, 800, 800, true);
    w->medium = test_medium;
    int s = add_circle(w, 400, 100, 30);
    run(w, 4);
    float x, y;
    sp_body_centroid(w, &w->bodies[s], &x, &y);
    CHECK(y > 150 && y < 260, "should float at the fluid's surface, y=%.1f", y);
    /* With no lift, it should stop on the solid instead. */
    w->medium = NULL;
    run(w, 0.1f);
    sp_world_free(w);
}

static void test_soak_everything_stays_finite(void) {
    SpWorld *w = sp_world_new();
    sp_set_bounds(w, 0, 0, 1000, 700, true);
    srand(7);
    for (int i = 0; i < 12; i++) add_circle(w, 80 + i * 70, 100 + (i % 3) * 100, 28);
    for (int i = 0; i < 6; i++) sp_add_balloon(w, 100 + i * 140, 400, 34, NULL);
    sp_add_cloth(w, 200, 20, 24, 12, NULL, 4);
    sp_add_rope(w, 50, 30, 400, 30, NULL, true);
    sp_add_goo(w, 700, 200, 70, 300);
    for (int i = 0; i < 600; i++) {
        if (i % 40 == 0) sp_blast(w, rand() % 1000, rand() % 700, 200, 900);
        if (i % 90 == 0) w->gy = (rand() % 3 - 1) * 900.0f;
        sp_step(w, 1.0f / 60);
        if (!all_finite(w)) { CHECK(false, "non finite at frame %d", i); break; }
    }
    CHECK(all_in_bounds(w, 2), "something escaped");
    sp_world_free(w);
}

int main(void) {
    struct { const char *name; void (*fn)(void); } tests[] = {
#define T(n) { #n, n }
        T(test_free_fall_matches_gravity),
        T(test_jelly_rests_on_floor_and_keeps_shape),
        T(test_stack_does_not_interpenetrate),
        T(test_fast_body_does_not_tunnel_through_jelly),
        T(test_balloon_holds_area_and_overpressure_bursts),
        T(test_cloth_hangs_and_blade_cuts),
        T(test_cloth_tears_under_load),
        T(test_rope_pinned_swings_below_pin),
        T(test_goo_spreads_without_collapsing_or_exploding),
        T(test_static_box_carries_and_pushes),
        T(test_peg_does_not_enter_jelly),
        T(test_shrinking_walls_squeeze_without_escape),
        T(test_removal_keeps_other_bodies_consistent),
        T(test_vaporize_only_affects_matter_in_range),
        T(test_shatter_cuts_cloth_and_pulls_pins_in_range_only),
        T(test_shockwave_front_pushes_outward_once),
        T(test_medium_supports_and_floats_bodies),
        T(test_soak_everything_stays_finite),
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
