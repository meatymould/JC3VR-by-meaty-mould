#pragma once
#include "render/rifle_scope.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cassert>
#include <array>

inline void TestRifleScopeGpu() {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)));
    ComPtr<ID3DBlob> vs,ps,error;
    assert(SUCCEEDED(D3DCompile(jc3vr::kRifleScopeShader,sizeof(jc3vr::kRifleScopeShader)-1,"scope_gpu",nullptr,nullptr,"VS","vs_5_0",0,0,&vs,&error)));
    assert(SUCCEEDED(D3DCompile(jc3vr::kRifleScopeShader,sizeof(jc3vr::kRifleScopeShader)-1,"scope_gpu",nullptr,nullptr,"PS","ps_5_0",0,0,&ps,&error)));
    ComPtr<ID3D11VertexShader> vertex;ComPtr<ID3D11PixelShader> pixel;
    assert(SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex)));
    assert(SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel)));
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=4;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    std::array<unsigned,16> green;green.fill(0xFF00FF00);
    D3D11_SUBRESOURCE_DATA data{green.data(),16,0};
    ComPtr<ID3D11Texture2D> scene;ComPtr<ID3D11ShaderResourceView> sceneView;
    assert(SUCCEEDED(device->CreateTexture2D(&desc,&data,&scene)));
    assert(SUCCEEDED(device->CreateShaderResourceView(scene.Get(),nullptr,&sceneView)));
    desc.Width=desc.Height=100;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target,readback;ComPtr<ID3D11RenderTargetView> targetView;
    assert(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&target)));
    assert(SUCCEEDED(device->CreateRenderTargetView(target.Get(),nullptr,&targetView)));
    desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    assert(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&readback)));
    D3D11_SAMPLER_DESC samplerDesc{};samplerDesc.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU=samplerDesc.AddressV=samplerDesc.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    assert(SUCCEEDED(device->CreateSamplerState(&samplerDesc,&sampler)));
    D3D11_RASTERIZER_DESC rasterDesc{};rasterDesc.FillMode=D3D11_FILL_SOLID;rasterDesc.CullMode=D3D11_CULL_NONE;rasterDesc.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;
    assert(SUCCEEDED(device->CreateRasterizerState(&rasterDesc,&raster)));
    float constants[36]{0,0,-0.12f,0, 1,0,0,0, 0,1,0,0, -1,1,1,-1,
        1,1,0,0, 1,0,0,0, 0,1,0,0, 0,0,-1,0, 0.022f,4,0.12f,1};
    D3D11_BUFFER_DESC bufferDesc{};bufferDesc.ByteWidth=sizeof(constants);bufferDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> buffer;
    assert(SUCCEEDED(device->CreateBuffer(&bufferDesc,nullptr,&buffer)));
    D3D11_VIEWPORT viewport{0,0,100,100,0,1};context->RSSetViewports(1,&viewport);context->RSSetState(raster.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex.Get(),nullptr,0);context->PSSetShader(pixel.Get(),nullptr,0);
    auto* cb=buffer.Get();auto* srv=sceneView.Get();auto* ss=sampler.Get();auto* rtv=targetView.Get();
    context->VSSetConstantBuffers(0,1,&cb);context->PSSetConstantBuffers(0,1,&cb);
    context->PSSetShaderResources(0,1,&srv);context->PSSetSamplers(0,1,&ss);
    const float red[4]{1,0,0,1};
    const auto draw=[&] {
        context->ClearRenderTargetView(rtv,red);context->OMSetRenderTargets(1,&rtv,nullptr);
        context->UpdateSubresource(cb,0,nullptr,constants,0,0);context->Draw(6,0);
        context->OMSetRenderTargets(0,nullptr,nullptr);context->CopyResource(readback.Get(),target.Get());
    };
    const auto sample=[&](unsigned x,unsigned y) {
        D3D11_MAPPED_SUBRESOURCE mapped{};assert(SUCCEEDED(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped)));
        const auto* p=static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch+4*x;
        std::array<unsigned char,3> rgb{p[0],p[1],p[2]};context->Unmap(readback.Get(),0);return rgb;
    };
    draw();auto rgb=sample(52,52);assert(rgb[1]>220 && rgb[0]<10); // scene visible inside aligned lens
    rgb=sample(62,50);assert(rgb[0]>220 && rgb[1]<10); // outside aperture unchanged
    constants[35]=0;draw();rgb=sample(52,52);assert(rgb[0]<10 && rgb[1]<10); // side-eye blackout
    constants[35]=.5f;draw();rgb=sample(52,52);assert(rgb[1]>110 && rgb[1]<150 && rgb[0]<10); // continuous eye-box fade
    constants[35]=1;constants[28]=10;draw();rgb=sample(52,52);assert(rgb[0]<10 && rgb[1]<10); // uncovered ray black, not edge stretched
}
