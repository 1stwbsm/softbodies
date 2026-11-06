/* SoftWin platform core: event queue, backend choice, and a headless backend. */
#include "softwin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
double sw_now(void) {
    static double period;
    LARGE_INTEGER t;
    if (period == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        period = 1.0 / (double)f.QuadPart;
    }
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * period;
}
#elif defined(__wasm__)
__attribute__((import_module("env"), import_name("js_now"))) double js_now(void);
double sw_now(void) { return js_now(); }
#else
#include <time.h>
double sw_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
#endif

void sw_push(SwWindow *win, SwEvent ev) {
    int next = (win->qtail + 1) % 1024;
    if (next == win->qhead) return; /* full: dropping beats blocking */
    win->queue[win->qtail] = ev;
    win->qtail = next;
}

bool sw_poll(SwWindow *win, SwEvent *out) {
    win->ops->pump(win, 0);
    if (win->qhead == win->qtail) return false;
    *out = win->queue[win->qhead];
    win->qhead = (win->qhead + 1) % 1024;
    return true;
}

void sw_resize_fb(SwWindow *win, int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (win->fb && w == win->w && h == win->h) return;
    free(win->fb);
    win->fb = calloc((size_t)w * h, sizeof(uint32_t));
    win->w = w;
    win->h = h;
    sw_push(win, (SwEvent){ .type = SW_EV_RESIZE, .x = w, .y = h });
}

/* The display backends this platform has, in the order AUTO tries them. */
static const SwOps *const NATIVE[] = {
#if defined(_WIN32)
    &sw_win32_ops,
#elif defined(__wasm__)
    &sw_web_ops,
#else
    /* X11 first, even in a Wayland session: through XWayland it can still see where
     * its window is and where the taskbar is, and draw over the desktop. Native
     * Wayland deliberately tells a program none of that. */
    &sw_x11_ops, &sw_wayland_ops,
#endif
};
#define NATIVE_COUNT (int)(sizeof NATIVE / sizeof *NATIVE)

static const SwOps *find_backend(SwBackend backend) {
    const char *name = backend == SW_BACKEND_WAYLAND ? "wayland" : backend == SW_BACKEND_X11 ? "x11" : "";
    if (backend == SW_BACKEND_HEADLESS) return &sw_headless_ops;
    for (int i = 0; i < NATIVE_COUNT; i++)
        if (!strcmp(NATIVE[i]->name, name)) return NATIVE[i];
    return NULL;
}

SwWindow *sw_open(SwBackend backend, const char *title, int w, int h) {
    SwWindow *win = calloc(1, sizeof *win);
    win->scale = 1;
    bool ok = false;
    if (backend == SW_BACKEND_AUTO) {
        for (int i = 0; i < NATIVE_COUNT && !ok; i++) {
            win->ops = NATIVE[i];
            ok = win->ops->open(win, title, w, h);
        }
    } else {
        win->ops = find_backend(backend);
        ok = win->ops && win->ops->open(win, title, w, h);
    }
    if (!ok) {
        free(win->fb);
        free(win);
        return NULL;
    }
    return win;
}

void sw_close(SwWindow *win) {
    if (!win) return;
    win->ops->close(win);
    free(win->fb);
    free(win);
}

void sw_wait_frame(SwWindow *win) {
    /* The cap matters on Wayland: a hidden window gets no frame callbacks, and
     * the app should still notice a close request. */
    double deadline = sw_now() + 0.25;
    while (!win->ops->frame_ready(win)) {
        if (win->closed) return;
        double left = deadline - sw_now();
        if (left <= 0) return;
        /* Short slices: X11 readiness is partly a timer, not just socket data. */
        win->ops->pump(win, left > 0.002 ? 2 : 1);
    }
}

void sw_present(SwWindow *win) { win->ops->present(win); win->presented++; }
void sw_begin_move(SwWindow *win) { win->ops->begin_move(win); }
void sw_begin_resize(SwWindow *win, int edges) { win->ops->begin_resize(win, edges); }
void sw_minimize(SwWindow *win) { win->ops->minimize(win); }
const char *sw_backend_name(const SwWindow *win) { return win->ops->name; }

bool sw_screenshot(SwWindow *win, const char *path) {
    if (win->ops->screenshot) return win->ops->screenshot(win, path);
    return sw_write_png(path, win->fb, win->w, win->h);
}

char sw_key_to_char(int key) {
    static const char *const row1 = "1234567890-=";
    static const char *const row2 = "QWERTYUIOP[]";
    static const char *const row3 = "ASDFGHJKL;'";
    static const char *const row4 = "ZXCVBNM,./";
    if (key >= 2 && key <= 13) return row1[key - 2];
    if (key >= 16 && key <= 27) return row2[key - 16];
    if (key >= 30 && key <= 40) return row3[key - 30];
    if (key >= 44 && key <= 53) return row4[key - 44];
    if (key == SW_KEY_SPACE) return ' ';
    return 0;
}

char sw_shifted_char(char ch) {
    static const char *const plain = "1234567890-=;',./", *const shift = "!@#$%^&*()_+:\"<>?";
    for (int i = 0; plain[i]; i++)
        if (plain[i] == ch) return shift[i];
    return ch;
}

/* ---- headless: renders into memory, for tests and screenshots ----------- */

static bool hl_open(SwWindow *win, const char *title, int w, int h) {
    (void)title;
    sw_resize_fb(win, w, h);
    /* A pretend desktop, the size of a common screen with a taskbar along the bottom. */
    win->has_pos = win->has_desk = true;
    win->sx = win->sy = 100;
    win->desk_x0 = win->desk_y0 = 0;
    win->desk_x1 = 1920;
    win->desk_y1 = 1040;
    return true;
}
static bool hl_outer_open(SwWindow *win) {
    win->outer_w = 1920;
    win->outer_h = 1080;
    win->outer = malloc((size_t)win->outer_w * win->outer_h * 4);
    return win->outer != NULL;
}
static void hl_outer_present(SwWindow *win, int x, int y, int w, int h) { (void)win; (void)x; (void)y; (void)w; (void)h; }
static void hl_close(SwWindow *win) { free(win->outer); win->outer = NULL; }
static void hl_pump(SwWindow *win, int t) { (void)win; (void)t; }
static void hl_present(SwWindow *win) { win->shown++; }
static bool hl_ready(SwWindow *win) { (void)win; return true; }
static void hl_move(SwWindow *win) { (void)win; }
static void hl_resize(SwWindow *win, int e) { (void)win; (void)e; }
static void hl_min(SwWindow *win) { (void)win; }

const SwOps sw_headless_ops = { "headless", hl_open, hl_close, hl_pump, hl_present, hl_ready, hl_move, hl_resize, hl_min, NULL,
                                  hl_outer_open, hl_outer_present };
