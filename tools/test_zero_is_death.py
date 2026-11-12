#!/usr/bin/env python3
"""An enemy BG3 reads at 0 HP is dead only when BG3 says so.

A zombie's Undead Fortitude: BG3 read 1, 0 and 1 HP within 0.3 s. The 0 went to WoW as the kill, WoW's zombie died
and BG3's fought on at 1 HP. LinkLive.lua now reports an enemy's 0 only with the server's word that the stand-in is
dead (mirror-status.json: Dead is BG3's IsDead) or gone. Needs the `lupa` module for the Lua part.

  python3 tools/test_zero_is_death.py
"""
from pathlib import Path

LUA = Path(__file__).resolve().parent.parent / 'mod/ScriptExtender/Lua'
TEXT = (LUA / 'LinkLive.lua').read_text()


def test_the_reading_asks_the_servers_record_before_a_zero_counts():
    body = TEXT[TEXT.index('    local function memberReading(id, uuid)'):TEXT.index('    -- Whether BG3 has this enemy standing')]
    assert 'if hp <= 0 and id ~= "hero" and inWorld and guids[id] and not M.ZeroIsDeath(serverRecord(id, uuid)) then' in body
    assert 'if now <= 0 then return nil end' in body, "the server's own 0 HP with Dead false is no kill either"
    assert body.index('if m.Dead == true then return 0, maxHp, at end') < body.index('if now <= 0 then return nil end')


def test_zero_is_death_only_by_bg3s_word():
    try:
        import lupa
    except ImportError:
        print('test_zero_is_death.py: the Lua part needs the lupa module; skipped')
        return
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    src = TEXT[TEXT.index('function M.ZeroIsDeath(record)'):]
    src = src[:src.index('\nend\n') + 5]
    L.execute('M = {}\n' + src)
    f = L.globals().M.ZeroIsDeath
    t = L.eval('function(dead, hp) return {Dead = dead, Hp = hp, MaxHp = 10} end')
    assert f(t(False, 0)) is False, 'Undead Fortitude: 0 HP, BG3 has it alive'
    assert f(t(True, 0)) is True, 'dead by BG3'
    assert f(t(None, 0)) is False, 'a record without the word is not a death'
    assert f(False) is True, "BG3's server has no such stand-in any more"
    assert f(None) is None, 'the record unread: no word, no reading'


if __name__ == '__main__':
    for k, f in sorted(globals().items()):
        if k.startswith('test_') and callable(f):
            f()
            print('ok', k)
