#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
struct V{float x,y; float r,g,b;};
const char* SH=
"struct VO{float4 p:SV_POSITION;float3 c:COLOR;};\n"
"VO vs(float2 pos:POSITION,float3 col:COLOR){VO o;o.p=float4(pos,0,1);o.c=col;return o;}\n"
"float4 ps(VO i):SV_TARGET{return float4(i.c,1);}\n";
int main(){
  ID3D11Device*dev=0; ID3D11DeviceContext*ctx=0; D3D_FEATURE_LEVEL fl;
  HRESULT hr=D3D11CreateDevice(0,D3D_DRIVER_TYPE_HARDWARE,0,0,0,0,D3D11_SDK_VERSION,&dev,&fl,&ctx);
  printf("device hr=0x%08lx\n",hr); fflush(stdout); if(FAILED(hr))return 1;
  const int W=256,H=256;
  D3D11_TEXTURE2D_DESC td={}; td.Width=W; td.Height=H; td.MipLevels=1; td.ArraySize=1;
  td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count=1; td.Usage=D3D11_USAGE_DEFAULT;
  td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
  ID3D11Texture2D*tex=0; hr=dev->CreateTexture2D(&td,0,&tex); printf("offscreen tex hr=0x%08lx\n",hr);
  ID3D11RenderTargetView*rtv=0; hr=dev->CreateRenderTargetView(tex,0,&rtv); printf("RTV hr=0x%08lx\n",hr);
  ID3DBlob*vb=0,*pb=0,*e=0;
  D3DCompile(SH,strlen(SH),0,0,0,"vs","vs_4_0",0,0,&vb,&e); if(e)printf("%s\n",(char*)e->GetBufferPointer());
  D3DCompile(SH,strlen(SH),0,0,0,"ps","ps_4_0",0,0,&pb,&e); if(e)printf("%s\n",(char*)e->GetBufferPointer());
  ID3D11VertexShader*vs=0; dev->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),0,&vs);
  ID3D11PixelShader*ps=0; dev->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),0,&ps);
  D3D11_INPUT_ELEMENT_DESC il[]={{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
    {"COLOR",0,DXGI_FORMAT_R32G32B32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0}};
  ID3D11InputLayout*lay=0; dev->CreateInputLayout(il,2,vb->GetBufferPointer(),vb->GetBufferSize(),&lay);
  V v[3]={{0,0.7f,1,1,1},{0.7f,-0.7f,1,1,1},{-0.7f,-0.7f,1,1,1}}; // white tri
  D3D11_BUFFER_DESC bd={sizeof(v),D3D11_USAGE_DEFAULT,D3D11_BIND_VERTEX_BUFFER}; D3D11_SUBRESOURCE_DATA srd={v};
  ID3D11Buffer*vbuf=0; dev->CreateBuffer(&bd,&srd,&vbuf);
  float clr[4]={0.2f,0.0f,0.0f,1}; ctx->ClearRenderTargetView(rtv,clr); // dark red bg
  D3D11_VIEWPORT vp={0,0,W,H,0,1}; ctx->RSSetViewports(1,&vp);
  ctx->OMSetRenderTargets(1,&rtv,0); ctx->IASetInputLayout(lay);
  UINT st=sizeof(V),of=0; ctx->IASetVertexBuffers(0,1,&vbuf,&st,&of);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(vs,0,0); ctx->PSSetShader(ps,0,0); ctx->Draw(3,0);
  // readback
  D3D11_TEXTURE2D_DESC sd=td; sd.BindFlags=0; sd.Usage=D3D11_USAGE_STAGING; sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D*stg=0; dev->CreateTexture2D(&sd,0,&stg); ctx->CopyResource(stg,tex);
  D3D11_MAPPED_SUBRESOURCE m; hr=ctx->Map(stg,0,D3D11_MAP_READ,0,&m);
  printf("Map hr=0x%08lx\n",hr); fflush(stdout); if(FAILED(hr))return 2;
  unsigned char* p=(unsigned char*)m.pData;
  auto px=[&](int x,int y){ return p+y*m.RowPitch+x*4; };
  unsigned char* c=px(128,150); unsigned char* corner=px(5,5);
  printf("center px RGBA=%d,%d,%d,%d (expect ~white tri)\n",c[0],c[1],c[2],c[3]);
  printf("corner px RGBA=%d,%d,%d,%d (expect dark-red bg)\n",corner[0],corner[1],corner[2],corner[3]);
  int ok=(c[0]>200&&c[1]>200&&c[2]>200)&&(corner[0]>30&&corner[1]<30&&corner[2]<30);
  ctx->Unmap(stg,0);
  printf(ok?"RTT_OK render-to-texture + readback correct\n":"RTT_FAIL\n"); fflush(stdout);
  return ok?0:3;
}
