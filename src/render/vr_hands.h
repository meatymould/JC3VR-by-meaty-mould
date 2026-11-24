#pragma once
#include <vector>

namespace jc3vr {
// One vertex of the procedural VR hand, in the OpenXR grip-pose frame:
// palms face the midline (+X is the left palm's normal, -X the right's),
// -Z runs along the handle from little finger to thumb, and extended
// fingers point along -Y (away from the user).
struct HandMeshVertex {
    float position[3];
    float normal[3];
    float color[3];
};

// 0 = open, 1 = fully curled. Index follows the trigger, the other three
// fingers follow the grip, the thumb drops when a face button is pressed.
struct HandCurl {
    float index = 0.0f;
    float grip = 0.0f;
    float thumb = 0.0f;
};

// Rico-styled articulated hand (palm, 3-joint fingers, 2-joint thumb,
// forearm; grappling gauntlet on the left wrist) as a triangle list.
void BuildHandMesh(bool leftHand, const HandCurl& curl, std::vector<HandMeshVertex>& out);

// Meta Quest (Touch Plus style) controller in the same grip-pose frame:
// handle along Z, tracking-free head at the -Z end with thumbstick and two
// face buttons, trigger on the finger (-Y) side. Trigger/grip values move
// the trigger and grip button.
void BuildControllerMesh(bool leftHand, const HandCurl& curl, std::vector<HandMeshVertex>& out);

// Thin aim line in the OpenXR aim-pose frame: from the origin along -Z for
// `length` metres (appended to `out`).
void BuildAimBeam(float length, float width, const float color[3], std::vector<HandMeshVertex>& out);
}
