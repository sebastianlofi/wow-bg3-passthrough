#!/usr/bin/env python3
"""wowbg3-chatcmd.py: answers `/wowbg3 ...` typed in WoW's chat.

benilla (patch 0053) writes each command to LINK/chatcmd-request.txt; this helper runs it with the same
code as the PC tools (wowbg3cmd.py: the party verbs, tp, where, find, faction, level, xprate, help) and writes the answer lines
to LINK/chatcmd-reply.txt, which benilla prints in WoW's chat. Run beside benilla (the release's
coordinator starts it with the WoW side; on the PC harness by hand), no arguments:

  python3 link/tools/wowbg3-chatcmd.py

Environment: WOWBG3_LINK (benilla's link dir), WOWBG3_IO, WOW_CHAR, WOW_BG3_SERVER and
WOWBG3_SERVER_VAR (wowbg3cmd.py); WOWBG3_DEBUG_COMMANDS=1 allows the test commands (tp, where, find, faction,
level, xprate, refill), else `debug_commands = 1` in $WOWBG3_HOME/settings.conf; on in the beta.
Files (one line each, tab separated):
  request  `WCC1 <pid>`, then `<seq> <unix time> <WoW character> <text>`        (benilla's)
  reply    `<seq> <kind> <text>`, kind + a line, ! an error line, . the answer is complete   (ours)
Requests older than 30 s when the helper starts, or already answered in the reply file, are not run. SIGTERM: a clean exit.
"""
import json
import os
import signal
import sys
import threading
import time
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wowbg3cmd as W  # noqa: E402

KEEP_LINES = 400
STALE = 30
PANEL_EVERY = 60  # s


def log(*a):
    print(time.strftime('%H:%M:%S'), 'chatcmd:', *a, flush=True)


def debug_commands():
    v = os.environ.get('WOWBG3_DEBUG_COMMANDS')
    if v is not None and v.strip() != '':
        return v.strip() == '1'
    home = os.environ.get('WOWBG3_HOME')
    if home:
        try:
            for line in (Path(home) / 'settings.conf').read_text().splitlines():
                line = line.split('#', 1)[0]
                if '=' in line and line.split('=', 1)[0].strip() == 'debug_commands':
                    return line.split('=', 1)[1].strip() == '1'
        except OSError:
            pass
    return False


class Replies:
    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()
        try:
            self.lines = path.read_text(errors='replace').splitlines()[-KEEP_LINES:]
        except OSError:
            self.lines = []

    def add(self, seq, kind, text):
        with self.lock:
            self.lines.append('%d\t%s\t%s' % (seq, kind, text.replace('\t', ' ').replace('\r', ' ')))
            self.lines = self.lines[-KEEP_LINES:]
            W.atomic_write(self.path, '\n'.join(self.lines) + '\n')


def read_requests(path):
    out = []
    try:
        lines = path.read_text(errors='replace').splitlines()
    except OSError:
        return out
    for l in lines[1:]:
        p = l.split('\t', 3)
        if len(p) == 4 and p[0].isdigit() and p[1].isdigit():
            out.append((int(p[0]), int(p[1]), p[2], p[3]))
    return out


# One command of a kind at a time (party-command.json, world-move.json and the server console each take
# one job); different kinds run side by side, so a long teleport never holds up a party list.
LOCKS = {k: threading.Lock() for k in ('party', 'tele', 'server', 'other')}


def kind_of(text):
    """Which lock a command line takes, by its verb."""
    try:
        verb, rest = W.verb_of(text.split())
    except Exception:  # noqa: BLE001  unknown word or a usage error: W.run reports it
        return 'other'
    if verb in W.PARTY_ACTIONS:
        return 'party'
    if verb in ('tp', 'where', 'find') or (verb == 'waypoint' and rest[:1] == ['go']):
        return 'tele'
    if verb in ('level', 'xprate'):
        return 'server'
    return 'other'


def is_move(text):
    """Whether a command line moves the party. A second one while one runs is refused, not queued: clicks made
    during a long wait each moved the party again, one after the other, for minutes."""
    try:
        verb, rest = W.verb_of(text.split())
    except Exception:  # noqa: BLE001  W.run reports it
        return False
    return verb == 'tp' or (verb == 'waypoint' and rest[:1] == ['go'])


def handle(replies, seq, who, text, refused=None):
    first = [True]

    def out(line, error=False):
        for l in (str(line).splitlines() or ['']):
            if first[0]:
                l, first[0] = 'WOW-BG3: ' + l, False
            replies.add(seq, '!' if error else '+', l)
    ctx = W.Ctx(char=who or None, debug=debug_commands(), out=out, chat=True)
    t0 = time.time()
    try:
        lock = LOCKS[kind_of(text)]
        if not lock.acquire(blocking=not is_move(text)):
            raise W.CmdError('already on the way')
        try:
            W.run(ctx, text)
        finally:
            lock.release()
        result = 'ok'
    except W.CmdError as e:
        out(str(e) or 'failed', True)
        result = 'refused: %s' % str(e).splitlines()[0] if str(e) else 'refused'
        if refused:
            refused(str(e))
    except Exception as e:  # noqa: BLE001
        out('the command failed: %s' % e, True)
        result = 'error'
        traceback.print_exc()
    finally:
        replies.add(seq, '.', '')
    log('#%d %r from %s: %s in %.1f s' % (seq, text, who or '?', result, time.time() - t0))


def panel_loop():
    """What BG3's own waypoint panel lists (a discovery only reaches the table when the server saves)."""
    ctx = W.Ctx(debug=False, out=lambda *_: None, chat=True)
    while True:
        try:
            with LOCKS['server']:
                W.waypoint_panel(ctx)
            time.sleep(PANEL_EVERY)
        except Exception:  # noqa: BLE001  WoW's character not online yet
            time.sleep(5)


def stamp_of(path):
    try:
        st = path.stat()
        return st.st_mtime_ns, st.st_size
    except OSError:
        return None


def main():
    link = Path(os.environ.get('WOWBG3_LINK') or W.Ctx().link)
    req = link / 'chatcmd-request.txt'
    replies = Replies(link / 'chatcmd-reply.txt')
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    start = time.time()
    # Not again: requests older than STALE, and those this helper already answered before a restart.
    answered = set()
    for l in replies.lines:
        p = l.split('\t')
        if len(p) >= 2 and p[1] == '.' and p[0].isdigit():
            answered.add(int(p[0]))
    done = set((s, t) for s, t, _, _ in read_requests(req) if t < start - STALE or s in answered)
    log('watching %s (test commands %s; server %s)' % (req, 'on' if debug_commands() else 'off',
                                                        os.environ.get('WOW_BG3_SERVER') or 'Docker test server'))
    threading.Thread(target=panel_loop, daemon=True).start()
    click = link / 'waypoint-click.json'  # a click in BG3's own panel (Waypoints.lua)
    last_click = stamp_of(click)
    last = None
    while True:
        stamp = stamp_of(click)
        if stamp and stamp != last_click:
            last_click = stamp
            try:
                text = 'waypoint go %d' % json.loads(click.read_text())['Id']
                seq = int(time.time() * 10)
                log('#%d %r from the panel' % (seq, text))
                # WoW's chat prints only answers to its own requests: a refusal goes back to the panel's side.
                said = lambda why, s=seq: W.atomic_write(link / 'waypoint-refused.json', json.dumps({'Seq': s, 'Text': why}))
                threading.Thread(target=handle, args=(replies, seq, None, text, said), daemon=True).start()
            except (OSError, ValueError, KeyError, TypeError) as e:
                log('panel click: %s' % e)
        stamp = stamp_of(req)
        if stamp and stamp != last:
            last = stamp
            for s, t, who, text in read_requests(req):
                if (s, t) in done:
                    continue
                done.add((s, t))
                if t < start - STALE:
                    continue
                log('#%d %r from %s' % (s, text, who or '?'))
                threading.Thread(target=handle, args=(replies, s, who, text), daemon=True).start()
        time.sleep(0.2)


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        pass
