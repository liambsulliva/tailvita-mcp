import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class MemoryBudgetTests(unittest.TestCase):
    """The SceShell budget follows vitacompanion-slim: a 1 MiB heap."""

    def cache_default(self, cmake, name):
        match = re.search(r'set\(%s (\d+) CACHE STRING' % name, cmake)
        self.assertIsNotNone(match, name)
        return int(match.group(1))

    def test_defaults_match_slim_budget_and_fit_transfer_slots(self):
        cmake = (ROOT / "CMakeLists.txt").read_text()
        pool = self.cache_default(cmake, "VITACOMPANION_POOL_SIZE")
        buf = self.cache_default(cmake, "VITACOMPANION_FTP_BUFFER_SIZE")
        slots = int(re.search(
            r"#define FTPVITA_TRANSFER_POOL_SLOTS (\d+)U",
            (ROOT / "vendor" / "libftpvita" / "transfer_pool.h").read_text(),
        ).group(1))
        self.assertEqual(pool, 1024 * 1024)
        self.assertEqual(buf, 64 * 1024)
        self.assertLess(slots * buf, pool)
        self.assertIn("math(EXPR VITACOMPANION_FTP_POOL_BYTES", cmake)

    def test_sources_use_the_knobs(self):
        self.assertIn("heap_init(POOL_SIZE)", (ROOT / "src" / "main.c").read_text())
        self.assertIn("ftpvita_set_file_buf_size(FTP_BUFFER_SIZE)", (ROOT / "src" / "net.c").read_text())
        main_h = (ROOT / "src" / "main.h").read_text()
        self.assertIn("#define POOL_SIZE (1 * 1024 * 1024)", main_h)
        self.assertIn("#define FTP_BUFFER_SIZE (64 * 1024)", main_h)


if __name__ == "__main__":
    unittest.main()
