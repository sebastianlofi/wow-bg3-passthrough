#!/usr/bin/env python3
"""Nothing of the mod starts unless BG3 itself loaded WOW-BG3's module (mod/ScriptExtender/Lua/Module.lua).

The extender loads a mod's scripts when modsettings.lsx names the mod, and that file can change after BG3 has read
it (Steam Cloud put another machine's copy in place four seconds after a start). The scripts then ran in a BG3
without the module, took a New Game as theirs and sent the party to a level BG3 did not have: the player stood in
the character-creation level. These tests fail when a start-up script goes round the check, or when the check and
the launcher stop meaning the same module."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LUA = ROOT / 'mod/ScriptExtender/Lua'


def code(name):
    """A Lua file without its comments."""
    return '\n'.join(l.split('--')[0] for l in (LUA / name).read_text().splitlines())


def test_both_start_up_scripts_start_everything_through_the_check():
    for name, context in (('BootstrapClient.lua', 'client'), ('BootstrapServer.lua', 'server')):
        c = code(name)
        assert c.lstrip().startswith('Ext.Require("Module.lua").Start("%s", function()' % context), name
        # every other file is asked for inside that one call: nothing at the file's own level
        depth = 0
        for line in c.splitlines():
            if 'Ext.Require(' in line and 'Module.lua' not in line:
                assert depth > 0, '%s asks for a file outside the check: %s' % (name, line.strip())
            depth += len(re.findall(r'\bfunction\b', line)) - len(re.findall(r'\bend\b', line))


def test_the_check_reads_bg3s_own_module_list_and_not_the_settings_file():
    c = code('Module.lua')
    assert 'Ext._Internal.ModCount' in c and 'Ext._Internal.ModUuidAt' in c
    for word in ('ModSettingsOrder', 'IsModLoaded', 'GetLoadOrder', 'modsettings'):
        assert word not in c, 'Module.lua must not go by %s: it follows the settings file' % word


def test_the_check_and_the_launcher_mean_the_same_module_and_the_launcher_is_told():
    lua = code('Module.lua')
    c = (ROOT / 'plugins/launcher/wowbg3_launcher.c').read_text()
    uuid = re.search(r'M\.UUID = "([0-9a-f-]{36})"', lua).group(1)
    assert '#define MOD_UUID "%s"' % uuid in c
    assert 'Ext.Plugins.Set, LAUNCHER, "module"' in lua and 'local LAUNCHER = "wowbg3-launcher"' in lua
    assert 'add_setting(self, "module", BG3LE_SETTING_INT, &s_module, -1, 1)' in c
    # the file alone starts nothing and holds no loading screen
    assert 'case START_ENABLED: return mod_active();' in c
    assert 'case START_SESSION: return mod_active() && atomic_load(&s_session_started);' in c
    assert 'return s_mod_enabled && s_module != 0;' in c


if __name__ == '__main__':
    for k, f in sorted(globals().items()):
        if k.startswith('test_') and callable(f):
            f()
            print('ok', k)
