#!/usr/bin/env python3
"""shot.py OUT.png - screenshot of the whole KWin workspace (X11 or Wayland session) through the
org.kde.KWin.ScreenShot2 D-Bus API, scaled to 1920 wide. KWin must run with
KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1; DBUS_SESSION_BUS_ADDRESS must point at the session bus."""
import os, subprocess, sys, threading
import dbus

out = sys.argv[1]
bus = dbus.SessionBus()
iface = dbus.Interface(bus.get_object("org.kde.KWin", "/org/kde/KWin/ScreenShot2"), "org.kde.KWin.ScreenShot2")
r, w = os.pipe()
chunks = []
def reader():
    while True:
        b = os.read(r, 1 << 20)
        if not b:
            break
        chunks.append(b)
t = threading.Thread(target=reader)
t.start()
res = iface.CaptureWorkspace({"native-resolution": True}, dbus.types.UnixFd(w))
os.close(w)
t.join(timeout=30)
data = b"".join(chunks)
W, H, stride = int(res["width"]), int(res["height"]), int(res["stride"])
# QImage ARGB32/RGB32 formats are BGRA bytes in memory on little-endian.
rows = b"".join(data[y * stride:y * stride + W * 4] for y in range(H))
subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "bgra", "-s", f"{W}x{H}",
                "-i", "-", "-vf", "scale=1920:-2", "-frames:v", "1", out], input=rows, check=True)
print(f"shot {out} {W}x{H} format {int(res.get('format', -1))}")
