#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdlib>
struct Vtx{ float x,y,z; float r,g,b; };
const char* SH=
"cbuffer C:register(b0){float a; int iters; float2 pad;};\n"
"struct VO{float4 p:SV_POSITION;float3 c:COLOR;};\n"
"VO vs(float3 pos:POSITION,float3 col:COLOR){VO o;float s=sin(a),co=cos(a);\n"
" o.p=float4(pos.x*co-pos.y*s,pos.x*s+pos.y*co,0,1);o.c=col;return o;}\n"
"float4 ps(VO i):SV_TARGET{float v=0;[loop]for(int k=0;k<iters;k++){v+=sin(a+k*0.13+i.p.x*0.01)*cos(i.p.y*0.01+k*0.07);}\n"
" return float4(i.c*(0.5+0.5*frac(v)),1);}\n";
int main(int argc,char**argv){
  int ndraw = argc>1?atoi(argv[1]):1;
  int iters = argc>2?atoi(argv[2]):1;
  int secs  = argc>3?atoi(argv[3]):8;
  WNDCLASSA wc={}; wc.lpfnWndProc=DefWindowProcA; wc.hInstance=GetModuleHandleA(0); wc.lpszClassName="b";
  RegisterClassA(&wc);
  char title[128]; sprintf(title,"DXVK bench: %d draws x %d ps-iters",ndraw,iters);
  HWND h=CreateWindowA("b",title,WS_OVERLAPPEDWINDOW,80,80,1024,768,0,0,wc.hInstance,0); ShowWindow(h,SW_SHOW);
  DXGI_SWAP_CHAIN_DESC sd={}; sd.BufferCount=2; sd.BufferDesc.Width=1024; sd.BufferDesc.Height=768;
  sd.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM; sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow=h; sd.SampleDesc.Count=1; sd.Windowed=TRUE; sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
  ID3D11Device*dev=0; ID3D11DeviceContext*ctx=0; IDXGISwapChain*sc=0; D3D_FEATURE_LEVEL fl;
  HRESULT hr=D3D11CreateDeviceAndSwapChain(0,D3D_DRIVER_TYPE_HARDWARE,0,0,0,0,D3D11_SDK_VERSION,&sd,&sc,&dev,&fl,&ctx);
  printf("device hr=0x%08lx FL=0x%x\n",hr,fl); fflush(stdout); if(FAILED(hr))return 1;
  ID3D11Texture2D*bb=0; sc->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&bb);
  ID3D11RenderTargetView*rtv=0; dev->CreateRenderTargetView(bb,0,&rtv);
  ID3DBlob*vb=0,*pb=0,*e=0;
  hr=D3DCompile(SH,strlen(SH),0,0,0,"vs","vs_4_0",0,0,&vb,&e); if(e)printf("%s\n",(char*)e->GetBufferPointer());
  hr|=D3DCompile(SH,strlen(SH),0,0,0,"ps","ps_4_0",0,0,&pb,&e); if(e)printf("%s\n",(char*)e->GetBufferPointer());
  printf("shader compile hr=0x%08lx\n",hr); fflush(stdout); if(FAILED(hr))return 2;
  ID3D11VertexShader*vs=0; dev->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),0,&vs);
  ID3D11PixelShader*ps=0; dev->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),0,&ps);
  D3D11_INPUT_ELEMENT_DESC il[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
    {"COLOR",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0}};
  ID3D11InputLayout*lay=0; dev->CreateInputLayout(il,2,vb->GetBufferPointer(),vb->GetBufferSize(),&lay);
  Vtx v[3]={{0,1.3f,0,1,0,0},{1.3f,-1.0f,0,0,1,0},{-1.3f,-1.0f,0,0,0,1}}; // big tri (overdraw)
  D3D11_BUFFER_DESC bd={sizeof(v),D3D11_USAGE_DEFAULT,D3D11_BIND_VERTEX_BUFFER}; D3D11_SUBRESOURCE_DATA srd={v};
  ID3D11Buffer*vbuf=0; dev->CreateBuffer(&bd,&srd,&vbuf);
  D3D11_BUFFER_DESC cbd={16,D3D11_USAGE_DYNAMIC,D3D11_BIND_CONSTANT_BUFFER,D3D11_CPU_ACCESS_WRITE};
  ID3D11Buffer*cb=0; dev->CreateBuffer(&cbd,0,&cb);
  D3D11_VIEWPORT vp={0,0,1024,768,0,1};
  printf("benchmarking %ds: %d draws/frame, %d ps-iters\n",secs,ndraw,iters); fflush(stdout);
  LARGE_INTEGER fr,t0,t1; QueryPerformanceFrequency(&fr); QueryPerformanceCounter(&t0);
  long long frames=0; double el=0;
  while(1){
    D3D11_MAPPED_SUBRESOURCE m; ctx->Map(cb,0,D3D11_MAP_WRITE_DISCARD,0,&m);
    struct{float a;int it;float p[2];}cbv={(float)(frames*0.01),iters,{0,0}}; *(decltype(cbv)*)m.pData=cbv; ctx->Unmap(cb,0);
    float clr[4]={0,0,0,1}; ctx->ClearRenderTargetView(rtv,clr);
    ctx->RSSetViewports(1,&vp); ctx->OMSetRenderTargets(1,&rtv,0); ctx->IASetInputLayout(lay);
    UINT st=sizeof(Vtx),of=0; ctx->IASetVertexBuffers(0,1,&vbuf,&st,&of);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs,0,0); ctx->VSSetConstantBuffers(0,1,&cb); ctx->PSSetShader(ps,0,0);
    for(int d=0;d<ndraw;d++) ctx->Draw(3,0);
    sc->Present(0,0); frames++;
    MSG ms; while(PeekMessageA(&ms,0,0,0,PM_REMOVE)){TranslateMessage(&ms);DispatchMessageA(&ms);}
    QueryPerformanceCounter(&t1); el=(double)(t1.QuadPart-t0.QuadPart)/fr.QuadPart;
    if(el>=secs) break;
  }
  double fps=frames/el;
  printf("RESULT frames=%lld time=%.2fs FPS=%.1f draws/s=%.0f\n",frames,el,fps,fps*ndraw); fflush(stdout);
  return 0;
}
