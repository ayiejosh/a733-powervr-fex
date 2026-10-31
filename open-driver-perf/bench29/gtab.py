import re,sys
t=open(sys.argv[1]).read()
for m in re.finditer(r"\[(\w+)\] ([^\n]*?)(?:MESA[^\n]*\n)*?[^\[]*?FPS: (\d+)", t):
    print("%-12s %6s  %s" % (m.group(1), m.group(3), m.group(2)[:70]))
s=re.search(r"glmark2 Score: \d+", t); print(s.group(0) if s else "no score")
