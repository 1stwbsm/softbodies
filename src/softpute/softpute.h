/* SoftPute - a small 2D soft body physics engine.
 *
 * Solver: XPBD with small substeps. Matter is particles held together by:
 *   links     distance constraints, optionally tearable
 *   pressure  area constraint over a closed loop (balloons)
 *   matching  shape matching toward a rest pose (jelly memory)
 *   density   double density relaxation (goo)
 * Collisions: particle vs closed polygon, particle vs particle, particle and
 * edge vs static capsules and boxes, plus the arena bounds.
 *
 * Units: distance = world units (one logical pixel), time = seconds, y points
 * down. No dependencies beyond libc and libm.
 */
#ifndef SOFTPUTE_H
#define SOFTPUTE_H

#include <stdbool.h>
#include <stdint.h>

#define SP_MAX_BODIES   256
#define SP_MAX_STATICS  512
#define SP_MAX_GAPS     8
#define SP_MAX_GRABS    64
#define SP_MAX_EVENTS   64
#define SP_MAX_PARTICLES 12000

typedef enum {
    SP_JELLY,    /* closed outline, interior mesh, shape matching */
    SP_BALLOON,  /* closed outline, pressure                      */
    SP_ROPE,     /* open chain                                    */
    SP_CLOTH,    /* open tearable sheet                           */
    SP_GOO       /* loose particles with density relaxation       */
} SpKind;

typedef enum { SP_STATIC_CAPSULE, SP_STATIC_BOX } SpStaticKind;

typedef enum {
    SP_EVENT_POP,  /* balloon burst        */
    SP_EVENT_TEAR, /* cloth or rope tore   */
    SP_EVENT_SPLAT /* jelly cut by a blade */
} SpEventKind;

typedef struct { SpEventKind kind; float x, y; int body; } SpEvent;

typedef struct {
    int a, b;           /* global particle indices */
    float rest;
    float compliance;   /* XPBD compliance, 0 = rigid        */
    float tear;         /* break above rest*tear, 0 = never  */
    int body;           /* owner slot, -1 if none            */
    bool dead;
} SpLink;

typedef struct { int a, b; } SpEdge;              /* local particle indices */
typedef struct { int a, b, c; int link[3]; } SpTri; /* link: global, -1 none, -2 dead */

typedef struct {
    SpKind kind;
    bool alive;
    bool closed;        /* edges form a solid polygon          */
    bool popped;
    int first, count;   /* particles [first, first+count)      */

    SpEdge *edges; int nedges;
    SpTri  *tris;  int ntris;
    int *loop;  int nloop;  /* local indices, pressure ring    */
    int *col;   int ncol;   /* local indices of collider points */
    float *q0x, *q0y;       /* rest pose about centroid        */

    float match;        /* shape matching strength per 1/60 s, 0..1 */
    float scale_x, scale_y; /* world-axis scale of the match target */
    float angle;        /* rotation found by the last match    */
    float area0;        /* rest area of the loop               */
    float pressure;     /* per-body pressure multiplier        */
    float pressure_compliance;
    float burst;        /* link stretch that bursts it, 0 = never */
    float damping;      /* internal damping per second         */
    float gravity_scale;
    float friction;     /* multiplied by world friction        */
    float skin;         /* collision thickness of the outline  */
    float radius;       /* rough size, for the host            */
    int center;         /* local index of a center particle, or -1 */

    float minx, miny, maxx, maxy;
    int contacts;       /* contacts touched during the last step */
    float stretch_peak; /* largest link stretch in the last step */

    int tag;            /* free for the host */
    void *user;
} SpBody;

typedef struct {
    SpStaticKind kind;
    bool alive;
    float x0, y0, x1, y1;     /* capsule ends, or box min and max */
    float tx0, ty0, tx1, ty1; /* target for this step             */
    float dx0, dy0, dx1, dy1; /* motion per substep               */
    float radius;             /* capsule only                     */
    float friction;
    int tag;                  /* free for the host */
    bool outside_only;        /* only touches matter outside the arena */
    float hit_min;            /* measure impacts faster than this, 0 = don't */
    float hit[4];             /* impulse per struck face (SpSide) in the last step */
    float hit_x, hit_y;       /* where the latest of them landed */
} SpStatic;

/* The four arena walls, and the faces of a box. */
typedef enum { SP_TOP, SP_BOTTOM, SP_LEFT, SP_RIGHT } SpSide;

/* A wall with holes in it. Coordinates run along the wall. */
typedef struct {
    struct { float a, b; } gaps[SP_MAX_GAPS];
    int ngaps;
    float hit;     /* impulse taken in the last step */
    float hit_at;  /* where, weighted by impulse, until the step ends */
} SpWall;

typedef struct { int particle; float ox, oy; } SpGrab;

/* What a particle experiences at a point in an outside medium, such as a grid of
 * sand and water. The engine stays ignorant of what the medium actually is. */
typedef struct {
    float push_x, push_y;   /* move out of something solid */
    float accel_x, accel_y; /* buoyancy and the like */
    float drag;             /* velocity lost per second */
    float friction;         /* against the push surface */
} SpMediumSample;
typedef bool (*SpMediumFn)(void *user, float x, float y, SpMediumSample *out);

/* Base material for builders. Zero values fall back to defaults. */
typedef struct {
    float compliance;   /* link softness    */
    float match;        /* shape memory     */
    float damping;
    float friction;
    float gravity_scale;
    float mass;         /* per particle     */
    float spacing;      /* mesh resolution  */
} SpMaterial;

typedef struct SpWorld {
    /* particles, structure of arrays */
    int np, cap;
    float *x, *y, *qx, *qy, *vx, *vy;
    float *w;       /* inverse mass in use, 0 = pinned */
    float *w0;      /* inverse mass when not pinned    */
    float *r;       /* collision radius                */
    int *body;
    float *rho, *rhon; /* goo density scratch */

    SpLink *links; int nlinks, caplinks;
    SpBody bodies[SP_MAX_BODIES];
    SpStatic statics[SP_MAX_STATICS];
    int nstatics;

    /* candidate particle pairs for the current step */
    struct { int a, b; } *pairs; int npairs, cappairs;
    int *hash_head, *hash_next; int hash_cap;

    /* grabbing */
    SpGrab grabs[SP_MAX_GRABS]; int ngrabs;
    float grab_x, grab_y;

    /* radial force field, e.g. a blower */
    bool field_on; float field_x, field_y, field_radius, field_strength;

    /* optional medium, sampled for every particle each substep */
    SpMediumFn medium; void *medium_user;

    /* arena */
    float bx0, by0, bx1, by1, tbx0, tby0, tbx1, tby1;
    float wall_prev[4];   /* each wall's position when the last substep ended */

    /* Broken walls. Until one breaks, nothing is ever outside the arena. Once one
     * has, each wall is a thin barrier: solid from both sides, except at its gaps. */
    SpWall walls[4];
    bool open_bounds;
    float wall_hit_min;   /* measure wall impacts faster than this, 0 = don't */

    /* What contains matter outside the arena, such as a desktop. */
    bool has_desk;
    float desk[4], tdesk[4], ddesk[4]; /* indexed by SpSide; ddesk is motion per substep */
    float sub_h;          /* length of the current substep */

    /* tunables */
    float gx, gy;
    float air;        /* velocity loss per second, 0..          */
    float stiffness;  /* 0..1, 0.5 keeps authored materials     */
    float pressure;   /* global balloon pressure multiplier     */
    float friction;   /* global friction multiplier             */
    int substeps;
    float goo_h, goo_rest, goo_k, goo_near, goo_visc;

    /* output */
    SpEvent events[SP_MAX_EVENTS]; int nevents;
    float kinetic;    /* mean v^2/2 per particle after the step */
    long steps;
    bool need_compact;
} SpWorld;

/* lifecycle */
SpWorld *sp_world_new(void);
void sp_world_free(SpWorld *w);
void sp_world_clear(SpWorld *w);
void sp_step(SpWorld *w, float dt);
void sp_set_bounds(SpWorld *w, float x0, float y0, float x1, float y1, bool snap);

/* The arena is a box that moved by (dx, dy), and its velocity changed by (dvx, dvy).
 * Bodies in it keep their motion through space, so they feel the opposite of that
 * change. Bodies outside it are out in that space, so they don't budge: their
 * coordinates just slide by (-dx, -dy), as do the desk and outside-only statics. */
void sp_frame_move(SpWorld *w, float dx, float dy, float dvx, float dvy);
/* Outside the arena, matter is kept within this. */
void sp_set_desk(SpWorld *w, float x0, float y0, float x1, float y1, bool snap);
void sp_wall_open(SpWorld *w, SpSide side, float a, float b);
void sp_walls_repair(SpWorld *w);
bool sp_outside(const SpWorld *w, float x, float y);

/* builders: each returns a body slot or -1 when out of room */
int sp_add_polygon(SpWorld *w, const float *pts, int npts, const SpMaterial *m);
int sp_add_bitmap(SpWorld *w, const uint8_t *cells, int cw, int ch,
                  float x, float y, const SpMaterial *m);
int sp_add_balloon(SpWorld *w, float x, float y, float radius, const SpMaterial *m);
int sp_add_rope(SpWorld *w, float x0, float y0, float x1, float y1, const SpMaterial *m, bool pin_start);
int sp_add_cloth(SpWorld *w, float x, float y, int nx, int ny, const SpMaterial *m, int pin_every);
int sp_add_goo(SpWorld *w, float x, float y, float radius, int count);
int sp_add_link(SpWorld *w, int a, int b, float compliance, float tear, int body);

/* statics: each returns an index or -1 */
int sp_add_capsule(SpWorld *w, float x0, float y0, float x1, float y1, float radius, int tag);
int sp_add_box(SpWorld *w, float x0, float y0, float x1, float y1, int tag);
void sp_static_move(SpWorld *w, int i, float x0, float y0, float x1, float y1);
void sp_static_remove(SpWorld *w, int i);
int sp_static_at(const SpWorld *w, float x, float y, float slop, int tag);

/* interaction */
void sp_grab_begin(SpWorld *w, float x, float y, float radius);
void sp_grab_move(SpWorld *w, float x, float y);
void sp_grab_end(SpWorld *w);
void sp_blast(SpWorld *w, float x, float y, float radius, float impulse);
void sp_cut(SpWorld *w, float x0, float y0, float x1, float y1);
/* Pushes particles in the annulus r0 <= d < r1 outward. Sweep it to make a front. */
void sp_shockwave(SpWorld *w, float x, float y, float r0, float r1, float impulse, float falloff);
/* Bursts balloons in range and emits SP_EVENT_SPLAT for jelly centred in range.
 * Returns the splats emitted. Handle those events before calling it again. */
int sp_vaporize(SpWorld *w, float x, float y, float r);
/* Snaps rope and cloth links and pulls out pins within r. */
void sp_shatter(SpWorld *w, float x, float y, float r);
int sp_nearest_particle(const SpWorld *w, float x, float y, float max_dist);
void sp_toggle_pin(SpWorld *w, int particle);
int sp_body_at(const SpWorld *w, float x, float y);
void sp_body_remove(SpWorld *w, int body);
void sp_body_pop(SpWorld *w, int body);

/* queries */
void sp_body_centroid(const SpWorld *w, const SpBody *b, float *cx, float *cy);
float sp_body_area(const SpWorld *w, const SpBody *b);
bool sp_tri_alive(const SpWorld *w, const SpTri *t);
int sp_count_bodies(const SpWorld *w);
int sp_count_links(const SpWorld *w);
void sp_emit(SpWorld *w, SpEventKind kind, float x, float y, int body);

#endif
