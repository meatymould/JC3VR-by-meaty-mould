#include "render/controller_model.h"
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <array>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cmath>
#include <cstring>

namespace jc3vr {
namespace {
bool ReadFile(const std::wstring& path, std::string& text) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    text = buffer.str();
    return true;
}

// Resolves a 1-based (or negative, relative) OBJ index.
int ResolveIndex(int index, std::size_t count) {
    if (index > 0) return index - 1;
    if (index < 0) return static_cast<int>(count) + index;
    return -1;
}

bool DecodePng(const std::wstring& path, std::vector<std::uint8_t>& rgba, unsigned& width, unsigned& height) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    {
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        UINT w = 0, h = 0;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) &&
            SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(converter->GetSize(&w, &h)) && w && h && w<=4096 && h<=4096) {
            rgba.resize(static_cast<std::size_t>(w) * h * 4);
            ok = SUCCEEDED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(rgba.size()), rgba.data()));
            width = w;
            height = h;
        }
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}
}

bool LoadRgbaPng(const std::wstring& path, std::vector<std::uint8_t>& rgba, unsigned& width, unsigned& height) {
    return DecodePng(path, rgba, width, height);
}

bool LoadInstalledChargeAsset(const std::wstring& directory,ControllerModelAsset& out,std::string& error) {
    const auto folder=std::filesystem::path(directory);
    std::ifstream file(folder/L"c4_inactive.mesh",std::ios::binary|std::ios::ate);
    if(!file || file.tellg()<12 || file.tellg()>12+24576*32) {error="charge cache absent/oversized";return false;}
    const auto size=static_cast<std::size_t>(file.tellg());file.seekg(0);
    char magic[4]{};std::uint32_t version=0,count=0;
    file.read(magic,4);file.read(reinterpret_cast<char*>(&version),4);file.read(reinterpret_cast<char*>(&count),4);
    if(std::memcmp(magic,"JC4M",4) || version!=1 || !count || count>24576 || count%3 || size!=12+count*32) {
        error="charge cache header invalid";return false;
    }
    ControllerModelAsset candidate;candidate.vertices.resize(count);
    file.read(reinterpret_cast<char*>(candidate.vertices.data()),count*sizeof(ControllerModelVertex));
    if(!file) {error="charge cache truncated";return false;}
    for(const auto& vertex:candidate.vertices) {
        float normalLength=0;
        for(int i=0;i<3;++i) {
            if(!std::isfinite(vertex.position[i]) || std::abs(vertex.position[i])>0.2f || !std::isfinite(vertex.normal[i])) {
                error="charge geometry invalid";return false;
            }
            normalLength+=vertex.normal[i]*vertex.normal[i];
        }
        if(normalLength<0.9f || normalLength>1.1f || !std::isfinite(vertex.uv[0]) || !std::isfinite(vertex.uv[1]) ||
            vertex.uv[0]<0 || vertex.uv[0]>1 || vertex.uv[1]<0 || vertex.uv[1]>1) {
            error="charge attributes invalid";return false;
        }
    }
    if(!LoadRgbaPng((folder/L"c4_diffuse.png").wstring(),candidate.rgba,candidate.width,candidate.height) ||
        candidate.width>1024 || candidate.height>1024) {error="charge diffuse unavailable";return false;}
    out=std::move(candidate);return true;
}

bool ParseObjTriangles(const std::string& text, std::vector<ControllerModelVertex>& out) {
    std::vector<std::array<float, 3>> positions, normals;
    std::vector<std::array<float, 2>> uvs;
    std::istringstream lines(text);
    std::string line;
    const std::size_t start = out.size();
    while (std::getline(lines, line)) {
        std::istringstream in(line);
        std::string kind;
        in >> kind;
        if (kind == "v") {
            std::array<float, 3> v{};
            in >> v[0] >> v[1] >> v[2];
            positions.push_back(v);
        } else if (kind == "vn") {
            std::array<float, 3> n{};
            in >> n[0] >> n[1] >> n[2];
            normals.push_back(n);
        } else if (kind == "vt") {
            std::array<float, 2> t{};
            in >> t[0] >> t[1];
            uvs.push_back(t);
        } else if (kind == "f") {
            std::vector<ControllerModelVertex> polygon;
            std::string token;
            while (in >> token) {
                int indices[3]{0, 0, 0};
                std::size_t field = 0, begin = 0;
                for (std::size_t i = 0; i <= token.size() && field < 3; ++i) {
                    if (i == token.size() || token[i] == '/') {
                        if (i > begin) indices[field] = std::atoi(token.substr(begin, i - begin).c_str());
                        ++field;
                        begin = i + 1;
                    }
                }
                const int p = ResolveIndex(indices[0], positions.size());
                const int t = ResolveIndex(indices[1], uvs.size());
                const int n = ResolveIndex(indices[2], normals.size());
                if (p < 0 || p >= static_cast<int>(positions.size())) { polygon.clear(); break; }
                ControllerModelVertex vertex{};
                for (int c = 0; c != 3; ++c) vertex.position[c] = positions[p][c];
                if (n >= 0 && n < static_cast<int>(normals.size())) for (int c = 0; c != 3; ++c) vertex.normal[c] = normals[n][c];
                if (t >= 0 && t < static_cast<int>(uvs.size())) {
                    vertex.uv[0] = uvs[t][0];
                    vertex.uv[1] = 1.0f - uvs[t][1];
                }
                polygon.push_back(vertex);
            }
            for (std::size_t i = 1; i + 1 < polygon.size(); ++i) {
                out.push_back(polygon[0]);
                out.push_back(polygon[i]);
                out.push_back(polygon[i + 1]);
            }
        }
    }
    return out.size() > start;
}

RenderModelComponent ParseRenderModelComponent(const std::string& json, const std::string& name) {
    RenderModelComponent component;
    const std::size_t key = json.find("\"" + name + "\"");
    if (key == std::string::npos) return component;
    const auto readTriple = [&](const char* field, float* values) {
        const std::size_t at = json.find(std::string("\"") + field + "\"", key);
        if (at == std::string::npos) return false;
        const std::size_t open = json.find('[', at);
        const std::size_t close = json.find(']', open);
        if (open == std::string::npos || close == std::string::npos) return false;
        std::string list = json.substr(open + 1, close - open - 1);
        for (char& c : list) if (c == ',') c = ' ';
        std::istringstream in(list);
        return static_cast<bool>(in >> values[0] >> values[1] >> values[2]);
    };
    component.found = readTriple("origin", component.origin) && readTriple("rotate_xyz", component.rotateXyzDegrees);
    return component;
}

std::wstring FindSteamVrRoot() {
    // %LOCALAPPDATA%\openvr\openvrpaths.vrpath lists the active runtime.
    wchar_t local[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    std::string json;
    if (length && length < MAX_PATH && ReadFile(std::wstring(local) + L"\\openvr\\openvrpaths.vrpath", json)) {
        const std::size_t runtime = json.find("\"runtime\"");
        const std::size_t open = runtime == std::string::npos ? std::string::npos : json.find('"', json.find('[', runtime));
        const std::size_t close = open == std::string::npos ? std::string::npos : json.find('"', open + 1);
        if (close != std::string::npos) {
            std::string path;
            for (std::size_t i = open + 1; i < close; ++i) {
                if (json[i] == '\\' && i + 1 < close && json[i + 1] == '\\') ++i;
                path += json[i];
            }
            if (!path.empty()) return std::wstring(path.begin(), path.end());
        }
    }
    return L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR";
}

bool LoadQuestPlusControllerModel(bool leftHand, ControllerModelAsset& out, std::string& error) {
    const std::wstring name = leftHand ? L"oculus_quest_plus_controller_left" : L"oculus_quest_plus_controller_right";
    const std::wstring folder = FindSteamVrRoot() + L"\\resources\\rendermodels\\" + name + L"\\";
    std::string obj, json;
    if (!ReadFile(folder + name + L".obj", obj) || !ReadFile(folder + name + L".json", json)) {
        error = "render model files not found under the SteamVR install";
        return false;
    }
    out.vertices.clear();
    if (!ParseObjTriangles(obj, out.vertices)) {
        error = "render model OBJ has no triangles";
        return false;
    }
    out.grip = ParseRenderModelComponent(json, "openxr_grip");
    out.aim = ParseRenderModelComponent(json, "openxr_aim");
    if (!out.grip.found) {
        error = "render model JSON has no openxr_grip component";
        return false;
    }
    if (!DecodePng(folder + name + L"_color.png", out.rgba, out.width, out.height)) {
        // Untextured fallback: a single mid-grey texel.
        out.rgba = {90, 90, 96, 255};
        out.width = out.height = 1;
    }
    return true;
}
}
