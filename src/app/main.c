/* softbodies: a soft body toy running on SoftPute, in a SoftWin window. */
#include "app.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WINDOW_TITLE "softbodies"
#define VERSION "1.0"

static const char *const TOOL_NAMES[TOOL_COUNT] = {
    "GRAB", "SPAWN", "CUT", "BLOW", "BOMB", "PIN", "WALL", "ERASE", "TYPE", "NUKE", "MAGNET", "FREEZE", "GLUE"
};

/* ---- layout -------------------------------------------------------------- */

static void layout(App *a) {
    SwWindow *win = a->win;
    SwDesk *d = &a->desk;
    a->canvas.px = win->fb;
    a->canvas.w = win->w;
    a->canvas.h = win->h;
    sw_noclip(&a->canvas);
    a->vs = win->scale > 0 ? win->scale : 1;
    sw_desk_metrics(d, a->vs);
    a->title_h = d->dot * 12;
    a->bar_h = d->dot * 11;
    a->status_h = d->dot * 11;
    a->edge = d->dot * 2;
    int frame = d->line;
    a->ax = frame;
    a->ay = frame + a->title_h + a->bar_h;
    a->aw = win->w - 2 * frame;
    a->ah = win->h - a->ay - a->status_h - frame;
    if (a->aw < 1) a->aw = 1;
    if (a->ah < 1) a->ah = 1;
    a->world_w = (float)a->aw / a->vs;
    a->world_h = (float)a->ah / a->vs;
    sp_set_bounds(a->world_bodies, 0, 0, a->world_w, a->world_h, false);
    sp_set_bounds(a->world_phys, 0, 0, a->world_w, a->world_h, false);
    phys_layout(a);
    sw_desk_bounds(d, a->ax, a->ay, a->ax + a->aw, a->ay + a->ah);

    int u = d->dot;
    a->p_tools.w = u * 112;
    a->p_world.w = u * 106;
    a->p_scope.w = u * 106;
    a->p_help.w = u * 140;
    a->p_about.w = u * 128;
    a->p_term.w = u * (TERM_COLS * SW_CHAR_W + 8);
    a->p_elements.w = u * 112;
    a->p_lab.w = u * 106;
    a->p_info.w = u * 106;
    if (!a->panels_placed) {
        int m = u * 4;
        a->p_tools.x = a->ax + m;
        a->p_tools.y = a->ay + m;
        a->p_world.x = a->ax + a->aw - a->p_world.w - m;
        a->p_world.y = a->ay + m;
        a->p_scope.x = a->p_world.x;
        a->p_scope.y = a->ay + m + u * 178;
        a->p_help.x = a->ax + (a->aw - a->p_help.w) / 2;
        a->p_help.y = a->ay + u * 30;
        a->p_about.x = a->ax + (a->aw - a->p_about.w) / 2;
        a->p_about.y = a->ay + u * 44;
        a->p_term.x = a->ax + (a->aw - a->p_term.w) / 2;
        a->p_term.y = a->ay + a->ah - u * 158;
        a->p_elements.x = a->ax + m;
        a->p_elements.y = a->ay + m;
        a->p_lab.x = a->ax + a->aw - a->p_lab.w - m;
        a->p_lab.y = a->ay + m;
        a->p_info.x = a->p_lab.x;
        a->p_info.y = a->ay + m + u * 192;
        a->panels_placed = true;
    }
}

/* ---- inner windows --------------------------------------------------------- */

static void select_tool(App *a, Tool t) {
    a->tool = t;
    a->glue_from = -1;
    a->grabbing = false;
    a->walling = false;
    a->cutting = false;
    sp_grab_end(a->world);
    a->world->field_on = false;
}

static void panel_tools(App *a) {
    SwDesk *d = &a->desk;
    if (!sw_panel_begin(d, &a->p_tools)) return;
    static const Tool order[] = { TOOL_GRAB, TOOL_CUT, TOOL_BLOW, TOOL_BOMB, TOOL_PIN, TOOL_WALL, TOOL_ERASE, TOOL_TYPE };
    sw_heading(d, "TOOL");
    for (int i = 0; i < 8; i++)
        if (sw_button_half(d, TOOL_NAMES[order[i]], a->tool == order[i], i & 1)) select_tool(a, order[i]);
    if (sw_button(d, TOOL_NAMES[TOOL_NUKE], a->tool == TOOL_NUKE)) select_tool(a, TOOL_NUKE);
    if (sw_button_half(d, TOOL_NAMES[TOOL_MAGNET], a->tool == TOOL_MAGNET, false)) select_tool(a, TOOL_MAGNET);
    if (sw_button_half(d, TOOL_NAMES[TOOL_FREEZE], a->tool == TOOL_FREEZE, true)) select_tool(a, TOOL_FREEZE);
    if (sw_button(d, TOOL_NAMES[TOOL_GLUE], a->tool == TOOL_GLUE)) select_tool(a, TOOL_GLUE);
    sw_space(d, 2);
    sw_heading(d, "SPAWN");
    for (int i = 0; i < ITEM_COUNT; i++)
        if (sw_button_half(d, item_name((Item)i), a->tool == TOOL_SPAWN && a->item == (Item)i, i & 1)) {
            select_tool(a, TOOL_SPAWN);
            a->item = (Item)i;
        }
    sw_space(d, 1);
    sw_slider(d, "SIZE", &a->size, 14, 80, "%.0f");
    sw_panel_end(d);
}

static void panel_world(App *a) {
    SwDesk *d = &a->desk;
    if (!sw_panel_begin(d, &a->p_world)) return;
    SpWorld *w = a->world;
    float g = w->gy / 900.0f, subs = (float)w->substeps, gx = w->gx / 900.0f;
    if (sw_slider(d, "GRAVITY", &g, -1, 2, "%.2f")) w->gy = g * 900;
    if (sw_slider(d, "WIND", &gx, -1, 1, "%.2f")) w->gx = gx * 900;
    sw_slider(d, "STIFFNESS", &w->stiffness, 0, 1, "%.2f");
    sw_slider(d, "PRESSURE", &w->pressure, 0.3f, 3.5f, "%.2f");
    sw_slider(d, "FRICTION", &w->friction, 0, 2, "%.2f");
    sw_slider(d, "TIME", &a->time_scale, 0.05f, 1.5f, "%.2f");
    if (sw_slider(d, "SUBSTEPS", &subs, 2, 24, "%.0f")) w->substeps = (int)(subs + 0.5f);
    if (sw_button_half(d, a->paused ? "RUN" : "PAUSE", a->paused, false)) a->paused = !a->paused;
    if (sw_button_half(d, "STEP", false, true)) { a->paused = true; a->step_once++; }
    if (sw_button_half(d, "RESET", false, false)) scene_load(a, a->scene);
    if (sw_button_half(d, "CLEAR", false, true)) {
        toys_reset(a);
    }
    sw_panel_end(d);
}

static void panel_scope(App *a) {
    SwDesk *d = &a->desk;
    if (!sw_panel_begin(d, &a->p_scope)) return;
    SwCanvas *c = &a->canvas;
    SpWorld *w = a->world;
    int x, y, gw, gh = d->dot * 26;
    sw_reserve(d, gh, &x, &y, &gw);
    sw_frame(c, x, y, gw, gh, d->line, SW_INK);

    /* One series on one axis: square-rooted, so calm and chaos both read. */
    float top = 1;
    for (int i = 0; i < HISTORY; i++) if (a->history[i] > top) top = a->history[i];
    float rtop = sqrtf(top);
    int inner_w = gw - 2 * d->line, inner_h = gh - 2 * d->line - d->line * 2;
    int px = 0, py = 0;
    for (int i = 0; i < HISTORY; i++) {
        float v = a->history[(a->hist_pos + i) % HISTORY];
        int gx = x + d->line + i * inner_w / (HISTORY - 1);
        int gy = y + gh - d->line * 2 - (int)(sqrtf(v) / rtop * (float)inner_h);
        if (i) sw_line(c, px, py, gx, gy, d->line * 2, SW_INK);
        px = gx; py = gy;
    }
    char buf[64];
    bool over = d->hover_panel >= 0 && d->panels[d->hover_panel] == &a->p_scope &&
                d->in.mx >= x && d->in.mx < x + gw && d->in.my >= y && d->in.my < y + gh;
    if (over) {
        /* Hover: a crosshair and that moment's value. */
        int i = (d->in.mx - x - d->line) * (HISTORY - 1) / (inner_w > 1 ? inner_w : 1);
        i = i < 0 ? 0 : i >= HISTORY ? HISTORY - 1 : i;
        float v = a->history[(a->hist_pos + i) % HISTORY];
        sw_xor(c, d->in.mx, y + d->line, d->line, gh - 2 * d->line);
        snprintf(buf, sizeof buf, "T-%.1fS  %.0f", (double)(HISTORY - 1 - i) / 30.0, (double)v);
    } else {
        float now = a->history[(a->hist_pos + HISTORY - 1) % HISTORY];
        snprintf(buf, sizeof buf, "ENERGY  %.0f", (double)now);
    }
    sw_label(d, buf);
    snprintf(buf, sizeof buf, "BODIES  %d", sp_count_bodies(w));
    sw_label(d, buf);
    snprintf(buf, sizeof buf, "POINTS  %d", w->np);
    sw_label(d, buf);
    snprintf(buf, sizeof buf, "LINKS   %d", sp_count_links(w));
    sw_label(d, buf);
    sw_panel_end(d);
}

static void panel_help(App *a) {
    SwDesk *d = &a->desk;
    if (!sw_panel_begin(d, &a->p_help)) return;
    static const char *const bodies[] = {
        "LEFT   USE TOOL",
        "RIGHT  BLAST AT CURSOR",
        "WHEEL  SPAWN SIZE",
        "",
        "G GRAB    X CUT     B BLOW",
        "M BOMB    I PIN     W WALL",
        "E ERASE   T TYPE    K NUKE",
        "A MAGNET  Z FREEZE  J GLUE",
        "ESC BACK TO GRAB",
        "",
        "1-0    LOAD SCENE",
        "R      RESET SCENE",
        "C      CLEAR WORLD",
        "SPACE  PAUSE",
        "N      STEP ONE FRAME",
        "V      CYCLE VIEW",
        "TAB    HIDE ALL WINDOWS",
        "P      SCREENSHOT PNG",
        "`      TERMINAL",
        "F1     THIS WINDOW",
        "CTRL+Q QUIT",
        NULL,
    };
    static const char *const phys[] = {
        "LEFT   USE TOOL",
        "RIGHT  SMALL EXPLOSION",
        "WHEEL  BRUSH SIZE",
        "",
        "D DRAW    E ERASE   L LINE",
        "F FILL    H HEAT    O COOL",
        "U PUSH    B BLAST   G GRAB",
        "T THROW   K NUKE",
        "",
        "1-8    LOAD SCENE",
        "R      RESET SCENE",
        "C      CLEAR WORLD",
        "V      HEAT VIEW",
        "SPACE  PAUSE",
        "N      STEP ONE FRAME",
        "TAB    HIDE ALL WINDOWS",
        "P      SCREENSHOT PNG",
        "`      TERMINAL",
        "F1     THIS WINDOW",
        "CTRL+Q QUIT",
        NULL,
    };
    static const char *const sel[] = {
        "1 2    OPEN A TOY",
        "ARROWS PICK ONE",
        "ENTER  OPEN IT",
        "`      TERMINAL",
        "F1     THIS WINDOW",
        "CTRL+Q QUIT",
        NULL,
    };
    const char *const *lines = a->mode == MODE_PHYS ? phys : a->mode == MODE_BODIES ? bodies : sel;
    for (int i = 0; lines[i]; i++) {
        if (lines[i][0]) sw_label(d, lines[i]);
        else sw_space(d, 4);
    }
    sw_panel_end(d);
}

static void panel_about(App *a) {
    SwDesk *d = &a->desk;
    if (!sw_panel_begin(d, &a->p_about)) return;
    char buf[64];
    sw_label(d, "SOFT TOYS " VERSION);
    sw_space(d, 3);
    sw_heading(d, "PHYSICS");
    sw_label(d, "SOFTPUTE");
    sw_label(d, "XPBD, SHAPE MATCHING,");
    sw_label(d, "PRESSURE, DENSITY FLUID");
    sw_space(d, 3);
    sw_heading(d, "WINDOW SYSTEM");
    sw_label(d, "SOFTWIN");
    snprintf(buf, sizeof buf, "BACKEND: %s", sw_backend_name(a->win));
    sw_label(d, buf);
    sw_label(d, "NO TOOLKIT UNDERNEATH.");
    snprintf(buf, sizeof buf, "SCALE %.2f  %dX%d", (double)a->win->scale, a->win->w, a->win->h);
    sw_label(d, buf);
    sw_panel_end(d);
}

/* ---- chrome ------------------------------------------------------------------ */

/* Returns true when the button was pressed. */
static bool title_button(App *a, int x, int y, int s, char kind) {
    SwCanvas *c = &a->canvas;
    SwDesk *d = &a->desk;
    bool over = d->in.inside && d->in.mx >= x && d->in.mx < x + s && d->in.my >= y && d->in.my < y + s;
    sw_fill(c, x, y, s, s, over ? SW_PAPER : SW_INK);
    sw_frame(c, x, y, s, s, d->line, over ? SW_INK : SW_PAPER);
    SwColor fg = over ? SW_INK : SW_PAPER;
    int m = s / 4 + d->line;
    if (kind == 'x') {
        sw_line(c, x + m, y + m, x + s - m - 1, y + s - m - 1, d->line + 1, fg);
        sw_line(c, x + s - m - 1, y + m, x + m, y + s - m - 1, d->line + 1, fg);
    } else {
        sw_fill(c, x + m, y + s - m - d->line * 2, s - 2 * m, d->line * 2, fg);
    }
    if (over) d->mouse_on_ui = true;
    return over && d->in.pressed[0];
}

static void draw_chrome(App *a) {
    SwCanvas *c = &a->canvas;
    SwDesk *d = &a->desk;
    SwWindow *win = a->win;
    int f = d->line, w = win->w, h = win->h, u = d->dot;

    /* title bar */
    sw_fill(c, 0, 0, w, f + a->title_h, SW_INK);
    const char *title = a->mode == MODE_SELECTOR ? "SOFT TOYS" : app_mode_name(a->mode);
    int tw = sw_text_w(title, u);
    sw_text(c, u * 4, f + (a->title_h - 7 * u) / 2, title, u, SW_PAPER);
    int bs = a->title_h - u * 3;
    int bx = w - f - u * 2 - bs, by = f + (a->title_h - bs) / 2;
    /* Hatched grab area between the name and the buttons. */
    int hx0 = u * 4 + tw + u * 4, hx1 = bx - bs - u * 5;
    for (int r = 0; r < 3; r++)
        sw_fill(c, hx0, by + bs * (r + 1) / 4 - d->line / 2, hx1 - hx0 > 0 ? hx1 - hx0 : 0, d->line, SW_PAPER);
    if (title_button(a, bx, by, bs, 'x')) a->quit = true;
    if (title_button(a, bx - bs - u * 2, by, bs, '_')) {
        sw_minimize(win);
        d->in.down[0] = false;
    }
    bool on_title = d->in.inside && d->in.my < f + a->title_h && d->in.mx < bx - bs - u * 2;
    if (on_title) d->mouse_on_ui = true;
    if (on_title && d->in.pressed[0]) {
        sw_begin_move(win);
        d->in.down[0] = false;
    }

    /* status bar */
    int sy = h - f - a->status_h;
    sw_fill(c, f, sy, w - 2 * f, a->status_h, SW_PAPER);
    sw_fill(c, f, sy, w - 2 * f, d->line, SW_INK);
    char left[128], right[96];
    if (a->flash_t > 0) snprintf(left, sizeof left, "%s", a->flash);
    else if (a->term.focus && a->p_term.open) snprintf(left, sizeof left, "TERMINAL HAS THE KEYBOARD  ESC TO LEAVE");
    else if (a->mode == MODE_SELECTOR) snprintf(left, sizeof left, "PICK A TOY");
    else if (a->mode == MODE_PHYS) phys_status(a, left, sizeof left);
    else if (a->tool == TOOL_SPAWN) snprintf(left, sizeof left, "SPAWN %s  SIZE %.0f", item_name(a->item), (double)a->size);
    else if (a->tool == TOOL_TYPE) snprintf(left, sizeof left, "TYPE LETTERS  ESC TO STOP");
    else snprintf(left, sizeof left, "%s", TOOL_NAMES[a->tool]);
    snprintf(right, sizeof right, "%s%.1fMS  %.0fFPS", a->paused ? "PAUSED  " : "", (double)a->step_ms, (double)a->fps);
    int ty = sy + (a->status_h - 7 * u) / 2 + d->line / 2;
    sw_text(c, f + u * 3, ty, left, u, SW_INK);
    int grip = a->status_h;
    sw_text(c, w - f - grip - u * 2 - sw_text_w(right, u), ty, right, u, SW_INK);

    /* resize grip: a staircase of hard squares */
    for (int i = 0; i < 4; i++)
        for (int j = 0; j <= i; j++)
            sw_fill(c, w - f - u * (2 + (3 - j) * 2) , sy + u * (2 + i * 2) + d->line, u, u, SW_INK);

    /* window outline, drawn last so nothing crosses it */
    sw_frame(c, 0, 0, w, h, f, SW_INK);
}

static int edges_at(const App *a, int x, int y) {
    int e = 0, z = a->edge, w = a->win->w, h = a->win->h;
    if (x < z) e |= SW_EDGE_LEFT;
    if (x >= w - z) e |= SW_EDGE_RIGHT;
    if (y < z) e |= SW_EDGE_TOP;
    if (y >= h - z) e |= SW_EDGE_BOTTOM;
    /* The grip in the corner of the status bar. */
    if (x >= w - a->status_h && y >= h - a->status_h) e = SW_EDGE_RIGHT | SW_EDGE_BOTTOM;
    return e;
}

static void toy_menu(App *a) {
    SwDesk *d = &a->desk;
    if (sw_menu_begin(d, "TOY", 16)) {
        if (sw_menu_item(d, "SOFTPHYS", a->mode == MODE_PHYS)) app_set_mode(a, MODE_PHYS);
        if (sw_menu_item(d, "SOFTBODIES", a->mode == MODE_BODIES)) app_set_mode(a, MODE_BODIES);
        sw_menu_sep(d);
        if (sw_menu_item(d, "SELECTOR", a->mode == MODE_SELECTOR)) app_set_mode(a, MODE_SELECTOR);
        sw_menu_end(d);
    }
}

static void window_menu_common(App *a) {
    SwDesk *d = &a->desk;
    if (sw_menu_item(d, "TERMINAL", a->p_term.open)) {
        a->p_term.open = !a->p_term.open;
        a->term.focus = a->p_term.open;
    }
    if (sw_menu_item(d, "KEYS", a->p_help.open)) a->p_help.open = !a->p_help.open;
    if (sw_menu_item(d, "ABOUT", a->p_about.open)) a->p_about.open = !a->p_about.open;
    sw_menu_sep(d);
    if (sw_menu_item(d, "REPAIR", false)) brk_repair(a);
}

static void draw_menus(App *a) {
    SwDesk *d = &a->desk;
    sw_bar_begin(d, d->line, d->line + a->title_h, a->win->w - 2 * d->line, a->bar_h);
    toy_menu(a);
    if (a->mode == MODE_BODIES) {
        if (sw_menu_begin(d, "SCENE", 16)) {
            for (int i = 0; i < NSCENES; i++)
                if (sw_menu_item(d, scene_name(i), a->scene == i)) scene_load(a, i);
            sw_menu_sep(d);
            if (sw_menu_item(d, "RESET", false)) scene_load(a, a->scene);
            if (sw_menu_item(d, "CLEAR", false)) toys_reset(a);
            sw_menu_end(d);
        }
        if (sw_menu_begin(d, "VIEW", 18)) {
            if (sw_menu_item(d, "SOLID", a->view == VIEW_FLAT)) a->view = VIEW_FLAT;
            if (sw_menu_item(d, "STRESS", a->view == VIEW_STRESS)) a->view = VIEW_STRESS;
            if (sw_menu_item(d, "WIREFRAME", a->view == VIEW_WIRE)) a->view = VIEW_WIRE;
            sw_menu_sep(d);
            if (sw_menu_item(d, "FACES", a->show_faces)) a->show_faces = !a->show_faces;
            if (sw_menu_item(d, "POINTS", a->show_points)) a->show_points = !a->show_points;
            if (sw_menu_item(d, "INVERT", a->invert)) a->invert = !a->invert;
            if (sw_menu_item(d, "NUKE FLASH", a->nuke_flash)) a->nuke_flash = !a->nuke_flash;
            sw_menu_end(d);
        }
        if (sw_menu_begin(d, "WINDOW", 18)) {
            if (sw_menu_item(d, "TOOLBOX", a->p_tools.open)) a->p_tools.open = !a->p_tools.open;
            if (sw_menu_item(d, "WORLD", a->p_world.open)) a->p_world.open = !a->p_world.open;
            if (sw_menu_item(d, "SCOPE", a->p_scope.open)) a->p_scope.open = !a->p_scope.open;
            sw_menu_sep(d);
            if (sw_menu_item(d, "SOLID WINDOWS", a->solid_windows)) a->solid_windows = !a->solid_windows;
            sw_menu_sep(d);
            window_menu_common(a);
            sw_menu_end(d);
        }
    } else if (a->mode == MODE_PHYS) {
        if (sw_menu_begin(d, "SCENE", 16)) {
            for (int i = 0; i < PHYS_NSCENES; i++)
                if (sw_menu_item(d, phys_scene_name(i), a->phys.scene == i)) phys_load_scene(a, i);
            sw_menu_sep(d);
            if (sw_menu_item(d, "RESET", false)) phys_load_scene(a, a->phys.scene);
            if (sw_menu_item(d, "CLEAR", false)) { phys_clear(a->phys.g); phys_clear_world(a); }
            sw_menu_end(d);
        }
        if (sw_menu_begin(d, "VIEW", 18)) {
            if (sw_menu_item(d, "HEAT VIEW", a->phys.temp_view)) a->phys.temp_view = !a->phys.temp_view;
            if (sw_menu_item(d, "FACES", a->show_faces)) a->show_faces = !a->show_faces;
            if (sw_menu_item(d, "INVERT", a->invert)) a->invert = !a->invert;
            if (sw_menu_item(d, "NUKE FLASH", a->nuke_flash)) a->nuke_flash = !a->nuke_flash;
            sw_menu_end(d);
        }
        if (sw_menu_begin(d, "THROW", 14)) {
            static const Item items[] = { ITEM_BLOB, ITEM_BOX, ITEM_BALLOON, ITEM_CRATE, ITEM_DONUT, ITEM_ROPE, ITEM_JELLYFISH, ITEM_SNAKE };
            for (int i = 0; i < 8; i++)
                if (sw_menu_item(d, item_name(items[i]), a->phys.tool == PT_BODY && a->phys.body_item == items[i])) {
                    a->phys.body_item = items[i];
                    a->phys.tool = PT_BODY;
                }
            sw_menu_end(d);
        }
        if (sw_menu_begin(d, "WINDOW", 18)) {
            if (sw_menu_item(d, "ELEMENTS", a->p_elements.open)) a->p_elements.open = !a->p_elements.open;
            if (sw_menu_item(d, "LAB", a->p_lab.open)) a->p_lab.open = !a->p_lab.open;
            if (sw_menu_item(d, "INFO", a->p_info.open)) a->p_info.open = !a->p_info.open;
            sw_menu_sep(d);
            window_menu_common(a);
            sw_menu_end(d);
        }
    } else {
        if (sw_menu_begin(d, "VIEW", 18)) {
            if (sw_menu_item(d, "INVERT", a->invert)) a->invert = !a->invert;
            sw_menu_end(d);
        }
        if (sw_menu_begin(d, "WINDOW", 18)) {
            window_menu_common(a);
            sw_menu_end(d);
        }
    }
    sw_bar_end(d);
}

/* ---- toys ------------------------------------------------------------------- */

const char *app_mode_name(ToyMode mode) {
    static const char *const n[MODE_COUNT] = { "SELECTOR", "SOFTPHYS", "SOFTBODIES" };
    return n[mode];
}

const char *app_tool_name(int tool) { return TOOL_NAMES[tool]; }
void app_select_tool(App *a, Tool t) { select_tool(a, t); }

static int mode_panels(App *a, ToyMode m, SwPanel **out) {
    if (m == MODE_BODIES) { out[0] = &a->p_tools; out[1] = &a->p_world; out[2] = &a->p_scope; return 3; }
    if (m == MODE_PHYS) { out[0] = &a->p_elements; out[1] = &a->p_lab; out[2] = &a->p_info; return 3; }
    return 0;
}

/* Each toy keeps its world, its windows and its state while you're elsewhere. */
void app_set_mode(App *a, ToyMode mode) {
    if (mode == a->mode) return;
    SwPanel *list[4];
    int n = mode_panels(a, a->mode, list);
    for (int i = 0; i < n; i++) {
        a->panel_memory[a->mode][i] = list[i]->open;
        list[i]->open = false;
    }
    /* Whatever was being held or thrown stays in the toy it belongs to. */
    sp_grab_end(a->world);
    a->world->field_on = false;
    a->grabbing = a->phys.grabbing = a->cutting = a->walling = false;
    memset(a->nukes, 0, sizeof a->nukes);
    memset(a->fx, 0, sizeof a->fx);
    a->shake = 0;
    a->paused = false;
    a->step_once = 0;

    a->mode = mode;
    if (mode == MODE_PHYS) {
        a->world = a->world_phys;
        a->toys = a->toys_phys;
    } else {
        a->world = a->world_bodies;
        a->toys = a->toys_bodies;
    }
    n = mode_panels(a, mode, list);
    for (int i = 0; i < n; i++) list[i]->open = a->panel_memory[mode][i];
    if (mode == MODE_PHYS) phys_start(a);
    if (mode == MODE_BODIES && !a->bodies_started) {
        a->bodies_started = true;
        scene_load(a, a->scene);
    }
    brk_idle(a); /* where the window got to while this toy wasn't looking */
}

/* ---- input ------------------------------------------------------------------- */

static void to_world(const App *a, int sx, int sy, float *x, float *y) {
    *x = (float)(sx - a->ax) / a->vs;
    *y = (float)(sy - a->ay) / a->vs;
}

static void arena_input(App *a) {
    SwDesk *d = &a->desk;
    SpWorld *w = a->world;
    const SwInput *in = &d->in;
    bool free_mouse = !d->mouse_on_ui && in->inside;
    to_world(a, in->mx, in->my, &a->mouse_x, &a->mouse_y);
    a->mouse_in_arena = free_mouse && a->mouse_x >= 0 && a->mouse_y >= 0 && a->mouse_x < a->world_w && a->mouse_y < a->world_h;
    bool in_arena = a->mouse_in_arena;

    /* The mouse keeps driving a tool it already holds, even over the chrome. */
    if (a->grabbing) sp_grab_move(w, a->mouse_x, a->mouse_y);
    if (a->tool != TOOL_BLOW && a->tool != TOOL_MAGNET) w->field_on = false;

    if (in_arena && in->pressed[1]) {
        sp_blast(w, a->mouse_x, a->mouse_y, 120, 900);
        fx_ring(a, a->mouse_x, a->mouse_y, 120, 0.25f);
    }
    if (in_arena && in->wheel && a->tool == TOOL_SPAWN) {
        a->size -= (float)in->wheel * 3;
        a->size = a->size < 14 ? 14 : a->size > 80 ? 80 : a->size;
    }
    switch (a->tool) {
    case TOOL_GRAB:
        if (in_arena && in->pressed[0]) {
            sp_grab_begin(w, a->mouse_x, a->mouse_y, 26);
            a->grabbing = w->ngrabs > 0;
        }
        if (a->grabbing && !in->down[0]) {
            sp_grab_end(w);
            a->grabbing = false;
        }
        break;
    case TOOL_SPAWN:
        if (in_arena && in->pressed[0]) toy_spawn(a, a->item, a->mouse_x, a->mouse_y, a->size);
        break;
    case TOOL_CUT:
        if (in_arena && in->pressed[0]) {
            a->cutting = true;
            a->cut_x = a->mouse_x;
            a->cut_y = a->mouse_y;
        }
        if (a->cutting && in->down[0]) {
            sp_cut(w, a->cut_x, a->cut_y, a->mouse_x, a->mouse_y);
            a->cut_x += (a->mouse_x - a->cut_x) * 0.6f;
            a->cut_y += (a->mouse_y - a->cut_y) * 0.6f;
            /* sp_cut reports splats; handle them now, not on the next step. */
            toys_events(a);
        }
        if (!in->down[0]) a->cutting = false;
        break;
    case TOOL_BLOW:
        w->field_on = in->down[0] && (in_arena || w->field_on);
        w->field_x = a->mouse_x;
        w->field_y = a->mouse_y;
        w->field_radius = 240;
        w->field_strength = 6000;
        break;
    case TOOL_BOMB:
        if (in_arena && in->pressed[0]) {
            sp_blast(w, a->mouse_x, a->mouse_y, 150, 1800);
            fx_ring(a, a->mouse_x, a->mouse_y, 150, 0.4f);
            fx_text(a, a->mouse_x - 26, a->mouse_y - 10, "BOOM");
        }
        break;
    case TOOL_NUKE:
        if (in_arena && in->pressed[0]) nuke_launch(a, a->mouse_x, a->mouse_y);
        break;
    case TOOL_MAGNET:
        /* The blower's field, turned around. */
        w->field_on = in->down[0] && (in_arena || w->field_on);
        w->field_x = a->mouse_x;
        w->field_y = a->mouse_y;
        w->field_radius = 320;
        w->field_strength = -7000;
        break;
    case TOOL_FREEZE:
        if (in_arena && in->pressed[0]) {
            int s = sp_body_at(w, a->mouse_x, a->mouse_y);
            if (s >= 0) {
                const SpBody *b = &w->bodies[s];
                /* All pinned means thaw, anything else means freeze solid. */
                bool frozen = true;
                for (int i = b->first; i < b->first + b->count; i++) frozen = frozen && w->w[i] <= 0;
                for (int i = b->first; i < b->first + b->count; i++) {
                    w->w[i] = frozen ? w->w0[i] : 0;
                    w->vx[i] = w->vy[i] = 0;
                }
                app_flash(a, frozen ? "THAWED" : "FROZEN");
            }
        }
        break;
    case TOOL_GLUE:
        if (in_arena && in->pressed[0]) {
            int p = sp_nearest_particle(w, a->mouse_x, a->mouse_y, 26);
            if (p >= 0 && a->glue_from >= 0 && a->glue_from < w->np && p != a->glue_from) {
                sp_add_link(w, a->glue_from, p, 2e-6f, 0, -1);
                a->glue_from = -1;
                app_flash(a, "STUCK");
            } else if (p >= 0) {
                a->glue_from = p;
                app_flash(a, "NOW CLICK WHAT TO GLUE IT TO");
            }
        }
        break;
    case TOOL_PIN:
        if (in_arena && in->pressed[0]) {
            int p = sp_nearest_particle(w, a->mouse_x, a->mouse_y, 26);
            if (p >= 0) sp_toggle_pin(w, p);
        }
        break;
    case TOOL_WALL:
        if (in_arena && in->pressed[0]) {
            a->walling = true;
            a->wall_x = a->mouse_x;
            a->wall_y = a->mouse_y;
        }
        if (a->walling && !in->down[0]) {
            a->walling = false;
            sp_add_capsule(w, a->wall_x, a->wall_y, a->mouse_x, a->mouse_y, 5, TAG_WALL);
        }
        break;
    case TOOL_ERASE:
        if (in_arena && in->down[0]) {
            int st = sp_static_at(w, a->mouse_x, a->mouse_y, 3, TAG_WALL);
            if (st >= 0) sp_static_remove(w, st);
            else {
                int s = sp_body_at(w, a->mouse_x, a->mouse_y);
                if (s >= 0) sp_body_remove(w, s);
            }
        }
        break;
    case TOOL_TYPE:
        if (in_arena && in->pressed[0]) {
            a->type_x = a->mouse_x;
            a->type_y = a->mouse_y;
        }
        break;
    default: break;
    }
}

void app_screenshot(App *a) {
    char path[64];
    snprintf(path, sizeof path, "softbodies-%03d.png", ++a->shot_count);
    bool ok = sw_screenshot(a->win, path);
    char msg[80];
    snprintf(msg, sizeof msg, ok ? "SAVED %s" : "COULD NOT SAVE %s", path);
    app_flash(a, msg);
}

/* Key entries carry the Ctrl state from when they were pressed, as a whole
 * chord can arrive and finish within a single batch of events. */
#define KEY_CTRL_FLAG 0x10000
#define KEY_SHIFT_FLAG 0x20000

static void toggle_terminal(App *a) {
    /* The first press opens it or gives it the keyboard, the next one hides it. */
    if (a->p_term.open && a->term.focus) {
        a->p_term.open = false;
        a->term.focus = false;
    } else {
        a->p_term.open = true;
        a->term.focus = true;
        sw_desk_raise(&a->desk, &a->p_term);
    }
}

static void handle_key(App *a, int entry) {
    int key = entry & ~(KEY_CTRL_FLAG | KEY_SHIFT_FLAG);
    bool shift = entry & KEY_SHIFT_FLAG;
    if ((entry & KEY_CTRL_FLAG) && key == SW_KEY_Q) { a->quit = true; return; }
    if (key == SW_KEY_GRAVE) { toggle_terminal(a); return; }
    if (a->term.focus && a->p_term.open) {
        term_key(a, key, shift);
        a->term.held_key = key;
        a->term.held_shift = shift;
        a->term.held_next = sw_now() + 0.42;
        return;
    }
    /* Shared by every toy. */
    switch (key) {
    case SW_KEY_F1: a->p_help.open = !a->p_help.open; return;
    case SW_KEY_P: if (a->tool != TOOL_TYPE || a->mode != MODE_BODIES) { app_screenshot(a); return; } break;
    default: break;
    }
    if (a->mode == MODE_SELECTOR) { selector_key(a, key); return; }
    if (a->mode == MODE_PHYS) {
        switch (key) {
        case SW_KEY_SPACE: a->paused = !a->paused; return;
        case SW_KEY_N: a->paused = true; a->step_once++; return;
        case SW_KEY_TAB: {
            bool any = a->p_elements.open || a->p_lab.open || a->p_info.open;
            a->p_elements.open = a->p_lab.open = a->p_info.open = !any;
            return;
        }
        default: phys_key(a, key); return;
        }
    }
    if (a->tool == TOOL_TYPE) {
        if (key == SW_KEY_ESC) { select_tool(a, TOOL_GRAB); return; }
        if (key == SW_KEY_ENTER) { a->type_x = a->world_w * 0.3f; a->type_y += 90; return; }
        char ch = sw_key_to_char(key);
        if (ch) {
            const float cell = 10;
            toy_letter(a, ch, a->type_x, a->type_y, cell);
            a->type_x += cell * 6;
            if (a->type_x > a->world_w - cell * 6) { a->type_x = 20; a->type_y += 90; }
            if (a->type_y > a->world_h - 80) a->type_y = 30;
        }
        return;
    }
    if (key >= SW_KEY_1 && key < SW_KEY_1 + NSCENES) { scene_load(a, key - SW_KEY_1); return; }
    switch (key) {
    case SW_KEY_ESC: case SW_KEY_G: select_tool(a, TOOL_GRAB); break;
    case SW_KEY_X: select_tool(a, TOOL_CUT); break;
    case SW_KEY_B: select_tool(a, TOOL_BLOW); break;
    case SW_KEY_M: select_tool(a, TOOL_BOMB); break;
    case SW_KEY_I: select_tool(a, TOOL_PIN); break;
    case SW_KEY_W: select_tool(a, TOOL_WALL); break;
    case SW_KEY_E: select_tool(a, TOOL_ERASE); break;
    case SW_KEY_T: select_tool(a, TOOL_TYPE); break;
    case SW_KEY_K: select_tool(a, TOOL_NUKE); break;
    case SW_KEY_A: select_tool(a, TOOL_MAGNET); break;
    case SW_KEY_Z: select_tool(a, TOOL_FREEZE); break;
    case SW_KEY_J: select_tool(a, TOOL_GLUE); break;
    case SW_KEY_R: scene_load(a, a->scene); break;
    case SW_KEY_C: toys_reset(a); break;
    case SW_KEY_SPACE: a->paused = !a->paused; break;
    case SW_KEY_N: a->paused = true; a->step_once++; break;
    case SW_KEY_V: a->view = (ViewMode)((a->view + 1) % 3); break;
    case SW_KEY_TAB: {
        bool any = a->p_tools.open || a->p_world.open || a->p_scope.open || a->p_help.open || a->p_about.open;
        a->p_tools.open = !any;
        a->p_world.open = !any;
        a->p_scope.open = !any;
        a->p_help.open = false;
        a->p_about.open = false;
        break;
    }
    default: break;
    }
}

static bool gather_input(App *a) {
    SwDesk *d = &a->desk;
    SwInput *in = &d->in;
    memset(in->pressed, 0, sizeof in->pressed);
    memset(in->released, 0, sizeof in->released);
    in->wheel = 0;
    in->nkeys = 0;
    SwEvent ev;
    bool resized = false;
    while (sw_poll(a->win, &ev)) {
        switch (ev.type) {
        case SW_EV_CLOSE: a->quit = true; break;
        case SW_EV_RESIZE: resized = true; break;
        case SW_EV_MOTION: in->mx = ev.x; in->my = ev.y; in->inside = true; break;
        case SW_EV_LEAVE:
            in->inside = false;
            for (int i = 0; i < 3; i++) in->down[i] = false;
            break;
        case SW_EV_BUTTON_DOWN:
            in->mx = ev.x; in->my = ev.y; in->inside = true;
            in->down[ev.button] = true;
            in->pressed[ev.button] = true;
            break;
        case SW_EV_BUTTON_UP:
            in->mx = ev.x; in->my = ev.y;
            in->down[ev.button] = false;
            in->released[ev.button] = true;
            break;
        case SW_EV_WHEEL: in->wheel += ev.wheel; break;
        case SW_EV_KEY_DOWN:
            if (ev.key == SW_KEY_KP_ENTER) ev.key = SW_KEY_ENTER;
            if (ev.key == SW_KEY_LSHIFT || ev.key == SW_KEY_RSHIFT) in->shift = true;
            if (ev.key == SW_KEY_LCTRL || ev.key == SW_KEY_RCTRL) in->ctrl = true;
            if (in->nkeys < SW_MAX_KEYS) in->keys[in->nkeys++] = ev.key | (in->ctrl ? KEY_CTRL_FLAG : 0) | (in->shift ? KEY_SHIFT_FLAG : 0);
            break;
        case SW_EV_KEY_UP:
            if (ev.key == SW_KEY_LSHIFT || ev.key == SW_KEY_RSHIFT) in->shift = false;
            if (ev.key == SW_KEY_LCTRL || ev.key == SW_KEY_RCTRL) in->ctrl = false;
            if (ev.key == a->term.held_key) a->term.held_key = 0;
            break;
        }
    }
    return resized;
}

/* ---- scripting, for tests and demos -------------------------------------------- */

typedef struct { long frame; int kind; int a, b; } ScriptEv;
#define MAX_SCRIPT 512

#ifndef __wasm__
/* "frame:move,x,y;frame:down,x,y;frame:up,x,y;frame:rdown,x,y;frame:key,code;frame:wheel,n" */
static int parse_script(const char *s, ScriptEv *out) {
    int n = 0;
    while (s && *s && n < MAX_SCRIPT) {
        char verb[16] = { 0 };
        long frame = 0;
        int a = 0, b = 0, used = 0;
        if (sscanf(s, "%ld:%15[a-z],%d,%d%n", &frame, verb, &a, &b, &used) < 3 || used == 0) {
            used = 0;
            b = 0;
            if (sscanf(s, "%ld:%15[a-z],%d%n", &frame, verb, &a, &used) < 3 || used == 0) {
                fprintf(stderr, "bad script near: %s\n", s);
                return -1;
            }
        }
        int kind = !strcmp(verb, "move") ? 0 : !strcmp(verb, "down") ? 1 : !strcmp(verb, "up") ? 2
                 : !strcmp(verb, "rdown") ? 3 : !strcmp(verb, "rup") ? 4 : !strcmp(verb, "key") ? 5
                 : !strcmp(verb, "wheel") ? 6 : !strcmp(verb, "winmove") ? 7 : -1;
        if (kind < 0) { fprintf(stderr, "bad script verb: %s\n", verb); return -1; }
        out[n++] = (ScriptEv){ frame, kind, a, b };
        s += used;
        if (*s == ';') s++;
    }
    return n;
}

#endif

static void script_fire(SwWindow *win, const ScriptEv *e) {
    SwEvent ev = { .x = e->a, .y = e->b };
    switch (e->kind) {
    case 0: ev.type = SW_EV_MOTION; break;
    case 1: ev.type = SW_EV_BUTTON_DOWN; ev.button = 0; break;
    case 2: ev.type = SW_EV_BUTTON_UP; ev.button = 0; break;
    case 3: ev.type = SW_EV_BUTTON_DOWN; ev.button = 1; break;
    case 4: ev.type = SW_EV_BUTTON_UP; ev.button = 1; break;
    case 5: sw_push(win, (SwEvent){ .type = SW_EV_KEY_DOWN, .key = e->a });
            ev = (SwEvent){ .type = SW_EV_KEY_UP, .key = e->a }; break;
    case 6: ev = (SwEvent){ .type = SW_EV_WHEEL, .wheel = e->a }; break;
    case 7: win->sx = e->a; win->sy = e->b; return; /* the window itself is thrown */
    }
    sw_push(win, ev);
}

/* ---- main ------------------------------------------------------------------------ */

typedef struct {
    bool help, about, stress;
    int scene;
    ToyMode toy;
    SwBackend backend;
    int w, h;
    long max_frames;
    const char *shots, *prefix;
} Options;

/* What persists between ticks. */
typedef struct {
    App *a;
    bool realtime;
    double last, acc, fps_t;
    long frame, max_frames;
    int fps_n, hist_tick, next_script, nscript;
    const char *shots, *prefix;
    ScriptEv script[MAX_SCRIPT];
} Runner;

static Runner R;

static App *app_create(const Options *o) {
    App *a = calloc(1, sizeof *a);
    a->win = sw_open(o->backend, WINDOW_TITLE, o->w, o->h);
    if (!a->win) {
        free(a);
        return NULL;
    }
    a->world_bodies = sp_world_new();
    a->world_phys = sp_world_new();
    a->world = a->world_bodies;
    a->toys = a->toys_bodies;
    a->glue_from = -1;
    a->rng = 0x51F7B0D1u;
    a->size = 34;
    a->time_scale = 1;
    a->show_faces = true;
    a->solid_windows = true;
    a->nuke_flash = true;
    a->view = o->stress ? VIEW_STRESS : VIEW_FLAT;
    a->tool = TOOL_GRAB;
    brk_world_cleared(a, a->world_phys);
    brk_world_cleared(a, a->world_bodies);

    SwDesk *d = &a->desk;
    sw_canvas(&a->canvas, a->win->fb, a->win->w, a->win->h);
    sw_desk_init(d, &a->canvas, a->win->scale);
    a->p_tools = (SwPanel){ .title = "TOOLBOX", .open = true, .solid_id = 0 };
    a->p_world = (SwPanel){ .title = "WORLD", .open = true, .solid_id = 1 };
    a->p_scope = (SwPanel){ .title = "SCOPE", .open = true, .solid_id = 2 };
    a->p_help = (SwPanel){ .title = "KEYS", .open = o->help, .solid_id = 3 };
    a->p_about = (SwPanel){ .title = "ABOUT", .open = o->about, .solid_id = 4 };
    a->p_term = (SwPanel){ .title = "TERMINAL", .solid_id = -1 };
    a->p_elements = (SwPanel){ .title = "ELEMENTS", .solid_id = -1 };
    a->p_lab = (SwPanel){ .title = "LAB", .solid_id = -1 };
    a->p_info = (SwPanel){ .title = "INFO", .solid_id = -1 };
    /* The selector is what opens first. Each toy's windows open with it. */
    a->p_tools.open = a->p_world.open = a->p_scope.open = false;
    for (int m = 0; m < MODE_COUNT; m++)
        for (int i = 0; i < 4; i++) a->panel_memory[m][i] = true;
    sw_desk_add(d, &a->p_tools);
    sw_desk_add(d, &a->p_world);
    sw_desk_add(d, &a->p_scope);
    sw_desk_add(d, &a->p_help);
    sw_desk_add(d, &a->p_about);
    sw_desk_add(d, &a->p_elements);
    sw_desk_add(d, &a->p_lab);
    sw_desk_add(d, &a->p_info);
    sw_desk_add(d, &a->p_term);

    a->mode = MODE_SELECTOR;
    a->scene = o->scene;
    layout(a);
    term_init(a);
    app_set_mode(a, o->toy);
    if (o->toy == MODE_PHYS && o->scene > 0) phys_load_scene(a, o->scene);

    R.a = a;
    R.realtime = o->backend != SW_BACKEND_HEADLESS;
    R.last = R.fps_t = sw_now();
    R.max_frames = o->max_frames;
    R.shots = o->shots;
    R.prefix = o->prefix;
    return a;
}

/* One frame: input, widgets, physics, drawing. */
static void app_tick(void *user) {
    (void)user;
    App *a = R.a;
    SwDesk *d = &a->desk;

    while (R.next_script < R.nscript && R.script[R.next_script].frame <= R.frame)
        script_fire(a->win, &R.script[R.next_script++]);

    if (gather_input(a)) layout(a);
    if (a->win->closed) a->quit = true;

    /* Grab the window edge before anything else claims the click. */
    d->cursor_hint = NULL;
    int edges = d->in.inside ? edges_at(a, d->in.mx, d->in.my) : 0;
    if (edges && d->in.pressed[0]) {
        sw_begin_resize(a->win, edges);
        d->in.pressed[0] = false;
        d->in.down[0] = false;
    }

    sw_desk_begin(d);
    if (edges) { d->cursor_hint = "resize"; d->mouse_on_ui = true; }

    /* Drawing happens first since widgets are immediate mode, and the
     * chrome claims the mouse before the arena reads it. */
    sw_clear(&a->canvas, SW_PAPER);
    if (a->mode == MODE_BODIES) render_world(a);
    else if (a->mode == MODE_PHYS) phys_render(a);
    else selector_draw(a);
    sw_clip(&a->canvas, a->ax, a->ay, a->aw, a->ah);
    for (int i = 0; i < d->npanels; i++) {
        SwPanel *p = d->panels[i];
        if (p == &a->p_tools) panel_tools(a);
        else if (p == &a->p_world) panel_world(a);
        else if (p == &a->p_scope) panel_scope(a);
        else if (p == &a->p_help) panel_help(a);
        else if (p == &a->p_about) panel_about(a);
        else if (p == &a->p_term) term_panel(a);
        else phys_panels(a, p);
    }
    sw_noclip(&a->canvas);
    draw_chrome(a);
    draw_menus(a);
    brk_draw_holes(a);

    if (a->mode == MODE_BODIES) arena_input(a);
    else if (a->mode == MODE_PHYS) phys_input(a);
    for (int i = 0; i < d->in.nkeys; i++) handle_key(a, d->in.keys[i]);
    /* A held key repeats, but only for typing. */
    if (a->term.focus && a->term.held_key && sw_now() >= a->term.held_next) {
        term_key(a, a->term.held_key, a->term.held_shift);
        a->term.held_next = sw_now() + 0.04;
    }
    if (a->mode == MODE_BODIES) brk_sync_panels(a);

    /* Physics: fixed steps from real time, or exactly one per frame when
     * headless, so test runs are deterministic. */
    double now = sw_now();
    double real_dt = now - R.last;
    R.last = now;
    if (real_dt > 0.1) real_dt = 0.1;
    R.acc += R.realtime ? real_dt : 1.0 / 60;
    const double fixed = 1.0 / 60;
    int steps = 0;
    double t0 = sw_now();
    while (R.acc >= fixed && steps < 3) {
        R.acc -= fixed;
        if (a->mode == MODE_SELECTOR) continue;
        if (a->paused && a->step_once <= 0) continue;
        if (a->step_once > 0) a->step_once--;
        float dt = (float)fixed * a->time_scale;
        brk_before_step(a, (float)fixed);
        if (a->mode == MODE_PHYS) phys_tick(a, dt);
        toys_update(a, dt);
        sp_step(a->world, dt);
        toys_events(a);
        brk_after_step(a);
        a->sim_time += dt;
        steps++;
        if (++R.hist_tick >= 2) {
            R.hist_tick = 0;
            a->history[a->hist_pos] = a->world->kinetic;
            a->hist_pos = (a->hist_pos + 1) % HISTORY;
        }
    }
    if (R.acc > fixed * 3) R.acc = 0;
    /* Stopped time: the window can still move, and the outside doesn't care. */
    if (a->mode == MODE_SELECTOR || (a->paused && !steps)) brk_idle(a);
    if (steps) a->step_ms = a->step_ms * 0.9f + (float)((sw_now() - t0) * 1000 / steps) * 0.1f;
    /* Per rendered frame, so real time: displays run far from 60 Hz. */
    float frame_dt = R.realtime ? (float)real_dt : (float)fixed;
    if (a->mode == MODE_SELECTOR) selector_update(a, frame_dt);
    fx_update(a, frame_dt);
    if (a->flash_t > 0) a->flash_t -= frame_dt;
    if (a->crack_t > 0) a->crack_t -= frame_dt;

    render_cursor(a);
    if (a->invert) sw_invert_all(&a->canvas);
    sw_present(a->win);
    brk_outer(a);

    R.fps_n++;
    if (now - R.fps_t >= 0.5) {
        a->fps = (float)R.fps_n / (float)(now - R.fps_t);
        R.fps_n = 0;
        R.fps_t = now;
    }
    R.frame++;

    if (R.shots) {
        const char *p = R.shots;
        while (p && *p) {
            if (atol(p) == R.frame) {
                char path[512];
                snprintf(path, sizeof path, "%s-%ld.png", R.prefix, R.frame);
                if (!sw_write_png(path, a->win->fb, a->win->w, a->win->h))
                    fprintf(stderr, "could not write %s\n", path);
                if (a->win->outer) {
                    snprintf(path, sizeof path, "%s-%ld-desktop.png", R.prefix, R.frame);
                    sw_write_png(path, a->win->outer, a->win->outer_w, a->win->outer_h);
                }
            }
            p = strchr(p, ',');
            if (p) p++;
        }
    }
    if (R.max_frames && R.frame >= R.max_frames) a->quit = true;
}

#ifdef __wasm__

/* The browser owns the loop: web/shell.html calls these. */
#define WASM_EXPORT(name) __attribute__((export_name(#name)))

WASM_EXPORT(sb_init) int sb_init(int w, int h, float scale, int toy, int scene) {
    Options o = { .backend = SW_BACKEND_AUTO, .w = w, .h = h, .toy = (ToyMode)toy, .scene = scene };
    sw_web_set_scale(scale);
    return app_create(&o) != NULL;
}

/* The desktop around the toy draws its text in the same font. */
WASM_EXPORT(sb_glyph) const uint8_t *sb_glyph(int ch) { return sw_glyph((char)ch); }

WASM_EXPORT(sb_frame) int sb_frame(void) {
    if (!R.a || R.a->quit) return 0;
    app_tick(NULL);
    return !R.a->quit;
}

WASM_EXPORT(sb_mode) int sb_mode(void) { return R.a ? (int)R.a->mode : -1; }

WASM_EXPORT(sb_resize) void sb_resize(int w, int h, float scale) { sw_web_resize(w, h, scale); }
WASM_EXPORT(sb_position) void sb_position(int x, int y) { sw_web_position(x, y); }
WASM_EXPORT(sb_desk) void sb_desk(int x0, int y0, int x1, int y1, int sw, int sh) { sw_web_desk(x0, y0, x1, y1, sw, sh); }
WASM_EXPORT(sb_obstacle) void sb_obstacle(int i, int x, int y, int w, int h) { sw_web_obstacle(i, x, y, w, h); }
WASM_EXPORT(sb_obstacles) void sb_obstacles(int n) { sw_web_obstacles(n); }

/* For tests/web_drive.py. */
WASM_EXPORT(sb_stat) float sb_stat(int which) {
    App *a = R.a;
    if (!a) return -1;
    switch (which) {
    case 0: return (float)a->escaped;
    case 1: return (float)brk_count_gaps(a);
    case 2: return (float)brk_count_broken(a);
    case 3: return brk_lowest(a);
    case 4: return a->world->kinetic;
    default: return -1;
    }
}

__attribute__((export_name("sw_web_event"))) void sw_web_event_export(int type, int x, int y, int code) {
    sw_web_event(type, x, y, code);
}

#else

static void usage(void) {
    puts("softbodies " VERSION " - a soft body toy\n"
         "\n"
         "  softbodies [options]\n"
         "\n"
         "  --backend NAME   auto, wayland, x11 or headless\n"
         "  --size WxH       window size in logical pixels (default 1200x800)\n"
         "  --toy NAME       start in a toy: softphys or softbodies (default: the selector)\n"
         "  --scene N        first scene of that toy\n"
         "\n"
         "for testing:\n"
         "  --frames N       exit after N frames, printing a summary\n"
         "  --shots A,B,C    save a PNG at these frames\n"
         "  --shot-prefix P  path prefix for those PNGs (default: shot)\n"
         "  --script S       inject input, e.g. \"10:move,300,200;11:down,300,200\"\n"
         "  --exec CMDS      run terminal commands at start\n"
         "  --dump-term      print the terminal's text on exit\n"
         "  --help-window    open the keys window at start\n");
}

static void app_destroy(App *a) {
    sp_world_free(a->world_bodies);
    sp_world_free(a->world_phys);
    sp_world_free(a->sel.world);
    phys_free(a->sel.grid);
    phys_free(a->phys.g);
    sw_close(a->win);
    free(a);
}

int main(int argc, char **argv) {
    Options o = { .backend = SW_BACKEND_AUTO, .w = 1200, .h = 800, .prefix = "shot" };
    const char *script_s = NULL, *exec_s = NULL;
    bool dump_term = false, toy_given = false, scene_given = false;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
#define NEXT() (i + 1 < argc ? argv[++i] : "")
        if (!strcmp(arg, "--backend")) {
            const char *b = NEXT();
            o.backend = !strcmp(b, "wayland") ? SW_BACKEND_WAYLAND : !strcmp(b, "x11") ? SW_BACKEND_X11
                      : !strcmp(b, "headless") ? SW_BACKEND_HEADLESS : SW_BACKEND_AUTO;
        } else if (!strcmp(arg, "--size")) {
            if (sscanf(NEXT(), "%dx%d", &o.w, &o.h) != 2) { usage(); return 2; }
        } else if (!strcmp(arg, "--toy")) {
            const char *t = NEXT();
            toy_given = true;
            if (!strcmp(t, "softphys")) o.toy = MODE_PHYS;
            else if (!strcmp(t, "softbodies")) o.toy = MODE_BODIES;
            else if (strcmp(t, "selector")) { usage(); return 2; }
        } else if (!strcmp(arg, "--scene")) { o.scene = atoi(NEXT()) - 1; scene_given = true; }
        else if (!strcmp(arg, "--exec")) exec_s = NEXT();
        else if (!strcmp(arg, "--dump-term")) dump_term = true;
        else if (!strcmp(arg, "--frames")) o.max_frames = atol(NEXT());
        else if (!strcmp(arg, "--shots")) o.shots = NEXT();
        else if (!strcmp(arg, "--shot-prefix")) o.prefix = NEXT();
        else if (!strcmp(arg, "--script")) script_s = NEXT();
        else if (!strcmp(arg, "--help-window")) o.help = true;
        else if (!strcmp(arg, "--about-window")) o.about = true;
        else if (!strcmp(arg, "--stress-view")) o.stress = true;
        else { usage(); return strcmp(arg, "--help") ? 2 : 0; }
    }

    /* Asking for a scene means asking for the toy it's in. */
    if (scene_given && !toy_given) o.toy = MODE_BODIES;
    R.nscript = script_s ? parse_script(script_s, R.script) : 0;
    if (R.nscript < 0) return 2;

    App *a = app_create(&o);
    if (!a) {
        fprintf(stderr, "softbodies: no display could be opened\n");
        return 1;
    }
    a->win->modal_tick = app_tick;
    if (exec_s) term_exec(a, exec_s);
    while (!a->quit) {
        sw_wait_frame(a->win);
        app_tick(NULL);
    }

    if (dump_term) {
        const Term *t = &a->term;
        for (int i = 0; i < t->count; i++) puts(t->lines[(t->head - t->count + i + TERM_LINES * 2) % TERM_LINES]);
    }
    if (o.max_frames) {
        double ke = a->world->kinetic;
        printf("toy=%s backend=%s frames=%ld presented=%ld shown=%ld size=%dx%d scale=%.3f bodies=%d points=%d links=%d step_ms=%.2f fps=%.0f ke=%.1f gaps=%d broken=%d escaped=%d\n",
               app_mode_name(a->mode), sw_backend_name(a->win), R.frame, a->win->presented, a->win->shown, a->win->w, a->win->h,
               (double)a->win->scale, sp_count_bodies(a->world), a->world->np, sp_count_links(a->world),
               (double)a->step_ms, (double)a->fps, ke, brk_count_gaps(a), brk_count_broken(a), a->escaped);
    }
    app_destroy(a);
    return 0;
}

#endif
