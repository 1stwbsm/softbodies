/* SoftWin Wayland backend: the wire protocol by hand, no libwayland.
 *
 * Messages are 32-bit words: object id, then (size << 16 | opcode), then args.
 * File descriptors travel out of band as SCM_RIGHTS. Opcodes match wayland.xml
 * and xdg-shell.xml, viewporter.xml and fractional-scale-v1.xml.
 *
 * Client object ids are never reused. At one frame callback per frame the id
 * space lasts for years of continuous running.
 */
#define _GNU_SOURCE
#include "softwin.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define BUFFERS 3

typedef struct {
    uint32_t id;
    uint32_t *px;
    int w, h;
    bool busy;
} WlBuf;

typedef struct {
    int fd;
    uint8_t in[65536]; size_t in_n;
    uint8_t out[16384]; size_t out_n;
    int fds[16]; int nfds;

    uint32_t next_id;
    uint32_t registry, compositor, shm, wm_base, seat, viewporter, frac_mgr;
    uint32_t surface, xdg_surface, toplevel, viewport, frac;
    uint32_t pointer, keyboard;
    uint32_t frame_cb;
    uint32_t sync_cb; bool sync_done;
    uint32_t compositor_version;

    WlBuf bufs[BUFFERS];
    void *map; size_t map_size;   /* the shared memory behind all the buffers */

    int logical_w, logical_h;     /* surface size */
    int pending_w, pending_h;
    uint32_t pending_serial;
    bool configured, need_ack;
    uint32_t scale120;            /* fractional scale, in 120ths */
    int int_scale;                /* fallback integer scale */
    double px, py;                /* pointer, logical */
    uint32_t last_serial;         /* of the last button press */
    uint32_t enter_serial;
    bool fatal;
} Wl;

/* ---- writing ------------------------------------------------------------ */

static void wl_flush(Wl *c) {
    size_t off = 0;
    while (off < c->out_n) {
        ssize_t n = send(c->fd, c->out + off, c->out_n - off, MSG_NOSIGNAL);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) {
            struct pollfd p = { c->fd, POLLOUT, 0 };
            poll(&p, 1, 1000);
            continue;
        }
        c->fatal = true;
        break;
    }
    c->out_n = 0;
}

static void msg_put(Wl *c, const void *d, size_t n) {
    if (c->out_n + n > sizeof c->out) wl_flush(c);
    memcpy(c->out + c->out_n, d, n);
    c->out_n += n;
}

/* A small message builder: words and strings go in, the header goes on last. */
typedef struct { uint32_t w[64]; int n; } Msg;

static void m_u(Msg *m, uint32_t v) { m->w[m->n++] = v; }
static void m_s(Msg *m, const char *s) {
    uint32_t len = (uint32_t)strlen(s) + 1, words = (len + 3) / 4;
    m->w[m->n++] = len;
    memset(&m->w[m->n], 0, words * 4);
    memcpy(&m->w[m->n], s, len);
    m->n += (int)words;
}
static void msg_send(Wl *c, uint32_t obj, uint32_t op, const Msg *m) {
    uint32_t hdr[2] = { obj, ((uint32_t)(m->n * 4 + 8) << 16) | op };
    msg_put(c, hdr, 8);
    if (m->n) msg_put(c, m->w, (size_t)m->n * 4);
}
#define SEND(c, obj, op, ...) do { \
    Msg _m = { .n = 0 }; \
    uint32_t _a[] = { __VA_ARGS__ }; \
    for (size_t _i = 0; _i < sizeof _a / sizeof *_a; _i++) m_u(&_m, _a[_i]); \
    msg_send(c, obj, op, &_m); \
} while (0)
static void send0(Wl *c, uint32_t obj, uint32_t op) { Msg m = { .n = 0 }; msg_send(c, obj, op, &m); }

static uint32_t new_id(Wl *c) { return c->next_id++; }

static uint32_t bind_global(Wl *c, uint32_t name, const char *iface, uint32_t ver) {
    uint32_t id = new_id(c);
    Msg m = { .n = 0 };
    m_u(&m, name);
    m_s(&m, iface);
    m_u(&m, ver);
    m_u(&m, id);
    msg_send(c, c->registry, 0, &m);
    return id;
}

/* ---- buffers ------------------------------------------------------------- */

static int scale_px(const Wl *c, int logical) {
    if (c->viewport && c->scale120) return (logical * (int)c->scale120 + 60) / 120;
    return logical * c->int_scale;
}

static void destroy_buffers(Wl *c) {
    for (int i = 0; i < BUFFERS; i++) {
        WlBuf *b = &c->bufs[i];
        if (!b->id) continue;
        send0(c, b->id, 0);
        memset(b, 0, sizeof *b);
    }
    if (c->map) munmap(c->map, c->map_size);
    c->map = NULL;
}

static bool create_buffers(Wl *c, int w, int h) {
    destroy_buffers(c);
    size_t one = (size_t)w * h * 4, total = one * BUFFERS;
    int fd = memfd_create("softwin-fb", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)total) < 0) {
        if (fd >= 0) close(fd);
        return false;
    }
    uint8_t *map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); return false; }

    /* create_pool carries a descriptor, so it needs its own sendmsg. */
    wl_flush(c);
    uint32_t pool = new_id(c);
    uint32_t msg[4] = { c->shm, (16u << 16) | 0, pool, (uint32_t)total };
    char ctrl[CMSG_SPACE(sizeof(int))] = { 0 };
    struct iovec iov = { msg, sizeof msg };
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctrl, .msg_controllen = sizeof ctrl };
    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    if (sendmsg(c->fd, &mh, MSG_NOSIGNAL) < 0) c->fatal = true;
    close(fd);

    for (int i = 0; i < BUFFERS; i++) {
        WlBuf *b = &c->bufs[i];
        b->id = new_id(c);
        /* XRGB8888 is format 1 */
        SEND(c, pool, 0, b->id, (uint32_t)(one * i), (uint32_t)w, (uint32_t)h, (uint32_t)w * 4, 1);
        b->w = w; b->h = h; b->busy = false;
        b->px = (uint32_t *)(map + one * i);
    }
    send0(c, pool, 1); /* the pool may go; the buffers keep the memory */

    c->map = map;
    c->map_size = total;
    return true;
}

/* ---- incoming events ----------------------------------------------------- */

static void apply_size(SwWindow *win, Wl *c) {
    int w = scale_px(c, c->logical_w), h = scale_px(c, c->logical_h);
    win->scale = c->viewport && c->scale120 ? (float)c->scale120 / 120.0f : (float)c->int_scale;
    if (c->viewport)
        SEND(c, c->viewport, 2, (uint32_t)c->logical_w, (uint32_t)c->logical_h);
    else
        SEND(c, c->surface, 8, (uint32_t)c->int_scale);
    SEND(c, c->xdg_surface, 3, 0, 0, (uint32_t)c->logical_w, (uint32_t)c->logical_h);
    if (w != c->bufs[0].w || h != c->bufs[0].h) create_buffers(c, w, h);
    sw_resize_fb(win, w, h);
}

static void push_motion(SwWindow *win, Wl *c) {
    sw_push(win, (SwEvent){ .type = SW_EV_MOTION, .x = (int)(c->px * win->scale), .y = (int)(c->py * win->scale) });
}

static void on_global(Wl *c, uint32_t name, const char *iface, uint32_t ver) {
#define MINV(a, b) ((a) < (b) ? (a) : (b))
    if (!strcmp(iface, "wl_compositor")) {
        c->compositor_version = MINV(ver, 6);
        c->compositor = bind_global(c, name, iface, c->compositor_version);
    } else if (!strcmp(iface, "wl_shm")) c->shm = bind_global(c, name, iface, 1);
    else if (!strcmp(iface, "xdg_wm_base")) c->wm_base = bind_global(c, name, iface, 1);
    else if (!strcmp(iface, "wl_seat") && !c->seat) c->seat = bind_global(c, name, iface, MINV(ver, 5));
    else if (!strcmp(iface, "wp_viewporter")) c->viewporter = bind_global(c, name, iface, 1);
    else if (!strcmp(iface, "wp_fractional_scale_manager_v1")) c->frac_mgr = bind_global(c, name, iface, 1);
}

static void dispatch(SwWindow *win, Wl *c, uint32_t obj, uint32_t op, const uint32_t *a, uint32_t nbytes) {
    (void)nbytes;
    if (obj == 1) { /* wl_display */
        if (op == 0) {
            fprintf(stderr, "softwin: wayland protocol error on object %u, code %u: %s\n",
                    a[0], a[1], (const char *)&a[3]);
            c->fatal = true;
        }
        return;
    }
    if (obj == c->registry) {
        if (op == 0) on_global(c, a[0], (const char *)&a[2], a[1 + 1 + (a[1] + 3) / 4]);
        return;
    }
    if (obj == c->sync_cb) { c->sync_done = true; return; }
    if (obj == c->frame_cb && c->frame_cb) { c->frame_cb = 0; win->shown++; return; }
    if (obj == c->wm_base) { if (op == 0) { SEND(c, c->wm_base, 3, a[0]); } return; }
    if (obj == c->xdg_surface) {
        if (op == 0) {
            c->pending_serial = a[0];
            c->need_ack = true;
            c->configured = true;
        }
        return;
    }
    if (obj == c->toplevel) {
        if (op == 0) { c->pending_w = (int)a[0]; c->pending_h = (int)a[1]; }
        else if (op == 1) { win->closed = true; sw_push(win, (SwEvent){ .type = SW_EV_CLOSE }); }
        return;
    }
    if (obj == c->frac && c->frac) {
        if (op == 0 && a[0] != c->scale120) {
            c->scale120 = a[0];
            if (c->configured && !c->need_ack) apply_size(win, c);
        }
        return;
    }
    if (obj == c->surface) {
        if (op == 2 && !c->frac && (int)a[0] != c->int_scale && (int)a[0] > 0) {
            c->int_scale = (int)a[0];
            if (c->configured && !c->need_ack) apply_size(win, c);
        }
        return;
    }
    for (int i = 0; i < BUFFERS; i++)
        if (obj == c->bufs[i].id && c->bufs[i].id) { c->bufs[i].busy = false; return; }

    if (obj == c->seat) {
        if (op == 0) {
            if ((a[0] & 1) && !c->pointer) { c->pointer = new_id(c); SEND(c, c->seat, 0, c->pointer); }
            if ((a[0] & 2) && !c->keyboard) { c->keyboard = new_id(c); SEND(c, c->seat, 1, c->keyboard); }
        }
        return;
    }
    if (obj == c->pointer && c->pointer) {
        switch (op) {
        case 0: /* enter: hide the system cursor; SoftWin draws its own */
            c->enter_serial = a[0];
            SEND(c, c->pointer, 0, a[0], 0, 0, 0);
            c->px = (int32_t)a[2] / 256.0;
            c->py = (int32_t)a[3] / 256.0;
            push_motion(win, c);
            break;
        case 1:
            sw_push(win, (SwEvent){ .type = SW_EV_LEAVE });
            break;
        case 2:
            c->px = (int32_t)a[1] / 256.0;
            c->py = (int32_t)a[2] / 256.0;
            push_motion(win, c);
            break;
        case 3: {
            int b = a[2] == 0x110 ? 0 : a[2] == 0x111 ? 1 : a[2] == 0x112 ? 2 : -1;
            if (a[3] == 1) c->last_serial = a[0];
            if (b >= 0)
                sw_push(win, (SwEvent){ .type = a[3] ? SW_EV_BUTTON_DOWN : SW_EV_BUTTON_UP,
                                        .button = b, .x = (int)(c->px * win->scale), .y = (int)(c->py * win->scale) });
            break;
        }
        case 4: /* axis: 15 units per detent is the convention */
            if (a[1] == 0) {
                int v = (int32_t)a[2] / 256;
                sw_push(win, (SwEvent){ .type = SW_EV_WHEEL, .wheel = v > 0 ? 1 : v < 0 ? -1 : 0 });
            }
            break;
        default: break;
        }
        return;
    }
    if (obj == c->keyboard && c->keyboard) {
        if (op == 0) { /* keymap: the layout is not parsed, so drop the fd */
            if (c->nfds > 0) {
                close(c->fds[0]);
                memmove(c->fds, c->fds + 1, sizeof(int) * (size_t)--c->nfds);
            }
        } else if (op == 3) {
            sw_push(win, (SwEvent){ .type = a[3] ? SW_EV_KEY_DOWN : SW_EV_KEY_UP, .key = (int)a[2] });
        }
        return;
    }
}

static void read_events(SwWindow *win, Wl *c, int timeout_ms) {
    struct pollfd p = { c->fd, POLLIN, 0 };
    wl_flush(c);
    int r = poll(&p, 1, timeout_ms);
    if (r <= 0) return;
    if (p.revents & (POLLERR | POLLHUP)) c->fatal = true;

    char ctrl[CMSG_SPACE(sizeof(int) * 16)];
    struct iovec iov = { c->in + c->in_n, sizeof c->in - c->in_n };
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctrl, .msg_controllen = sizeof ctrl };
    ssize_t n = recvmsg(c->fd, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (n == 0) { c->fatal = true; return; }
    if (n < 0) {
        if (errno != EAGAIN && errno != EINTR) c->fatal = true;
        return;
    }
    c->in_n += (size_t)n;
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
        if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) continue;
        int count = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        int *fds = (int *)CMSG_DATA(cm);
        for (int i = 0; i < count; i++) {
            if (c->nfds < 16) c->fds[c->nfds++] = fds[i];
            else close(fds[i]);
        }
    }
    size_t off = 0;
    while (c->in_n - off >= 8) {
        uint32_t hdr[2];
        memcpy(hdr, c->in + off, 8);
        uint32_t size = hdr[1] >> 16, op = hdr[1] & 0xFFFF;
        if (size < 8 || size > sizeof c->in) { c->fatal = true; break; }
        if (c->in_n - off < size) break;
        uint32_t args[(size - 8) / 4 + 1];
        memcpy(args, c->in + off + 8, size - 8);
        args[(size - 8) / 4] = 0;
        dispatch(win, c, hdr[0], op, args, size - 8);
        off += size;
    }
    if (off) {
        memmove(c->in, c->in + off, c->in_n - off);
        c->in_n -= off;
    }
}

static void process_configure(SwWindow *win, Wl *c) {
    if (!c->need_ack) return;
    SEND(c, c->xdg_surface, 4, c->pending_serial);
    c->need_ack = false;
    if (c->pending_w > 0) c->logical_w = c->pending_w;
    if (c->pending_h > 0) c->logical_h = c->pending_h;
    apply_size(win, c);
    /* The ack must be followed by a commit with a buffer of the new size; the
     * main loop presents within a frame. Clear the pacing gate so it can. */
    c->frame_cb = 0;
}

static void roundtrip(SwWindow *win, Wl *c) {
    c->sync_cb = new_id(c);
    c->sync_done = false;
    SEND(c, 1, 0, c->sync_cb);
    while (!c->sync_done && !c->fatal) read_events(win, c, 1000);
    c->sync_cb = 0;
}

/* ---- ops ----------------------------------------------------------------- */

static bool wl_open_(SwWindow *win, const char *title, int w, int h) {
    const char *name = getenv("WAYLAND_DISPLAY");
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!name) return false;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (name[0] == '/') snprintf(addr.sun_path, sizeof addr.sun_path, "%s", name);
    else if (dir) snprintf(addr.sun_path, sizeof addr.sun_path, "%s/%s", dir, name);
    else return false;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { close(fd); return false; }

    Wl *c = calloc(1, sizeof *c);
    c->fd = fd;
    c->next_id = 2;
    c->int_scale = 1;
    win->impl = c;

    c->registry = new_id(c);
    SEND(c, 1, 1, c->registry);
    roundtrip(win, c);
    if (c->fatal || !c->compositor || !c->shm || !c->wm_base) {
        fprintf(stderr, "softwin: compositor lacks wl_compositor, wl_shm or xdg_wm_base\n");
        close(fd);
        free(c);
        win->impl = NULL;
        return false;
    }
    roundtrip(win, c); /* seat capabilities */

    c->surface = new_id(c);
    SEND(c, c->compositor, 0, c->surface);
    if (c->viewporter && c->frac_mgr) {
        c->viewport = new_id(c);
        SEND(c, c->viewporter, 1, c->viewport, c->surface);
        c->frac = new_id(c);
        SEND(c, c->frac_mgr, 1, c->frac, c->surface);
    }
    c->xdg_surface = new_id(c);
    SEND(c, c->wm_base, 2, c->xdg_surface, c->surface);
    c->toplevel = new_id(c);
    SEND(c, c->xdg_surface, 1, c->toplevel);
    Msg m = { .n = 0 };
    m_s(&m, title);
    msg_send(c, c->toplevel, 2, &m);
    Msg m2 = { .n = 0 };
    m_s(&m2, "softbodies");
    msg_send(c, c->toplevel, 3, &m2);
    SEND(c, c->toplevel, 8, 480, 360); /* min size */
    c->logical_w = w;
    c->logical_h = h;
    send0(c, c->surface, 6);
    wl_flush(c);

    while (!c->need_ack && !c->fatal && !win->closed) read_events(win, c, 1000);
    if (c->fatal) { close(fd); free(c); win->impl = NULL; return false; }
    roundtrip(win, c); /* collect a preferred scale before the first buffer */
    process_configure(win, c);
    return true;
}

static void wl_close_(SwWindow *win) {
    Wl *c = win->impl;
    if (!c) return;
    wl_flush(c);
    close(c->fd);
    free(c);
    win->impl = NULL;
}

static void wl_pump(SwWindow *win, int timeout_ms) {
    Wl *c = win->impl;
    read_events(win, c, timeout_ms);
    process_configure(win, c);
    wl_flush(c);
    if (c->fatal && !win->closed) {
        win->closed = true;
        sw_push(win, (SwEvent){ .type = SW_EV_CLOSE });
    }
}

static void wl_present(SwWindow *win) {
    Wl *c = win->impl;
    if (!c->configured || c->fatal) return;
    WlBuf *b = NULL;
    for (int i = 0; i < BUFFERS; i++)
        if (!c->bufs[i].busy && c->bufs[i].w == win->w && c->bufs[i].h == win->h) { b = &c->bufs[i]; break; }
    if (!b) return; /* all with the compositor: skip this frame */
    memcpy(b->px, win->fb, (size_t)win->w * win->h * 4);
    b->busy = true;
    SEND(c, c->surface, 1, b->id, 0, 0);
    if (c->compositor_version >= 4) {
        SEND(c, c->surface, 9, 0, 0, (uint32_t)win->w, (uint32_t)win->h);
    } else {
        SEND(c, c->surface, 2, 0, 0, (uint32_t)c->logical_w, (uint32_t)c->logical_h);
    }
    c->frame_cb = new_id(c);
    SEND(c, c->surface, 3, c->frame_cb);
    send0(c, c->surface, 6);
    wl_flush(c);
}

static bool wl_ready(SwWindow *win) { return ((Wl *)win->impl)->frame_cb == 0; }

static void wl_begin_move(SwWindow *win) {
    Wl *c = win->impl;
    if (c->seat) SEND(c, c->toplevel, 5, c->seat, c->last_serial);
    wl_flush(c);
}

static void wl_begin_resize(SwWindow *win, int edges) {
    Wl *c = win->impl;
    /* SwEdge uses the xdg_toplevel.resize_edge bit values. */
    if (c->seat) SEND(c, c->toplevel, 6, c->seat, c->last_serial, (uint32_t)edges);
    wl_flush(c);
}

static void wl_minimize(SwWindow *win) {
    Wl *c = win->impl;
    send0(c, c->toplevel, 13);
    wl_flush(c);
}

const SwOps sw_wayland_ops = { "wayland", wl_open_, wl_close_, wl_pump, wl_present, wl_ready,
                               wl_begin_move, wl_begin_resize, wl_minimize, NULL, NULL, NULL };
