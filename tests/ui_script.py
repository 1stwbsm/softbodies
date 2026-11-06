#!/usr/bin/env python3
"""Drives the real program headlessly with injected input, and checks what it reports.

usage: ui_script.py ./softbodies
"""
import os
import re
import subprocess
import sys
import tempfile

exe = sys.argv[1]
failures = []


def run(*args):
    out = subprocess.run([exe, "--backend", "headless", *args], capture_output=True, text=True, timeout=120).stdout
    m = re.search(r"toy=(\w+)", out)
    return (m.group(1) if m else None), out


def check(ok, what):
    print(("  ok   " if ok else "  FAIL ") + what)
    if not ok:
        failures.append(what)


def click(frame, x, y):
    return f"{frame}:move,{x},{y};{frame + 1}:down,{x},{y};{frame + 2}:up,{x},{y}"


def keys(frame, *codes):
    return ";".join(f"{frame + i * 2}:key,{c}" for i, c in enumerate(codes))


toy, _ = run("--frames", "10")
check(toy == "SELECTOR", f"the program opens on the selector, got {toy}")

# Cards: at 1200x800 the first is left of centre, the second right of it.
toy, _ = run("--frames", "12", "--script", click(4, 400, 400))
check(toy == "SOFTPHYS", f"clicking the first card opens SoftPhys, got {toy}")
toy, _ = run("--frames", "12", "--script", click(4, 800, 400))
check(toy == "SOFTBODIES", f"clicking the second card opens SoftBodies, got {toy}")
toy, _ = run("--frames", "12", "--script", keys(4, 2))
check(toy == "SOFTPHYS", f"key 1 opens SoftPhys, got {toy}")
toy, _ = run("--frames", "12", "--script", keys(4, 3))
check(toy == "SOFTBODIES", f"key 2 opens SoftBodies, got {toy}")

# The TOY drop-down, from inside a toy: open it, then pick from its list.
MENU_X, MENU_Y = 24, 36
for start, row, want in (("softbodies", 59, "SOFTPHYS"), ("softphys", 81, "SOFTBODIES"), ("softphys", 106, "SELECTOR")):
    toy, _ = run("--toy", start, "--frames", "20", "--script",
                 click(4, MENU_X, MENU_Y) + ";" + click(10, 50, row))
    check(toy == want, f"TOY menu takes {start} to {want}, got {toy}")

# A toy keeps its state while you're away.
_, out = run("--toy", "softbodies", "--frames", "8", "--dump-term", "--exec",
             "wipe; spawn crate 3; toy softphys; toy softbodies; stats")
check("BODIES 3" in out, "SoftBodies still has its 3 crates after a visit elsewhere")

# The terminal takes the keyboard, and gives it back.
_, out = run("--toy", "softphys", "--frames", "30", "--dump-term", "--script", keys(3, 41, 34, 18, 20, 28))
check("> GET" in out and "NO COMMAND CALLED GET" in out, "keys typed in the terminal reach it")
_, bare = run("--toy", "softbodies", "--frames", "12", "--dump-term", "--script", keys(3, 41, 44, 28))
check("NO COMMAND CALLED Z" in bare, "letters that are shortcuts elsewhere go to the terminal while it's focused")
_, out = run("--toy", "softphys", "--frames", "20", "--dump-term", "--script",
             "3:key,41;5:key,17;6:key,18;7:key,14;8:key,28")
check("> W" in out, "backspace removes a typed character")
_, out = run("--frames", "4", "--dump-term", "--exec", "toy softphys; set brush 99; set brush")
check("BRUSH = 24.00" in out, "SET clamps to the allowed range")
_, out = run("--frames", "4", "--dump-term", "--exec", "toy softphys; bogus; spawn nothing; paint nope 1 1; toy nope; scene 99")
for line in ("NO COMMAND CALLED BOGUS", "NO ITEM CALLED NOTHING", "NO ELEMENT CALLED NOPE", "NO TOY CALLED NOPE", "SCENES GO FROM 1 TO 8"):
    check(line in out, f"bad input is explained: {line}")
_, out = run("--frames", "4", "--dump-term", "--exec", "spawn blob")
check("NO TOY IS RUNNING" in out, "commands that need a toy say so at the selector")
_, out = run("--frames", "4", "--dump-term", "--exec", "toy softbodies; paint water 1 1")
check("THAT NEEDS SOFTPHYS" in out, "a command for the other toy says which it needs")
_, out = run("--frames", "4", "--dump-term", "--exec", "repeat 3 echo hi")
check(out.count("HI\n") == 3, "REPEAT runs its command that many times")

# Drawing in SoftPhys changes the grid, and the terminal can read it back.
_, out = run("--toy", "softphys", "--frames", "8", "--dump-term", "--exec", "wipe; paint water 100 50 3; stats")
check("WATER" in out, "PAINT puts water in the grid, and STATS sees it")
with tempfile.TemporaryDirectory() as tmp:
    shot = os.path.join(tmp, "s")
    run("--toy", "softphys", "--frames", "12", "--exec", "wipe; element wood", "--shots", "1,12", "--shot-prefix", shot,
        "--script", "2:move,600,300;3:down,600,300;5:move,640,300;7:up,640,300")
    a = open(shot + "-1.png", "rb").read()
    b = open(shot + "-12.png", "rb").read()
check(a != b, "dragging the mouse with DRAW changes the picture")

# Soft bodies and the grid affect each other.
POOL = "toy softphys; wipe; rect wall 0 240 398 242; rect {} 0 190 398 239; spawn crate 1 24"
_, out = run("--frames", "500", "--exec", POOL.format("lava"))
check("bodies=0" in out, "a body dropped in lava burns up")
_, out = run("--frames", "500", "--exec", POOL.format("water"))
check("bodies=1" in out, "a body dropped in water survives")
_, out = run("--frames", "500", "--dump-term", "--exec",
             "toy softphys; wipe; rect rock 0 200 398 242; spawn crate 1 24; step 400; run")
check("bodies=1" in out, "a body comes to rest on solid ground")

# Every scene of both toys runs.
for s in range(1, 11):
    toy, out = run("--toy", "softbodies", "--scene", str(s), "--frames", "90")
    check(toy == "SOFTBODIES" and "bodies=" in out, f"SoftBodies scene {s} runs") if toy != "SOFTBODIES" else None
for s in range(1, 9):
    toy, out = run("--toy", "softphys", "--scene", str(s), "--frames", "90")
    check(toy == "SOFTPHYS", f"SoftPhys scene {s} runs") if toy != "SOFTPHYS" else None
print("  ok   every scene of both toys ran")

print(f"ui_script: {len(failures)} failed")
sys.exit(1 if failures else 0)
