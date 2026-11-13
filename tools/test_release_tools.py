#!/usr/bin/env python3
"""test_release_tools.py: every tool the release ships finds the modules it imports in the release.

A shipped tool that imports a tools/ module the release leaves out starts in the harness (which copies files by
hand) and dies at a player's. The list is release/build-release.py TOOLS and TOOLS_LATER.

  python3 tools/test_release_tools.py
"""
import ast
import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('build_release', ROOT / 'release/build-release.py')
BR = importlib.util.module_from_spec(spec)
spec.loader.exec_module(BR)
# Shipped beside the lists (build-release.py copies them by name).
ALSO = {'relenv.py', 'wowbg3-coord.py', 'world-console.py'}
# Never shipped, on purpose: the developers' test server. One shipped file may name it, wowserver.py, inside dev(),
# which says so plainly when the file is not there (a release); every other tool goes through wowserver.dev().
DEV_ONLY = {'devserver.py': 'wowserver.py'}


def imports(path):
    """Names of the modules a Python file imports, anywhere in it."""
    out = set()
    for node in ast.walk(ast.parse(path.read_text())):
        if isinstance(node, ast.Import):
            out.update(a.name.split('.')[0] for a in node.names)
        elif isinstance(node, ast.ImportFrom) and node.module and not node.level:
            out.add(node.module.split('.')[0])
    return out


class ReleaseTools(unittest.TestCase):
    def test_the_bridge_ships_with_the_places(self):
        self.assertIn('wow_places.py', BR.TOOLS)

    def test_every_shipped_tool_finds_its_own_modules(self):
        shipped = set(BR.TOOLS) | set(BR.TOOLS_LATER) | ALSO
        missing = []
        for name in sorted(shipped):
            src = ROOT / 'tools' / name
            if not name.endswith('.py') or not src.is_file():
                continue
            for mod in sorted(imports(src)):
                if DEV_ONLY.get(mod + '.py') == name:
                    continue
                if (ROOT / 'tools' / (mod + '.py')).is_file() and mod + '.py' not in shipped:
                    missing.append('%s imports %s.py' % (name, mod))
        self.assertEqual(missing, [])

    def test_the_developers_server_stays_out_and_fails_plainly(self):
        shipped = set(BR.TOOLS) | set(BR.TOOLS_LATER) | ALSO
        for dev, user in DEV_ONLY.items():
            self.assertNotIn(dev, shipped)
            self.assertTrue((ROOT / 'tools' / dev).is_file())
            tree = ast.parse((ROOT / 'tools' / user).read_text())
            # Imported in one function only, never at the top of the file, and an ImportError there is answered.
            self.assertFalse(any(isinstance(n, ast.Import) and any(a.name + '.py' == dev for a in n.names) for n in tree.body))
            fn = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'dev')
            self.assertTrue(any(isinstance(n, ast.Try) for n in ast.walk(fn)))
            self.assertIn('ImportError', ast.unparse(fn))


if __name__ == '__main__':
    unittest.main()
