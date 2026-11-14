#!/usr/bin/env python3
"""test_sql_charset.py: wowbg3cmd.sql reads the portable server's answers as UTF-8 whatever the locale. Started from BG3
there is none (Steam's runtime passes no LANG), MariaDB's client then answers in latin1, and a place name with an accent
(game_tele's "trépas") stopped every query of the places table: the Dream Visitor's crossings failed with a
UnicodeDecodeError. The stand-in client here answers in latin1 unless asked for UTF-8, as the real one does with no
locale; the old code fails on it.

  python3 tools/test_sql_charset.py
"""
import os, sys, tempfile, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

CLIENT = r'''#!/usr/bin/env python3
import sys
sys.stdin.read()
name = "trépas\n"
sys.stdout.buffer.write(name.encode("utf-8" if "--default-character-set=utf8mb4" in sys.argv else "latin-1"))
'''


class SqlCharset(unittest.TestCase):
    def setUp(self):
        d = Path(tempfile.mkdtemp())
        (d / 'mariadb/bin').mkdir(parents=True)
        c = d / 'mariadb/bin/mariadb'
        c.write_text(CLIENT)
        c.chmod(0o755)
        (d / 'lib').mkdir()
        var = d / 'var'
        (var / 'run').mkdir(parents=True)
        (var / 'secrets.env').write_text('ROOT_PASS=x\n')
        self.env = {'WOW_BG3_SERVER': str(d), 'WOWBG3_SERVER_VAR': str(var)}
        self.saved = {k: os.environ.get(k) for k in ('WOW_BG3_SERVER', 'WOWBG3_SERVER_VAR', 'LANG', 'LC_ALL', 'LC_CTYPE')}
        for k in ('LANG', 'LC_ALL', 'LC_CTYPE'):
            os.environ.pop(k, None)  # no locale, as under Steam's runtime
        os.environ.update(self.env)

    def tearDown(self):
        for k, v in self.saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v

    def test_accented_name_reads_back(self):
        import wowbg3cmd
        self.assertEqual(wowbg3cmd.sql('SELECT name FROM game_tele;', 'mangos'), 'trépas\n')


if __name__ == '__main__':
    unittest.main()
