"""Fase A0: o android_perf.py lê /proc/<pid>/task/*/stat, o uso da GPU e o RE5_FPS_LOG."""
import os
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import android_perf  # noqa: E402


def stat_line(tid, name, utime, stime, cpu):
    # campos 3..52 de /proc/<pid>/task/<tid>/stat; utime = 14, stime = 15, processor = 39
    f = ["S"] + ["0"] * 49
    f[14 - 3], f[15 - 3], f[39 - 3] = str(utime), str(stime), str(cpu)
    return f"{tid} ({name}) " + " ".join(f)


class AndroidPerf(unittest.TestCase):
    def test_parse_stats(self):
        text = "\n".join([stat_line(100, "re5-main", 500, 20, 7), stat_line(101, "dxvk-cs", 90, 10, 4),
                          stat_line(102, "a (b) c", 1, 2, 0), "lixo"])
        rows = android_perf.parse_stats(text)
        self.assertEqual(rows[100], ("re5-main", 520, 7))
        self.assertEqual(rows[101], ("dxvk-cs", 100, 4))
        self.assertEqual(rows[102], ("a (b) c", 3, 0))   # nome com parênteses: o último ")" vale
        self.assertEqual(len(rows), 3)

    def test_groups(self):
        g = android_perf.group_of
        self.assertEqual(g("re5-main"), "jogo")
        self.assertEqual(g("g00E5A1B0"), "jogo")
        self.assertEqual(g("dxvk-submit"), "dxvk")
        self.assertEqual(g("mali-cmar-backe"), "driver")
        self.assertEqual(g("kgsl_worker_thr"), "driver")
        self.assertEqual(g("SDLAudioP2"), "outros")
        self.assertEqual(g("gralloc"), "outros")

    def test_gpu(self):
        self.assertEqual(android_perf.read_gpu("45 %\n"), 45.0)
        self.assertEqual(android_perf.read_gpu("37"), 37.0)
        self.assertAlmostEqual(android_perf.read_gpu("250 1000"), 25.0)   # kgsl gpubusy

    def test_fps_line(self):
        new = ("I/RE5Native( 123): [INFO ] d3d9      fps 28.0 (Present 900) thunks 1 d3d9 2 draws 3 heap 4 "
               "dispatch 5 lock_ms 120.5 d3d9_ms 140.0 present_ms 30.2")
        m = android_perf.FPS_RE.search(new)
        self.assertEqual(m.groups(), ("28.0", "120.5", "140.0", "30.2"))
        old = "fps 30.0 (Present 10) thunks 1 d3d9 2 draws 3 heap 4 dispatch 5 lock_ms 1.0"
        self.assertEqual(android_perf.FPS_RE.search(old).groups(), ("30.0", "1.0", None, None))


if __name__ == "__main__":
    unittest.main()
