"""wowserver.py: which WoW server a harness tool talks to (the harness notes).

WOW_BG3_SERVER unset (harness A, the default): the test server in Docker, exactly as the tools did it
before: the database container's shell (`docker exec`), mangosd's console (Docker attach).
WOW_BG3_SERVER=FOLDER (a test harness sets it): a portable server
(server/wowbg3-server.sh): the same shell command runs on the host with `mariadb` and
`mariadb-dump` on PATH, MYSQL_UNIX_PORT at its socket and MARIADB_ROOT_PASSWORD set, and console lines go
through `wowbg3-server.sh console`.

    db_sh(cmd, stdin=None, container='wow-bg3-database-1') -> subprocess.CompletedProcess (text)
    console(line, seconds=3, container='wow-bg3-mangosd-1') -> what the console printed
    mangosd_running() -> whether the world server runs (wowsave.py rehome needs it stopped)
"""
import os, select, socket, subprocess, time
from pathlib import Path


def portable():
    p = os.environ.get('WOW_BG3_SERVER')
    return Path(p) if p else None


def db_sh(cmd, stdin=None, container='wow-bg3-database-1'):
    p = portable()
    if p is None:
        return subprocess.run(['sudo', '-n', 'docker', 'exec', '-i', container, 'sh', '-c', cmd], input=stdin,
                              capture_output=True, text=True)
    # var/ from WOWBG3_SERVER_VAR when set (the release: ~/.local/share/wow-bg3/server).
    var = Path(os.environ.get('WOWBG3_SERVER_VAR') or p / 'var')
    sec = dict(l.split('=', 1) for l in (var / 'secrets.env').read_text().split() if '=' in l)
    env = dict(os.environ, PATH='%s:%s' % (p / 'mariadb/bin', os.environ.get('PATH', '/usr/bin:/bin')),
               LD_LIBRARY_PATH=str(p / 'lib'), MYSQL_UNIX_PORT=str(var / 'run/mariadb.sock'),
               MARIADB_ROOT_PASSWORD=sec['ROOT_PASS'])
    return subprocess.run(['sh', '-c', cmd], input=stdin, capture_output=True, text=True, env=env)


def mangosd_running(container='wow-bg3-mangosd-1'):
    """Whether the world server runs (a portable one: its pid file names a live mangosd; Docker: the container
    runs, or docker could not say)."""
    p = portable()
    if p is None:
        r = subprocess.run(['sudo', '-n', 'docker', 'inspect', '-f', '{{.State.Running}}', container],
                           capture_output=True, text=True)
        return r.returncode != 0 or r.stdout.strip() == 'true'
    var = Path(os.environ.get('WOWBG3_SERVER_VAR') or p / 'var')
    try:
        pid = int((var / 'run/mangosd.pid').read_text().strip())
        return Path('/proc/%d/comm' % pid).read_text().startswith('mangosd')  # its main thread: mangosd-main
    except (OSError, ValueError):
        return False


def _receive(sock, seconds):
    end, out = time.monotonic() + seconds, []
    while time.monotonic() < end:
        if select.select([sock], [], [], max(0, end - time.monotonic()))[0]:
            c = sock.recv(65536)
            if not c:
                break
            out.append(c)
    return b''.join(out).decode(errors='replace')


def console(line, seconds=3, container='wow-bg3-mangosd-1'):
    p = portable()
    if p is not None:
        r = subprocess.run(['bash', str(p / 'wowbg3-server.sh'), 'console', line, str(seconds)],
                           capture_output=True, text=True)
        return r.stdout + r.stderr
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect('/var/run/docker.sock')
        s.sendall(('POST /v1.47/containers/%s/attach?stream=1&stdin=1&stdout=1&stderr=1 HTTP/1.1\r\n'
                   'Host: localhost\r\nConnection: Upgrade\r\nUpgrade: tcp\r\nContent-Length: 0\r\n\r\n' % container).encode())
        head = _receive(s, 1)
        assert head.startswith('HTTP/1.1 101'), head[:200]
        s.sendall((line + '\n').encode())
        return _receive(s, seconds)
