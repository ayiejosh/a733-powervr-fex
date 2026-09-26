#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cmath>
struct Vtx{ float x,y,z; float r,g,b; };
const char* SHADER=
"cbuffer C:register(b0){float a;};\n"
"struct VO{float4 p:SV_POSITION;float3 c:COLOR;};\n"
"VO vs(float3 pos:POSITION,float3 col:COLOR){VO o;\n"
" float s=sin(a),co=cos(a);\n"
" float2 r=float2(pos.x*co-pos.y*s,pos.x*s+pos.y*co);\n"
" o.p=float4(r,0,1);o.c=col;return o;}\n"
"float4 ps(VO i):SV_TARGET{return float4(i.c,1);}\n";
int main(){
  WNDCLASSA wc={}; wc.lpfnWndProc=DefWindowProcA; wc.hInstance=GetModuleHandleA(0); wc.lpszClassName="tri";
  RegisterClassA(&wc);
  HWND h=CreateWindowA("tri","D3D11 spinning triangle (PowerVR)",WS_OVERLAPPEDWINDOW,150,150,640,480,0,0,wc.hInstance,0);
  ShowWindow(h,SW_SHOW);
  DXGI_SWAP_CHAIN_DESC sd={}; sd.BufferCount=2; sd.BufferDesc.Width=640; sd.BufferDesc.Height=480;
  sd.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM; sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow=h; sd.SampleDesc.Count=1; sd.Windowed=TRUE; sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
  ID3D11Device*dev=0; ID3D11DeviceContext*ctx=0; IDXGISwapChain*sc=0; D3D_FEATURE_LEVEL fl;
  HRESULT hr=D3D11CreateDeviceAndSwapChain(0,D3D_DRIVER_TYPE_HARDWARE,0,0,0,0,D3D11_SDK_VERSION,&sd,&sc,&dev,&fl,&ctx);
  printf("device hr=0x%08lx FL=0x%x\n",hr,fl); fflush(stdout); if(FAILED(hr))return 1;
  ID3D11Texture2D*bb=0; sc->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&bb);
  ID3D11RenderTargetView*rtv=0; dev->CreateRenderTargetView(bb,0,&rtv);
  ID3DBlob*vb=0,*pb=0,*err=0;
  hr=D3DCompile(SHADER,strlen(SHADER),0,0,0,"vs","vs_4_0",0,0,&vb,&err);
  printf("VS compile hr=0x%08lx\n",hr); if(err){printf("%s\n",(char*)err->GetBufferPointer());} fflush(stdout); if(FAILED(hr))return 2;
  D3DCompile(SHADER,strlen(SHADER),0,0,0,"ps","ps_4_0",0,0,&pb,0);
  ID3D11VertexShader*vs=0; dev->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),0,&vs);
  ID3D11PixelShader*ps=0; dev->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),0,&ps);
  D3D11_INPUT_ELEMENT_DESC il[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
    {"COLOR",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0}};
  ID3D11InputLayout*lay=0; dev->CreateInputLayout(il,2,vb->GetBufferPointer(),vb->GetBufferSize(),&lay);
  Vtx v[3]={{0,0.6f,0,1,0,0},{0.6f,-0.5f,0,0,1,0},{-0.6f,-0.5f,0,0,0,1}};
  D3D11_BUFFER_DESC bd={sizeof(v),D3D11_USAGE_DEFAULT,D3D11_BIND_VERTEX_BUFFER}; D3D11_SUBRESOURCE_DATA srd={v};
  ID3D11Buffer*vbuf=0; dev->CreateBuffer(&bd,&srd,&vbuf);
  D3D11_BUFFER_DESC cbd={16,D3D11_USAGE_DYNAMIC,D3D11_BIND_CONSTANT_BUFFER,D3D11_CPU_ACCESS_WRITE};
  ID3D11Buffer*cb=0; dev->CreateBuffer(&cbd,0,&cb);
  printf("pipeline ready; rendering\n"); fflush(stdout);
  for(int i=0;i<6000;i++){
    D3D11_MAPPED_SUBRESOURCE m; ctx->Map(cb,0,D3D11_MAP_WRITE_DISCARD,0,&m); float a=i*0.03f; ((float*)m.pData)[0]=a; ctx->Unmap(cb,0);
    float clr[4]={0.05f,0.05f,0.1f,1}; ctx->ClearRenderTargetView(rtv,clr);
    D3D11_VIEWPORT vp={0,0,640,480,0,1}; ctx->RSSetViewports(1,&vp);
    ctx->OMSetRenderTargets(1,&rtv,0); ctx->IASetInputLayout(lay);
    UINT st=sizeof(Vtx),of=0; ctx->IASetVertexBuffers(0,1,&vbuf,&st,&of);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs,0,0); ctx->VSSetConstantBuffers(0,1,&cb); ctx->PSSetShader(ps,0,0);
    ctx->Draw(3,0); sc->Present(1,0);
    MSG ms; while(PeekMessageA(&ms,0,0,0,PM_REMOVE)){TranslateMessage(&ms);DispatchMessageA(&ms);} Sleep(16);
  }
  printf("RENDERED_6000_FRAMES_OK\n"); fflush(stdout); return 0;
}
