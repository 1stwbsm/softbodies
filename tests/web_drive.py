#!/usr/bin/env python3
"""Drives dist/softbodies.html in real headless Firefox, with real input events.

It talks to Firefox over Marionette, using a throwaway profile, so an open browser
and its settings aren't touched.

usage: web_drive.py dist/softbodies.html [screenshot-prefix]
"""
import base64
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

PORT = 28417
failures = []


def check(ok, what):
    print(("  ok   " if ok else "  FAIL ") + what, flush=True)
    if not ok:
        failures.append(what)


class Browser:
    def __init__(self):
        self.sock = socket.create_connection(("127.0.0.1", PORT), timeout=30)
        self.buf = b""
        self.msg_id = 0
        self._read()

    def _read(self):
        while b":" not in self.buf:
            self.buf += self.sock.recv(65536)
        head, rest = self.buf.split(b":", 1)
        size = int(head)
        while len(rest) < size:
            rest += self.sock.recv(1 << 20)
        self.buf = rest[size:]
        return json.loads(rest[:size])

    def call(self, name, params=None):
        self.msg_id += 1
        data = json.dumps([0, self.msg_id, name, params or {}]).encode()
        self.sock.sendall(str(len(data)).encode() + b":" + data)
        _, _, err, result = self._read()
        if err:
            raise RuntimeError(f"{name}: {err.get('message', err)}")
        return result

    def js(self, script):
        return self.call("WebDriver:ExecuteScript", {"script": script, "args": []})["value"]

    def state(self):
        return json.loads(self.js("return window.softdesk.state();"))

    def pixels(self, i=0):
        return json.loads(self.js(f"return window.softdesk.pixels({i});"))

    def toy_mode(self, i=0):
        return self.js(f"return window.softdesk.toyMode({i});")

    def type_text(self, text):
        for ch in text:
            self.key(ch)

    def is_white(self, x, y, i=0):
        return self.js(f"return window.softdesk.isWhite({i},{x},{y});")

    def pointer(self, *steps):
        self.call("WebDriver:PerformActions", {"actions": [{
            "type": "pointer", "id": "mouse", "parameters": {"pointerType": "mouse"}, "actions": list(steps)}]})
        self.call("WebDriver:ReleaseActions")

    def click(self, x, y, button=0):
        self.pointer(move(x, y), {"type": "pointerDown", "button": button}, {"type": "pause", "duration": 40},
                     {"type": "pointerUp", "button": button})

    def double_click(self, x, y):
        self.pointer(move(x, y), {"type": "pointerDown", "button": 0}, {"type": "pointerUp", "button": 0},
                     {"type": "pause", "duration": 60},
                     {"type": "pointerDown", "button": 0}, {"type": "pointerUp", "button": 0})

    def drag(self, x0, y0, x1, y1):
        n = 10
        path = [move(x0 + (x1 - x0) * i // n, y0 + (y1 - y0) * i // n, 30) for i in range(1, n + 1)]
        self.pointer(move(x0, y0), {"type": "pointerDown", "button": 0}, {"type": "pause", "duration": 120},
                     *path, {"type": "pause", "duration": 120}, {"type": "pointerUp", "button": 0})

    def key(self, value):
        self.call("WebDriver:PerformActions", {"actions": [{
            "type": "key", "id": "kb", "actions": [{"type": "keyDown", "value": value}, {"type": "keyUp", "value": value}]}]})
        self.call("WebDriver:ReleaseActions")

    def shot(self, path):
        data = self.call("WebDriver:TakeScreenshot", {"full": False})["value"]
        with open(path, "wb") as f:
            f.write(base64.b64decode(data))


def move(x, y, duration=0):
    return {"type": "pointerMove", "x": int(x), "y": int(y), "duration": duration, "origin": "viewport"}


def wait_for(fn, seconds=8):
    end = time.time() + seconds
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.05)
    return fn()


def worlds(state):
    return [w for w in state["windows"] if w["kind"] == "softbodies"]


def scenario(b, page, prefix):
    b.call("WebDriver:Navigate", {"url": "file://" + os.path.abspath(page)})
    b.js("window.__errors = []; window.addEventListener('error', "
         "e => window.__errors.push(e.message)); return 1;")

    wait_for(lambda: worlds(b.state()) and not b.state()["screen"])
    s = b.state()
    check(len(worlds(s)) == 1, "boots to a desktop with one world open")
    check(s["active"] == "softbodies", "the world has the focus")
    check(len(s["icons"]) == 9, f"the desktop has its icons, {len(s['icons'])}")
    check(b.toy_mode() == 0, f"the world opens on the toy selector, mode {b.toy_mode()}")
    time.sleep(1.2)
    if prefix:
        b.shot(prefix + "-selector.png")
    # The selector, with the keyboard and then the TOY drop-down.
    b.key("1")
    time.sleep(0.15)
    check(b.toy_mode() == 1, f"key 1 opens SoftPhys, mode {b.toy_mode()}")
    w = worlds(b.state())[0]
    b.click(w["x"] + 24, w["y"] + 36)
    time.sleep(0.1)
    b.click(w["x"] + 50, w["y"] + 81)
    time.sleep(0.15)
    check(b.toy_mode() == 2, f"the TOY menu switches to SoftBodies, mode {b.toy_mode()}")
    b.click(w["x"] + 24, w["y"] + 36)
    time.sleep(0.1)
    b.click(w["x"] + 50, w["y"] + 59)
    time.sleep(0.6)
    check(b.toy_mode() == 1, f"and back to SoftPhys, mode {b.toy_mode()}")
    p = b.pixels()
    check(p["other"] == 0, f"SoftPhys draws pure black and white: {p}")
    if prefix:
        b.shot(prefix + "-softphys.png")
    # The terminal, by typing: the same commands as everywhere else.
    b.key("`")
    time.sleep(0.1)
    b.type_text("toy softbodies")
    b.key("\ue007")  # enter
    time.sleep(0.15)
    check(b.toy_mode() == 2, f"the terminal switches toys, mode {b.toy_mode()}")
    if prefix:
        b.shot(prefix + "-terminal.png")
    b.key("`")  # hides it, and hands the keyboard back
    time.sleep(0.8)

    w = worlds(b.state())[0]
    p = b.pixels()
    check(p["other"] == 0 and p["white"] > 0 and p["black"] > 0, f"world pixels are pure black and white: {p}")
    check(p["w"] == w["w"] and p["h"] == w["h"], f"canvas matches its window, {p['w']}x{p['h']}")
    if prefix:
        b.shot(prefix + "-desktop.png")

    def at(x, y):
        return w["x"] + x, w["y"] + y

    # Keyboard, then the nuke, through the app's own input handling.
    b.click(*at(w["w"] // 2, 300))
    check(not b.is_white(60, 206), "NUKE button starts unlit")
    b.key("k")
    time.sleep(0.15)
    check(b.is_white(60, 206), "key K lights the NUKE button")
    before = b.pixels()["white"]
    b.click(*at(w["w"] // 2, 400))
    time.sleep(0.62)
    during = b.pixels()
    check(during["white"] > before * 1.5, f"a click fires the nuke: white {before} -> {during['white']}")
    check(during["other"] == 0, "the blast is pure black and white")
    if prefix:
        b.shot(prefix + "-nuke.png")
    time.sleep(1.2)
    if prefix:
        b.shot(prefix + "-cloud.png")
    b.key("g")
    b.key("r")
    time.sleep(0.1)

    # Window management through SoftWin's own chrome. Resizing goes first, since
    # the move would otherwise carry the corner down behind the tray.
    w0 = worlds(b.state())[0]
    b.drag(w0["x"] + w0["w"] - 3, w0["y"] + w0["h"] - 3, w0["x"] + w0["w"] - 203, w0["y"] + w0["h"] - 153)
    time.sleep(0.2)
    w1 = worlds(b.state())[0]
    check((w1["w"], w1["h"]) == (w0["w"] - 200, w0["h"] - 150), f"corner drag resizes it, to {w1['w']}x{w1['h']}")
    p = b.pixels()
    check((p["w"], p["h"]) == (w1["w"], w1["h"]) and p["other"] == 0, f"the canvas follows, pure b&w: {p}")

    x0, y0 = w1["x"], w1["y"]
    b.drag(x0 + 300, y0 + 12, x0 + 380, y0 + 62)
    w3 = worlds(b.state())[0]
    check((w3["x"] - x0, w3["y"] - y0) == (80, 50), f"title drag moves the window, by ({w3['x'] - x0},{w3['y'] - y0})")

    # Minimize button in SoftWin's title bar, computed as in draw_chrome.
    dot, title, f = 2, 24, 1
    bs = title - dot * 3
    bx = w3["w"] - f - dot * 2 - bs
    by = f + (title - bs) // 2
    b.click(w3["x"] + bx - bs - dot * 2 + bs // 2, w3["y"] + by + bs // 2)
    s = wait_for(lambda: b.state()["tabs"] == 1 and b.state())
    check(s and worlds(s)[0]["minimized"], "minimize button sends the world to the tray")
    b.js("window.document.querySelector('.tab').click(); return 1;")
    time.sleep(0.2)
    s = b.state()
    check(not worlds(s)[0]["minimized"] and s["tabs"] == 0, "the tray tab brings it back")

    # Close button: the program quits, and the desktop survives.
    b.click(w3["x"] + bx + bs // 2, w3["y"] + by + bs // 2)
    s = wait_for(lambda: not worlds(b.state()) and b.state())
    check(s and not worlds(s), "close button ends that world")
    if prefix:
        b.shot(prefix + "-empty.png")

    # Desktop icon: a real double click.
    icon = next(i for i in b.state()["icons"] if i["id"] == "softbodies")
    b.double_click(icon["x"] + 48, icon["y"] + 20)
    s = wait_for(lambda: worlds(b.state()) and b.state())
    check(s and len(worlds(s)) == 1, "double clicking the icon opens a new world")
    check(b.toy_mode() == 2, f"and that icon goes straight to its toy, mode {b.toy_mode()}")

    # Several worlds at once, each its own program.
    readme = next(i for i in b.state()["icons"] if i["id"] == "readme")
    b.double_click(readme["x"] + 48, readme["y"] + 20)
    s = wait_for(lambda: any(w["key"] == "readme" for w in b.state()["windows"]) and b.state())
    check(bool(s), "double clicking READ ME opens it")
    check(b.state()["active"] == "readme", "that takes the focus")

    b.js("window.document.querySelector('[data-menu=OPEN]')"
         ".dispatchEvent(new window.PointerEvent('pointerdown', {bubbles:true})); return 1;")
    item = b.js("const r = window.document.querySelector('[data-item=SCENES]').getBoundingClientRect();"
                "return [r.left + 20, r.top + 8];")
    b.click(item[0], item[1])
    scenes = wait_for(lambda: next((w for w in b.state()["windows"] if w["key"] == "scenes"), None))
    check(bool(scenes), "the menu opens SCENES")
    row = b.js("const r = window.document.querySelector('[data-scene=\"SOFTPHYS2\"]').getBoundingClientRect();"
               "return [r.left + 20, r.top + 8];")
    b.click(row[0], row[1])
    s = wait_for(lambda: len(worlds(b.state())) == 2 and b.state())
    check(s and len(worlds(s)) == 2, "a scene opens as a second world")
    time.sleep(0.8)
    check(b.toy_mode(1) == 1, f"a SoftPhys scene opens in SoftPhys, mode {b.toy_mode(1)}")
    a = b.pixels(0)
    c = b.pixels(1)
    check(a["other"] == 0 and c["other"] == 0, "both worlds draw pure black and white")
    if prefix:
        b.shot(prefix + "-worlds.png")

    # Trash: documents go in, SOFTBODIES won't.
    trash = next(i for i in b.state()["icons"] if i["id"] == "trash")
    keys = next(i for i in b.state()["icons"] if i["id"] == "keys")
    b.drag(keys["x"] + 48, keys["y"] + 20, trash["x"] + 48, trash["y"] + 20)
    s = b.state()
    check(s["trashed"] == 1, f"a dropped icon goes in the trash, {s['trashed']}")
    sb = next(i for i in s["icons"] if i["id"] == "softbodies")
    b.drag(sb["x"] + 48, sb["y"] + 20, trash["x"] + 48, trash["y"] + 20)
    s = b.state()
    sb2 = next(i for i in s["icons"] if i["id"] == "softbodies")
    check(s["trashed"] == 1 and not sb2["hidden"], "SOFTBODIES refuses the trash")
    check((sb2["x"], sb2["y"]) == (sb["x"], sb["y"]), "and returns to where it was")

    b.js("window.setWallpaperForTest = 0; return 1;")
    crashes = b.state()["crashes"]
    check(crashes == 0, f"no world crashed, {crashes}")
    errors = b.js("return Array.from(window.__errors);")
    check(not errors, f"no page errors: {errors}")


def main():
    page = sys.argv[1]
    prefix = sys.argv[2] if len(sys.argv) > 2 else None
    with tempfile.TemporaryDirectory() as profile:
        with open(os.path.join(profile, "user.js"), "w") as f:
            f.write(f'user_pref("marionette.port", {PORT});\n')
            f.write('user_pref("datareporting.policy.dataSubmissionEnabled", false);\n')
            f.write('user_pref("browser.shell.checkDefaultBrowser", false);\n')
        env = dict(os.environ, MOZ_HEADLESS_WIDTH="1400", MOZ_HEADLESS_HEIGHT="900")
        env.pop("WAYLAND_DISPLAY", None)
        fx = subprocess.Popen(["firefox", "--headless", "--marionette", "--no-remote", "--profile", profile],
                              env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            for _ in range(200):
                try:
                    b = Browser()
                    break
                except OSError:
                    time.sleep(0.2)
            else:
                check(False, "firefox never opened its control port")
                return 1
            b.call("WebDriver:NewSession", {"capabilities": {"alwaysMatch": {"acceptInsecureCerts": False}}})
            scenario(b, page, prefix)
            b.call("Marionette:Quit")
        finally:
            try:
                fx.wait(timeout=8)
            except subprocess.TimeoutExpired:
                fx.kill()
                fx.wait()
    print(f"web_drive: {len(failures)} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
