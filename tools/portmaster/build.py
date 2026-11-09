#!/usr/bin/env python3
"""Cross-builds the `crusader` executable for aarch64 Linux handhelds (PortMaster).

Needs: python, cmake, ninja (pip install ninja), zig (pip install ziglang).
  python tools/portmaster/build.py [--zig PATH_TO_ZIG] [--out DIR]
The executable links against the device's libSDL2-2.0.so.0 (a symbol-only stub
is used at link time), so nothing but the binary has to be shipped.
"""
import argparse, os, re, shutil, subprocess, sys, tarfile, urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
SDL_VERSION = "2.32.10"


def run(cmd, **kw):
    print("+", " ".join(map(str, cmd)))
    subprocess.run(list(map(str, cmd)), check=True, **kw)


def find_zig(arg):
    if arg:
        return Path(arg)
    if os.environ.get("ZIG_EXE"):
        return Path(os.environ["ZIG_EXE"])
    found = shutil.which("zig")
    if found:
        return Path(found)
    try:
        import ziglang  # noqa: F401
        return Path(ziglang.__file__).parent / ("zig.exe" if os.name == "nt" else "zig")
    except ImportError:
        sys.exit("zig not found: pip install ziglang, or pass --zig")


def sdl_headers(work):
    existing = ROOT / "build" / "_deps" / "sdl2-src" / "include"
    if (existing / "SDL.h").exists():
        return existing
    dest = work / "sdl2-src"
    if not (dest / "include" / "SDL.h").exists():
        tgz = work / f"SDL2-{SDL_VERSION}.tar.gz"
        urllib.request.urlretrieve(
            f"https://github.com/libsdl-org/SDL/releases/download/release-{SDL_VERSION}/SDL2-{SDL_VERSION}.tar.gz", tgz)
        with tarfile.open(tgz) as t:
            # headers only: the archive holds symlinks (android-project-ant/src)
            # that the "data" filter rejects
            members = [m for m in t.getmembers() if "/include/" in m.name and m.isfile()]
            t.extractall(dest, members=members, filter="data")
        inner = next(dest.iterdir())
        for item in inner.iterdir():
            shutil.move(str(item), dest / item.name)
    return dest / "include"


def make_stub(zig, headers, work):
    names = set()
    for h in headers.glob("SDL*.h"):
        text = h.read_text(encoding="utf-8", errors="ignore")
        names.update(re.findall(r"\bextern\s+DECLSPEC\b[^;{]*?\bSDLCALL\s+(SDL_\w+)\s*\(", text))
        names.update(re.findall(r"\bDECLSPEC\b[^;{(]*?\*?\s*SDLCALL\s+(SDL_\w+)\s*\(", text))
    src = work / "sdl2_stub.c"
    src.write_text("".join(f"void {n}(void) {{}}\n" for n in sorted(names)))
    lib = work / "libSDL2-2.0.so.0"
    run([zig, "cc", "-target", "aarch64-linux-gnu.2.28", "-shared", "-Wl,-soname,libSDL2-2.0.so.0", "-o", lib, src])
    print(f"stub with {len(names)} SDL functions")
    return lib


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zig")
    ap.add_argument("--out", default=str(ROOT / "build-portmaster"))
    ap.add_argument("--version", default="0.0.0-dev")
    args = ap.parse_args()

    out = Path(args.out).resolve()
    work = out / "_work"
    work.mkdir(parents=True, exist_ok=True)
    zig = find_zig(args.zig)
    headers = sdl_headers(work)
    stub = make_stub(zig, headers, work)

    env = dict(os.environ, ZIG_EXE=str(zig))
    run(["cmake", "-S", ROOT, "-B", out, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         f"-DCMAKE_TOOLCHAIN_FILE={HERE / 'zig-aarch64.cmake'}",
         f"-DSDL2_DIR={HERE / 'sdl2-stub'}",
         f"-DCRUSADER_SDL2_STUB_LIB={stub}", f"-DCRUSADER_SDL2_HEADERS={headers}",
         "-DCRUSADER_GAME_DATA_TESTS=OFF", "-DCRUSADER_BUILD_EDITOR=OFF",
         f"-DCRUSADER_VERSION={args.version}"], env=env)
    run(["cmake", "--build", out, "--target", "crusader"], env=env)
    print("built:", out / "crusader")


if __name__ == "__main__":
    main()
