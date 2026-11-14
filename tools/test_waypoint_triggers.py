#!/usr/bin/env python3
"""test_waypoint_triggers.py: BG3's waypoint panel in a level of our own uses point triggers of that level. Its entries
used to sit on BGO_Main_A's triggers (the borrowed host before our own level), and a click from our level or from
camp was BG3's teleport into BGO_Main_A: world mode never started (the loading screen held on "WoW: logging in").
Waypoints.lua's OWN_TRIGGERS must be exactly the point triggers wowbg3_level.py writes for the release's level.

  python3 tools/test_waypoint_triggers.py
"""
import json, os, re, sys, tempfile, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import wowbg3_level  # noqa: E402

SPEC = ROOT / 'release/levels/azeroth_a.json'
LUA = ROOT / 'mod/ScriptExtender/Lua/Waypoints.lua'


def own_triggers():
    s = LUA.read_text()
    block = re.search(r'M\.OWN_TRIGGERS = \{(.*?)\n\}', s, re.S).group(1)
    return re.findall(r'"([0-9a-f-]{36})"', block)


class WaypointTriggers(unittest.TestCase):
    def setUp(self):
        self.spec = json.loads(SPEC.read_text())

    def test_lua_names_the_levels_triggers(self):
        n = self.spec.get('Waypoints', 0)
        self.assertGreaterEqual(n, 33)  # BG3's panel: at most 33 points a team (Waypoints.lua)
        want = [wowbg3_level.waypoint_uid(self.spec['Name'], i) for i in range(n)]
        self.assertEqual(own_triggers(), want)

    def test_the_level_has_them_as_point_triggers(self):
        d = tempfile.mkdtemp()
        wowbg3_level.build(dict(self.spec), d)  # the level folder as the release builds it
        x = open(os.path.join(d, 'Triggers', '_merged.lsx')).read()
        for u in own_triggers():
            i = x.index(u)
            node = x[x.rindex('<node id="GameObjects">', 0, i):x.index('</node>', i)]
            self.assertIn('15111ce4-12fc-4757-bb81-7c2c212e141a', node)  # BG3's point trigger template

    def test_own_level_switches_to_them(self):
        self.assertIn('if Hero.OWN then M.TRIGGERS = M.OWN_TRIGGERS end', LUA.read_text())


if __name__ == '__main__':
    unittest.main()
