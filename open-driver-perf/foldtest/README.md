# foldtest

Regression test for the pvr patch "hold clear-only dynamic render passes until a later pass uses the image"
(commit 696bbd6 on the board, `0016-pvr-hold-clear-only-dynamic-render-passes.patch`).

Build (on the board): `glslangValidator -V --target-env vulkan1.1 -o fold.vert.spv fold.vert`, same for
`fold.frag`, then `gcc -O1 -Wall -o foldtest foldtest.c -lvulkan`.
Run: `VK_ICD_FILENAMES=<icd.json> PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 ./foldtest` (needs the open stack loaded).
`FOLD_ONLY=<0..14>` runs one scenario (13, 14: null texture descriptors in one pipeline, see WORKLOG; needs tex3.frag.spv: `glslangValidator -V --target-env vulkan1.1 -o tex3.frag.spv tex3.frag`); `PVR_DEBUG=noclearfold` turns the fold off.

Scenarios (each starts with stale contents A=green, B=blue, checks pixel colours after readback):
0 baseline held clear at end of command buffer, 1 copy in the same command buffer, 2 legacy render pass right
after a held clear, 3 occlusion query inside a clear-only pass (+ the query result must become available),
4 STORE_OP_NONE pass after a held clear, 5 consume (LOAD,STORE), 6 a pass whose draw samples the held-cleared
image (read path, reorder), 7 the zink shape (held clear, unrelated pass, load pass), 8 an occlusion query around the draw of a pass that
samples the held-cleared image (sample count must be 4096 after the reorder).
Results 2026-10-13: vp (before the change) and the 696bbd6 build (cf6): 11/11 checks PASS (cf6 with noclearfold: 9/9 on the first
9 checks);
the first commit (281ebd1 build, cf5) segfaults on scenario 2, never completes the query in scenario 3 and leaves stale
contents in scenario 4 (scenarios 0, 1, 5, 6, 7 pass there). Scenario 8 passes on cf5 too (the availability write moved ahead of the pass is a transient window, not visible in the final
state). Not covered: relink with resolve/ZLS copy-back sub-commands in the reading pass, and a pass that reads the image whose clear it took over.
