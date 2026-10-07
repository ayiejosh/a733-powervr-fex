#version 450
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
layout(location = 2) out vec4 o2;
layout(location = 3) out vec4 o3;
layout(location = 4) out vec4 o4;
layout(location = 5) out vec4 o5;
layout(location = 6) out vec4 o6;
layout(location = 7) out vec4 o7;
void main() {
    /* output i writes i into red, so attachment i must read back i and nothing else */
    o0 = vec4(0.0/255.0, 0, 0, 1);
    o1 = vec4(1.0/255.0, 0, 0, 1);
    o2 = vec4(2.0/255.0, 0, 0, 1);
    o3 = vec4(3.0/255.0, 0, 0, 1);
    o4 = vec4(4.0/255.0, 0, 0, 1);
    o5 = vec4(5.0/255.0, 0, 0, 1);
    o6 = vec4(6.0/255.0, 0, 0, 1);
    o7 = vec4(7.0/255.0, 0, 0, 1);
}
