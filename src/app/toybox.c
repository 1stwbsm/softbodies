/* The toy box: what can be spawned, how creatures behave, and the scenes. */
#include "app.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TAU 6.2831853f

float app_rand(App *a) {
    a->rng = a->rng * 1664525u + 1013904223u;
    return (float)((a->rng >> 8) & 0xFFFFFF) / 16777216.0f;
}

void app_flash(App *a, const char *msg) {
    snprintf(a->flash, sizeof a->flash, "%s", msg);
    a->flash_t = 2.5f;
}

const char *item_name(Item item) {
    static const char *const n[ITEM_COUNT] = {
        "BLOB", "BOX", "STAR", "HEART", "HOPPER", "BALLOON", "HELIUM", "ROPE", "CLOTH", "GOO",
        "DONUT", "CRATE", "JELLY", "SNAKE"
    };
    return n[item];
}

const char *scene_name(int i) {
    static const char *const n[NSCENES] = {
        "1 HELLO", "2 TOWER", "3 LAUNDRY", "4 TRAMPOLINE",
        "5 PEGBOARD", "6 HOPPERS", "7 GOO MILL", "8 BALLOONS", "9 AQUARIUM", "0 WASHER"
    };
    return n[i];
}

/* ---- effects ------------------------------------------------------------ */

static Fx *fx_slot(App *a) {
    Fx *best = &a->fx[0];
    for (int i = 0; i < MAX_FX; i++) {
        if (a->fx[i].ttl <= 0) return &a->fx[i];
        if (a->fx[i].ttl - a->fx[i].t < best->ttl - best->t) best = &a->fx[i];
    }
    return best;
}

void fx_ring(App *a, float x, float y, float r, float ttl) {
    *fx_slot(a) = (Fx){ .kind = FX_RING, .x = x, .y = y, .r = r, .ttl = ttl };
}

void fx_text(App *a, float x, float y, const char *text) {
    Fx *f = fx_slot(a);
    *f = (Fx){ .kind = FX_TEXT, .x = x, .y = y, .ttl = 0.9f };
    snprintf(f->text, sizeof f->text, "%s", text);
}

void fx_update(App *a, float dt) {
    for (int i = 0; i < MAX_FX; i++) {
        Fx *f = &a->fx[i];
        if (f->ttl <= 0) continue;
        f->t += dt;
        if (f->t >= f->ttl) f->ttl = 0;
    }
}

/* ---- spawning ------------------------------------------------------------ */

static Toy *toy_init(App *a, int slot, int shade, bool face) {
    Toy *t = &a->toys[slot];
    memset(t, 0, sizeof *t);
    t->shade = shade;
    t->face = face;
    t->blink = 1 + app_rand(a) * 4;
    t->phase = app_rand(a) * TAU;
    return t;
}

static int pick_shade(App *a) {
    static const int shades[] = { 3, 5, 8, 11, 16 };
    return shades[(int)(app_rand(a) * 5) % 5];
}

static int add_poly(App *a, const float *pts, int n, float size, float friction) {
    SpMaterial m = { 0 };
    m.spacing = size < 24 ? 7 : size < 44 ? 9 : 11;
    m.friction = friction;
    return sp_add_polygon(a->world, pts, n, &m);
}

static int spawn_circle(App *a, float x, float y, float r, bool face) {
    float pts[48 * 2];
    for (int i = 0; i < 48; i++) {
        float t = i * TAU / 48;
        /* A faint lobe, so blobs aren't all perfect circles. */
        float k = r * (1 + 0.06f * sinf(t * 3 + x));
        pts[i * 2] = x + cosf(t) * k;
        pts[i * 2 + 1] = y + sinf(t) * k;
    }
    int s = add_poly(a, pts, 48, r, 0.7f);
    if (s >= 0) toy_init(a, s, pick_shade(a), face);
    return s;
}

static int spawn_box(App *a, float x, float y, float hw, float hh) {
    float pts[8] = { x - hw, y - hh, x + hw, y - hh, x + hw, y + hh, x - hw, y + hh };
    int s = add_poly(a, pts, 4, hw < hh ? hw : hh, 0.85f);
    if (s >= 0) toy_init(a, s, pick_shade(a), false);
    return s;
}

static int spawn_star(App *a, float x, float y, float r) {
    float pts[10 * 2];
    for (int i = 0; i < 10; i++) {
        float t = i * TAU / 10 - TAU / 4, k = (i & 1) ? r * 0.5f : r;
        pts[i * 2] = x + cosf(t) * k;
        pts[i * 2 + 1] = y + sinf(t) * k;
    }
    int s = add_poly(a, pts, 10, r * 0.6f, 0.7f);
    if (s >= 0) toy_init(a, s, pick_shade(a), false);
    return s;
}

static int spawn_heart(App *a, float x, float y, float r) {
    float pts[40 * 2];
    for (int i = 0; i < 40; i++) {
        float t = i * TAU / 40, st = sinf(t);
        pts[i * 2] = x + r / 17 * 16 * st * st * st;
        pts[i * 2 + 1] = y - r / 17 * (13 * cosf(t) - 5 * cosf(2 * t) - 2 * cosf(3 * t) - cosf(4 * t));
    }
    int s = add_poly(a, pts, 40, r * 0.8f, 0.7f);
    if (s >= 0) {
        Toy *t = toy_init(a, s, 16, false);
        t->heart = true;
    }
    return s;
}

static int spawn_balloon(App *a, float x, float y, float r, float gravity) {
    SpMaterial m = { 0 };
    m.gravity_scale = gravity;
    m.spacing = r < 26 ? 7 : 9;
    int s = sp_add_balloon(a->world, x, y, r, &m);
    if (s >= 0) toy_init(a, s, 2, true);
    return s;
}

/* A helium balloon on a light string. Returns the balloon. */
static int spawn_helium(App *a, float x, float y, float r, float string_len, bool tether) {
    int s = spawn_balloon(a, x, y, r, -0.35f);
    if (s < 0) return -1;
    SpMaterial m = { 0 };
    m.mass = 0.12f;
    m.spacing = 9;
    int rope = sp_add_rope(a->world, x, y + r, x, y + r + string_len, &m, false);
    if (rope < 0) return s;
    toy_init(a, rope, 16, false);
    const SpBody *bb = &a->world->bodies[s], *rb = &a->world->bodies[rope];
    /* The balloon's particle a quarter way round its ring is its lowest. */
    sp_add_link(a->world, bb->first + bb->count / 4, rb->first, 1e-7f, 0, -1);
    if (tether) sp_toggle_pin(a->world, rb->first + rb->count - 1);
    return s;
}

static int spawn_cloth(App *a, float x, float y, int nx, int ny, int pin_every) {
    int s = sp_add_cloth(a->world, x, y, nx, ny, NULL, pin_every);
    if (s >= 0) {
        Toy *t = toy_init(a, s, 6, false);
        t->cloth_nx = nx;
    }
    return s;
}

/* A ring: a solid with a real hole in it. */
static int spawn_donut(App *a, float x, float y, float r) {
    float cell = r < 30 ? 7 : 9;
    int n = (int)(r * 2 / cell);
    if (n < 5) n = 5;
    if (n > 24) n = 24;
    uint8_t cells[24 * 24];
    float mid = (float)n / 2;
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            float d = hypotf((float)i + 0.5f - mid, (float)j + 0.5f - mid);
            cells[j * n + i] = d < mid && d > mid * 0.45f;
        }
    SpMaterial m = { 0 };
    m.spacing = cell;
    m.friction = 0.7f;
    int s = sp_add_bitmap(a->world, cells, n, n, x - mid * cell, y - mid * cell, &m);
    if (s >= 0) toy_init(a, s, pick_shade(a), false);
    return s;
}

/* Nearly rigid: it barely gives, and it's heavy enough to flatten things. */
static int spawn_crate(App *a, float x, float y, float hw) {
    float pts[8] = { x - hw, y - hw, x + hw, y - hw, x + hw, y + hw, x - hw, y + hw };
    SpMaterial m = { 0 };
    m.spacing = 11;
    m.friction = 0.9f;
    m.match = 0.92f;
    m.compliance = 2e-7f;
    m.mass = 2.5f;
    int s = sp_add_polygon(a->world, pts, 4, &m);
    if (s >= 0) toy_init(a, s, 11, false);
    return s;
}

/* Barely weighs anything, trails tentacles, and swims by pulsing. */
static int spawn_jellyfish(App *a, float x, float y, float r) {
    int s = spawn_balloon(a, x, y, r, 0.04f);
    if (s < 0) return -1;
    a->toys[s].jellyfish = true;
    a->toys[s].hop_t = app_rand(a) * 1.5f;
    SpBody *bell = &a->world->bodies[s];
    bell->burst = 0; /* its pulse would otherwise pop it */
    int first = bell->first, count = bell->count;
    for (int i = 0; i < 4; i++) {
        float tx = x + ((float)i - 1.5f) * r * 0.4f;
        SpMaterial m = { 0 };
        m.mass = 0.03f;
        m.spacing = 7;
        int rope = sp_add_rope(a->world, tx, y + r * 0.8f, tx, y + r * 2.8f, &m, false);
        if (rope < 0) break;
        toy_init(a, rope, 16, false);
        /* Around the underside of the ring. */
        int at = count / 8 + (i * count / 4) / 3 + count / 16;
        sp_add_link(a->world, first + at % count, a->world->bodies[rope].first, 1e-6f, 0, -1);
    }
    return s;
}

static int spawn_snake(App *a, float x, float y, float size) {
    SpMaterial m = { 0 };
    m.spacing = 9;
    m.mass = 0.8f;
    m.friction = 0.5f;
    int s = sp_add_rope(a->world, x, y, x + size * 5, y, &m, false);
    if (s < 0) return -1;
    Toy *t = toy_init(a, s, 16, true);
    t->snake = true;
    t->rope_width = 6;
    SpBody *b = &a->world->bodies[s];
    /* It can't be stretched apart by its own thrashing. */
    for (int i = 0; i < a->world->nlinks; i++)
        if (a->world->links[i].body == s) a->world->links[i].tear = 0;
    for (int i = b->first; i < b->first + b->count; i++) a->world->r[i] = 5;
    return s;
}

int toy_spawn(App *a, Item item, float x, float y, float size) {
    int s = -1;
    switch (item) {
    case ITEM_BLOB: s = spawn_circle(a, x, y, size, true); break;
    case ITEM_BOX: s = spawn_box(a, x, y, size, size); break;
    case ITEM_STAR: s = spawn_star(a, x, y, size * 1.25f); break;
    case ITEM_HEART: s = spawn_heart(a, x, y, size * 1.1f); break;
    case ITEM_HOPPER:
        s = spawn_circle(a, x, y, size * 0.85f, true);
        if (s >= 0) {
            a->toys[s].hopper = true;
            a->toys[s].hop_t = 0.3f + app_rand(a);
        }
        break;
    case ITEM_BALLOON: s = spawn_balloon(a, x, y, size, 0.4f); break;
    case ITEM_HELIUM: s = spawn_helium(a, x, y, size, size * 3, false); break;
    case ITEM_ROPE: {
        s = sp_add_rope(a->world, x, y, x + size * 6, y, NULL, true);
        if (s >= 0) toy_init(a, s, 16, false);
        break;
    }
    case ITEM_CLOTH: {
        int n = (int)(size / 3);
        if (n < 8) n = 8;
        s = spawn_cloth(a, x - n * 5.5f, y, n * 2, n + 4, 4);
        break;
    }
    case ITEM_DONUT: s = spawn_donut(a, x, y, size * 1.1f); break;
    case ITEM_CRATE: s = spawn_crate(a, x, y, size * 0.9f); break;
    case ITEM_JELLYFISH: s = spawn_jellyfish(a, x, y, size * 0.8f); break;
    case ITEM_SNAKE: s = spawn_snake(a, x, y, size); break;
    case ITEM_GOO: {
        s = sp_add_goo(a->world, x, y, size * 1.3f, (int)(size * 4));
        if (s >= 0) toy_init(a, s, 16, false);
        break;
    }
    default: break;
    }
    if (s < 0) app_flash(a, "THE WORLD IS FULL");
    return s;
}

int toy_letter(App *a, char ch, float x, float y, float cell) {
    const uint8_t *g = sw_glyph(ch);
    if (!g) return -1;
    uint8_t cells[35];
    int any = 0;
    for (int r = 0; r < 7; r++)
        for (int c = 0; c < 5; c++) {
            cells[r * 5 + c] = (g[r] >> (4 - c)) & 1;
            any += cells[r * 5 + c];
        }
    if (!any) return -1;
    SpMaterial m = { 0 };
    m.spacing = cell;
    m.friction = 0.8f;
    int s = sp_add_bitmap(a->world, cells, 5, 7, x, y, &m);
    if (s >= 0) toy_init(a, s, 16, false);
    return s;
}

int toy_word(App *a, const char *text, float x, float y, float cell) {
    int n = 0;
    for (const char *p = text; *p; p++, x += cell * 6)
        if (toy_letter(a, *p, x, y, cell) >= 0) n++;
    return n;
}

/* ---- behaviour ------------------------------------------------------------ */

bool nuke_launch(App *a, float x, float y) {
    for (int i = 0; i < MAX_NUKES; i++) {
        Nuke *n = &a->nukes[i];
        if (n->active) continue;
        *n = (Nuke){ .active = true, .x = x, .y = y, .seed = a->rng ^ (unsigned)(i * 7919 + 13) };
        return true;
    }
    app_flash(a, "SILO RELOADING");
    return false;
}

static void nuke_detonate(App *a, Nuke *n) {
    SpWorld *w = a->world;
    n->exploded = true;
    n->t = 0;
    n->front = 0;
    /* One flash at a time, and never faster than this: no strobing. */
    if (a->sim_time - a->blast_flash_at >= 0.6) a->blast_flash_at = a->sim_time;
    a->shake = 22;
    if (a->mode == MODE_PHYS && a->phys.g) {
        float wpc = a->phys.world_per_cell;
        phys_blast(a->phys.g, (int)(n->x / wpc), (int)(n->y / wpc), (int)(NUKE_FIREBALL / wpc * 0.6f));
    }
    /* The event buffer is finite, so drain it until nothing is left to splat. */
    while (sp_vaporize(w, n->x, n->y, NUKE_FIREBALL) > 0) toys_events(a);
    toys_events(a);
    sp_shatter(w, n->x, n->y, NUKE_FIREBALL * 1.35f);
    for (int i = 0; i < w->nstatics; i++) {
        const SpStatic *st = &w->statics[i];
        if (!st->alive || st->tag != TAG_WALL) continue;
        float mx = (st->x0 + st->x1) / 2 - n->x, my = (st->y0 + st->y1) / 2 - n->y;
        if (mx * mx + my * my < NUKE_FIREBALL * NUKE_FIREBALL * 1.8f) sp_static_remove(w, i);
    }
    for (int s = 0; s < SP_MAX_BODIES; s++)
        if (w->bodies[s].alive) a->toys[s].dizzy = 3.5f;
}

static void nukes_update(App *a, float dt) {
    SpWorld *w = a->world;
    float reach = hypotf(a->world_w, a->world_h);
    a->shake *= powf(0.02f, dt);
    if (a->shake < 0.2f) a->shake = 0;
    for (int i = 0; i < MAX_NUKES; i++) {
        Nuke *n = &a->nukes[i];
        if (!n->active) continue;
        n->t += dt;
        if (!n->exploded) {
            if (n->t >= NUKE_FALL) nuke_detonate(a, n);
            continue;
        }
        /* The front sweeps out once. */
        if (n->front < reach) {
            float next = n->front + 1500 * dt;
            sp_shockwave(w, n->x, n->y, n->front, next, 2600, 110);
            n->front = next;
        }
        /* Then the air rushes back to fill the void and feeds the cloud. */
        if (n->t > 0.9f && n->t < 1.7f) sp_blast(w, n->x, n->y, 520, -1100 * dt);
        if (n->t >= NUKE_LIFE) n->active = false;
    }
}

static void add_velocity(SpWorld *w, const SpBody *b, float vx, float vy) {
    for (int i = b->first; i < b->first + b->count; i++) {
        if (w->w[i] <= 0) continue;
        w->vx[i] += vx;
        w->vy[i] += vy;
    }
}

static void update_hopper(App *a, int s, SpBody *b, Toy *t, float dt) {
    SpWorld *w = a->world;
    float cx, cy;
    sp_body_centroid(w, b, &cx, &cy);
    bool grounded = b->contacts > 0;
    /* Jump against whatever way gravity currently points. */
    float gy = w->gy >= 0 ? 1 : -1;
    (void)s;
    switch (t->hop) {
    case HOP_IDLE:
        b->scale_x += (1 - b->scale_x) * 0.15f;
        b->scale_y += (1 - b->scale_y) * 0.15f;
        t->hop_t -= dt;
        if (t->hop_t <= 0 && grounded) { t->hop = HOP_CROUCH; t->hop_t = 0.22f; }
        break;
    case HOP_CROUCH:
        /* Anticipation: the rest pose squashes, and physics does the rest. */
        b->scale_x += (1.25f - b->scale_x) * 0.25f;
        b->scale_y += (0.6f - b->scale_y) * 0.25f;
        t->hop_t -= dt;
        if (t->hop_t <= 0) {
            float dir;
            if (a->mouse_in_arena) dir = a->mouse_x > cx ? 1 : -1;
            else dir = app_rand(a) < 0.5f ? 1 : -1;
            float reach = a->mouse_in_arena ? fabsf(a->mouse_x - cx) : 200;
            if (reach > 260) reach = 260;
            add_velocity(w, b, dir * (60 + reach * 0.9f), -gy * (360 + app_rand(a) * 200));
            b->scale_x = 0.85f;
            b->scale_y = 1.2f;
            t->hop = HOP_AIR;
            t->hop_t = 0.15f;
        }
        break;
    case HOP_AIR:
        t->hop_t -= dt;
        b->scale_x += (1 - b->scale_x) * 0.1f;
        b->scale_y += (1 - b->scale_y) * 0.1f;
        if (t->hop_t <= 0 && grounded) {
            t->hop = HOP_IDLE;
            t->hop_t = 0.3f + app_rand(a) * 0.9f;
        }
        break;
    }
}

static void update_jellyfish(App *a, SpBody *b, Toy *t, float dt) {
    SpWorld *w = a->world;
    t->hop_t -= dt;
    float cx, cy;
    sp_body_centroid(w, b, &cx, &cy);
    if (t->hop_t <= 0) {
        /* A squeeze and a kick: toward the cursor if it's around, else just upward. */
        t->hop_t = 1.1f + app_rand(a) * 0.9f;
        t->phase = 0.32f;
        float dx = a->mouse_in_arena ? a->mouse_x - cx : (app_rand(a) - 0.5f) * 200;
        dx = dx > 200 ? 200 : dx < -200 ? -200 : dx;
        add_velocity(w, b, dx * 0.5f, cy < 80 ? 40 : -300);
    }
    if (t->phase > 0) {
        t->phase -= dt;
        b->pressure = 0.72f;
    } else {
        b->pressure += (1.0f - b->pressure) * 0.1f;
    }
}

static void update_snake(App *a, int s, SpBody *b) {
    SpWorld *w = a->world;
    Toy *t = &a->toys[s];
    t->phase += 0.16f;
    int head = b->first;
    float tx = a->mouse_in_arena ? a->mouse_x : w->bx1 * (0.5f + 0.4f * sinf((float)a->sim_time * 0.31f + (float)s));
    float ty = a->mouse_in_arena ? a->mouse_y : w->by1 * 0.8f;
    float dx = tx - w->x[head], dy = ty - w->y[head], d = hypotf(dx, dy) + 1;
    /* Thrust at the head, with a wave travelling down the body to slither by. */
    w->vx[head] += dx / d * 95;
    w->vy[head] += dy / d * 95;
    for (int i = 1; i < b->count; i++) {
        int p = b->first + i;
        if (w->w[p] <= 0) continue;
        float wave = sinf(t->phase - (float)i * 0.7f) * 12;
        w->vx[p] += -dy / d * wave;
        w->vy[p] += dx / d * wave;
    }
    /* Don't fly: only partly undo gravity, so it stays on the ground unless lifted. */
    float sp = hypotf(w->vx[head], w->vy[head]);
    if (sp > 620) { w->vx[head] *= 620 / sp; w->vy[head] *= 620 / sp; }
}

void toys_update(App *a, float dt) {
    SpWorld *w = a->world;
    nukes_update(a, dt);
    /* Rotors belong to SoftBodies' scenes. */
    for (int i = 0; w == a->world_bodies && i < a->nrotors; i++) {
        Rotor *r = &a->rotors[i];
        r->angle += r->omega * dt;
        float c = cosf(r->angle) * r->len, sn = sinf(r->angle) * r->len;
        sp_static_move(w, r->st, r->px - c, r->py - sn, r->px + c, r->py + sn);
    }
    for (int s = 0; s < SP_MAX_BODIES; s++) {
        SpBody *b = &w->bodies[s];
        if (!b->alive) continue;
        Toy *t = &a->toys[s];
        /* Shreds always clear away, even if the pop event didn't fit the buffer. */
        if (b->popped && t->ttl <= 0) t->ttl = 2.2f;
        if (t->ttl > 0) {
            t->ttl -= dt;
            if (t->ttl <= 0) { sp_body_remove(w, s); continue; }
        }
        t->blink -= dt;
        if (t->blink < -0.12f) t->blink = 1.5f + app_rand(a) * 5;
        if (t->dizzy > 0) t->dizzy -= dt;
        if (b->stretch_peak > 1.35f && b->kind == SP_JELLY) t->dizzy = 1.2f;
        if (t->heart) {
            /* lub-dub */
            float beat = fmodf((float)a->sim_time * 1.1f + t->phase, 1.0f);
            float pulse = exp(-beat * 18) * 0.26f + exp(-fabsf(beat - 0.22f) * 22) * 0.16f;
            b->scale_x = b->scale_y = 1 + pulse;
        }
        if (t->hopper) update_hopper(a, s, b, t, dt);
        if (t->jellyfish && !b->popped) update_jellyfish(a, b, t, dt);
        if (t->snake) update_snake(a, s, b);
    }
}

/* Reacts to what the engine reports, once per step. */
void toys_events(App *a) {
    SpWorld *w = a->world;
    int nev = w->nevents;
    for (int i = 0; i < nev; i++) {
        SpEvent e = w->events[i];
        if (e.kind == SP_EVENT_POP) {
            fx_ring(a, e.x, e.y, 90, 0.35f);
            fx_text(a, e.x - 20, e.y - 10, "POP!");
            if (e.body >= 0) a->toys[e.body].ttl = 2.2f;
        } else if (e.kind == SP_EVENT_SPLAT) {
            int s = e.body;
            if (s < 0 || !w->bodies[s].alive) continue;
            /* A sliced jelly loses its structure and collapses into goo. */
            float r = w->bodies[s].radius;
            int count = (int)(r * r * 0.11f);
            if (count < 16) count = 16;
            if (count > 260) count = 260;
            sp_body_remove(w, s);
            int g = sp_add_goo(w, e.x, e.y, r * 0.9f, count);
            if (g >= 0) toy_init(a, g, 16, false);
            fx_text(a, e.x - 26, e.y - 10, "SPLAT");
        }
    }
    w->nevents = 0;
}

/* ---- scenes --------------------------------------------------------------- */

void toys_reset(App *a) {
    sp_world_clear(a->world);
    memset(a->toys, 0, sizeof(Toy) * SP_MAX_BODIES);
    memset(a->fx, 0, sizeof a->fx);
    memset(a->nukes, 0, sizeof a->nukes);
    a->shake = 0;
    a->blast_flash_at = -10;
    a->nrotors = 0;
    a->grabbing = false;
    brk_world_cleared(a, a->world);
    a->glue_from = -1;
    a->type_x = a->world_w * 0.3f;
    a->type_y = 60;
}

static void add_rotor(App *a, float x, float y, float len, float rad, float omega) {
    if (a->nrotors >= MAX_ROTORS) return;
    int st = sp_add_capsule(a->world, x - len, y, x + len, y, rad, TAG_SCENE);
    if (st < 0) return;
    a->rotors[a->nrotors++] = (Rotor){ st, x, y, len, 0, omega };
}

static void scene_hello(App *a, float W, float H) {
    float cell = W > 900 ? 11 : 8;
    float wa = cell * 6 * 4, wb = cell * 6 * 6;
    toy_word(a, "SOFT", W / 2 - wa / 2, H * 0.06f, cell);
    toy_word(a, "BODIES", W / 2 - wb / 2, H * 0.20f, cell);
    spawn_circle(a, W * 0.40f, H * 0.62f, 38, true);
    spawn_circle(a, W * 0.60f, H * 0.62f, 30, true);
    spawn_balloon(a, W * 0.5f, H * 0.68f, 40, 0.4f);
    spawn_heart(a, W * 0.71f, H * 0.68f, 28);
    spawn_star(a, W * 0.29f, H * 0.70f, 34);
}

static void scene_tower(App *a, float W, float H) {
    float s = 26, floor = H - 4;
    for (int col = 0; col < 4; col++) {
        float x = W * (0.32f + col * 0.12f);
        int rows = 3 + (col + 1) % 3;
        for (int r = 0; r < rows; r++)
            spawn_box(a, x, floor - s - 2 - r * (2 * s + 5), s, s);
    }
    int ball = spawn_circle(a, W * 0.2f, H * 0.2f, 34, true);
    if (ball >= 0) {
        SpBody *b = &a->world->bodies[ball];
        add_velocity(a->world, b, 480, 40);
    }
}

static void scene_laundry(App *a, float W, float H) {
    (void)H;
    float y = 34;
    spawn_cloth(a, W * 0.30f, y, 20, 18, 3);
    spawn_cloth(a, W * 0.56f, y, 14, 22, 4);
    int r = sp_add_rope(a->world, W * 0.30f, y - 14, W * 0.72f, y - 14, NULL, true);
    if (r >= 0) {
        toy_init(a, r, 16, false);
        const SpBody *b = &a->world->bodies[r];
        sp_toggle_pin(a->world, b->first + b->count - 1);
    }
    /* Two blobs get thrown through the washing as the scene opens. */
    int b1 = spawn_circle(a, W * 0.22f, H * 0.42f, 30, true);
    int b2 = spawn_circle(a, W * 0.80f, H * 0.50f, 24, true);
    if (b1 >= 0) add_velocity(a->world, &a->world->bodies[b1], 620, -330);
    if (b2 >= 0) add_velocity(a->world, &a->world->bodies[b2], -560, -420);
}

static void scene_trampoline(App *a, float W, float H) {
    float x = W * 0.25f, y = H * 0.72f;
    int nx = (int)(W * 0.5f / 11);
    int s = spawn_cloth(a, x, y, nx, 3, 0);
    if (s >= 0) {
        const SpBody *b = &a->world->bodies[s];
        for (int j = 0; j < 3; j++) {
            sp_toggle_pin(a->world, b->first + j * nx);
            sp_toggle_pin(a->world, b->first + j * nx + nx - 1);
        }
    }
    spawn_circle(a, W * 0.40f, H * 0.2f, 32, true);
    spawn_balloon(a, W * 0.52f, H * 0.12f, 38, 0.8f);
    spawn_star(a, W * 0.62f, H * 0.28f, 32);
    spawn_box(a, W * 0.48f, H * 0.38f, 22, 22);
}

static void scene_pegboard(App *a, float W, float H) {
    float gap = 66, x0 = W * 0.26f, x1 = W * 0.74f;
    int row = 0;
    for (float y = H * 0.30f; y < H * 0.82f; y += gap * 0.85f, row++)
        for (float x = x0 + (row & 1 ? gap / 2 : 0); x <= x1; x += gap)
            sp_add_capsule(a->world, x, y, x, y, 7, TAG_SCENE);
    for (int i = 0; i < 6; i++) {
        float x = x0 + (x1 - x0) * (i + 0.5f) / 6;
        if (i % 3 == 2) spawn_heart(a, x, 40, 22);
        else spawn_circle(a, x, 40 + (i % 2) * 30, 21, true);
    }
}

static void scene_hoppers(App *a, float W, float H) {
    for (int i = 0; i < 7; i++)
        toy_spawn(a, ITEM_HOPPER, W * (0.28f + i * 0.075f), H - 70 - (i % 2) * 70, 24 + (i % 3) * 7);
    spawn_box(a, W * 0.5f, H - 36, 32, 32);
    sp_add_capsule(a->world, W * 0.36f, H * 0.62f, W * 0.48f, H * 0.62f, 6, TAG_SCENE);
    sp_add_capsule(a->world, W * 0.58f, H * 0.48f, W * 0.70f, H * 0.48f, 6, TAG_SCENE);
    app_flash(a, "THEY FOLLOW YOUR CURSOR");
}

static void scene_goomill(App *a, float W, float H) {
    /* A sealed basin keeps the goo in the rotor's reach: its walls run past
     * the floor. Only what the rotor flings over the rim can escape. */
    sp_add_capsule(a->world, W * 0.27f, H * 0.42f, W * 0.41f, H * 1.02f, 6, TAG_SCENE);
    sp_add_capsule(a->world, W * 0.73f, H * 0.42f, W * 0.59f, H * 1.02f, 6, TAG_SCENE);
    add_rotor(a, W * 0.5f, H * 0.70f, W * 0.105f, 7, 2.4f);
    int g = sp_add_goo(a->world, W * 0.5f, H * 0.40f, 110, 760);
    if (g >= 0) toy_init(a, g, 16, false);
    spawn_circle(a, W * 0.42f, H * 0.14f, 26, true);
    spawn_box(a, W * 0.58f, H * 0.12f, 22, 22);
}

static void scene_balloons(App *a, float W, float H) {
    for (int i = 0; i < 6; i++) {
        float x = W * (0.28f + i * 0.09f);
        spawn_helium(a, x, H * 0.46f + (i % 2) * 40, 26 + (i % 3) * 6, H * 0.34f, true);
    }
    app_flash(a, "RAISE PRESSURE AND WATCH");
}

static void scene_aquarium(App *a, float W, float H) {
    for (int i = 0; i < 5; i++) spawn_jellyfish(a, W * (0.3f + i * 0.1f), H * (0.35f + (i % 2) * 0.2f), 20 + (i % 3) * 6);
    int g = sp_add_goo(a->world, W * 0.5f, H - 40, 180, 560);
    if (g >= 0) toy_init(a, g, 16, false);
    spawn_snake(a, W * 0.3f, H - 90, 30);
    spawn_star(a, W * 0.72f, H - 70, 28);
    app_flash(a, "THEY SWIM TOWARD THE CURSOR");
}

static void scene_washer(App *a, float W, float H) {
    /* Three paddles turning inside a drum, and everyone's in the wash. */
    float cx = W * 0.5f, cy = H * 0.52f, R = H * 0.46f;
    for (int i = 0; i < 28; i++) {
        float t0 = (float)i * 6.2831853f / 28, t1 = (float)(i + 1) * 6.2831853f / 28;
        sp_add_capsule(a->world, cx + cosf(t0) * R, cy + sinf(t0) * R, cx + cosf(t1) * R, cy + sinf(t1) * R, 6, TAG_SCENE);
    }
    add_rotor(a, cx, cy, R * 0.52f, 8, 1.2f);
    for (int i = 0; i < 5; i++) spawn_circle(a, cx - R * 0.3f + i * R * 0.15f, cy - R * 0.55f, 18 + (i % 3) * 4, true);
    spawn_donut(a, cx, cy + R * 0.6f, 26);
    spawn_crate(a, cx + R * 0.45f, cy + R * 0.4f, 16);
    spawn_cloth(a, cx - R * 0.35f, cy - R * 0.1f, 12, 9, 0);
}

void scene_load(App *a, int index) {
    if (index < 0 || index >= NSCENES) return;
    a->scene = index;
    toys_reset(a);
    float W = a->world_w, H = a->world_h;
    sp_set_bounds(a->world, 0, 0, W, H, true);
    switch (index) {
    case 0: scene_hello(a, W, H); break;
    case 1: scene_tower(a, W, H); break;
    case 2: scene_laundry(a, W, H); break;
    case 3: scene_trampoline(a, W, H); break;
    case 4: scene_pegboard(a, W, H); break;
    case 5: scene_hoppers(a, W, H); break;
    case 6: scene_goomill(a, W, H); break;
    case 7: scene_balloons(a, W, H); break;
    case 8: scene_aquarium(a, W, H); break;
    case 9: scene_washer(a, W, H); break;
    }
}
