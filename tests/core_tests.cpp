// Keep assert() active in Release builds; these are the test checks.
#undef NDEBUG
#include "render/tracked_hand_rig.h"
#include "render/mapped_write_registry.h"
#include "core/resource_crash_trace.h"
#include "game/placed_transform_history.h"
#include "game/upload_matrix_ownership.h"
#include "game/gauntlet_upload.h"
#include "game/pause_chord.h"
#include "game/haptic_feedback.h"
#include "game/c4_placement.h"
#include "game/support_grip.h"
#include "game/vehicle_camera.h"
#include "game/native_signature.h"
#include "game/tracking_limits.h"
#include "game/dual_wield.h"
#include "openxr/xr_session.h"
#if JC3VR_HAVE_OPENXR_HEADERS
#include "../third_party/openxr/include/openxr/openxr.h"
#endif
#include "core/build_fingerprint.h"
#include "game/camera_probe.h"
#include "game/head_look.h"
#include "game/vr_gamepad.h"
#include "render/vr_hands.h"
#include "render/controller_model.h"
#include "game/vr_body.h"
#include "game/weapon_pose.h"
#include "game/movement_anchor.h"
#include "game/wrist_pose.h"
#include "game/rico_hand_pose.h"
#include "game/pocket_holster.h"
#include <algorithm>
#include <vector>
#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <filesystem>

#include "render/weapon_selector.h"
#include "render/rifle_scope.h"
#include "render/wrist_hud.h"
#include "render/front_end_panel.h"
#include "game/hud_snapshot.h"
#include "wrist_hud_gpu.h"
#include "rifle_scope_gpu.h"
#include <d3dcompiler.h>
#include <wrl/client.h>

int main(int argc,char** argv) {
    {
        using enum jc3vr::PocketAction;
        jc3vr::PocketHolster mode;
        assert(jc3vr::InRightPocket(0.25f, 0.55f, 0.1f));
        assert(!jc3vr::InRightPocket(-0.25f, 0.55f, 0.1f));
        assert(jc3vr::UpdatePocketHolster(mode, true, true, 1) == None);
        assert(jc3vr::UpdatePocketHolster(mode, true, true, 0) == None);
        assert(jc3vr::UpdatePocketHolster(mode, true, true, 0.8f) == Holster && mode.hands);
        assert(jc3vr::UpdatePocketHolster(mode, true, true, 0.8f) == None);
        jc3vr::UpdatePocketHolster(mode, true, false, 0);
        assert(jc3vr::UpdatePocketHolster(mode, true, false, 1) == None && mode.hands);
        assert(jc3vr::UpdatePocketHolster(mode, true, true, 1) == None && mode.hands);
        jc3vr::UpdatePocketHolster(mode, true, true, 0.1f);
        assert(jc3vr::UpdatePocketHolster(mode, true, true, 0.8f) == Draw && !mode.hands);
        jc3vr::UpdatePocketHolster(mode, false, true, 0);
        assert(!mode.released && !mode.hands);
        jc3vr::UploadMatrixOwnership ownership;
        ownership.Protect(96, 48);
        assert(ownership.Overlaps(128) && !ownership.Overlaps(144));
    }
    {
        assert(jc3vr::RicoWeaponIsHolstered(3) && jc3vr::RicoWeaponIsHolstered(4));
        for (const int state : {-1, 0, 1, 2, 5, 8, 10, 11})
            assert(!jc3vr::RicoWeaponIsHolstered(state));
        const jc3vr::RicoFingerInput relaxed{true, 0.0f, 0.0f};
        const jc3vr::RicoFingerInput trigger{true, 0.0f, 1.0f};
        const jc3vr::RicoFingerInput grip{true, 1.0f, 0.0f};
        assert(jc3vr::RicoFingerFlex(30, trigger) > jc3vr::RicoFingerFlex(30, relaxed));
        assert(jc3vr::RicoFingerFlex(41, trigger) == jc3vr::RicoFingerFlex(41, relaxed));
        assert(jc3vr::RicoFingerFlex(41, grip) > jc3vr::RicoFingerFlex(41, relaxed));
        assert(jc3vr::RicoFingerFlex(30, {false, 1, 1}) == 0.0f);
        assert(jc3vr::RicoFingerFlex(41, {true, NAN, NAN}) == jc3vr::RicoFingerFlex(41, relaxed));
    }
    {
        jc3vr::PauseChord chord;
        auto step=[&](bool x,bool y,std::uint64_t t,bool active=true) {
            return jc3vr::UpdatePauseChord(chord,active,x,y,t);
        };
        assert(!step(true,false,0).consume);
        assert(!step(false,true,1).consume);
        assert(step(true,true,2).consume);
        assert(!step(true,true,1001).pause);
        auto fired=step(true,true,1002);assert(fired.fired && fired.pause && fired.consume);
        assert(!step(true,true,1050).fired && step(true,true,1051).pause);
        assert(!step(true,true,1152).pause);
        assert(!step(true,true,5000).fired);
        assert(step(false,true,5001).consume);
        step(false,false,5002);
        assert(!step(true,false,5003).consume);
        step(true,true,5010);step(false,true,5200);
        assert(!step(true,true,7000).fired);
        step(false,false,7001);step(true,true,7010);
        assert(step(true,true,8010).fired);
        assert(!step(true,true,8020,false).pause);
        assert(!step(true,true,10000).pause);
        step(false,false,10001);step(true,true,10002);
        assert(!step(true,true,5).pause);
        step(false,false,6);step(true,true,7);
        assert(step(true,true,1007).fired);
        jc3vr::CancelPauseChord(chord);
        assert(!step(true,true,3000).pause);
        step(false,false,3001);step(true,true,3002);
        assert(step(true,true,4002).fired);
        jc3vr::VrControllerInputs inputs{};inputs.active=true;inputs.extraButtons=0x0010;
        assert(jc3vr::MapVrControllersToGamepad(inputs).buttons==0x0010);
        inputs.extraButtons|=0x0020;
        assert(jc3vr::MapVrControllersToGamepad(inputs).buttons==0x0030);
    }
    {
        assert(jc3vr::UseFrontEndPanel(false,false));
        assert(jc3vr::UseFrontEndPanel(false,true));
        assert(jc3vr::UseFrontEndPanel(true,false)); // stale world camera while loading
        assert(!jc3vr::UseFrontEndPanel(true,true));
        assert(std::abs(jc3vr::FrontEndPanelHeight(1920,1080)-1.35f)<.0001f);
        assert(std::abs(jc3vr::FrontEndPanelHeight(2048,2208)-2.5875f)<.0001f);
        assert(jc3vr::FrontEndPanelHeight(0,1080)==0);
        assert(jc3vr::FrontEndCopyCompatible(DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_TYPELESS));
        assert(jc3vr::FrontEndCopyCompatible(DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB));
        assert(!jc3vr::FrontEndCopyCompatible(DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM));
        assert(!jc3vr::FrontEndCopyCompatible(DXGI_FORMAT_UNKNOWN,DXGI_FORMAT_UNKNOWN));
        assert(jc3vr::FrontEndSrgbFormat(DXGI_FORMAT_R8G8B8A8_UNORM)==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        // Actual full-image GPU copy into the typeless family SteamVR uses:
        // every pixel, including the final row/corners, must survive unchanged.
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,
            nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)));
        std::array<std::uint32_t,35> pixels{};
        for(unsigned i=0;i<pixels.size();++i) pixels[i]=0xff000000u|i|(i<<8)|(i<<16);
        D3D11_TEXTURE2D_DESC desc{};desc.Width=7;desc.Height=5;
        desc.ArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        D3D11_SUBRESOURCE_DATA initial{pixels.data(),7*4,0};
        Microsoft::WRL::ComPtr<ID3D11Texture2D> source,target,staging;
        assert(SUCCEEDED(device->CreateTexture2D(&desc,&initial,&source)));
        desc.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;
        assert(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&target)));
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        assert(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&staging)));
        context->CopyResource(target.Get(),source.Get());context->CopyResource(staging.Get(),target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        assert(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)));
        for(unsigned y=0;y<5;++y)
            assert(std::memcmp(static_cast<const char*>(mapped.pData)+y*mapped.RowPitch,pixels.data()+y*7,7*4)==0);
        context->Unmap(staging.Get(),0);
    }
    {
        // Traversal getters are 22/24 bytes, exceeding the old 16-byte
        // reader. Late state-bit/RET differences must still fail closed.
        std::uint8_t actual[65]{},expected[65]{};
        for(int i=0;i<65;++i) actual[i]=expected[i]=static_cast<std::uint8_t>(i);
        unsigned reads=0;
        auto read=[&](void* dst,const void* src,std::size_t n) {
            ++reads;std::memcpy(dst,src,n);return true;
        };
        const auto address=reinterpret_cast<std::uintptr_t>(actual);
        for(std::size_t n : {15u,16u,22u,24u,64u})
            assert(jc3vr::MatchNativeSignature(address,expected,n,read));
        actual[21]^=1;
        assert(!jc3vr::MatchNativeSignature(address,expected,22,read));
        actual[21]^=1;actual[23]^=1;
        assert(!jc3vr::MatchNativeSignature(address,expected,24,read));
        const auto before=reads;
        assert(!jc3vr::MatchNativeSignature(address,expected,65,read));
        assert(!jc3vr::MatchNativeSignature(0,expected,24,read));
        assert(!jc3vr::MatchNativeSignature(address,nullptr,24,read));
        assert(!jc3vr::MatchNativeSignature(address,expected,0,read));
        assert(reads==before);
        assert(!jc3vr::MatchNativeSignature(address,expected,24,
            [](void*,const void*,std::size_t){return false;}));
    }
    {
        jc3vr::HapticFeedback h;
        assert(jc3vr::ShotHapticHand(true,true,-1,-1,true)==1);
        assert(jc3vr::ShotHapticHand(true,true,0,1,false)==0);
        assert(jc3vr::ShotHapticHand(true,true,1,0,false)==1);
        assert(jc3vr::ShotHapticHand(true,true,-1,0,false)==0);
        assert(jc3vr::ShotHapticHand(true,true,-1,-1,false)==-1);
        assert(jc3vr::ShotHapticHand(false,true,1,1,true)==-1);
        assert(jc3vr::ShotHapticHand(true,false,1,1,true)==-1);
        h.Pulse(1,.6f,50);
        assert(h.Sample(50,1)[0]==0 && h.Sample(50,1)[1]==.6f);
        h={};
        assert(h.Sample(100,1)[0]==0);
        h.Rumble(65535,0,100);
        auto a=h.Sample(100,.7f);assert(a[0]==.7f && a[1]==.7f);
        assert(h.Sample(351,1)[0]==0); // stale game output expires
        h.Rumble(0,0,101);assert(h.Sample(101,1)[1]==0); // native stop
        h.Pulse(0,.45f,200);a=h.Sample(200,1);
        assert(a[0]==.45f && a[1]==0); // grapple only left
        assert(h.Sample(260,1)[0]==0);
        h.Pulse(1,.6f,300);h.Pulse(2,1,300);h.Pulse(0,NAN,300);
        a=h.Sample(301,1);assert(a[0]==0 && a[1]==.6f);
        assert(h.Sample(301,0)[1]==0 && h.Sample(301,NAN)[1]==0);
        jc3vr::ConfigureVrGamepad(true);jc3vr::ConfigureVrHaptics(true,1);
        jc3vr::VrControllerInputs input;input.active=true;jc3vr::PublishVrControllerInputs(input);
        jc3vr::RequestVrHapticPulse(0,.5f);
        assert(jc3vr::GetVrHapticAmplitudes(true)[0]==.5f);
        assert(jc3vr::GetVrHapticAmplitudes(false)[0]==0);
        assert(jc3vr::GetVrHapticAmplitudes(true)[0]==0); // no replay after resume
        jc3vr::ConfigureVrHaptics(false,1);jc3vr::RequestVrHapticPulse(0,1);
        assert(jc3vr::GetVrHapticAmplitudes(true)[0]==0);
        jc3vr::PublishVrControllerInputs({});jc3vr::ConfigureVrHaptics(true,.7f);
    }
    {
        assert(!jc3vr::NeedsTraversalCamera(false,false,false,false,false,false));
        for(int state=0;state<5;++state) {
            bool flags[5]{};flags[state]=true;
            assert(jc3vr::NeedsTraversalCamera(false,flags[0],flags[1],flags[2],flags[3],flags[4]));
            assert(!jc3vr::NeedsTraversalCamera(true,flags[0],flags[1],flags[2],flags[3],flags[4]));
        }
        const float world[16]{1,0,0,0,0,1,0,0,0,0,1,0,100,200,300,1};
        const float eye[3]{.1f,1.6f,-.2f};
        auto anchor=jc3vr::BuildVehicleCameraAnchor(world,eye);anchor.traversal=true;
        const float right[3]{0,0,-1};
        assert(jc3vr::SetTraversalYawFrame(anchor,right));
        assert(anchor.position[0]==100.1f && anchor.position[1]==201.6f);
        assert(anchor.basis[2]==-1 && anchor.basis[4]==1 && anchor.basis[6]==1);
        float out[9]{};const float head[9]{1,0,0,0,1,0,0,0,1};
        assert(jc3vr::ComposeVehicleHeadBasis(anchor.basis,head,out));
        for(int i=0;i<9;++i) assert(out[i]==anchor.basis[i]);
        jc3vr::PublishVehicleCameraAnchor(anchor);
        assert(jc3vr::GetFirstPersonCameraAnchor().valid);
        assert(!jc3vr::GetVehicleCameraAnchor().valid); // no seated weapon/gaze gates
        const float badRight[3]{NAN,0,1},vertical[3]{0,1,0};
        assert(!jc3vr::SetTraversalYawFrame(anchor,badRight));
        assert(!jc3vr::SetTraversalYawFrame(anchor,vertical));
        anchor.traversal=false;
        assert(!jc3vr::SetTraversalYawFrame(anchor,right));
        jc3vr::PublishVehicleCameraAnchor(anchor);
        assert(jc3vr::GetVehicleCameraAnchor().valid);
        jc3vr::PublishVehicleCameraAnchor({});
        assert(!jc3vr::GetFirstPersonCameraAnchor().valid);
    }
    {
        // A world 4x4 at the start of a mixed upload also passes the old
        // rigid 3x4 hide heuristic: rotation norms=1, translation columns=0.
        // Simulate that sweep with the production byte-ownership guard.
        float mixed[1024]{};
        const float gun[16]{1,0,0,0,0,1,0,0,0,0,1,0,10,20,30,1};
        std::copy(gun, gun+16, mixed);
        const float bone[12]{1,0,0,.2f,0,1,0,.3f,0,0,1,.4f};
        for (int i : {16,28,40}) std::copy(bone,bone+12,mixed+i);
        jc3vr::UploadMatrixOwnership ownership;
        ownership.Protect(0);
        assert(ownership.Overlaps(0) && ownership.Overlaps(16) && ownership.Overlaps(48));
        assert(!ownership.Overlaps(64));
        for (std::size_t i=0;i+12<=1024;i+=4) {
            if (ownership.Overlaps(i*4)) continue;
            const float* m=mixed+i;
            const float a=m[0]*m[0]+m[1]*m[1]+m[2]*m[2];
            const float b=m[4]*m[4]+m[5]*m[5]+m[6]*m[6];
            const float c=m[8]*m[8]+m[9]*m[9]+m[10]*m[10];
            if(std::abs(a-1)>.05f || std::abs(b-1)>.05f || std::abs(c-1)>.05f) continue;
            std::fill(mixed+i,mixed+i+12,0.f);
            i+=8;
        }
        for(int i=0;i<16;++i) assert(mixed[i]==gun[i]);
        for(int i=16;i<52;++i) assert(mixed[i]==0.f); // body still hidden
        jc3vr::UploadMatrixOwnership boundary;
        boundary.Protect(64);
        assert(!boundary.Overlaps(16) && boundary.Overlaps(32));
        assert(boundary.Overlaps(112) && !boundary.Overlaps(128));
        jc3vr::UploadMatrixOwnership full;
        for(std::size_t i=0;i<4096;i+=64) full.Protect(i);
        for(std::size_t i=0;i<4096;i+=16) assert(full.Overlaps(i));
        assert(jc3vr::FiniteMatrix(gun,16));
        float bad[16]; std::copy(gun,gun+16,bad); bad[4]=NAN;
        assert(!jc3vr::FiniteMatrix(bad,16));
        assert(!jc3vr::MatchesPlacedWeapon(bad,gun));
        std::copy(gun,gun+16,bad); bad[3]=.1f;
        assert(!jc3vr::MatchesPlacedWeapon(bad,gun));
        std::copy(gun,gun+16,bad); bad[15]=0.f;
        assert(!jc3vr::MatchesPlacedWeapon(bad,gun));
        assert(jc3vr::MatchesPlacedWeapon(gun,gun));
    }
    assert(jc3vr::IsResourceCrash(0xc0000005,0x140000000,0x1400a53c0));
    assert(!jc3vr::IsResourceCrash(0xc0000005,0,0xa53c0));
    assert(!jc3vr::IsResourceCrash(0x80000003,0x140000000,0x1400a53c0));
    assert(!jc3vr::IsResourceCrash(0xc0000005,0x140000000,0x1400a53c1));
    {
        // Real WARP deferred contexts can hold distinct discard maps of the
        // same resource. Exercise the production ownership registry against
        // the actual API, including command-list GPU data after each Unmap.
        using Microsoft::WRL::ComPtr;
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> immediate,a,b;
        assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,
            nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&immediate)));
        assert(SUCCEEDED(device->CreateDeferredContext(0,&a)));
        assert(SUCCEEDED(device->CreateDeferredContext(0,&b)));
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=64;desc.Usage=D3D11_USAGE_DYNAMIC;
        desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> buffer,staging;
        assert(SUCCEEDED(device->CreateBuffer(&desc,nullptr,&buffer)));
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        assert(SUCCEEDED(device->CreateBuffer(&desc,nullptr,&staging)));
        D3D11_MAPPED_SUBRESOURCE ma{},mb{};
        assert(SUCCEEDED(a->Map(buffer.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&ma)));
        assert(SUCCEEDED(b->Map(buffer.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mb)));
        assert(ma.pData!=mb.pData); // real independent map storage, not a synthetic collision
        jc3vr::MappedWriteRegistry<void*> registry;
        registry.Publish({a.Get(),buffer.Get(),0},ma.pData);
        registry.Publish({b.Get(),buffer.Get(),0},mb.pData);
        void* owned=nullptr;
        assert(registry.Consume({a.Get(),buffer.Get(),0},owned) && owned==ma.pData);
        std::memset(owned,0x35,64);a->Unmap(buffer.Get(),0);
        assert(registry.Consume({b.Get(),buffer.Get(),0},owned) && owned==mb.pData);
        std::memset(owned,0x72,64);b->Unmap(buffer.Get(),0);
        ComPtr<ID3D11CommandList> la,lb;
        assert(SUCCEEDED(a->FinishCommandList(FALSE,&la)));
        assert(SUCCEEDED(b->FinishCommandList(FALSE,&lb)));
        const auto verify=[&](ID3D11CommandList* list,unsigned char expected){
            immediate->ExecuteCommandList(list,TRUE);immediate->CopyResource(staging.Get(),buffer.Get());
            D3D11_MAPPED_SUBRESOURCE read{};
            assert(SUCCEEDED(immediate->Map(staging.Get(),0,D3D11_MAP_READ,0,&read)));
            for(unsigned i=0;i<64;++i)assert(static_cast<unsigned char*>(read.pData)[i]==expected);
            immediate->Unmap(staging.Get(),0);
        };
        verify(la.Get(),0x35);verify(lb.Get(),0x72);
    }
    {
        jc3vr::MappedWriteRegistry<int*> maps;
        int contextA{},contextB{},resource{},other{},bytesA{},bytesB{},bytesSub{};
        int* found=nullptr;
        maps.Publish({&contextA,&resource,0},&bytesA);
        maps.Publish({&contextB,&resource,0},&bytesB);
        maps.Publish({&contextA,&resource,1},&bytesSub);
        assert(!maps.Consume({&contextB,&resource,1},found));
        assert(!maps.Consume({&contextA,&other,0},found));
        assert(maps.Consume({&contextA,&resource,0},found) && found==&bytesA);
        assert(!maps.Consume({&contextA,&resource,0},found));
        assert(maps.Consume({&contextB,&resource,0},found) && found==&bytesB);
        assert(maps.Consume({&contextA,&resource,1},found) && found==&bytesSub);
        // Resource-only tracking would return bytesB to contextA here.
        maps.Publish({&contextA,&resource,0},&bytesA);
        maps.Publish({&contextB,&resource,0},&bytesB);
        assert(maps.Consume({&contextB,&resource,0},found) && found==&bytesB);
        assert(maps.Consume({&contextA,&resource,0},found) && found==&bytesA);
    }
    assert(!jc3vr::DeveloperTestInputEnabled()); // normal build never arms scripted controls at boot
    assert(jc3vr::SuppressVrDamage(true,true,true));
    assert(!jc3vr::SuppressVrDamage(false,true,true));
    assert(!jc3vr::SuppressVrDamage(true,false,true));
    assert(!jc3vr::SuppressVrDamage(true,true,false));
    {
        float right[3]{},up[3]{};const float head[3]{.15f,.12f,.4f};
        assert(jc3vr::ReadableHudFrame(head,right,up));
        assert(std::abs(right[0]*right[0]+right[2]*right[2]-1)<.0001f);
        assert(std::abs(up[0]*up[0]+up[1]*up[1]+up[2]*up[2]-1)<.0001f);
        assert(std::abs(right[0]*up[0]+right[1]*up[1]+right[2]*up[2])<.0001f);
        const float invalid[3]{NAN,0,0};assert(!jc3vr::ReadableHudFrame(invalid,right,up));
    }
    {
        jc3vr::HudSnapshot hud;double n=0;
        jc3vr::HudValue args[2]{};args[0].type=3;args[0].value=25;args[1].type=2;
        assert(jc3vr::ApplyHudEvent(hud,"hud.OnSetSelectedMagazineAmmo",args,2) && hud.magazine==25);
        assert(!jc3vr::ApplyHudEvent(hud,"OnSetSelectedMagazineAmmo",args,1));
        args[0].value=360;assert(jc3vr::ApplyHudEvent(hud,"OnSetSelectedInventoryAmmo",args,1) && hud.reserve==360);
        args[0].value=static_cast<std::uint32_t>(-1);assert(!jc3vr::ApplyHudEvent(hud,"OnSetSelectedInventoryAmmo",args,1));
        assert(!jc3vr::ApplyHudEvent(hud,"OnSetSelectedInventoryAmmo",nullptr,1));
        args[1].type=5;double fraction=.75;std::memcpy(&args[1].value,&fraction,8);
        const auto revision=hud.revision;
        assert(!jc3vr::ApplyHudEvent(hud,"UpdateHealth",args,2) && hud.health==-1 && hud.revision==revision);
        args[0].value=800;fraction=.012487;std::memcpy(&args[1].value,&fraction,8);
        assert(!jc3vr::ApplyHudEvent(hud,"UpdateHealth",args,2) && hud.health==-1);
        fraction=NAN;std::memcpy(&args[1].value,&fraction,8);assert(!jc3vr::HudNumber(args[1],n));
        assert(!jc3vr::ApplyHudEvent(hud,"UpdateHealth",args,2));
        assert(jc3vr::ApplyHudEvent(hud,"OnSelectWeapon",nullptr,0) && hud.magazine==-1 && hud.reserve==-1);
        jc3vr::HudValue selection[9]{};selection[2].type=3;selection[2].value=14;
        jc3vr::DamagePresentationParameters damage{};
        std::memset(damage.prefix,0x5a,sizeof(damage.prefix));damage.intensity=.75f;
        const auto quiet=jc3vr::ZeroDamagePresentation(damage);
        assert(quiet.intensity==0 && damage.intensity==.75f);
        assert(std::memcmp(quiet.prefix,damage.prefix,sizeof(damage.prefix))==0);
        for(const auto visual:{"OnOmniDamage","UpdateHealth","UpdateCharacterDmgIndicators"}) {
            assert(jc3vr::SuppressHudDamageVisual(visual,true,true,true));
            assert(!jc3vr::SuppressHudDamageVisual(visual,false,true,true));
            assert(!jc3vr::SuppressHudDamageVisual(visual,true,false,true));
            assert(!jc3vr::SuppressHudDamageVisual(visual,true,true,false));
        }
        for(const auto keep:{"OnSetSelectedMagazineAmmo","UpdateHealthBars","UpdateMechHealth",
                            "OnPlayerDidDamage","OnPlayerDogeDamage","InitHealth","ResetHealth"})
            assert(!jc3vr::SuppressHudDamageVisual(keep,true,true,true));
        assert(jc3vr::ApplyHudEvent(hud,"OnSelectWeapon",selection,9) && hud.capacity==14 && hud.magazine==-1);
        selection[2].value=0;assert(jc3vr::ApplyHudEvent(hud,"OnSelectWeapon",selection,9) && hud.capacity==-1);
        selection[2].type=5;double invalid=NAN;std::memcpy(&selection[2].value,&invalid,8);
        assert(jc3vr::ApplyHudEvent(hud,"OnSelectWeapon",selection,9) && hud.capacity==-1);
        assert(jc3vr::ApplyHudEvent(hud,"OnSelectWeapon",nullptr,9) && hud.capacity==-1);
        Microsoft::WRL::ComPtr<ID3DBlob> shader,errors;
        assert(SUCCEEDED(D3DCompile(jc3vr::kWristHudShader,sizeof(jc3vr::kWristHudShader)-1,"wrist",nullptr,nullptr,"VS","vs_5_0",0,0,&shader,&errors)));
        const auto compiled=D3DCompile(jc3vr::kWristHudShader,sizeof(jc3vr::kWristHudShader)-1,"wrist",nullptr,nullptr,"PS","ps_5_0",0,0,&shader,&errors);
        if(FAILED(compiled) && errors) std::cerr<<static_cast<const char*>(errors->GetBufferPointer());
        assert(SUCCEEDED(compiled));
    }
    {
        float crouch[3]{.2f,-1.1f,.1f};assert(jc3vr::ClampRoomOffset(crouch,.65f,1.8f) && crouch[1]==-1.1f);
        float distantOffset[3]{2,-1.2f,0};assert(jc3vr::ClampRoomOffset(distantOffset,.65f,1.8f) && std::abs(distantOffset[0]-.65f)<1e-6f && distantOffset[1]==-1.2f);
        // Zero disables the optional mod cap: full crouch/prone and lateral
        // room movement stay in the same metre-space as tracked hand targets.
        float prone[3]{1.4f,-2.0f,-1.2f};
        assert(jc3vr::ClampRoomOffset(prone,0,0));
        assert(prone[0]==1.4f && prone[1]==-2.0f && prone[2]==-1.2f);
        float verticalOnly[3]{2,-2,0};
        assert(jc3vr::ClampRoomOffset(verticalOnly,0,1.8f));
        assert(verticalOnly[0]==2 && verticalOnly[1]==-1.8f);
        float bad[3]{NAN,0,0};assert(!jc3vr::ClampRoomOffset(bad,.65f,1.8f));
        jc3vr::RecenterTap tap;
        assert(!jc3vr::TapRecenter(tap,false));
        assert(jc3vr::TapRecenter(tap,true)); // immediate, no duration/action gate
        assert(!jc3vr::TapRecenter(tap,true)); // held combo never repeats
        assert(!jc3vr::TapRecenter(tap,true));
        assert(!jc3vr::TapRecenter(tap,false)); // release either click rearms
        assert(jc3vr::TapRecenter(tap,true));
        assert(!jc3vr::TapRecenter(tap,false));
        assert(jc3vr::TapRecenter(tap,true)); // rapid repeated taps work
        assert(!jc3vr::LegacyAimGate(true,true,false));assert(jc3vr::LegacyAimGate(true,false,false));
        assert(jc3vr::NativeGrappleAimHand(true,false,true,true,1.f)==0);
        assert(jc3vr::NativeGrappleAimHand(true,false,true,true,0.f)==-1); // released/consumed foregrip
        assert(jc3vr::NativeGrappleAimHand(true,false,true,false,1.f)==-1); // tracking lost
        assert(jc3vr::NativeGrappleAimHand(true,true,true,true,1.f)==-1); // vehicle
        assert(jc3vr::NativeGrappleAimHand(false,false,true,true,1.f)==-1);
        assert(jc3vr::NativeGrappleAimHand(true,false,false,true,1.f)==-1);
        assert(jc3vr::NativeGrappleAimHand(true,false,true,true,NAN)==-1);
    }
    if(argc>1) {
        jc3vr::ControllerModelAsset charge;std::string error;
        assert(jc3vr::LoadInstalledChargeAsset(std::filesystem::path(argv[1]).wstring(),charge,error));
        assert(charge.vertices.size()==6192 && charge.width==256 && charge.height==256);
        assert(!jc3vr::LoadInstalledChargeAsset((std::filesystem::path(argv[1])/L"missing").wstring(),charge,error));
    }
    TestRifleScopeGpu();
    TestWristHudGpu();
    {
        const float grip[3]{1,2,3},barrel[3]{0,0,-1},rolledUp[3]{1,0,0};
        float lens[3]{},right[3]{},up[3]{},forward[3]{};
        assert(jc3vr::BuildScopeLensFrame(grip,barrel,rolledUp,lens,right,up,forward));
        assert(std::abs(lens[0]-1.08f)<1e-5f && std::abs(lens[2]-2.88f)<1e-5f);
        assert(right[1]==-1 && up[0]==1 && forward[2]==-1); // full wrist roll retained
        assert(!jc3vr::BuildScopeLensFrame(grip,barrel,barrel,lens,right,up,forward));
        const float bad[3]{NAN,0,0};
        assert(!jc3vr::BuildScopeLensFrame(grip,bad,rolledUp,lens,right,up,forward));
        const auto optic=jc3vr::SniperModelOptic(161);
        assert(optic.valid && optic.radius>0.012f && optic.radius<0.013f);
        assert(jc3vr::BuildScopeLensFrame(grip,barrel,rolledUp,lens,right,up,forward,&optic));
        assert(std::abs(lens[0]-(1+optic.fromGrip[1]))<1e-5f);
        assert(std::abs(lens[2]-(3+optic.fromGrip[2]))<1e-5f);
        assert(jc3vr::SniperModelOptic(162).fromGrip[2]<0);
        assert(jc3vr::SniperModelOptic(163).fromGrip[1]>0.21f);
        assert(!jc3vr::SniperModelOptic(119).valid);
        assert(!jc3vr::BuildScopeLensFrame(grip,barrel,rolledUp,lens,right,up,forward,&optic,0));
    }
    {
        const float lens[3]{0,0,-0.12f},axis[3]{0,0,-1},eye[3]{},side[3]{0.03f,0,0},close[3]{0,0,-0.08f},distantEye[3]{0,0,0.2f};
        assert(jc3vr::ScopeEyeVisibility(eye,lens,axis,.016f)==1);
        assert(jc3vr::ScopeEyeVisibility(side,lens,axis,.016f)==0);
        assert(jc3vr::ScopeEyeVisibility(close,lens,axis,.016f)>0.8f); // 4cm relief now fades, not abrupt blackout
        assert(jc3vr::ScopeEyeVisibility(distantEye,lens,axis,.016f)>0 && jc3vr::ScopeEyeVisibility(distantEye,lens,axis,.016f)<.3f);
        const float backwards[3]{0,0,1},bad[3]{NAN,0,0};
        assert(jc3vr::ScopeEyeVisibility(eye,lens,backwards,.016f)==0);
        assert(jc3vr::ScopeEyeVisibility(bad,lens,axis,.016f)==0);
        const float previouslyTooClose[3]{0,0,-.055f};
        assert(jc3vr::ScopeEyeVisibility(previouslyTooClose,lens,axis,.016f)==1); // live trace6.5cm
        const float pupilEdge[3]{.016f,0,0};
        assert(std::abs(jc3vr::ScopeEyeVisibility(pupilEdge,lens,axis,.016f)-.5f)<1e-4f);
        assert(jc3vr::ScopeEyeVisibility(eye,lens,axis,NAN)==0);
        Microsoft::WRL::ComPtr<ID3DBlob> shader,errors;
        for(const char* entry:{"VS","PS"}) assert(SUCCEEDED(D3DCompile(jc3vr::kRifleScopeShader,sizeof(jc3vr::kRifleScopeShader)-1,
            "scope_test",nullptr,nullptr,entry,entry[0]=='V'?"vs_5_0":"ps_5_0",0,0,&shader,&errors)));
    }
    {
        float source[16]{1,0,0,0,0,1,0,0,0,0,1,0,10.06f,20,30,1},out[16]{};
        const float oldGrip[3]{10,20,30},newGrip[3]{13,22,31},forward[3]{1,0,0},down[3]{0,-1,0};
        assert(jc3vr::RetargetPlacedWeapon(source,oldGrip,newGrip,forward,down,out));
        assert(std::abs(out[12]-13)<1e-5f && std::abs(out[14]-31.06f)<1e-5f);
        float again[16];assert(jc3vr::RetargetPlacedWeapon(out,newGrip,newGrip,forward,down,again));
        for(int i=0;i<16;++i) assert(std::abs(out[i]-again[i])<1e-5f);
        const float nextGrip[3]{14,22,31};assert(jc3vr::RetargetPlacedWeapon(out,newGrip,nextGrip,forward,down,again));
        assert(std::abs(again[12]-14)<1e-5f);
        jc3vr::C4PlacementState state;jc3vr::C4PlacementInput input;
        input.equipped=input.gameplay=input.trackingValid=true;input.frame=1;
        input.contact.hit=true;input.contact.frame=1;input.contact.point[1]=0.1f;input.contact.normal[1]=-1;input.contact.object=123;
        assert(jc3vr::UpdateC4Placement(state,input).contactReady);
        input.placePressed=true;const auto placed=jc3vr::UpdateC4Placement(state,input);
        assert(placed.requestNativePlace && placed.contact.normal[1]==-1 && placed.contact.object==123);
        assert(!jc3vr::UpdateC4Placement(state,input).requestNativePlace);
        float fraction=0;
        assert(jc3vr::C4NativeHitFraction(0.5f,0.12f,3.0f,fraction));
        assert(std::abs(fraction*3.0f-0.06f)<1e-6f);
        assert(!jc3vr::C4NativeHitFraction(1,0.12f,3,fraction));
        assert(!jc3vr::C4NativeHitFraction(0.5f,0.12f,0.01f,fraction));
    }
    {
        float previous[16]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        float next[16]{0,0,-1,0,0,1,0,0,1,0,0,0,20,2,4,1}, out[16]{};
        assert(jc3vr::InterpolateVehicleRoot(previous,next,0.25f,out));
        assert(out[12]==5 && out[13]==0.5f && out[14]==1);
        const float eye[3]{0,1.2f,0}; assert(jc3vr::BuildVehicleCameraAnchor(out,eye).valid);
        assert(!jc3vr::InterpolateVehicleRoot(previous,next,1.1f,out));
    }
    {
        const float sampled[3]{102,3,4}, oldRoot[3]{100,0,0}, currentRoot[3]{115,2,-5};
        float rebased[3]{};
        assert(jc3vr::RebaseTrackedPoint(sampled,oldRoot,currentRoot,rebased));
        assert(rebased[0]==117 && rebased[1]==5 && rebased[2]==-1);
        // Idempotent consumption of the same tracking sample: no accumulation.
        assert(jc3vr::RebaseTrackedPoint(sampled,oldRoot,currentRoot,rebased));
        assert(rebased[0]==117);
        const float teleport[3]{1000,0,0};
        assert(!jc3vr::RebaseTrackedPoint(sampled,oldRoot,teleport,rebased));
    }
    {
        // Camera and actor snapshots can have different interpolation ages.
        // A controller remains camera-local on a fast train, not root-local.
        const float cameraAtSample[3]{100,2,50}, actorAtSample[3]{101,0,50};
        const float handAtSample[3]{100.3f,1.7f,49.5f};
        const float cameraNow[3]{104,2,50}, actorNow[3]{106,0,50};
        float corrected[3]{}, oldMixed[3]{};
        assert(jc3vr::RebaseTrackedPoint(handAtSample,cameraAtSample,cameraNow,corrected));
        assert(jc3vr::RebaseTrackedPoint(handAtSample,actorAtSample,actorNow,oldMixed));
        assert(std::abs(corrected[0]-cameraNow[0]-.3f)<.0001f);
        assert(std::abs(oldMixed[0]-corrected[0]-1.f)<.0001f);
        // Repeating consumption never adds another actor displacement.
        assert(jc3vr::RebaseTrackedPoint(handAtSample,cameraAtSample,cameraNow,corrected));
        assert(std::abs(corrected[0]-104.3f)<.0001f);
        float weapon[16]{1,0,0,0,0,1,0,0,0,0,1,0,100.36f,1.7f,49.5f,1}, retargeted[16]{};
        const float forward[3]{0,0,-1}, down[3]{0,-1,0};
        assert(jc3vr::RetargetPlacedWeapon(weapon,handAtSample,corrected,forward,down,retargeted));
        assert(std::abs(retargeted[12]-104.36f)<.0001f);
        const float restWrist[3]{0,1,0};
        const float restMount[16]{1,0,0,0,0,1,0,0,0,0,1,0,0,1,.06f,1};
        float before[16]{}, after[16]{};
        assert(jc3vr::BuildWristAttachment(restWrist,forward,down,restMount,handAtSample,forward,down,before));
        assert(jc3vr::BuildWristAttachment(restWrist,forward,down,restMount,corrected,forward,down,after));
        for(int i=0;i<12;++i) assert(before[i]==after[i]);
        for(int i=0;i<3;++i) assert(std::abs(after[12+i]-before[12+i]-(cameraNow[i]-cameraAtSample[i]))<.0001f);
    }
    {
        float weapon[16]{1,0,0,0,0,1,0,0,0,0,1,0,10.06f,20,30,1};
        const float grip[3]{10,20,30}, forward[3]{1,0,0}, down[3]{0,-1,0};
        assert(jc3vr::AlignWeaponAboutGrip(grip,forward,down,weapon));
        // The measured 6 cm mesh/mount offset rotates with the gun. Its grip
        // remains fixed, rather than leaving that offset sideways in world X.
        assert(std::abs(weapon[12]-10)<1e-5f && std::abs(weapon[14]-30.06f)<1e-5f);
        assert(std::abs(weapon[12]-0.06f*weapon[0]-grip[0])<1e-5f);
        assert(std::abs(weapon[14]-0.06f*weapon[2]-grip[2])<1e-5f);
        float farGrip[3]{100,20,30};
        assert(!jc3vr::AlignWeaponAboutGrip(farGrip,forward,down,weapon));
    }
    {
        jc3vr::DualFireState left{},right{};
        left.flags[1]=left.flags[3]=left.flags[6]=1;left.flags[4]=1;left.sinceShot=.1f;
        right.sinceShot=.7f;
        auto both=jc3vr::CombinedDualFireState(left,right);
        assert(both.flags[1]==1 && both.flags[3]==1 && both.flags[6]==1 && both.flags[4]==0);
        assert(both.sinceShot==.1f && jc3vr::SaneDualFireState(both));
        jc3vr::AdvanceDualFireClock(left,.02f);
        assert(std::abs(left.sinceShot-.12f)<1e-6f && right.sinceShot==.7f);
        assert(jc3vr::PermittedDualTrigger(1,0)==0); // native reload/menu veto
        assert(jc3vr::PermittedDualTrigger(1,.4f)==.4f);
        assert(jc3vr::PermittedDualTrigger(0,1)==0); // other held trigger cannot fire this hand
        assert(jc3vr::PermittedDualTrigger(std::nanf(""),1)==0);
        left.flags[1]=0;right.flags[1]=1;
        assert(jc3vr::CombinedDualFireState(left,right).flags[1]==1);
        right.buffer=std::nanf("");assert(!jc3vr::SaneDualFireState(right));
    }
    {
        float world[16]{1,0,0,0,0,1,0,0,0,0,1,0,10,20,30,1};
        const float eye[3]{0,1.2f,0.1f};
        const auto seat=jc3vr::BuildVehicleCameraAnchor(world,eye);
        assert(seat.valid && seat.position[0]==10 && std::abs(seat.position[1]-21.2f)<1e-5f);
        world[0]=0;world[2]=-1;world[8]=1;world[10]=0;
        const auto turned=jc3vr::BuildVehicleCameraAnchor(world,eye);
        assert(turned.valid && std::abs(turned.position[0]-10.1f)<1e-5f && turned.basis[2]==-1);
        world[4]=0.2f; assert(!jc3vr::BuildVehicleCameraAnchor(world,eye).valid);
        world[4]=0;world[15]=0;assert(!jc3vr::BuildVehicleCameraAnchor(world,eye).valid);
        world[15]=1;world[5]=-1;assert(!jc3vr::BuildVehicleCameraAnchor(world,eye).valid); // reflection
    }
    {
        float flat[16]{1,0,0,0,0,1,0,0,0,0,1,0,10,20,30,1};
        const float eye[3]{0,1.2f,0};
        auto seat=jc3vr::BuildVehicleCameraAnchor(flat,eye);
        float yaw=99;assert(jc3vr::AircraftSeatYaw(flat,seat.basis,yaw) && std::abs(yaw)<1e-6f);
        float banked[16]{0,1,0,0,-1,0,0,0,0,0,1,0,10,20,30,1};
        assert(jc3vr::ApplyAircraftBank(seat,banked,yaw));
        assert(seat.basis[1]==1 && seat.basis[3]==-1 && seat.position[0]==10);
        const float headIdentity[9]{1,0,0,0,1,0,0,0,1};float camera[9]{},offset[3]{};
        assert(jc3vr::ComposeVehicleHeadBasis(seat.basis,headIdentity,camera));
        assert(camera[1]==1 && camera[3]==-1); // production helper must retain 90deg roll
        jc3vr::VehicleLocalOffset(seat.basis,.0325f,0,0,offset);
        assert(std::abs(offset[1]-.0325f)<1e-6f && offset[0]==0); // IPD banks with eye rig
        jc3vr::VehicleLocalOffset(seat.basis,0,.1f,0,offset);
        assert(std::abs(offset[0]+.1f)<1e-6f); // local up not gravity-up in cockpit
        const float headYaw[9]{0,0,-1,0,1,0,1,0,0};
        assert(jc3vr::ComposeVehicleHeadBasis(seat.basis,headYaw,camera));
        assert(camera[2]==-1 && camera[3]==-1 && camera[7]==1);
        assert(std::abs(seat.position[1]-21.2f)<1e-5f); // orientation only, position preserved
        assert(jc3vr::ApplyAircraftBank(seat,banked,1.57079632679f));
        assert(std::abs(seat.basis[2]-1)<1e-5f && std::abs(seat.basis[7]+1)<1e-5f);
        float inverted[16]{-1,0,0,0,0,-1,0,0,0,0,1,0,10,20,30,1};
        assert(jc3vr::ApplyAircraftBank(seat,inverted,0) && seat.basis[4]==-1);
        banked[15]=0;assert(!jc3vr::ApplyAircraftBank(seat,banked,yaw));
        assert(jc3vr::HelicopterGazeYaw(0,1,0)==0);
        assert(jc3vr::HelicopterGazeYaw(.01f,1,0)==0);
        assert(std::abs(jc3vr::HelicopterGazeYaw(1,0,0)-.65f)<1e-6f);
        assert(std::abs(jc3vr::HelicopterGazeYaw(-1,0,0)+.65f)<1e-6f);
        assert(jc3vr::HelicopterGazeYaw(1,0,-.4f)==-.4f);
        assert(jc3vr::HelicopterGazeYaw(NAN,1,0)==0);
        assert(jc3vr::HelicopterGazeYaw(0,0,0)==0);
    }
    {
        assert((jc3vr::kWeaponSelectorSlots==std::array<int,4>{0,4,6,1}));
        const auto icon=jc3vr::WeaponIconQuad(232,80,false);
        const auto picked=jc3vr::WeaponIconQuad(232,80,true);
        assert(icon[0].u==0 && icon[0].v==0 && icon[2].u==1 && icon[2].v==1);
        assert(icon[0].x<0 && icon[1].x>0 && icon[0].y>0 && icon[2].y<0);
        assert(std::abs(icon[0].y/-icon[0].x-80.0f/232)<1e-6f);
        assert(picked[1].x>icon[1].x);
        const auto backing=jc3vr::WeaponIconBacking(232,80,false);
        assert(backing[0].x<icon[0].x && backing[0].y>icon[0].y && backing[2].y<icon[2].y);
        assert(backing[0].u<0 && backing[2].u<0);
        assert(jc3vr::WeaponIconBacking(0,80,false)[0].x==0);
        assert(jc3vr::WeaponIconQuad(0,80,false)[0].x==0);
        assert(jc3vr::WeaponIconQuad(5000,80,false)[0].x==0);
        Microsoft::WRL::ComPtr<ID3DBlob> vs,ps;
        assert(SUCCEEDED(D3DCompile(jc3vr::kWeaponIconShader,sizeof(jc3vr::kWeaponIconShader)-1,
            "weapon_icons",nullptr,nullptr,"VS","vs_5_0",0,0,&vs,nullptr)));
        assert(SUCCEEDED(D3DCompile(jc3vr::kWeaponIconShader,sizeof(jc3vr::kWeaponIconShader)-1,
            "weapon_icons",nullptr,nullptr,"PS","ps_5_0",0,0,&ps,nullptr)));
    }
    {
        const float wrist[3]{0,1,0}, forward[3]{0,0,-1}, down[3]{0,-1,0}, grip[3]{10,20,30};
        float socket[16]{1,0,0,0,0,1,0,0,0,0,1,0,0,1,0.06f,1}, mounted[16]{};
        assert(jc3vr::BuildWristAttachment(wrist, forward, down, socket, grip, forward, down, mounted));
        assert(std::abs(mounted[12]-10) < 1e-5f && std::abs(mounted[13]-20) < 1e-5f && std::abs(mounted[14]-30.14f) < 1e-5f);
        // 90-degree wrist turn rotates the socket offset, not just its axes.
        const float turned[3]{1,0,0};
        assert(jc3vr::BuildWristAttachment(wrist, forward, down, socket, grip, turned, down, mounted));
        assert(std::abs(mounted[12]-9.86f) < 1e-5f && std::abs(mounted[14]-30) < 1e-5f);
        assert(std::abs(mounted[8]+1) < 1e-5f && std::abs(mounted[2]-1) < 1e-5f);
        const float degenerate[3]{1,0,0};
        assert(!jc3vr::BuildWristAttachment(wrist, forward, down, socket, grip, turned, degenerate, mounted));
        socket[12] = 2.0f;
        assert(!jc3vr::BuildWristAttachment(wrist, forward, down, socket, grip, forward, down, mounted));
    }
    // Weapon axes follow the complete tracked aim frame, even for reverse
    // directions or imperfect input axes; animated roll cannot survive.
    {
        float weapon[16]{0,1,0,0,-1,0,0,0,0,0,1,0,123,456,789,1};
        const float forward[3]{0,0,-2}, down[3]{0,-3,0.2f};
        assert(jc3vr::AlignWeaponToController(forward, down, weapon));
        assert(std::abs(weapon[0]-1) < 1e-6f && std::abs(weapon[5]-1) < 1e-6f && std::abs(weapon[10]-1) < 1e-6f);
        assert(weapon[12] == 123 && weapon[13] == 456 && weapon[14] == 789 && weapon[15] == 1);
        const float backward[3]{0,0,1}, rolledDown[3]{1,0,0};
        assert(jc3vr::AlignWeaponToController(backward, rolledDown, weapon));
        assert(weapon[4] == -1 && weapon[10] == -1);
        float copy[16]; std::copy(weapon, weapon+16, copy);
        const float parallel[3]{0,0,3};
        assert(!jc3vr::AlignWeaponToController(backward, parallel, weapon));
        assert(std::equal(weapon, weapon+16, copy));
        assert(jc3vr::MatchesPlacedWeapon(weapon, copy));
        copy[12] += 0.02f;
        assert(!jc3vr::MatchesPlacedWeapon(weapon, copy));
        copy[12] = weapon[12]; copy[0] += 0.1f;
        assert(!jc3vr::MatchesPlacedWeapon(weapon, copy));
        const float tiltedForward[3]{1,2,-3}, tiltedDown[3]{0.4f,-2,0.3f};
        assert(jc3vr::AlignWeaponToController(tiltedForward, tiltedDown, weapon));
        for (int row = 0; row != 3; ++row)
            for (int other = 0; other != 3; ++other) {
                float dot = 0;
                for (int c = 0; c != 3; ++c) dot += weapon[row*4+c] * weapon[other*4+c];
                assert(std::abs(dot - (row == other ? 1.0f : 0.0f)) < 1e-5f);
            }
        const float determinant = weapon[0]*(weapon[5]*weapon[10]-weapon[6]*weapon[9]) -
            weapon[1]*(weapon[4]*weapon[10]-weapon[6]*weapon[8]) + weapon[2]*(weapon[4]*weapon[9]-weapon[5]*weapon[8]);
        assert(std::abs(determinant - 1) < 1e-5f);
    }
    std::string error;
    const auto executable = jc3vr::FingerprintExecutable(L"C:\\Windows\\System32\\notepad.exe", error);
    assert(executable.has_value());
    assert(executable->fileSize > 0 && executable->sha256.size() == 64);
    const std::array<float, 16> projection{1.0f, 0, 0, 0, 0, 1.5f, 0, 0, 0, 0, 1.0f, 1.0f, 0, 0, -0.1f, 0};
    assert(jc3vr::DiagnoseMatrix(projection).resemblesProjection);
    jc3vr::OpenXrHeadPose identity{true, 0.0f, 0.0f, 0.0f, 1.0f, 1};
    jc3vr::OpenXrHeadPose yawRight{true, 0.0f, 0.38268343f, 0.0f, 0.92387953f, 2};
    const auto yawDelta = jc3vr::CalculateHeadRotationDelta(identity, yawRight);
    assert(yawDelta.yawRadians > 0.7f && yawDelta.yawRadians < 0.9f);
    // VR gamepad mapping: deadzone, triggers, grips -> shoulders, buttons.
    jc3vr::VrControllerInputs pad{};
    assert(jc3vr::MapVrControllersToGamepad(pad).buttons == 0);
    pad.active = true;
    pad.leftStickX = 0.05f;
    pad.rightStickY = 1.0f;
    pad.rightTrigger = 1.0f;
    pad.leftGrip = 0.9f;
    pad.a = true;
    pad.menu = true;
    const auto mapped = jc3vr::MapVrControllersToGamepad(pad);
    assert(mapped.thumbLX == 0 && mapped.thumbRY == 32767 && mapped.rightTrigger == 255 && mapped.leftTrigger == 0);
    assert(mapped.buttons == (0x0100 | 0x1000));  // LB, A; the menu button is not bound
    // VR hand mesh conventions (OpenXR grip frame): open fingers point along
    // -Y, curling pulls them back, each hand lies behind its palm normal.
    const auto extent = [](const std::vector<jc3vr::HandMeshVertex>& mesh, float& minY, float& meanX) {
        minY = 1.0f;
        meanX = 0.0f;
        for (const auto& vertex : mesh) { minY = std::min(minY, vertex.position[1]); meanX += vertex.position[0]; }
        meanX /= static_cast<float>(mesh.size());
    };
    std::vector<jc3vr::HandMeshVertex> openLeft, fistLeft, openRight;
    jc3vr::BuildHandMesh(true, {}, openLeft);
    jc3vr::BuildHandMesh(true, {1.0f, 1.0f, 1.0f}, fistLeft);
    jc3vr::BuildHandMesh(false, {}, openRight);
    float minYOpen = 0, minYFist = 0, meanXLeft = 0, meanXRight = 0, unused = 0;
    extent(openLeft, minYOpen, meanXLeft);
    extent(fistLeft, minYFist, unused);
    extent(openRight, unused, meanXRight);
    assert(minYOpen < -0.12f);
    assert(minYFist > minYOpen + 0.04f);
    assert(meanXLeft < 0.0f && meanXRight > 0.0f);
    // Render-model parsing: a quad face fans into two triangles, v flips.
    std::vector<jc3vr::ControllerModelVertex> parsed;
    assert(jc3vr::ParseObjTriangles("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
                                    "vn 0 0 1\nf 1/1/1 2/2/1 3/3/1 4/4/1\n", parsed));
    assert(parsed.size() == 6 && parsed[2].position[1] == 1.0f && parsed[0].uv[1] == 1.0f && parsed[0].normal[2] == 1.0f);
    const auto component = jc3vr::ParseRenderModelComponent(
        "{\"openxr_grip\": {\"component_local\" : {\"origin\": [-0.007, -0.0018, 0.1019], \"rotate_xyz\" : [20.6, 0.0, 0.0]}}}", "openxr_grip");
    assert(component.found && std::abs(component.origin[2] - 0.1019f) < 1e-5f && std::abs(component.rotateXyzDegrees[0] - 20.6f) < 1e-4f);
    // When a local SteamVR install is present, the real Touch Plus models load.
    {
        jc3vr::ControllerModelAsset asset;
        std::string error;
        if (jc3vr::LoadQuestPlusControllerModel(false, asset, error)) {
            assert(asset.vertices.size() == 9728u * 3u && asset.grip.found && asset.aim.found && asset.width > 1);
            std::cout << "local Touch Plus model: " << asset.vertices.size() << " vertices, texture " << asset.width << "x" << asset.height
                      << ", grip rotate_x=" << asset.grip.rotateXyzDegrees[0] << "\n";
        } else std::cout << "local Touch Plus model not checked: " << error << "\n";
    }
    // Controller model: non-empty, within ~12 cm of the grip origin.
    std::vector<jc3vr::HandMeshVertex> controller;
    jc3vr::BuildControllerMesh(false, {}, controller);
    assert(controller.size() >= 36 * 5);
    for (const auto& v : controller)
        assert(std::abs(v.position[0]) < 0.06f && std::abs(v.position[1]) < 0.08f && std::abs(v.position[2]) < 0.12f);
    // Aim line: 36 vertices spanning z in [-length, 0] along the aim axis.
    std::vector<jc3vr::HandMeshVertex> beam;
    const float beamColor[3]{1.0f, 0.5f, 0.1f};
    jc3vr::BuildAimBeam(1.5f, 0.006f, beamColor, beam);
    {
        using jc3vr::BodyVec3;
        const auto distance = [](BodyVec3 a, BodyVec3 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); };
        // Arm: 0.30 m upper, 0.27 m lower, hanging down from the shoulder.
        const BodyVec3 shoulder{0.2f, 1.45f, 0.0f}, elbow{0.2f, 1.15f, 0.0f}, wrist{0.2f, 0.88f, 0.0f};
        const BodyVec3 pole{0.35f, -1.0f, 0.25f};
        // Reachable target in front: bone lengths preserved, wrist on target,
        // elbow bent towards the pole (below the shoulder-target line).
        const BodyVec3 target{0.25f, 1.30f, -0.40f};
        const auto ik = jc3vr::SolveTwoBoneIk(shoulder, elbow, wrist, target, pole);
        assert(ik.reached);
        assert(std::abs(distance(shoulder, ik.elbow) - 0.30f) < 1e-3f && std::abs(distance(ik.elbow, ik.wrist) - 0.27f) < 1e-3f);
        assert(distance(ik.wrist, target) < 1e-3f);
        assert(ik.elbow.y < 1.40f);
        // Out of reach: no stretching; wrist stops short along the reach line.
        const auto farTarget = jc3vr::SolveTwoBoneIk(shoulder, elbow, wrist, BodyVec3{0.2f, 1.45f, -2.0f}, pole);
        assert(!farTarget.reached);
        assert(std::abs(distance(shoulder, farTarget.elbow) - 0.30f) < 1e-3f && std::abs(distance(farTarget.elbow, farTarget.wrist) - 0.27f) < 1e-3f);
        assert(distance(shoulder, farTarget.wrist) < 0.57f);
        // Neck pivot sits behind and below the eyes.
        const auto neck = jc3vr::NeckFromEyes({0.0f, 1.7f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f});
        assert(std::abs(neck.y - 1.59f) < 1e-4f && std::abs(neck.z - 0.08f) < 1e-4f);
        // Body yaw: inside the comfort zone the body holds; beyond it, it
        // turns until the head is within the settle band; moving follows.
        jc3vr::BodyYawState yaw;
        jc3vr::UpdateBodyYaw(yaw, 0.0f, 0.0f, 0.011f);
        for (int i = 0; i != 100; ++i) jc3vr::UpdateBodyYaw(yaw, 0.5f, 0.0f, 0.011f);   // ~29 deg: hold
        assert(std::abs(yaw.bodyYaw) < 1e-6f);
        for (int i = 0; i != 300; ++i) jc3vr::UpdateBodyYaw(yaw, 1.2f, 0.0f, 0.011f);   // ~69 deg: turn, settle
        assert(yaw.bodyYaw > 1.2f - 0.22f && yaw.bodyYaw < 1.2f - 0.15f);
        for (int i = 0; i != 300; ++i) jc3vr::UpdateBodyYaw(yaw, 1.2f, 1.0f, 0.011f);   // moving: align
        assert(std::abs(yaw.bodyYaw - 1.2f) < 1e-3f);
        assert(std::abs(jc3vr::WrapAngle(3.3f) - (3.3f - 6.2831853f)) < 1e-5f);
    }
    assert(beam.size() == 36);
    float beamMinZ = 0.0f, beamMaxZ = -9.0f, beamMaxX = 0.0f;
    for (const auto& v : beam) {
        beamMinZ = std::min(beamMinZ, v.position[2]);
        beamMaxZ = std::max(beamMaxZ, v.position[2]);
        beamMaxX = std::max(beamMaxX, std::abs(v.position[0]));
    }
    assert(std::abs(beamMinZ + 1.5f) < 1e-4f && std::abs(beamMaxZ) < 1e-4f && beamMaxX < 0.004f);
    #if JC3VR_HAVE_OPENXR_HEADERS
    assert(jc3vr::OpenXrResultRequiresReconnect(XR_SESSION_LOSS_PENDING));
    assert(jc3vr::OpenXrResultRequiresReconnect(XR_ERROR_SESSION_LOST));
    assert(jc3vr::OpenXrResultRequiresReconnect(XR_ERROR_INSTANCE_LOST));
    assert(jc3vr::OpenXrResultRequiresReconnect(XR_ERROR_SESSION_NOT_RUNNING));
    assert(jc3vr::OpenXrResultRequiresReconnect(XR_ERROR_RUNTIME_FAILURE));
    assert(!jc3vr::OpenXrResultRequiresReconnect(XR_SUCCESS));
    assert(!jc3vr::OpenXrResultRequiresReconnect(XR_EVENT_UNAVAILABLE));
    assert(!jc3vr::OpenXrResultRequiresReconnect(XR_TIMEOUT_EXPIRED));
    assert(!jc3vr::OpenXrResultRequiresReconnect(XR_ERROR_VALIDATION_FAILURE));
    #endif
    std::cout << "JC3VR core tests passed\n";
    {
        const float grip[3]{10,20,30}, aim[4]{0,0,0,1};
        float position[3], orientation[4];
        assert(jc3vr::BuildTrackedHandFrame(grip,aim,position,orientation));
        assert(position[0]==10 && position[1]==20 && position[2]==30);
        // +90deg X maps authored finger -Y to authoritative aim -Z.
        assert(std::abs(2*orientation[0]*orientation[3]-1)<1e-5f);
        float otherPosition[3], otherOrientation[4];
        assert(jc3vr::BuildTrackedHandFrame(grip,aim,otherPosition,otherOrientation));
        assert(std::equal(position,position+3,otherPosition)); // independent of eye/player movement
        const float invalid[4]{};
        assert(!jc3vr::BuildTrackedHandFrame(grip,invalid,position,orientation));
        std::vector<jc3vr::HandMeshVertex> hands;
        jc3vr::BuildHandMesh(true,{0.5f,0.5f,0.5f},hands);
        jc3vr::BuildHandMesh(false,{0.5f,0.5f,0.5f},hands);
        assert(hands.size()<4096);
        for (const auto& vertex:hands) {
            const auto* n=vertex.normal;
            assert(std::isfinite(vertex.position[0]) && std::abs(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]-1)<1e-4f);
        }
    }
    {
        // Buffer size is shared with body palettes; only exact LOCAL gauntlet
        // placement or bone45 can exempt an attachment from whole-body hide.
        const float forearm[16]{1,0,0,0,0,1,0,0,0,0,1,0,10,20,30,1};
        assert(jc3vr::IsLocalGauntletUpload(4096,forearm,true,nullptr));
        assert(jc3vr::IsLocalGauntletUpload(4096,forearm,false,forearm));
        assert(!jc3vr::IsLocalGauntletUpload(528,forearm,true,forearm));
        assert(!jc3vr::IsLocalGauntletUpload(4096,forearm,false,nullptr));
        float other[16];std::copy(forearm,forearm+16,other);other[12]+=.1f;
        assert(!jc3vr::IsLocalGauntletUpload(4096,other,false,forearm));
        other[3]=.2f;
        assert(!jc3vr::IsLocalGauntletUpload(4096,other,true,forearm));
        other[3]=0;other[7]=NAN;
        assert(!jc3vr::IsLocalGauntletUpload(4096,other,true,forearm));
        assert(!jc3vr::IsLocalGauntletUpload(4096,nullptr,true,forearm));
        jc3vr::PlacedTransformHistory history;
        float first[16]{}, second[16]{};
        first[0]=first[5]=first[10]=first[15]=1;
        std::copy(first, first+16, second);
        first[12]=2; second[12]=3;
        history.Publish(first,1000); history.Publish(second,1010);
        assert(history.Contains(first,1020) && history.Contains(second,1020));
        assert(!history.Contains(first,1200));
        assert(!history.Contains(second,900));
        second[12]=20;
        assert(!history.Contains(second,1020));
        jc3vr::VrControllerInputs input{};
        input.active=true; input.rightTrigger=1; input.leftGrip=1;
        const auto pad=jc3vr::MapVrControllersToGamepad(input);
        assert(pad.rightTrigger==255 && (pad.buttons & 0x0100)); // RT and LB both delivered
    }
    {
        jc3vr::SupportGripState state;
        jc3vr::SupportGripInput in;
        in.eligible=in.tracking=in.gameplay=true; in.weapon=123;
        in.forward[2]=-1; in.left[2]=-0.30f;
        assert(!jc3vr::UpdateSupportGrip(state,in).precision);
        in.squeeze=0.9f;
        auto grab=jc3vr::UpdateSupportGrip(state,in);
        assert(grab.precision && grab.consumeGrapple);
        in.left[0]=0.12f; // release hysteresis, no chatter near the capsule edge
        assert(jc3vr::UpdateSupportGrip(state,in).precision);
        float forward[3]{}, socket[3]{};
        in.left[0]=0.30f; in.left[2]=0;
        assert(jc3vr::UpdateSupportGrip(state,in).precision); // rotate outside original aim cone
        assert(jc3vr::SupportGripFrame(state,in.left,in.right,forward,socket));
        assert(std::abs(forward[0]-1)<1e-5f && std::abs(socket[0]-0.30f)<1e-5f);
        in.left[0]=0.50f;
        assert(jc3vr::SupportGripFrame(state,in.left,in.right,forward,socket));
        assert(std::abs(socket[0]-0.30f)<1e-5f); // no gun stretching when hands separate
        in.left[0]=1.0f;
        grab=jc3vr::UpdateSupportGrip(state,in);
        assert(!grab.precision && grab.consumeGrapple); // no accidental grapple on detaching
        in.squeeze=0;
        assert(!jc3vr::UpdateSupportGrip(state,in).consumeGrapple);
        in.squeeze=1;
        assert(!jc3vr::UpdateSupportGrip(state,in).consumeGrapple); // grip elsewhere remains LB
        in.left[0]=0; in.left[2]=-0.30f;
        assert(!jc3vr::UpdateSupportGrip(state,in).precision); // no capture halfway through grapple
        in.squeeze=0; jc3vr::UpdateSupportGrip(state,in);
        in.squeeze=1;
        assert(jc3vr::UpdateSupportGrip(state,in).precision);
        in.tracking=false;
        grab=jc3vr::UpdateSupportGrip(state,in);
        assert(!grab.precision && grab.consumeGrapple);
        in.tracking=true;
        assert(!jc3vr::UpdateSupportGrip(state,in).precision); // tracking resume doesn't recapture held press
        in.squeeze=0; jc3vr::UpdateSupportGrip(state,in);
        in.squeeze=1; in.weapon=456;
        assert(jc3vr::UpdateSupportGrip(state,in).precision);
        in.weapon=789;
        assert(!jc3vr::UpdateSupportGrip(state,in).precision);
        in.squeeze=0; jc3vr::UpdateSupportGrip(state,in);
        in.squeeze=1; in.eligible=false;
        assert(!jc3vr::UpdateSupportGrip(state,in).consumeGrapple);
        in.eligible=true; in.squeeze=0; jc3vr::UpdateSupportGrip(state,in);
        in.forward[0]=NAN; in.squeeze=1;
        assert(!jc3vr::UpdateSupportGrip(state,in).precision);
    }
    {
        const auto rifle=jc3vr::SupportProfileForTable(0x23E3038);
        const auto sniper=jc3vr::SupportProfileForTable(0x23F0A48);
        const auto launcher=jc3vr::SupportProfileForTable(0x23FB5E8);
        assert(rifle.eligible && rifle.nativePrecision);
        assert(sniper.eligible && !sniper.nativePrecision && sniper.maximum>rifle.maximum);
        assert(launcher.eligible && !launcher.nativePrecision);
        assert(!jc3vr::SupportProfileForTable(0x23F8F88).eligible); // charge is never a rifle grab
        assert(!jc3vr::SupportProfileForTable(0x23F0C48).eligible); // secondary vtable rejected
        jc3vr::SupportGripState state; jc3vr::SupportGripInput in;
        in.eligible=in.tracking=in.gameplay=true;in.weapon=123;in.squeeze=1;
        in.forward[2]=-1;in.left[2]=-0.62f;
        assert(!jc3vr::UpdateSupportGrip(state,in).precision); // old rifle range
        in.squeeze=0;jc3vr::UpdateSupportGrip(state,in);
        in.maximum=sniper.maximum;in.squeeze=1;
        assert(jc3vr::UpdateSupportGrip(state,in).precision);
        float forward[3]{},socket[3]{};
        assert(jc3vr::SupportGripFrame(state,in.left,in.right,forward,socket));
        assert(std::abs(socket[2]+0.62f)<1e-5f);
        in.maximum=NAN;
        assert(!jc3vr::UpdateSupportGrip(state,in).precision);
    }
    {
        jc3vr::C4PlacementState state;
        jc3vr::C4PlacementInput input;
        input.equipped = input.trackingValid = input.gameplay = true;
        input.frame = input.contact.frame = 20;
        input.contact.hit = true;
        input.contact.point[2] = 0.1f; input.contact.normal[2] = -2.0f;
        input.contact.object = 123;
        input.placePressed = true;
        assert(!jc3vr::UpdateC4Placement(state, input).requestNativePlace); // equip while holding
        input.placePressed = false;
        assert(jc3vr::UpdateC4Placement(state, input).contactReady);
        input.placePressed = true;
        const auto placed = jc3vr::UpdateC4Placement(state, input);
        assert(placed.requestNativePlace && placed.contact.object == 123 && placed.contact.normal[2] == -1);
        assert(!jc3vr::UpdateC4Placement(state, input).requestNativePlace);
        input.contact.point[2] = 0.14f;
        assert(jc3vr::UpdateC4Placement(state, input).contactReady); // contact hysteresis
        input.contact.point[2] = 2.0f;
        assert(!jc3vr::UpdateC4Placement(state, input).contactReady); // not remote planting
        input.contact.point[2] = 0.1f;
        input.contact.frame = 19;
        assert(!jc3vr::UpdateC4Placement(state, input).contactReady);
        input.trackingValid = false;
        assert(!jc3vr::UpdateC4Placement(state, input).showHeldCharge);
        input.trackingValid = true; input.contact.frame = 20;
        assert(!jc3vr::UpdateC4Placement(state, input).requestNativePlace);
    }
}
