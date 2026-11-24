#pragma once
#include <cstdint>
#include <dxgiformat.h>
namespace jc3vr {
// Camera existence alone survives some loading transitions. Require fresh
// scene rendering too; a loading/front-end backbuffer is not a stereo eye.
inline bool UseFrontEndPanel(bool gameplayCamera,bool sceneRendering) {
    return !gameplayCamera || !sceneRendering;
}
inline float FrontEndPanelHeight(std::uint32_t width,std::uint32_t height) {
    return width && height ? 2.4f*static_cast<float>(height)/width : 0.f;
}
inline DXGI_FORMAT FrontEndColorFamily(DXGI_FORMAT format) {
    switch(format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:case DXGI_FORMAT_B8G8R8X8_UNORM:case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_TYPELESS;
    default:return format;
    }
}
inline bool FrontEndCopyCompatible(DXGI_FORMAT source,DXGI_FORMAT destination) {
    return source!=DXGI_FORMAT_UNKNOWN && FrontEndColorFamily(source)==FrontEndColorFamily(destination);
}
inline DXGI_FORMAT FrontEndSrgbFormat(DXGI_FORMAT source) {
    switch(FrontEndColorFamily(source)) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    default:return source;
    }
}
}
