---
name: Board test report
about: Report what works / doesn't on your A733 board (A7A/A7S/A7Z/other)
title: "[board] <your board> — works/doesn't"
labels: board-report
---

**Board / SoC / variant:**
**Kernel / BSP / DDK:**

**What I tested and the result:**
- [ ] kernel PRIME patch applies + loads
- [ ] `dmabuf_*_test` pass (PRIME import works)
- [ ] Vulkan probe (`vkprobe`) sees the GPU
- [ ] Zink GL works
- [ ] sway + wayvnc GPU desktop
- [ ] FEX runs x86 + Vulkan thunk
- [ ] other:

**Differences from the documented A7A baseline** (a finding that's wrong/changed for you?):
