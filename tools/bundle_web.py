#!/usr/bin/env python3
"""Packs softbodies.wasm into web/shell.html, making one self-contained page.

The result needs no server: it can be opened directly, or hosted anywhere.

usage: bundle_web.py shell.html softbodies.wasm out.html
"""
import base64
import sys


def main():
    shell_path, wasm_path, out_path = sys.argv[1:4]
    with open(shell_path, encoding="utf-8") as f:
        shell = f.read()
    with open(wasm_path, "rb") as f:
        wasm = f.read()
    if not wasm.startswith(b"\0asm"):
        sys.exit(f"{wasm_path} is not a WebAssembly module")
    marker = "@@WASM_BASE64@@"
    if shell.count(marker) != 1:
        sys.exit(f"{shell_path} must contain {marker} exactly once")
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(shell.replace(marker, base64.b64encode(wasm).decode("ascii")))
    print(f"{out_path}: {len(wasm)} bytes of wasm")


if __name__ == "__main__":
    main()
