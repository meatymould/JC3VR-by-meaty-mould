#pragma once
#include "render/wrist_hud.h"
#include "render/hud_composite.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cassert>
#include <fstream>
#include <cstdlib>
inline void TestWristHudGpu() {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)));
    ComPtr<ID3DBlob> vs,ps,errors;
    assert(SUCCEEDED(D3DCompile(jc3vr::kWristHudShader,sizeof(jc3vr::kWristHudShader)-1,"wrist_gpu",nullptr,nullptr,"VS","vs_5_0",0,0,&vs,&errors)));
    assert(SUCCEEDED(D3DCompile(jc3vr::kWristHudShader,sizeof(jc3vr::kWristHudShader)-1,"wrist_gpu",nullptr,nullptr,"PS","ps_5_0",0,0,&ps,&errors)));
    ComPtr<ID3D11VertexShader> vertex;ComPtr<ID3D11PixelShader> pixel;ComPtr<ID3D11Buffer> constants;
    assert(SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex)));
    assert(SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel)));
    D3D11_BUFFER_DESC cb{};cb.ByteWidth=112;cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    assert(SUCCEEDED(device->CreateBuffer(&cb,nullptr,&constants)));
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=256;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target,readback;ComPtr<ID3D11RenderTargetView> rtv;
    assert(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&target)));
    assert(SUCCEEDED(device->CreateRenderTargetView(target.Get(),nullptr,&rtv)));
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    assert(SUCCEEDED(device->CreateTexture2D(&td,nullptr,&readback)));
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;assert(SUCCEEDED(device->CreateRasterizerState(&rd,&raster)));
    D3D11_VIEWPORT viewport{0,0,256,256,0,1};context->RSSetViewports(1,&viewport);context->RSSetState(raster.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex.Get(),nullptr,0);context->PSSetShader(pixel.Get(),nullptr,0);
    auto* buffer=constants.Get();auto* view=rtv.Get();context->VSSetConstantBuffers(0,1,&buffer);context->PSSetConstantBuffers(0,1,&buffer);
    float values[28]{0,0,-.5f,0,1,0,0,0,0,1,0,1,-1,1,1,-1,25,360,.75f,1,.09f,.055f,0,0,0,0,1,1};
    const auto draw=[&](float cameraX,float cameraY,bool left,bool checkText=true,bool expectEmpty=false) {
        values[0]=-cameraX;values[1]=-cameraY;values[19]=left?0.f:1.f;
        const float red[4]{1,0,0,1};context->ClearRenderTargetView(view,red);context->OMSetRenderTargets(1,&view,nullptr);
        context->UpdateSubresource(buffer,0,nullptr,values,0,0);context->Draw(6,0);
        context->OMSetRenderTargets(0,nullptr,nullptr);context->CopyResource(readback.Get(),target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};assert(SUCCEEDED(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped)));
        double sum=0;unsigned count=0,bright=0;
        for(unsigned y=0;y<256;++y) for(unsigned x=0;x<256;++x) {
            const auto* p=static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch+4*x;
            if(p[0]==255 && p[1]==0 && p[2]==0) continue;
            sum+=x;++count;if(p[0]>150 && p[1]>150 && p[2]>150) ++bright;
        }
        context->Unmap(readback.Get(),0);
        const unsigned requiredInk=(left?values[18]:values[16])<0?8:20; // dashes occupy less ink than numbers
        if(expectEmpty) {assert(count==0);return 127.5;}
        // Transparent UI measures artwork ink, NOT an opaque card footprint.
        if(count<=30 || (checkText && bright<=requiredInk)) std::cerr<<"wrist pixels="<<count<<" text="<<bright<<" left="<<left<<" value="<<values[16]<<"\n";
        assert(count>30 && (!checkText || bright>requiredInk));return sum/count;
    };
    const auto l=draw(-.032f,0,false),r=draw(.032f,0,false);
    assert(l>r && std::abs((l-r)-16.384)<2); // one physical anchor, genuine per-eye parallax
    const auto neutralCentre=draw(0,0,false);
    values[12]=-.8f;values[13]=1.1f;values[14]=.9f;values[15]=-.7f;
    assert(std::abs(draw(0,0,false)-neutralCentre+128*.3/1.9)<2); // compare the SAME floating artwork, not an opaque-card centroid
    values[12]=-1;values[13]=1;values[14]=1;values[15]=-1;
    draw(0,0,true);values[16]=values[17]=values[18]=-1;draw(0,0,false);draw(0,0,true); // unavailable != fabricated zero
    // A square native icon stays square inside the wide header region.
    // Test raster pixels, not just the algebra or successful shader compile.
    D3D11_TEXTURE2D_DESC iconDesc{};iconDesc.Width=iconDesc.Height=1;
    iconDesc.MipLevels=iconDesc.ArraySize=iconDesc.SampleDesc.Count=1;
    iconDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;iconDesc.Usage=D3D11_USAGE_IMMUTABLE;
    iconDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    const unsigned white=0xffffffff;D3D11_SUBRESOURCE_DATA iconData{&white,4,0};
    ComPtr<ID3D11Texture2D> iconTexture;ComPtr<ID3D11ShaderResourceView> iconView;
    assert(SUCCEEDED(device->CreateTexture2D(&iconDesc,&iconData,&iconTexture)));
    assert(SUCCEEDED(device->CreateShaderResourceView(iconTexture.Get(),nullptr,&iconView)));
    D3D11_SAMPLER_DESC samplerDesc{};samplerDesc.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU=samplerDesc.AddressV=samplerDesc.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    ComPtr<ID3D11SamplerState> sampler;assert(SUCCEEDED(device->CreateSamplerState(&samplerDesc,&sampler)));
    auto* srv=iconView.Get();auto* sampling=sampler.Get();
    context->PSSetShaderResources(0,1,&srv);context->PSSetSamplers(0,1,&sampling);
    values[22]=values[23]=1;draw(0,0,false);
    D3D11_MAPPED_SUBRESOURCE pixels{};assert(SUCCEEDED(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&pixels)));
    int minX=256,maxX=-1,minY=256,maxY=-1;
    // Header lies above the digit region. Exclude digits and coloured border.
    for(int y=116;y<125;++y) for(int x=100;x<156;++x) {
        const auto* p=static_cast<const unsigned char*>(pixels.pData)+y*pixels.RowPitch+4*x;
        if(p[0]>250 && p[1]>250 && p[2]>250) {
            minX=std::min(minX,x);maxX=std::max(maxX,x);minY=std::min(minY,y);maxY=std::max(maxY,y);
        }
    }
    context->Unmap(readback.Get(),0);
    assert(maxX>=minX && maxY>=minY && std::abs((maxX-minX)-(maxY-minY))<=1);
    // Circle is measured in physical card coordinates, not rectangular UVs.
    // Compare the top arc for full/empty magazines and unknown capacity.
    values[22]=0;values[16]=14;values[17]=66;values[18]=14;
    const auto topArc=[&]() {
        draw(0,0,false,false); // this assertion measures the arc, not digit ink
        D3D11_MAPPED_SUBRESOURCE m{};assert(SUCCEEDED(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&m)));
        unsigned bright=0;
        for(unsigned y=114;y<118;++y) for(unsigned x=125;x<131;++x) {
            const auto* p=static_cast<const unsigned char*>(m.pData)+y*m.RowPitch+x*4;
            if(p[0]>180 && p[1]>180 && p[2]>180) ++bright;
        }
        context->Unmap(readback.Get(),0);return bright;
    };
    const auto full=topArc();values[16]=0;const auto empty=topArc();
    values[16]=14;values[18]=-1;const auto unknown=topArc();
    assert(full>0 && empty==0 && unknown==0); // no fabricated progress
    // Optional local-only native artwork for visual QA, never a test fixture
    // or a redistributed asset. Normal CTest remains independent of the game.
    if(const auto* iconPath=_wgetenv(L"JC3VR_WRIST_PREVIEW_ICON")) {
        std::vector<std::uint8_t> rgba;unsigned width=0,height=0;
        assert(jc3vr::LoadRgbaPng(iconPath,rgba,width,height) && width && height);
        iconDesc.Width=width;iconDesc.Height=height;
        iconData={rgba.data(),width*4,0};iconTexture.Reset();iconView.Reset();
        assert(SUCCEEDED(device->CreateTexture2D(&iconDesc,&iconData,&iconTexture)));
        assert(SUCCEEDED(device->CreateShaderResourceView(iconTexture.Get(),nullptr,&iconView)));
        srv=iconView.Get();context->PSSetShaderResources(0,1,&srv);
        values[22]=1;values[23]=static_cast<float>(width)/height;
    }
    // Diagnostic image of the actual compiled shader, generated in test cwd.
    values[2]=-.20f;values[16]=5;values[17]=360;values[18]=30;draw(0,0,false);
    D3D11_MAPPED_SUBRESOURCE preview{};assert(SUCCEEDED(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&preview)));
    std::ofstream bitmap("wrist_hud_preview.bmp",std::ios::binary);
    unsigned char header[54]{};header[0]='B';header[1]='M';
    const auto put32=[&](unsigned offset,unsigned value){std::memcpy(header+offset,&value,4);};
    put32(2,54+256*256*4);put32(10,54);put32(14,40);put32(18,256);put32(22,256);
    header[26]=1;header[28]=32;put32(34,256*256*4);
    bitmap.write(reinterpret_cast<const char*>(header),sizeof(header));
    for(int y=255;y>=0;--y) for(unsigned x=0;x<256;++x) {
        const auto* p=static_cast<const unsigned char*>(preview.pData)+y*preview.RowPitch+4*x;
        const unsigned char bgra[]{p[2],p[1],p[0],255};bitmap.write(reinterpret_cast<const char*>(bgra),4);
    }
    context->Unmap(readback.Get(),0);
    // Native status relay: independent source regions, correct hand ownership,
    // and unchanged scene suppression. This tests actual GPU pixels, not just
    // compile success. Synthetic fixtures contain NO redistributed game art.
    D3D11_TEXTURE2D_DESC nativeDesc=iconDesc;nativeDesc.Width=4;nativeDesc.Height=2;
    const unsigned nativePixels[8]{0xff00ff00,0xff00ff00,0xffff0000,0xffff0000,
                                   0xff00ff00,0xff00ff00,0xffff0000,0xffff0000};
    const unsigned plainPixels[8]{};
    D3D11_SUBRESOURCE_DATA nativeData{nativePixels,16,0},plainData{plainPixels,16,0};
    ComPtr<ID3D11Texture2D> nativeTexture,plainTexture;
    ComPtr<ID3D11ShaderResourceView> nativeView,plainView;
    assert(SUCCEEDED(device->CreateTexture2D(&nativeDesc,&nativeData,&nativeTexture)));
    assert(SUCCEEDED(device->CreateTexture2D(&nativeDesc,&plainData,&plainTexture)));
    assert(SUCCEEDED(device->CreateShaderResourceView(nativeTexture.Get(),nullptr,&nativeView)));
    assert(SUCCEEDED(device->CreateShaderResourceView(plainTexture.Get(),nullptr,&plainView)));
    ID3D11ShaderResourceView* nativeViews[2]{nativeView.Get(),plainView.Get()};
    context->PSSetShaderResources(1,2,nativeViews);
    values[2]=-.5f; // restore measured half-metre fixture after close-up preview
    values[3]=1;values[7]=3.f/255.f;values[11]=1;values[24]=.05f;values[25]=.05f;values[26]=.45f;values[27]=.95f;
    const auto centrePixel=[&]() {
        D3D11_MAPPED_SUBRESOURCE m{};assert(SUCCEEDED(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&m)));
        unsigned rgba=0;std::memcpy(&rgba,static_cast<const unsigned char*>(m.pData)+128*m.RowPitch+128*4,4);
        context->Unmap(readback.Get(),0);return rgba;
    };
    draw(0,0,true,false);assert((centrePixel()&0xffffff)==0x00ff00);
    const auto nativeLeft=draw(-.032f,0,true,false),nativeRight=draw(.032f,0,true,false);
    assert(nativeLeft>nativeRight && std::abs(nativeLeft-nativeRight-16.384)<2);
    values[24]=.55f;values[26]=.95f;draw(0,0,false,false);
    assert((centrePixel()&0xffffff)==0xff0000);
    nativeViews[1]=nativeView.Get();context->PSSetShaderResources(1,2,nativeViews);
    draw(0,0,false,false,true);assert((centrePixel()&0xffffff)==0x0000ff); // unchanged native region writes NOTHING, red clear survives
    ID3D11ShaderResourceView* clearNative[2]{};context->PSSetShaderResources(1,2,clearNative);
    values[3]=values[7]=0;values[11]=1;
    // The actual main-HUD shader must compile and mask only transferred areas.
    ComPtr<ID3DBlob> hudVs,hudPs;
    assert(SUCCEEDED(D3DCompile(jc3vr::kHudCompositeShader,sizeof(jc3vr::kHudCompositeShader)-1,"hud_gpu",nullptr,nullptr,"VS","vs_5_0",0,0,&hudVs,&errors)));
    assert(SUCCEEDED(D3DCompile(jc3vr::kHudCompositeShader,sizeof(jc3vr::kHudCompositeShader)-1,"hud_gpu",nullptr,nullptr,"PS","ps_5_0",0,0,&hudPs,&errors)));
    ComPtr<ID3DBlob> menuPs;
    assert(SUCCEEDED(D3DCompile(jc3vr::kHudCompositeShader,sizeof(jc3vr::kHudCompositeShader)-1,"hud_gpu",nullptr,nullptr,"PSMenu","ps_5_0",0,0,&menuPs,&errors)));
    ComPtr<ID3D11VertexShader> hudVertex;ComPtr<ID3D11PixelShader> hudPixel;
    assert(SUCCEEDED(device->CreateVertexShader(hudVs->GetBufferPointer(),hudVs->GetBufferSize(),nullptr,&hudVertex)));
    assert(SUCCEEDED(device->CreatePixelShader(hudPs->GetBufferPointer(),hudPs->GetBufferSize(),nullptr,&hudPixel)));
    context->VSSetShader(hudVertex.Get(),nullptr,0);context->PSSetShader(hudPixel.Get(),nullptr,0);
    ID3D11ShaderResourceView* hudViews[2]{plainView.Get(),nativeView.Get()};context->PSSetShaderResources(0,2,hudViews);
    std::fill(std::begin(values),std::end(values),0.f);values[2]=1;values[3]=3.f/255.f;values[4]=values[5]=256;
    const auto maskDraw=[&](bool hideStatus) {
        values[6]=values[7]=hideStatus?1.f:0.f;context->OMSetRenderTargets(1,&view,nullptr);
        context->UpdateSubresource(buffer,0,nullptr,values,0,0);context->Draw(3,0);
        context->OMSetRenderTargets(0,nullptr,nullptr);context->CopyResource(readback.Get(),target.Get());
    };
    const auto pixelAt=[&](unsigned x,unsigned y) {
        D3D11_MAPPED_SUBRESOURCE m{};assert(SUCCEEDED(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&m)));
        unsigned pixel=0;std::memcpy(&pixel,static_cast<const unsigned char*>(m.pData)+y*m.RowPitch+x*4,4);
        context->Unmap(readback.Get(),0);return pixel&0xffffff;
    };
    maskDraw(false);assert(pixelAt(25,25)!=0 && pixelAt(230,230)!=0);
    maskDraw(true);assert(pixelAt(25,25)==0 && pixelAt(230,230)==0 && pixelAt(128,128)!=0);
    ID3D11ShaderResourceView* clearHud[2]{};context->PSSetShaderResources(0,2,clearHud);
}
