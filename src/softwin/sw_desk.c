/* SoftWin desk: inner windows, menus and widgets, immediate mode. */
#include "softwin.h"

#include <stdio.h>
#include <string.h>

static bool in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && py >= y && px < x + w && py < y + h;
}

static uintptr_t make_id(const SwDesk *d, const char *label) {
    uintptr_t h = (uintptr_t)d->cur * 31u;
    for (const char *s = label; *s; s++) h = h * 131u + (unsigned char)*s;
    return h | 1u;
}

void sw_desk_metrics(SwDesk *d, float scale) {
    int dot = (int)(2.0f * scale + 0.5f);
    if (dot < 2) dot = 2;
    d->dot = dot;
    d->line = dot / 2;
    if (d->line < 1) d->line = 1;
    d->pad = dot * 2;
    d->row = dot * 11;
    if (d->c) d->c->pat = d->line;
}

void sw_desk_init(SwDesk *d, SwCanvas *c, float scale) {
    memset(d, 0, sizeof *d);
    d->c = c;
    d->menu_open = -1;
    d->hover_panel = -1;
    d->line_x = -1;
    sw_desk_metrics(d, scale);
}

void sw_desk_bounds(SwDesk *d, int x0, int y0, int x1, int y1) {
    d->dx0 = x0; d->dy0 = y0; d->dx1 = x1; d->dy1 = y1;
}

void sw_desk_add(SwDesk *d, SwPanel *p) {
    if (d->npanels < SW_MAX_PANELS) d->panels[d->npanels++] = p;
}

void sw_desk_raise(SwDesk *d, SwPanel *p) {
    int i;
    for (i = 0; i < d->npanels; i++) if (d->panels[i] == p) break;
    if (i >= d->npanels) return;
    for (; i + 1 < d->npanels; i++) d->panels[i] = d->panels[i + 1];
    d->panels[d->npanels - 1] = p;
}

static int title_h(const SwDesk *d) { return d->dot * 10; }

static int panel_visible_h(const SwDesk *d, const SwPanel *p) {
    return p->shaded ? title_h(d) : p->h;
}

/* What of a broken panel can be touched: its title bar and its widgets. */
static bool broken_panel_hit(const SwDesk *d, const SwPanel *p) {
    if (in_rect(d->in.mx, d->in.my, p->x, p->y, p->w, title_h(d))) return true;
    for (int i = 0; i < p->nelems; i++) {
        const SwElem *e = &p->elems[i];
        if (e->gone) continue;
        int ox = e->loose ? e->ox : 0, oy = e->loose ? e->oy : 0;
        if (in_rect(d->in.mx, d->in.my, p->x + e->x + ox, p->y + e->y + oy, e->w, e->h)) return true;
    }
    return false;
}

/* Decides who owns the mouse this frame, from last frame's layout. */
void sw_desk_begin(SwDesk *d) {
    d->hover_panel = -1;
    d->mouse_on_ui = false;
    d->cursor_hint = NULL;
    d->blocked = false;
    d->cur = NULL;
    d->hot = 0;

    /* An open menu popup is above everything. */
    if (d->menu_open >= 0 &&
        (in_rect(d->in.mx, d->in.my, d->menu_rx, d->menu_ry, d->menu_rw, d->menu_rh) ||
         in_rect(d->in.mx, d->in.my, d->dx0, d->bar_y, d->dx1 - d->dx0, d->bar_h))) {
        d->blocked = true;
        d->mouse_on_ui = true;
    } else if (d->menu_open >= 0) {
        /* Clicking away closes the menu and is swallowed. */
        d->mouse_on_ui = true;
        if (d->in.pressed[0] || d->in.pressed[1]) d->menu_open = -1;
        d->blocked = true;
    }
    if (d->bar_h && in_rect(d->in.mx, d->in.my, d->dx0, d->bar_y, d->dx1 - d->dx0, d->bar_h)) {
        d->mouse_on_ui = true;
        d->blocked = true;
    }
    if (!d->blocked && d->in.inside) {
        for (int i = d->npanels - 1; i >= 0; i--) {
            const SwPanel *p = d->panels[i];
            if (p->open && (p->broken ? broken_panel_hit(d, p) : in_rect(d->in.mx, d->in.my, p->x, p->y, p->w, panel_visible_h(d, p)))) {
                d->hover_panel = i;
                d->mouse_on_ui = true;
                break;
            }
        }
    }
    if (d->active || d->dragging) d->mouse_on_ui = true;
    /* `active` survives the release frame, which is when widgets report a click. */
    if (!d->in.down[0] && !d->in.released[0]) d->active = 0;
    if (!d->in.down[0]) d->dragging = NULL;
    if (d->dragging) {
        SwPanel *p = d->dragging;
        p->x = d->in.mx - d->drag_ox;
        p->y = d->in.my - d->drag_oy;
    }
    /* Clicking a panel raises it, before the draw order is walked. */
    if (d->hover_panel >= 0 && d->in.pressed[0]) {
        SwPanel *p = d->panels[d->hover_panel];
        sw_desk_raise(d, p);
        d->hover_panel = d->npanels - 1;
    }
}

static void clamp_panel(const SwDesk *d, SwPanel *p) {
    int h = panel_visible_h(d, p);
    if (p->x + p->w > d->dx1) p->x = d->dx1 - p->w;
    if (p->y + h > d->dy1) p->y = d->dy1 - h;
    if (p->x < d->dx0) p->x = d->dx0;
    if (p->y < d->dy0) p->y = d->dy0;
}

static bool panel_has_mouse(const SwDesk *d) {
    if (d->blocked || d->hover_panel < 0 || !d->cur) return false;
    return d->panels[d->hover_panel] == d->cur;
}

/* A square chrome button at (x, y). Returns true when clicked. */
static bool chrome_button(SwDesk *d, int x, int y, int s, char glyph) {
    SwCanvas *c = d->c;
    bool over = panel_has_mouse(d) && in_rect(d->in.mx, d->in.my, x, y, s, s);
    sw_fill(c, x, y, s, s, over ? SW_INK : SW_PAPER);
    sw_frame(c, x, y, s, s, d->line, over ? SW_PAPER : SW_INK);
    int m = s / 4 + 1;
    SwColor fg = over ? SW_PAPER : SW_INK;
    if (glyph == 'x') {
        sw_line(c, x + m, y + m, x + s - m - 1, y + s - m - 1, d->line, fg);
        sw_line(c, x + s - m - 1, y + m, x + m, y + s - m - 1, d->line, fg);
    } else {
        sw_fill(c, x + m, y + s / 2 - d->line / 2, s - 2 * m, d->line, fg);
    }
    return over && d->in.pressed[0];
}

bool sw_panel_begin(SwDesk *d, SwPanel *p) {
    if (!p->open) return false;
    SwCanvas *c = d->c;
    clamp_panel(d, p);
    d->cur = p;
    int th = title_h(d), vis = panel_visible_h(d, p);

    if (p->broken) {
        /* No back, so matter shows through. What's left of the frame stands, and
         * where a side has gone, its stumps show that it did. */
        int t = d->dot, stump = d->dot * 4;
        if (!(p->gone_sides & SW_SIDE_BOTTOM)) sw_fill(c, p->x, p->y + vis - t, p->w, t, SW_INK);
        else sw_dash(c, p->x, p->y + vis - t / 2, p->x + p->w, p->y + vis - t / 2, d->line, stump, stump * 3, SW_INK);
        if (!(p->gone_sides & SW_SIDE_LEFT)) sw_fill(c, p->x, p->y, t, vis, SW_INK);
        else sw_dash(c, p->x + t / 2, p->y, p->x + t / 2, p->y + vis, d->line, stump, stump * 3, SW_INK);
        if (!(p->gone_sides & SW_SIDE_RIGHT)) sw_fill(c, p->x + p->w - t, p->y, t, vis, SW_INK);
        else sw_dash(c, p->x + p->w - t / 2, p->y, p->x + p->w - t / 2, p->y + vis, d->line, stump, stump * 3, SW_INK);
    } else {
        /* A hard one-sided offset shadow, in dither, reads as depth without grey. */
        int sh = d->dot * 2;
        sw_fill_shade(c, p->x + sh, p->y + vis, p->w, sh, 8);
        sw_fill_shade(c, p->x + p->w, p->y + sh, sh, vis, 8);

        sw_fill(c, p->x, p->y, p->w, vis, SW_PAPER);
        sw_frame(c, p->x, p->y, p->w, vis, d->line, SW_INK);
    }

    /* Title bar: solid ink, inverse text. */
    sw_fill(c, p->x, p->y, p->w, th, SW_INK);
    sw_text(c, p->x + d->pad, p->y + (th - 7 * d->dot) / 2, p->title, d->dot, SW_PAPER);
    /* A title bar that has been broken through still works, and wears a crack. */
    if (p->broken && (p->gone_sides & SW_SIDE_TOP))
        sw_dash(c, p->x, p->y + th - d->line, p->x + p->w, p->y + th - d->line, d->line, d->dot * 2, d->dot * 2, SW_PAPER);

    int bs = th - d->dot * 2;
    int bx = p->x + p->w - bs - d->dot, by = p->y + d->dot;
    if (chrome_button(d, bx, by, bs, 'x')) p->open = false;
    if (chrome_button(d, bx - bs - d->dot, by, bs, '-')) p->shaded = !p->shaded;

    if (panel_has_mouse(d) && d->in.pressed[0] && !d->active &&
        in_rect(d->in.mx, d->in.my, p->x, p->y, bx - bs - d->dot - p->x, th)) {
        d->dragging = p;
        d->drag_ox = d->in.mx - p->x;
        d->drag_oy = d->in.my - p->y;
    }
    if (p->shaded || !p->open) { d->cur = NULL; return false; }
    d->cx = p->x + d->pad;
    d->cy = p->y + th + d->pad;
    d->cw = p->w - 2 * d->pad;
    d->line_x = -1;
    p->nelems = 0;
    return true;
}

/* Every widget comes through here: it notes where the widget belongs, and moves
 * (x, y) to where it actually is. Returns false when it no longer exists. */
static bool elem_place(SwDesk *d, const char *label, int *x, int *y, int w, int h) {
    SwPanel *p = d->cur;
    if (!p || p->nelems >= SW_MAX_ELEMS) return true;
    SwElem *e = &p->elems[p->nelems++];
    e->x = *x - p->x; e->y = *y - p->y; e->w = w; e->h = h;
    snprintf(e->label, sizeof e->label, "%s", label);
    if (e->gone) return false;
    if (e->loose) { *x += e->ox; *y += e->oy; }
    if (p->broken) {
        /* With no panel behind it, a widget brings its own card. */
        sw_fill(d->c, *x, *y, w, h, SW_PAPER);
        if (e->loose) sw_frame(d->c, *x, *y, w, h, d->line, SW_INK);
    }
    return true;
}

void sw_panel_end(SwDesk *d) {
    if (d->cur) d->cur->h = d->cy - d->cur->y + d->pad - d->dot;
    d->cur = NULL;
}

/* ---- widgets ------------------------------------------------------------ */

static bool button_at(SwDesk *d, int x, int y, int w, int h, const char *label, bool selected) {
    SwCanvas *c = d->c;
    uintptr_t id = make_id(d, label);
    if (!elem_place(d, label, &x, &y, w, h)) return false;
    bool over = panel_has_mouse(d) && in_rect(d->in.mx, d->in.my, x, y, w, h);
    if (over) {
        d->hot = id;
        if (d->in.pressed[0]) d->active = id;
    }
    bool held = d->active == id && over && d->in.down[0];
    bool fill = selected != held;
    sw_fill(c, x, y, w, h, fill ? SW_INK : SW_PAPER);
    sw_frame(c, x, y, w, h, over && !fill ? d->line * 2 : d->line, SW_INK);
    int tw = sw_text_w(label, d->dot);
    sw_text(c, x + (w - tw) / 2, y + (h - 7 * d->dot) / 2, label, d->dot, fill ? SW_PAPER : SW_INK);
    return over && d->active == id && d->in.released[0];
}

bool sw_button(SwDesk *d, const char *label, bool selected) {
    int h = d->row;
    bool r = button_at(d, d->cx, d->cy, d->cw, h, label, selected);
    d->cy += h + d->dot;
    return r;
}

bool sw_button_half(SwDesk *d, const char *label, bool selected, bool second) {
    int gap = d->dot, w = (d->cw - gap) / 2, h = d->row;
    int x = second ? d->cx + w + gap : d->cx;
    bool r = button_at(d, x, d->cy, second ? d->cw - w - gap : w, h, label, selected);
    if (second) d->cy += h + d->dot;
    return r;
}

bool sw_toggle(SwDesk *d, const char *label, bool *value) {
    SwCanvas *c = d->c;
    int h = d->dot * 9, s = d->dot * 7, x = d->cx, y = d->cy;
    d->cy += h + d->dot;
    uintptr_t id = make_id(d, label);
    if (!elem_place(d, label, &x, &y, d->cw, h)) return false;
    bool over = panel_has_mouse(d) && in_rect(d->in.mx, d->in.my, x, y, d->cw, h);
    if (over && d->in.pressed[0]) d->active = id;
    bool clicked = over && d->active == id && d->in.released[0];
    if (clicked) *value = !*value;
    sw_frame(c, x, y + d->dot, s, s, d->line, SW_INK);
    if (*value) sw_fill(c, x + d->dot * 2, y + d->dot * 3, s - d->dot * 4, s - d->dot * 4, SW_INK);
    sw_text(c, x + s + d->dot * 3, y + d->dot, label, d->dot, SW_INK);
    if (over) sw_fill(c, x + s + d->dot * 3, y + h - d->line, sw_text_w(label, d->dot), d->line, SW_INK);
    return clicked;
}

bool sw_slider(SwDesk *d, const char *label, float *value, float lo, float hi, const char *fmt) {
    SwCanvas *c = d->c;
    char val[32];
    int x = d->cx, y = d->cy, th = d->dot * 7;
    d->cy += d->dot * 9 + th + d->dot * 3;
    if (!elem_place(d, label, &x, &y, d->cw, d->dot * 9 + th)) return false;
    snprintf(val, sizeof val, fmt, (double)*value);
    sw_text(c, x, y, label, d->dot, SW_INK);
    sw_text(c, x + d->cw - sw_text_w(val, d->dot), y, val, d->dot, SW_INK);
    int ty = y + d->dot * 9;
    uintptr_t id = make_id(d, label);
    bool over = panel_has_mouse(d) && in_rect(d->in.mx, d->in.my, x, ty, d->cw, th);
    if (over && d->in.pressed[0]) d->active = id;
    bool changed = false;
    if (d->active == id && d->in.down[0]) {
        float t = (float)(d->in.mx - x) / (float)(d->cw > 1 ? d->cw - 1 : 1);
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        *value = lo + (hi - lo) * t;
        changed = true;
    }
    if (over && d->in.wheel) {
        float v = *value - (float)d->in.wheel * (hi - lo) / 40.0f;
        *value = v < lo ? lo : v > hi ? hi : v;
        changed = true;
    }
    /* Track: a dithered bar up to the value, inside a hard frame, then a post. */
    float t = hi > lo ? (*value - lo) / (hi - lo) : 0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int fw = (int)(t * (float)(d->cw - 2 * d->line));
    sw_frame(c, x, ty, d->cw, th, d->line, SW_INK);
    sw_fill_shade(c, x + d->line, ty + d->line, fw, th - 2 * d->line, 8);
    sw_fill(c, x + d->line + fw - d->dot, ty, d->dot * 2, th, SW_INK);
    return changed;
}

void sw_label(SwDesk *d, const char *text) {
    int x = d->cx, y = d->cy;
    d->cy += d->dot * 9;
    if (elem_place(d, text, &x, &y, sw_text_w(text, d->dot), d->dot * 7))
        sw_text(d->c, x, y, text, d->dot, SW_INK);
}

void sw_heading(SwDesk *d, const char *text) {
    int tw = sw_text_w(text, d->dot), x = d->cx, y = d->cy;
    d->cy += d->dot * 10;
    if (!elem_place(d, text, &x, &y, d->cw, d->dot * 7)) return;
    sw_text(d->c, x, y, text, d->dot, SW_INK);
    sw_fill(d->c, x + tw + d->dot * 2, y + (7 * d->dot) / 2, d->cw - tw - d->dot * 2, d->line, SW_INK);
}

void sw_space(SwDesk *d, int dots) { d->cy += d->dot * dots; }

void sw_reserve(SwDesk *d, int h, int *x, int *y, int *w) {
    *x = d->cx; *y = d->cy; *w = d->cw;
    d->cy += h + d->dot;
    if (!elem_place(d, "", x, y, d->cw, h)) *x = *y = -30000; /* the caller draws, off the edge of everything */
}

/* ---- menus -------------------------------------------------------------- */

void sw_bar_begin(SwDesk *d, int x, int y, int w, int h) {
    SwCanvas *c = d->c;
    d->bar_y = y; d->bar_h = h;
    d->bar_x = x + d->pad;
    d->menu_i = 0;
    d->menu_click = false;
    sw_fill(c, x, y, w, h, SW_PAPER);
    sw_fill(c, x, y + h - d->line, w, d->line, SW_INK);
}

bool sw_menu_begin(SwDesk *d, const char *title, int width_chars) {
    SwCanvas *c = d->c;
    int i = d->menu_i++, tw = sw_text_w(title, d->dot) + d->dot * 6;
    int x = d->bar_x, y = d->bar_y, h = d->bar_h - d->line;
    d->bar_x += tw;
    bool over = d->in.inside && in_rect(d->in.mx, d->in.my, x, y, tw, h);
    if (over && d->in.pressed[0]) d->menu_open = d->menu_open == i ? -1 : i;
    /* Sliding across the bar with a menu open switches menus. */
    else if (over && d->menu_open >= 0) d->menu_open = i;
    bool open = d->menu_open == i;
    sw_fill(c, x, y, tw, h, open ? SW_INK : SW_PAPER);
    sw_text(c, x + d->dot * 3, y + (h - 7 * d->dot) / 2, title, d->dot, open ? SW_PAPER : SW_INK);
    if (over && !open) sw_fill(c, x + d->dot * 3, y + h - d->dot, tw - d->dot * 6, d->line, SW_INK);
    if (!open) return false;
    d->menu_x = x;
    d->menu_y = y + d->bar_h;
    d->menu_w = (width_chars * SW_CHAR_W + 6) * d->dot;
    d->menu_cy = d->menu_y + d->line;
    return true;
}

/* The popup height isn't known up front, so items draw in a reserved run and
 * the frame is drawn last, using the height reached. Items paint their own
 * background, so nothing beneath shows through. */
bool sw_menu_item(SwDesk *d, const char *label, bool check) {
    SwCanvas *c = d->c;
    int h = d->dot * 11, x = d->menu_x, w = d->menu_w, y = d->menu_cy;
    bool over = in_rect(d->in.mx, d->in.my, x, y, w, h);
    sw_fill(c, x, y, w, h, over ? SW_INK : SW_PAPER);
    SwColor fg = over ? SW_PAPER : SW_INK;
    sw_text(c, x + d->dot * 3, y + (h - 7 * d->dot) / 2, label, d->dot, fg);
    if (check) {
        int s = d->dot * 3;
        sw_fill(c, x + w - s * 2, y + (h - s) / 2, s, s, fg);
    }
    d->menu_cy += h;
    if (over && d->in.released[0] && !d->in.pressed[0]) {
        d->menu_click = true;
        return true;
    }
    return false;
}

void sw_menu_sep(SwDesk *d) {
    int h = d->dot * 3;
    sw_fill(d->c, d->menu_x, d->menu_cy, d->menu_w, h, SW_PAPER);
    sw_fill(d->c, d->menu_x, d->menu_cy + h / 2, d->menu_w, d->line, SW_INK);
    d->menu_cy += h;
}

void sw_menu_end(SwDesk *d) {
    int h = d->menu_cy - d->menu_y + d->line;
    SwCanvas *c = d->c;
    int sh = d->dot * 2;
    sw_fill_shade(c, d->menu_x + sh, d->menu_y + h, d->menu_w, sh, 8);
    sw_fill_shade(c, d->menu_x + d->menu_w, d->menu_y + sh, sh, h, 8);
    sw_frame(c, d->menu_x, d->menu_y, d->menu_w, h, d->line, SW_INK);
    d->menu_rx = d->menu_x; d->menu_ry = d->menu_y;
    d->menu_rw = d->menu_w; d->menu_rh = h;
}

void sw_bar_end(SwDesk *d) {
    if (d->menu_click) d->menu_open = -1;
}
