/* SoftPute solver. See softpute.h for the model. */
#include "softpute.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SPEED 2600.0f
#define EPS 1e-5f

static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static int mini(int a, int b) { return a < b ? a : b; }
static int maxi(int a, int b) { return a > b ? a : b; }

/* ---- storage ---------------------------------------------------------- */

static void reserve_particles(SpWorld *w, int need) {
    if (need <= w->cap) return;
    int cap = w->cap ? w->cap : 1024;
    while (cap < need) cap *= 2;
    w->x = realloc(w->x, cap * sizeof(float));
    w->y = realloc(w->y, cap * sizeof(float));
    w->qx = realloc(w->qx, cap * sizeof(float));
    w->qy = realloc(w->qy, cap * sizeof(float));
    w->vx = realloc(w->vx, cap * sizeof(float));
    w->vy = realloc(w->vy, cap * sizeof(float));
    w->w = realloc(w->w, cap * sizeof(float));
    w->w0 = realloc(w->w0, cap * sizeof(float));
    w->r = realloc(w->r, cap * sizeof(float));
    w->rho = realloc(w->rho, cap * sizeof(float));
    w->rhon = realloc(w->rhon, cap * sizeof(float));
    w->body = realloc(w->body, cap * sizeof(int));
    w->cap = cap;
}

static int add_particle(SpWorld *w, float x, float y, float mass, float r, int body) {
    reserve_particles(w, w->np + 1);
    int i = w->np++;
    w->x[i] = w->qx[i] = x;
    w->y[i] = w->qy[i] = y;
    w->vx[i] = w->vy[i] = 0;
    w->w[i] = w->w0[i] = mass > 0 ? 1.0f / mass : 0;
    w->r[i] = r;
    w->body[i] = body;
    return i;
}

int sp_add_link(SpWorld *w, int a, int b, float compliance, float tear, int body) {
    if (w->nlinks == w->caplinks) {
        w->caplinks = w->caplinks ? w->caplinks * 2 : 2048;
        w->links = realloc(w->links, w->caplinks * sizeof(SpLink));
    }
    float dx = w->x[a] - w->x[b], dy = w->y[a] - w->y[b];
    SpLink *l = &w->links[w->nlinks];
    l->a = a; l->b = b;
    l->rest = sqrtf(dx * dx + dy * dy);
    l->compliance = compliance;
    l->tear = tear;
    l->body = body;
    l->dead = false;
    return w->nlinks++;
}

static int free_slot(SpWorld *w) {
    for (int i = 0; i < SP_MAX_BODIES; i++)
        if (!w->bodies[i].alive) return i;
    return -1;
}

static void body_free_arrays(SpBody *b) {
    free(b->edges); free(b->tris); free(b->loop); free(b->col); free(b->q0x); free(b->q0y);
    b->edges = NULL; b->tris = NULL; b->loop = NULL; b->col = NULL; b->q0x = NULL; b->q0y = NULL;
    b->nedges = b->ntris = b->nloop = b->ncol = 0;
}

static SpBody *begin_body(SpWorld *w, int slot, SpKind kind, const SpMaterial *m) {
    SpBody *b = &w->bodies[slot];
    memset(b, 0, sizeof *b);
    b->kind = kind;
    b->alive = true;
    b->first = w->np;
    b->scale_x = b->scale_y = 1;
    b->pressure = 1;
    b->center = -1;
    b->skin = 1.5f;
    b->friction = (m && m->friction > 0) ? m->friction : 0.6f;
    b->gravity_scale = (m && m->gravity_scale != 0) ? m->gravity_scale : 1.0f;
    b->damping = (m && m->damping > 0) ? m->damping : 9.0f;
    b->match = m ? m->match : 0;
    return b;
}

/* Stores the rest pose used by shape matching. */
static void capture_rest_pose(SpWorld *w, SpBody *b) {
    b->q0x = malloc(sizeof(float) * b->count);
    b->q0y = malloc(sizeof(float) * b->count);
    float cx, cy;
    sp_body_centroid(w, b, &cx, &cy);
    for (int i = 0; i < b->count; i++) {
        b->q0x[i] = w->x[b->first + i] - cx;
        b->q0y[i] = w->y[b->first + i] - cy;
    }
}

/* ---- lifecycle -------------------------------------------------------- */

SpWorld *sp_world_new(void) {
    SpWorld *w = calloc(1, sizeof *w);
    w->gy = 900;
    w->air = 0.02f;
    w->stiffness = 0.5f;
    w->pressure = 1;
    w->friction = 1;
    w->substeps = 10;
    w->goo_h = 15;
    w->goo_rest = 2.6f;
    w->goo_k = 90000;
    w->goo_near = 150000;
    w->goo_visc = 0.03f;
    sp_set_bounds(w, 0, 0, 800, 600, true);
    return w;
}

void sp_world_clear(SpWorld *w) {
    for (int i = 0; i < SP_MAX_BODIES; i++) {
        body_free_arrays(&w->bodies[i]);
        w->bodies[i].alive = false;
    }
    w->np = 0;
    w->nlinks = 0;
    w->nstatics = 0;
    w->ngrabs = 0;
    w->npairs = 0;
    w->nevents = 0;
    w->need_compact = false;
    w->field_on = false;
    memset(w->walls, 0, sizeof w->walls);
    w->open_bounds = false;
}

void sp_world_free(SpWorld *w) {
    if (!w) return;
    sp_world_clear(w);
    free(w->x); free(w->y); free(w->qx); free(w->qy); free(w->vx); free(w->vy);
    free(w->w); free(w->w0); free(w->r); free(w->body); free(w->rho); free(w->rhon);
    free(w->links); free(w->pairs); free(w->hash_head); free(w->hash_next);
    free(w);
}

void sp_set_bounds(SpWorld *w, float x0, float y0, float x1, float y1, bool snap) {
    w->tbx0 = x0; w->tby0 = y0; w->tbx1 = x1; w->tby1 = y1;
    if (snap) {
        w->bx0 = x0; w->by0 = y0; w->bx1 = x1; w->by1 = y1;
        w->wall_prev[SP_TOP] = y0; w->wall_prev[SP_BOTTOM] = y1;
        w->wall_prev[SP_LEFT] = x0; w->wall_prev[SP_RIGHT] = x1;
    }
}

void sp_set_desk(SpWorld *w, float x0, float y0, float x1, float y1, bool snap) {
    w->tdesk[SP_TOP] = y0; w->tdesk[SP_BOTTOM] = y1;
    w->tdesk[SP_LEFT] = x0; w->tdesk[SP_RIGHT] = x1;
    if (snap || !w->has_desk) memcpy(w->desk, w->tdesk, sizeof w->desk);
    w->has_desk = true;
}

bool sp_outside(const SpWorld *w, float x, float y) {
    return x < w->bx0 || x > w->bx1 || y < w->by0 || y > w->by1;
}

void sp_frame_move(SpWorld *w, float dx, float dy, float dvx, float dvy) {
    for (int s = 0; s < SP_MAX_BODIES; s++) {
        const SpBody *b = &w->bodies[s];
        if (!b->alive) continue;
        float cx, cy;
        sp_body_centroid(w, b, &cx, &cy);
        bool outside = w->open_bounds && sp_outside(w, cx, cy);
        for (int i = b->first; i < b->first + b->count; i++) {
            if (w->w[i] <= 0) continue;
            if (outside) {
                w->x[i] -= dx; w->qx[i] -= dx;
                w->y[i] -= dy; w->qy[i] -= dy;
            } else {
                w->vx[i] -= dvx;
                w->vy[i] -= dvy;
            }
        }
    }
    for (int k = 0; k < w->nstatics; k++) {
        SpStatic *st = &w->statics[k];
        if (!st->alive || !st->outside_only) continue;
        st->x0 -= dx; st->x1 -= dx; st->tx0 -= dx; st->tx1 -= dx;
        st->y0 -= dy; st->y1 -= dy; st->ty0 -= dy; st->ty1 -= dy;
    }
    w->desk[SP_TOP] -= dy; w->desk[SP_BOTTOM] -= dy; w->desk[SP_LEFT] -= dx; w->desk[SP_RIGHT] -= dx;
    w->tdesk[SP_TOP] -= dy; w->tdesk[SP_BOTTOM] -= dy; w->tdesk[SP_LEFT] -= dx; w->tdesk[SP_RIGHT] -= dx;
}

void sp_wall_open(SpWorld *w, SpSide side, float a, float b) {
    SpWall *wl = &w->walls[side];
    /* Holes that touch become one hole. */
    for (int i = 0; i < wl->ngaps; ) {
        if (wl->gaps[i].a <= b && a <= wl->gaps[i].b) {
            a = fminf(a, wl->gaps[i].a);
            b = fmaxf(b, wl->gaps[i].b);
            wl->gaps[i] = wl->gaps[--wl->ngaps];
            i = 0;
        } else {
            i++;
        }
    }
    if (wl->ngaps == SP_MAX_GAPS) wl->ngaps--; /* the oldest few are as good as gone anyway */
    wl->gaps[wl->ngaps].a = a;
    wl->gaps[wl->ngaps].b = b;
    wl->ngaps++;
    w->open_bounds = true;
}

/* Anything already outside stays outside, so the walls keep working from both sides. */
void sp_walls_repair(SpWorld *w) {
    for (int k = 0; k < 4; k++) w->walls[k].ngaps = 0;
    bool out = false;
    for (int i = 0; i < w->np && !out; i++) out = sp_outside(w, w->x[i], w->y[i]);
    w->open_bounds = out;
}

void sp_emit(SpWorld *w, SpEventKind kind, float x, float y, int body) {
    if (w->nevents >= SP_MAX_EVENTS) return;
    w->events[w->nevents++] = (SpEvent){ kind, x, y, body };
}

/* ---- compaction: removes dead bodies and dead links ------------------- */

static void compact(SpWorld *w) {
    int *pmap = malloc(sizeof(int) * (w->np + 1));
    int np = 0;
    for (int i = 0; i < w->np; i++) {
        int bi = w->body[i];
        bool keep = bi >= 0 && w->bodies[bi].alive;
        pmap[i] = keep ? np : -1;
        if (keep) {
            if (np != i) {
                w->x[np] = w->x[i]; w->y[np] = w->y[i];
                w->qx[np] = w->qx[i]; w->qy[np] = w->qy[i];
                w->vx[np] = w->vx[i]; w->vy[np] = w->vy[i];
                w->w[np] = w->w[i]; w->w0[np] = w->w0[i];
                w->r[np] = w->r[i]; w->body[np] = bi;
            }
            np++;
        }
    }
    /* Bodies are contiguous and keep their order, so only `first` shifts. */
    for (int s = 0; s < SP_MAX_BODIES; s++) {
        SpBody *b = &w->bodies[s];
        if (b->alive) b->first = pmap[b->first];
    }

    int *lmap = malloc(sizeof(int) * (w->nlinks + 1));
    int nl = 0;
    for (int i = 0; i < w->nlinks; i++) {
        SpLink l = w->links[i];
        lmap[i] = -2;
        if (l.dead || pmap[l.a] < 0 || pmap[l.b] < 0) continue;
        l.a = pmap[l.a];
        l.b = pmap[l.b];
        lmap[i] = nl;
        w->links[nl++] = l;
    }
    for (int s = 0; s < SP_MAX_BODIES; s++) {
        SpBody *b = &w->bodies[s];
        if (!b->alive) continue;
        for (int t = 0; t < b->ntris; t++)
            for (int k = 0; k < 3; k++)
                if (b->tris[t].link[k] >= 0) b->tris[t].link[k] = lmap[b->tris[t].link[k]];
    }
    int ng = 0;
    for (int i = 0; i < w->ngrabs; i++) {
        if (pmap[w->grabs[i].particle] < 0) continue;
        w->grabs[ng] = w->grabs[i];
        w->grabs[ng++].particle = pmap[w->grabs[i].particle];
    }
    w->ngrabs = ng;
    w->np = np;
    w->nlinks = nl;
    w->npairs = 0;
    free(pmap);
    free(lmap);
    w->need_compact = false;
}

void sp_body_remove(SpWorld *w, int s) {
    if (s < 0 || s >= SP_MAX_BODIES || !w->bodies[s].alive) return;
    body_free_arrays(&w->bodies[s]);
    w->bodies[s].alive = false;
    w->need_compact = true;
    /* A particle may still reference a removed slot until compaction, and a
     * builder could reuse the slot first, so compact right away. */
    compact(w);
}

/* ---- broadphase for loose particles ----------------------------------- */

static bool is_loose(SpKind k) { return k == SP_GOO || k == SP_ROPE || k == SP_CLOTH; }

static void build_pairs(SpWorld *w, float dt) {
    w->npairs = 0;
    float vmax2 = 0;
    bool any = false;
    for (int s = 0; s < SP_MAX_BODIES; s++) {
        const SpBody *b = &w->bodies[s];
        if (!b->alive || !is_loose(b->kind)) continue;
        any = true;
        for (int i = b->first; i < b->first + b->count; i++) {
            float v2 = w->vx[i] * w->vx[i] + w->vy[i] * w->vy[i];
            if (v2 > vmax2) vmax2 = v2;
        }
    }
    if (!any) return;
    float margin = mini(40, (int)(2.2f * sqrtf(vmax2) * dt) + 2);
    float cell = w->goo_h + margin;
    enum { BUCKETS = 8192 };
    if (w->hash_cap < w->np) {
        w->hash_cap = w->np * 2;
        w->hash_next = realloc(w->hash_next, sizeof(int) * w->hash_cap);
    }
    if (!w->hash_head) w->hash_head = malloc(sizeof(int) * BUCKETS);
    memset(w->hash_head, 0xff, sizeof(int) * BUCKETS);

#define HASHCELL(cx, cy) ((((unsigned)(cx) * 73856093u) ^ ((unsigned)(cy) * 19349663u)) & (BUCKETS - 1))
    for (int s = 0; s < SP_MAX_BODIES; s++) {
        const SpBody *b = &w->bodies[s];
        if (!b->alive || !is_loose(b->kind)) continue;
        for (int i = b->first; i < b->first + b->count; i++) {
            int cx = (int)floorf(w->x[i] / cell), cy = (int)floorf(w->y[i] / cell);
            unsigned h = HASHCELL(cx, cy);
            w->hash_next[i] = w->hash_head[h];
            w->hash_head[h] = i;
        }
    }
    float reach2 = cell * cell;
    for (int s = 0; s < SP_MAX_BODIES; s++) {
        const SpBody *b = &w->bodies[s];
        if (!b->alive || !is_loose(b->kind)) continue;
        for (int i = b->first; i < b->first + b->count; i++) {
            int cx = (int)floorf(w->x[i] / cell), cy = (int)floorf(w->y[i] / cell);
            for (int oy = -1; oy <= 1; oy++)
                for (int ox = -1; ox <= 1; ox++) {
                    unsigned h = HASHCELL(cx + ox, cy + oy);
                    for (int j = w->hash_head[h]; j >= 0; j = w->hash_next[j]) {
                        if (j <= i) continue;
                        const SpBody *bj = &w->bodies[w->body[j]];
                        /* a chain or sheet does not collide with itself */
                        if (w->body[j] == s && b->kind != SP_GOO) continue;
                        (void)bj;
                        float dx = w->x[i] - w->x[j], dy = w->y[i] - w->y[j];
                        if (dx * dx + dy * dy > reach2) continue;
                        if (w->npairs == w->cappairs) {
                            w->cappairs = w->cappairs ? w->cappairs * 2 : 4096;
                            w->pairs = realloc(w->pairs, w->cappairs * sizeof *w->pairs);
                        }
                        w->pairs[w->npairs].a = i;
                        w->pairs[w->npairs].b = j;
                        w->npairs++;
                    }
                }
        }
    }
}

/* ---- solvers ---------------------------------------------------------- */

static float stiffness_factor(const SpWorld *w) {
    /* Maps the 0..1 slider to a compliance multiplier, 0.5 -> 1. */
    return powf(10.0f, (0.5f - w->stiffness) * 3.0f);
}

static void burst_body(SpWorld *w, int s) {
    SpBody *b = &w->bodies[s];
    if (b->popped) return;
    b->popped = true;
    b->closed = false;
    float cx, cy;
    sp_body_centroid(w, b, &cx, &cy);
    sp_emit(w, SP_EVENT_POP, cx, cy, s);
    /* Rip the skin at a handful of points and fling the shreds outward. */
    int n = b->count;
    for (int i = 0; i < w->nlinks; i++) {
        SpLink *l = &w->links[i];
        if (l->body != s) continue;
        int la = l->a - b->first;
        int lb = l->b - b->first;
        bool neighbour = abs(la - lb) == 1 || abs(la - lb) == n - 1;
        if (!neighbour || (la % 5) == 0) l->dead = true;
    }
    for (int i = b->first; i < b->first + n; i++) {
        float dx = w->x[i] - cx, dy = w->y[i] - cy;
        float d = sqrtf(dx * dx + dy * dy) + EPS;
        w->vx[i] += dx / d * 420;
        w->vy[i] += dy / d * 420;
    }
}

void sp_body_pop(SpWorld *w, int s) {
    if (s >= 0 && s < SP_MAX_BODIES && w->bodies[s].alive && w->bodies[s].kind == SP_BALLOON)
        burst_body(w, s);
}

static void solve_links(SpWorld *w, float h) {
    float invh2 = stiffness_factor(w) / (h * h);
    for (int i = 0; i < w->nlinks; i++) {
        SpLink *l = &w->links[i];
        if (l->dead) continue;
        int a = l->a, b = l->b;
        float wsum = w->w[a] + w->w[b];
        if (wsum <= 0) continue;
        float dx = w->x[a] - w->x[b], dy = w->y[a] - w->y[b];
        float d = sqrtf(dx * dx + dy * dy);
        if (d < EPS) continue;
        if (l->rest > 0) {
            float stretch = d / l->rest;
            if (l->body >= 0) {
                SpBody *bd = &w->bodies[l->body];
                if (stretch > bd->stretch_peak) bd->stretch_peak = stretch;
            }
            if (l->tear > 0 && stretch > l->tear) {
                l->dead = true;
                sp_emit(w, SP_EVENT_TEAR, (w->x[a] + w->x[b]) / 2, (w->y[a] + w->y[b]) / 2, l->body);
                continue;
            }
        }
        float C = d - l->rest;
        float lambda = -C / (wsum + l->compliance * invh2);
        float nx = dx / d, ny = dy / d;
        w->x[a] += w->w[a] * lambda * nx;
        w->y[a] += w->w[a] * lambda * ny;
        w->x[b] -= w->w[b] * lambda * nx;
        w->y[b] -= w->w[b] * lambda * ny;
    }
}

static void solve_pressure(SpWorld *w, SpBody *b, float h) {
    int n = b->nloop, f = b->first;
    float area = 0;
    for (int i = 0; i < n; i++) {
        int p = f + b->loop[i], q = f + b->loop[(i + 1) % n];
        area += w->x[p] * w->y[q] - w->x[q] * w->y[p];
    }
    area *= 0.5f;
    float target = b->area0 * b->pressure * w->pressure;
    float C = area - target;
    float denom = 0;
    for (int i = 0; i < n; i++) {
        int prev = f + b->loop[(i + n - 1) % n], next = f + b->loop[(i + 1) % n];
        float gx = 0.5f * (w->y[next] - w->y[prev]);
        float gy = 0.5f * (w->x[prev] - w->x[next]);
        denom += w->w[f + b->loop[i]] * (gx * gx + gy * gy);
    }
    denom += b->pressure_compliance / (h * h);
    if (denom < EPS) return;
    float lambda = -C / denom;
    /* The gradient reads neighbours, so positions are written afterwards. */
    float dxs[256], dys[256];
    int nn = mini(n, 256);
    for (int i = 0; i < nn; i++) {
        int prev = f + b->loop[(i + n - 1) % n], next = f + b->loop[(i + 1) % n];
        float gx = 0.5f * (w->y[next] - w->y[prev]);
        float gy = 0.5f * (w->x[prev] - w->x[next]);
        float pw = w->w[f + b->loop[i]];
        dxs[i] = pw * lambda * gx;
        dys[i] = pw * lambda * gy;
    }
    for (int i = 0; i < nn; i++) {
        w->x[f + b->loop[i]] += dxs[i];
        w->y[f + b->loop[i]] += dys[i];
    }
}

static void solve_match(SpWorld *w, SpBody *b, float h) {
    int f = b->first, n = b->count;
    float cx = 0, cy = 0;
    for (int i = 0; i < n; i++) { cx += w->x[f + i]; cy += w->y[f + i]; }
    cx /= n; cy /= n;
    float a = 0, c = 0;
    for (int i = 0; i < n; i++) {
        float px = w->x[f + i] - cx, py = w->y[f + i] - cy;
        float qx = b->q0x[i], qy = b->q0y[i];
        a += px * qx + py * qy;
        c += py * qx - px * qy;
    }
    float len = sqrtf(a * a + c * c);
    if (len < EPS) return;
    float cs = a / len, sn = c / len;
    b->angle = atan2f(sn, cs);

    /* Authored strength is per 1/60 s. Convert to this substep. */
    float m = clampf(b->match * (0.4f + 1.2f * w->stiffness), 0, 0.98f);
    float alpha = 1.0f - powf(1.0f - m, h * 60.0f);
    for (int i = 0; i < n; i++) {
        float wp = w->w[f + i];
        if (wp <= 0) continue;
        float qx = b->q0x[i], qy = b->q0y[i];
        float gx = (cs * qx - sn * qy) * b->scale_x + cx;
        float gy = (sn * qx + cs * qy) * b->scale_y + cy;
        w->x[f + i] += (gx - w->x[f + i]) * alpha;
        w->y[f + i] += (gy - w->y[f + i]) * alpha;
    }
}

static void solve_pairs(SpWorld *w, float h) {
    if (!w->npairs) return;
    float H = w->goo_h, invH = 1.0f / H;
    /* density */
    for (int s = 0; s < SP_MAX_BODIES; s++) {
        const SpBody *b = &w->bodies[s];
        if (!b->alive || b->kind != SP_GOO) continue;
        for (int i = b->first; i < b->first + b->count; i++) w->rho[i] = w->rhon[i] = 0;
    }
    for (int k = 0; k < w->npairs; k++) {
        int a = w->pairs[k].a, b = w->pairs[k].b;
        if (w->bodies[w->body[a]].kind != SP_GOO || w->bodies[w->body[b]].kind != SP_GOO) continue;
        float dx = w->x[b] - w->x[a], dy = w->y[b] - w->y[a];
        float d2 = dx * dx + dy * dy;
        if (d2 >= H * H) continue;
        float q = 1 - sqrtf(d2) * invH, q2 = q * q, q3 = q2 * q;
        w->rho[a] += q2; w->rho[b] += q2;
        w->rhon[a] += q3; w->rhon[b] += q3;
    }
    float h2 = h * h;
    for (int k = 0; k < w->npairs; k++) {
        int a = w->pairs[k].a, b = w->pairs[k].b;
        float dx = w->x[b] - w->x[a], dy = w->y[b] - w->y[a];
        float d2 = dx * dx + dy * dy;
        bool ga = w->bodies[w->body[a]].kind == SP_GOO, gb = w->bodies[w->body[b]].kind == SP_GOO;
        if (ga && gb) {
            if (d2 >= H * H || d2 < EPS) continue;
            float d = sqrtf(d2), q = 1 - d * invH;
            float pa = w->goo_k * (w->rho[a] - w->goo_rest), pb = w->goo_k * (w->rho[b] - w->goo_rest);
            float na = w->goo_near * w->rhon[a], nb = w->goo_near * w->rhon[b];
            float D = h2 * ((pa + pb) * q + (na + nb) * q * q) * 0.5f;
            D = clampf(D, -1.5f, 1.5f);
            float ux = dx / d * D * 0.5f, uy = dy / d * D * 0.5f;
            w->x[a] -= ux; w->y[a] -= uy;
            w->x[b] += ux; w->y[b] += uy;
        } else {
            float rr = w->r[a] + w->r[b];
            if (d2 >= rr * rr || d2 < EPS) continue;
            float wsum = w->w[a] + w->w[b];
            if (wsum <= 0) continue;
            float d = sqrtf(d2), C = (rr - d) / wsum;
            float nx = dx / d, ny = dy / d;
            w->x[a] -= nx * C * w->w[a]; w->y[a] -= ny * C * w->w[a];
            w->x[b] += nx * C * w->w[b]; w->y[b] += ny * C * w->w[b];
        }
    }
}

/* Applies Coulomb friction positionally to particle i, against a surface
 * whose own motion this substep is (sx, sy). `pen` is the normal correction. */
static void apply_friction(SpWorld *w, int i, float nx, float ny, float pen, float mu, float sx, float sy) {
    float dx = w->x[i] - w->qx[i] - sx, dy = w->y[i] - w->qy[i] - sy;
    float dn = dx * nx + dy * ny;
    float tx = dx - dn * nx, ty = dy - dn * ny;
    float tl = sqrtf(tx * tx + ty * ty);
    if (tl < EPS) return;
    float reduce = mu * pen;
    if (reduce > tl) reduce = tl;
    w->x[i] -= tx / tl * reduce;
    w->y[i] -= ty / tl * reduce;
}

/* Particle vs closed polygon. */
static void collide_particle_polygon(SpWorld *w, int i, SpBody *owner, SpBody *poly) {
    float px = w->x[i], py = w->y[i];
    float thick = w->r[i] + poly->skin;
    if (px < poly->minx - thick || px > poly->maxx + thick ||
        py < poly->miny - thick || py > poly->maxy + thick) return;

    int f = poly->first;
    bool inside = false;
    float best = FLT_MAX, bcx = 0, bcy = 0, bt = 0;
    int be = -1;
    for (int e = 0; e < poly->nedges; e++) {
        int ia = f + poly->edges[e].a, ib = f + poly->edges[e].b;
        float ax = w->x[ia], ay = w->y[ia], bx = w->x[ib], by = w->y[ib];
        if ((ay > py) != (by > py) && px < (bx - ax) * (py - ay) / (by - ay) + ax) inside = !inside;
        float ex = bx - ax, ey = by - ay;
        float len2 = ex * ex + ey * ey;
        float t = len2 > EPS ? clampf(((px - ax) * ex + (py - ay) * ey) / len2, 0, 1) : 0;
        float cx = ax + ex * t, cy = ay + ey * t;
        float d2 = (px - cx) * (px - cx) + (py - cy) * (py - cy);
        if (d2 < best) { best = d2; be = e; bcx = cx; bcy = cy; bt = t; }
    }
    if (be < 0) return;
    float d = sqrtf(best);
    if (!inside && d >= thick) return;

    float nx, ny, pen;
    if (d > EPS) {
        nx = (px - bcx) / d; ny = (py - bcy) / d;
        if (inside) { nx = -nx; ny = -ny; pen = thick + d; }
        else pen = thick - d;
    } else {
        /* Exactly on the edge: use the edge normal, outward from the centre. */
        int ia = f + poly->edges[be].a, ib = f + poly->edges[be].b;
        float ex = w->x[ib] - w->x[ia], ey = w->y[ib] - w->y[ia];
        float el = sqrtf(ex * ex + ey * ey) + EPS;
        nx = ey / el; ny = -ex / el;
        float mx = (poly->minx + poly->maxx) / 2, my = (poly->miny + poly->maxy) / 2;
        if ((px - mx) * nx + (py - my) * ny < 0) { nx = -nx; ny = -ny; }
        pen = thick;
    }
    /* Never resolve more than a sane distance in one substep. */
    if (pen > 6) pen = 6;

    int ia = f + poly->edges[be].a, ib = f + poly->edges[be].b;
    float ca = 1 - bt, cb = bt;
    float wi = w->w[i], wa = w->w[ia], wb = w->w[ib];
    float denom = wi + wa * ca * ca + wb * cb * cb;
    if (denom < EPS) return;
    float lambda = pen / denom;
    w->x[i] += nx * lambda * wi;
    w->y[i] += ny * lambda * wi;
    w->x[ia] -= nx * lambda * wa * ca;
    w->y[ia] -= ny * lambda * wa * ca;
    w->x[ib] -= nx * lambda * wb * cb;
    w->y[ib] -= ny * lambda * wb * cb;

    /* Friction acts on both sides of the contact, or stacks drift sideways. */
    float mu = sqrtf(owner->friction * poly->friction) * w->friction;
    float sx = ca * (w->x[ia] - w->qx[ia]) + cb * (w->x[ib] - w->qx[ib]);
    float sy = ca * (w->y[ia] - w->qy[ia]) + cb * (w->y[ib] - w->qy[ib]);
    float rx = w->x[i] - w->qx[i] - sx, ry = w->y[i] - w->qy[i] - sy;
    float rn = rx * nx + ry * ny;
    float tx = rx - rn * nx, ty = ry - rn * ny;
    float tl = sqrtf(tx * tx + ty * ty);
    if (tl > EPS) {
        float reduce = mu * pen;
        if (reduce > tl) reduce = tl;
        float lt = reduce / denom / tl;
        w->x[i] -= tx * lt * wi;
        w->y[i] -= ty * lt * wi;
        w->x[ia] += tx * lt * wa * ca;
        w->y[ia] += ty * lt * wa * ca;
        w->x[ib] += tx * lt * wb * cb;
        w->y[ib] += ty * lt * wb * cb;
    }
    owner->contacts++;
    poly->contacts++;
}

static void update_bbox(const SpWorld *w, SpBody *b) {
    float x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX;
    for (int i = b->first; i < b->first + b->count; i++) {
        if (w->x[i] < x0) x0 = w->x[i];
        if (w->x[i] > x1) x1 = w->x[i];
        if (w->y[i] < y0) y0 = w->y[i];
        if (w->y[i] > y1) y1 = w->y[i];
    }
    b->minx = x0; b->miny = y0; b->maxx = x1; b->maxy = y1;
}

static void collide_bodies(SpWorld *w) {
    for (int a = 0; a < SP_MAX_BODIES; a++)
        if (w->bodies[a].alive) update_bbox(w, &w->bodies[a]);
    const float pad = 8;
    for (int a = 0; a < SP_MAX_BODIES; a++) {
        SpBody *A = &w->bodies[a];
        if (!A->alive || A->ncol == 0) continue;
        for (int b = 0; b < SP_MAX_BODIES; b++) {
            SpBody *B = &w->bodies[b];
            if (a == b || !B->alive || !B->closed) continue;
            if (A->maxx + pad < B->minx || B->maxx + pad < A->minx ||
                A->maxy + pad < B->miny || B->maxy + pad < A->miny) continue;
            for (int k = 0; k < A->ncol; k++)
                collide_particle_polygon(w, A->first + A->col[k], A, B);
        }
    }
}

/* Particle vs static capsule or box. */
static void collide_particle_static(SpWorld *w, int i, SpBody *b, SpStatic *st) {
    if (w->w[i] <= 0) return;
    float px = w->x[i], py = w->y[i], nx, ny, pen, t = 0;
    if (st->outside_only && !sp_outside(w, px, py)) return;
    if (st->kind == SP_STATIC_CAPSULE) {
        float ex = st->x1 - st->x0, ey = st->y1 - st->y0;
        float len2 = ex * ex + ey * ey;
        t = len2 > EPS ? clampf(((px - st->x0) * ex + (py - st->y0) * ey) / len2, 0, 1) : 0;
        float cx = st->x0 + ex * t, cy = st->y0 + ey * t;
        float dx = px - cx, dy = py - cy, min = st->radius + w->r[i];
        float d2 = dx * dx + dy * dy;
        if (d2 >= min * min) return;
        float d = sqrtf(d2);
        if (d > EPS) { nx = dx / d; ny = dy / d; }
        else { nx = 0; ny = -1; }
        pen = min - d;
    } else {
        float rad = w->r[i];
        if (px < st->x0 - rad || px > st->x1 + rad || py < st->y0 - rad || py > st->y1 + rad) return;
        float l = px - (st->x0 - rad), r = (st->x1 + rad) - px;
        float u = py - (st->y0 - rad), d = (st->y1 + rad) - py;
        pen = l; nx = -1; ny = 0;
        if (r < pen) { pen = r; nx = 1; ny = 0; }
        if (u < pen) { pen = u; nx = 0; ny = -1; }
        if (d < pen) { pen = d; nx = 0; ny = 1; }
        t = 0.5f;
    }
    float sx = st->dx0 + (st->dx1 - st->dx0) * t, sy = st->dy0 + (st->dy1 - st->dy0) * t;
    if (st->hit_min > 0) {
        /* How fast it was closing on the surface as it arrived. */
        float vn = -((px - w->qx[i] - sx) * nx + (py - w->qy[i] - sy) * ny) / w->sub_h;
        if (vn > st->hit_min) {
            SpSide face = nx < 0 ? SP_LEFT : nx > 0 ? SP_RIGHT : ny < 0 ? SP_TOP : SP_BOTTOM;
            st->hit[face] += (vn - st->hit_min) / w->w[i];
            st->hit_x = px;
            st->hit_y = py;
        }
    }
    w->x[i] += nx * pen;
    w->y[i] += ny * pen;
    apply_friction(w, i, nx, ny, pen, b->friction * st->friction * w->friction, sx, sy);
    b->contacts++;
}

/* Static corner vs outline edge: stops pegs slipping between outline points. */
static void collide_edges_point(SpWorld *w, SpBody *b, float cx, float cy, float rad) {
    float reach = rad + b->skin;
    if (cx < b->minx - reach || cx > b->maxx + reach || cy < b->miny - reach || cy > b->maxy + reach) return;
    int f = b->first;
    for (int e = 0; e < b->nedges; e++) {
        int ia = f + b->edges[e].a, ib = f + b->edges[e].b;
        float ax = w->x[ia], ay = w->y[ia], bx = w->x[ib], by = w->y[ib];
        float ex = bx - ax, ey = by - ay, len2 = ex * ex + ey * ey;
        float t = len2 > EPS ? clampf(((cx - ax) * ex + (cy - ay) * ey) / len2, 0, 1) : 0;
        float qx = ax + ex * t, qy = ay + ey * t;
        float dx = qx - cx, dy = qy - cy, d2 = dx * dx + dy * dy;
        if (d2 >= reach * reach || d2 < EPS) continue;
        float d = sqrtf(d2), nx = dx / d, ny = dy / d, pen = reach - d;
        float ca = 1 - t, cb = t, wa = w->w[ia], wb = w->w[ib];
        float denom = wa * ca * ca + wb * cb * cb;
        if (denom < EPS) continue;
        float lambda = pen / denom;
        w->x[ia] += nx * lambda * wa * ca; w->y[ia] += ny * lambda * wa * ca;
        w->x[ib] += nx * lambda * wb * cb; w->y[ib] += ny * lambda * wb * cb;
        b->contacts++;
    }
}

static void collide_statics(SpWorld *w) {
    for (int s = 0; s < SP_MAX_BODIES; s++) {
        SpBody *b = &w->bodies[s];
        if (!b->alive) continue;
        update_bbox(w, b);
        for (int k = 0; k < w->nstatics; k++) {
            SpStatic *st = &w->statics[k];
            if (!st->alive) continue;
            float rad = st->kind == SP_STATIC_CAPSULE ? st->radius + 6 : 6;
            float sx0 = (st->x0 < st->x1 ? st->x0 : st->x1) - rad, sx1 = (st->x0 > st->x1 ? st->x0 : st->x1) + rad;
            float sy0 = (st->y0 < st->y1 ? st->y0 : st->y1) - rad, sy1 = (st->y0 > st->y1 ? st->y0 : st->y1) + rad;
            if (b->maxx < sx0 || b->minx > sx1 || b->maxy < sy0 || b->miny > sy1) continue;
            for (int i = b->first; i < b->first + b->count; i++) collide_particle_static(w, i, b, st);
            if (b->nedges && !st->outside_only) {
                if (st->kind == SP_STATIC_CAPSULE) {
                    collide_edges_point(w, b, st->x0, st->y0, st->radius);
                    collide_edges_point(w, b, st->x1, st->y1, st->radius);
                } else {
                    collide_edges_point(w, b, st->x0, st->y0, 0.5f);
                    collide_edges_point(w, b, st->x1, st->y0, 0.5f);
                    collide_edges_point(w, b, st->x0, st->y1, 0.5f);
                    collide_edges_point(w, b, st->x1, st->y1, 0.5f);
                }
            }
        }
    }
}

static inline float wall_position(const SpWorld *w, int side) {
    return side == SP_TOP ? w->by0 : side == SP_BOTTOM ? w->by1 : side == SP_LEFT ? w->bx0 : w->bx1;
}

static void collide_wall(SpWorld *w, int i, SpBody *b, int side, float mu) {
    bool across_y = side == SP_TOP || side == SP_BOTTOM;
    float *p = across_y ? &w->y[i] : &w->x[i];
    float q = across_y ? w->qy[i] : w->qx[i];
    float along = across_y ? w->x[i] : w->y[i];
    float pos = wall_position(w, side), r = w->r[i];
    /* The way out through this wall, along its axis. */
    float out = side == SP_BOTTOM || side == SP_RIGHT ? 1.0f : -1.0f;
    float side_of_wall = 1; /* which way this particle gets pushed: 1 back inside, -1 further out */
    if (w->open_bounds) {
        float lo = across_y ? w->bx0 : w->by0, hi = across_y ? w->bx1 : w->by1;
        if (along < lo - r || along > hi + r) return; /* off the end of this wall */
        const SpWall *wl = &w->walls[side];
        for (int g = 0; g < wl->ngaps; g++)
            if (along >= wl->gaps[g].a && along <= wl->gaps[g].b) return;
        if ((w->wall_prev[side] - q) * out < 0) side_of_wall = -1;
    }
    float pen = r - (pos - *p) * out * side_of_wall;
    if (pen <= 0) return;
    if (w->wall_hit_min > 0 && side_of_wall > 0) {
        float vn = (*p - q) * out / w->sub_h;
        if (vn > w->wall_hit_min) {
            SpWall *wl = &w->walls[side];
            float imp = (vn - w->wall_hit_min) / w->w[i];
            wl->hit += imp;
            wl->hit_at += along * imp;
        }
    }
    *p -= pen * out * side_of_wall;
    float n = -out * side_of_wall;
    apply_friction(w, i, across_y ? 0 : n, across_y ? n : 0, pen, mu, 0, 0);
    b->contacts++;
}

/* Outside the arena, the desk's edges hold matter in. */
static void collide_desk(SpWorld *w, int i, SpBody *b, float mu) {
    float r = w->r[i], pen;
    if ((pen = w->y[i] + r - w->desk[SP_BOTTOM]) > 0) {
        w->y[i] -= pen;
        apply_friction(w, i, 0, -1, pen, mu, w->ddesk[SP_LEFT], w->ddesk[SP_BOTTOM]);
        b->contacts++;
    }
    if ((pen = w->desk[SP_TOP] + r - w->y[i]) > 0) {
        w->y[i] += pen;
        apply_friction(w, i, 0, 1, pen, mu, w->ddesk[SP_LEFT], w->ddesk[SP_TOP]);
        b->contacts++;
    }
    if ((pen = w->x[i] + r - w->desk[SP_RIGHT]) > 0) {
        w->x[i] -= pen;
        apply_friction(w, i, -1, 0, pen, mu, w->ddesk[SP_RIGHT], w->ddesk[SP_TOP]);
        b->contacts++;
    }
    if ((pen = w->desk[SP_LEFT] + r - w->x[i]) > 0) {
        w->x[i] += pen;
        apply_friction(w, i, 1, 0, pen, mu, w->ddesk[SP_LEFT], w->ddesk[SP_TOP]);
        b->contacts++;
    }
}

static void collide_bounds(SpWorld *w) {
    for (int s = 0; s < SP_MAX_BODIES; s++) {
        SpBody *b = &w->bodies[s];
        if (!b->alive) continue;
        float mu = b->friction * w->friction;
        for (int i = b->first; i < b->first + b->count; i++) {
            if (w->w[i] <= 0) continue;
            collide_wall(w, i, b, SP_BOTTOM, mu);
            collide_wall(w, i, b, SP_TOP, mu);
            collide_wall(w, i, b, SP_RIGHT, mu);
            collide_wall(w, i, b, SP_LEFT, mu);
            if (w->open_bounds && w->has_desk && sp_outside(w, w->x[i], w->y[i])) collide_desk(w, i, b, mu);
        }
    }
    for (int k = 0; k < 4; k++) w->wall_prev[k] = wall_position(w, k);
}

static void apply_medium(SpWorld *w, float h) {
    for (int s = 0; s < SP_MAX_BODIES; s++) {
        SpBody *b = &w->bodies[s];
        if (!b->alive) continue;
        for (int i = b->first; i < b->first + b->count; i++) {
            if (w->w[i] <= 0) continue;
            SpMediumSample m = { 0, 0, 0, 0, 0, 0 };
            if (!w->medium(w->medium_user, w->x[i], w->y[i], &m)) continue;
            float pen = sqrtf(m.push_x * m.push_x + m.push_y * m.push_y);
            if (pen > EPS) {
                w->x[i] += m.push_x;
                w->y[i] += m.push_y;
                apply_friction(w, i, m.push_x / pen, m.push_y / pen, pen, m.friction * b->friction * w->friction, 0, 0);
                b->contacts++;
            }
            w->x[i] += m.accel_x * h * h;
            w->y[i] += m.accel_y * h * h;
            if (m.drag > 0) {
                float k = m.drag * h;
                if (k > 0.9f) k = 0.9f;
                w->x[i] -= (w->x[i] - w->qx[i]) * k;
                w->y[i] -= (w->y[i] - w->qy[i]) * k;
            }
        }
    }
}

static void solve_grabs(SpWorld *w, float h) {
    (void)h;
    const float max_move = 7.0f;
    for (int g = 0; g < w->ngrabs; g++) {
        int i = w->grabs[g].particle;
        if (w->w[i] <= 0) continue;
        float dx = w->grab_x + w->grabs[g].ox - w->x[i];
        float dy = w->grab_y + w->grabs[g].oy - w->y[i];
        dx *= 0.14f; dy *= 0.14f;
        float d = sqrtf(dx * dx + dy * dy);
        if (d > max_move) { dx *= max_move / d; dy *= max_move / d; }
        w->x[i] += dx;
        w->y[i] += dy;
    }
}

static void damp_links(SpWorld *w, float h) {
    for (int i = 0; i < w->nlinks; i++) {
        const SpLink *l = &w->links[i];
        if (l->dead || l->body < 0) continue;
        int a = l->a, b = l->b;
        float wsum = w->w[a] + w->w[b];
        if (wsum <= 0) continue;
        float dx = w->x[b] - w->x[a], dy = w->y[b] - w->y[a];
        float d = sqrtf(dx * dx + dy * dy);
        if (d < EPS) continue;
        dx /= d; dy /= d;
        float dvn = (w->vx[b] - w->vx[a]) * dx + (w->vy[b] - w->vy[a]) * dy;
        float k = w->bodies[l->body].damping * h;
        if (k > 0.5f) k = 0.5f;
        float imp = dvn * k / wsum;
        w->vx[a] += dx * imp * w->w[a]; w->vy[a] += dy * imp * w->w[a];
        w->vx[b] -= dx * imp * w->w[b]; w->vy[b] -= dy * imp * w->w[b];
    }
}

/* Bleeds off jiggle while leaving the body's overall motion and spin alone:
 * each velocity moves toward the rigid motion of the whole body. */
static void damp_jiggle(SpWorld *w, SpBody *b, float h) {
    float k = b->damping * h;
    if (k <= 0) return;
    if (k > 0.5f) k = 0.5f;
    int f = b->first, n = b->count;
    float cx = 0, cy = 0, vx = 0, vy = 0, mass = 0;
    for (int i = f; i < f + n; i++) {
        if (w->w[i] <= 0) continue;
        float m = 1.0f / w->w[i];
        cx += w->x[i] * m; cy += w->y[i] * m;
        vx += w->vx[i] * m; vy += w->vy[i] * m;
        mass += m;
    }
    if (mass <= 0) return;
    cx /= mass; cy /= mass; vx /= mass; vy /= mass;
    float L = 0, I = 0;
    for (int i = f; i < f + n; i++) {
        if (w->w[i] <= 0) continue;
        float m = 1.0f / w->w[i], rx = w->x[i] - cx, ry = w->y[i] - cy;
        L += m * (rx * (w->vy[i] - vy) - ry * (w->vx[i] - vx));
        I += m * (rx * rx + ry * ry);
    }
    float om = I > EPS ? L / I : 0;
    for (int i = f; i < f + n; i++) {
        if (w->w[i] <= 0) continue;
        float rx = w->x[i] - cx, ry = w->y[i] - cy;
        w->vx[i] += (vx - om * ry - w->vx[i]) * k;
        w->vy[i] += (vy + om * rx - w->vy[i]) * k;
    }
}

static void goo_viscosity(SpWorld *w) {
    float H = w->goo_h;
    for (int k = 0; k < w->npairs; k++) {
        int a = w->pairs[k].a, b = w->pairs[k].b;
        if (w->bodies[w->body[a]].kind != SP_GOO || w->bodies[w->body[b]].kind != SP_GOO) continue;
        float dx = w->x[b] - w->x[a], dy = w->y[b] - w->y[a], d2 = dx * dx + dy * dy;
        if (d2 >= H * H) continue;
        float q = 1 - sqrtf(d2) / H, c = w->goo_visc * q;
        float dvx = (w->vx[b] - w->vx[a]) * c, dvy = (w->vy[b] - w->vy[a]) * c;
        w->vx[a] += dvx; w->vy[a] += dvy;
        w->vx[b] -= dvx; w->vy[b] -= dvy;
    }
}

/* ---- stepping ---------------------------------------------------------- */

static void substep(SpWorld *w, float h) {
    float air = 1.0f - w->air * h;
    if (air < 0) air = 0;
    float vmax = MAX_SPEED;
    w->sub_h = h;

    /* Moving statics and bounds advance first, so contacts see their motion. */
    for (int k = 0; k < w->nstatics; k++) {
        SpStatic *st = &w->statics[k];
        if (!st->alive) continue;
        st->x0 += st->dx0; st->y0 += st->dy0;
        st->x1 += st->dx1; st->y1 += st->dy1;
    }
    if (w->has_desk)
        for (int k = 0; k < 4; k++) w->desk[k] += w->ddesk[k];

    for (int s = 0; s < SP_MAX_BODIES; s++) {
        SpBody *b = &w->bodies[s];
        if (!b->alive) continue;
        float gx = w->gx * b->gravity_scale, gy = w->gy * b->gravity_scale;
        for (int i = b->first; i < b->first + b->count; i++) {
            w->qx[i] = w->x[i];
            w->qy[i] = w->y[i];
            if (w->w[i] <= 0) { w->vx[i] = w->vy[i] = 0; continue; }
            float vx = w->vx[i] + gx * h, vy = w->vy[i] + gy * h;
            if (w->field_on) {
                float dx = w->x[i] - w->field_x, dy = w->y[i] - w->field_y;
                float d2 = dx * dx + dy * dy, R = w->field_radius;
                if (d2 < R * R && d2 > 1) {
                    float d = sqrtf(d2), f = w->field_strength * (1 - d / R) * h / d;
                    vx += dx * f;
                    vy += dy * f;
                }
            }
            vx *= air; vy *= air;
            float v2 = vx * vx + vy * vy;
            if (v2 > vmax * vmax) {
                float k = vmax / sqrtf(v2);
                vx *= k; vy *= k;
            }
            w->vx[i] = vx;
            w->vy[i] = vy;
            w->x[i] += vx * h;
            w->y[i] += vy * h;
        }
    }

    solve_grabs(w, h);
    solve_links(w, h);
    for (int s = 0; s < SP_MAX_BODIES; s++) {
        SpBody *b = &w->bodies[s];
        if (!b->alive) continue;
        if (b->kind == SP_BALLOON && !b->popped && b->nloop >= 3) solve_pressure(w, b, h);
        if (b->match > 0 && b->q0x) solve_match(w, b, h);
    }
    solve_pairs(w, h);
    collide_bodies(w);
    collide_statics(w);
    if (w->medium) apply_medium(w, h);
    collide_bounds(w);

    float invh = 1.0f / h;
    for (int i = 0; i < w->np; i++) {
        if (!isfinite(w->x[i]) || !isfinite(w->y[i])) {
            /* Last resort: a corrupted particle returns to its previous spot. */
            w->x[i] = isfinite(w->qx[i]) ? w->qx[i] : (w->bx0 + w->bx1) / 2;
            w->y[i] = isfinite(w->qy[i]) ? w->qy[i] : (w->by0 + w->by1) / 2;
            w->qx[i] = w->x[i];
            w->qy[i] = w->y[i];
            w->vx[i] = w->vy[i] = 0;
            continue;
        }
        float vx = (w->x[i] - w->qx[i]) * invh, vy = (w->y[i] - w->qy[i]) * invh;
        float v2 = vx * vx + vy * vy;
        if (v2 > vmax * vmax) {
            /* Large corrections must not become large speeds. */
            float k = vmax / sqrtf(v2);
            vx *= k; vy *= k;
        }
        w->vx[i] = vx;
        w->vy[i] = vy;
    }
    damp_links(w, h);
    for (int s = 0; s < SP_MAX_BODIES; s++) {
        SpBody *b = &w->bodies[s];
        if (b->alive && b->kind == SP_JELLY) damp_jiggle(w, b, h);
    }
    goo_viscosity(w);
}

void sp_step(SpWorld *w, float dt) {
    if (w->need_compact) compact(w);
    int n = maxi(1, w->substeps);
    float h = dt / n;
    w->nevents = 0;

    /* The arena walls move at a limited speed, so a window resize squeezes
     * matter rather than teleporting past it. */
    float lim = 1400.0f * dt;
    w->bx0 += clampf(w->tbx0 - w->bx0, -lim, lim);
    w->by0 += clampf(w->tby0 - w->by0, -lim, lim);
    w->bx1 += clampf(w->tbx1 - w->bx1, -lim, lim);
    w->by1 += clampf(w->tby1 - w->by1, -lim, lim);

    for (int k = 0; k < w->nstatics; k++) {
        SpStatic *st = &w->statics[k];
        if (!st->alive) continue;
        st->dx0 = (st->tx0 - st->x0) / n; st->dy0 = (st->ty0 - st->y0) / n;
        st->dx1 = (st->tx1 - st->x1) / n; st->dy1 = (st->ty1 - st->y1) / n;
        memset(st->hit, 0, sizeof st->hit);
    }
    for (int k = 0; k < 4; k++) {
        w->ddesk[k] = (w->tdesk[k] - w->desk[k]) / n;
        w->walls[k].hit = w->walls[k].hit_at = 0;
    }
    for (int s = 0; s < SP_MAX_BODIES; s++) {
        SpBody *b = &w->bodies[s];
        b->contacts = 0;
        b->stretch_peak = 0;
    }
    build_pairs(w, dt);
    for (int i = 0; i < n; i++) substep(w, h);

    for (int k = 0; k < w->nstatics; k++) {
        SpStatic *st = &w->statics[k];
        if (!st->alive) continue;
        st->x0 = st->tx0; st->y0 = st->ty0; st->x1 = st->tx1; st->y1 = st->ty1;
    }
    if (w->has_desk) memcpy(w->desk, w->tdesk, sizeof w->desk);
    for (int k = 0; k < 4; k++)
        if (w->walls[k].hit > 0) w->walls[k].hit_at /= w->walls[k].hit;

    /* Balloons burst when their skin overstretches. */
    double ke = 0;
    for (int s = 0; s < SP_MAX_BODIES; s++) {
        SpBody *b = &w->bodies[s];
        if (b->alive && b->kind == SP_BALLOON && !b->popped && b->burst > 0 && b->stretch_peak > b->burst)
            burst_body(w, s);
    }
    int moving = 0;
    for (int i = 0; i < w->np; i++) {
        if (w->w[i] <= 0) continue;
        ke += 0.5 * (w->vx[i] * w->vx[i] + w->vy[i] * w->vy[i]);
        moving++;
    }
    w->kinetic = moving ? (float)(ke / moving) : 0;
    w->steps++;
}

/* ---- builders ---------------------------------------------------------- */

static void default_material(SpMaterial *out, const SpMaterial *in, float compliance, float match, float spacing) {
    memset(out, 0, sizeof *out);
    if (in) *out = *in;
    if (out->compliance <= 0) out->compliance = compliance;
    if (out->match <= 0) out->match = match;
    if (out->spacing <= 0) out->spacing = spacing;
    if (out->mass <= 0) out->mass = 1;
}

/* Resamples a closed polygon to evenly spaced points. Returns the count. */
static int resample(const float *pts, int npts, float spacing, float **ox, float **oy) {
    float perim = 0;
    for (int i = 0; i < npts; i++) {
        int j = (i + 1) % npts;
        perim += hypotf(pts[j * 2] - pts[i * 2], pts[j * 2 + 1] - pts[i * 2 + 1]);
    }
    int n = (int)(perim / spacing + 0.5f);
    if (n < 8) n = 8;
    if (n > 200) n = 200;
    float *xs = malloc(sizeof(float) * n), *ys = malloc(sizeof(float) * n);
    float step = perim / n, pos = 0;
    int seg = 0;
    float seglen = hypotf(pts[2] - pts[0], pts[3] - pts[1]), segstart = 0;
    for (int i = 0; i < n; i++) {
        while (pos > segstart + seglen && seg < npts - 1) {
            segstart += seglen;
            seg++;
            int k = (seg + 1) % npts;
            seglen = hypotf(pts[k * 2] - pts[seg * 2], pts[k * 2 + 1] - pts[seg * 2 + 1]);
        }
        int k = (seg + 1) % npts;
        float t = seglen > 0 ? (pos - segstart) / seglen : 0;
        xs[i] = pts[seg * 2] + (pts[k * 2] - pts[seg * 2]) * t;
        ys[i] = pts[seg * 2 + 1] + (pts[k * 2 + 1] - pts[seg * 2 + 1]) * t;
        pos += step;
    }
    *ox = xs; *oy = ys;
    return n;
}

int sp_add_polygon(SpWorld *w, const float *pts, int npts, const SpMaterial *min) {
    int slot = free_slot(w);
    if (slot < 0 || npts < 3) return -1;
    SpMaterial m;
    default_material(&m, min, 2e-6f, 0.25f, 11);

    float *rx, *ry;
    int n = resample(pts, npts, m.spacing, &rx, &ry);
    float cx = 0, cy = 0, area = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        float c = rx[i] * ry[j] - rx[j] * ry[i];
        area += c;
        cx += (rx[i] + rx[j]) * c;
        cy += (ry[i] + ry[j]) * c;
    }
    if (fabsf(area) < 1) { free(rx); free(ry); return -1; }
    cx /= 3 * area; cy /= 3 * area;
    float radius = 0;
    for (int i = 0; i < n; i++) radius = fmaxf(radius, hypotf(rx[i] - cx, ry[i] - cy));

    /* One ring only: rim, spokes and a centre. Shape matching supplies the
     * interior stiffness. Dense concentric rings produce very short links,
     * which tangle on impact and pump energy into the solver. */
    int rings = 1;
    if (w->np + n * rings + 1 > SP_MAX_PARTICLES) { free(rx); free(ry); return -1; }

    SpBody *b = begin_body(w, slot, SP_JELLY, &m);
    b->closed = true;
    b->radius = radius;

    /* Rings run from the rim (ring 0) toward the centre. */
    reserve_particles(w, w->np + n * rings + 1);
    for (int r = 0; r < rings; r++) {
        float k = 1.0f - (float)r / rings;
        for (int i = 0; i < n; i++)
            add_particle(w, cx + (rx[i] - cx) * k, cy + (ry[i] - cy) * k, m.mass, r == 0 ? 2.5f : 2.0f, slot);
    }
    /* The hub carries many links. Unless it is heavy and they are soft, solving
     * them in sequence throws it back and forth and pumps energy in. */
    int center = add_particle(w, cx, cy, m.mass * n / 6.0f, 2, slot) - b->first;
    b->center = center;
    b->count = w->np - b->first;
    int f = b->first;
#define NODE(r, i) (f + (r) * n + (((i) % n + n) % n))
    for (int r = 0; r < rings; r++) {
        for (int i = 0; i < n; i++) {
            sp_add_link(w, NODE(r, i), NODE(r, i + 1), m.compliance, 0, slot);
            if (r + 1 < rings) {
                sp_add_link(w, NODE(r, i), NODE(r + 1, i), m.compliance, 0, slot);
                sp_add_link(w, NODE(r, i), NODE(r + 1, i + 1), m.compliance, 0, slot);
                sp_add_link(w, NODE(r + 1, i), NODE(r, i + 1), m.compliance, 0, slot);
            } else {
                sp_add_link(w, NODE(r, i), f + center, m.compliance * 60, 0, slot);
            }
        }
    }
    /* Skip links resist the outline folding in on itself. */
    for (int i = 0; i < n; i++) sp_add_link(w, NODE(0, i), NODE(0, i + 2), m.compliance * 4, 0, slot);

    b->nedges = n;
    b->edges = malloc(sizeof(SpEdge) * n);
    b->ncol = n;
    b->col = malloc(sizeof(int) * n);
    for (int i = 0; i < n; i++) {
        b->edges[i] = (SpEdge){ i, (i + 1) % n };
        b->col[i] = i;
    }
    b->ntris = n * (rings - 1) * 2 + n;
    b->tris = malloc(sizeof(SpTri) * b->ntris);
    int t = 0;
    for (int r = 0; r < rings; r++) {
        for (int i = 0; i < n; i++) {
            int a = r * n + i, c = r * n + (i + 1) % n;
            if (r + 1 < rings) {
                int d = (r + 1) * n + i, e = (r + 1) * n + (i + 1) % n;
                b->tris[t++] = (SpTri){ a, c, d, { -1, -1, -1 } };
                b->tris[t++] = (SpTri){ c, e, d, { -1, -1, -1 } };
            } else {
                b->tris[t++] = (SpTri){ a, c, center, { -1, -1, -1 } };
            }
        }
    }
    capture_rest_pose(w, b);
#undef NODE
    free(rx); free(ry);
    return slot;
}

int sp_add_bitmap(SpWorld *w, const uint8_t *cells, int cw, int ch, float x, float y, const SpMaterial *min) {
    int slot = free_slot(w);
    if (slot < 0 || cw < 1 || ch < 1) return -1;
    SpMaterial m;
    default_material(&m, min, 2e-6f, 0.45f, 10);
    float s = m.spacing;

#define CELL(cx, cy) ((cx) >= 0 && (cy) >= 0 && (cx) < cw && (cy) < ch && cells[(cy) * cw + (cx)])
    int nw = cw + 1, nh = ch + 1;
    int *nodes = malloc(sizeof(int) * nw * nh);
    memset(nodes, 0xff, sizeof(int) * nw * nh);
    int count = 0;
    for (int j = 0; j < nh; j++)
        for (int i = 0; i < nw; i++)
            if (CELL(i, j) || CELL(i - 1, j) || CELL(i, j - 1) || CELL(i - 1, j - 1)) count++;
    if (!count || w->np + count > SP_MAX_PARTICLES) { free(nodes); return -1; }

    SpBody *b = begin_body(w, slot, SP_JELLY, &m);
    b->closed = true;
    b->radius = fmaxf(cw, ch) * s * 0.5f;
    reserve_particles(w, w->np + count);
    for (int j = 0; j < nh; j++)
        for (int i = 0; i < nw; i++)
            if (CELL(i, j) || CELL(i - 1, j) || CELL(i, j - 1) || CELL(i - 1, j - 1))
                nodes[j * nw + i] = add_particle(w, x + i * s, y + j * s, m.mass, 2, slot) - b->first;
    b->count = w->np - b->first;
    int f = b->first;

    int maxedges = cw * ch * 4;
    b->edges = malloc(sizeof(SpEdge) * maxedges);
    b->tris = malloc(sizeof(SpTri) * cw * ch * 2);
    bool *isrim = calloc(count, sizeof(bool));
    for (int j = 0; j < ch; j++)
        for (int i = 0; i < cw; i++) {
            if (!CELL(i, j)) continue;
            int a = nodes[j * nw + i], c = nodes[j * nw + i + 1];
            int d = nodes[(j + 1) * nw + i], e = nodes[(j + 1) * nw + i + 1];
            sp_add_link(w, f + a, f + e, m.compliance, 0, slot);
            sp_add_link(w, f + c, f + d, m.compliance, 0, slot);
            /* Shared sides are linked once, by the cell above or left. */
            if (!CELL(i, j - 1)) sp_add_link(w, f + a, f + c, m.compliance, 0, slot);
            if (!CELL(i - 1, j)) sp_add_link(w, f + a, f + d, m.compliance, 0, slot);
            sp_add_link(w, f + c, f + e, m.compliance, 0, slot);
            sp_add_link(w, f + d, f + e, m.compliance, 0, slot);
            b->tris[b->ntris++] = (SpTri){ a, c, d, { -1, -1, -1 } };
            b->tris[b->ntris++] = (SpTri){ c, e, d, { -1, -1, -1 } };
#define RIM(p, q) do { b->edges[b->nedges++] = (SpEdge){ p, q }; isrim[p] = isrim[q] = true; } while (0)
            if (!CELL(i, j - 1)) RIM(a, c);
            if (!CELL(i + 1, j)) RIM(c, e);
            if (!CELL(i, j + 1)) RIM(e, d);
            if (!CELL(i - 1, j)) RIM(d, a);
#undef RIM
        }
    b->col = malloc(sizeof(int) * count);
    for (int i = 0; i < count; i++)
        if (isrim[i]) b->col[b->ncol++] = i;
    free(isrim);
    free(nodes);
    capture_rest_pose(w, b);
#undef CELL
    return slot;
}

int sp_add_balloon(SpWorld *w, float x, float y, float radius, const SpMaterial *min) {
    int slot = free_slot(w);
    if (slot < 0 || radius < 4) return -1;
    SpMaterial m;
    default_material(&m, min, 4e-7f, 0, 9);
    int n = (int)(2 * (float)M_PI * radius / m.spacing + 0.5f);
    if (n < 10) n = 10;
    if (n > 128) n = 128;
    if (w->np + n > SP_MAX_PARTICLES) return -1;

    SpBody *b = begin_body(w, slot, SP_BALLOON, &m);
    b->match = 0;
    b->closed = true;
    b->radius = radius;
    b->burst = 1.6f;
    b->pressure_compliance = 0.002f;
    b->damping = (min && min->damping > 0) ? min->damping : 1.2f;
    reserve_particles(w, w->np + n);
    for (int i = 0; i < n; i++) {
        float a = 2 * (float)M_PI * i / n;
        add_particle(w, x + cosf(a) * radius, y + sinf(a) * radius, m.mass * 0.6f, 2.5f, slot);
    }
    b->count = n;
    int f = b->first;
    for (int i = 0; i < n; i++) {
        sp_add_link(w, f + i, f + (i + 1) % n, m.compliance, 0, slot);
        sp_add_link(w, f + i, f + (i + 2) % n, m.compliance * 30, 0, slot);
    }
    b->edges = malloc(sizeof(SpEdge) * n);
    b->nedges = n;
    b->col = malloc(sizeof(int) * n);
    b->ncol = n;
    b->loop = malloc(sizeof(int) * n);
    b->nloop = n;
    for (int i = 0; i < n; i++) {
        b->edges[i] = (SpEdge){ i, (i + 1) % n };
        b->col[i] = i;
        b->loop[i] = i;
    }
    b->area0 = sp_body_area(w, b);
    return slot;
}

int sp_add_rope(SpWorld *w, float x0, float y0, float x1, float y1, const SpMaterial *min, bool pin_start) {
    int slot = free_slot(w);
    if (slot < 0) return -1;
    SpMaterial m;
    default_material(&m, min, 1e-7f, 0, 8);
    float len = hypotf(x1 - x0, y1 - y0);
    int n = (int)(len / m.spacing + 1.5f);
    if (n < 2) n = 2;
    if (w->np + n > SP_MAX_PARTICLES) return -1;
    SpBody *b = begin_body(w, slot, SP_ROPE, &m);
    b->match = 0;
    b->radius = len / 2;
    reserve_particles(w, w->np + n);
    for (int i = 0; i < n; i++) {
        float t = (float)i / (n - 1);
        add_particle(w, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, m.mass, 3, slot);
    }
    b->count = n;
    int f = b->first;
    for (int i = 0; i + 1 < n; i++) sp_add_link(w, f + i, f + i + 1, m.compliance, 3.5f, slot);
    for (int i = 0; i + 2 < n; i++) sp_add_link(w, f + i, f + i + 2, m.compliance * 3000, 0, slot);
    b->col = malloc(sizeof(int) * n);
    b->ncol = n;
    for (int i = 0; i < n; i++) b->col[i] = i;
    if (pin_start) w->w[f] = 0;
    return slot;
}

int sp_add_cloth(SpWorld *w, float x, float y, int nx, int ny, const SpMaterial *min, int pin_every) {
    int slot = free_slot(w);
    if (slot < 0 || nx < 2 || ny < 2) return -1;
    SpMaterial m;
    default_material(&m, min, 2e-7f, 0, 11);
    int count = nx * ny;
    if (w->np + count > SP_MAX_PARTICLES) return -1;
    SpBody *b = begin_body(w, slot, SP_CLOTH, &m);
    b->match = 0;
    b->radius = fmaxf(nx, ny) * m.spacing / 2;
    reserve_particles(w, w->np + count);
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++)
            add_particle(w, x + i * m.spacing, y + j * m.spacing, m.mass * 0.5f, 3, slot);
    b->count = count;
    int f = b->first;
    const float tear = 2.4f;
#define ID(i, j) ((j) * nx + (i))
    int *lh = malloc(sizeof(int) * count), *lv = malloc(sizeof(int) * count), *ld = malloc(sizeof(int) * count);
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) {
            lh[ID(i, j)] = i + 1 < nx ? sp_add_link(w, f + ID(i, j), f + ID(i + 1, j), m.compliance, tear, slot) : -1;
            lv[ID(i, j)] = j + 1 < ny ? sp_add_link(w, f + ID(i, j), f + ID(i, j + 1), m.compliance, tear, slot) : -1;
            ld[ID(i, j)] = (i + 1 < nx && j + 1 < ny)
                ? sp_add_link(w, f + ID(i + 1, j), f + ID(i, j + 1), m.compliance * 400, tear * 1.5f, slot) : -1;
        }
    b->ntris = (nx - 1) * (ny - 1) * 2;
    b->tris = malloc(sizeof(SpTri) * b->ntris);
    int t = 0;
    for (int j = 0; j + 1 < ny; j++)
        for (int i = 0; i + 1 < nx; i++) {
            b->tris[t++] = (SpTri){ ID(i, j), ID(i + 1, j), ID(i, j + 1),
                { lh[ID(i, j)], lv[ID(i, j)], ld[ID(i, j)] } };
            b->tris[t++] = (SpTri){ ID(i + 1, j), ID(i + 1, j + 1), ID(i, j + 1),
                { lv[ID(i + 1, j)], lh[ID(i, j + 1)], ld[ID(i, j)] } };
        }
    free(lh); free(lv); free(ld);
    b->col = malloc(sizeof(int) * count);
    b->ncol = count;
    for (int i = 0; i < count; i++) b->col[i] = i;
    if (pin_every > 0) {
        for (int i = 0; i < nx; i += pin_every) w->w[f + ID(i, 0)] = 0;
        w->w[f + ID(nx - 1, 0)] = 0;
    }
#undef ID
    return slot;
}

int sp_add_goo(SpWorld *w, float x, float y, float radius, int count) {
    int slot = free_slot(w);
    if (slot < 0 || count < 1) return -1;
    if (w->np + count > SP_MAX_PARTICLES) count = SP_MAX_PARTICLES - w->np;
    if (count < 1) return -1;
    SpBody *b = begin_body(w, slot, SP_GOO, NULL);
    b->friction = 0.15f;
    b->radius = radius;
    reserve_particles(w, w->np + count);
    /* Sunflower spiral: even coverage with no lattice artefacts. */
    for (int i = 0; i < count; i++) {
        float t = sqrtf((i + 0.5f) / count) * radius, a = i * 2.39996f;
        add_particle(w, x + cosf(a) * t, y + sinf(a) * t, 1, 3, slot);
    }
    b->count = count;
    b->col = malloc(sizeof(int) * count);
    b->ncol = count;
    for (int i = 0; i < count; i++) b->col[i] = i;
    return slot;
}

/* ---- statics ----------------------------------------------------------- */

static int static_slot(SpWorld *w) {
    for (int i = 0; i < w->nstatics; i++)
        if (!w->statics[i].alive) return i;
    if (w->nstatics >= SP_MAX_STATICS) return -1;
    return w->nstatics++;
}

int sp_add_capsule(SpWorld *w, float x0, float y0, float x1, float y1, float radius, int tag) {
    int i = static_slot(w);
    if (i < 0) return -1;
    w->statics[i] = (SpStatic){ .kind = SP_STATIC_CAPSULE, .alive = true,
        .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1,
        .tx0 = x0, .ty0 = y0, .tx1 = x1, .ty1 = y1,
        .radius = radius, .friction = 0.8f, .tag = tag };
    return i;
}

int sp_add_box(SpWorld *w, float x0, float y0, float x1, float y1, int tag) {
    int i = static_slot(w);
    if (i < 0) return -1;
    w->statics[i] = (SpStatic){ .kind = SP_STATIC_BOX, .alive = true,
        .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1,
        .tx0 = x0, .ty0 = y0, .tx1 = x1, .ty1 = y1,
        .friction = 0.8f, .tag = tag };
    return i;
}

void sp_static_move(SpWorld *w, int i, float x0, float y0, float x1, float y1) {
    if (i < 0 || i >= w->nstatics) return;
    SpStatic *s = &w->statics[i];
    s->tx0 = x0; s->ty0 = y0; s->tx1 = x1; s->ty1 = y1;
}

void sp_static_remove(SpWorld *w, int i) {
    if (i >= 0 && i < w->nstatics) w->statics[i].alive = false;
}

int sp_static_at(const SpWorld *w, float x, float y, float slop, int tag) {
    for (int i = 0; i < w->nstatics; i++) {
        const SpStatic *s = &w->statics[i];
        if (!s->alive || s->tag != tag) continue;
        if (s->kind == SP_STATIC_BOX) {
            if (x >= s->x0 - slop && x <= s->x1 + slop && y >= s->y0 - slop && y <= s->y1 + slop) return i;
        } else {
            float ex = s->x1 - s->x0, ey = s->y1 - s->y0, l2 = ex * ex + ey * ey;
            float t = l2 > EPS ? clampf(((x - s->x0) * ex + (y - s->y0) * ey) / l2, 0, 1) : 0;
            if (hypotf(x - s->x0 - ex * t, y - s->y0 - ey * t) <= s->radius + slop) return i;
        }
    }
    return -1;
}

/* ---- interaction ------------------------------------------------------- */

void sp_grab_begin(SpWorld *w, float x, float y, float radius) {
    w->ngrabs = 0;
    w->grab_x = x;
    w->grab_y = y;
    /* Grab everything in reach, but only ever one body: the nearest. */
    int near = sp_nearest_particle(w, x, y, radius);
    if (near < 0) return;
    int body = w->body[near];
    for (int i = w->bodies[body].first; i < w->bodies[body].first + w->bodies[body].count && w->ngrabs < SP_MAX_GRABS; i++) {
        float dx = w->x[i] - x, dy = w->y[i] - y;
        if (dx * dx + dy * dy <= radius * radius && w->w[i] > 0)
            w->grabs[w->ngrabs++] = (SpGrab){ i, dx, dy };
    }
}

void sp_grab_move(SpWorld *w, float x, float y) { w->grab_x = x; w->grab_y = y; }
void sp_grab_end(SpWorld *w) { w->ngrabs = 0; }

void sp_blast(SpWorld *w, float x, float y, float radius, float impulse) {
    for (int i = 0; i < w->np; i++) {
        if (w->w[i] <= 0) continue;
        float dx = w->x[i] - x, dy = w->y[i] - y;
        float d = sqrtf(dx * dx + dy * dy);
        if (d >= radius || d < EPS) continue;
        float f = impulse * (1 - d / radius) / d;
        w->vx[i] += dx * f;
        w->vy[i] += dy * f;
    }
}

static bool segments_cross(float ax, float ay, float bx, float by, float cx, float cy, float dx, float dy) {
    float d1 = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    float d2 = (bx - ax) * (dy - ay) - (by - ay) * (dx - ax);
    float d3 = (dx - cx) * (ay - cy) - (dy - cy) * (ax - cx);
    float d4 = (dx - cx) * (by - cy) - (dy - cy) * (bx - cx);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}

void sp_cut(SpWorld *w, float x0, float y0, float x1, float y1) {
    bool hit[SP_MAX_BODIES] = { false };
    for (int i = 0; i < w->nlinks; i++) {
        SpLink *l = &w->links[i];
        if (l->dead) continue;
        if (!segments_cross(x0, y0, x1, y1, w->x[l->a], w->y[l->a], w->x[l->b], w->y[l->b])) continue;
        int s = l->body;
        if (s < 0) { l->dead = true; continue; }
        SpBody *b = &w->bodies[s];
        if (b->kind == SP_ROPE || b->kind == SP_CLOTH) l->dead = true;
        else hit[s] = true;
    }
    for (int s = 0; s < SP_MAX_BODIES; s++) {
        if (!hit[s] || !w->bodies[s].alive) continue;
        SpBody *b = &w->bodies[s];
        float cx, cy;
        sp_body_centroid(w, b, &cx, &cy);
        if (b->kind == SP_BALLOON) burst_body(w, s);
        else if (b->kind == SP_JELLY) sp_emit(w, SP_EVENT_SPLAT, cx, cy, s);
    }
}

void sp_shockwave(SpWorld *w, float x, float y, float r0, float r1, float impulse, float falloff) {
    for (int i = 0; i < w->np; i++) {
        if (w->w[i] <= 0) continue;
        float dx = w->x[i] - x, dy = w->y[i] - y;
        float d = sqrtf(dx * dx + dy * dy);
        if (d < r0 || d >= r1) continue;
        float f = impulse / (1 + d / falloff);
        if (d < EPS) { w->vy[i] -= f; continue; }
        w->vx[i] += dx / d * f;
        w->vy[i] += dy / d * f;
    }
}

int sp_vaporize(SpWorld *w, float x, float y, float r) {
    int splats = 0;
    for (int s = 0; s < SP_MAX_BODIES; s++) {
        SpBody *b = &w->bodies[s];
        if (!b->alive) continue;
        float cx, cy;
        sp_body_centroid(w, b, &cx, &cy);
        if ((cx - x) * (cx - x) + (cy - y) * (cy - y) >= r * r) continue;
        if (b->kind == SP_BALLOON) burst_body(w, s);
        else if (b->kind == SP_JELLY && w->nevents < SP_MAX_EVENTS) {
            sp_emit(w, SP_EVENT_SPLAT, cx, cy, s);
            splats++;
        }
    }
    return splats;
}

void sp_shatter(SpWorld *w, float x, float y, float r) {
    float r2 = r * r;
    for (int i = 0; i < w->nlinks; i++) {
        SpLink *l = &w->links[i];
        if (l->dead) continue;
        if (l->body >= 0) {
            SpKind k = w->bodies[l->body].kind;
            if (k != SP_ROPE && k != SP_CLOTH) continue;
        }
        float mx = (w->x[l->a] + w->x[l->b]) / 2 - x, my = (w->y[l->a] + w->y[l->b]) / 2 - y;
        if (mx * mx + my * my < r2) l->dead = true;
    }
    for (int i = 0; i < w->np; i++) {
        if (w->w[i] > 0) continue;
        float dx = w->x[i] - x, dy = w->y[i] - y;
        if (dx * dx + dy * dy < r2) w->w[i] = w->w0[i];
    }
}

int sp_nearest_particle(const SpWorld *w, float x, float y, float max_dist) {
    int best = -1;
    float bd = max_dist * max_dist;
    for (int i = 0; i < w->np; i++) {
        float dx = w->x[i] - x, dy = w->y[i] - y, d2 = dx * dx + dy * dy;
        if (d2 < bd) { bd = d2; best = i; }
    }
    return best;
}

void sp_toggle_pin(SpWorld *w, int i) {
    if (i < 0 || i >= w->np) return;
    if (w->w[i] > 0) { w->w[i] = 0; w->vx[i] = w->vy[i] = 0; }
    else w->w[i] = w->w0[i];
}

int sp_body_at(const SpWorld *w, float x, float y) {
    for (int s = SP_MAX_BODIES - 1; s >= 0; s--) {
        const SpBody *b = &w->bodies[s];
        if (!b->alive || !b->closed) continue;
        if (x < b->minx || x > b->maxx || y < b->miny || y > b->maxy) continue;
        bool in = false;
        for (int e = 0; e < b->nedges; e++) {
            int ia = b->first + b->edges[e].a, ib = b->first + b->edges[e].b;
            float ax = w->x[ia], ay = w->y[ia], bx = w->x[ib], by = w->y[ib];
            if ((ay > y) != (by > y) && x < (bx - ax) * (y - ay) / (by - ay) + ax) in = !in;
        }
        if (in) return s;
    }
    int n = sp_nearest_particle(w, x, y, 14);
    return n >= 0 ? w->body[n] : -1;
}

/* ---- queries ----------------------------------------------------------- */

void sp_body_centroid(const SpWorld *w, const SpBody *b, float *cx, float *cy) {
    float x = 0, y = 0;
    for (int i = b->first; i < b->first + b->count; i++) { x += w->x[i]; y += w->y[i]; }
    int n = b->count > 0 ? b->count : 1;
    *cx = x / n; *cy = y / n;
}

float sp_body_area(const SpWorld *w, const SpBody *b) {
    float a = 0;
    int f = b->first, n = b->nloop;
    for (int i = 0; i < n; i++) {
        int p = f + b->loop[i], q = f + b->loop[(i + 1) % n];
        a += w->x[p] * w->y[q] - w->x[q] * w->y[p];
    }
    return a * 0.5f;
}

bool sp_tri_alive(const SpWorld *w, const SpTri *t) {
    for (int k = 0; k < 3; k++) {
        if (t->link[k] == -2) return false;
        if (t->link[k] >= 0 && w->links[t->link[k]].dead) return false;
    }
    return true;
}

int sp_count_bodies(const SpWorld *w) {
    int n = 0;
    for (int i = 0; i < SP_MAX_BODIES; i++) n += w->bodies[i].alive;
    return n;
}

int sp_count_links(const SpWorld *w) {
    int n = 0;
    for (int i = 0; i < w->nlinks; i++) n += !w->links[i].dead;
    return n;
}
