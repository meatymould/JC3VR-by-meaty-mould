"""Integration guards: owned gauntlet data cannot enter Rico's palette rewrite."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GauntletUpload(unittest.TestCase):
    def test_owned_attachment_exits_before_any_palette_matching(self):
        source = (ROOT / "src/game/vr_body_rig.cpp").read_text()
        body = source[source.index("void VrBodyRewriteUpload("):source.index("void ConfigureVrBodyHideAll(")]
        start = body.index("if (IsLocalGauntletUpload(")
        end = body.index("if (g_rewriteKey", start)
        guarded = body[start:end]
        self.assertIn("return;", guarded)
        self.assertIn("std::memcpy(data, copy.data(), 64)", guarded)
        self.assertNotIn("g_written[", guarded)
        self.assertNotIn("memcpy(data, copy.data(), desc.ByteWidth)", guarded)
        self.assertLess(start, body.index("std::vector<std::pair<std::size_t, int>> hits"))

    def test_both_upload_routes_keep_the_guarded_rewrite(self):
        source = (ROOT / "src/game/camera_capture.cpp").read_text()
        self.assertIn("VrBodyRewriteUpload(resource, bodyCopy.data())", source)
        self.assertIn("VrBodyRewriteUpload(resource, mapped.bytes)", source)


if __name__ == "__main__":
    unittest.main()
