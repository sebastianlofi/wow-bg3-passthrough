"""wowserver.py: which WoW server a tool talks to.

WOW_BG3_SERVER=FOLDER (the release's launcher and coordinator set it, and so does a test harness): a portable
server (server/wowbg3-server.sh): the shell command runs on the host with `mariadb` and
`mariadb-dump` on PATH, MYSQL_UNIX_PORT at its socket and MARIADB_ROOT_PASSWORD set, and console lines go
through `wowbg3-server.sh console`.
WOW_BG3_SERVER unset: only in the source tree, the developers' test server (devserver.py, which no release
carries); in a release it is an error that says so.

    db_sh(cmd, stdin=None, container='wow-bg3-database-1') -> subprocess.CompletedProcess (text)
    console(line, seconds=3, container='wow-bg3-mangosd-1') -> what the console printed
    mangosd_running() -> whether the world server runs (wowsave.py rehome needs it stopped)
"""
import os, subprocess
from pathlib import Path


def portable():
    p = os.environ.get('WOW_BG3_SERVER')
    return Path(p) if p else None


def dev():
    """The developers' test server (devserver.py, beside this file in the source tree only)."""
    try:
        import devserver
    except ImportError:
        raise SystemExit('WOW_BG3_SERVER is not set: no WoW server to talk to (WOW-BG3\'s launcher sets it; '
                         'by hand, name the WOW-BG3/server folder)')
    return devserver


def db_sh(cmd, stdin=None, container='wow-bg3-database-1'):
    p = portable()
    if p is None:
        return dev().db_sh(cmd, stdin, container)
    # var/ from WOWBG3_SERVER_VAR when set (the release: Mods/WOW-BG3/data/server).
    var = Path(os.environ.get('WOWBG3_SERVER_VAR') or p / 'var')
    sec = dict(l.split('=', 1) for l in (var / 'secrets.env').read_text().split() if '=' in l)
    env = dict(os.environ, PATH='%s:%s' % (p / 'mariadb/bin', os.environ.get('PATH', '/usr/bin:/bin')),
               LD_LIBRARY_PATH=str(p / 'lib'), MYSQL_UNIX_PORT=str(db_socket(var)),
               MARIADB_ROOT_PASSWORD=sec['ROOT_PASS'])
    return subprocess.run(['sh', '-c', cmd], input=stdin, capture_output=True, text=True, env=env)


def db_socket(var):
    """The portable server's MariaDB socket: the path server/wowbg3-server.sh wrote to var/run/socket (a short folder
    when var/ is too deep for a socket's 107 bytes, as in the release's Mods/WOW-BG3/data/server), else var/run/."""
    f = Path(var) / 'run/socket'
    try:
        return Path(f.read_text().strip() or Path(var) / 'run/mariadb.sock')
    except OSError:
        return Path(var) / 'run/mariadb.sock'


def mangosd_running(container='wow-bg3-mangosd-1'):
    """Whether the world server runs (its pid file names a live mangosd)."""
    p = portable()
    if p is None:
        return dev().mangosd_running(container)
    var = Path(os.environ.get('WOWBG3_SERVER_VAR') or p / 'var')
    try:
        pid = int((var / 'run/mangosd.pid').read_text().strip())
        return Path('/proc/%d/comm' % pid).read_text().startswith('mangosd')  # its main thread: mangosd-main
    except (OSError, ValueError):
        return False


def console(line, seconds=3, container='wow-bg3-mangosd-1'):
    p = portable()
    if p is not None:
        r = subprocess.run(['bash', str(p / 'wowbg3-server.sh'), 'console', line, str(seconds)],
                           capture_output=True, text=True)
        return r.stdout + r.stderr
    return dev().console(line, seconds, container)
