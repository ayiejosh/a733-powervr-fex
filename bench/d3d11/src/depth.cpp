#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
struct V{float x,y,z; float r,g,b;};
const char* SH=
"struct VO{float4 p:SV_POSITION;float3 c:COLOR;};\n"
"VO vs(float3 pos:POSITION,float3 col:COLOR){VO o;o.p=float4(pos,1);o.c=col;return o;}\n"
"float4 ps(VO i):SV_TARGET{return float4(i.c,1);}\n";
int main(){
  ID3D11Device*dev=0;ID3D11DeviceContext*ctx=0;D3D_FEATURE_LEVEL fl;
  if(FAILED(D3D11CreateDevice(0,D3D_DRIVER_TYPE_HARDWARE,0,0,0,0,D3D11_SDK_VERSION,&dev,&fl,&ctx))){printf("dev FAIL\n");return 1;}
  const int W=256,H=256;
  D3D11_TEXTURE2D_DESC rd={};rd.Width=W;rd.Height=H;rd.MipLevels=1;rd.ArraySize=1;rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;rd.SampleDesc.Count=1;rd.Usage=D3D11_USAGE_DEFAULT;rd.BindFlags=D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D*rt=0;dev->CreateTexture2D(&rd,0,&rt);ID3D11RenderTargetView*rtv=0;dev->CreateRenderTargetView(rt,0,&rtv);
  D3D11_TEXTURE2D_DESC dd={};dd.Width=W;dd.Height=H;dd.MipLevels=1;dd.ArraySize=1;dd.Format=DXGI_FORMAT_D32_FLOAT;dd.SampleDesc.Count=1;dd.Usage=D3D11_USAGE_DEFAULT;dd.BindFlags=D3D11_BIND_DEPTH_STENCIL;
  ID3D11Texture2D*dt=0;HRESULT h=dev->CreateTexture2D(&dd,0,&dt);printf("depthtex hr=0x%08lx\n",h);
  ID3D11DepthStencilView*dsv=0;h=dev->CreateDepthStencilView(dt,0,&dsv);printf("DSV hr=0x%08lx\n",h);
  D3D11_DEPTH_STENCIL_DESC dsd={};dsd.DepthEnable=TRUE;dsd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;dsd.DepthFunc=D3D11_COMPARISON_LESS;
  ID3D11DepthStencilState*dss=0;dev->CreateDepthStencilState(&dsd,&dss);ctx->OMSetDepthStencilState(dss,0);
  ID3DBlob*vb=0,*pb=0,*e=0;D3DCompile(SH,strlen(SH),0,0,0,"vs","vs_4_0",0,0,&vb,&e);D3DCompile(SH,strlen(SH),0,0,0,"ps","ps_4_0",0,0,&pb,&e);
  ID3D11VertexShader*vs=0;dev->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),0,&vs);
  ID3D11PixelShader*ps=0;dev->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),0,&ps);
  D3D11_INPUT_ELEMENT_DESC il[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"COLOR",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0}};
  ID3D11InputLayout*lay=0;dev->CreateInputLayout(il,2,vb->GetBufferPointer(),vb->GetBufferSize(),&lay);
  // NEAR blue quad z=0.3, FAR red quad z=0.8 (both cover center). Draw NEAR first, then FAR -> depth must reject FAR.
  V near_[6]={{-0.8f,-0.8f,0.3f,0,0,1},{-0.8f,0.8f,0.3f,0,0,1},{0.8f,0.8f,0.3f,0,0,1},{-0.8f,-0.8f,0.3f,0,0,1},{0.8f,0.8f,0.3f,0,0,1},{0.8f,-0.8f,0.3f,0,0,1}};
  V far_[6]; for(int i=0;i<6;i++){far_[i]=near_[i];far_[i].z=0.8f;far_[i].r=1;far_[i].g=0;far_[i].b=0;}
  auto mk=[&](V*d){D3D11_BUFFER_DESC b={sizeof(V)*6,D3D11_USAGE_DEFAULT,D3D11_BIND_VERTEX_BUFFER};D3D11_SUBRESOURCE_DATA s={d};ID3D11Buffer*bf=0;dev->CreateBuffer(&b,&s,&bf);return bf;};
  ID3D11Buffer*nb=mk(near_),*fb=mk(far_);
  float clr[4]={0,0,0,1};ctx->ClearRenderTargetView(rtv,clr);ctx->ClearDepthStencilView(dsv,D3D11_CLEAR_DEPTH,1.0f,0);
  D3D11_VIEWPORT vp={0,0,W,H,0,1};ctx->RSSetViewports(1,&vp);ctx->OMSetRenderTargets(1,&rtv,dsv);ctx->IASetInputLayout(lay);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->VSSetShader(vs,0,0);ctx->PSSetShader(ps,0,0);
  UINT st=sizeof(V),of=0;
  ctx->IASetVertexBuffers(0,1,&nb,&st,&of);ctx->Draw(6,0); // near (blue) first
  ctx->IASetVertexBuffers(0,1,&fb,&st,&of);ctx->Draw(6,0); // far (red) second -> should be depth-rejected
  D3D11_TEXTURE2D_DESC sd=rd;sd.BindFlags=0;sd.Usage=D3D11_USAGE_STAGING;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D*stg=0;dev->CreateTexture2D(&sd,0,&stg);ctx->CopyResource(stg,rt);
  D3D11_MAPPED_SUBRESOURCE m;if(FAILED(ctx->Map(stg,0,D3D11_MAP_READ,0,&m))){printf("map FAIL\n");return 2;}
  unsigned char*c=(unsigned char*)m.pData+128*m.RowPitch+128*4;
  printf("center RGBA=%d,%d,%d (expect BLUE if depth works, red if not)\n",c[0],c[1],c[2]);
  int ok=(c[2]>200&&c[0]<50);ctx->Unmap(stg,0);printf(ok?"DEPTH_OK near occludes far\n":"DEPTH_FAIL\n");return ok?0:3;
}
