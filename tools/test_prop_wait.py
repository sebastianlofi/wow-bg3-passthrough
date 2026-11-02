#!/usr/bin/env python3
"""test_prop_wait.py: the layer's age rule for a spell's meshes (wowbg3_fxcap.inc.c ib_young), compiled as it is
with a small stand-in for the layer around it (needs a C compiler; skipped without one).

  python3 tools/test_prop_wait.py
"""
import shutil, subprocess, tempfile, unittest
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / 'plugins/capture/wowbg3_fxcap.inc.c'
CC = shutil.which('clang') or shutil.which('gcc') or shutil.which('cc')
if not CC:
    print('test_prop_wait.py: no C compiler; skipped')
    raise SystemExit(0)

HEAD = """
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static struct { uint64_t presents; } g_req;
static struct { uint32_t fxpropage; } g_cc = {900};
static uint64_t g_fx_seen_at, g_hide_at;
static uint32_t hslot(uint64_t k, uint32_t cap) { return (uint32_t)(k * 11400714819323198485ull >> 40) & (cap - 1); }
#define FNV0 14695981039346656037ull
static uint64_t fnv64(const void* p, size_t n, uint64_t h) { const uint8_t* b = p; while (n--) h = (h ^ *b++) * 1099511628211ull; return h; }
"""
MAIN = """
int main(void) {
    g_req.presents = 1000; g_fx_seen_at = 1000; g_ibage_warm = 0;  // a spell is on
    uint64_t body[20], mesh = ib_slice(77, 0, 0, 36, 0);
    for (int i = 0; i < 20; i++) body[i] = ib_slice(100 + i, 0, 0, 36, 0);
    int kept = 0;
    // A present with 20 new skinned slices, and a list read before it.
    g_hide_at = 999;
    for (int i = 0; i < 20; i++) kept += ib_young(body[i], 1);
    printf("before %d ask %u\\n", kept, g_hide_ask);
    // The next presents, the list not read yet; an unskinned mesh made in the same present goes through at once.
    g_req.presents = 1001;
    kept = 0;
    for (int i = 0; i < 20; i++) kept += ib_young(body[i], 1);
    printf("waiting %d unskinned %d\\n", kept, ib_young(mesh, 0));
    // A list made after them is read: the slices are through this test, and hide_has answers for each.
    g_hide_at = 1002; g_req.presents = 1003;
    kept = 0;
    for (int i = 0; i < 20; i++) kept += ib_young(body[i], 1);
    printf("after %d\\n", kept);
    // Older than fxpropage.
    g_req.presents = 1000 + 900 + 1; g_fx_seen_at = g_req.presents;
    for (uint64_t now = 1003; now <= 1901; now += 50) { g_req.presents = now; ib_young(body[0], 1); }
    g_req.presents = 1901;
    printf("old %d\\n", ib_young(body[0], 1));
    return 0;
}
"""


class Wait(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        s = SRC.read_text()
        a = s.index('#define IBAGE_GAP')
        b = s.index('// Once per present: a new skinned slice asks')
        with tempfile.TemporaryDirectory() as d:
            c, exe = Path(d) / 't.c', Path(d) / 't'
            c.write_text(HEAD + s[a:b] + MAIN)
            subprocess.run([CC, '-w', '-std=gnu11', '-o', str(exe), str(c)], check=True)
            cls.out = dict(line.split(' ', 1) for line in subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout.splitlines())

    def test_many_new_skinned_slices_wait_and_ask(self):
        self.assertEqual(self.out['before'], '0 ask 1')

    def test_an_unskinned_mesh_does_not_wait(self):
        self.assertEqual(self.out['waiting'], '0 unskinned 1')

    def test_a_list_made_after_them_lets_all_of_them_on_to_the_hide_test(self):
        self.assertEqual(self.out['after'], '20')

    def test_old_is_old(self):
        self.assertEqual(self.out['old'], '0')


if __name__ == '__main__':
    unittest.main()
