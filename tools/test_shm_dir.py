#!/usr/bin/env python3
"""test_shm_dir.py: the folder WoW's frames go through is one of our own (plugins/wowbg3_shm.h).

/dev/shm is everyone's and the folder's name can be guessed, so the launcher and the render code use
/dev/shm/wow-bg3-<uid> only as a real directory of this user that nobody else can enter, and otherwise a folder of
our own in the runtime folder, else <data>/shm. Both go through the one function, so they can never name two places.
The header is compiled into a small program here (needs a C compiler; skipped without one).

  python3 tools/test_shm_dir.py
"""
import os, shutil, stat, subprocess, sys, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CC = shutil.which('cc') or shutil.which('clang') or shutil.which('gcc')
DRIVER = r'''
#include <stdlib.h>
#include "wowbg3_shm.h"
int main(int argc, char** argv) {
    char out[800];
    if (argc != 5) return 2;
    int r = wowbg3_shm_dir(out, sizeof out, argv[1], argv[2][0] ? argv[2] : NULL, argv[3], (uid_t)atoi(argv[4]));
    printf("%d %s\n", r, out);
    return 0;
}
'''


@unittest.skipUnless(CC, 'no C compiler')
class ShmDir(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = Path(tempfile.mkdtemp())
        (cls.build / 'driver.c').write_text(DRIVER)
        subprocess.run([CC, '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'plugins'),
                        str(cls.build / 'driver.c'), '-o', str(cls.build / 'driver')], check=True)

    def setUp(self):
        self.t = Path(tempfile.mkdtemp())
        self.shm, self.rt, self.data = self.t / 'shm', self.t / 'run', self.t / 'data'
        for d in (self.shm, self.rt, self.data):
            d.mkdir()
        self.me = os.getuid()
        self.ours = self.shm / ('wow-bg3-%d' % self.me)

    def ask(self, me=None, runtime=True):
        out = subprocess.run([str(self.build / 'driver'), str(self.shm), str(self.rt) if runtime else '', str(self.data),
                              str(self.me if me is None else me)], capture_output=True, text=True, check=True).stdout
        where, path = out.rstrip('\n').split(' ', 1)
        return int(where), Path(path)

    def test_a_fresh_folder_is_made_closed(self):
        self.assertEqual(self.ask(), (0, self.ours))
        self.assertTrue(self.ours.is_dir())
        self.assertEqual(stat.S_IMODE(self.ours.stat().st_mode), 0o700)

    def test_our_own_folder_with_a_loose_mode_is_closed(self):
        self.ours.mkdir()
        os.chmod(self.ours, 0o755)
        self.assertEqual(self.ask(), (0, self.ours))
        self.assertEqual(stat.S_IMODE(self.ours.stat().st_mode), 0o700)

    def test_a_link_of_that_name_is_not_followed(self):
        elsewhere = self.t / 'elsewhere'
        elsewhere.mkdir()
        self.ours.symlink_to(elsewhere)
        self.assertEqual(self.ask(), (1, self.rt / 'wow-bg3'))
        self.assertEqual(list(elsewhere.iterdir()), [])
        self.assertEqual(stat.S_IMODE((self.rt / 'wow-bg3').stat().st_mode), 0o700)

    def test_a_file_of_that_name_is_left_alone(self):
        self.ours.write_text('x')
        self.assertEqual(self.ask(), (1, self.rt / 'wow-bg3'))
        self.assertEqual(self.ours.read_text(), 'x')

    def test_somebody_elses_folder_is_left_alone(self):
        """Asked as another user: the folder of that user's name exists and belongs to us, not to them."""
        other = self.me + 1
        theirs = self.shm / ('wow-bg3-%d' % other)
        theirs.mkdir()
        os.chmod(theirs, 0o777)
        (theirs / 'wowframe.a.bin').write_text('theirs')
        where, path = self.ask(me=other)
        self.assertEqual((where, path), (2, self.data / 'shm'))        # the runtime folder is not that user's either
        self.assertEqual(stat.S_IMODE(theirs.stat().st_mode), 0o777)  # not even its mode is touched
        self.assertEqual((theirs / 'wowframe.a.bin').read_text(), 'theirs')

    @unittest.skipIf(os.getuid() == 0, 'root can write anywhere')
    def test_without_a_writable_shm_the_runtime_folder(self):
        os.chmod(self.shm, 0o555)
        try:
            self.assertEqual(self.ask(), (1, self.rt / 'wow-bg3'))
        finally:
            os.chmod(self.shm, 0o755)

    def test_without_a_runtime_folder_the_data_folder(self):
        self.ours.write_text('x')
        self.assertEqual(self.ask(runtime=False), (2, self.data / 'shm'))

    def test_both_plugins_use_the_one_rule(self):
        for src in ('plugins/launcher/wowbg3_launcher.c', 'plugins/capture/wowbg3_capture.c'):
            text = (ROOT / src).read_text()
            self.assertIn('#include "../wowbg3_shm.h"', text, src)
            self.assertIn('wowbg3_shm_dir(', text, src)
            self.assertNotIn('"/dev/shm/wow-bg3-%d"', text, src + ' names the folder itself')

    def test_the_servers_socket_fits_when_the_frames_folder_is_deep(self):
        """With the frames in <data>/shm (as deep as var/), MariaDB's socket goes to the next folder where a socket's
        107 bytes hold it."""
        deep = self.t / ('d' * 90) / 'var'
        runtime = Path(tempfile.mkdtemp(prefix='rt', dir='/tmp'))
        script = (ROOT / 'server/wowbg3-server.sh').read_text()
        head = script[:script.index('WORLD_PORT=')]   # the part that works the socket's path out
        r = subprocess.run(['bash', '-c', head + '\necho "$SOCK"', 'x'], capture_output=True, text=True,
                           env=dict(os.environ, WOWBG3_SERVER_VAR=str(deep), WOWBG3_SHM=str(deep.parent / 'shm'),
                                    XDG_RUNTIME_DIR=str(runtime)))
        self.assertEqual(r.returncode, 0, r.stderr)
        sock = r.stdout.strip()
        self.assertTrue(sock.startswith(str(runtime) + '/mariadb-') and len(sock) <= 100, sock)


if __name__ == '__main__':
    unittest.main()
