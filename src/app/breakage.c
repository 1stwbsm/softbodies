/* The window as a thing in the world.
 *
 * It's a box. Move it and what's inside gets thrown about. Hit one of its walls hard
 * enough and the wall breaks, and matter can leave. Outside, it falls down the screen
 * onto the taskbar. Inner windows break the same way: bodies can get in, and their
 * widgets can get knocked off, still working, wherever they land.
 */
#include "app.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Speeds are world units per second, and impulses are speed over the minimum times
 * mass, summed over a step. Falling the height of a big window arrives at about 1300,
 * and a few hits like that shouldn't break anything. */
#define WALL_MIN_SPEED 1500.0f
#define WALL_BREAK 4000.0f
#define PANEL_MIN_SPEED 1150.0f
#define PANEL_BREAK 3000.0f
#define ELEM_MIN_SPEED 260.0f
#define ELEM_LOOSEN 60.0f
#define FRAME_THICK 4.0f
#define MAX_FRAME_SPEED 5000.0f
#define MAX_FRAME_KICK 2600.0f
#define SHAKE_LOOSENS 1100.0f

static inline int world_index(const App *a) { return a->world == a->world_phys; }

bool brk_can_escape(const App *a) {
    const SwWindow *win = a->win;
    return win->has_pos && win->has_desk && win->ops->outer_present && !a->outer_failed;
}

/* ---- the window, moving ------------------------------------------------- */

static void set_desk(App *a, bool snap) {
    const SwWindow *win = a->win;
    if (!win->has_desk || !win->has_pos) return;
    float ox = (float)(win->sx + a->ax), oy = (float)(win->sy + a->ay);
    sp_set_desk(a->world, ((float)win->desk_x0 - ox) / a->vs, ((float)win->desk_y0 - oy) / a->vs,
                ((float)win->desk_x1 - ox) / a->vs, ((float)win->desk_y1 - oy) / a->vs, snap);
}

/* Other windows on the desktop are things to land on. */
static void sync_outside(App *a) {
    const SwWindow *win = a->win;
    int *ids = a->outside_st[world_index(a)];
    SpWorld *w = a->world;
    float ox = (float)(win->sx + a->ax), oy = (float)(win->sy + a->ay);
    for (int i = 0; i < SW_MAX_OBSTACLES; i++) {
        if (i >= win->nobstacles) {
            if (ids[i] >= 0) sp_static_remove(w, ids[i]);
            ids[i] = -1;
            continue;
        }
        float x0 = ((float)win->obstacles[i].x - ox) / a->vs, y0 = ((float)win->obstacles[i].y - oy) / a->vs;
        float x1 = x0 + (float)win->obstacles[i].w / a->vs, y1 = y0 + (float)win->obstacles[i].h / a->vs;
        if (ids[i] < 0) {
            ids[i] = sp_add_box(w, x0, y0, x1, y1, TAG_OUTSIDE);
            if (ids[i] >= 0) w->statics[ids[i]].outside_only = true;
        } else {
            sp_static_move(w, ids[i], x0, y0, x1, y1);
        }
    }
}

/* Cells have no momentum of their own, so the grid remembers how it's been thrown
 * as a whole, and that wears off quickly as its contents pile up against the walls. */
static void slosh_grid(App *a, float kick_x, float kick_y, float dt) {
    PhysUi *u = &a->phys;
    if (!u->g) return;
    u->slosh_vx -= kick_x;
    u->slosh_vy -= kick_y;
    float wpc = (float)u->cell / a->vs;
    u->slosh_x += u->slosh_vx * dt / wpc;
    u->slosh_y += u->slosh_vy * dt / wpc;
    int ix = (int)u->slosh_x, iy = (int)u->slosh_y;
    if (ix || iy) {
        u->slosh_x -= (float)ix;
        u->slosh_y -= (float)iy;
        ix = ix > 12 ? 12 : ix < -12 ? -12 : ix;
        iy = iy > 12 ? 12 : iy < -12 ? -12 : iy;
        phys_shove(u->g, ix, iy);
    }
    u->slosh_vx *= 0.9f;
    u->slosh_vy *= 0.9f;
    if (fabsf(u->slosh_vx) < 2 && fabsf(u->slosh_vy) < 2) u->slosh_vx = u->slosh_vy = 0;
}

static void loosen_all_some(App *a);

void brk_before_step(App *a, float real_dt) {
    SwWindow *win = a->win;
    SpWorld *w = a->world;
    if (!win->has_pos) return;
    int k = world_index(a);
    if (!a->frame_seen[k]) {
        a->frame_seen[k] = true;
        a->frame_x[k] = win->sx;
        a->frame_y[k] = win->sy;
    }
    float dx = (float)(win->sx - a->frame_x[k]) / a->vs, dy = (float)(win->sy - a->frame_y[k]) / a->vs;
    a->frame_x[k] = win->sx;
    a->frame_y[k] = win->sy;

    /* A dragged window's position arrives in uneven lumps. Followed exactly, those
     * would rattle everything against the walls even in a smooth drag, so what
     * matter feels is a short average. Its sum is the true distance moved. */
    float vx = dx / real_dt, vy = dy / real_dt;
    vx = vx > MAX_FRAME_SPEED ? MAX_FRAME_SPEED : vx < -MAX_FRAME_SPEED ? -MAX_FRAME_SPEED : vx;
    vy = vy > MAX_FRAME_SPEED ? MAX_FRAME_SPEED : vy < -MAX_FRAME_SPEED ? -MAX_FRAME_SPEED : vy;
    float nvx = a->frame_vx[k] + (vx - a->frame_vx[k]) * 0.45f, nvy = a->frame_vy[k] + (vy - a->frame_vy[k]) * 0.45f;
    if (fabsf(nvx) < 3 && vx == 0) nvx = 0;
    if (fabsf(nvy) < 3 && vy == 0) nvy = 0;
    float kx = nvx - a->frame_vx[k], ky = nvy - a->frame_vy[k];
    kx = kx > MAX_FRAME_KICK ? MAX_FRAME_KICK : kx < -MAX_FRAME_KICK ? -MAX_FRAME_KICK : kx;
    ky = ky > MAX_FRAME_KICK ? MAX_FRAME_KICK : ky < -MAX_FRAME_KICK ? -MAX_FRAME_KICK : ky;
    a->frame_vx[k] += kx;
    a->frame_vy[k] += ky;

    sp_frame_move(w, dx, dy, kx, ky);
    if (a->mode == MODE_PHYS) slosh_grid(a, kx, ky, real_dt);
    if (a->mode == MODE_BODIES && hypotf(kx, ky) > SHAKE_LOOSENS) loosen_all_some(a);

    w->wall_hit_min = brk_can_escape(a) ? WALL_MIN_SPEED : 0;
    set_desk(a, false);
    sync_outside(a);
}

void brk_idle(App *a) {
    SwWindow *win = a->win;
    if (!win->has_pos) return;
    int k = world_index(a);
    if (a->frame_seen[k]) {
        sp_frame_move(a->world, (float)(win->sx - a->frame_x[k]) / a->vs, (float)(win->sy - a->frame_y[k]) / a->vs, 0, 0);
    }
    a->frame_seen[k] = true;
    a->frame_x[k] = win->sx;
    a->frame_y[k] = win->sy;
    a->frame_vx[k] = a->frame_vy[k] = 0;
    set_desk(a, true);
    sync_outside(a);
}

/* ---- the main window's walls ------------------------------------------- */

static void crack(App *a, float x, float y, const char *word) {
    fx_ring(a, x, y, 60, 0.3f);
    fx_text(a, x - 26, y - 10, word);
    if (a->shake < 5) a->shake = 5;
    a->crack_t = 0.25f;
}

bool brk_wall(App *a, SpSide side, float at, float half) {
    if (!brk_can_escape(a)) return false;
    SpWorld *w = a->world;
    float len = side == SP_TOP || side == SP_BOTTOM ? a->world_w : a->world_h;
    float lo = at - half, hi = at + half;
    /* A hole that nearly reaches a corner takes the corner with it. */
    if (lo < 20) lo = -40;
    if (hi > len - 20) hi = len + 40;
    sp_wall_open(w, side, lo, hi);
    set_desk(a, true);
    float x = side == SP_LEFT ? 0 : side == SP_RIGHT ? a->world_w : at;
    float y = side == SP_TOP ? 0 : side == SP_BOTTOM ? a->world_h : at;
    crack(a, x, y, "CRASH");
    app_flash(a, "THE WINDOW BROKE. WINDOW > REPAIR MENDS IT");
    return true;
}

/* ---- inner windows ------------------------------------------------------ */

static SwPanel *panel_by_id(App *a, int id) {
    SwDesk *d = &a->desk;
    for (int i = 0; i < d->npanels; i++)
        if (d->panels[i]->solid_id == id) return d->panels[i];
    return NULL;
}

static void panel_mend(App *a, SwPanel *p) {
    SpWorld *w = a->world_bodies;
    int id = p->solid_id;
    if (id < 0) return;
    for (int s = 0; s < 4; s++) {
        if (a->sides[id][s] >= 0) sp_static_remove(w, a->sides[id][s]);
        a->sides[id][s] = -1;
    }
    for (int i = 0; i < SW_MAX_ELEMS; i++) {
        SwElem *e = &p->elems[i];
        if (e->st >= 0 && p->broken) sp_static_remove(w, e->st);
        if (e->loose && !e->gone && e->body >= 0 && e->body < SP_MAX_BODIES && w->bodies[e->body].alive &&
            a->toys_bodies[e->body].elem == (id + 1) * 64 + i) {
            sp_body_remove(w, e->body);
            a->toys_bodies[e->body].elem = 0;
        }
        e->st = -1;
        e->body = -1;
        e->loose = e->gone = false;
        e->ox = e->oy = 0;
    }
    p->broken = false;
    p->gone_sides = 0;
}

static void elem_rect(const App *a, const SwPanel *p, const SwElem *e, float *x0, float *y0, float *x1, float *y1) {
    *x0 = (float)(p->x + e->x - a->ax) / a->vs;
    *y0 = (float)(p->y + e->y - a->ay) / a->vs;
    *x1 = *x0 + (float)e->w / a->vs;
    *y1 = *y0 + (float)e->h / a->vs;
}

/* A widget comes off, and it's a body now. */
static void elem_loosen(App *a, SwPanel *p, int i, float vx, float vy) {
    SwElem *e = &p->elems[i];
    if (e->loose || e->gone) return;
    SpWorld *w = a->world_bodies;
    if (e->st >= 0) sp_static_remove(w, e->st);
    e->st = -1;
    float x0, y0, x1, y1;
    elem_rect(a, p, e, &x0, &y0, &x1, &y1);
    float cy = (y0 + y1) / 2, hh = (y1 - y0) / 2;
    if (hh < 6) hh = 6; /* thin labels still need room for a body */
    float pts[8] = { x0, cy - hh, x1, cy - hh, x1, cy + hh, x0, cy + hh };
    SpMaterial m = { 0 };
    m.spacing = 10;
    m.friction = 0.8f;
    m.match = 0.95f;
    m.compliance = 2e-7f;
    m.mass = 1.2f;
    int s = sp_add_polygon(w, pts, 4, &m);
    if (s < 0) return; /* no room in the world: it stays where it is */
    memset(&a->toys_bodies[s], 0, sizeof(Toy));
    a->toys_bodies[s].elem = (p->solid_id + 1) * 64 + i;
    const SpBody *b = &w->bodies[s];
    for (int q = b->first; q < b->first + b->count; q++) { w->vx[q] = vx; w->vy[q] = vy; }
    e->body = s;
    e->loose = true;
    e->ox = e->oy = 0;
}

/* The whole box jolted: some of what's barely hanging on lets go. */
static void loosen_all_some(App *a) {
    for (int id = 0; id < SW_MAX_PANELS; id++) {
        SwPanel *p = panel_by_id(a, id);
        if (!p || !p->broken || !p->open) continue;
        for (int i = 0; i < p->nelems; i++)
            if (app_rand(a) < 0.15f) elem_loosen(a, p, i, 0, 0);
    }
}

static void panel_break_side(App *a, SwPanel *p, SpSide side, float hx, float hy) {
    SpWorld *w = a->world_bodies;
    int id = p->solid_id;
    if (!p->broken) {
        p->broken = true;
        if (a->solid[id] >= 0) sp_static_remove(w, a->solid[id]);
        a->solid[id] = -1;
        for (int i = 0; i < SW_MAX_ELEMS; i++) p->elems[i].st = -1;
        /* What's near the hit doesn't stay on. */
        for (int i = 0; i < p->nelems; i++) {
            float x0, y0, x1, y1;
            elem_rect(a, p, &p->elems[i], &x0, &y0, &x1, &y1);
            if (hypotf((x0 + x1) / 2 - hx, (y0 + y1) / 2 - hy) < 70) elem_loosen(a, p, i, 0, 0);
        }
    }
    p->gone_sides |= 1 << side;
    if (a->sides[id][side] >= 0) sp_static_remove(w, a->sides[id][side]);
    a->sides[id][side] = -1;
    crack(a, hx, hy, "CRACK");
}

bool brk_panel(App *a, SwPanel *p, SpSide side) {
    if (p->solid_id < 0 || !p->open || p->shaded || !a->solid_windows || a->mode != MODE_BODIES) return false;
    float hx = (float)(p->x + p->w / 2 - a->ax) / a->vs, hy = (float)(p->y + p->h / 2 - a->ay) / a->vs;
    if (side == SP_TOP) hy = (float)(p->y - a->ay) / a->vs;
    if (side == SP_BOTTOM) hy = (float)(p->y + p->h - a->ay) / a->vs;
    if (side == SP_LEFT) hx = (float)(p->x - a->ax) / a->vs;
    if (side == SP_RIGHT) hx = (float)(p->x + p->w - a->ax) / a->vs;
    panel_break_side(a, p, side, hx, hy);
    return true;
}

/* Makes the physics match the windows: where they are, and what's left of them. */
void brk_sync_panels(App *a) {
    SwDesk *d = &a->desk;
    SpWorld *w = a->world;
    for (int i = 0; i < d->npanels; i++) {
        SwPanel *p = d->panels[i];
        int id = p->solid_id;
        if (id < 0) continue;
        bool want = a->solid_windows && p->open && p->h > 0;
        /* Closing a window, or rolling it up, puts it back together. */
        if (p->broken && (!want || p->shaded)) panel_mend(a, p);
        if (!want) {
            if (a->solid[id] >= 0) sp_static_remove(w, a->solid[id]);
            a->solid[id] = -1;
            continue;
        }
        int vis = p->shaded ? d->dot * 10 : p->h;
        float x0 = (float)(p->x - a->ax) / a->vs, y0 = (float)(p->y - a->ay) / a->vs;
        float x1 = (float)(p->x + p->w - a->ax) / a->vs, y1 = (float)(p->y + vis - a->ay) / a->vs;
        if (!p->broken) {
            if (a->solid[id] < 0) {
                a->solid[id] = sp_add_box(w, x0, y0, x1, y1, TAG_PANEL);
                if (a->solid[id] >= 0) w->statics[a->solid[id]].hit_min = PANEL_MIN_SPEED;
            } else {
                sp_static_move(w, a->solid[id], x0, y0, x1, y1);
            }
            continue;
        }
        float title = (float)(d->dot * 10) / a->vs;
        float rects[4][4] = {
            [SP_TOP] = { x0, y0, x1, y0 + title },
            [SP_BOTTOM] = { x0, y1 - FRAME_THICK, x1, y1 },
            [SP_LEFT] = { x0, y0, x0 + FRAME_THICK, y1 },
            [SP_RIGHT] = { x1 - FRAME_THICK, y0, x1, y1 },
        };
        for (int s = 0; s < 4; s++) {
            if (p->gone_sides & (1 << s)) continue;
            const float *r = rects[s];
            if (a->sides[id][s] < 0) {
                a->sides[id][s] = sp_add_box(w, r[0], r[1], r[2], r[3], TAG_FRAME);
                if (a->sides[id][s] >= 0) w->statics[a->sides[id][s]].hit_min = PANEL_MIN_SPEED;
            } else {
                sp_static_move(w, a->sides[id][s], r[0], r[1], r[2], r[3]);
            }
        }
        for (int q = 0; q < p->nelems; q++) {
            SwElem *e = &p->elems[q];
            if (e->gone) continue;
            if (!e->loose) {
                float ex0, ey0, ex1, ey1;
                elem_rect(a, p, e, &ex0, &ey0, &ex1, &ey1);
                if (e->st < 0) {
                    e->st = sp_add_box(w, ex0, ey0, ex1, ey1, TAG_ELEM);
                    if (e->st >= 0) w->statics[e->st].hit_min = ELEM_MIN_SPEED;
                } else {
                    sp_static_move(w, e->st, ex0, ey0, ex1, ey1);
                }
                continue;
            }
            /* Cut up or vaporized: that's the end of that widget, until a repair. */
            if (e->body < 0 || !w->bodies[e->body].alive || a->toys_bodies[e->body].elem != (id + 1) * 64 + q) {
                e->gone = true;
                continue;
            }
            float cx, cy;
            sp_body_centroid(w, &w->bodies[e->body], &cx, &cy);
            e->ox = (int)(cx * a->vs) + a->ax - (p->x + e->x + e->w / 2);
            e->oy = (int)(cy * a->vs) + a->ay - (p->y + e->y + e->h / 2);
        }
        /* Keep the rest ready, in case the layout has come and gone. */
        for (int q = p->nelems; q < SW_MAX_ELEMS; q++)
            if (p->elems[q].st >= 0) { sp_static_remove(w, p->elems[q].st); p->elems[q].st = -1; }
    }
}

static void check_panels(App *a) {
    SpWorld *w = a->world_bodies;
    SwDesk *d = &a->desk;
    for (int i = 0; i < d->npanels; i++) {
        SwPanel *p = d->panels[i];
        int id = p->solid_id;
        if (id < 0 || !p->open) continue;
        if (!p->broken) {
            if (a->solid[id] < 0) continue;
            const SpStatic *st = &w->statics[a->solid[id]];
            for (int s = 0; s < 4; s++)
                if (st->hit[s] > PANEL_BREAK) { panel_break_side(a, p, (SpSide)s, st->hit_x, st->hit_y); break; }
            continue;
        }
        for (int s = 0; s < 4; s++) {
            if (a->sides[id][s] < 0) continue;
            const SpStatic *st = &w->statics[a->sides[id][s]];
            float sum = st->hit[0] + st->hit[1] + st->hit[2] + st->hit[3];
            if (sum > PANEL_BREAK) panel_break_side(a, p, (SpSide)s, st->hit_x, st->hit_y);
        }
        for (int q = 0; q < p->nelems; q++) {
            SwElem *e = &p->elems[q];
            if (e->loose || e->gone || e->st < 0) continue;
            const SpStatic *st = &w->statics[e->st];
            float sum = st->hit[0] + st->hit[1] + st->hit[2] + st->hit[3];
            if (sum > ELEM_LOOSEN) {
                /* Knocked along the way it was hit. */
                float vx = st->hit[SP_LEFT] > 0 ? 120.0f : st->hit[SP_RIGHT] > 0 ? -120.0f : 0;
                float vy = st->hit[SP_TOP] > 0 ? 120.0f : st->hit[SP_BOTTOM] > 0 ? -120.0f : 0;
                elem_loosen(a, p, q, vx, vy);
            }
        }
    }
}

void brk_after_step(App *a) {
    SpWorld *w = a->world;
    if (a->mode == MODE_BODIES) check_panels(a);
    if (!w->wall_hit_min) return;
    for (int s = 0; s < 4; s++) {
        const SpWall *wl = &w->walls[s];
        if (wl->hit <= WALL_BREAK) continue;
        /* Harder hits leave bigger holes. */
        float half = 70 + wl->hit * 0.012f;
        brk_wall(a, (SpSide)s, wl->hit_at, half > 240 ? 240 : half);
    }
}

/* ---- starting over ------------------------------------------------------ */

void brk_world_cleared(App *a, SpWorld *w) {
    int k = w == a->world_phys;
    for (int i = 0; i < SW_MAX_OBSTACLES; i++) a->outside_st[k][i] = -1;
    if (w != a->world_bodies) return;
    /* All of its statics and bodies went with it. */
    for (int i = 0; i < SW_MAX_PANELS; i++) {
        a->solid[i] = -1;
        for (int s = 0; s < 4; s++) a->sides[i][s] = -1;
    }
    for (int i = 0; i < a->desk.npanels; i++) {
        SwPanel *p = a->desk.panels[i];
        for (int q = 0; q < SW_MAX_ELEMS; q++) {
            SwElem *e = &p->elems[q];
            e->st = e->body = -1;
            e->loose = e->gone = false;
            e->ox = e->oy = 0;
        }
        p->broken = false;
        p->gone_sides = 0;
    }
}

void brk_repair(App *a) {
    sp_walls_repair(a->world);
    if (a->mode == MODE_BODIES)
        for (int i = 0; i < a->desk.npanels; i++) {
            SwPanel *p = a->desk.panels[i];
            if (p->solid_id >= 0 && p->broken) panel_mend(a, p);
        }
    app_flash(a, "ALL MENDED");
}

int brk_count_gaps(const App *a) {
    int n = 0;
    for (int s = 0; s < 4; s++) n += a->world->walls[s].ngaps;
    return n;
}

int brk_count_broken(const App *a) {
    int n = 0;
    for (int i = 0; i < a->desk.npanels; i++) n += a->desk.panels[i]->broken;
    return n;
}

/* How far down the screen the lowest escaped particle is, including its radius. */
float brk_lowest(const App *a) {
    const SpWorld *w = a->world;
    float low = -1;
    for (int i = 0; i < w->np; i++) {
        if (!sp_outside(w, w->x[i], w->y[i])) continue;
        float y = (w->y[i] + w->r[i]) * a->vs + (float)(a->ay + a->win->sy);
        if (y > low) low = y;
    }
    return low;
}

/* ---- drawing ------------------------------------------------------------- */

/* A hole in the window's frame: nothing there, and a jagged edge at each end of it. */
void brk_draw_holes(App *a) {
    SwCanvas *c = &a->canvas;
    const SwWindow *win = a->win;
    if (a->mode == MODE_SELECTOR) return;
    int tooth = a->desk.dot * 3;
    for (int s = 0; s < 4; s++) {
        const SpWall *wl = &a->world->walls[s];
        for (int g = 0; g < wl->ngaps; g++) {
            int p0 = (int)(wl->gaps[g].a * a->vs), p1 = (int)(wl->gaps[g].b * a->vs);
            if (s == SP_TOP || s == SP_BOTTOM) {
                int x0 = a->ax + p0, x1 = a->ax + p1;
                x0 = x0 < 0 ? 0 : x0;
                x1 = x1 > win->w ? win->w : x1;
                int y0 = s == SP_TOP ? 0 : a->ay + a->ah, y1 = s == SP_TOP ? a->ay : win->h;
                sw_fill(c, x0, y0, x1 - x0, y1 - y0, SW_PAPER);
                for (int y = y0, k = 0; y < y1; y += tooth, k++) {
                    int t = k & 1 ? tooth : tooth * 2;
                    sw_fill(c, x0 - t + tooth, y, t, tooth, SW_INK);
                    sw_fill(c, x1 - tooth, y, t, tooth, SW_INK);
                }
            } else {
                int y0 = a->ay + p0, y1 = a->ay + p1;
                y0 = y0 < 0 ? 0 : y0;
                y1 = y1 > win->h ? win->h : y1;
                int x0 = s == SP_LEFT ? 0 : a->ax + a->aw, x1 = s == SP_LEFT ? a->ax : win->w;
                sw_fill(c, x0, y0, x1 - x0, y1 - y0, SW_PAPER);
                /* The frame is hair thin here, so its broken ends poke into the arena. */
                int dir = s == SP_LEFT ? 1 : -1, edge = s == SP_LEFT ? x1 : x0;
                for (int k = 0; k < 4; k++) {
                    int t = (k & 1 ? 1 : 2) * tooth;
                    int xa = dir > 0 ? edge : edge - t;
                    sw_fill(c, xa, y0 + k * tooth - tooth * 2, t, tooth, SW_INK);
                    sw_fill(c, xa, y1 - k * tooth + tooth, t, tooth, SW_INK);
                }
            }
        }
    }
}

static inline void grow(int *x0, int *y0, int *x1, int *y1, int ax, int ay, int bx, int by) {
    if (ax < *x0) *x0 = ax;
    if (ay < *y0) *y0 = ay;
    if (bx > *x1) *x1 = bx;
    if (by > *y1) *y1 = by;
}

static void clear_rect(SwWindow *win, int x, int y, int w, int h) {
    for (int j = y; j < y + h; j++) {
        uint32_t *row = win->outer + (size_t)j * win->outer_w;
        for (int i = x; i < x + w; i++) row[i] = SW_OUTER_CLEAR;
    }
}

/* What has left the window gets drawn on the overlay, where it is on the screen. */
void brk_outer(App *a) {
    SwWindow *win = a->win;
    SpWorld *w = a->world;
    a->escaped = 0;
    bool active = a->mode != MODE_SELECTOR && w->open_bounds && brk_can_escape(a);
    /* Screen position of the arena's corner, measured from the overlay's corner. */
    int ox = win->sx - win->outer_x + a->ax, oy = win->sy - win->outer_y + a->ay;
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    if (active) {
        for (int s = 0; s < SP_MAX_BODIES; s++) {
            const SpBody *b = &w->bodies[s];
            if (!b->alive) continue;
            bool out = false;
            for (int i = b->first; i < b->first + b->count; i++)
                if (sp_outside(w, w->x[i], w->y[i])) { a->escaped++; out = true; }
            if (!out) continue;
            float px = 14; /* world units of margin, for skins, faces and goo */
            float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
            for (int i = b->first; i < b->first + b->count; i++) {
                bx0 = fminf(bx0, w->x[i]); by0 = fminf(by0, w->y[i]);
                bx1 = fmaxf(bx1, w->x[i]); by1 = fmaxf(by1, w->y[i]);
            }
            /* A widget is drawn by its rectangle, not its body. */
            if (a->toys[s].elem) px = 120;
            grow(&x0, &y0, &x1, &y1, ox + (int)((bx0 - px) * a->vs), oy + (int)((by0 - px) * a->vs),
                 ox + (int)((bx1 + px) * a->vs), oy + (int)((by1 + px) * a->vs));
        }
    }
    if (a->escaped == 0) {
        if (win->outer && a->dirty_w > 0) {
            clear_rect(win, a->dirty_x, a->dirty_y, a->dirty_w, a->dirty_h);
            a->dirty_w = 0;
            win->ops->outer_present(win, 0, 0, 0, 0);
        }
        return;
    }
    if (!win->outer) {
        if (!win->ops->outer_open(win)) {
            a->outer_failed = true;
            return;
        }
    }
    if (win->outer != a->outer_seen) {
        a->outer_seen = win->outer;
        a->dirty_w = 0;
        clear_rect(win, 0, 0, win->outer_w, win->outer_h);
        ox = win->sx - win->outer_x + a->ax;
        oy = win->sy - win->outer_y + a->ay;
    }
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > win->outer_w ? win->outer_w : x1;
    y1 = y1 > win->outer_h ? win->outer_h : y1;
    if (a->dirty_w > 0) clear_rect(win, a->dirty_x, a->dirty_y, a->dirty_w, a->dirty_h);

    int pw = x1 - x0, ph = y1 - y0;
    if (pw > 0 && ph > 0) {
        /* Draw with the same code as ever, in another place. */
        SwCanvas saved = a->canvas;
        int sax = a->ax, say = a->ay;
        a->canvas.px = win->outer;
        a->canvas.w = win->outer_w;
        a->canvas.h = win->outer_h;
        sw_clip(&a->canvas, x0, y0, pw, ph);
        a->ax = ox;
        a->ay = oy;
        a->shake_x = a->shake_y = 0;
        render_matter(a);
        /* Widgets that have gone out into the world, as far as can be managed. */
        if (a->mode == MODE_BODIES) {
            SwDesk *d = &a->desk;
            for (int i = 0; i < d->npanels; i++) {
                const SwPanel *p = d->panels[i];
                if (!p->broken || !p->open) continue;
                for (int q = 0; q < p->nelems; q++) {
                    const SwElem *e = &p->elems[q];
                    if (!e->loose || e->gone) continue;
                    int ex = p->x + e->x + e->ox - sax + ox, ey = p->y + e->y + e->oy - say + oy;
                    sw_fill(&a->canvas, ex, ey, e->w, e->h, SW_PAPER);
                    sw_frame(&a->canvas, ex, ey, e->w, e->h, d->line, SW_INK);
                    sw_text(&a->canvas, ex + d->dot, ey + (e->h - 7 * d->dot) / 2, e->label, d->dot, SW_INK);
                }
            }
        }
        a->canvas = saved;
        a->ax = sax;
        a->ay = say;
        /* The window has its own copy of what's in the arena. */
        int ix0 = ox, iy0 = oy, ix1 = ox + a->aw, iy1 = oy + a->ah;
        ix0 = ix0 < x0 ? x0 : ix0;
        iy0 = iy0 < y0 ? y0 : iy0;
        ix1 = ix1 > x1 ? x1 : ix1;
        iy1 = iy1 > y1 ? y1 : iy1;
        if (ix1 > ix0 && iy1 > iy0) clear_rect(win, ix0, iy0, ix1 - ix0, iy1 - iy0);
        if (a->invert) {
            for (int j = y0; j < y1; j++) {
                uint32_t *row = win->outer + (size_t)j * win->outer_w;
                for (int i = x0; i < x1; i++)
                    if (row[i] != SW_OUTER_CLEAR) row[i] ^= 0x00FFFFFFu;
            }
        }
    }
    /* Everything that changed: where it was, and where it is. */
    int ux0 = x0, uy0 = y0, ux1 = x1, uy1 = y1;
    if (a->dirty_w > 0) grow(&ux0, &uy0, &ux1, &uy1, a->dirty_x, a->dirty_y, a->dirty_x + a->dirty_w, a->dirty_y + a->dirty_h);
    if (ux1 > ux0 && uy1 > uy0) win->ops->outer_present(win, ux0, uy0, ux1 - ux0, uy1 - uy0);
    a->dirty_x = x0; a->dirty_y = y0;
    a->dirty_w = pw > 0 ? pw : 0;
    a->dirty_h = ph > 0 ? ph : 0;
}
