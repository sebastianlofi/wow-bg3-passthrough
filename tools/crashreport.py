#!/usr/bin/env python3
"""crashreport.py: one zip with what it takes to fix a crash or a bug, for a player to send.

  crashreport.py [REASON]   writes WOWBG3_DATA/reports/WOW-BG3-report-TIME.zip and prints its path

lifecycle.py makes one when benilla dies, and `/wowbg3 report` makes one on demand. It holds the tails of the
logs, benilla's crash reports, the kernel's GPU lines and GPU dump, and what the machine is. Your home folder
reads as ~, your user name as USER, the PC's name as HOST, and a line that names a password, secret or token is
blanked. Passwords files (account.env, secrets.env), saves and game files never go in.
"""
import getpass, glob, os, platform, re, subprocess, sys, time, zipfile
from pathlib import Path

TAIL_LINES = 30000
MAX_DUMP = 32 * 1024 * 1024
SECRET = re.compile(r'(?i)(pass(word)?|secret|token|api[_-]?key)\b\s*[=:]')
ANSI = re.compile(r'\x1b\[[0-9;]*m')


def whole(word):
    """A pattern for `word` standing alone (not inside a longer name or number), or None for no word."""
    return re.compile(r'(?<![A-Za-z0-9_-])%s(?![A-Za-z0-9_-])' % re.escape(word)) if word else None


def clean(text, home=None, user=None, host=None):
    """Colour codes out; a line naming a password cut there; the home folder as ~, the user name as USER and the
    PC's name (with and without its domain) as HOST, each only where it stands alone."""
    home = str(Path.home()) if home is None else home
    user = getpass.getuser() if user is None else user
    host = platform.node() if host is None else host
    names = [(whole(host), 'HOST'), (whole(host.split('.')[0]), 'HOST'), (whole(user), 'USER')]
    lines = []
    for line in ANSI.sub('', text).splitlines():
        if SECRET.search(line):
            line = line[:SECRET.search(line).start()] + '[blanked]'
        line = line.replace(home, '~') if home else line
        for pattern, name in names:
            if pattern:
                line = pattern.sub(name, line)
        lines.append(line)
    return '\n'.join(lines) + '\n'


def tail(path):
    """The last TAIL_LINES lines of a text file, without reading a huge one whole."""
    with open(path, 'rb') as f:
        f.seek(0, os.SEEK_END)
        f.seek(max(0, f.tell() - 16 * 1024 * 1024))
        raw = f.read()
    return b'\n'.join(raw.splitlines()[-TAIL_LINES:]).decode('utf-8', 'replace')


def run(*cmd):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        return r.stdout + r.stderr
    except (OSError, subprocess.SubprocessError) as e:
        return '%s: %s\n' % (cmd[0], e)


def mod_data():
    """The mod's data folder, Mods/WOW-BG3/data: data/ in the WOW-BG3 folder ($WOWBG3_HOME, else the folder
    above this file that holds launcher.conf)."""
    if os.environ.get('WOWBG3_HOME'):
        return Path(os.environ['WOWBG3_HOME']) / 'data'
    for d in Path(__file__).resolve().parents:
        if (d / 'launcher.conf').is_file():
            return d / 'data'
    return Path.home() / ".local/share/Larian Studios/Baldur's Gate 3/Mods/WOW-BG3/data"


def make(reason='asked'):
    data = Path(os.environ.get('WOWBG3_DATA') or mod_data())
    out = data / 'reports'
    out.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime('%Y%m%d-%H%M%S')
    path = out / ('WOW-BG3-report-%s.zip' % stamp)
    notes = []
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        def text(name, src):
            try:
                z.writestr(name, clean(tail(src)))
            except OSError as e:
                notes.append('%s: %s' % (name, e))
        for f in sorted(glob.glob(str(data / 'logs/*.log'))):
            text('logs/' + Path(f).name, f)
        text('logs/layer.log', data / 'capture/layer.log')
        bg3le = sorted(glob.glob(str(Path.home() / '.local/share/bg3le/logs/*.log')), key=os.path.getmtime)
        if bg3le:
            text('logs/bg3le.log', bg3le[-1])
        diag = sorted(glob.glob(str(data / 'benilla/Diagnostics/*')), key=os.path.getmtime)
        for f in diag[-4:]:
            text('crash/' + Path(f).name, f)
        for i, f in enumerate(glob.glob('/sys/class/drm/card*/device/devcoredump/data')):
            try:
                with open(f, 'rb') as d:
                    z.writestr('crash/gpu-dump-%d.txt' % i, d.read(MAX_DUMP))
            except OSError as e:
                notes.append('%s: %s' % (f, e))
        kernel = [line for line in run('dmesg', '-T').splitlines() if re.search(r'(?i)amdgpu|nouveau|nvidia|i915|xe |gpu|drm', line)]
        z.writestr('system/kernel-gpu.txt', clean('\n'.join(kernel[-400:])))
        z.writestr('system/machine.txt', clean('\n'.join([platform.platform(), run('uname', '-srvmo'), run('sh', '-c', 'cat /etc/os-release'),
                                                          run('sh', '-c', 'lspci | grep -iE "vga|3d"'), run('sh', '-c', 'vulkaninfo --summary')])))
        z.writestr('report.txt', 'reason: %s\ntime: %s\nnot kept:\n%s\n' % (reason, time.strftime('%c'), clean('\n'.join(notes))))
    prune(out)
    return path


KEEP = 5  # reports kept; one is made at every crash of the WoW side, up to tens of MB each


def prune(out):
    """The newest KEEP reports stay; older ones go (a player sends the newest)."""
    reports = sorted(out.glob('WOW-BG3-report-*.zip'), key=lambda p: p.stat().st_mtime)
    for old in reports[:-KEEP]:
        try:
            old.unlink()
        except OSError:
            pass


if __name__ == '__main__':
    print(make(' '.join(sys.argv[1:]) or 'asked'))
