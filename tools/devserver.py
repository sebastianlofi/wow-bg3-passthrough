"""devserver.py: the developers' Docker test server, for the tools run from the source tree.

Not part of a release (release/build-release.py leaves it out, tools/test_release_tools.py and test_release_contents.py check it): a player's
install has one WoW server, the portable one its launcher starts, and the tools reach it through WOW_BG3_SERVER
(wowserver.py). With that unset, in the source tree, wowserver.dev() hands the same calls to this file: the test
stack's database container (`docker exec`), mangosd's console (Docker attach) and its config file, which root owns.

    db_sh(cmd, stdin, container) -> subprocess.CompletedProcess (text)   a shell command in the database container
    console(line, seconds, container) -> what the console printed
    mangosd_running(container) -> whether the world server's container runs
    sql(query, db) -> subprocess.CompletedProcess                       one query, tab separated, no header
    conf_lines(pattern) -> the config lines mangosd sees that match     (an ERE)
    conf_read() / conf_write(text)                                      the stack's mangosd.conf on the host
"""
import os, select, socket, subprocess, time

STACK = os.environ.get('WOWBG3_TEST_STACK') or '/opt/stacks/wow-bg3'
CONF = STACK + '/config/mangosd.conf'
CONF_INSIDE = '/opt/vmangos/config/mangosd.conf'
DBC = STACK + '/storage/mangosd/extracted-data/5875/dbc'
DB = 'wow-bg3-database-1'
WORLD = 'wow-bg3-mangosd-1'
# SQL from stdin on the mangos database (wow_items.py --db's default in the source tree).
DB_CMD = ['sudo', '-n', 'docker', 'exec', '-i', DB, 'sh', '-c', 'mariadb -uroot -p"$MARIADB_ROOT_PASSWORD" --batch mangos']


def db_sh(cmd, stdin=None, container=DB):
    return subprocess.run(['sudo', '-n', 'docker', 'exec', '-i', container, 'sh', '-c', cmd], input=stdin,
                          capture_output=True, text=True)


def sql(query, db=''):
    return db_sh('mariadb -uroot -p"$MARIADB_ROOT_PASSWORD" -N -B %s' % db, query)


def mangosd_running(container=WORLD):
    """The container runs, or docker could not say."""
    r = subprocess.run(['sudo', '-n', 'docker', 'inspect', '-f', '{{.State.Running}}', container],
                       capture_output=True, text=True)
    return r.returncode != 0 or r.stdout.strip() == 'true'


def _receive(sock, seconds):
    end, out = time.monotonic() + seconds, []
    while time.monotonic() < end:
        if select.select([sock], [], [], max(0, end - time.monotonic()))[0]:
            c = sock.recv(65536)
            if not c:
                break
            out.append(c)
    return b''.join(out).decode(errors='replace')


def console(line, seconds=3, container=WORLD):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.connect('/var/run/docker.sock')
        s.sendall(('POST /v1.47/containers/%s/attach?stream=1&stdin=1&stdout=1&stderr=1 HTTP/1.1\r\n'
                   'Host: localhost\r\nConnection: Upgrade\r\nUpgrade: tcp\r\nContent-Length: 0\r\n\r\n' % container).encode())
        head = _receive(s, 1)
        assert head.startswith('HTTP/1.1 101'), head[:200]
        s.sendall((line + '\n').encode())
        return _receive(s, seconds)


def conf_lines(pattern):
    r = subprocess.run(['sudo', '-n', 'docker', 'exec', WORLD, 'grep', '-E', pattern, CONF_INSIDE],
                       capture_output=True, text=True)
    return r.stdout


def conf_read():
    return subprocess.run(['sudo', '-n', 'cat', CONF], capture_output=True, text=True, check=True).stdout


def conf_write(text):
    """In place (the file is a single-file bind mount: the container keeps the same inode)."""
    subprocess.run(['sudo', '-n', 'tee', CONF], input=text, capture_output=True, text=True, check=True)
