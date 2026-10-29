"""Regression tests for baremetal_size_gate.py on synthetic size/log output -
no cross toolchain or firmware build needed."""
import unittest

import baremetal_size_gate as gate

SIZE_A = """hello.elf  :
section             size        addr
.isr_vector           64   134217728
.text              44372   134217792
.ARM.exidx             8   134262164
.init_array           16   134262172
.data                216   603979776
.bss                1576   603979992
.heap             514392   603989672
.comment              31           0
.ARM.attributes       52           0
Total             560727
"""


class BaremetalSizeGateTests(unittest.TestCase):
    def test_sections_split_into_flash_and_ram(self):
        m = gate.measure_sections(gate.parse_size_a(SIZE_A))
        # flash: vectors, code, exidx, init array and the .data load image
        self.assertEqual(m["flash"], 64 + 44372 + 8 + 16 + 216)
        # static RAM: .data + .bss; the linker's .heap region is not counted
        self.assertEqual(m["static_ram"], 216 + 1576)

    def test_high_water_line(self):
        log = ("PASS x\nsteady allocs per request: 0\n"
               "heap high water: 22296 bytes, stack used: 8352 bytes\nbaremetal-smoke: PASS\n")
        self.assertEqual(gate.parse_log(log), {"heap": 22296, "stack": 8352, "allocs": 0})
        self.assertEqual(gate.parse_log("no numbers here"), {})

    def test_budget_parsing(self):
        b = gate.parse_budget("# c\nhello flash 100  # note\n\nhello stack 9\nhello allocs 0\n")
        self.assertEqual(b, {("hello", "flash"): 100, ("hello", "stack"): 9, ("hello", "allocs"): 0})
        for bad in ("hello flash", "hello flsh 1", "hello flash 1k"):
            with self.assertRaises(ValueError):
                gate.parse_budget(bad)

    def test_check(self):
        budget = {("hello", "flash"): 100, ("hello", "heap"): 50, ("hello", "allocs"): 0,
                  ("other", "flash"): 1}
        self.assertEqual(gate.check({"hello": {"flash": 100, "heap": 50, "allocs": 0}}, budget), [])
        over = gate.check({"hello": {"flash": 101, "heap": 50, "allocs": 1}}, budget)
        self.assertEqual(len(over), 2)
        self.assertIn("hello flash: 101 bytes > budget 100 (+1)", over)
        self.assertIn("hello allocs: 1 allocations/request > budget 0 (+1)", over)
        # a budgeted metric the run did not report fails instead of passing silently
        missing = gate.check({"hello": {"flash": 1}}, budget)
        self.assertEqual(len(missing), 2)
        self.assertTrue(any("hello heap: not measured" in failure for failure in missing))


if __name__ == "__main__":
    unittest.main()
