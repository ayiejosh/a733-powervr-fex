# tf-test

Does emulated transform feedback actually capture?

zink emulates it by having the vertex shader store the captured varyings through
a device address, so the only way to know it works is to ask GL for the data back.

A vertex shader passes `gl_Vertex` to a varying, three vertices are drawn with
rasterisation discarded and feedback active, and the buffer is read back.

    gcc -O2 -o tftest tftest.c -lEGL -lX11 -lGL

Measured, on the patched build with no layer and no fakes:

    captured: 1 2 3 4 5 6 7 8 9 10 11 12
    RESULT: transform feedback CAPTURES correctly, in order

All four components and the order, reproducibly. Getting only `1 0 0 0 5 0 0 0
9 0 0 0` means the varying is being read as a scalar - it is scalarised, so each
component has to be resolved by location *and* location_frac.
