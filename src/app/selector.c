/* The toy selector: what the program opens on, with live previews of each toy. */
#include "app.h"

#include <math.h>
#include <stdio.h>

typedef struct { const char *name; const char *line1, *line2; ToyMode mode; } Card;
static const Card CARDS[2] = {
    { "SOFTPHYS", "POWDER, FLUID, GAS AND HEAT.", "MIX THINGS. WATCH THEM REACT.", MODE_PHYS },
    { "SOFTBODIES", "JELLY, CLOTH, ROPE AND GOO.", "SQUISH THINGS. NUKE THEM.", MODE_BODIES },
};

static void add_blob(SpWorld *w, float x, float y, float r) {
    float p[24 * 2];
    for (int i = 0; i < 24; i++) {
        p[i * 2] = x + cosf((float)i * 0.2618f) * r;
        p[i * 2 + 1] = y + sinf((float)i * 0.2618f) * r;
    }
    SpMaterial m = { 0 };
    m.spacing = 7;
    sp_add_polygon(w, p, 24, &m);
}

static void preview_setup(App *a, int cw, int ch, int cell) {
    SelectorState *s = &a->sel;
    int gw = cw / cell + 1, gh = ch / cell + 1;
    if (!s->grid || s->grid->w != gw || s->grid->h != gh) {
        phys_free(s->grid);
        s->grid = phys_new(gw, gh);
        PhysGrid *g = s->grid;
        phys_rect(g, 0, gh - 2, gw - 1, gh - 1, PE_WALL);
        phys_rect(g, gw / 2 - 3, gh - 2, gw / 2 + 3, gh - 2, PE_DRAIN);
        for (int i = -1; i <= 1; i++) {
            phys_set_spout(g, gw / 4 + i, 1, PE_SAND);
            phys_set_spout(g, gw * 3 / 4 + i, 1, PE_WATER);
        }
        phys_line(g, gw / 8, gh * 2 / 3, gw * 3 / 8, gh / 2, 0, PE_WOOD, true);
        phys_line(g, gw * 7 / 8, gh * 2 / 3, gw * 5 / 8, gh / 2, 0, PE_METAL, true);
    }
    float bw = (float)cw / a->vs, bh = (float)ch / a->vs;
    if (!s->world) {
        s->world = sp_world_new();
        s->world->substeps = 8;
        sp_set_bounds(s->world, 0, 0, bw, bh, true);
        add_blob(s->world, bw * 0.3f, bh * 0.4f, bh * 0.17f);
        add_blob(s->world, bw * 0.55f, bh * 0.25f, bh * 0.13f);
        add_blob(s->world, bw * 0.75f, bh * 0.45f, bh * 0.2f);
        sp_add_balloon(s->world, bw * 0.45f, bh * 0.7f, bh * 0.16f, NULL);
    }
    sp_set_bounds(s->world, 0, 0, bw, bh, false);
}

void selector_update(App *a, float dt) {
    SelectorState *s = &a->sel;
    if (!s->world || !s->grid) return;
    s->t += dt;
    s->acc += dt;
    while (s->acc >= 1.0f / 60) {
        s->acc -= 1.0f / 60;
        phys_step(s->grid);
        sp_step(s->world, 1.0f / 60);
    }
    if (s->t - s->last_kick > 2.6f) {
        s->last_kick = s->t;
        float bw = s->world->bx1, bh = s->world->by1;
        sp_blast(s->world, bw * (0.3f + app_rand(a) * 0.4f), bh, bw, 900);
    }
}

static void draw_body_preview(App *a, int ox, int oy) {
    SwCanvas *c = &a->canvas;
    const SpWorld *w = a->sel.world;
    float vs = a->vs;
    int shades[4] = { 8, 16, 4, 2 }, n = 0;
#define PX(v) ((float)ox + (v) * vs)
#define PY(v) ((float)oy + (v) * vs)
    for (int s = 0; s < SP_MAX_BODIES; s++) {
        const SpBody *b = &w->bodies[s];
        if (!b->alive) continue;
        float cx, cy;
        sp_body_centroid(w, b, &cx, &cy);
        for (int e = 0; e < b->nedges; e++) {
            int i = b->first + b->edges[e].a, j = b->first + b->edges[e].b;
            sw_tri(c, PX(cx), PY(cy), PX(w->x[i]), PY(w->y[i]), PX(w->x[j]), PY(w->y[j]), shades[n % 4]);
        }
        for (int e = 0; e < b->nedges; e++) {
            int i = b->first + b->edges[e].a, j = b->first + b->edges[e].b;
            sw_line(c, (int)PX(w->x[i]), (int)PY(w->y[i]), (int)PX(w->x[j]), (int)PY(w->y[j]), a->desk.line + 1, SW_INK);
        }
        int er = (int)(b->radius * 0.2f * vs);
        for (int side = -1; side <= 1; side += 2) {
            int ex = (int)PX(cx + (float)side * b->radius * 0.34f), ey = (int)PY(cy - b->radius * 0.12f);
            sw_disc(c, ex, ey, er + a->desk.line, 0);
            sw_disc(c, ex, ey, er, 16);
            sw_disc(c, ex, ey, er / 2, 0);
        }
        n++;
    }
#undef PX
#undef PY
}

void selector_draw(App *a) {
    SwCanvas *c = &a->canvas;
    SwDesk *d = &a->desk;
    int u = d->dot;
    sw_clip(c, a->ax, a->ay, a->aw, a->ah);
    sw_fill(c, a->ax, a->ay, a->aw, a->ah, SW_PAPER);

    const char *title = "SOFT TOYS";
    int big = u * 3;
    int top = a->ay + (a->ah - (u * 142 + big * 7 + u * 28)) / 2;
    if (top < a->ay + u * 2) top = a->ay + u * 2;
    sw_text(c, a->ax + (a->aw - sw_text_w(title, big)) / 2, top, title, big, SW_INK);
    const char *hint = "PICK A TOY. SWITCH ANY TIME FROM THE TOY MENU.";
    sw_text(c, a->ax + (a->aw - sw_text_w(hint, u)) / 2, top + big * 7 + u * 5, hint, u, SW_INK);

    int gap = u * 8;
    int cw = (a->aw - gap * 3) / 2;
    if (cw > u * 190) cw = u * 190;
    int ch = u * 142;
    int y = top + big * 7 + u * 20;
    if (y + ch > a->ay + a->ah) ch = a->ay + a->ah - y - u * 2;
    if (ch < u * 40) ch = u * 40;
    int x0 = a->ax + (a->aw - cw * 2 - gap) / 2;
    int info = u * 44, pw = cw - u * 4, ph = ch - info;
    if (ph < u * 8) ph = u * 8;
    int cell = (int)(2 * a->vs + 0.5f);
    if (cell < 2) cell = 2;
    preview_setup(a, pw, ph, cell);

    a->sel.hover = -1;
    for (int i = 0; i < 2; i++) {
        int x = x0 + i * (cw + gap);
        bool over = !d->mouse_on_ui && d->in.inside && d->in.mx >= x && d->in.mx < x + cw && d->in.my >= y && d->in.my < y + ch;
        bool lit = over || a->sel.focus == i;
        if (over) a->sel.hover = i;
        /* The preview */
        sw_clip(c, x + u * 2, y + u * 2, pw, ph);
        sw_fill(c, x + u * 2, y + u * 2, pw, ph, SW_PAPER);
        if (CARDS[i].mode == MODE_PHYS) phys_draw(c, a->sel.grid, x + u * 2, y + u * 2, cell, false);
        else draw_body_preview(a, x + u * 2, y + u * 2);
        sw_clip(c, a->ax, a->ay, a->aw, a->ah);
        /* The label strip: fully inverted when it's the one you'd pick. */
        int ly = y + u * 2 + ph;
        sw_fill(c, x, ly, cw, ch - u * 2 - ph, lit ? SW_INK : SW_PAPER);
        SwColor fg = lit ? SW_PAPER : SW_INK;
        char name[32];
        snprintf(name, sizeof name, "%d  %s", i + 1, CARDS[i].name);
        sw_text(c, x + u * 4, ly + u * 4, name, u * 2, fg);
        sw_text(c, x + u * 4, ly + u * 22, CARDS[i].line1, u, fg);
        sw_text(c, x + u * 4, ly + u * 32, CARDS[i].line2, u, fg);
        sw_frame(c, x, y, cw, ch, lit ? d->line * 3 : d->line, SW_INK);
        if (over && d->in.pressed[0]) app_set_mode(a, CARDS[i].mode);
    }
    sw_noclip(c);
}

void selector_key(App *a, int key) {
    if (key == SW_KEY_1 || key == SW_KEY_1 + 1) app_set_mode(a, CARDS[key - SW_KEY_1].mode);
    else if (key == SW_KEY_LEFT || key == SW_KEY_RIGHT || key == SW_KEY_TAB) a->sel.focus = !a->sel.focus;
    else if (key == SW_KEY_ENTER) app_set_mode(a, CARDS[a->sel.focus].mode);
}
