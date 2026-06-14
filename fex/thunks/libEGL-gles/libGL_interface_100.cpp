#include "/home/radxa/FEX-src/ThunkLibs/libGL/libGL_interface.cpp"
extern "C" {
  void glBlendBarrier(void);
  void glPrimitiveBoundingBox(float,float,float,float,float,float,float,float);
}
template<> struct fex_gen_config<glBlendBarrier> {};
template<> struct fex_gen_config<glPrimitiveBoundingBox> {};
