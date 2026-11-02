#!/usr/bin/env python3
"""server/wowbg3-server.sh knows the processes it started (Linux only: it reads /proc). Stand-ins for mariadbd,
realmd and mangosd (python scripts) behave as the real ones do where the script looks: mangosd and realmd name their
main thread mangosd-main and realmd-main (the old name check took the live server for a left-over pid file, so
`stop` skipped its saveall and the sweep killed it), mariadbd writes its own pid file and socket."""
import os, shutil, signal, subprocess, sys, tempfile, time, unittest
from pathlib import Path

SCRIPT = Path(os.environ.get('WOWBG3_SERVER_SCRIPT') or Path(__file__).resolve().parent.parent / 'server/wowbg3-server.sh')
if not sys.platform.startswith('linux'):
    print('test_server_pids.py: needs Linux (/proc); skipped')
    sys.exit(0)

NAME = '''import ctypes
ctypes.CDLL(None).prctl(15, %r.encode(), 0, 0, 0)  # PR_SET_NAME: what /proc/PID/comm shows
'''
MANGOSD = '''#!/usr/bin/env python3
import os, signal, sys
''' + NAME % 'mangosd-main' + '''
log = open(os.environ['FAKE_LOG'], 'a', buffering=1)
signal.signal(signal.SIGINT, lambda *_: (log.write('SIGINT\\n'), sys.exit(0)))
print('World initialized.', flush=True)
for line in sys.stdin:
    log.write('console: %s' % line)
signal.pause()
'''
REALMD = '''#!/usr/bin/env python3
import signal
''' + NAME % 'realmd-main' + '''
signal.pause()
'''
MARIADBD = '''#!/usr/bin/env python3
import os, signal, socket, sys
arg = dict(a[2:].split('=', 1) for a in sys.argv[1:] if '=' in a)
open(arg['pid-file'], 'w').write('%d\\n' % os.getpid())
s = socket.socket(socket.AF_UNIX)
s.bind(arg['socket'])
def end(*_):
    os.unlink(arg['pid-file'])
    sys.exit(0)
signal.signal(signal.SIGTERM, end)
signal.pause()
'''


class ServerPids(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        t = Path(self.tmp)
        self.root = t / "Baldur's Gate 3/server"   # the release's folder has spaces and a quote
        (self.root / 'bin').mkdir(parents=True)
        (self.root / 'mariadb/bin').mkdir(parents=True)
        shutil.copy2(str(SCRIPT), str(self.root / 'wowbg3-server.sh'))
        for path, text in ((self.root / 'bin/mangosd', MANGOSD), (self.root / 'bin/realmd', REALMD),
                           (self.root / 'mariadb/bin/mariadbd', MARIADBD)):
            path.write_text(text)
            path.chmod(0o755)
        self.var = t / 'var'
        for d in ('run', 'logs', 'etc', 'db'):
            (self.var / d).mkdir(parents=True)
        (self.var / '.initialized').touch()
        (self.var / 'secrets.env').write_text('ROOT_PASS=x\nMANGOS_PASS=y\n')
        (self.var / 'ports.env').write_text('WORLD_PORT=18105\nAUTH_PORT=13744\n')
        (self.var / 'etc/mangosd.conf').write_text('AutoHonorRestart = 1\nReusableGuidPoolSize = 100\n')
        (self.var / 'etc/realmd.conf').write_text('\n')
        self.log = t / 'mangosd-fake.log'
        self.env = dict(os.environ, WOWBG3_SERVER_VAR=str(self.var), FAKE_LOG=str(self.log))
        for k in ('WOWBG3_HOME', 'WOWBG3_DATA', 'WOWBG3_IO'):
            self.env.pop(k, None)
        self.others = []

    def tearDown(self):
        self.server('stop')
        for p in self.others:
            p.kill()
            p.wait()
        for name in ('mangosd', 'realmd', 'mariadb', 'console-holder'):  # nothing may outlive a failed test
            try:
                os.kill(int((self.var / 'run' / ('%s.pid' % name)).read_text()), signal.SIGKILL)
            except (OSError, ValueError):
                pass
        shutil.move(self.tmp, self.tmp + '.done')  # left for the system's temp cleanup

    def server(self, *args):
        # Into a file: the server's children keep a pipe's other end open (the console holder's stderr).
        with tempfile.TemporaryFile('w+') as f:
            r = subprocess.run(['bash', str(self.root / 'wowbg3-server.sh')] + list(args), env=self.env,
                               stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT, timeout=120)
            f.seek(0)
            return r.returncode, f.read()

    def pid(self, name):
        return int((self.var / 'run' / ('%s.pid' % name)).read_text())

    def running(self, pid):
        try:
            return Path('/proc/%d/cmdline' % pid).read_bytes() != b''
        except OSError:
            return False

    def test_stop_saves_first_and_stops_what_it_started(self):
        rc, out = self.server('start')
        self.assertEqual(rc, 0, out)
        self.assertEqual(Path('/proc/%d/comm' % self.pid('mangosd')).read_text().strip(), 'mangosd-main')
        rc, out = self.server('status')
        self.assertEqual(out.split('\n')[:3], ['mariadb running (pid %d)' % self.pid('mariadb'),
                                               'realmd running (pid %d)' % self.pid('realmd'),
                                               'mangosd running (pid %d)' % self.pid('mangosd')], out)
        pids = [self.pid(n) for n in ('mangosd', 'realmd', 'mariadb', 'console-holder')]
        rc, out = self.server('stop')
        self.assertEqual(rc, 0, out)
        self.assertNotIn('left over', out)
        self.assertNotIn('killing', out)
        self.assertEqual(self.log.read_text(), 'console: saveall\nSIGINT\n')
        time.sleep(0.5)
        self.assertEqual([p for p in pids if self.running(p)], [])

    def test_a_pid_file_naming_another_process_is_left_over_and_that_process_left_alone(self):
        other = subprocess.Popen(['sleep', '60'])
        self.others.append(other)
        (self.var / 'run/mangosd.pid').write_text('%d\n' % other.pid)
        rc, out = self.server('status')
        self.assertIn('mangosd stopped', out)
        rc, out = self.server('start')
        self.assertEqual(rc, 0, out)
        self.assertIn("mangosd's pid file names pid %d, which is not our mangosd: left over" % other.pid, out)
        self.assertNotEqual(self.pid('mangosd'), other.pid)
        rc, out = self.server('stop')
        self.assertEqual(self.log.read_text(), 'console: saveall\nSIGINT\n')
        self.assertIsNone(other.poll())

    def test_another_copys_server_is_not_ours(self):
        rc, out = self.server('start')
        self.assertEqual(rc, 0, out)
        mine = self.env['WOWBG3_SERVER_VAR']
        self.env['WOWBG3_SERVER_VAR'] = str(Path(self.tmp) / 'var2')
        shutil.copytree(mine, self.env['WOWBG3_SERVER_VAR'], ignore=shutil.ignore_patterns('mariadb.sock', 'console'))
        rc, out = self.server('status')
        self.assertEqual(out.count('stopped'), 3, out)
        self.env['WOWBG3_SERVER_VAR'] = mine


if __name__ == '__main__':
    unittest.main()
