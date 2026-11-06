/* SoftWin WebAssembly backend.
 *
 * The browser owns the event loop, so nothing here blocks: web/shell.html calls
 * sb_frame from requestAnimationFrame and forwards input through sw_web_event.
 * Pointer positions arrive in framebuffer pixels, and keys as evdev codes.
 */
#include "softwin.h"

#include <stdlib.h>

#define JS_IMPORT(name) __attribute__((import_module("env"), import_name(#name)))

JS_IMPORT(js_present) void js_present(const uint32_t *px, int w, int h);
JS_IMPORT(js_begin_move) void js_begin_move(void);
JS_IMPORT(js_begin_resize) void js_begin_resize(int edges);
JS_IMPORT(js_minimize) void js_minimize(void);
JS_IMPORT(js_screenshot) void js_screenshot(const char *name);
JS_IMPORT(js_outer_present) void js_outer_present(const uint32_t *px, int w, int h, int x, int y, int rw, int rh);

typedef struct {
    uint32_t *rgba; /* canvas pixels: the framebuffer plus opaque alpha */
    int cap;
    uint32_t *outer_rgba;
} Web;

static SwWindow *g_win;
static float g_initial_scale = 1;

void sw_web_set_scale(float scale) { g_initial_scale = scale > 0 ? scale : 1; }

void sw_web_event(int type, int x, int y, int code) {
    if (!g_win) return;
    SwEvent ev = { .type = (SwEventType)type, .x = x, .y = y };
    if (type == SW_EV_BUTTON_DOWN || type == SW_EV_BUTTON_UP) ev.button = code;
    else if (type == SW_EV_KEY_DOWN || type == SW_EV_KEY_UP) ev.key = code;
    else if (type == SW_EV_WHEEL) ev.wheel = code;
    sw_push(g_win, ev);
}

void sw_web_resize(int w, int h, float scale) {
    if (!g_win) return;
    g_win->scale = scale > 0 ? scale : 1;
    sw_resize_fb(g_win, w, h);
    /* A scale change alone moves the layout, with no change in pixels. */
    sw_push(g_win, (SwEvent){ .type = SW_EV_RESIZE, .x = w, .y = h });
}

void sw_web_position(int x, int y) {
    if (!g_win) return;
    g_win->sx = x;
    g_win->sy = y;
    g_win->has_pos = true;
}

void sw_web_desk(int x0, int y0, int x1, int y1, int screen_w, int screen_h) {
    if (!g_win) return;
    if (g_win->outer && (screen_w != g_win->outer_w || screen_h != g_win->outer_h)) {
        free(g_win->outer);
        g_win->outer = NULL; /* the next frame opens one of the new size */
    }
    g_win->outer_w = screen_w;
    g_win->outer_h = screen_h;
    g_win->desk_x0 = x0; g_win->desk_y0 = y0;
    g_win->desk_x1 = x1; g_win->desk_y1 = y1;
    g_win->has_desk = true;
}

void sw_web_obstacle(int i, int x, int y, int w, int h) {
    if (!g_win || i < 0 || i >= SW_MAX_OBSTACLES) return;
    g_win->obstacles[i].x = x; g_win->obstacles[i].y = y;
    g_win->obstacles[i].w = w; g_win->obstacles[i].h = h;
}

void sw_web_obstacles(int count) {
    if (g_win) g_win->nobstacles = count < 0 ? 0 : count > SW_MAX_OBSTACLES ? SW_MAX_OBSTACLES : count;
}

static bool web_open(SwWindow *win, const char *title, int w, int h) {
    (void)title; /* the page has its own title */
    win->impl = calloc(1, sizeof(Web));
    win->scale = g_initial_scale;
    g_win = win;
    sw_resize_fb(win, (int)((float)w * win->scale), (int)((float)h * win->scale));
    return true;
}

static void web_close(SwWindow *win) {
    Web *c = win->impl;
    g_win = NULL;
    if (c) { free(c->rgba); free(c->outer_rgba); }
    free(win->outer);
    win->outer = NULL;
    free(c);
    win->impl = NULL;
}

static void web_pump(SwWindow *win, int timeout_ms) { (void)win; (void)timeout_ms; }

static void web_present(SwWindow *win) {
    Web *c = win->impl;
    int n = win->w * win->h;
    if (n > c->cap) {
        free(c->rgba);
        c->rgba = malloc((size_t)n * 4);
        c->cap = n;
    }
    /* Black and white read the same in BGR and RGB, so only alpha needs setting. */
    for (int i = 0; i < n; i++) c->rgba[i] = win->fb[i] | 0xFF000000u;
    js_present(c->rgba, win->w, win->h);
    win->shown++;
}

static bool web_ready(SwWindow *win) { (void)win; return true; }
static void web_begin_move(SwWindow *win) { (void)win; js_begin_move(); }
static void web_begin_resize(SwWindow *win, int edges) { (void)win; js_begin_resize(edges); }
static void web_minimize(SwWindow *win) { (void)win; js_minimize(); }

static bool web_screenshot(SwWindow *win, const char *path) {
    (void)win;
    js_screenshot(path); /* a browser download: pages can't write files */
    return true;
}

static bool web_outer_open(SwWindow *win) {
    Web *c = win->impl;
    size_t n = (size_t)win->outer_w * win->outer_h;
    win->outer = malloc(n * 4);
    free(c->outer_rgba);
    c->outer_rgba = calloc(n, 4);
    return win->outer && c->outer_rgba;
}

static void web_outer_present(SwWindow *win, int x, int y, int w, int h) {
    Web *c = win->impl;
    if (w > 0 && h > 0) {
        for (int j = y; j < y + h; j++) {
            const uint32_t *from = win->outer + (size_t)j * win->outer_w;
            uint32_t *to = c->outer_rgba + (size_t)j * win->outer_w;
            for (int i = x; i < x + w; i++)
                to[i] = from[i] == SW_OUTER_CLEAR ? 0 : from[i] | 0xFF000000u;
        }
    }
    js_outer_present(c->outer_rgba, win->outer_w, win->outer_h, x, y, w, h);
}

const SwOps sw_web_ops = { "web", web_open, web_close, web_pump, web_present, web_ready,
                           web_begin_move, web_begin_resize, web_minimize, web_screenshot,
                           web_outer_open, web_outer_present };
