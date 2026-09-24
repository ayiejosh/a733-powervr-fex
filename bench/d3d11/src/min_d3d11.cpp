#include <windows.h>
#include <d3d11.h>
#include <cstdio>
int main(){ printf("APP_START\n"); fflush(stdout);
  WNDCLASSA wc={}; wc.lpfnWndProc=DefWindowProcA; wc.hInstance=GetModuleHandleA(0); wc.lpszClassName="d3dmin";
  RegisterClassA(&wc); printf("REGCLASS_OK\n");fflush(stdout);
  HWND hwnd=CreateWindowA("d3dmin","min_d3d11",WS_OVERLAPPEDWINDOW,120,120,640,480,0,0,wc.hInstance,0); printf("CREATEWIN=%p\n",hwnd);fflush(stdout);
  ShowWindow(hwnd,SW_SHOW); printf("WINDOW_OK\n"); fflush(stdout);
  DXGI_SWAP_CHAIN_DESC scd={};
  scd.BufferCount=2; scd.BufferDesc.Width=640; scd.BufferDesc.Height=480;
  scd.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
  scd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; scd.OutputWindow=hwnd;
  scd.SampleDesc.Count=1; scd.Windowed=TRUE; scd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
  ID3D11Device* dev=0; ID3D11DeviceContext* ctx=0; IDXGISwapChain* sc=0; D3D_FEATURE_LEVEL fl;
  HRESULT hr=D3D11CreateDeviceAndSwapChain(0,D3D_DRIVER_TYPE_HARDWARE,0,0,0,0,D3D11_SDK_VERSION,&scd,&sc,&dev,&fl,&ctx);
  printf("CreateDeviceAndSwapChain hr=0x%08lx fl=0x%x\n",hr,fl); fflush(stdout);
  if(FAILED(hr)) return 1;
  ID3D11Texture2D* bb=0; sc->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&bb);
  ID3D11RenderTargetView* rtv=0; dev->CreateRenderTargetView(bb,0,&rtv);
  printf("got RTV; entering present loop\n"); fflush(stdout);
  for(int i=0;i<1800;i++){
    float t=i*0.02f; float c[4]={0.2f+0.3f*__builtin_sinf(t),0.3f,0.5f+0.4f*__builtin_cosf(t),1.0f};
    ctx->ClearRenderTargetView(rtv,c);
    HRESULT pr=sc->Present(1,0);
    if(i==0){ printf("first Present hr=0x%08lx\n",pr); fflush(stdout);} 
    MSG m; while(PeekMessageA(&m,0,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageA(&m);}
    Sleep(16);
  }
  printf("PRESENTED_600_FRAMES_OK\n"); fflush(stdout); return 0;
}
