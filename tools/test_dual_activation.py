"""Integration guards for the video-reported inactive dual firing path."""
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]

class DualActivationTests(unittest.TestCase):
    def test_zoom_is_not_fire_permission(self):
        source=(ROOT/'src/game/dual_wield.cpp').read_text()
        begin=source.index('bool ReadPair(')
        end=source.index('bool ReadFireState(',begin)
        gate=source[begin:end]
        self.assertNotIn('weapon+0x466',gate)
        self.assertIn('SupportedDualFireMechanics(',gate)
        self.assertIn('parent!=weapon',gate)
        self.assertIn('seen.HandFor(pair.gun[index],now)',gate)
        header=(ROOT/'src/game/dual_wield.h').read_text()
        self.assertIn('now-entry.tick<=250',header)
        self.assertIn('pair.hand[0]==pair.hand[1]',gate)

    def test_component_identity_not_last_hand_only(self):
        source=(ROOT/'src/game/dual_wield.cpp').read_text()
        self.assertIn('DualGunPlacements placements',source)
        self.assertIn('placements.Record(data,hand,GetTickCount64())',source)
        self.assertIn('pair.gun[index]',source)
        self.assertIn('placements.HandFor(data,now)',source)

    def test_missing_activation_has_bounded_explanation(self):
        source=(ROOT/'src/game/dual_wield.cpp').read_text()
        self.assertIn('eligibility rejected:',source)
        self.assertIn('reported.fetch_or(bit)',source)
        self.assertIn('caller!=image+0x9A419F',source)
        self.assertIn('RestoreDualAim(aim,directionMode,aimBackup)',source)

    def test_native_idle_sentinel_not_rejected(self):
        header=(ROOT/'src/game/dual_wield.h').read_text()
        self.assertIn('(forceCount==-1 || forceCount==0)',header)
        source=(ROOT/'src/game/dual_wield.cpp').read_text()
        self.assertIn('force_count=%d',source)
        self.assertIn('summaries<12',source)
        self.assertIn('caller_reject=',source)
        # No worker-side trace formatting/writes in either firing function.
        begin=source.index('bool RunPair(')
        end=source.index('bool InstallDualWieldHooks()',begin)
        self.assertNotIn('TraceRuntime(',source[begin:end])

if __name__=='__main__':unittest.main()
