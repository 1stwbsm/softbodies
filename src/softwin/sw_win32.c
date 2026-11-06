/* SoftWin Windows backend: Win32 and GDI directly.
 *
 * The window is a borderless WS_POPUP, so all of its chrome is SoftWin's. It keeps
 * WS_THICKFRAME only so the system's move and resize loops will run on it.
 *
 * Those loops are modal: DefWindowProc doesn't return until the drag ends. A timer
 * keeps the app ticking meanwhile, which is what lets a resize squeeze the matter
 * live. Because a loop must never start inside a tick, begin_move and begin_resize
 * only leave a note, and the loop starts between frames in frame_ready.
 */
#define WIN32_LEAN_AND_MEAN
#include "softwin.h"

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <string.h>

#define MIN_W 480
#define MIN_H 360
#define MODAL_TIMER 1

typedef struct {
    HWND hwnd;
    double last_present;
    bool vsync_works;
    bool in_modal;
    bool tracking;
    int buttons;           /* held mouse buttons, for capture */
    WPARAM pending_hit;    /* hit-test code of a deferred move or resize, 0 = none */
    HWND overlay;
    bool overlay_shown;
} Win;

/* Window messages carry no context of ours, and there is only ever one window. */
static SwWindow *g_win;

static Win *impl(void) { return g_win ? g_win->impl : NULL; }

static void blit(SwWindow *win, HDC dc) {
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biWidth = win->w;
    bmi.bmiHeader.biHeight = -win->h; /* negative: top-down, as SoftWin draws */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(dc, 0, 0, (DWORD)win->w, (DWORD)win->h, 0, 0, 0, (UINT)win->h, win->fb, &bmi, DIB_RGB_COLORS);
}

static UINT window_dpi(HWND hwnd) {
    typedef UINT (WINAPI *GetDpiFn)(HWND);
    GetDpiFn fn = (GetDpiFn)(void *)GetProcAddress(GetModuleHandleA("user32.dll"), "GetDpiForWindow");
    if (fn) return fn(hwnd);
    HDC dc = GetDC(hwnd);
    UINT dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(hwnd, dc);
    return dpi ? dpi : 96;
}

/* Sharp pixels on scaled displays: without this, Windows would blur the window. */
static void declare_dpi_aware(void) {
    typedef BOOL (WINAPI *SetCtxFn)(HANDLE);
    typedef BOOL (WINAPI *SetAwareFn)(void);
    HMODULE user32 = GetModuleHandleA("user32.dll");
    SetCtxFn ctx = (SetCtxFn)(void *)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (ctx && ctx((HANDLE)(intptr_t)-4)) return; /* per monitor, version 2 */
    SetAwareFn aware = (SetAwareFn)(void *)GetProcAddress(user32, "SetProcessDPIAware");
    if (aware) aware();
}

/* The monitor the window is on, minus the taskbar. */
static void read_work_area(SwWindow *win, HWND hwnd) {
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;
    win->desk_x0 = mi.rcWork.left;
    win->desk_y0 = mi.rcWork.top;
    win->desk_x1 = mi.rcWork.right;
    win->desk_y1 = mi.rcWork.bottom;
    win->has_desk = true;
}

static void button_event(SwWindow *win, Win *c, HWND hwnd, int button, bool down, LPARAM lp) {
    if (down) {
        if (!c->buttons) SetCapture(hwnd);
        c->buttons |= 1 << button;
    } else {
        c->buttons &= ~(1 << button);
        if (!c->buttons) ReleaseCapture();
    }
    sw_push(win, (SwEvent){ .type = down ? SW_EV_BUTTON_DOWN : SW_EV_BUTTON_UP, .button = button,
                            .x = GET_X_LPARAM(lp), .y = GET_Y_LPARAM(lp) });
}

/* Set 1 scan codes match the evdev codes SoftWin uses, across the main block. */
static int key_code(LPARAM lp) {
    int scan = (int)((lp >> 16) & 0xFF);
    if (lp & (1 << 24)) {
        switch (scan) {
        case SW_KEY_LCTRL: return SW_KEY_RCTRL;
        case SW_KEY_ENTER: return SW_KEY_ENTER;
        case 0x48: return SW_KEY_UP;
        case 0x50: return SW_KEY_DOWN;
        case 0x4B: return SW_KEY_LEFT;
        case 0x4D: return SW_KEY_RIGHT;
        case 0x47: return SW_KEY_HOME;
        case 0x4F: return SW_KEY_END;
        case 0x53: return SW_KEY_DELETE;
        default: return 0; /* others would collide with the main block */
        }
    }
    return scan;
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SwWindow *win = g_win;
    Win *c = impl();
    if (!win || !c) return DefWindowProcA(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_NCCALCSIZE:
        return 0; /* the client area is the whole window: no system frame */
    case WM_NCHITTEST:
        return HTCLIENT;  /* SoftWin decides what its edges mean */
    case WM_GETMINMAXINFO: {
        MINMAXINFO *m = (MINMAXINFO *)lp;
        m->ptMinTrackSize.x = (LONG)((float)MIN_W * win->scale);
        m->ptMinTrackSize.y = (LONG)((float)MIN_H * win->scale);
        return 0;
    }
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED && LOWORD(lp) && HIWORD(lp)) sw_resize_fb(win, LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_MOVE:
        /* Minimized windows are parked far off screen, and that isn't a throw. */
        if (!IsIconic(hwnd)) {
            win->sx = GET_X_LPARAM(lp);
            win->sy = GET_Y_LPARAM(lp);
            win->has_pos = true;
            read_work_area(win, hwnd);
        }
        return 0;
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        win->scale = (float)HIWORD(wp) / 96.0f;
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        sw_push(win, (SwEvent){ .type = SW_EV_RESIZE, .x = win->w, .y = win->h });
        return 0;
    }
    case WM_ENTERSIZEMOVE:
        c->in_modal = true;
        SetTimer(hwnd, MODAL_TIMER, 15, NULL);
        return 0;
    case WM_EXITSIZEMOVE:
        c->in_modal = false;
        KillTimer(hwnd, MODAL_TIMER);
        return 0;
    case WM_TIMER:
        if (wp == MODAL_TIMER && c->in_modal && win->modal_tick) win->modal_tick(win->modal_user);
        return 0;
    case WM_CLOSE:
        win->closed = true;
        sw_push(win, (SwEvent){ .type = SW_EV_CLOSE });
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (win->fb) blit(win, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(NULL); return TRUE; } /* SoftWin draws its own */
        break;
    case WM_MOUSEMOVE:
        if (!c->tracking) {
            TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&t);
            c->tracking = true;
        }
        sw_push(win, (SwEvent){ .type = SW_EV_MOTION, .x = GET_X_LPARAM(lp), .y = GET_Y_LPARAM(lp) });
        return 0;
    case WM_MOUSELEAVE:
        c->tracking = false;
        sw_push(win, (SwEvent){ .type = SW_EV_LEAVE });
        return 0;
    case WM_LBUTTONDOWN: button_event(win, c, hwnd, 0, true, lp); return 0;
    case WM_LBUTTONUP:   button_event(win, c, hwnd, 0, false, lp); return 0;
    case WM_RBUTTONDOWN: button_event(win, c, hwnd, 1, true, lp); return 0;
    case WM_RBUTTONUP:   button_event(win, c, hwnd, 1, false, lp); return 0;
    case WM_MBUTTONDOWN: button_event(win, c, hwnd, 2, true, lp); return 0;
    case WM_MBUTTONUP:   button_event(win, c, hwnd, 2, false, lp); return 0;
    case WM_CAPTURECHANGED:
        c->buttons = 0;
        return 0;
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        sw_push(win, (SwEvent){ .type = SW_EV_WHEEL, .wheel = delta > 0 ? -1 : delta < 0 ? 1 : 0 });
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (!(lp & (1 << 30))) { /* the other platforms don't report auto-repeat */
            int key = key_code(lp);
            if (key) sw_push(win, (SwEvent){ .type = SW_EV_KEY_DOWN, .key = key });
        }
        if (msg == WM_SYSKEYDOWN) break; /* leaves Alt+F4 working */
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        int key = key_code(lp);
        if (key) sw_push(win, (SwEvent){ .type = SW_EV_KEY_UP, .key = key });
        if (msg == WM_SYSKEYUP) break;
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static bool w32_open(SwWindow *win, const char *title, int w, int h) {
    declare_dpi_aware();
    timeBeginPeriod(1); /* the default timer tick is far too coarse to pace frames */

    HINSTANCE inst = GetModuleHandleA(NULL);
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = "SoftWin";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (!RegisterClassA(&wc)) return false;

    Win *c = calloc(1, sizeof *c);
    win->impl = c;
    g_win = win;
    win->scale = 1;

    DWORD style = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_SYSMENU;
    c->hwnd = CreateWindowExA(WS_EX_APPWINDOW, "SoftWin", title, style, CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                              NULL, NULL, inst, NULL);
    if (!c->hwnd) {
        g_win = NULL;
        win->impl = NULL;
        free(c);
        return false;
    }

    /* Size in real pixels for this monitor, centred in its work area. */
    win->scale = (float)window_dpi(c->hwnd) / 96.0f;
    int pw = (int)((float)w * win->scale), ph = (int)((float)h * win->scale);
    RECT area = { 0, 0, pw, ph };
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &area, 0);
    SetWindowPos(c->hwnd, NULL, area.left + (area.right - area.left - pw) / 2,
                 area.top + (area.bottom - area.top - ph) / 2, pw, ph,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    sw_resize_fb(win, pw, ph);
    ShowWindow(c->hwnd, SW_SHOWNORMAL);
    POINT origin = { 0, 0 };
    ClientToScreen(c->hwnd, &origin);
    win->sx = origin.x;
    win->sy = origin.y;
    win->has_pos = true;
    read_work_area(win, c->hwnd);
    return true;
}

static void w32_close(SwWindow *win) {
    Win *c = win->impl;
    if (!c) return;
    g_win = NULL;
    if (c->overlay) DestroyWindow(c->overlay);
    free(win->outer);
    win->outer = NULL;
    DestroyWindow(c->hwnd);
    timeEndPeriod(1);
    free(c);
    win->impl = NULL;
}

static void w32_pump(SwWindow *win, int timeout_ms) {
    Win *c = win->impl;
    if (c->in_modal) return; /* the system's own loop is dispatching for us */
    if (timeout_ms > 0) MsgWaitForMultipleObjects(0, NULL, FALSE, (DWORD)timeout_ms, QS_ALLINPUT);
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            win->closed = true;
            sw_push(win, (SwEvent){ .type = SW_EV_CLOSE });
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static void w32_present(SwWindow *win) {
    Win *c = win->impl;
    HDC dc = GetDC(c->hwnd);
    blit(win, dc);
    ReleaseDC(c->hwnd, dc);
    win->shown++;
    /* DwmFlush waits for the compositor's next frame. Where it fails or returns at
     * once, such as under Wine, frame_ready falls back to a 60 Hz timer. */
    double t0 = sw_now();
    c->vsync_works = !c->in_modal && SUCCEEDED(DwmFlush()) && sw_now() - t0 > 0.001;
    c->last_present = t0;
}

static bool w32_ready(SwWindow *win) {
    Win *c = win->impl;
    if (c->pending_hit) {
        /* Between frames: the safe place to enter the system's modal loop. */
        WPARAM hit = c->pending_hit;
        c->pending_hit = 0;
        POINT p;
        GetCursorPos(&p);
        ReleaseCapture();
        SendMessageA(c->hwnd, WM_NCLBUTTONDOWN, hit, MAKELPARAM(p.x, p.y));
    }
    return c->vsync_works || sw_now() - c->last_present >= 1.0 / 60 - 0.001;
}

static void w32_begin_move(SwWindow *win) { ((Win *)win->impl)->pending_hit = HTCAPTION; }

static void w32_begin_resize(SwWindow *win, int e) {
    bool t = e & SW_EDGE_TOP, b = e & SW_EDGE_BOTTOM, l = e & SW_EDGE_LEFT, r = e & SW_EDGE_RIGHT;
    WPARAM hit = HTBOTTOMRIGHT;
    if (t && l) hit = HTTOPLEFT;
    else if (t && r) hit = HTTOPRIGHT;
    else if (b && l) hit = HTBOTTOMLEFT;
    else if (b && r) hit = HTBOTTOMRIGHT;
    else if (t) hit = HTTOP;
    else if (b) hit = HTBOTTOM;
    else if (l) hit = HTLEFT;
    else if (r) hit = HTRIGHT;
    ((Win *)win->impl)->pending_hit = hit;
}

/* The overlay: always on top, never the focus, invisible to the mouse, and with one
 * colour that the system treats as not there. */
static LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static bool w32_outer_open(SwWindow *win) {
    Win *c = win->impl;
    HINSTANCE inst = GetModuleHandleA(NULL);
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = overlay_proc;
    wc.hInstance = inst;
    wc.lpszClassName = "SoftWinOverlay";
    RegisterClassA(&wc);
    win->outer_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    win->outer_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    win->outer_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    win->outer_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    win->outer = malloc((size_t)win->outer_w * win->outer_h * 4);
    if (!win->outer) return false;
    c->overlay = CreateWindowExA(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                 "SoftWinOverlay", "", WS_POPUP, win->outer_x, win->outer_y, win->outer_w, win->outer_h,
                                 NULL, NULL, inst, NULL);
    if (!c->overlay) { free(win->outer); win->outer = NULL; return false; }
    SetLayeredWindowAttributes(c->overlay, RGB(SW_OUTER_CLEAR >> 16 & 0xFF, SW_OUTER_CLEAR >> 8 & 0xFF, SW_OUTER_CLEAR & 0xFF),
                               0, LWA_COLORKEY);
    return true;
}

static void w32_outer_present(SwWindow *win, int x, int y, int w, int h) {
    Win *c = win->impl;
    if (!c->overlay) return;
    if (w <= 0 || h <= 0) {
        if (c->overlay_shown) ShowWindow(c->overlay, SW_HIDE);
        c->overlay_shown = false;
        return;
    }
    /* A bitmap of just those rows, so that its rows can't be counted from the wrong end. */
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biWidth = win->outer_w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(c->overlay);
    SetDIBitsToDevice(dc, x, y, (DWORD)w, (DWORD)h, x, 0, 0, (UINT)h, win->outer + (size_t)y * win->outer_w, &bmi, DIB_RGB_COLORS);
    ReleaseDC(c->overlay, dc);
    if (!c->overlay_shown) {
        ShowWindow(c->overlay, SW_SHOWNOACTIVATE);
        c->overlay_shown = true;
    }
}

static void w32_minimize(SwWindow *win) { ShowWindow(((Win *)win->impl)->hwnd, SW_MINIMIZE); }

const SwOps sw_win32_ops = { "win32", w32_open, w32_close, w32_pump, w32_present, w32_ready,
                             w32_begin_move, w32_begin_resize, w32_minimize, NULL,
                             w32_outer_open, w32_outer_present };
