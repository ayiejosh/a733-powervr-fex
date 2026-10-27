#!/usr/bin/env python3
"""occgen.py N [N ...]  -- write occN.frag for each N: a fragment shader with N live accumulators, about 480 FMAs and 6 dependent texture fetches
in total (so the work is the same for every N and only the live register count changes)."""
import sys
for N in map(int, sys.argv[1:]):
    R = max(6, 480 // N)
    fetch_rounds = set(((R * (k + 1)) // 6) - 1 for k in range(6))
    L = ["#version 450", "layout(set = 0, binding = 0) uniform sampler2D tex;", "layout(location = 0) in vec2 uv;", "layout(location = 0) out vec4 o;", "void main() {"]
    for i in range(N):
        L.append("   float a%d = uv.x * %.4f + uv.y * %.4f;" % (i, 0.1 + 0.013 * i, 0.07 + 0.011 * i))
    for r in range(R):
        for i in range(N):
            L.append("   a%d = a%d * 0.%d + a%d * 0.0%d;" % (i, i, 990 + (i % 7), (i + 1) % N, 1 + (i % 5)))
        if r in fetch_rounds:
            L.append("   a0 += texture(tex, vec2(a0 * 0.0007 + 0.5, a1 * 0.0007 + 0.5)).x;")
    L.append("   float s = " + " + ".join("a%d" % i for i in range(N)) + ";")
    L.append("   o = vec4(s * 0.0001, a0 * 0.001, a1 * 0.001, 1.0);")
    L.append("}")
    open("occ%d.frag" % N, "w").write("\n".join(L) + "\n")
    print("occ%d.frag: %d rounds, %d fetches" % (N, R, len(fetch_rounds)))
