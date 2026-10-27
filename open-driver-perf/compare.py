#!/usr/bin/env python3
"""Compare harness result files: compare.py VENDOR OPEN_BASE OPEN_NEW [OPEN_NEW2 ...]

Prints the median of each workload per file, the gain of each new open run over the open
baseline, and where it stands against vendor (>1.00x = open is slower than vendor).
Correctness failures recorded by the harness are listed at the end.
"""
import re
import statistics as st
import sys


def parse(path):
    data, fails, key = {}, [], None
    for line in open(path):
        m = re.match(r"=== rep (\d+) (\S+) (\S+) (\S+)", line)
        if m:
            key = (m.group(2), m.group(3))
            continue
        m = re.search(r"speed\s*:\s*(\S+) ms/frame.*?(\S+) M inv/s", line)
        if m and key:
            val = m.group(1) if m.group(1) != "None" else m.group(2)
            data.setdefault(key, []).append(float(val))
        if "correctness" in line and key and "PASS" not in line and "None" not in line:
            fails.append((path, key, line.strip()))
    return data, fails


def main():
    files = sys.argv[1:]
    if len(files) < 3:
        sys.exit(__doc__)
    parsed = [parse(f) for f in files]
    vendor, base, news = parsed[0][0], parsed[1][0], [p[0] for p in parsed[2:]]
    head = f"{'workload':<16}{'unit':<10}{'vendor':>10}{'open base':>11}"
    for i in range(len(news)):
        head += f"{'new' + str(i + 1):>10}{'gain':>8}{'vs vend':>9}"
    print(head)
    for key in base:
        is_ms = key[0].startswith("vk")
        mv, mb = st.median(vendor[key]), st.median(base[key])
        row = f"{key[0] + ' ' + key[1]:<16}{'ms/frame' if is_ms else 'M inv/s':<10}{mv:>10.3f}{mb:>11.3f}"
        for new in news:
            mn = st.median(new[key])
            gain = mb / mn if is_ms else mn / mb
            vs = mn / mv if is_ms else mv / mn
            row += f"{mn:>10.3f}{gain:>7.2f}x{vs:>8.2f}x"
        print(row)
    fails = [f for p in parsed for f in p[1]]
    print("correctness failures:", fails or "none")


if __name__ == "__main__":
    main()
