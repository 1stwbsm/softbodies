/* SoftWin - a tiny window system with no toolkit underneath.
 *
 * Three layers, all in this project and needing only libc:
 *   Platform   speaks the Wayland or X11 wire protocol directly over a socket
 *   Gfx        strictly 1-bit drawing into a framebuffer, plus a pixel font
 *   Desk       inner windows, menus and widgets, drawn immediate mode
 */
#ifndef SOFTWIN_H
#define SOFTWIN_H

#include <stdbool.h>
#include <stdint.h>

/* ---- platform --------------------------------------------------------- */

typedef enum { SW_BACKEND_AUTO, SW_BACKEND_WAYLAND, SW_BACKEND_X11, SW_BACKEND_HEADLESS } SwBackend;

typedef enum {
    SW_EV_CLOSE, SW_EV_RESIZE, SW_EV_MOTION, SW_EV_BUTTON_DOWN, SW_EV_BUTTON_UP,
    SW_EV_WHEEL, SW_EV_KEY_DOWN, SW_EV_KEY_UP, SW_EV_LEAVE
} SwEventType;

typedef struct {
    SwEventType type;
    int x, y;    /* framebuffer pixels */
    int button;  /* 0 left, 1 right, 2 middle */
    int key;     /* Linux evdev code, see SW_KEY_* */
    int wheel;   /* positive = down */
} SwEvent;

typedef enum {
    SW_EDGE_TOP = 1, SW_EDGE_BOTTOM = 2, SW_EDGE_LEFT = 4, SW_EDGE_RIGHT = 8
} SwEdge;

typedef struct SwWindow SwWindow;

typedef struct {
    const char *name;
    bool (*open)(SwWindow *win, const char *title, int w, int h);
    void (*close)(SwWindow *win);
    void (*pump)(SwWindow *win, int timeout_ms); /* read events, wait at most timeout */
    void (*present)(SwWindow *win);
    bool (*frame_ready)(SwWindow *win);
    void (*begin_move)(SwWindow *win);
    void (*begin_resize)(SwWindow *win, int edges);
    void (*minimize)(SwWindow *win);
    bool (*screenshot)(SwWindow *win, const char *path); /* NULL: write a PNG file */
    /* The overlay: a click-through layer over the whole desktop, for what has left
     * the window. NULL where the platform can't do it. Pixels equal to SW_OUTER_CLEAR
     * show what's underneath. Present sends only the rect given, and a rect of no
     * width hides the overlay. */
    bool (*outer_open)(SwWindow *win);
    void (*outer_present)(SwWindow *win, int x, int y, int w, int h);
} SwOps;

#define SW_OUTER_CLEAR 0x00FF00FFu
#define SW_MAX_OBSTACLES 16

struct SwWindow {
    const SwOps *ops;
    void *impl;
    uint32_t *fb;    /* back buffer, XRGB */
    int w, h;        /* framebuffer pixels */
    float scale;     /* framebuffer pixels per logical pixel */
    SwEvent queue[1024];
    int qhead, qtail;
    bool closed;
    long presented;  /* frames handed to the display server */
    long shown;      /* frames the display server has shown or accepted */
    /* Called while a system loop that won't return is running, such as a window
     * drag on Windows, so the app keeps going during it. */
    void (*modal_tick)(void *user);
    void *modal_user;

    /* Where the window is on the screen, in framebuffer pixels. Wayland never says. */
    bool has_pos;
    int sx, sy;
    /* The usable desktop, which stops short of the taskbar, in the same units. */
    bool has_desk;
    int desk_x0, desk_y0, desk_x1, desk_y1;
    /* The overlay's pixels, and where its corner is on the screen. */
    uint32_t *outer;
    int outer_w, outer_h, outer_x, outer_y;
    /* Other windows, where the platform knows of any. */
    struct { int x, y, w, h; } obstacles[SW_MAX_OBSTACLES];
    int nobstacles;
};

SwWindow *sw_open(SwBackend backend, const char *title, int w, int h);
void sw_close(SwWindow *win);
bool sw_poll(SwWindow *win, SwEvent *out);
void sw_wait_frame(SwWindow *win);   /* block until the next frame is due */
void sw_present(SwWindow *win);
void sw_begin_move(SwWindow *win);
void sw_begin_resize(SwWindow *win, int edges);
void sw_minimize(SwWindow *win);
const char *sw_backend_name(const SwWindow *win);
bool sw_screenshot(SwWindow *win, const char *path);
double sw_now(void);

/* for backends */
void sw_push(SwWindow *win, SwEvent ev);
void sw_resize_fb(SwWindow *win, int w, int h);
extern const SwOps sw_headless_ops;
#if defined(_WIN32)
extern const SwOps sw_win32_ops;
#elif defined(__wasm__)
extern const SwOps sw_web_ops;
void sw_web_set_scale(float scale);
void sw_web_event(int type, int x, int y, int code);
void sw_web_resize(int w, int h, float scale);
void sw_web_position(int x, int y);
void sw_web_desk(int x0, int y0, int x1, int y1, int screen_w, int screen_h);
void sw_web_obstacle(int index, int x, int y, int w, int h);
void sw_web_obstacles(int count);
#else
extern const SwOps sw_wayland_ops, sw_x11_ops;
#endif

/* Key codes (evdev). A layout-free physical position, shown here as US QWERTY. */
enum {
    SW_KEY_ESC = 1, SW_KEY_1 = 2, SW_KEY_0 = 11, SW_KEY_MINUS = 12, SW_KEY_EQUAL = 13,
    SW_KEY_BACKSPACE = 14, SW_KEY_TAB = 15, SW_KEY_Q = 16, SW_KEY_W = 17, SW_KEY_E = 18,
    SW_KEY_R = 19, SW_KEY_T = 20, SW_KEY_Y = 21, SW_KEY_U = 22, SW_KEY_I = 23, SW_KEY_O = 24,
    SW_KEY_P = 25, SW_KEY_ENTER = 28, SW_KEY_LCTRL = 29, SW_KEY_A = 30, SW_KEY_S = 31,
    SW_KEY_D = 32, SW_KEY_F = 33, SW_KEY_G = 34, SW_KEY_H = 35, SW_KEY_J = 36, SW_KEY_K = 37,
    SW_KEY_L = 38, SW_KEY_LSHIFT = 42, SW_KEY_Z = 44, SW_KEY_X = 45, SW_KEY_C = 46,
    SW_KEY_V = 47, SW_KEY_B = 48, SW_KEY_N = 49, SW_KEY_M = 50, SW_KEY_RSHIFT = 54,
    SW_KEY_GRAVE = 41, SW_KEY_SPACE = 57, SW_KEY_F1 = 59, SW_KEY_F2 = 60, SW_KEY_KP_ENTER = 96, SW_KEY_RCTRL = 97,
    SW_KEY_HOME = 102, SW_KEY_UP = 103, SW_KEY_LEFT = 105, SW_KEY_RIGHT = 106, SW_KEY_END = 107,
    SW_KEY_DOWN = 108, SW_KEY_DELETE = 111
};
/* ASCII for a key, uppercase, 0 when it has none. */
char sw_key_to_char(int key);
char sw_shifted_char(char ch);

/* ---- gfx: strictly black and white ------------------------------------ */

typedef enum { SW_PAPER = 0, SW_INK = 1 } SwColor;

typedef struct {
    uint32_t *px;
    int w, h;
    int cx0, cy0, cx1, cy1; /* clip, exclusive on the far side */
    int pat;                /* dither cell size in pixels */
} SwCanvas;

#define SW_SHADES 16 /* dither levels: 0 = paper ... 16 = ink */

void sw_canvas(SwCanvas *c, uint32_t *px, int w, int h);
void sw_clip(SwCanvas *c, int x, int y, int w, int h);
void sw_noclip(SwCanvas *c);
void sw_clear(SwCanvas *c, SwColor color);
void sw_pixel(SwCanvas *c, int x, int y, SwColor color);
void sw_fill(SwCanvas *c, int x, int y, int w, int h, SwColor color);
void sw_fill_shade(SwCanvas *c, int x, int y, int w, int h, int shade);
void sw_stipple(SwCanvas *c, int x, int y, int w, int h, int shade); /* ink only, keeps what's beneath */
void sw_frame(SwCanvas *c, int x, int y, int w, int h, int th, SwColor color);
void sw_line(SwCanvas *c, int x0, int y0, int x1, int y1, int th, SwColor color);
void sw_dash(SwCanvas *c, int x0, int y0, int x1, int y1, int th, int on, int off, SwColor color);
void sw_tri(SwCanvas *c, float x0, float y0, float x1, float y1, float x2, float y2, int shade);
void sw_disc(SwCanvas *c, int cx, int cy, int r, int shade);
void sw_ring(SwCanvas *c, int cx, int cy, int r, int th, SwColor color);
void sw_xor(SwCanvas *c, int x, int y, int w, int h);
void sw_invert_all(SwCanvas *c);
void sw_text(SwCanvas *c, int x, int y, const char *s, int dot, SwColor color);
int sw_text_w(const char *s, int dot);
const uint8_t *sw_glyph(char ch);     /* 7 rows, low 5 bits, or NULL */
bool sw_write_png(const char *path, const uint32_t *px, int w, int h);

#define SW_CHAR_W 6 /* advance in dots */
#define SW_CHAR_H 7

/* ---- desk: inner windows and widgets ---------------------------------- */

#define SW_MAX_PANELS 12
#define SW_MAX_KEYS 32

#define SW_MAX_ELEMS 48
#define SW_SIDE_TOP 1
#define SW_SIDE_BOTTOM 2
#define SW_SIDE_LEFT 4
#define SW_SIDE_RIGHT 8

/* One widget of a panel. The desk measures it each frame, and once the panel has
 * broken, the host can move it elsewhere. It keeps working wherever it ends up. */
typedef struct {
    int x, y, w, h;      /* where it belongs, relative to the panel */
    int ox, oy;          /* how far from there it has got, when loose */
    bool loose, gone;
    char label[20];
    int st, body;        /* free for the host */
} SwElem;

typedef struct {
    int x, y, w, h;      /* outer rect in framebuffer pixels */
    const char *title;
    bool open, shaded;   /* shaded = rolled up to its title bar */
    int solid_id;        /* free for the host */
    /* A broken panel has no back and no frame on the sides in `gone_sides`. Matter
     * can be seen inside it, and the mouse goes through to whatever's behind. */
    bool broken;
    int gone_sides;
    SwElem elems[SW_MAX_ELEMS];
    int nelems;
} SwPanel;

typedef struct {
    int mx, my;
    bool down[3], pressed[3], released[3];
    int wheel;
    int keys[SW_MAX_KEYS]; int nkeys;
    bool shift, ctrl;
    bool inside;
    int dbl;             /* left double click this frame */
} SwInput;

typedef struct {
    SwCanvas *c;
    SwInput in;
    int dot;             /* font dot size */
    int line;            /* thin line width */
    int row;             /* widget row height */
    int pad;

    /* desk bounds, panels are kept inside this */
    int dx0, dy0, dx1, dy1;

    /* panels, back to front */
    SwPanel *panels[SW_MAX_PANELS]; int npanels;
    int hover_panel;     /* index into panels, -1 for none */

    /* widget state */
    SwPanel *cur;
    int cx, cy, cw;      /* layout cursor and content width */
    int line_x;          /* same-line continuation, or -1 */
    uintptr_t hot, active;
    bool blocked;        /* something above owns the mouse */
    int drag_ox, drag_oy;
    SwPanel *dragging;

    /* menus */
    int menu_open;       /* index or -1 */
    int menu_x, menu_y, menu_w, menu_cy;
    int menu_rx, menu_ry, menu_rw, menu_rh; /* last frame's popup rect */
    int bar_x;
    int menu_i;
    int bar_y, bar_h;
    bool menu_click;
    const char *cursor_hint;
    bool mouse_on_ui;    /* the host should ignore the mouse this frame */
} SwDesk;

void sw_desk_init(SwDesk *d, SwCanvas *c, float scale);
void sw_desk_metrics(SwDesk *d, float scale);
void sw_desk_bounds(SwDesk *d, int x0, int y0, int x1, int y1);
void sw_desk_begin(SwDesk *d);
void sw_desk_add(SwDesk *d, SwPanel *p);
void sw_desk_raise(SwDesk *d, SwPanel *p);
bool sw_panel_begin(SwDesk *d, SwPanel *p);
void sw_panel_end(SwDesk *d);

bool sw_button(SwDesk *d, const char *label, bool selected);
bool sw_button_half(SwDesk *d, const char *label, bool selected, bool second);
bool sw_toggle(SwDesk *d, const char *label, bool *value);
bool sw_slider(SwDesk *d, const char *label, float *value, float lo, float hi, const char *fmt);
void sw_label(SwDesk *d, const char *text);
void sw_heading(SwDesk *d, const char *text);
void sw_space(SwDesk *d, int dots);
/* Reserves a box inside the panel for custom drawing. */
void sw_reserve(SwDesk *d, int h, int *x, int *y, int *w);

/* Menu bar: call inside sw_bar_begin/end. Items return true when chosen. */
void sw_bar_begin(SwDesk *d, int x, int y, int w, int h);
bool sw_menu_begin(SwDesk *d, const char *title, int width_chars);
bool sw_menu_item(SwDesk *d, const char *label, bool check);
void sw_menu_sep(SwDesk *d);
void sw_menu_end(SwDesk *d);
void sw_bar_end(SwDesk *d);

#endif
