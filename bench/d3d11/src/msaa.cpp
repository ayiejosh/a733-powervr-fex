#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
struct V{float x,y;};
const char* SH=
"float4 vs(float2 p:POSITION):SV_POSITION{return float4(p,0,1);}\n"
"float4 ps():SV_TARGET{return float4(1,1,0,1);}\n";
int main(){ID3D11Device*dev=0;ID3D11DeviceContext*ctx=0;D3D_FEATURE_LEVEL fl;
 if(FAILED(D3D11CreateDevice(0,D3D_DRIVER_TYPE_HARDWARE,0,0,0,0,D3D11_SDK_VERSION,&dev,&fl,&ctx)))return 1;
 UINT q=0;HRESULT hq=dev->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM,4,&q);printf("MSAA4x quality levels=%u (hr=0x%08lx)\n",q,hq);
 const int W=64,H=64;
 D3D11_TEXTURE2D_DESC md={};md.Width=W;md.Height=H;md.MipLevels=1;md.ArraySize=1;md.Format=DXGI_FORMAT_R8G8B8A8_UNORM;md.SampleDesc.Count=4;md.Usage=D3D11_USAGE_DEFAULT;md.BindFlags=D3D11_BIND_RENDER_TARGET;
 ID3D11Texture2D*mt=0;HRESULT h=dev->CreateTexture2D(&md,0,&mt);printf("MSAA tex hr=0x%08lx\n",h);if(FAILED(h))return 2;
 ID3D11RenderTargetView*rtv=0;h=dev->CreateRenderTargetView(mt,0,&rtv);printf("MSAA RTV hr=0x%08lx\n",h);
 D3D11_TEXTURE2D_DESC rd=md;rd.SampleDesc.Count=1;ID3D11Texture2D*rt=0;dev->CreateTexture2D(&rd,0,&rt);
 ID3DBlob*vb=0,*pb=0,*e=0;D3DCompile(SH,strlen(SH),0,0,0,"vs","vs_4_0",0,0,&vb,&e);D3DCompile(SH,strlen(SH),0,0,0,"ps","ps_4_0",0,0,&pb,&e);
 ID3D11VertexShader*vs=0;dev->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),0,&vs);
 ID3D11PixelShader*ps=0;dev->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),0,&ps);
 D3D11_INPUT_ELEMENT_DESC il[]={{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0}};
 ID3D11InputLayout*lay=0;dev->CreateInputLayout(il,1,vb->GetBufferPointer(),vb->GetBufferSize(),&lay);
 V v[3]={{0,0.8f},{0.8f,-0.8f},{-0.8f,-0.8f}};D3D11_BUFFER_DESC bd={sizeof(v),D3D11_USAGE_DEFAULT,D3D11_BIND_VERTEX_BUFFER};D3D11_SUBRESOURCE_DATA s={v};ID3D11Buffer*vbuf=0;dev->CreateBuffer(&bd,&s,&vbuf);
 float clr[4]={0,0,0.3f,1};ctx->ClearRenderTargetView(rtv,clr);D3D11_VIEWPORT vp={0,0,W,H,0,1};ctx->RSSetViewports(1,&vp);
 ctx->OMSetRenderTargets(1,&rtv,0);ctx->IASetInputLayout(lay);UINT st=sizeof(V),of=0;ctx->IASetVertexBuffers(0,1,&vbuf,&st,&of);
 ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->VSSetShader(vs,0,0);ctx->PSSetShader(ps,0,0);ctx->Draw(3,0);
 ctx->ResolveSubresource(rt,0,mt,0,DXGI_FORMAT_R8G8B8A8_UNORM);
 D3D11_TEXTURE2D_DESC sd=rd;sd.BindFlags=0;sd.Usage=D3D11_USAGE_STAGING;sd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ID3D11Texture2D*stg=0;dev->CreateTexture2D(&sd,0,&stg);ctx->CopyResource(stg,rt);
 D3D11_MAPPED_SUBRESOURCE m;if(FAILED(ctx->Map(stg,0,D3D11_MAP_READ,0,&m)))return 3;unsigned char*c=(unsigned char*)m.pData+40*m.RowPitch+32*4;
 printf("center RGB=%d,%d,%d (want yellow tri)\n",c[0],c[1],c[2]);int ok=(c[0]>200&&c[1]>200&&c[2]<60);ctx->Unmap(stg,0);
 printf(ok?"MSAA_OK 4x render+resolve works\n":"MSAA_FAIL\n");return ok?0:4;}
