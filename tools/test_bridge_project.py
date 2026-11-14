#!/usr/bin/env python3
"""test_bridge_project.py: the fight bridge's folder (SHM/bridge, per user id) gets link/tools pointing at this
install's tools, also when another install with the same user id left its own link there (a Wolf container's and
the desktop's share /dev/shm/wow-bg3-UID). Before, a link that pointed elsewhere was kept and every fight failed on a
missing wow_arena.py. bridge_project is lifted out of wowbg3-coord.py, so none of its imports are needed.

  python3 tools/test_bridge_project.py
"""
import ast, os, tempfile, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent


def lift(shm, tools):
    tree = ast.parse((HERE / 'wowbg3-coord.py').read_text())
    keep = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'bridge_project']
    ns = {'os': os, 'Path': Path, 'SHM': shm, 'TOOLS': tools}
    exec(compile(ast.Module(body=keep, type_ignores=[]), 'wowbg3-coord.py', 'exec'), ns)
    return ns['bridge_project']


class BridgeProject(unittest.TestCase):
    def setUp(self):
        self.d = Path(tempfile.mkdtemp())
        self.tools = self.d / 'mine/tools/link/tools'
        self.tools.mkdir(parents=True)
        (self.tools / 'wow_arena.py').write_text('')
        self.shm = self.d / 'shm'
        self.bp = lift(self.shm, self.tools)

    def arena(self, proj):
        return (proj / 'link/tools/wow_arena.py').exists()

    def test_fresh_folder(self):
        self.assertTrue(self.arena(self.bp()))

    def test_another_installs_link_is_replaced(self):
        (self.shm / 'bridge/link').mkdir(parents=True)
        (self.shm / 'bridge/link/tools').symlink_to('/home/retro/.local/share/elsewhere/tools/link/tools')
        proj = self.bp()
        self.assertEqual(os.readlink(proj / 'link/tools'), str(self.tools))
        self.assertTrue(self.arena(proj))

    def test_own_link_is_kept(self):
        self.bp()
        before = os.lstat(self.shm / 'bridge/link/tools').st_ino
        self.bp()
        self.assertEqual(os.lstat(self.shm / 'bridge/link/tools').st_ino, before)


if __name__ == '__main__':
    unittest.main()
