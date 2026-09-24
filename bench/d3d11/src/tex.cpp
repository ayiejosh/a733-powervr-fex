#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
struct V{float x,y; float u,v;};
const char* SH=
"Texture2D t:register(t0);SamplerState s:register(s0);\n"
"struct VO{float4 p:SV_POSITION;float2 uv:TEXCOORD;};\n"
"VO vs(float2 pos:POSITION,float2 uv:TEXCOORD){VO o;o.p=float4(pos,0,1);o.uv=uv;return o;}\n"
"float4 ps(VO i):SV_TARGET{return t.Sample(s,i.uv);}\n";
int main(){
  ID3D11Device*dev=0;ID3D11DeviceContext*ctx=0;D3D_FEATURE_LEVEL fl;
  if(FAILED(D3D11CreateDevice(0,D3D_DRIVER_TYPE_HARDWARE,0,0,0,0,D3D11_SDK_VERSION,&dev,&fl,&ctx))){printf("dev FAIL\n");return 1;}
  const int W=256,H=256;
  // offscreen RT
  D3D11_TEXTURE2D_DESC rd={};rd.Width=W;rd.Height=H;rd.MipLevels=1;rd.ArraySize=1;rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;rd.SampleDesc.Count=1;rd.Usage=D3D11_USAGE_DEFAULT;rd.BindFlags=D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D*rt=0;dev->CreateTexture2D(&rd,0,&rt);ID3D11RenderTargetView*rtv=0;dev->CreateRenderTargetView(rt,0,&rtv);
  // procedural 8x8 checkerboard texture (red/blue)
  unsigned px[64];for(int y=0;y<8;y++)for(int x=0;x<8;x++)px[y*8+x]=((x^y)&1)?0xFF0000FFu:0xFFFF0000u;
  D3D11_TEXTURE2D_DESC td={};td.Width=8;td.Height=8;td.MipLevels=1;td.ArraySize=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA tsd={px,8*4,0};ID3D11Texture2D*tex=0;HRESULT h=dev->CreateTexture2D(&td,&tsd,&tex);printf("tex hr=0x%08lx\n",h);
  ID3D11ShaderResourceView*srv=0;h=dev->CreateShaderResourceView(tex,0,&srv);printf("SRV hr=0x%08lx\n",h);
  D3D11_SAMPLER_DESC smd={};smd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;smd.AddressU=smd.AddressV=smd.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;
  ID3D11SamplerState*smp=0;dev->CreateSamplerState(&smd,&smp);
  ID3DBlob*vb=0,*pb=0,*e=0;D3DCompile(SH,strlen(SH),0,0,0,"vs","vs_4_0",0,0,&vb,&e);if(e)printf("%s\n",(char*)e->GetBufferPointer());
  D3DCompile(SH,strlen(SH),0,0,0,"ps","ps_4_0",0,0,&pb,&e);if(e)printf("%s\n",(char*)e->GetBufferPointer());
  ID3D11VertexShader*vs=0;dev->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),0,&vs);
  ID3D11PixelShader*ps=0;dev->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),0,&ps);
  D3D11_INPUT_ELEMENT_DESC il[]={{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0}};
  ID3D11InputLayout*lay=0;dev->CreateInputLayout(il,2,vb->GetBufferPointer(),vb->GetBufferSize(),&lay);
  V v[6]={{-1,-1,0,1},{-1,1,0,0},{1,1,1,0},{-1,-1,0,1},{1,1,1,0},{1,-1,1,1}}; // fullscreen quad
  D3D11_BUFFER_DESC bd={sizeof(v),D3D11_USAGE_DEFAULT,D3D11_BIND_VERTEX_BUFFER};D3D11_SUBRESOURCE_DATA sr={v};
  ID3D11Buffer*vbuf=0;dev->CreateBuffer(&bd,&sr,&vbuf);
  float clr[4]={0,0,0,1};ctx->ClearRenderTargetView(rtv,clr);D3D11_VIEWPORT vp={0,0,W,H,0,1};ctx->RSSetViewports(1,&vp);
  ctx->OMSetRenderTargets(1,&rtv,0);ctx->IASetInputLayout(lay);UINT st=sizeof(V),of=0;ctx->IASetVertexBuffers(0,1,&vbuf,&st,&of);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->VSSetShader(vs,0,0);ctx->PSSetShader(ps,0,0);
  ctx->PSSetShaderResources(0,1,&srv);ctx->PSSetSamplers(0,1,&smp);ctx->Draw(6,0);
  D3D11_TEXTURE2D_DESC sd=rd;sd.BindFlags=0;sd.Usage=D3D11_USAGE_STAGING;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D*stg=0;dev->CreateTexture2D(&sd,0,&stg);ctx->CopyResource(stg,rt);
  D3D11_MAPPED_SUBRESOURCE m;if(FAILED(ctx->Map(stg,0,D3D11_MAP_READ,0,&m))){printf("map FAIL\n");return 2;}
  unsigned char*p=(unsigned char*)m.pData;
  auto P=[&](int x,int y){return p+y*m.RowPitch+x*4;};
  unsigned char*a=P(16,16);unsigned char*b=P(48,16);// two adjacent checker cells (256px/8=32px cells)
  printf("cell(16,16) RGBA=%d,%d,%d cell(48,16) RGBA=%d,%d,%d\n",a[0],a[1],a[2],b[0],b[1],b[2]);
  int ok=((a[0]>200&&a[2]<50)||(a[2]>200&&a[0]<50)) && ((b[0]>200&&b[2]<50)||(b[2]>200&&b[0]<50)) && (a[0]!=b[0]);
  ctx->Unmap(stg,0);printf(ok?"TEXTURE_OK procedural checkerboard sampled\n":"TEXTURE_FAIL\n");return ok?0:3;
}
