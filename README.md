# Soft Toys

A little box of desktop toys: black and white, hard edges, and no toolkit underneath. This is made using Claude Code, and do not expect it to be maintained frquently.

    make
    ./softbodies

The program opens on a selector. Pick a toy, and switch any time from the **TOY**
drop-down menu. Each toy keeps its state while you're elsewhere.

- **SoftPhys**: a powder, fluid, gas and heat sandbox.
- **SoftBodies**: squishy physics, from jelly to nukes.

Press `` ` `` in any of them for the terminal.

Needs a C compiler and Linux with Wayland or X11. Nothing else: the binary links
only libc and libm. `make test` also needs Python 3.

## Other platforms

| Platform    | Build          | Output                 | Needs                                              |
|-------------|----------------|------------------------|----------------------------------------------------|
| Windows     | `make windows` | `dist/softbodies.exe`  | MinGW-w64                                          |
| WebAssembly | `make web`     | `dist/softbodies.html` | Zig, or another `wasm32-wasi` C compiler; Python 3 |

The Windows build is a single 32-bit `.exe` with no DLLs to ship, and defaults to
`i686-w64-mingw32-gcc`. For 64-bit, use `make windows WIN_CC=x86_64-w64-mingw32-gcc`.

The web build is one self-contained page that needs no server, holding **SoftDesk**: a
fake 1-bit desktop for the toys to live on. It has icons, a menu bar, a clock, a tray for
minimized windows, wallpapers, a trash can, and a shut-down screen. Every window you open
is its own independent copy of the program.

## What's inside

**SoftPhys** (`src/softphys/`) is a cellular simulation of 28 elements. Each cell holds an
element and a temperature, so heat really flows: metal carries it, sand melts into glass,
lava sets into gravel, water boils and rains back down. Soft bodies float and sink in it,
and burn when it's hot.

**SoftPute** (`src/softpute/`) is the physics engine: XPBD with small substeps.

| Matter   | Held together by                                        |
|----------|---------------------------------------------------------|
| Jelly    | links plus shape matching, which remembers its rest pose |
| Balloons | a skin of links plus an area constraint, and they burst  |
| Cloth    | a tearable sheet of links                                |
| Rope     | a chain of links with light bending resistance           |
| Goo      | double density relaxation                                |

**SoftWin** (`src/softwin/`) is the window system, with no Qt, GTK, SDL, Xlib or
libwayland underneath:

- `sw_wayland.c` and `sw_x11.c` speak the display protocols directly over a socket
- `sw_gfx.c` draws strictly 1-bit: every grey you think you see is a dither
- `sw_desk.c` is an inner window manager with menus, sliders and buttons

## Things to try

- **Resize the window.** The walls are real, and they squeeze whatever's inside.
- **Drag an inner window into the pile.** They're solid, and things land on them.
  Turn that off with WINDOW > SOLID WINDOWS.
- **Raise PRESSURE** in scene 8 until the balloons go.
- **Cut a jelly** with the CUT tool and it collapses into goo. Cut cloth and it stays cut.
- **Press T and type.** Every key drops a soft letter.
- **Open scene 6.** The hoppers follow your cursor.
- **Set GRAVITY negative.**
- **Open scene 9.** Jellyfish swim toward the cursor. Scene 0 is a washing machine.
- **MAGNET, FREEZE and GLUE:** pull everything in, pin a body solid, stick two together.
- **In SoftPhys, try scene 3** for a volcano over the sea, **scene 4** for a forest fire,
  or **scene 5** and heat the powder at the foot of the dam.
- **In SoftPhys, press V** to see the heat.
- **Press K and click.** The NUKE vaporizes jelly and balloons near ground zero, cuts
  cloth and ropes, destroys your walls, and flings everything else. Drop TIME first to
  watch it in slow motion. The white flash is a single fade, limited in how often it
  can fire, and VIEW > NUKE FLASH turns it off.

## Controls

These are for SoftBodies. Press `F1` in a toy for its own list.

| Input         | Action                 | Key   | Action           |
|---------------|------------------------|-------|------------------|
| Left mouse    | use tool               | 1-8   | load scene       |
| Right mouse   | blast at cursor        | R / C | reset / clear    |
| Wheel         | spawn size             | Space | pause            |
| G X B M       | grab, cut, blow, bomb  | N     | step one frame   |
| I W E T       | pin, wall, erase, type | V     | cycle view       |
| K             | nuke                   | Tab   | hide all windows |
| A Z J         | magnet, freeze, glue   | `     | terminal         |
| Esc           | back to grab           |       |                  |
| Ctrl+Q        | quit                   | P     | save a PNG       |
| F1            | keys window            |       |                  |

## The terminal

Press `` ` ``, or use WINDOW > TERMINAL. It holds the keyboard until you press Esc or click
elsewhere. Type `HELP` for the commands, and `HELP NAME` for one of them. Up and down recall
history, and `;` separates commands.

    TOY SOFTPHYS; SCENE 3; SET SPEED 4
    RAIN WATER 200; HEAT 100 60 10 900; PROBE 100 60
    TOY SOFTBODIES; SPAWN SNAKE; REPEAT 5 SPAWN JELLY; NUKE

## Known limits

- Keys are read by physical position as US QWERTY, since SoftWin doesn't parse
  keymaps. On other layouts, the letter you press may not be the one that drops.
- The X11 backend needs a local 24-bit display. It has no remote TCP display support.
- On the web, the browser keeps its own shortcuts, so Ctrl+Q may close the browser. Use
  the window's close button there.
- X11 sends frames with PutImage rather than shared memory, so it's slower than Wayland.
- There's no maximize button, on purpose.

## Testing

    make test

This runs the behaviour tests for both physics engines, then `tests/mock_wayland.py`, a
scripted fake compositor that checks SoftWin's protocol handling, and `tests/ui_script.py`,
which drives the real program with injected input.

`make web-test` runs the web page in real headless Firefox, using a throwaway profile.

`make windows-test` builds `dist/win_drive.exe`, which launches the game, sends it
real mouse and keyboard input, and checks the window's pixels and geometry. Run it
on Windows, or under Wine:

    wine explorer /desktop=test,1500x1000 'dist\win_drive.exe' 'dist\softbodies.exe'

Use the virtual desktop under Wine, because otherwise window drags go to the host's
window manager, which can't see injected input. Without it the move and resize checks fail.

For repeatable runs, use `--backend headless` with `--frames`, `--shots` and `--script`.
See `./softbodies --help`.
