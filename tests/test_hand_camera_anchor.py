"""Integration guards for native hand camera anchoring; not headset proof."""
from pathlib import Path
import unittest
ROOT = Path(__file__).resolve().parents[1]


class HandCameraAnchor(unittest.TestCase):
    def test_publisher_supplies_the_origin_used_to_construct_hands(self):
        source = (ROOT / "src/openxr/xr_session.cpp").read_text()
        self.assertIn("std::copy(engine, engine + 3, sampledHandCamera)", source)
        self.assertIn("sampledHandCameraValid ? sampledHandCamera : nullptr, rig[0].nativeOffset.baseAnchor", source)

    def test_render_and_simulation_do_not_add_both_displacements(self):
        source = (ROOT / "src/game/vr_body_rig.cpp").read_text()
        body = source[source.index("static bool GetHandTarget("):source.index("bool VrBodyActive()")]
        self.assertIn("if (render && g_targetCameraValid[hand])", body)
        self.assertIn("RebaseTrackedPoint(g_targetWorld[hand], g_targetCamera[hand], camera, grip)", body)
        self.assertIn("} else\n    if (g_targetCharacter[hand])", body)

    def test_owned_gauntlet_refresh_reuses_socket_builder(self):
        source = (ROOT / "src/game/vr_body_rig.cpp").read_text()
        body = source[source.index("static bool IsAlreadyPlacedWeapon("):source.index("// The hand-attach bones")]
        self.assertIn("g_placedGrapple.Contains(matrix, now)", body)
        self.assertIn("GetVrBodyRenderHandTarget(0, grip, fingers, down)", body)
        self.assertIn("BuildLeftWristMountLocked(45, grip, fingers, down, refreshed)", body)
        self.assertIn("g_placedGrapple.Publish(refreshed, now)", body)


if __name__ == "__main__":
    unittest.main()
