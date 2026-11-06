/* Drives softbodies.exe the way a person would, and checks what ends up on screen.
 *
 * Input goes through SendInput, and pixels are read back from the window through
 * GDI. It works under Wine and on real Windows.
 *
 * usage: win_drive softbodies.exe [screenshot.png [log.txt]]
 */
#define WIN32_LEAN_AND_MEAN
#include "../src/softwin/softwin.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static HWND hwnd;

#define CHECK(ok, ...) do { \
    int ok_ = (ok); \
    printf("  %s ", ok_ ? "ok  " : "FAIL"); printf(__VA_ARGS__); printf("\n"); fflush(stdout); \
    if (!ok_) failures++; \
} while (0)

typedef struct { uint32_t *px; int w, h; } Shot;

static Shot capture(void) {
    RECT r;
    GetClientRect(hwnd, &r);
    Shot s = { NULL, r.right, r.bottom };
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader;
    bmi.bmiHeader.biWidth = s.w;
    bmi.bmiHeader.biHeight = -s.h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(hwnd), mem = CreateCompatibleDC(screen);
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, s.w, s.h, screen, 0, 0, SRCCOPY);
    GdiFlush();
    s.px = malloc((size_t)s.w * s.h * 4);
    CopyMemory(s.px, bits, (size_t)s.w * s.h * 4);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, screen);
    return s;
}

static int non_bw(const Shot *s, uint32_t *example) {
    int bad = 0;
    for (int i = 0; i < s->w * s->h; i++) {
        uint32_t v = s->px[i] & 0xFFFFFF;
        if (v != 0 && v != 0xFFFFFF) { if (!bad) *example = v; bad++; }
    }
    return bad;
}

static int count_white(const Shot *s) {
    int n = 0;
    for (int i = 0; i < s->w * s->h; i++) n += (s->px[i] & 0xFFFFFF) == 0xFFFFFF;
    return n;
}

static bool white_at(const Shot *s, int x, int y) { return (s->px[y * s->w + x] & 0xFFFFFF) == 0xFFFFFF; }

static void mouse_to(int cx, int cy) {
    POINT p = { cx, cy };
    ClientToScreen(hwnd, &p);
    SetCursorPos(p.x, p.y);
    INPUT in;
    ZeroMemory(&in, sizeof in);
    in.type = INPUT_MOUSE;
    /* A zero-distance move, so the window is told where SetCursorPos put it. */
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof in);
    Sleep(40);
}

static void button(DWORD flag) {
    INPUT in;
    ZeroMemory(&in, sizeof in);
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flag;
    SendInput(1, &in, sizeof in);
    Sleep(60);
}

static void key(int scan, bool down) {
    INPUT in;
    ZeroMemory(&in, sizeof in);
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = (WORD)scan;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof in);
    Sleep(50);
}

static void tap(int scan) { key(scan, true); key(scan, false); }

int main(int argc, char **argv) {
    if (argc < 2) { puts("usage: win_drive softbodies.exe [screenshot.png [log.txt]]"); return 2; }
    if (argc > 3 && !freopen(argv[3], "w", stdout)) return 2;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    char cmd[MAX_PATH + 32];
    snprintf(cmd, sizeof cmd, "\"%s\"", argv[1]);
    for (char *q = cmd; *q; q++) if (*q == '/') *q = '\\'; /* CreateProcess is picky */
    strncat(cmd, " --toy softbodies", sizeof cmd - strlen(cmd) - 1);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("could not start %s: error %lu\n", argv[1], GetLastError());
        return 2;
    }
    for (int i = 0; i < 200 && !hwnd; i++) { hwnd = FindWindowA("SoftWin", NULL); Sleep(100); }
    if (!hwnd) { puts("  FAIL window never appeared"); TerminateProcess(pi.hProcess, 1); return 1; }
    Sleep(1500);
    SetForegroundWindow(hwnd);

    RECT r0, c0;
    GetWindowRect(hwnd, &r0);
    GetClientRect(hwnd, &c0);
    int W = c0.right, H = c0.bottom;
    CHECK(r0.right - r0.left == W && r0.bottom - r0.top == H,
          "no system frame: window %ldx%ld, client %dx%d", r0.right - r0.left, r0.bottom - r0.top, W, H);
    /* Layout scales from a dot of 2 at 100%, and these positions assume that. */
    CHECK(W == 1200 && H == 800, "opens at the requested size: %dx%d", W, H);

    uint32_t bad_px = 0;
    Shot s = capture();
    int bad = non_bw(&s, &bad_px);
    CHECK(bad == 0, "first frame is pure black and white, %d pixels were not, e.g. %06x", bad, bad_px);
    CHECK(!white_at(&s, 60, 206), "NUKE button starts unlit");
    free(s.px);

    mouse_to(600, 450);
    button(MOUSEEVENTF_LEFTDOWN);
    button(MOUSEEVENTF_LEFTUP);
    tap(SW_KEY_K);
    Sleep(200);
    s = capture();
    CHECK(white_at(&s, 60, 206), "key K reaches the app: NUKE button lit");
    int before = count_white(&s);
    free(s.px);

    button(MOUSEEVENTF_LEFTDOWN);
    button(MOUSEEVENTF_LEFTUP);
    Sleep(620); /* the warhead falls, and the flash peaks */
    s = capture();
    int during = count_white(&s);
    CHECK(during > before * 3 / 2, "a click fires the nuke: white pixels %d -> %d", before, during);
    bad = non_bw(&s, &bad_px);
    CHECK(bad == 0, "blast frame is pure black and white, %d pixels were not, e.g. %06x", bad, bad_px);
    free(s.px);

    tap(SW_KEY_G);
    tap(SW_KEY_R);
    Sleep(150);

    /* Title drag: the window follows, and physics must keep running meanwhile. */
    RECT r1;
    GetWindowRect(hwnd, &r1);
    mouse_to(300, 12);
    button(MOUSEEVENTF_LEFTDOWN);
    Sleep(150);
    Shot a = capture();
    POINT p;
    GetCursorPos(&p);
    for (int i = 1; i <= 10; i++) { SetCursorPos(p.x + i * 8, p.y + i * 5); button(MOUSEEVENTF_MOVE); Sleep(25); }
    Sleep(300);
    Shot b = capture();
    bool same = a.w == b.w && a.h == b.h && !memcmp(a.px, b.px, (size_t)a.w * a.h * 4);
    CHECK(!same, "physics keeps running during a title drag");
    free(a.px);
    free(b.px);
    button(MOUSEEVENTF_LEFTUP);
    Sleep(300);
    RECT r2;
    GetWindowRect(hwnd, &r2);
    CHECK(r2.left - r1.left == 80 && r2.top - r1.top == 50,
          "title drag moves the window by (80,50), got (%ld,%ld)", r2.left - r1.left, r2.top - r1.top);

    /* Corner drag: sizes the window, and the framebuffer follows. */
    mouse_to(W - 3, H - 3);
    button(MOUSEEVENTF_LEFTDOWN);
    Sleep(120);
    GetCursorPos(&p);
    for (int i = 1; i <= 10; i++) { SetCursorPos(p.x - i * 20, p.y - i * 15); button(MOUSEEVENTF_MOVE); Sleep(25); }
    Sleep(200);
    button(MOUSEEVENTF_LEFTUP);
    Sleep(500);
    RECT c1;
    GetClientRect(hwnd, &c1);
    CHECK(c1.right == W - 200 && c1.bottom == H - 150, "corner drag resizes to %dx%d, got %ldx%ld",
          W - 200, H - 150, c1.right, c1.bottom);
    s = capture();
    bad = non_bw(&s, &bad_px);
    CHECK(bad == 0, "resized window is pure black and white, %d pixels were not", bad);
    CHECK(white_at(&s, s.w - 60, 3), "the title bar reaches the new right edge");
    CHECK(s.h > 40 && white_at(&s, s.w / 2, s.h - 1), "the window outline reaches the new bottom edge");
    if (argc > 2 && !sw_write_png(argv[2], s.px, s.w, s.h)) printf("  could not save %s\n", argv[2]);
    free(s.px);

    /* Minimize button at the right of the title bar. */
    int dot = 2, title = dot * 12, bs = title - dot * 3, f = 1;
    int bx = c1.right - f - dot * 2 - bs, by = f + (title - bs) / 2;
    mouse_to(bx - bs - dot * 2 + bs / 2, by + bs / 2);
    button(MOUSEEVENTF_LEFTDOWN);
    button(MOUSEEVENTF_LEFTUP);
    Sleep(300);
    CHECK(IsIconic(hwnd), "minimize button minimizes the window");
    ShowWindow(hwnd, SW_RESTORE);
    Sleep(300);
    SetForegroundWindow(hwnd);
    Sleep(100);

    key(SW_KEY_LCTRL, true);
    tap(SW_KEY_Q);
    key(SW_KEY_LCTRL, false);
    DWORD wait = WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CHECK(wait == WAIT_OBJECT_0 && code == 0, "Ctrl+Q exits cleanly, wait %lu, code %lu", wait, code);

    printf("win_drive: %d failed\n", failures);
    return failures ? 1 : 0;
}
