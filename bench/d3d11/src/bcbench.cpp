// Timed BC1 decode-on-upload bench vs raw RGBA8 upload. Headless, offscreen-safe.
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdlib>
int main(){
  ID3D11Device*dev=0;ID3D11DeviceContext*ctx=0;D3D_FEATURE_LEVEL fl;
  if(FAILED(D3D11CreateDevice(0,D3D_DRIVER_TYPE_HARDWARE,0,0,0,0,D3D11_SDK_VERSION,&dev,&fl,&ctx)))return 1;
  const int W=2048,H=2048,ITER=8;
  // BC1: 8 bytes per 4x4 block
  size_t bcSize=(size_t)(W/4)*(H/4)*8, rgbaSize=(size_t)W*H*4;
  unsigned char*bc=(unsigned char*)malloc(bcSize); for(size_t i=0;i<bcSize;i++)bc[i]=(unsigned char)i;
  unsigned char*rgba=(unsigned char*)malloc(rgbaSize); for(size_t i=0;i<rgbaSize;i+=4096)rgba[i]=(unsigned char)i;
  LARGE_INTEGER f,t0,t1; QueryPerformanceFrequency(&f);
  double tbc=0,trgba=0;
  for(int i=0;i<ITER;i++){
    D3D11_TEXTURE2D_DESC td={};td.Width=W;td.Height=H;td.MipLevels=1;td.ArraySize=1;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd;
    td.Format=DXGI_FORMAT_BC1_UNORM; sd={bc,(UINT)(W/4*8),(UINT)bcSize};
    QueryPerformanceCounter(&t0);
    ID3D11Texture2D*t=0; if(FAILED(dev->CreateTexture2D(&td,&sd,&t)))return 2;
    ctx->Flush();
    QueryPerformanceCounter(&t1); if(i)tbc+=double(t1.QuadPart-t0.QuadPart)/f.QuadPart; t->Release();
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; sd={rgba,(UINT)W*4,(UINT)rgbaSize};
    QueryPerformanceCounter(&t0);
    ID3D11Texture2D*t2=0; if(FAILED(dev->CreateTexture2D(&td,&sd,&t2)))return 3;
    ctx->Flush();
    QueryPerformanceCounter(&t1); if(i)trgba+=double(t1.QuadPart-t0.QuadPart)/f.QuadPart; t2->Release();
  }
  int n=ITER-1; // first iter = warmup, excluded
  printf("BC1 2048x2048 (%zu KB) upload+decode: %.2f ms/tex (%.1f MB/s of decoded pixels)\n",bcSize/1024,tbc/n*1000,(rgbaSize/1048576.0)/(tbc/n));
  printf("RGBA8 2048x2048 (%zu KB) upload:      %.2f ms/tex (%.1f MB/s)\n",rgbaSize/1024,trgba/n*1000,(rgbaSize/1048576.0)/(trgba/n));
  printf("BC1 decode overhead vs RGBA: %.2fx\nBCBENCH_OK\n",tbc/trgba);
  return 0;
}
