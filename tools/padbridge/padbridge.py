"""Controller bridge for the Android emulator: the PC's first XInput pad shows up in the emulator as an Xbox 360 pad.

Builds padbridge.c with the NDK, pushes it, runs it as root and feeds it the pad's state over adb's stdin.
Needs the emulator running and `adb root`. Ctrl+C ends it, and the pad disappears from the emulator.

    python tools/padbridge/padbridge.py [--abi x86_64]
"""
import argparse
import ctypes
import os
import pathlib
import struct
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
SDK = pathlib.Path(os.environ.get("ANDROID_HOME") or pathlib.Path(os.environ["LOCALAPPDATA"]) / "Android" / "Sdk")
NDK = SDK / "ndk" / "29.0.14206865"
ADB = str(SDK / "platform-tools" / "adb.exe")
REMOTE = "/data/local/tmp/padbridge"
TARGETS = {"x86_64": "x86_64-linux-android30", "arm64-v8a": "aarch64-linux-android30"}


class XInputGamepad(ctypes.Structure):
    _fields_ = [("buttons", ctypes.c_uint16), ("lt", ctypes.c_uint8), ("rt", ctypes.c_uint8),
                ("lx", ctypes.c_int16), ("ly", ctypes.c_int16), ("rx", ctypes.c_int16), ("ry", ctypes.c_int16)]


class XInputState(ctypes.Structure):
    _fields_ = [("packet", ctypes.c_uint32), ("pad", XInputGamepad)]


def build(abi):
    out = HERE.parent.parent / "out" / "padbridge" / abi / "padbridge"
    src = HERE / "padbridge.c"
    if not out.exists() or out.stat().st_mtime < src.stat().st_mtime:
        out.parent.mkdir(parents=True, exist_ok=True)
        clang = NDK / "toolchains" / "llvm" / "prebuilt" / "windows-x86_64" / "bin" / "clang.exe"
        subprocess.run([str(clang), f"--target={TARGETS[abi]}", "-O2", "-static", "-o", str(out), str(src)], check=True)
    return out


def main():
    sys.stdout.reconfigure(line_buffering=True)
    ap = argparse.ArgumentParser()
    ap.add_argument("--abi", default="x86_64", choices=TARGETS)
    ap.add_argument("--pad", type=int, default=0, help="XInput user index 0-3")
    args = ap.parse_args()

    binary = build(args.abi)
    subprocess.run([ADB, "root"], check=True, capture_output=True)
    subprocess.run([ADB, "wait-for-device"], check=True)
    subprocess.run([ADB, "push", str(binary), REMOTE], check=True, capture_output=True)
    subprocess.run([ADB, "shell", "chmod", "755", REMOTE], check=True)

    xinput = ctypes.WinDLL("xinput1_4")
    bridge = subprocess.Popen([ADB, "shell", "-T", REMOTE], stdin=subprocess.PIPE)
    print(f"bridge running for XInput pad {args.pad}, Ctrl+C ends it")
    state, last, connected = XInputState(), None, None
    try:
        while bridge.poll() is None:
            ok = xinput.XInputGetState(args.pad, ctypes.byref(state)) == 0
            if ok != connected:
                connected = ok
                print("pad connected" if ok else "pad not found, waiting")
            p = state.pad if ok else XInputGamepad()
            data = struct.pack("<HBBhhhh", p.buttons, p.lt, p.rt, p.lx, p.ly, p.rx, p.ry)
            if data != last:
                bridge.stdin.write(data)
                bridge.stdin.flush()
                last = data
            time.sleep(0.004)
    except KeyboardInterrupt:
        pass
    finally:
        if bridge.stdin:
            bridge.stdin.close()
        bridge.wait(5)
    return bridge.returncode or 0


if __name__ == "__main__":
    sys.exit(main())
