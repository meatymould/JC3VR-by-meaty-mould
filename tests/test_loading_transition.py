"""Structural guards for loading-path integration, not headset validation."""
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class LoadingTransition(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = (ROOT / "src/openxr/xr_session.cpp").read_text()
        start = source.index("OpenXrBootstrapResult SubmitNativeAlternateFrame(")
        end = source.index("\nvoid LogStereoCalibration(", start)
        cls.body = source[start:end]

    def test_loading_submission_cannot_skip_native_preparation(self):
        panel = self.body.index("return SubmitFrontEndPanel(")
        for operation in ("SetNativeEyeProjection(", "SetNativeCameraOffset(",
                          "SyncNativeCameraProjectionToEye(", "SyncPairedCameraProjectionToEye(",
                          "ArmPairedCameraUploadSync(", "UpdateHandAim("):
            self.assertLess(self.body.index(operation), panel, operation)
        self.assertEqual(self.body.count("return SubmitFrontEndPanel("), 1)
        prepared = self.body.index("ArmPairedCameraUploadSync(")
        self.assertLess(panel, self.body.index("backbuffer->Release();", prepared))

    def test_loading_does_not_relocate_gameplay_hud(self):
        self.assertIn("SetPreHudCapture(g_hudAlign && !frontEnd, backbuffer)", self.body)
        self.assertIn("if (!frontEnd && preHudCopy", self.body)

    def test_borrowed_pre_hud_pointer_not_released(self):
        self.assertNotRegex(self.body, r"ConsumePreHudCopy\(\).*?Release\(")
        self.assertNotIn("preHudCopy->Release", self.body)


if __name__ == "__main__":
    unittest.main()
