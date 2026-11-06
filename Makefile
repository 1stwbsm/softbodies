CC      ?= cc
CFLAGS  ?= -O2 -g
WARN    := -std=gnu11 -Wall -Wextra -Wshadow
LDLIBS  := -lm

BUILD   := build
CORE    := src/softpute/softpute.c src/softphys/softphys.c src/softwin/sw_platform.c \
           src/softwin/sw_gfx.c src/softwin/sw_desk.c src/app/main.c src/app/toybox.c \
           src/app/render.c src/app/breakage.c src/app/selector.c src/app/phys_ui.c src/app/terminal.c
LINUX_SRC := $(CORE) src/softwin/sw_wayland.c src/softwin/sw_x11.c
OBJS    := $(patsubst %.c,$(BUILD)/linux/%.o,$(LINUX_SRC))
TEST_OBJS := $(BUILD)/linux/tests/test_softpute.o $(BUILD)/linux/src/softpute/softpute.o
PHYS_TEST_OBJS := $(BUILD)/linux/tests/test_softphys.o $(BUILD)/linux/src/softphys/softphys.o
HEADERS := $(wildcard src/*/*.h)

# Windows. Any MinGW-w64 will do, so a 64-bit build is `make windows WIN_CC=x86_64-w64-mingw32-gcc`.
WIN_CC  ?= i686-w64-mingw32-gcc
WIN_SRC := $(CORE) src/softwin/sw_win32.c
WIN_EXE := dist/softbodies.exe

# WebAssembly. Needs a C compiler with a wasm32-wasi target and wasi-libc.
WASM_CC ?= zig cc -target wasm32-wasi
WASM_SRC := $(CORE) src/softwin/sw_web.c
WEB_PAGE := dist/softbodies.html

all: softbodies

softbodies: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/linux/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(WARN) -MMD -MP -c -o $@ $<

$(BUILD)/test_softpute: $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test_softphys: $(PHYS_TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

windows: $(WIN_EXE)

# Static, so it runs without MinGW's DLLs. SSE2 keeps 32-bit floats away from x87.
$(WIN_EXE): $(WIN_SRC) $(HEADERS)
	@mkdir -p dist
	$(WIN_CC) -O2 -msse2 -mfpmath=sse $(WARN) -mwindows -static -s -o $@ $(WIN_SRC) \
		-lgdi32 -luser32 -ldwmapi -lwinmm -lm

# Builds the test driver for Windows. Run it there, or under Wine: dist/win_drive.exe dist/softbodies.exe
windows-test: $(WIN_EXE) tests/win_drive.c src/softwin/sw_gfx.c
	$(WIN_CC) -O2 -msse2 -mfpmath=sse $(WARN) -static -s -o dist/win_drive.exe tests/win_drive.c src/softwin/sw_gfx.c -lgdi32 -luser32 -lm

web: $(WEB_PAGE)

$(BUILD)/softbodies.wasm: $(WASM_SRC) $(HEADERS)
	@mkdir -p $(BUILD)
	$(WASM_CC) -O2 $(WARN) -mexec-model=reactor -s -o $@ $(WASM_SRC) -lm

$(WEB_PAGE): $(BUILD)/softbodies.wasm web/shell.html tools/bundle_web.py
	@mkdir -p dist
	python3 tools/bundle_web.py web/shell.html $(BUILD)/softbodies.wasm $@

# Runs the page in real headless Firefox, using a throwaway profile.
web-test: $(WEB_PAGE)
	python3 tests/web_drive.py $(WEB_PAGE)

test: $(BUILD)/test_softpute $(BUILD)/test_softphys softbodies
	$(BUILD)/test_softpute
	$(BUILD)/test_softphys
	python3 tests/mock_wayland.py ./softbodies
	python3 tests/ui_script.py ./softbodies

clean:
	rm -rf $(BUILD) dist softbodies

-include $(OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(PHYS_TEST_OBJS:.o=.d)

.PHONY: all windows windows-test web web-test test clean
