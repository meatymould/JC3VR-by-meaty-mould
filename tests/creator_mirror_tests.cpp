#undef NDEBUG
#include "render/creator_mirror.h"
#include "core/config.h"
#include <wrl/client.h>
#include <array>
#include <cassert>
#include <iostream>
using Microsoft::WRL::ComPtr;
using namespace jc3vr;
int main() {
    assert(ParseCreatorMirrorEye(L"left")==CreatorMirrorEye::Left);
    assert(ParseCreatorMirrorEye(L"right")==CreatorMirrorEye::Right);
    assert(ParseCreatorMirrorEye(L"off")==CreatorMirrorEye::Off);
    assert(ParseCreatorMirrorEye(L"invalid")==CreatorMirrorEye::Off);
    assert(Config{}.creatorMirrorEye==L"off");
    const auto configPath=std::filesystem::current_path()/L"creator_mirror_config_fixture.ini";
    assert(!std::filesystem::exists(configPath));
    assert(LoadConfig(configPath).creatorMirrorEye==L"off");
    assert(WritePrivateProfileStringW(L"JC3VR",L"creator_mirror_eye",L"left",configPath.c_str()));
    assert(ParseCreatorMirrorEye(LoadConfig(configPath).creatorMirrorEye)==CreatorMirrorEye::Left);
    assert(WritePrivateProfileStringW(L"JC3VR",L"creator_mirror_eye",L"right",configPath.c_str()));
    assert(ParseCreatorMirrorEye(LoadConfig(configPath).creatorMirrorEye)==CreatorMirrorEye::Right);
    assert(WritePrivateProfileStringW(L"JC3VR",L"creator_mirror_eye",L"bad",configPath.c_str()));
    assert(ParseCreatorMirrorEye(LoadConfig(configPath).creatorMirrorEye)==CreatorMirrorEye::Off);
    assert(DeleteFileW(configPath.c_str()));
    assert(CreatorMirrorReady(CreatorMirrorEye::Left,true,false,20,20));
    assert(CreatorMirrorReady(CreatorMirrorEye::Right,true,false,21,20));
    assert(!CreatorMirrorReady(CreatorMirrorEye::Off,true,false,20,20));
    assert(!CreatorMirrorReady(CreatorMirrorEye::Left,false,false,20,20));
    assert(!CreatorMirrorReady(CreatorMirrorEye::Left,true,true,20,20));
    assert(!CreatorMirrorReady(CreatorMirrorEye::Left,true,false,24,20));
    assert(!CreatorMirrorReady(CreatorMirrorEye::Left,true,false,19,20));
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
                                     D3D11_SDK_VERSION,&device,nullptr,&context)));
    D3D11_TEXTURE2D_DESC d{};d.Width=8;d.Height=4;d.MipLevels=1;d.ArraySize=1;
    d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.SampleDesc.Count=1;d.Usage=D3D11_USAGE_DEFAULT;
    std::array<UINT,32> red{},blue{};red.fill(0xff1020d0);blue.fill(0xffd02010);
    // Distinct top/bottom markers verify full vertical extent and no flip/crop.
    red[0]=0xff112233;red[31]=0xffaabbcc;
    D3D11_SUBRESOURCE_DATA data{red.data(),32,0};
    ComPtr<ID3D11Texture2D> left,right,desktop,readback;
    assert(SUCCEEDED(device->CreateTexture2D(&d,&data,&left)));
    data.pSysMem=blue.data();assert(SUCCEEDED(device->CreateTexture2D(&d,&data,&right)));
    assert(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&desktop)));
    d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    assert(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&readback)));
    auto verify=[&](ID3D11Texture2D* source,const auto& pixels){
        context->CopyResource(readback.Get(),source);D3D11_MAPPED_SUBRESOURCE map{};
        assert(SUCCEEDED(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&map)));
        for(UINT y=0;y<4;++y)for(UINT x=0;x<8;++x)
            assert(reinterpret_cast<const UINT*>(static_cast<const char*>(map.pData)+y*map.RowPitch)[x]==pixels[y*8+x]);
        context->Unmap(readback.Get(),0);
    };
    D3D11_VIEWPORT viewport{1,2,3,4,0,1};context->RSSetViewports(1,&viewport);
    for(int frame=0;frame<4;++frame){
        // Even with alternating engine frames the mirror always selects left.
        context->CopyResource(desktop.Get(),frame%2?right.Get():left.Get());
        assert(CopyCreatorMirror(context.Get(),desktop.Get(),left.Get()));verify(desktop.Get(),red);
    }
    assert(CopyCreatorMirror(context.Get(),desktop.Get(),right.Get()));verify(desktop.Get(),blue);
    verify(left.Get(),red);verify(right.Get(),blue); // headset sources unchanged
    UINT n=1;D3D11_VIEWPORT after{};context->RSGetViewports(&n,&after);
    assert(after.TopLeftX==1 && after.TopLeftY==2 && after.Width==3 && after.Height==4);
    assert(!CopyCreatorMirror(context.Get(),desktop.Get(),desktop.Get()));
    assert(!CopyCreatorMirror(nullptr,desktop.Get(),left.Get()));
    d.Usage=D3D11_USAGE_DEFAULT;d.CPUAccessFlags=0;d.Width=4;
    ComPtr<ID3D11Texture2D> wrong;assert(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&wrong)));
    assert(!CopyCreatorMirror(context.Get(),desktop.Get(),wrong.Get()));verify(desktop.Get(),blue);
    std::cout<<"Creator mirror WARP: fixed-eye selection, full pixels, unchanged sources/state, resize/off/menu guards PASS\n";
}
