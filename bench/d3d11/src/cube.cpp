#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cmath>
#include <cstring>
struct V{float x,y,z,u,v;};
struct M{float m[16];};
static M mul(const M&a,const M&b){M r{};for(int i=0;i<4;i++)for(int j=0;j<4;j++){float s=0;for(int k=0;k<4;k++)s+=a.m[i*4+k]*b.m[k*4+j];r.m[i*4+j]=s;}return r;}
static M ident(){M r{};for(int i=0;i<4;i++)r.m[i*4+i]=1;return r;}
static M rotY(float a){M r=ident();r.m[0]=cosf(a);r.m[2]=-sinf(a);r.m[8]=sinf(a);r.m[10]=cosf(a);return r;}
static M rotX(float a){M r=ident();r.m[5]=cosf(a);r.m[6]=sinf(a);r.m[9]=-sinf(a);r.m[10]=cosf(a);return r;}
static M trans(float x,float y,float z){M r=ident();r.m[12]=x;r.m[13]=y;r.m[14]=z;return r;}
static M persp(float fov,float asp,float n,float f){M r{};float h=1/tanf(fov/2),w=h/asp;r.m[0]=w;r.m[5]=h;r.m[10]=f/(f-n);r.m[11]=1;r.m[14]=-n*f/(f-n);return r;}
int main(){
  int frames=__argc>1?atoi(__argv[1]):3000;
  WNDCLASSA wc={};wc.lpfnWndProc=DefWindowProcA;wc.hInstance=GetModuleHandleA(0);wc.lpszClassName="cube";RegisterClassA(&wc);
  HWND h=CreateWindowA("cube","D3D11 cube + BC1 texture (PowerVR)",WS_OVERLAPPEDWINDOW,100,100,800,600,0,0,wc.hInstance,0);ShowWindow(h,SW_SHOW);
  DXGI_SWAP_CHAIN_DESC sd={};sd.BufferCount=2;sd.BufferDesc.Width=800;sd.BufferDesc.Height=600;sd.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.OutputWindow=h;sd.SampleDesc.Count=1;sd.Windowed=TRUE;sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
  ID3D11Device*dev=0;ID3D11DeviceContext*ctx=0;IDXGISwapChain*sc=0;D3D_FEATURE_LEVEL fl;
  if(FAILED(D3D11CreateDeviceAndSwapChain(0,D3D_DRIVER_TYPE_HARDWARE,0,0,0,0,D3D11_SDK_VERSION,&sd,&sc,&dev,&fl,&ctx)))return 1;
  printf("device FL=0x%x\n",fl);fflush(stdout);
  ID3D11Texture2D*bb=0;sc->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&bb);ID3D11RenderTargetView*rtv=0;dev->CreateRenderTargetView(bb,0,&rtv);
  D3D11_TEXTURE2D_DESC dsd={};dsd.Width=800;dsd.Height=600;dsd.MipLevels=1;dsd.ArraySize=1;dsd.Format=DXGI_FORMAT_D32_FLOAT;dsd.SampleDesc.Count=1;dsd.Usage=D3D11_USAGE_DEFAULT;dsd.BindFlags=D3D11_BIND_DEPTH_STENCIL;
  ID3D11Texture2D*dt=0;dev->CreateTexture2D(&dsd,0,&dt);ID3D11DepthStencilView*dsv=0;dev->CreateDepthStencilView(dt,0,&dsv);
  D3D11_DEPTH_STENCIL_DESC dss={};dss.DepthEnable=TRUE;dss.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;dss.DepthFunc=D3D11_COMPARISON_LESS;ID3D11DepthStencilState*dso=0;dev->CreateDepthStencilState(&dss,&dso);
  // BC1 texture: 8x8 = 2x2 color blocks (red/green/blue/yellow) -- decoded by our DXVK patch
  unsigned char tex[32];auto sb=[&](int bi,unsigned short c0){unsigned char*b=tex+bi*8;b[0]=c0&0xFF;b[1]=c0>>8;b[2]=0;b[3]=0;b[4]=b[5]=b[6]=b[7]=0;};
  sb(0,0xF800);sb(1,0x07E0);sb(2,0x001F);sb(3,0xFFE0);
  D3D11_TEXTURE2D_DESC td={};td.Width=8;td.Height=8;td.MipLevels=1;td.ArraySize=1;td.Format=DXGI_FORMAT_BC1_UNORM;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA tsd={tex,16,32};ID3D11Texture2D*t2=0;HRESULT ht=dev->CreateTexture2D(&td,&tsd,&t2);printf("BC1 cube-tex hr=0x%08lx\n",ht);
  ID3D11ShaderResourceView*srv=0;dev->CreateShaderResourceView(t2,0,&srv);
  D3D11_SAMPLER_DESC smd={};smd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;smd.AddressU=smd.AddressV=smd.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;ID3D11SamplerState*smp=0;dev->CreateSamplerState(&smd,&smp);
  const char*SH="cbuffer C:register(b0){float4x4 mvp;};Texture2D t:register(t0);SamplerState s:register(s0);struct VO{float4 p:SV_POSITION;float2 uv:TEXCOORD;};VO vs(float3 pos:POSITION,float2 uv:TEXCOORD){VO o;o.p=mul(float4(pos,1),mvp);o.uv=uv;return o;}float4 ps(VO i):SV_TARGET{return t.Sample(s,i.uv);}";
  ID3DBlob*vb=0,*pb=0,*e=0;D3DCompile(SH,strlen(SH),0,0,0,"vs","vs_4_0",0,0,&vb,&e);if(e)printf("%s",(char*)e->GetBufferPointer());D3DCompile(SH,strlen(SH),0,0,0,"ps","ps_4_0",0,0,&pb,&e);
  ID3D11VertexShader*vs=0;dev->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),0,&vs);ID3D11PixelShader*ps=0;dev->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),0,&ps);
  D3D11_INPUT_ELEMENT_DESC il[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0}};
  ID3D11InputLayout*lay=0;dev->CreateInputLayout(il,2,vb->GetBufferPointer(),vb->GetBufferSize(),&lay);
  V cv[]={{-1,-1,-1,0,1},{-1,1,-1,0,0},{1,1,-1,1,0},{1,-1,-1,1,1},{1,-1,1,0,1},{1,1,1,0,0},{-1,1,1,1,0},{-1,-1,1,1,1},{-1,-1,1,0,1},{-1,1,1,0,0},{-1,1,-1,1,0},{-1,-1,-1,1,1},{1,-1,-1,0,1},{1,1,-1,0,0},{1,1,1,1,0},{1,-1,1,1,1},{-1,1,-1,0,1},{-1,1,1,0,0},{1,1,1,1,0},{1,1,-1,1,1},{-1,-1,1,0,1},{-1,-1,-1,0,0},{1,-1,-1,1,0},{1,-1,1,1,1}};
  unsigned idx[36];for(int f=0;f<6;f++){int b=f*4;int o[6]={b,b+1,b+2,b,b+2,b+3};for(int k=0;k<6;k++)idx[f*6+k]=o[k];}
  D3D11_BUFFER_DESC vbd={sizeof(cv),D3D11_USAGE_DEFAULT,D3D11_BIND_VERTEX_BUFFER};D3D11_SUBRESOURCE_DATA vsr={cv};ID3D11Buffer*vbuf=0;dev->CreateBuffer(&vbd,&vsr,&vbuf);
  D3D11_BUFFER_DESC ibd={sizeof(idx),D3D11_USAGE_DEFAULT,D3D11_BIND_INDEX_BUFFER};D3D11_SUBRESOURCE_DATA isr={idx};ID3D11Buffer*ibuf=0;dev->CreateBuffer(&ibd,&isr,&ibuf);
  D3D11_BUFFER_DESC cbd={64,D3D11_USAGE_DYNAMIC,D3D11_BIND_CONSTANT_BUFFER,D3D11_CPU_ACCESS_WRITE};ID3D11Buffer*cb=0;dev->CreateBuffer(&cbd,0,&cb);
  printf("cube ready; rendering\n");fflush(stdout);
  for(int i=0;i<frames;i++){
    M mvp=mul(mul(mul(rotX(i*0.011f),rotY(i*0.017f)),trans(0,0,5)),persp(1.0f,800.f/600.f,0.1f,100.f));
    D3D11_MAPPED_SUBRESOURCE m;ctx->Map(cb,0,D3D11_MAP_WRITE_DISCARD,0,&m);memcpy(m.pData,mvp.m,64);ctx->Unmap(cb,0);
    float clr[4]={0.1f,0.1f,0.15f,1};ctx->ClearRenderTargetView(rtv,clr);ctx->ClearDepthStencilView(dsv,D3D11_CLEAR_DEPTH,1.0f,0);
    D3D11_VIEWPORT vp={0,0,800,600,0,1};ctx->RSSetViewports(1,&vp);ctx->OMSetRenderTargets(1,&rtv,dsv);ctx->OMSetDepthStencilState(dso,0);
    ctx->IASetInputLayout(lay);UINT st=sizeof(V),of=0;ctx->IASetVertexBuffers(0,1,&vbuf,&st,&of);ctx->IASetIndexBuffer(ibuf,DXGI_FORMAT_R32_UINT,0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->VSSetShader(vs,0,0);ctx->VSSetConstantBuffers(0,1,&cb);ctx->PSSetShader(ps,0,0);ctx->PSSetShaderResources(0,1,&srv);ctx->PSSetSamplers(0,1,&smp);
    ctx->DrawIndexed(36,0,0);sc->Present(1,0);
    MSG ms;while(PeekMessageA(&ms,0,0,0,PM_REMOVE)){TranslateMessage(&ms);DispatchMessageA(&ms);}
  }
  printf("CUBE_DONE\n");fflush(stdout);return 0;
}
