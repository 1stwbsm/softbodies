/* SoftWin X11 backend: the core protocol by hand, no Xlib or XCB.
 *
 * Requests are little-endian 32-bit word streams. A blocking GetInputFocus
 * round trip after each frame paces PutImage so requests never pile up.
 */
#define _GNU_SOURCE
#include "softwin.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

typedef struct {
    int fd;
    uint8_t in[65536]; size_t in_n;
    uint32_t id_base, id_mask, next_id;
    uint32_t root, window, gc, visual;
    uint8_t depth;
    uint32_t max_req_bytes;
    uint32_t a_protocols, a_delete, a_moveresize, a_change_state, a_motif;
    int root_x, root_y;   /* pointer on the root, from the last event */
    int press_button;
    bool frame_pending;   /* waiting on the pacing round trip */
    double last_frame;
    bool mapped;
    bool fatal;
    uint16_t seq;         /* last request sent */
    uint16_t sync_seq;
    uint16_t pos_seq;     /* the request asking where the window is */
    bool pos_pending;
    int screen_w, screen_h;
    uint32_t a_workarea;
    uint8_t shape_major;  /* the SHAPE extension, 0 when the server lacks it */
    uint32_t overlay;
    bool overlay_mapped;
    int overlay_frames;
    uint32_t *rects;      /* scratch: the overlay's visible rows, as x,y,w,h in 16 bits */
    int rects_cap;
} X;

/* Drains what is readable, keeping the data for later dispatch. */
static void x_read_some(X *c) {
    if (c->in_n == sizeof c->in) return;
    ssize_t n = recv(c->fd, c->in + c->in_n, sizeof c->in - c->in_n, MSG_DONTWAIT);
    if (n > 0) c->in_n += (size_t)n;
    else if (n == 0) c->fatal = true;
    else if (errno != EAGAIN && errno != EINTR) c->fatal = true;
}

/* Writes it all. While the server can't take more, reads, so that neither side
 * can deadlock with both socket buffers full. */
static void x_write(X *c, const void *d, size_t n) {
    const uint8_t *p = d;
    while (n && !c->fatal) {
        ssize_t w = send(c->fd, p, n, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w > 0) { p += w; n -= (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && errno == EAGAIN) {
            struct pollfd pf = { c->fd, POLLOUT | POLLIN, 0 };
            poll(&pf, 1, 1000);
            if (pf.revents & POLLIN) x_read_some(c);
            continue;
        }
        c->fatal = true;
    }
}

static void x_req(X *c, const void *d, size_t n) { c->seq++; x_write(c, d, n); }
static uint32_t x_new(X *c) { return c->id_base | (c->next_id++ & c->id_mask); }

/* Reads until a reply to `seq` arrives, parsing it into `reply`, and keeping
 * any events ahead of it in the input buffer. Used during setup only. */
static bool x_wait_reply_n(X *c, uint16_t seq, uint8_t *reply, size_t cap) {
    double deadline = sw_now() + 3;
    size_t scan = 0;
    while (!c->fatal && sw_now() < deadline) {
        while (c->in_n - scan >= 32) {
            uint8_t *m = c->in + scan;
            uint32_t extra = 0;
            if (m[0] == 1) memcpy(&extra, m + 4, 4);
            size_t size = 32 + (size_t)extra * 4;
            if (c->in_n - scan < size) break;
            uint16_t s;
            memcpy(&s, m + 2, 2);
            if (m[0] == 1 && s == seq) {
                memcpy(reply, m, size < cap ? size : cap);
                memmove(m, m + size, c->in_n - scan - size);
                c->in_n -= size;
                return true;
            }
            scan += size;
        }
        struct pollfd pf = { c->fd, POLLIN, 0 };
        poll(&pf, 1, 100);
        x_read_some(c);
    }
    return false;
}

static bool x_wait_reply(X *c, uint16_t seq, uint8_t *reply) { return x_wait_reply_n(c, seq, reply, 32); }

static uint32_t x_atom(X *c, const char *name) {
    uint16_t len = (uint16_t)strlen(name), words = 2 + (len + 3) / 4;
    uint8_t b[128] = { 16, 0 };
    memcpy(b + 2, &words, 2);
    memcpy(b + 4, &len, 2);
    memcpy(b + 8, name, len);
    x_req(c, b, (size_t)words * 4);
    uint8_t r[32];
    if (!x_wait_reply(c, c->seq, r)) return 0;
    uint32_t atom;
    memcpy(&atom, r + 8, 4);
    return atom;
}

static void x_change_prop(X *c, uint32_t prop, uint32_t type, int format, const void *data, uint32_t count) {
    uint32_t bytes = count * (uint32_t)format / 8, words = (bytes + 3) / 4;
    uint32_t len = 6 + words;
    uint8_t *b = calloc(1, len * 4);
    b[0] = 18; b[1] = 0;
    uint16_t l16 = (uint16_t)len;
    memcpy(b + 2, &l16, 2);
    memcpy(b + 4, &c->window, 4);
    memcpy(b + 8, &prop, 4);
    memcpy(b + 12, &type, 4);
    b[16] = (uint8_t)format;
    memcpy(b + 20, &count, 4);
    memcpy(b + 24, data, bytes);
    x_req(c, b, len * 4);
    free(b);
}

/* The desktop minus the panels, as the window manager sees it. Without a window
 * manager that says, it's the whole screen. */
static void x_read_workarea(SwWindow *win, X *c) {
    win->desk_x0 = win->desk_y0 = 0;
    win->desk_x1 = c->screen_w;
    win->desk_y1 = c->screen_h;
    win->has_desk = true;
    if (!c->a_workarea) return;
    /* GetProperty: the first desktop's four CARDINALs. */
    uint32_t b[6] = { 20u | (6u << 16), c->root, c->a_workarea, 6, 0, 4 };
    x_req(c, b, 24);
    uint8_t r[32 + 16];
    memset(r, 0, sizeof r);
    if (!x_wait_reply_n(c, c->seq, r, sizeof r)) return;
    uint32_t count, area[4];
    memcpy(&count, r + 16, 4);
    if (r[1] != 32 || count < 4) return;
    memcpy(area, r + 32, 16);
    if (area[2] < 1 || area[3] < 1) return;
    win->desk_x0 = (int)area[0];
    win->desk_y0 = (int)area[1];
    win->desk_x1 = (int)(area[0] + area[2]);
    win->desk_y1 = (int)(area[1] + area[3]);
}

/* Reads MIT-MAGIC-COOKIE-1 for this display from the Xauthority file. */
static int read_cookie(const char *display_num, uint8_t *cookie) {
    const char *path = getenv("XAUTHORITY");
    char buf[512];
    if (!path || !*path) {
        const char *home = getenv("HOME");
        if (!home) return 0;
        snprintf(buf, sizeof buf, "%s/.Xauthority", home);
        path = buf;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int found = 0, fallback = 0;
    uint8_t fb[16];
    for (;;) {
        uint8_t h[2];
        if (fread(h, 1, 2, f) != 2) break;
        char field[3][256];
        int flen[3] = { 0, 0, 0 };
        bool bad = false;
        for (int i = 0; i < 3 && !bad; i++) {
            uint8_t l[2];
            if (fread(l, 1, 2, f) != 2) { bad = true; break; }
            flen[i] = (l[0] << 8) | l[1];
            if (flen[i] > 255 || fread(field[i], 1, (size_t)flen[i], f) != (size_t)flen[i]) { bad = true; break; }
            field[i][flen[i]] = 0;
        }
        if (bad) break;
        uint8_t l[2];
        if (fread(l, 1, 2, f) != 2) break;
        int dlen = (l[0] << 8) | l[1];
        uint8_t data[256];
        if (dlen > 255 || fread(data, 1, (size_t)dlen, f) != (size_t)dlen) break;
        if (strcmp(field[2], "MIT-MAGIC-COOKIE-1") || dlen != 16) continue;
        if (!strcmp(field[1], display_num)) { memcpy(cookie, data, 16); found = 16; break; }
        if (flen[1] == 0 && !fallback) { memcpy(fb, data, 16); fallback = 16; }
    }
    fclose(f);
    if (!found && fallback) { memcpy(cookie, fb, 16); found = 16; }
    return found;
}

static bool x_open(SwWindow *win, const char *title, int w, int h) {
    const char *disp = getenv("DISPLAY");
    if (!disp) return false;
    const char *colon = strrchr(disp, ':');
    if (!colon || colon != disp) {
        /* Host-qualified displays need TCP, which this backend doesn't do. */
        if (!colon || strncmp(disp, "unix", (size_t)(colon - disp))) return false;
    }
    char num[16];
    snprintf(num, sizeof num, "%s", colon + 1);
    char *dot = strchr(num, '.');
    if (dot) *dot = 0;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof addr.sun_path, "/tmp/.X11-unix/X%s", num);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { close(fd); return false; }

    X *c = calloc(1, sizeof *c);
    c->fd = fd;
    win->impl = c;

    /* Connection setup */
    uint8_t cookie[16];
    int clen = read_cookie(num, cookie);
    uint8_t setup[12 + 20 + 16] = { 'l', 0, 11, 0, 0, 0 };
    uint16_t nlen = clen ? 18 : 0, dlen = (uint16_t)clen;
    memcpy(setup + 6, &nlen, 2);
    memcpy(setup + 8, &dlen, 2);
    size_t slen = 12;
    if (clen) {
        memcpy(setup + 12, "MIT-MAGIC-COOKIE-1", 18);
        memcpy(setup + 32, cookie, 16);
        slen = 48;
    }
    x_write(c, setup, slen);

    uint8_t head[8];
    size_t got = 0;
    double deadline = sw_now() + 3;
    while (got < 8 && sw_now() < deadline) {
        ssize_t n = recv(fd, head + got, 8 - got, MSG_DONTWAIT);
        if (n > 0) got += (size_t)n;
        else if (n == 0) break;
        else { struct pollfd pf = { fd, POLLIN, 0 }; poll(&pf, 1, 100); }
    }
    bool ok = false;
    uint8_t *info = NULL;
    if (got == 8) {
        uint16_t more;
        memcpy(&more, head + 6, 2);
        size_t need = (size_t)more * 4;
        info = malloc(need + 1);
        got = 0;
        while (got < need && sw_now() < deadline) {
            ssize_t n = recv(fd, info + got, need - got, MSG_DONTWAIT);
            if (n > 0) got += (size_t)n;
            else if (n == 0) break;
            else { struct pollfd pf = { fd, POLLIN, 0 }; poll(&pf, 1, 100); }
        }
        ok = head[0] == 1 && got == need;
        if (head[0] != 1 && got) fprintf(stderr, "softwin: X11 setup refused: %.*s\n", head[1], (char *)info);
    }
    if (!ok) { free(info); close(fd); free(c); win->impl = NULL; return false; }

    memcpy(&c->id_base, info + 4, 4);
    memcpy(&c->id_mask, info + 8, 4);
    uint16_t vendor_len, max_req;
    memcpy(&vendor_len, info + 16, 2);
    memcpy(&max_req, info + 18, 2);
    uint8_t nformats = info[21];
    size_t off = 32 + (((size_t)vendor_len + 3) & ~3u);
    bool has32 = false;
    for (int i = 0; i < nformats; i++, off += 8)
        if (info[off] == 24 && info[off + 1] == 32) has32 = true;
    memcpy(&c->root, info + off, 4);
    memcpy(&c->visual, info + off + 32, 4);
    c->depth = info[off + 38];
    uint16_t screen_w, screen_h;
    memcpy(&screen_w, info + off + 20, 2);
    memcpy(&screen_h, info + off + 22, 2);
    c->screen_w = screen_w;
    c->screen_h = screen_h;
    free(info);
    if (!has32 || c->depth != 24) {
        fprintf(stderr, "softwin: X11 backend needs a 24-bit screen at 32 bits per pixel\n");
        close(fd); free(c); win->impl = NULL;
        return false;
    }
    c->max_req_bytes = (uint32_t)max_req * 4;

    /* Window */
    c->window = x_new(c);
    {
        /* value mask: background pixel | event mask */
        uint32_t event_mask = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40 | 0x8000 | 0x20000;
        uint8_t b[40];
        b[0] = 1; b[1] = c->depth;
        uint16_t len = 10;
        memcpy(b + 2, &len, 2);
        memcpy(b + 4, &c->window, 4);
        memcpy(b + 8, &c->root, 4);
        int16_t zero = 0;
        memcpy(b + 12, &zero, 2);
        memcpy(b + 14, &zero, 2);
        uint16_t uw = (uint16_t)w, uh = (uint16_t)h, border = 0, cls = 1;
        memcpy(b + 16, &uw, 2);
        memcpy(b + 18, &uh, 2);
        memcpy(b + 20, &border, 2);
        memcpy(b + 22, &cls, 2);
        memcpy(b + 24, &c->visual, 4);
        uint32_t mask = 0x2 | 0x800, bg = 0;
        memcpy(b + 28, &mask, 4);
        memcpy(b + 32, &bg, 4);
        memcpy(b + 36, &event_mask, 4);
        x_req(c, b, 40);
    }
    /* GC */
    c->gc = x_new(c);
    {
        uint32_t b[4];
        b[0] = 55u | (4u << 16);
        b[1] = c->gc;
        b[2] = c->window;
        b[3] = 0;
        x_req(c, b, 16);
    }
    uint32_t a_string = 31, a_atom = 4;
    uint32_t a_utf8 = x_atom(c, "UTF8_STRING"), a_name = x_atom(c, "_NET_WM_NAME");
    c->a_protocols = x_atom(c, "WM_PROTOCOLS");
    c->a_delete = x_atom(c, "WM_DELETE_WINDOW");
    c->a_moveresize = x_atom(c, "_NET_WM_MOVERESIZE");
    c->a_change_state = x_atom(c, "WM_CHANGE_STATE");
    c->a_motif = x_atom(c, "_MOTIF_WM_HINTS");
    c->a_workarea = x_atom(c, "_NET_WORKAREA");
    x_read_workarea(win, c);
    /* QueryExtension: the overlay needs SHAPE to be see-through and click-through. */
    {
        uint8_t b[16] = { 98, 0 };
        uint16_t words = 4, len = 5;
        memcpy(b + 2, &words, 2);
        memcpy(b + 4, &len, 2);
        memcpy(b + 8, "SHAPE", 5);
        x_req(c, b, 16);
        uint8_t r[32];
        if (x_wait_reply(c, c->seq, r) && r[8]) c->shape_major = r[9];
    }

    x_change_prop(c, 39, a_string, 8, title, (uint32_t)strlen(title)); /* WM_NAME */
    x_change_prop(c, a_name, a_utf8, 8, title, (uint32_t)strlen(title));
    x_change_prop(c, 67, a_string, 8, "softbodies\0softbodies", 22); /* WM_CLASS */
    x_change_prop(c, c->a_protocols, a_atom, 32, &c->a_delete, 1);
    uint32_t motif[5] = { 2, 0, 0, 0, 0 }; /* flags: decorations; decorations: none */
    x_change_prop(c, c->a_motif, c->a_motif, 32, motif, 5);
    uint32_t hints[18] = { 0 };
    hints[0] = 16; /* PMinSize */
    hints[5] = 480; hints[6] = 360;
    x_change_prop(c, 40, 41, 32, hints, 18); /* WM_NORMAL_HINTS */

    /* A blank cursor: SoftWin draws its own. */
    {
        uint32_t pix = x_new(c), cur = x_new(c);
        uint32_t p[4] = { 53u | (1u << 8) | (4u << 16), pix, c->window, 1u | (1u << 16) };
        x_req(c, p, 16);
        uint32_t k[8] = { 93u | (8u << 16), cur, pix, pix, 0, 0, 0, 0 };
        x_req(c, k, 32);
        uint32_t a[4] = { 2u | (4u << 16), c->window, 0x4000, cur };
        x_req(c, a, 16);
        uint32_t fp[2] = { 54u | (2u << 16), pix };
        x_req(c, fp, 8);
    }
    /* Map */
    uint32_t map[2] = { 8u | (2u << 16), c->window };
    x_req(c, map, 8);
    c->mapped = true;
    sw_resize_fb(win, w, h);
    return true;
}

static void x_close(SwWindow *win) {
    X *c = win->impl;
    if (!c) return;
    close(c->fd);
    free(c->rects);
    free(win->outer);
    win->outer = NULL;
    free(c);
    win->impl = NULL;
}

static void handle_event(SwWindow *win, X *c, const uint8_t *e) {
    int16_t ex, ey, rx, ry;
    memcpy(&rx, e + 20, 2);
    memcpy(&ry, e + 22, 2);
    memcpy(&ex, e + 24, 2);
    memcpy(&ey, e + 26, 2);
    switch (e[0] & 0x7F) {
    case 0: {
        uint16_t seq;
        uint32_t bad;
        memcpy(&seq, e + 2, 2);
        memcpy(&bad, e + 4, 4);
        fprintf(stderr, "softwin: X11 error %u for request opcode %u, seq %u, value 0x%x\n", e[1], e[10], seq, bad);
        break;
    }
    case 2: sw_push(win, (SwEvent){ .type = SW_EV_KEY_DOWN, .key = e[1] - 8 }); break;
    case 3: sw_push(win, (SwEvent){ .type = SW_EV_KEY_UP, .key = e[1] - 8 }); break;
    case 4:
    case 5: {
        c->root_x = rx; c->root_y = ry;
        bool down = (e[0] & 0x7F) == 4;
        int d = e[1];
        if (d == 4 || d == 5) {
            if (down) sw_push(win, (SwEvent){ .type = SW_EV_WHEEL, .wheel = d == 4 ? -1 : 1 });
            break;
        }
        if (d > 3) break;
        int map[4] = { 0, 0, 2, 1 };
        if (down) c->press_button = d;
        sw_push(win, (SwEvent){ .type = down ? SW_EV_BUTTON_DOWN : SW_EV_BUTTON_UP, .button = map[d], .x = ex, .y = ey });
        break;
    }
    case 6:
    case 7:
        c->root_x = rx; c->root_y = ry;
        sw_push(win, (SwEvent){ .type = SW_EV_MOTION, .x = ex, .y = ey });
        break;
    case 8:
        sw_push(win, (SwEvent){ .type = SW_EV_LEAVE });
        break;
    case 22: { /* ConfigureNotify */
        uint16_t w, h;
        memcpy(&w, e + 20, 2);
        memcpy(&h, e + 22, 2);
        sw_resize_fb(win, w, h);
        break;
    }
    case 33: { /* ClientMessage */
        uint32_t type, d0;
        memcpy(&type, e + 8, 4);
        memcpy(&d0, e + 12, 4);
        if (type == c->a_protocols && d0 == c->a_delete) {
            win->closed = true;
            sw_push(win, (SwEvent){ .type = SW_EV_CLOSE });
        }
        break;
    }
    default: break;
    }
}

static void x_pump(SwWindow *win, int timeout_ms) {
    X *c = win->impl;
    if (c->in_n < 32) {
        struct pollfd pf = { c->fd, POLLIN, 0 };
        if (poll(&pf, 1, timeout_ms) > 0) x_read_some(c);
    } else {
        x_read_some(c);
    }
    size_t off = 0;
    while (c->in_n - off >= 32) {
        const uint8_t *m = c->in + off;
        size_t size = 32;
        if (m[0] == 1) {
            uint32_t extra;
            uint16_t seq;
            memcpy(&extra, m + 4, 4);
            memcpy(&seq, m + 2, 2);
            size += (size_t)extra * 4;
            if (c->in_n - off < size) break;
            if (c->frame_pending && seq == c->sync_seq) { c->frame_pending = false; win->shown++; }
            if (c->pos_pending && seq == c->pos_seq) {
                /* TranslateCoordinates. Asking the server, rather than watching
                 * ConfigureNotify, gives the true position under any window manager. */
                int16_t px, py;
                memcpy(&px, m + 12, 2);
                memcpy(&py, m + 14, 2);
                win->sx = px;
                win->sy = py;
                win->has_pos = true;
                c->pos_pending = false;
            }
        } else {
            handle_event(win, c, m);
        }
        off += size;
    }
    if (off) {
        memmove(c->in, c->in + off, c->in_n - off);
        c->in_n -= off;
    }
    if (c->fatal && !win->closed) {
        win->closed = true;
        sw_push(win, (SwEvent){ .type = SW_EV_CLOSE });
    }
}

static void x_present(SwWindow *win) {
    X *c = win->impl;
    /* Measured from the start, so upload time counts toward the frame. */
    c->last_frame = sw_now();
    /* One PutImage per strip that fits the server's request size limit. */
    uint32_t stride = (uint32_t)win->w * 4;
    uint32_t per = (c->max_req_bytes - 24) / stride;
    if (per < 1) per = 1;
    for (int y = 0; y < win->h && !c->fatal; y += (int)per) {
        uint32_t rows = (uint32_t)win->h - (uint32_t)y < per ? (uint32_t)win->h - (uint32_t)y : per;
        uint32_t words = 6 + rows * (uint32_t)win->w;
        uint8_t h[24];
        h[0] = 72; h[1] = 2;
        uint16_t len = (uint16_t)words;
        memcpy(h + 2, &len, 2);
        memcpy(h + 4, &c->window, 4);
        memcpy(h + 8, &c->gc, 4);
        uint16_t uw = (uint16_t)win->w, uh = (uint16_t)rows;
        int16_t dx = 0, dy = (int16_t)y;
        memcpy(h + 12, &uw, 2);
        memcpy(h + 14, &uh, 2);
        memcpy(h + 16, &dx, 2);
        memcpy(h + 18, &dy, 2);
        h[20] = 0; h[21] = c->depth; h[22] = 0; h[23] = 0;
        c->seq++;
        x_write(c, h, 24);
        x_write(c, win->fb + (size_t)y * win->w, (size_t)rows * stride);
    }
    /* GetInputFocus: its reply tells us the server has caught up. */
    uint32_t sync = 43u | (1u << 16);
    x_req(c, &sync, 4);
    c->sync_seq = c->seq;
    c->frame_pending = true;
    if (!c->pos_pending) {
        uint32_t ask[4] = { 40u | (4u << 16), c->window, c->root, 0 };
        x_req(c, ask, 16);
        c->pos_seq = c->seq;
        c->pos_pending = true;
    }
}

/* ---- the overlay ---- */

/* Sets what shows of a window. No rects at all means nothing does. */
static void x_shape(X *c, uint8_t kind, const uint32_t *rects, uint32_t n) {
    uint32_t h[4];
    h[0] = (uint32_t)c->shape_major | (1u << 8) | ((4u + n * 2) << 16); /* ShapeRectangles */
    h[1] = 0u | ((uint32_t)kind << 8);                                   /* Set, unsorted */
    h[2] = c->overlay;
    h[3] = 0;
    c->seq++;
    x_write(c, h, 16);
    if (n) x_write(c, rects, (size_t)n * 8);
}

static bool x_outer_open(SwWindow *win) {
    X *c = win->impl;
    if (!c->shape_major) return false;
    x_read_workarea(win, c);
    win->outer_x = win->outer_y = 0;
    win->outer_w = c->screen_w;
    win->outer_h = c->screen_h;
    win->outer = malloc((size_t)win->outer_w * win->outer_h * 4);
    if (!win->outer) return false;
    c->overlay = x_new(c);
    /* An override-redirect window: the window manager leaves it alone. */
    uint8_t b[36];
    b[0] = 1; b[1] = c->depth;
    uint16_t len = 9;
    memcpy(b + 2, &len, 2);
    memcpy(b + 4, &c->overlay, 4);
    memcpy(b + 8, &c->root, 4);
    memset(b + 12, 0, 4);
    uint16_t uw = (uint16_t)c->screen_w, uh = (uint16_t)c->screen_h, border = 0, cls = 1;
    memcpy(b + 16, &uw, 2);
    memcpy(b + 18, &uh, 2);
    memcpy(b + 20, &border, 2);
    memcpy(b + 22, &cls, 2);
    memcpy(b + 24, &c->visual, 4);
    uint32_t mask = 0x200, redirect = 1;
    memcpy(b + 28, &mask, 4);
    memcpy(b + 32, &redirect, 4);
    x_req(c, b, 36);
    x_shape(c, 0, NULL, 0); /* bounding: nothing yet */
    x_shape(c, 2, NULL, 0); /* input: nothing, ever */
    return true;
}

/* Every run of visible pixels in the rect becomes a rectangle of the window's shape.
 * A row the same as the one above it just makes the rectangles above it taller. */
static uint32_t x_build_shape(X *c, const SwWindow *win, int rx, int ry, int rw, int rh) {
    uint32_t n = 0, prev_start = 0, prev_count = 0;
    uint32_t cap = (c->max_req_bytes - 16) / 8;
    for (int y = ry; y < ry + rh; y++) {
        const uint32_t *row = win->outer + (size_t)y * win->outer_w;
        uint32_t start = n;
        for (int x = rx; x < rx + rw; ) {
            if (row[x] == SW_OUTER_CLEAR) { x++; continue; }
            int x0 = x;
            while (x < rx + rw && row[x] != SW_OUTER_CLEAR) x++;
            if (n + 1 > cap) return n;
            if ((int)n + 1 > c->rects_cap) {
                c->rects_cap = c->rects_cap ? c->rects_cap * 2 : 4096;
                c->rects = realloc(c->rects, (size_t)c->rects_cap * 8);
            }
            uint16_t r[4] = { (uint16_t)x0, (uint16_t)y, (uint16_t)(x - x0), 1 };
            memcpy(&c->rects[n * 2], r, 8);
            n++;
        }
        uint32_t count = n - start;
        bool same = count && count == prev_count;
        for (uint32_t i = 0; i < count && same; i++) {
            uint16_t a[4], b[4];
            memcpy(a, &c->rects[(start + i) * 2], 8);
            memcpy(b, &c->rects[(prev_start + i) * 2], 8);
            same = a[0] == b[0] && a[2] == b[2];
        }
        if (same) {
            for (uint32_t i = 0; i < count; i++) {
                uint16_t b[4];
                memcpy(b, &c->rects[(prev_start + i) * 2], 8);
                b[3]++;
                memcpy(&c->rects[(prev_start + i) * 2], b, 8);
            }
            n = start;
        } else {
            prev_start = start;
            prev_count = count;
        }
    }
    return n;
}

static void x_outer_present(SwWindow *win, int x, int y, int w, int h) {
    X *c = win->impl;
    if (!c->overlay) return;
    if (w <= 0 || h <= 0) {
        if (c->overlay_mapped) {
            uint32_t unmap[2] = { 10u | (2u << 16), c->overlay };
            x_req(c, unmap, 8);
            c->overlay_mapped = false;
        }
        return;
    }
    x_shape(c, 0, c->rects, x_build_shape(c, win, x, y, w, h));
    if (!c->overlay_mapped) {
        uint32_t map[2] = { 8u | (2u << 16), c->overlay };
        x_req(c, map, 8);
        c->overlay_mapped = true;
    }
    /* Anything raised over it would hide what's fallen out, so it comes back up. */
    if (++c->overlay_frames % 30 == 1) {
        uint32_t raise[4] = { 12u | (4u << 16), c->overlay, 0x40, 0 };
        x_req(c, raise, 16);
    }
    /* Just the rect, in strips that fit the server's request size limit. */
    uint32_t stride = (uint32_t)w * 4;
    uint32_t per = (c->max_req_bytes - 24) / stride;
    if (per < 1) per = 1;
    for (int r = 0; r < h && !c->fatal; r += (int)per) {
        uint32_t rows = (uint32_t)(h - r) < per ? (uint32_t)(h - r) : per;
        uint8_t b[24];
        b[0] = 72; b[1] = 2;
        uint16_t len = (uint16_t)(6 + rows * (uint32_t)w);
        memcpy(b + 2, &len, 2);
        memcpy(b + 4, &c->overlay, 4);
        memcpy(b + 8, &c->gc, 4);
        uint16_t uw = (uint16_t)w, uh = (uint16_t)rows;
        int16_t dx = (int16_t)x, dy = (int16_t)(y + r);
        memcpy(b + 12, &uw, 2);
        memcpy(b + 14, &uh, 2);
        memcpy(b + 16, &dx, 2);
        memcpy(b + 18, &dy, 2);
        b[20] = 0; b[21] = c->depth; b[22] = 0; b[23] = 0;
        c->seq++;
        x_write(c, b, 24);
        for (uint32_t k = 0; k < rows; k++)
            x_write(c, win->outer + (size_t)(y + r + (int)k) * win->outer_w + x, stride);
    }
}

static bool x_ready(SwWindow *win) {
    X *c = win->impl;
    return !c->frame_pending && sw_now() - c->last_frame >= 1.0 / 60 - 0.002;
}

static void x_send_root(X *c, uint32_t type, const uint32_t data[5]) {
    uint8_t b[44] = { 25, 0 };
    uint16_t len = 11;
    memcpy(b + 2, &len, 2);
    memcpy(b + 4, &c->root, 4);
    uint32_t mask = 0x180000; /* substructure redirect | notify */
    memcpy(b + 8, &mask, 4);
    b[12] = 33; b[13] = 32;
    memcpy(b + 16, &c->window, 4);
    memcpy(b + 20, &type, 4);
    memcpy(b + 24, data, 20);
    x_req(c, b, 44);
}

static void x_moveresize(X *c, uint32_t direction) {
    /* The implicit button grab must end before the window manager can take over. */
    uint32_t ungrab[2] = { 27u | (2u << 16), 0 };
    x_req(c, ungrab, 8);
    uint32_t d[5] = { (uint32_t)c->root_x, (uint32_t)c->root_y, direction, (uint32_t)c->press_button, 1 };
    x_send_root(c, c->a_moveresize, d);
}

static void x_begin_move(SwWindow *win) { x_moveresize(win->impl, 8); }

static void x_begin_resize(SwWindow *win, int e) {
    uint32_t dir = 4;
    bool t = e & SW_EDGE_TOP, b = e & SW_EDGE_BOTTOM, l = e & SW_EDGE_LEFT, r = e & SW_EDGE_RIGHT;
    if (t && l) dir = 0;
    else if (t && r) dir = 2;
    else if (b && r) dir = 4;
    else if (b && l) dir = 6;
    else if (t) dir = 1;
    else if (r) dir = 3;
    else if (b) dir = 5;
    else if (l) dir = 7;
    x_moveresize(win->impl, dir);
}

static void x_minimize(SwWindow *win) {
    X *c = win->impl;
    uint32_t d[5] = { 3, 0, 0, 0, 0 }; /* IconicState */
    x_send_root(c, c->a_change_state, d);
}

const SwOps sw_x11_ops = { "x11", x_open, x_close, x_pump, x_present, x_ready,
                           x_begin_move, x_begin_resize, x_minimize, NULL,
                           x_outer_open, x_outer_present };
