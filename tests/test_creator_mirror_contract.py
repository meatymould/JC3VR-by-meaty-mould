"""Presentation contracts that can run from a public source archive."""
import hashlib
import json
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class CreatorContract(unittest.TestCase):
    def test_submission_precedes_desktop_copy(self):
        source = (ROOT / "src/openxr/xr_session.cpp").read_text()
        begin = source.index("OpenXrBootstrapResult SubmitNativeAlternateFrame(")
        body = source[begin:source.index("\nfloat Degrees(", begin)]
        self.assertLess(body.index("state.endFrame(state.session"), body.index("CopyCreatorMirror("))
        self.assertIn("if(g_creatorMirrorEye!=CreatorMirrorEye::Off)creatorDesktop=backbuffer;", body)
        mirror = body[body.index("if(creatorDesktop)"):]
        for forbidden in ["releaseImage(", "SetNative", "RenderNativeEye"]:
            self.assertNotIn(forbidden, mirror)

    def test_release_preserves_gameplay_sources(self):
        baseline = json.loads((ROOT / "tests/gameplay_source_baseline.json").read_text())
        for name, expected in baseline.items():
            content = (ROOT / name).read_bytes().replace(b"\r\n", b"\n")
            self.assertEqual(hashlib.sha256(content).hexdigest(), expected, name)

    def test_no_extra_scene_draw_readback_or_gpu_wait(self):
        header = (ROOT / "src/render/creator_mirror.h").read_text()
        self.assertEqual(header.count("context->CopyResource("), 1)
        for forbidden in ["->Draw", "->Flush", "->Map", "CreateTexture", "GetData", "->Clear", "->OMSet", "->RSSet"]:
            self.assertNotIn(forbidden, header)


if __name__ == "__main__":
    unittest.main()
