#pragma once

namespace jc3vr {
// Read-only call probe for JC3's grapple classes (RE step 2). Redirects the
// CGrapplingHook / CGrapplingHookWire / CGrapplingHookWireEndCastFilter
// vtable slots through pass-through stubs that count calls and remember the
// last rcx/rdx/r8/r9, then jump to the original. Nothing else is changed.
// Off unless grapple_probe=1; verifies each vtable's RTTI name first.
void ConfigureGrappleProbe(bool enabled);
bool InstallGrappleProbe();
// Called once per Present; logs per-slot call deltas about once a second,
// with argument samples for rarely called slots.
void GrappleProbeTick(bool grappleButtonHeld);
}
