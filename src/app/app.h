/* softbodies: state shared by the toy box, the renderer and the main loop. */
#ifndef APP_H
#define APP_H

#include "../softpute/softpute.h"
#include "../softwin/softwin.h"
#include "../softphys/softphys.h"

#define TAG_PANEL 1
#define TAG_WALL  2
#define TAG_SCENE 3
#define TAG_FRAME 4    /* what's left of a broken inner window */
#define TAG_ELEM  5    /* a widget still in its place in a broken inner window */
#define TAG_OUTSIDE 6  /* some other window, out on the desktop */

typedef enum { TOOL_GRAB, TOOL_SPAWN, TOOL_CUT, TOOL_BLOW, TOOL_BOMB, TOOL_PIN, TOOL_WALL, TOOL_ERASE, TOOL_TYPE, TOOL_NUKE, TOOL_MAGNET, TOOL_FREEZE, TOOL_GLUE, TOOL_COUNT } Tool;
typedef enum { ITEM_BLOB, ITEM_BOX, ITEM_STAR, ITEM_HEART, ITEM_HOPPER, ITEM_BALLOON, ITEM_HELIUM, ITEM_ROPE, ITEM_CLOTH, ITEM_GOO, ITEM_DONUT, ITEM_CRATE, ITEM_JELLYFISH, ITEM_SNAKE, ITEM_COUNT } Item;
typedef enum { VIEW_FLAT, VIEW_STRESS, VIEW_WIRE } ViewMode;
typedef enum { HOP_IDLE, HOP_CROUCH, HOP_AIR } HopState;

/* What the toy knows about a body beyond its physics. */
typedef struct {
    bool face;
    bool heart;
    bool hopper;
    int shade;
    int cloth_nx;
    float ttl;      /* seconds left, 0 = forever */
    float blink;    /* seconds to next blink, negative while blinking */
    float dizzy;    /* seconds of X eyes left */
    float phase;
    HopState hop;
    float hop_t;
    bool jellyfish;
    bool snake;
    int rope_width;  /* in world units, 0 = the usual */
    float scorch;    /* seconds spent somewhere much too hot */
    int elem;        /* which widget of an inner window this is, 0 = it isn't */
} Toy;

typedef enum { FX_RING, FX_TEXT } FxKind;
typedef struct { FxKind kind; float x, y, t, ttl, r; char text[16]; } Fx;
#define MAX_FX 64

/* A warhead: falls for NUKE_FALL seconds, then `t` counts on from detonation. */
#define NUKE_FALL 0.55f
#define NUKE_LIFE 5.0f
#define NUKE_FIREBALL 190.0f
typedef struct { bool active, exploded; float x, y, t, front; unsigned seed; } Nuke;
#define MAX_NUKES 4

typedef struct { int st; float px, py, len, angle, omega; } Rotor;
#define MAX_ROTORS 8

#define HISTORY 160
#define NSCENES 10
#define PHYS_NSCENES 8

typedef enum { MODE_SELECTOR, MODE_PHYS, MODE_BODIES, MODE_COUNT } ToyMode;
typedef enum { PT_DRAW, PT_ERASE, PT_LINE, PT_FILL, PT_HEAT, PT_COOL, PT_PUSH, PT_BLAST, PT_GRAB, PT_BODY, PT_NUKE, PT_COUNT } PhysTool;

typedef struct {
    PhysGrid *g;
    int cell;              /* framebuffer pixels per cell */
    float world_per_cell;
    int element;
    PhysTool tool;
    float brush, speed;
    bool temp_view;
    Item body_item;
    int scene;
    bool stroke, grabbing, line_on, hover_ok;
    int last_x, last_y, line_x, line_y, hover_x, hover_y;
    float slosh_vx, slosh_vy, slosh_x, slosh_y; /* the grid's own memory of being thrown */
} PhysUi;

typedef struct {
    SpWorld *world;        /* the two little previews */
    PhysGrid *grid;
    float t, acc, last_kick;
    int hover, focus;
} SelectorState;

#define TERM_LINES 200
#define TERM_COLS 62
#define TERM_INPUT 240
#define TERM_HISTORY 32
typedef struct {
    char lines[TERM_LINES][TERM_COLS + 1];
    int head, count;
    char input[TERM_INPUT];
    int len, pos;
    char hist[TERM_HISTORY][TERM_INPUT];
    int nhist, hist_at;
    bool focus;
    int held_key;          /* for key repeat */
    bool held_shift;
    double held_next;
} Term;

typedef struct {
    SwWindow *win;
    SwCanvas canvas;
    SwDesk desk;
    ToyMode mode;
    SpWorld *world;        /* whichever toy is running */
    Toy *toys;
    SpWorld *world_bodies, *world_phys;
    Toy toys_bodies[SP_MAX_BODIES], toys_phys[SP_MAX_BODIES];
    PhysUi phys;
    SelectorState sel;
    Term term;

    /* layout, framebuffer pixels */
    int ax, ay, aw, ah;   /* arena */
    int title_h, bar_h, status_h, edge;
    float vs;             /* framebuffer pixels per world unit */
    float world_w, world_h;

    /* inner windows */
    SwPanel p_tools, p_world, p_scope, p_help, p_about, p_term;
    SwPanel p_elements, p_lab, p_info;
    bool panel_memory[MODE_COUNT][4];  /* which of a toy's panels were open when you left */
    int solid[SW_MAX_PANELS];
    int sides[SW_MAX_PANELS][4]; /* a broken window's remaining frame, by SpSide */
    bool panels_placed;

    /* the window as a box: tracked for each toy's world */
    bool frame_seen[2];
    int frame_x[2], frame_y[2];
    float frame_vx[2], frame_vy[2];
    int outside_st[2][SW_MAX_OBSTACLES];
    /* the overlay */
    uint32_t *outer_seen;
    int dirty_x, dirty_y, dirty_w, dirty_h;
    bool outer_failed;
    int escaped;          /* particles out of the window, as of the last frame */
    float crack_t;

    /* toy state */
    Tool tool;
    Item item;
    float size;
    int scene;
    bool paused, bodies_started;
    int step_once;
    int glue_from;         /* particle waiting for its other end, or -1 */
    ViewMode view;
    bool show_points, show_faces, invert, solid_windows;
    bool grabbing;
    bool walling;
    float wall_x, wall_y;
    float cut_x, cut_y;
    bool cutting;
    float type_x, type_y;
    float mouse_x, mouse_y; /* world */
    bool mouse_in_arena;

    Rotor rotors[MAX_ROTORS]; int nrotors;
    Fx fx[MAX_FX];
    Nuke nukes[MAX_NUKES];
    float shake;          /* world units, decays */
    float shake_x, shake_y; /* framebuffer pixels, this frame */
    double blast_flash_at; /* sim time of the last flash */
    bool nuke_flash;

    /* readouts */
    float history[HISTORY]; int hist_pos;
    float fps, step_ms;
    float time_scale;
    double sim_time;
    char flash[80]; float flash_t;
    int shot_count;
    unsigned rng;
    bool quit;
} App;

/* toybox.c */
float app_rand(App *a);
void toys_reset(App *a);
int toy_spawn(App *a, Item item, float x, float y, float size);
int toy_letter(App *a, char ch, float x, float y, float cell);
int toy_word(App *a, const char *s, float x, float y, float cell);
void toys_update(App *a, float dt);
void toys_events(App *a);
void scene_load(App *a, int index);
const char *scene_name(int index);
const char *item_name(Item item);
void fx_ring(App *a, float x, float y, float r, float ttl);
void fx_text(App *a, float x, float y, const char *text);
void fx_update(App *a, float dt);
void app_flash(App *a, const char *msg);
bool nuke_launch(App *a, float x, float y);

/* main.c */
void app_set_mode(App *a, ToyMode mode);
const char *app_mode_name(ToyMode mode);
const char *app_tool_name(int tool);
void app_select_tool(App *a, Tool tool);
void app_screenshot(App *a);

/* breakage.c */
void brk_sync_panels(App *a);
void brk_before_step(App *a, float real_dt);
void brk_after_step(App *a);
void brk_idle(App *a);
void brk_world_cleared(App *a, SpWorld *w);
void brk_repair(App *a);
void brk_outer(App *a);
void brk_draw_holes(App *a);
bool brk_can_escape(const App *a);
bool brk_wall(App *a, SpSide side, float at, float half);
bool brk_panel(App *a, SwPanel *p, SpSide side);
int brk_count_gaps(const App *a);
int brk_count_broken(const App *a);
float brk_lowest(const App *a);

/* selector.c */
void selector_update(App *a, float dt);
void selector_draw(App *a);
void selector_key(App *a, int key);

/* phys_ui.c */
void phys_start(App *a);
void phys_layout(App *a);
void phys_tick(App *a, float dt);
void phys_render(App *a);
void phys_panels(App *a, SwPanel *p);
void phys_input(App *a);
void phys_key(App *a, int key);
void phys_load_scene(App *a, int index);
void phys_clear_world(App *a);
void phys_draw(SwCanvas *c, const PhysGrid *g, int ox, int oy, int cell, bool temp_view);
const char *phys_scene_name(int index);
const char *phys_tool_name(int tool);
const char *phys_status(App *a, char *buf, int n);

/* terminal.c */
void term_init(App *a);
void term_print(App *a, const char *fmt, ...);
void term_exec(App *a, const char *line);
void term_key(App *a, int key, bool shift);
void term_panel(App *a);

/* render.c */
void render_world(App *a);
void render_begin(App *a);
void render_matter(App *a);
void render_end(App *a);
void render_cursor(App *a);

#endif
