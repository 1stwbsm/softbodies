#!/usr/bin/env python3
"""A scripted fake Wayland compositor that tests SoftWin's wire protocol client.

It plays the compositor's side over a real socket, and checks that softbodies:
  - scales to a fractional preferred scale (crisp buffers, correct viewport)
  - follows a compositor resize, acknowledging the configure first
  - turns pointer input into the right move, resize and minimize requests
  - receives a keymap fd without leaking or choking, and obeys Ctrl+Q
  - only ever draws pure black and white pixels

usage: mock_wayland.py path/to/softbodies
"""
import array
import mmap
import os
import socket
import struct
import subprocess
import sys
import tempfile

BTN_LEFT = 0x110
GLOBALS = [
    (1, "wl_compositor", 6), (2, "wl_shm", 1), (3, "xdg_wm_base", 2), (4, "wl_seat", 5),
    (5, "wp_viewporter", 1), (6, "wp_fractional_scale_manager_v1", 1),
]
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


class ScriptEnded(Exception):
    pass


class Compositor:
    def __init__(self, sock):
        self.sock = sock
        self.objects = {1: "wl_display"}
        self.fds = []
        self.pools = {}      # id -> (mmap, size)
        self.buffers = {}    # id -> (pool, offset, w, h, stride)
        self.attached = None
        self.frame_cbs = []
        self.commits = 0
        self.buffered_commits = 0
        self.serial = 100
        self.buf = b""
        self.acks = []
        self.pending_serial = None
        self.viewport_dest = None
        self.geometry = None
        self.moves = []
        self.resizes = []
        self.minimized = 0
        self.cursor_hidden = False
        self.last_size = None
        self.non_bw = 0
        self.ids = {}        # interface -> object id
        self.title = None
        self.press_serial = None

    # ---- sending events
    def send(self, obj, op, payload=b"", fds=None):
        data = struct.pack("<II", obj, ((8 + len(payload)) << 16) | op) + payload
        if fds:
            self.sock.sendmsg([data], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", fds))])
        else:
            self.sock.sendall(data)

    def next_serial(self):
        self.serial += 1
        return self.serial

    def configure(self, w, h):
        self.send(self.ids["xdg_toplevel"], 0, struct.pack("<iiI", w, h, 0))
        s = self.next_serial()
        self.pending_serial = s
        self.send(self.ids["xdg_surface"], 0, struct.pack("<I", s))

    def pointer_at(self, x, y):
        p = self.ids["wl_pointer"]
        self.send(p, 2, struct.pack("<Iii", 0, int(x * 256), int(y * 256)))
        self.send(p, 5)

    def click_down(self):
        s = self.next_serial()
        self.press_serial = s
        self.send(self.ids["wl_pointer"], 3, struct.pack("<IIII", s, 0, BTN_LEFT, 1))
        self.send(self.ids["wl_pointer"], 5)

    def click_up(self):
        self.send(self.ids["wl_pointer"], 3, struct.pack("<IIII", self.next_serial(), 0, BTN_LEFT, 0))
        self.send(self.ids["wl_pointer"], 5)

    def key(self, code, state):
        self.send(self.ids["wl_keyboard"], 3, struct.pack("<IIII", self.next_serial(), 0, code, state))

    # ---- receiving requests
    def pump(self):
        data = self._recv()
        if not data:
            return False
        self.buf += data
        while len(self.buf) >= 8:
            obj, hdr = struct.unpack("<II", self.buf[:8])
            size, op = hdr >> 16, hdr & 0xFFFF
            if len(self.buf) < size:
                break
            self.handle(obj, op, self.buf[8:size])
            self.buf = self.buf[size:]
        return True

    def _recv(self):
        data, fds, _, _ = socket.recv_fds(self.sock, 65536, 16)
        self.fds.extend(fds)
        return data

    def handle(self, obj, op, a):
        iface = self.objects.get(obj)
        u = lambda i: struct.unpack_from("<I", a, i * 4)[0]
        i32 = lambda i: struct.unpack_from("<i", a, i * 4)[0]
        if iface is None:
            raise AssertionError(f"request on unknown object {obj} op {op}")
        if iface == "wl_display":
            if op == 0:
                self.send(u(0), 0, struct.pack("<I", 0))
            elif op == 1:
                self.objects[u(0)] = "wl_registry"
                for name, n, ver in GLOBALS:
                    b = n.encode() + b"\0"
                    pad = b + b"\0" * (-len(b) % 4)
                    self.send(u(0), 0, struct.pack("<II", name, len(b)) + pad + struct.pack("<I", ver))
        elif iface == "wl_registry":
            n = u(1)
            name = a[8:8 + n - 1].decode()
            words = (n + 3) // 4
            ver = u(2 + words)
            new = u(3 + words)
            self.objects[new] = name
            self.ids[name] = new
            allowed = {g[1]: g[2] for g in GLOBALS}
            check(ver <= allowed[name], f"binds {name} at supported version {ver}") if ver > allowed[name] else None
            if name == "wl_seat":
                self.send(new, 0, struct.pack("<I", 3))  # pointer + keyboard
        elif iface == "wl_compositor":
            if op == 0:
                self.objects[u(0)] = "wl_surface"
                self.ids["wl_surface"] = u(0)
        elif iface == "wl_shm":
            if op == 0:
                fd = self.fds.pop(0)
                size = i32(1)
                self.objects[u(0)] = "wl_shm_pool"
                self.pools[u(0)] = (mmap.mmap(fd, size, prot=mmap.PROT_READ), size)
                os.close(fd)
        elif iface == "wl_shm_pool":
            if op == 0:
                self.objects[u(0)] = "wl_buffer"
                self.buffers[u(0)] = (self.pools[obj][0], i32(1), i32(2), i32(3), i32(4))
                check(u(5) == 1, "uses XRGB8888") if u(5) != 1 else None
        elif iface == "wl_surface":
            if op == 1:
                self.attached = u(0)
            elif op == 3:
                self.objects[u(0)] = "wl_callback"
                self.frame_cbs.append(u(0))
            elif op == 6:
                self.on_commit()
        elif iface == "xdg_wm_base":
            if op == 2:
                self.objects[u(0)] = "xdg_surface"
                self.ids["xdg_surface"] = u(0)
        elif iface == "xdg_surface":
            if op == 1:
                self.objects[u(0)] = "xdg_toplevel"
                self.ids["xdg_toplevel"] = u(0)
            elif op == 3:
                self.geometry = (i32(2), i32(3))
            elif op == 4:
                self.acks.append(u(0))
        elif iface == "xdg_toplevel":
            if op == 2:
                self.title = a[4:4 + u(0) - 1].decode()
            elif op == 5:
                self.moves.append(u(1))
            elif op == 6:
                self.resizes.append((u(1), u(2)))
            elif op == 13:
                self.minimized += 1
        elif iface == "wp_viewporter":
            if op == 1:
                self.objects[u(0)] = "wp_viewport"
        elif iface == "wp_viewport":
            if op == 2:
                self.viewport_dest = (i32(0), i32(1))
        elif iface == "wp_fractional_scale_manager_v1":
            if op == 1:
                self.objects[u(0)] = "wp_fractional_scale_v1"
                self.ids["wp_fractional_scale_v1"] = u(0)
        elif iface == "wl_seat":
            if op == 0:
                self.objects[u(0)] = "wl_pointer"
                self.ids["wl_pointer"] = u(0)
            elif op == 1:
                self.objects[u(0)] = "wl_keyboard"
                self.ids["wl_keyboard"] = u(0)
        elif iface == "wl_pointer":
            if op == 0 and u(1) == 0:
                self.cursor_hidden = True

    def inspect_buffer(self, buf_id):
        pool, off, w, h, stride = self.buffers[buf_id]
        self.last_size = (w, h)
        data = pool[off:off + stride * h]
        px = array.array("I")
        px.frombytes(data)
        self.non_bw += sum(1 for v in px if (v & 0xFFFFFF) not in (0, 0xFFFFFF))
        return px

    def on_commit(self):
        self.commits += 1
        if self.attached is None:
            if self.commits == 1:
                self.configure(0, 0)
            return
        buf = self.attached
        self.attached = None
        self.buffered_commits += 1
        self.inspect_buffer(buf)
        self.send(buf, 0)
        for cb in self.frame_cbs:
            self.send(cb, 0, struct.pack("<I", 0))
        self.frame_cbs = []
        self.step(self.buffered_commits)

    def step(self, n):
        """The script. Each step runs when the client commits that many frames."""
        if n == 3:
            check(self.title == "softbodies", "sets the window title")
            check(self.last_size == (640, 480), f"first buffer is the requested size, got {self.last_size}")
            check(self.acks and self.acks[0] == self.pending_serial, "acks the first configure")
            self.send(self.ids["wp_fractional_scale_v1"], 0, struct.pack("<I", 180))  # 150%
        elif n == 8:
            check(self.last_size == (960, 720), f"150% scale gives a 960x720 buffer, got {self.last_size}")
            check(self.viewport_dest == (640, 480), f"viewport stays 640x480 logical, got {self.viewport_dest}")
            self.configure(800, 600)
        elif n == 14:
            check(self.acks[-1] == self.pending_serial, "acks the resize configure")
            check(self.last_size == (1200, 900), f"resize gives a 1200x900 buffer, got {self.last_size}")
            check(self.viewport_dest == (800, 600), f"viewport follows the resize, got {self.viewport_dest}")
            check(self.geometry == (800, 600), f"window geometry follows the resize, got {self.geometry}")
            # Keymap fd: the client must accept and close it, as a real compositor sends one.
            fd = os.memfd_create("keymap")
            os.write(fd, b"xkb_keymap {};\0")
            self.send(self.ids["wl_keyboard"], 0, struct.pack("<II", 1, 15), [fd])
            os.close(fd)
            self.send(self.ids["wl_pointer"], 0,
                      struct.pack("<IIii", self.next_serial(), self.ids["wl_surface"], 300 * 256, 300 * 256))
            self.send(self.ids["wl_pointer"], 5)
        elif n == 18:
            check(self.cursor_hidden, "hides the system cursor on enter")
            self.pointer_at(764, 12)  # minimize button, logical coordinates at 150%
        elif n == 20:
            self.click_down()
        elif n == 22:
            self.click_up()
        elif n == 24:
            check(self.minimized == 1, f"minimize button sends set_minimized, count {self.minimized}")
            self.pointer_at(300, 10)  # title bar
        elif n == 26:
            self.click_down()
        elif n == 28:
            check(self.moves == [self.press_serial], f"title drag sends move with the press serial, {self.moves}")
            self.click_up()
            self.pointer_at(798, 598)  # bottom right corner
        elif n == 30:
            self.click_down()
        elif n == 32:
            check(self.resizes == [(self.press_serial, 10)],
                  f"corner drag sends resize with edges=bottom-right, {self.resizes}")
            self.click_up()
            self.pointer_at(400, 300)
        elif n == 34:
            check(self.non_bw == 0, f"every pixel of every frame is pure black or white, {self.non_bw} were not")
            self.key(29, 1)  # ctrl
            self.key(16, 1)  # q
            self.key(16, 0)
            self.key(29, 0)
        elif n > 200:
            raise ScriptEnded("client ignored Ctrl+Q")


def main():
    exe = sys.argv[1]
    with tempfile.TemporaryDirectory() as run_dir:
        path = os.path.join(run_dir, "mock-0")
        server = socket.socket(socket.AF_UNIX)
        server.bind(path)
        server.listen(1)
        env = dict(os.environ, XDG_RUNTIME_DIR=run_dir, WAYLAND_DISPLAY="mock-0")
        client = subprocess.Popen([exe, "--backend", "wayland", "--size", "640x480"], env=env)
        server.settimeout(5)
        sock, _ = server.accept()
        sock.settimeout(5)
        comp = Compositor(sock)
        try:
            while comp.pump():
                pass
        except (ConnectionResetError, BrokenPipeError):
            pass
        except socket.timeout:
            failures.append("client went silent")
        except ScriptEnded as e:
            failures.append(str(e))
            client.kill()
        try:
            code = client.wait(timeout=5)
        except subprocess.TimeoutExpired:
            client.kill()
            code = -1
            failures.append("client did not exit")
        check(code == 0, f"exits cleanly on Ctrl+Q, code {code}")
        check(comp.buffered_commits >= 34, f"reached the end of the script, {comp.buffered_commits} frames")
    print(f"mock_wayland: {len(failures)} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
