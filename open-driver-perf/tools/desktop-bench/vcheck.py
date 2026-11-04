#!/usr/bin/env python3
"""vcheck.py MODE IMAGE.png - is a benchmark screenshot visually plausible? Prints one line, exits 1 if not.
  full   : the scene fills the screen (WebGL cubes, CSS boxes): every cell of a 4x4 grid has detail
           across x (catches flat areas and horizontal streaks).
  screen : something is shown (desktop): not black, most cells not one flat colour.
  video  : screen, and under 3% near-black pixels (the test clip is colour bars with no black area).
It does not compare against a reference image, so the screenshots are still looked at by hand.
Self-test: vcheck.py selftest"""
import subprocess, sys

W, H, G = 192, 108, 4

def gray(path):
    raw = subprocess.run(["ffmpeg", "-loglevel", "error", "-i", path, "-vf", f"scale={W}:{H},format=gray",
                          "-f", "rawvideo", "-"], check=True, capture_output=True).stdout
    if len(raw) != W * H:
        raise ValueError(f"decoded {len(raw)} bytes, expected {W * H}")
    return raw

def cell_stats(px):
    cw, ch = W // G, H // G
    out = []
    for gy in range(G):
        for gx in range(G):
            v = [px[y * W + x] for y in range(gy * ch, (gy + 1) * ch) for x in range(gx * cw, (gx + 1) * cw)]
            m = sum(v) / len(v)
            sd = (sum((x - m) ** 2 for x in v) / len(v)) ** .5
            # mean change between neighbouring pixels along x: zero for a cell of horizontal streaks,
            # which still has a high spread
            horiz = sum(abs(v[r * cw + c + 1] - v[r * cw + c]) for r in range(ch) for c in range(cw - 1)) / (ch * (cw - 1))
            out.append((m, sd, horiz))
    return out

def judge(mode, px):
    cells = cell_stats(px)
    mean = sum(px) / len(px)
    if mode == "full":
        bad = [i for i, (m, sd, h) in enumerate(cells) if sd < 6 or h < 1.5]
        return not bad, f"{16 - len(bad)}/16 cells with detail" + (f", flat or streaked cells {bad}" if bad else "")
    busy = sum(1 for (m, sd, h) in cells if sd >= 3)
    ok, why = mean >= 8 and busy >= 10, f"mean {mean:.0f}, {busy}/16 cells with detail"
    if mode == "video":
        # the test clip (colour bars) has no black areas; a black block is a frame not (fully) drawn.
        # All-black rows are letterbox bars (16:9 video on a 4:3 screen) and are left out.
        rows = [px[y * W:(y + 1) * W] for y in range(H)]
        rows = [r for r in rows if max(r) >= 16] or rows
        black = sum(1 for r in rows for b in r if b < 16) / (len(rows) * W)
        ok, why = ok and black < .03, why + f", {black:.1%} black"
    return ok, why

def selftest():
    import random
    rnd = random.Random(1)
    noise = bytes(rnd.randrange(256) for _ in range(W * H))
    flat = bytes([30]) * (W * H)
    # detail only in the bottom-left quarter, flat elsewhere: the 2026-11-04 Firefox WebGL frame
    quarter = bytes(noise[i] if (i // W >= H // 2 and i % W < W // 2) else 30 for i in range(W * H))
    streaks = bytes((i // W * 37) % 256 for i in range(W * H))
    assert judge("full", noise)[0]
    assert not judge("full", flat)[0]
    assert not judge("full", quarter)[0]
    assert not judge("full", streaks)[0]
    assert judge("screen", noise)[0]
    assert not judge("screen", bytes(W * H))[0]
    blackblock = bytes(0 if (i // W < H // 3 and i % W < W // 3) else noise[i] | 64 for i in range(W * H))
    assert judge("screen", blackblock)[0] and not judge("video", blackblock)[0]
    letterbox = bytes(0 if (i // W < H // 8 or i // W >= H - H // 8) else noise[i] | 64 for i in range(W * H))
    assert judge("video", letterbox)[0]
    print("selftest ok")

if __name__ == "__main__":
    if sys.argv[1:] == ["selftest"]:
        selftest(); sys.exit(0)
    mode, path = sys.argv[1], sys.argv[2]
    try:
        ok, why = judge(mode, gray(path))
    except Exception as e:
        print(f"VISUAL FAIL [{mode}]: {e}"); sys.exit(1)
    print(("visual ok" if ok else "VISUAL FAIL") + f" [{mode}]: {why}")
    sys.exit(0 if ok else 1)
