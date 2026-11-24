#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace jc3vr {
enum class AircraftKind { None, Helicopter, Plane };
struct VehicleCameraAnchor {
    bool valid=false; float position[3]{},basis[9]{},eyeModel[3]{};
    AircraftKind aircraft=AircraftKind::None;
    bool driver=false; std::uintptr_t vehicle=0; float seatYaw=0;
    bool traversal=false; // position anchor only; NOT a seated control mode
};
inline VehicleCameraAnchor BuildVehicleCameraAnchor(const float world[16],const float eyeModel[3]) {
    VehicleCameraAnchor a;
    for(int i=0;i<16;++i) if(!std::isfinite(world[i])) return a;
    if(std::abs(world[15]-1)>0.001f || std::abs(world[3])+std::abs(world[7])+std::abs(world[11])>0.001f) return a;
    for(int i=0;i<3;++i) {
        if(!std::isfinite(eyeModel[i]) || std::abs(eyeModel[i])>5) return a;
        a.eyeModel[i]=eyeModel[i];
        a.position[i]=world[12+i];
        for(int axis=0;axis<3;++axis) a.position[i]+=eyeModel[axis]*world[axis*4+i];
    }
    for(int axis=0;axis<3;++axis) {
        float length=0;
        for(int i=0;i<3;++i) length+=world[axis*4+i]*world[axis*4+i];
        if(length<0.9f || length>1.1f) return {};
        for(int i=0;i<3;++i) a.basis[axis*3+i]=world[axis*4+i]/std::sqrt(length);
    }
    for(int x=0;x<3;++x) for(int y=x+1;y<3;++y) {
        float dot=0; for(int i=0;i<3;++i) dot+=a.basis[x*3+i]*a.basis[y*3+i];
        if(std::abs(dot)>0.02f) return {};
    }
    const auto* b=a.basis;
    const float det=b[0]*(b[4]*b[8]-b[5]*b[7])-b[1]*(b[3]*b[8]-b[5]*b[6])+b[2]*(b[3]*b[7]-b[4]*b[6]);
    if(std::abs(det-1)>0.03f) return {};
    a.valid=true; return a;
}
// Preserve the occupied seat's yaw, but inherit pitch/roll from the actual
// aircraft rather than Rico's potentially upright character attachment.
inline bool AircraftSeatYaw(const float world[16],const float seatBasis[9],float& yaw) {
    const float zero[3]{};
    if(!BuildVehicleCameraAnchor(world,zero).valid) return false;
    float x=0,z=0;
    for(int c=0;c<3;++c) {
        if(!std::isfinite(seatBasis[c])) return false;
        x+=seatBasis[c]*world[c]; z+=seatBasis[c]*world[8+c];
    }
    if(x*x+z*z<0.1f) return false;
    yaw=std::atan2(z,x); return true;
}
inline bool ApplyAircraftBank(VehicleCameraAnchor& seat,const float world[16],float yaw) {
    const float zero[3]{};
    if(!seat.valid || !std::isfinite(yaw) || !BuildVehicleCameraAnchor(world,zero).valid) return false;
    const float cs=std::cos(yaw),sn=std::sin(yaw);
    for(int c=0;c<3;++c) {
        seat.basis[c]=world[c]*cs+world[8+c]*sn;
        seat.basis[3+c]=world[4+c];
        seat.basis[6+c]=-world[c]*sn+world[8+c]*cs;
    }
    return true;
}
// Measured engine render fraction, not elapsed-time/velocity extrapolation.
inline bool ComposeVehicleHeadBasis(const float frame[9],const float head[9],float out[9]) {
    float frameWorld[16]{},headWorld[16]{};frameWorld[15]=headWorld[15]=1;
    for(int axis=0;axis<3;++axis) for(int c=0;c<3;++c) {
        frameWorld[axis*4+c]=frame[axis*3+c];headWorld[axis*4+c]=head[axis*3+c];
    }
    const float zero[3]{};
    if(!BuildVehicleCameraAnchor(frameWorld,zero).valid || !BuildVehicleCameraAnchor(headWorld,zero).valid) return false;
    for(int axis=0;axis<3;++axis) for(int c=0;c<3;++c) {
        out[axis*3+c]=0;
        for(int local=0;local<3;++local) out[axis*3+c]+=head[axis*3+local]*frame[local*3+c];
    }
    return true;
}
inline void VehicleLocalOffset(const float frame[9],float right,float up,float forward,float out[3]) {
    for(int c=0;c<3;++c) out[c]=right*frame[c]+up*frame[3+c]-forward*frame[6+c];
}
// Installed keymap: HELI_TURN_LEFT/RIGHT = RSTICKLEFT/RIGHT. Deflection
// steers native yaw; never teleports the aircraft or changes the XR rig.
inline float HelicopterGazeYaw(float right,float forward,float manual) {
    if(!std::isfinite(manual)) return 0;
    if(std::abs(manual)>0.15f) return std::clamp(manual,-1.0f,1.0f);
    if(!std::isfinite(right)||!std::isfinite(forward)||right*right+forward*forward<0.01f) return manual;
    constexpr float dead=0.13962634f,full=0.78539816f;
    const float angle=std::atan2(right,forward);
    return std::copysign(0.65f*std::clamp((std::abs(angle)-dead)/(full-dead),0.0f,1.0f),angle);
}
inline bool InterpolateVehicleRoot(const float previous[16], const float next[16], float alpha, float out[16]) {
    const float zero[3]{};
    if (!std::isfinite(alpha) || alpha<0 || alpha>1 ||
        !BuildVehicleCameraAnchor(previous,zero).valid || !BuildVehicleCameraAnchor(next,zero).valid) return false;
    float travel=0; for(int c=0;c<3;++c) { const float d=next[12+c]-previous[12+c];travel+=d*d; }
    if (!std::isfinite(travel) || travel>100.0f*100.0f) return false;
    float m[16]{}; m[15]=1;
    for (int row=0;row<3;++row) for(int c=0;c<3;++c)
        m[row*4+c]=previous[row*4+c]*(1-alpha)+next[row*4+c]*alpha;
    float length=0; for(int c=0;c<3;++c) length+=m[c]*m[c];
    if(length<0.25f) return false;
    for(int c=0;c<3;++c) m[c]/=std::sqrt(length);
    float dot=0; for(int c=0;c<3;++c) dot+=m[c]*m[4+c];
    length=0; for(int c=0;c<3;++c) {m[4+c]-=dot*m[c];length+=m[4+c]*m[4+c];}
    if(length<0.25f) return false;
    for(int c=0;c<3;++c) m[4+c]/=std::sqrt(length);
    m[8]=m[1]*m[6]-m[2]*m[5]; m[9]=m[2]*m[4]-m[0]*m[6]; m[10]=m[0]*m[5]-m[1]*m[4];
    for(int c=0;c<3;++c) m[12+c]=previous[12+c]*(1-alpha)+next[12+c]*alpha;
    for(int i=0;i<16;++i) out[i]=m[i];
    return true;
}
void PublishVehicleCameraAnchor(const VehicleCameraAnchor& anchor, std::uintptr_t character=0);
// Camera consumers see traversal anchors; weapon/vehicle-control consumers do not.
VehicleCameraAnchor GetFirstPersonCameraAnchor(bool renderInterpolated=false);
inline bool NeedsTraversalCamera(bool seated,bool freefall,bool fastfall,bool preHang,bool grappleAttached,bool helicopterAttached) {
    return !seated && (freefall || fastfall || preHang || grappleAttached || helicopterAttached);
}
inline bool SetTraversalYawFrame(VehicleCameraAnchor& anchor,const float right[3]) {
    if(!anchor.valid || !anchor.traversal || !std::isfinite(right[0]) || !std::isfinite(right[2])) return false;
    const float length=std::hypot(right[0],right[2]);
    if(length<0.2f) return false;
    const float x=right[0]/length,z=right[2]/length;
    const float basis[9]{x,0,z,0,1,0,-z,0,x};
    std::copy(basis,basis+9,anchor.basis);return true;
}
void SetVehicleRenderInterpolation(float fraction);
VehicleCameraAnchor GetVehicleCameraAnchor(bool renderInterpolated=false);
bool GetCharacterRenderPosition(std::uintptr_t character,float position[3]);
}
