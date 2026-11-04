#!/usr/bin/env python3
"""world-console.py, the release's copy: one allowlisted line to the
portable WoW server's console, the same words as the PC's server/tools/world-console.py, which talks to
the Docker test server. Shipped as WOW-BG3/tools/server/tools/world-console.py, where the tools look
for it (ROOT/'server/tools/world-console.py'). The server is the one relenv.py names (WOW_BG3_SERVER,
WOWBG3_SERVER_VAR); output is what the console printed.

  world-console.py tele CHARACTER LOC | revive CHARACTER | saveall | raw 'server info'
  world-console.py wowbg3 HERO VERB ... | wowbg3 - mmtile MAP X Y | wowbg3 inv|loot|vendor SUB CHAR ...
  world-console.py bot level NAME N|follow | level NAME
"""
import re, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'link/tools'))
import relenv  # noqa: E402

READ_ONLY = {'server info', 'account onlinelist'}
WOWBG3_VERBS = {'bot', 'move', 'near', 'feed', 'view', 'hold', 'unhold', 'health', 'revive', 'status',
                'inv', 'itemdata', 'equip', 'unequip', 'moveitem', 'additem', 'delitem', 'money', 'give', 'loot',
                'vendor', 'buy', 'sell', 'side'}
TOKEN = re.compile(r'[A-Za-z0-9.,_:!-]{1,40}')
NAME = re.compile(r'[A-Za-z]{2,12}')
# `loot take`'s entries (server patch 0025, inventory_bridge.py take_token): IDX, money or all, each with the looter's
# place at BG3's take, `@X/Y/Z`, comma-separated; the console reads at most 255 characters a line.
_NUM = r'-?[0-9]{1,6}(?:\.[0-9]{1,3})?'
_ENTRY = r'(?:[0-9]{1,3}|money|all)(?:@%s/%s/%s)?' % (_NUM, _NUM, _NUM)
TAKE = re.compile(r'%s(?:,%s){0,15}' % (_ENTRY, _ENTRY))


def line_for(args):
    """The console line for these words, or None when they are not allowed."""
    a = list(args)
    if len(a) == 3 and a[0] == 'tele' and NAME.fullmatch(a[1]) and re.fullmatch(r'[A-Za-z0-9]{2,40}', a[2]):
        return 'tele name %s %s' % (a[1], a[2])
    if len(a) == 2 and a[0] == 'revive' and NAME.fullmatch(a[1]):
        return 'revive ' + a[1]
    if a == ['saveall']:
        return 'saveall'
    if len(a) == 2 and a[0] == 'raw' and a[1] in READ_ONLY:
        return a[1]
    if (len(a) == 6 and a[:3] == ['wowbg3', 'loot', 'take'] and NAME.fullmatch(a[3]) and TOKEN.fullmatch(a[4])
            and TAKE.fullmatch(a[5]) and len(' '.join(a)) <= 240):
        return ' '.join(a)
    if len(a) >= 3 and a[0] == 'wowbg3' and all(TOKEN.fullmatch(w) for w in a[1:]) and (
            a[1] in ('inv', 'loot', 'vendor') or (NAME.fullmatch(a[1]) and a[2] in WOWBG3_VERBS)
            or (a[1] == '-' and a[2] == 'mmtile')):
        return 'wowbg3 ' + ' '.join(a[1:])
    if len(a) == 4 and a[:2] == ['bot', 'level'] and NAME.fullmatch(a[2]) and re.fullmatch(r'[0-9]{1,2}|follow', a[3]):
        return 'wowbg3 - level %s %s' % (a[2], a[3])
    if len(a) == 2 and a[0] == 'level' and NAME.fullmatch(a[1]):
        return 'wowbg3 - level ' + a[1]
    return None


def send(args, seconds=3):
    """(exit code, printed text) for these words."""
    line = line_for(args)
    if line is None:
        return 2, __doc__
    import wowserver
    return 0, wowserver.console(line, seconds)


if __name__ == '__main__':
    relenv.configure()
    rc, out = send(sys.argv[1:])
    print(out)
    sys.exit(rc)
