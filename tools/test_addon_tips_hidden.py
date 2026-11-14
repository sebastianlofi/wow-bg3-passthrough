#!/usr/bin/env python3
"""test_addon_tips_hidden.py: the WoW layout addon hides every frame of WoW's new-player tips. WoW 1.12's
FrameXML (TutorialFrame.xml) makes the "!" buttons TutorialFrameAlertButton1 to TutorialFrameAlertButton10
(MAX_TUTORIAL_ALERTS = 10) under TutorialFrameParent; the addon once hid "TutorialFrameAlertButton", a frame
that does not exist, and the buttons came back on every new character (a play report).

  python3 tools/test_addon_tips_hidden.py
"""
import re, unittest
from pathlib import Path

LUA = Path(__file__).resolve().parent.parent / 'wow-addon' / 'WowBg3Layout' / 'WowBg3Layout.lua'


class TipsHidden(unittest.TestCase):
    def setUp(self):
        self.src = LUA.read_text()
        self.hide = self.src[self.src.index('local HIDE = {'):self.src.index('local MICRO = {')]

    def test_numbered_buttons(self):
        m = re.search(r'for i = 1, (\d+) do HIDE\[table\.getn\(HIDE\) \+ 1\] = "TutorialFrameAlertButton" \.\. i end', self.hide)
        self.assertIsNotNone(m, 'the ten numbered alert buttons are not in HIDE')
        self.assertGreaterEqual(int(m.group(1)), 10)

    def test_no_bare_name(self):
        self.assertNotIn('"TutorialFrameAlertButton",', self.hide)

    def test_frames(self):
        for name in ('"TutorialFrame"', '"TutorialFrameParent"', '"DurabilityFrame"'):
            self.assertIn(name, self.hide)

    def test_cvar_off(self):
        self.assertIn('SetCVar("showTutorials", "0")', self.src)


if __name__ == '__main__':
    unittest.main()
