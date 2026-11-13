#!/usr/bin/env python3
"""test_release_contents.py: what a player's install carries and what it leaves out (release/build-release.py).

The tools the release ships hold no developer-only code (the test stack's `sudo docker` calls and Docker socket are
devserver.py's, which no release carries) and no developer folder names; the release's python and server packs leave
out what a player's install never uses; no link reaches the zip; bg3le's Lua debugger stays off unless asked for;
the crash report names neither the user nor the PC.

  python3 tools/test_release_contents.py
"""
import importlib.util, json, os, re, tempfile, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
spec = importlib.util.spec_from_file_location('build_release', str(ROOT / 'release/build-release.py'))
br = importlib.util.module_from_spec(spec)
spec.loader.exec_module(br)
SHIPPED = ['wowbg3-coord.py', 'relenv.py', 'world-console.py'] + br.TOOLS + br.TOOLS_LATER


class ShippedTools(unittest.TestCase):
    def test_no_developer_server_code(self):
        self.assertNotIn('devserver.py', SHIPPED)
        self.assertTrue((HERE / 'devserver.py').is_file())
        for name in SHIPPED:
            if not name.endswith('.py'):
                continue
            text = (HERE / name).read_text()
            for word in ('sudo', 'docker.sock', "'docker'", '"docker"', 'docker exec', 'docker inspect'):
                self.assertNotIn(word, text, '%s holds %r: the test stack is devserver.py\'s' % (name, word))

    def test_no_developer_client_folder(self):
        for name in SHIPPED:
            self.assertNotIn('SoloCraft', (HERE / name).read_text(), name)

    def test_without_a_server_a_release_says_so(self):
        """wowserver.dev() in a folder without devserver.py (a release's tools folder) ends with a plain message."""
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / 'wowserver.py').write_text((HERE / 'wowserver.py').read_text())
            s = importlib.util.spec_from_file_location('wowserver_release', str(Path(d) / 'wowserver.py'))
            m = importlib.util.module_from_spec(s)
            s.loader.exec_module(m)
            import sys
            saved, env = list(sys.path), os.environ.pop('WOW_BG3_SERVER', None)
            sys.modules.pop('devserver', None)
            sys.path[:] = [p for p in sys.path if Path(p or '.').resolve() != HERE]
            try:
                with self.assertRaises(SystemExit) as e:
                    m.db_sh('true')
                self.assertIn('WOW_BG3_SERVER is not set', str(e.exception))
            finally:
                sys.path[:] = saved
                if env is not None:
                    os.environ['WOW_BG3_SERVER'] = env


class Packs(unittest.TestCase):
    def test_the_server_pack_leaves_out_what_no_install_uses(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = Path(d) / 'pack', Path(d) / 'out'
            keep = ['wowbg3-server.sh', 'bin/mangosd', 'lib/libmariadb.so.3', 'lib/libssl.so.1.1', 'mariadb/bin/mariadbd',
                    'mariadb/scripts/mariadb-install-db', 'mariadb/share/english/errmsg.sys', 'mariadb/share/charsets/Index.xml',
                    'mariadb/share/fill_help_tables.sql', 'mariadb/share/mariadb_system_tables.sql',
                    'mariadb/share/mariadb_system_tables_data.sql', 'mariadb/share/mariadb_performance_tables.sql',
                    'mariadb/share/maria_add_gis_sp_bootstrap.sql', 'mariadb/share/mariadb_test_db.sql',
                    'mariadb/share/mariadb_sys_schema.sql']
            drop = ['var/db/x', 'var.incomplete-1/x', 'sql/world.sql', 'mariadb/lib/libmariadb.so', 'mariadb/lib/libmariadb.so.3',
                    'mariadb/share/Mongo2.jar', 'mariadb/share/JdbcInterface.jar', 'mariadb/share/pam_user_map.so',
                    'mariadb/share/user_map.conf', 'mariadb/share/mroonga/install.sql', 'mariadb/share/groonga/COPYING',
                    'mariadb/share/groonga-normalizer-mysql/README.md', 'mariadb/share/aclocal/mysql.m4',
                    'mariadb/share/mariadb_test_data_timezone.sql']
            for rel in keep + drop:
                (src / rel).parent.mkdir(parents=True, exist_ok=True)
                (src / rel).write_text('x')
            br.shutil.copytree(src, dst, ignore=br.server_ignore(src))
            got = sorted(p.relative_to(dst).as_posix() for p in dst.rglob('*') if p.is_file())
            self.assertEqual(got, sorted(keep))

    def test_what_install_db_reads_stays(self):
        """mariadb-install-db stops when one of these is missing: none may match a pattern left out."""
        import fnmatch
        for name in ('fill_help_tables.sql', 'mariadb_system_tables.sql', 'mariadb_performance_tables.sql',
                     'mariadb_system_tables_data.sql', 'maria_add_gis_sp_bootstrap.sql', 'mariadb_test_db.sql',
                     'mariadb_sys_schema.sql', 'errmsg.sys', 'my_print_defaults', 'resolveip', 'mariadb-dump'):
            self.assertFalse(any(fnmatch.fnmatch(name, p) for p in br.SERVER_OUT), name)

    def test_one_python_and_no_links(self):
        with tempfile.TemporaryDirectory() as d:
            pack, py = Path(d) / 'pack', Path(d) / 'python'
            for rel in ('bin/python3.12', 'bin/2to3-3.12', 'bin/python3.12-config', 'lib/libpython3.12.so.1.0',
                        'lib/python3.12/os.py', 'lib/python3.12/turtle.py', 'lib/python3.12/unittest/case.py',
                        'lib/python3.12/config-3.12-x86_64-linux-gnu/Makefile', 'lib/python3.12/lib-dynload/_ssl.cpython-312.so',
                        'lib/python3.12/lib-dynload/_dbm.cpython-312-x86_64-linux-gnu.so',
                        'lib/python3.12/site-packages/numpy/__init__.py', 'lib/python3.12/site-packages/numpy/_core/tests/test_a.py',
                        'lib/python3.12/site-packages/numpy/f2py/__init__.py', 'lib/python3.12/__pycache__/os.pyc'):
                (pack / rel).parent.mkdir(parents=True, exist_ok=True)
                (pack / rel).write_text(rel)
            for link, target in (('bin/python3', 'python3.12'), ('bin/python', 'python3.12'), ('bin/2to3', '2to3-3.12'),
                                 ('bin/python3-config', 'python3.12-config'), ('lib/libpython3.12.so', 'libpython3.12.so.1.0')):
                (pack / link).symlink_to(target)
            br.shutil.copytree(pack, py, symlinks=True, ignore=br.shutil.ignore_patterns(*br.PYTHON_OUT))
            br.one_python(py)
            self.assertEqual([p for p in py.rglob('*') if p.is_symlink()], [])
            got = sorted(p.relative_to(py).as_posix() for p in py.rglob('*') if p.is_file())
            self.assertEqual(got, ['bin/python3', 'lib/python3.12/lib-dynload/_ssl.cpython-312.so', 'lib/python3.12/os.py',
                                   'lib/python3.12/site-packages/numpy/__init__.py', 'lib/python3.12/unittest/case.py'])
            self.assertEqual((py / 'bin/python3').read_text(), 'bin/python3.12')   # the interpreter itself, under the name called

    def test_the_names_the_release_calls(self):
        self.assertIn('$WOWBG3_HOME/python/bin/python3', (ROOT / 'release/launcher.conf.in').read_text())
        self.assertIn('python/bin/python3"', (ROOT / 'server/wowbg3-server.sh').read_text())
        rel = (ROOT / 'release/build-release.py').read_text()
        self.assertIn('links in the release', rel)   # a link anywhere in the release stops the build

    def test_python_packages_are_pinned(self):
        line = next(l for l in (ROOT / 'release/python/fetch.sh').read_text().splitlines() if 'pip install' in l)
        for word in line.split('pip install', 1)[1].split():
            if not word.startswith('-'):
                self.assertRegex(word, r'^[a-z0-9]+==[0-9.]+$', 'unpinned: ' + word)


class Bg3le(unittest.TestCase):
    def test_the_lua_debugger_is_off_unless_asked(self):
        lock = json.loads((ROOT / 'patches/bg3le/build.lock.json').read_text())
        files = [Path(p['file']).name for p in lock['runtime_patches']]
        self.assertIn('0016-lua-debugger-off-unless-asked.patch', files)
        patch = (ROOT / 'patches/bg3le/0016-lua-debugger-off-unless-asked.patch').read_text()
        self.assertIn("-    if (opt != nullptr && opt[0] == '0') return;", patch)   # upstream: on unless BG3LE_DEBUG=0
        self.assertIn("+    if (opt != nullptr && opt[0] != '\\0') return opt[0] == '1';", patch)
        self.assertIn('EnableLuaDebugger', patch)
        for doc in ('README.md', 'release/README-player.md'):
            self.assertIn('9998', (ROOT / doc).read_text(), doc + ' says what can listen')


class CrashReport(unittest.TestCase):
    def setUp(self):
        s = importlib.util.spec_from_file_location('crashreport', str(HERE / 'crashreport.py'))
        self.c = importlib.util.module_from_spec(s)
        s.loader.exec_module(self.c)

    def test_user_and_pc_names_are_masked(self):
        out = self.c.clean('/home/max/x maximum max@box box.lan\nLinux box 6.1 #1 SMP\n', home='/home/max', user='max', host='box.lan')
        self.assertEqual(out, '~/x maximum USER@HOST HOST\nLinux HOST 6.1 #1 SMP\n')

    def test_a_password_line_is_cut(self):
        self.assertEqual(self.c.clean('a\nWOW_PASS=abc def\n', home='/h', user='u1', host='h1'), 'a\nWOW_[blanked]\n')

    def test_uname_leaves_the_pc_name_out(self):
        text = (HERE / 'crashreport.py').read_text()
        self.assertNotIn("'uname', '-a'", text)
        self.assertNotRegex(text, r"'uname', '-[a-z]*n")


class Docs(unittest.TestCase):
    def test_removal_runs_the_uninstaller_before_the_folder_goes(self):
        """bg3le's installer is inside Mods/WOW-BG3/: every removal text names --uninstall, before the delete."""
        for doc, head in (('README.md', '- **Uninstall.**'), ('docs/install.md', '## Removing it'), ('release/README-player.md', '## Remove')):
            text = (ROOT / doc).read_text()
            part = text[text.index(head):][:1500]
            self.assertIn('--uninstall', part, doc)
            cut = re.search(r'(?i)\b(delete|remove)\b[^.\n]*Mods/WOW-BG3', part)
            self.assertTrue(cut and part.index('--uninstall') < cut.start(), doc + ': --uninstall comes first')


if __name__ == '__main__':
    unittest.main()
