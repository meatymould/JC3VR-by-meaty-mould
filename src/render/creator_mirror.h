#pragma once
#include "render/front_end_panel.h"
#include <d3d11.h>
#include <string_view>

namespace jc3vr {
enum class CreatorMirrorEye { Off = -1, Left = 0, Right = 1 };
inline CreatorMirrorEye ParseCreatorMirrorEye(std::wstring_view value) {
    if(value==L"left")return CreatorMirrorEye::Left;
    if(value==L"right")return CreatorMirrorEye::Right;
    return CreatorMirrorEye::Off;
}
inline bool CreatorMirrorReady(CreatorMirrorEye eye,bool ready,bool frontEnd,
                               std::uint64_t frame,std::uint64_t retained) {
    return eye!=CreatorMirrorEye::Off && ready && !frontEnd &&
           retained<=frame && frame-retained<=2;
}
// Bit-preserving full image copy, not an eye render/reprojection/crop. No context
// bindings are changed. A resize or unsupported format leaves desktop untouched.
inline bool CopyCreatorMirror(ID3D11DeviceContext* context,ID3D11Texture2D* desktop,
                              ID3D11Texture2D* eye) {
    if(!context || !desktop || !eye || desktop==eye || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return false;
    D3D11_TEXTURE2D_DESC dst{},src{};desktop->GetDesc(&dst);eye->GetDesc(&src);
    if(!src.Width || !src.Height || src.Width!=dst.Width || src.Height!=dst.Height ||
       src.ArraySize!=dst.ArraySize || src.MipLevels!=dst.MipLevels ||
       src.SampleDesc.Count!=dst.SampleDesc.Count || src.SampleDesc.Quality!=dst.SampleDesc.Quality ||
       dst.Usage==D3D11_USAGE_IMMUTABLE || !FrontEndCopyCompatible(src.Format,dst.Format))return false;
    context->CopyResource(desktop,eye);
    return true;
}
}
