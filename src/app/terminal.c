/* The terminal: a small command line that reaches into whichever toy is running. */
#include "app.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ARGS 12

void term_print(App *a, const char *fmt, ...) {
    Term *t = &a->term;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    /* Long output wraps at the terminal's width. */
    const char *p = buf;
    do {
        char *line = t->lines[t->head];
        size_t n = strlen(p);
        if (n > TERM_COLS) n = TERM_COLS;
        memcpy(line, p, n);
        line[n] = 0;
        for (char *q = line; *q; q++) if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 32);
        t->head = (t->head + 1) % TERM_LINES;
        if (t->count < TERM_LINES) t->count++;
        p += n;
    } while (*p);
}

static bool want(App *a, ToyMode mode) {
    if (a->mode == mode) return true;
    term_print(a, "THAT NEEDS %s. TRY: TOY %s", app_mode_name(mode), app_mode_name(mode));
    return false;
}

static bool in_toy(App *a) {
    if (a->mode != MODE_SELECTOR) return true;
    term_print(a, "NO TOY IS RUNNING. TRY: TOYS");
    return false;
}

static float num(const char *s, float fallback) {
    if (!s) return fallback;
    char *end;
    float v = strtof(s, &end);
    return end == s ? fallback : v;
}

/* ---- variables ---- */

typedef struct { const char *name; float lo, hi; ToyMode mode; float *(*ptr)(App *); float scale; } Var;

static float *v_gravity(App *a) { return &a->world->gy; }
static float *v_wind(App *a) { return &a->world->gx; }
static float *v_stiff(App *a) { return &a->world->stiffness; }
static float *v_press(App *a) { return &a->world->pressure; }
static float *v_fric(App *a) { return &a->world->friction; }
static float *v_time(App *a) { return &a->time_scale; }
static float *v_size(App *a) { return &a->size; }
static float *v_brush(App *a) { return &a->phys.brush; }
static float *v_speed(App *a) { return &a->phys.speed; }

static const Var VARS[] = {
    { "GRAVITY", -1, 2, MODE_BODIES, v_gravity, 900 },
    { "WIND", -1, 1, MODE_BODIES, v_wind, 900 },
    { "STIFFNESS", 0, 1, MODE_BODIES, v_stiff, 1 },
    { "PRESSURE", 0.3f, 3.5f, MODE_BODIES, v_press, 1 },
    { "FRICTION", 0, 2, MODE_BODIES, v_fric, 1 },
    { "TIME", 0.05f, 1.5f, MODE_BODIES, v_time, 1 },
    { "SIZE", 14, 80, MODE_BODIES, v_size, 1 },
    { "BRUSH", 0, 24, MODE_PHYS, v_brush, 1 },
    { "SPEED", 1, 4, MODE_PHYS, v_speed, 1 },
};
#define NVARS (int)(sizeof VARS / sizeof *VARS)

/* ---- commands ---- */

typedef void (*CmdFn)(App *a, int argc, char **argv);
typedef struct { const char *name, *usage; CmdFn fn; } Cmd;
static const Cmd CMDS[];
static void run_line(App *a, char *line, int depth);

static void c_help(App *a, int argc, char **argv) {
    if (argc > 1) {
        for (const Cmd *c = CMDS; c->name; c++)
            if (!strcmp(c->name, argv[1])) { term_print(a, "%s %s", c->name, c->usage); return; }
        term_print(a, "NO COMMAND CALLED %s", argv[1]);
        return;
    }
    char row[TERM_COLS + 1] = "";
    for (const Cmd *c = CMDS; c->name; c++) {
        if (strlen(row) + strlen(c->name) + 1 > TERM_COLS) { term_print(a, "%s", row); row[0] = 0; }
        strcat(row, c->name);
        strcat(row, " ");
    }
    if (row[0]) term_print(a, "%s", row);
    term_print(a, "HELP NAME EXPLAINS ONE. ; SEPARATES COMMANDS.");
}

static void c_cls(App *a, int argc, char **argv) { (void)argc; (void)argv; a->term.count = 0; a->term.head = 0; }
static void c_echo(App *a, int argc, char **argv) {
    char out[TERM_COLS + 1] = "";
    for (int i = 1; i < argc; i++) {
        if (strlen(out) + strlen(argv[i]) + 2 > TERM_COLS) break;
        strcat(out, argv[i]);
        if (i + 1 < argc) strcat(out, " ");
    }
    term_print(a, "%s", out);
}
static void c_exit(App *a, int argc, char **argv) { (void)argc; (void)argv; a->quit = true; }
static void c_close(App *a, int argc, char **argv) { (void)argc; (void)argv; a->p_term.open = false; a->term.focus = false; }

static void c_toys(App *a, int argc, char **argv) {
    (void)argc; (void)argv;
    term_print(a, "SELECTOR  SOFTPHYS  SOFTBODIES");
    term_print(a, "NOW: %s", app_mode_name(a->mode));
}
static void c_toy(App *a, int argc, char **argv) {
    if (argc < 2) { c_toys(a, argc, argv); return; }
    for (int m = 0; m < MODE_COUNT; m++)
        if (!strcmp(argv[1], app_mode_name((ToyMode)m))) {
            app_set_mode(a, (ToyMode)m);
            term_print(a, "NOW: %s", app_mode_name(a->mode));
            return;
        }
    term_print(a, "NO TOY CALLED %s", argv[1]);
}

static void c_scene(App *a, int argc, char **argv) {
    if (!in_toy(a)) return;
    int n = a->mode == MODE_PHYS ? PHYS_NSCENES : NSCENES;
    if (argc < 2) {
        for (int i = 0; i < n; i++) term_print(a, "%s", a->mode == MODE_PHYS ? phys_scene_name(i) : scene_name(i));
        return;
    }
    int v = atoi(argv[1]);
    /* The tenth SoftBodies scene sits on the 0 key. */
    if (a->mode == MODE_BODIES && !strcmp(argv[1], "0")) v = 10;
    if (v < 1 || v > n) { term_print(a, "SCENES GO FROM 1 TO %d", n); return; }
    if (a->mode == MODE_PHYS) { phys_load_scene(a, v - 1); term_print(a, "%s", phys_scene_name(v - 1)); }
    else { scene_load(a, v - 1); term_print(a, "%s", scene_name(v - 1)); }
}

static void c_reset(App *a, int argc, char **argv) {
    (void)argc; (void)argv;
    if (!in_toy(a)) return;
    if (a->mode == MODE_PHYS) phys_load_scene(a, a->phys.scene);
    else scene_load(a, a->scene);
}
static void c_wipe(App *a, int argc, char **argv) {
    (void)argc; (void)argv;
    if (!in_toy(a)) return;
    if (a->mode == MODE_PHYS) { phys_clear(a->phys.g); phys_clear_world(a); }
    else toys_reset(a);
}
static void c_pause(App *a, int argc, char **argv) { (void)argc; (void)argv; a->paused = true; term_print(a, "PAUSED"); }
static void c_run(App *a, int argc, char **argv) { (void)argc; (void)argv; a->paused = false; term_print(a, "RUNNING"); }
static void c_step(App *a, int argc, char **argv) {
    a->paused = true;
    a->step_once += argc > 1 ? (int)num(argv[1], 1) : 1;
}
static void c_invert(App *a, int argc, char **argv) { (void)argc; (void)argv; a->invert = !a->invert; }
static void c_shot(App *a, int argc, char **argv) { (void)argc; (void)argv; app_screenshot(a); term_print(a, "%s", a->flash); }

static void c_stats(App *a, int argc, char **argv) {
    (void)argc; (void)argv;
    term_print(a, "TOY %s  %.0f FPS  STEP %.1f MS", app_mode_name(a->mode), (double)a->fps, (double)a->step_ms);
    term_print(a, "BACKEND %s  %dX%d  SCALE %.2f", sw_backend_name(a->win), a->win->w, a->win->h, (double)a->win->scale);
    if (a->mode == MODE_SELECTOR) return;
    term_print(a, "BODIES %d  POINTS %d  LINKS %d", sp_count_bodies(a->world), a->world->np, sp_count_links(a->world));
    if (a->mode == MODE_PHYS) {
        const PhysGrid *g = a->phys.g;
        term_print(a, "GRID %dX%d  TICK %d", g->w, g->h, g->tick);
        char row[TERM_COLS + 1] = "";
        for (int e = 1; e < PE_COUNT; e++) {
            /* Counted now, as the step's own tally can be out of date. */
            long total = phys_total(g, e);
            if (!total) continue;
            char item[32];
            snprintf(item, sizeof item, "%s %ld ", phys_info(e)->name, total);
            if (strlen(row) + strlen(item) > TERM_COLS) { term_print(a, "%s", row); row[0] = 0; }
            strcat(row, item);
        }
        if (row[0]) term_print(a, "%s", row);
    }
}

static void c_vars(App *a, int argc, char **argv) {
    (void)argc; (void)argv;
    bool any = false;
    for (int i = 0; i < NVARS; i++) {
        if (VARS[i].mode != a->mode) continue;
        term_print(a, "%-10s %.2f  (%.2f TO %.2f)", VARS[i].name, (double)(*VARS[i].ptr(a) / VARS[i].scale),
                   (double)VARS[i].lo, (double)VARS[i].hi);
        any = true;
    }
    if (!any) term_print(a, "NOTHING TO SET HERE");
}

static void c_set(App *a, int argc, char **argv) {
    if (argc < 2) { c_vars(a, argc, argv); return; }
    for (int i = 0; i < NVARS; i++) {
        const Var *v = &VARS[i];
        if (strcmp(v->name, argv[1])) continue;
        if (!want(a, v->mode)) return;
        if (argc > 2) {
            char *end;
            float value = strtof(argv[2], &end);
            if (end == argv[2]) { term_print(a, "%s ISN'T A NUMBER", argv[2]); return; }
            value = value < v->lo ? v->lo : value > v->hi ? v->hi : value;
            *v->ptr(a) = value * v->scale;
        }
        term_print(a, "%s = %.2f", v->name, (double)(*v->ptr(a) / v->scale));
        return;
    }
    term_print(a, "NO VARIABLE CALLED %s. TRY: VARS", argv[1]);
}

static void c_tools(App *a, int argc, char **argv) {
    (void)argc; (void)argv;
    if (!in_toy(a)) return;
    char row[TERM_COLS + 1] = "";
    int n = a->mode == MODE_PHYS ? PT_COUNT : TOOL_COUNT;
    for (int i = 0; i < n; i++) {
        strcat(row, a->mode == MODE_PHYS ? phys_tool_name(i) : app_tool_name(i));
        strcat(row, " ");
    }
    term_print(a, "%s", row);
}
static void c_tool(App *a, int argc, char **argv) {
    if (!in_toy(a)) return;
    if (argc < 2) { c_tools(a, argc, argv); return; }
    int n = a->mode == MODE_PHYS ? PT_COUNT : TOOL_COUNT;
    for (int i = 0; i < n; i++) {
        const char *name = a->mode == MODE_PHYS ? phys_tool_name(i) : app_tool_name(i);
        if (strcmp(name, argv[1])) continue;
        if (a->mode == MODE_PHYS) a->phys.tool = (PhysTool)i;
        else app_select_tool(a, (Tool)i);
        term_print(a, "TOOL: %s", name);
        return;
    }
    term_print(a, "NO TOOL CALLED %s. TRY: TOOLS", argv[1]);
}

static void c_items(App *a, int argc, char **argv) {
    (void)argc; (void)argv;
    char row[TERM_COLS + 1] = "";
    for (int i = 0; i < ITEM_COUNT; i++) {
        if (strlen(row) + strlen(item_name((Item)i)) + 1 > TERM_COLS) { term_print(a, "%s", row); row[0] = 0; }
        strcat(row, item_name((Item)i));
        strcat(row, " ");
    }
    term_print(a, "%s", row);
}

/* SPAWN works in both toys: both have a SoftPute world. */
static void c_spawn(App *a, int argc, char **argv) {
    if (!in_toy(a)) return;
    if (argc < 2) { term_print(a, "SPAWN ITEM [COUNT] [SIZE]. TRY: ITEMS"); return; }
    for (int i = 0; i < ITEM_COUNT; i++) {
        if (strcmp(item_name((Item)i), argv[1])) continue;
        int count = argc > 2 ? (int)num(argv[2], 1) : 1;
        float size = argc > 3 ? num(argv[3], a->size) : a->size;
        count = count < 1 ? 1 : count > 40 ? 40 : count;
        int made = 0;
        for (int k = 0; k < count; k++) {
            float x = a->world_w * (0.2f + app_rand(a) * 0.6f), y = a->world_h * (0.1f + app_rand(a) * 0.3f);
            if (toy_spawn(a, (Item)i, x, y, size) >= 0) made++;
        }
        term_print(a, "SPAWNED %d %s", made, argv[1]);
        return;
    }
    term_print(a, "NO ITEM CALLED %s. TRY: ITEMS", argv[1]);
}

static void c_nuke(App *a, int argc, char **argv) {
    if (!in_toy(a)) return;
    float x = argc > 1 ? num(argv[1], 0) : a->world_w * 0.5f, y = argc > 2 ? num(argv[2], 0) : a->world_h * 0.7f;
    if (nuke_launch(a, x, y)) term_print(a, "INBOUND");
}

static void c_blast(App *a, int argc, char **argv) {
    if (!in_toy(a)) return;
    float x = argc > 1 ? num(argv[1], 0) : a->world_w * 0.5f, y = argc > 2 ? num(argv[2], 0) : a->world_h * 0.5f;
    float r = argc > 3 ? num(argv[3], 160) : 160;
    sp_blast(a->world, x, y, r, 1500);
    fx_ring(a, x, y, r, 0.3f);
    if (a->mode == MODE_PHYS) {
        float wpc = a->phys.world_per_cell;
        phys_blast(a->phys.g, (int)(x / wpc), (int)(y / wpc), (int)(r / wpc / 3));
    }
}

/* ---- SoftPhys commands, in cell coordinates ---- */

static int element_arg(App *a, const char *s) {
    int e = s ? phys_element_by_name(s) : -1;
    if (e < 0) term_print(a, "NO ELEMENT CALLED %s. TRY: ELEMENTS", s ? s : "THAT");
    return e;
}

static void c_elements(App *a, int argc, char **argv) {
    (void)argc; (void)argv;
    char row[TERM_COLS + 1] = "";
    for (int e = 1; e < PE_COUNT; e++) {
        if (strlen(row) + strlen(phys_info(e)->name) + 1 > TERM_COLS) { term_print(a, "%s", row); row[0] = 0; }
        strcat(row, phys_info(e)->name);
        strcat(row, " ");
    }
    if (row[0]) term_print(a, "%s", row);
}
static void c_element(App *a, int argc, char **argv) {
    if (!want(a, MODE_PHYS)) return;
    if (argc < 2) { term_print(a, "ELEMENT: %s", phys_info(a->phys.element)->name); return; }
    int e = element_arg(a, argv[1]);
    if (e < 0) return;
    a->phys.element = e;
    a->phys.tool = PT_DRAW;
    term_print(a, "%s: %s", phys_info(e)->name, phys_info(e)->about);
}
static void c_paint(App *a, int argc, char **argv) {
    if (!want(a, MODE_PHYS)) return;
    if (argc < 4) { term_print(a, "PAINT ELEMENT X Y [RADIUS]"); return; }
    int e = element_arg(a, argv[1]);
    if (e >= 0) phys_paint(a->phys.g, atoi(argv[2]), atoi(argv[3]), argc > 4 ? atoi(argv[4]) : 3, e, true);
}
static void c_rect(App *a, int argc, char **argv) {
    if (!want(a, MODE_PHYS)) return;
    if (argc < 6) { term_print(a, "RECT ELEMENT X0 Y0 X1 Y1"); return; }
    int e = element_arg(a, argv[1]);
    if (e >= 0) phys_rect(a->phys.g, atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), e);
}
static void c_rain(App *a, int argc, char **argv) {
    if (!want(a, MODE_PHYS)) return;
    if (argc < 2) { term_print(a, "RAIN ELEMENT [DROPS]"); return; }
    int e = element_arg(a, argv[1]);
    if (e < 0) return;
    PhysGrid *g = a->phys.g;
    int n = argc > 2 ? atoi(argv[2]) : g->w / 2;
    for (int i = 0; i < n; i++) {
        int x = (int)(app_rand(a) * (float)g->w), y = (int)(app_rand(a) * (float)g->h * 0.2f);
        if (phys_get(g, x, y) == PE_EMPTY) phys_set(g, x, y, e);
    }
}
static void c_heat(App *a, int argc, char **argv) {
    if (!want(a, MODE_PHYS)) return;
    if (argc < 3) { term_print(a, "HEAT X Y [RADIUS] [AMOUNT]"); return; }
    phys_heat(a->phys.g, atoi(argv[1]), atoi(argv[2]), argc > 3 ? atoi(argv[3]) : 4, argc > 4 ? num(argv[4], 300) : 300);
}
static void c_probe(App *a, int argc, char **argv) {
    if (!want(a, MODE_PHYS)) return;
    if (argc < 3) { term_print(a, "PROBE X Y"); return; }
    int x = atoi(argv[1]), y = atoi(argv[2]);
    term_print(a, "%d,%d HOLDS %s AT %.0f DEGREES", x, y, phys_info(phys_get(a->phys.g, x, y))->name,
               (double)phys_temp(a->phys.g, x, y));
}
static void c_gravity(App *a, int argc, char **argv) {
    if (!want(a, MODE_PHYS)) return;
    if (argc > 1) {
        int v = atoi(argv[1]);
        a->phys.g->gravity = v > 0 ? 1 : v < 0 ? -1 : 0;
        a->world->gy = (float)a->phys.g->gravity * 900;
    }
    term_print(a, "GRAVITY %d", a->phys.g->gravity);
}
static void c_tempview(App *a, int argc, char **argv) {
    (void)argc; (void)argv;
    if (want(a, MODE_PHYS)) a->phys.temp_view = !a->phys.temp_view;
}

static void c_repeat(App *a, int argc, char **argv) {
    if (argc < 3) { term_print(a, "REPEAT N COMMAND"); return; }
    int n = atoi(argv[1]);
    n = n < 0 ? 0 : n > 200 ? 200 : n;
    char line[TERM_INPUT] = "";
    for (int i = 2; i < argc; i++) {
        if (strlen(line) + strlen(argv[i]) + 2 >= sizeof line) break;
        strcat(line, argv[i]);
        strcat(line, " ");
    }
    for (int i = 0; i < n && !a->quit; i++) {
        char copy[TERM_INPUT];
        memcpy(copy, line, sizeof copy);
        run_line(a, copy, 1);
    }
}

static const char *const SIDE_NAMES[4] = { "TOP", "BOTTOM", "LEFT", "RIGHT" };

static int side_arg(const char *s) {
    for (int i = 0; i < 4; i++) if (!strcmp(s, SIDE_NAMES[i])) return i;
    return -1;
}

static void c_break(App *a, int argc, char **argv) {
    if (a->mode == MODE_SELECTOR) { term_print(a, "NO TOY IS RUNNING"); return; }
    if (argc < 2) { term_print(a, "BREAK SIDE [AT] OR BREAK WINDOW [SIDE]"); return; }
    int side = side_arg(argv[1]);
    if (side >= 0) {
        bool across = side == SP_TOP || side == SP_BOTTOM;
        float at = argc > 2 ? num(argv[2], 0) : (across ? a->world_w : a->world_h) / 2;
        float half = argc > 3 ? num(argv[3], 90) : 90;
        if (!brk_wall(a, (SpSide)side, at, half)) term_print(a, "THIS WINDOW CANNOT SEE ITS DESKTOP");
        return;
    }
    if (a->mode != MODE_BODIES) { term_print(a, "THAT NEEDS SOFTBODIES"); return; }
    for (int i = 0; i < a->desk.npanels; i++) {
        SwPanel *p = a->desk.panels[i];
        if (strcmp(p->title, argv[1])) continue;
        int on = argc > 2 ? side_arg(argv[2]) : SP_BOTTOM;
        if (on < 0 || !brk_panel(a, p, (SpSide)on)) term_print(a, "THAT WINDOW WILL NOT BREAK");
        return;
    }
    term_print(a, "NOTHING CALLED %s TO BREAK", argv[1]);
}
static void c_repair(App *a, int argc, char **argv) {
    (void)argc; (void)argv;
    brk_repair(a);
}
static void c_borders(App *a, int argc, char **argv) {
    (void)argc; (void)argv;
    const SwWindow *win = a->win;
    term_print(a, "WINDOW AT %d,%d %s", win->sx, win->sy, win->has_pos ? "" : "(NOT KNOWN HERE)");
    if (win->has_desk) term_print(a, "DESKTOP %d,%d TO %d,%d", win->desk_x0, win->desk_y0, win->desk_x1, win->desk_y1);
    term_print(a, "HOLES %d  BROKEN WINDOWS %d", brk_count_gaps(a), brk_count_broken(a));
    term_print(a, "ESCAPED %d  LOWEST %.0f", a->escaped, (double)brk_lowest(a));
}

static const Cmd CMDS[] = {
    { "HELP", "[COMMAND]", c_help }, { "CLS", "- CLEARS THIS SCREEN", c_cls }, { "ECHO", "TEXT", c_echo },
    { "TOYS", "- LISTS THEM", c_toys }, { "TOY", "NAME - SWITCHES TOY", c_toy },
    { "SCENE", "[N] - LISTS OR LOADS", c_scene }, { "RESET", "- RELOADS THE SCENE", c_reset },
    { "WIPE", "- EMPTIES THE WORLD", c_wipe },
    { "PAUSE", "", c_pause }, { "RUN", "", c_run }, { "STEP", "[N] - ADVANCES N STEPS", c_step },
    { "SET", "NAME [VALUE] - READS OR CHANGES", c_set }, { "VARS", "- LISTS WHAT SET CAN CHANGE", c_vars },
    { "TOOL", "[NAME]", c_tool }, { "TOOLS", "", c_tools },
    { "SPAWN", "ITEM [COUNT] [SIZE]", c_spawn }, { "ITEMS", "", c_items },
    { "NUKE", "[X Y]", c_nuke }, { "BLAST", "[X Y [RADIUS]]", c_blast },
    { "ELEMENTS", "", c_elements }, { "ELEMENT", "[NAME] - PICKS WHAT YOU DRAW", c_element },
    { "PAINT", "ELEMENT X Y [RADIUS]", c_paint }, { "RECT", "ELEMENT X0 Y0 X1 Y1", c_rect },
    { "RAIN", "ELEMENT [DROPS]", c_rain }, { "HEAT", "X Y [RADIUS] [AMOUNT]", c_heat },
    { "PROBE", "X Y - READS ONE CELL", c_probe }, { "GRAVITY", "[-1 0 1] - SOFTPHYS ONLY", c_gravity },
    { "TEMPVIEW", "- TOGGLES THE HEAT VIEW", c_tempview },
    { "INVERT", "- SWAPS BLACK AND WHITE", c_invert }, { "SHOT", "- SAVES A PNG", c_shot },
    { "BREAK", "SIDE [AT] OR WINDOW [SIDE]", c_break }, { "REPAIR", "- MENDS EVERY WINDOW", c_repair },
    { "BORDERS", "- WHAT IS BROKEN", c_borders },
    { "STATS", "", c_stats }, { "REPEAT", "N COMMAND", c_repeat },
    { "CLOSE", "- HIDES THIS TERMINAL", c_close }, { "EXIT", "- QUITS", c_exit },
    { NULL, NULL, NULL },
};

/* ---- running a line ---- */

static void run_one(App *a, char *cmd, int depth) {
    char *argv[MAX_ARGS];
    int argc = 0;
    for (char *p = strtok(cmd, " \t"); p && argc < MAX_ARGS; p = strtok(NULL, " \t")) {
        argv[argc++] = p;
        for (char *q = p; *q; q++) if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 32);
    }
    if (!argc) return;
    (void)depth;
    for (const Cmd *c = CMDS; c->name; c++)
        if (!strcmp(c->name, argv[0])) { c->fn(a, argc, argv); return; }
    term_print(a, "NO COMMAND CALLED %s. TRY: HELP", argv[0]);
}

static void run_line(App *a, char *line, int depth) {
    /* REPEAT keeps the rest of its line to itself. */
    while (*line == ' ') line++;
    if (!strncasecmp(line, "REPEAT ", 7) && depth == 0) { run_one(a, line, depth); return; }
    char *save = line;
    while (save) {
        char *semi = strchr(save, ';');
        if (semi) *semi = 0;
        run_one(a, save, depth);
        save = semi ? semi + 1 : NULL;
    }
}

void term_exec(App *a, const char *line) {
    char copy[TERM_INPUT];
    snprintf(copy, sizeof copy, "%s", line);
    run_line(a, copy, 0);
}

/* ---- keys ---- */

static void term_submit(App *a) {
    Term *t = &a->term;
    t->input[t->len] = 0;
    term_print(a, "> %s", t->input);
    if (t->len) {
        if (!t->nhist || strcmp(t->hist[(t->nhist - 1) % TERM_HISTORY], t->input)) {
            snprintf(t->hist[t->nhist % TERM_HISTORY], TERM_INPUT, "%s", t->input);
            t->nhist++;
        }
        char copy[TERM_INPUT];
        memcpy(copy, t->input, TERM_INPUT);
        t->len = t->pos = 0;
        t->input[0] = 0;
        t->hist_at = t->nhist;
        run_line(a, copy, 0);
    }
}

static void term_recall(App *a, int dir) {
    Term *t = &a->term;
    int oldest = t->nhist > TERM_HISTORY ? t->nhist - TERM_HISTORY : 0;
    int at = t->hist_at + dir;
    if (at < oldest || at > t->nhist) return;
    t->hist_at = at;
    if (at == t->nhist) { t->len = t->pos = 0; t->input[0] = 0; return; }
    snprintf(t->input, TERM_INPUT, "%s", t->hist[at % TERM_HISTORY]);
    t->len = t->pos = (int)strlen(t->input);
}

void term_key(App *a, int key, bool shift) {
    Term *t = &a->term;
    switch (key) {
    case SW_KEY_ENTER: term_submit(a); return;
    case SW_KEY_ESC: t->focus = false; return;
    case SW_KEY_BACKSPACE:
        if (t->pos > 0) {
            memmove(t->input + t->pos - 1, t->input + t->pos, (size_t)(t->len - t->pos + 1));
            t->pos--;
            t->len--;
        }
        return;
    case SW_KEY_DELETE:
        if (t->pos < t->len) {
            memmove(t->input + t->pos, t->input + t->pos + 1, (size_t)(t->len - t->pos));
            t->len--;
        }
        return;
    case SW_KEY_LEFT: if (t->pos > 0) t->pos--; return;
    case SW_KEY_RIGHT: if (t->pos < t->len) t->pos++; return;
    case SW_KEY_HOME: t->pos = 0; return;
    case SW_KEY_END: t->pos = t->len; return;
    case SW_KEY_UP: term_recall(a, -1); return;
    case SW_KEY_DOWN: term_recall(a, 1); return;
    default: break;
    }
    char ch = sw_key_to_char(key);
    if (!ch || t->len >= TERM_INPUT - 1) return;
    if (shift) ch = sw_shifted_char(ch);
    if (!sw_glyph(ch)) return;
    memmove(t->input + t->pos + 1, t->input + t->pos, (size_t)(t->len - t->pos + 1));
    t->input[t->pos++] = ch;
    t->len++;
}

/* ---- drawing ---- */

void term_panel(App *a) {
    SwDesk *d = &a->desk;
    Term *t = &a->term;
    SwPanel *p = &a->p_term;
    if (!sw_panel_begin(d, p)) return;
    SwCanvas *c = &a->canvas;
    int x, y, w, u = d->dot, lh = u * 9, rows = 13;
    sw_reserve(d, lh * (rows + 1) + u * 2, &x, &y, &w);

    /* Clicking in gives it the keyboard, and clicking anywhere else takes it back. */
    bool over = d->hover_panel >= 0 && d->panels[d->hover_panel] == p;
    if (d->in.pressed[0]) t->focus = over;

    int show = t->count < rows ? t->count : rows;
    for (int i = 0; i < show; i++) {
        int idx = (t->head - show + i + TERM_LINES * 2) % TERM_LINES;
        sw_text(c, x, y + i * lh, t->lines[idx], u, SW_INK);
    }
    int ly = y + rows * lh + u;
    sw_fill(c, x, ly - u, w, d->line, SW_INK);
    sw_text(c, x, ly + u, ">", u, SW_INK);
    int cols = (w - u * 12) / (SW_CHAR_W * u), start = t->pos > cols ? t->pos - cols : 0;
    char view[TERM_INPUT];
    snprintf(view, sizeof view, "%.*s", cols + 1, t->input + start);
    int tx = x + u * 12;
    sw_text(c, tx, ly + u, view, u, SW_INK);
    /* The cursor is hollow when the keyboard is elsewhere. */
    int cx = tx + (t->pos - start) * SW_CHAR_W * u;
    bool blink = fmod(sw_now(), 1.0) < 0.6;
    if (t->focus && blink) sw_xor(c, cx - u / 2, ly, u * 6, u * 8);
    else if (!t->focus) sw_frame(c, cx - u / 2, ly, u * 6, u * 8, d->line, SW_INK);
    sw_panel_end(d);
}

void term_init(App *a) {
    term_print(a, "SOFT TOYS TERMINAL. TYPE HELP.");
}
