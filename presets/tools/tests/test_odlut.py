#!/usr/bin/env python3
"""Tests for presets/tools/odlut. Standard library only; every Spectra 6 fixture is built here from segments."""
from __future__ import annotations

import random
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))
from odlut import bwry535, bwry_lzss, s6_charge, s6_codec as C, s6_lint, s6_lutc  # noqa: E402
import lutlint  # noqa: E402

PWR_73 = "3F 00 32 2A 0E 2A"
HEADER7 = bytes([0xFF] + [C.enc(v) for v in (0x32, 0x2A, 0x0E, 0x2A, 20, 8)])
BALANCED = [("S", 10), ("", 2), ("K", 10), ("", 2)]


def table(slots: dict, lutc=None) -> bytes:
    """800 B image: slots = {code: [(level, frames), ...]}; lutc = list of s6_lutc rows."""
    t = s6_lutc.blank(HEADER7)
    for k, segs in slots.items():
        t.slots[k] = C.encode_segments(segs)
    if lutc:
        t.lutc.rows = s6_lutc.pad(lutc)
    return s6_lutc.finalize(t, forbid=())


def good() -> bytes:
    return table({k: BALANCED for k in range(8)})


def rules(r, level="FAIL") -> set:
    return {x.rule.split()[0] for x in r.findings if x.level == level}


class Codec(unittest.TestCase):
    def test_value_scramble_is_a_bijection(self):
        self.assertEqual(sorted(C.val(x) for x in range(256)), list(range(256)))
        for v in range(256):
            self.assertEqual(C.val(C.enc(v)), v)
        for v in range(128):
            self.assertEqual(C.val(C.enc(v, skip5=True), skip5=True), v)

    def test_random_images_round_trip_exactly(self):
        rng = random.Random(7)
        for _ in range(200):
            e = bytes(rng.randrange(256) for _ in range(800))
            self.assertEqual(C.encode_table(C.decode_table(e)), e)

    def test_segments_survive_encoding(self):
        segs = [("S", 3), ("WK", 300), ("", 1), ("SK", 127), ("K", 128), ("SW", 2), ("W", 9)]
        e = table({2: segs})
        self.assertEqual(C.segments(e, 2), segs)
        self.assertEqual(C.decode_table(e).block_frames()[2], sum(n for _, n in segs))

    def test_too_many_rows_is_refused(self):
        with self.assertRaises(C.EncodeError):
            C.encode_segments([(lv, 200) for lv in ("S", "K") * 7])

    def test_swk_is_kept_apart_from_level_0(self):
        e = table({0: [("SWK", 4), ("", 2)]})
        self.assertEqual(C.segments(e, 0, keep_swk=True), [("SWK", 4), ("", 2)])
        self.assertEqual(C.segments(e, 0), [("", 6)])


class Lutc(unittest.TestCase):
    def test_timeline_reads_back_fields_and_rate(self):
        row = s6_lutc.lutc_row(a=[("lo", 8), ("0", 2), ("hi", 8)], b=[("0", 2)], rate=5, M=3)
        e = table({}, lutc=[row])
        self.assertEqual(s6_lutc.lutc_timeline(e),
                         [("lo", 5, 8), ("0", 5, 2), ("hi", 5, 8), ("0", 5, 2)] * 3)
        self.assertEqual(s6_lutc.lutc_frames(e), 60)

    def test_finalize_refuses_forbidden_bytes(self):
        t = s6_lutc.blank(HEADER7)
        t.slots[0] = C.encode_segments(BALANCED)
        e = s6_lutc.finalize(t, forbid=())
        with self.assertRaises(C.EncodeError):
            s6_lutc.finalize(C.decode_table(e), forbid=(e[96],))


class Charge(unittest.TestCase):
    def test_s_frames_count_plus_one_and_k_minus_one(self):
        e = table({0: [("S", 10), ("", 1)], 1: [("K", 4), ("", 1)], 2: BALANCED})
        r = s6_charge.charge(e, pwr=PWR_73, panel="7.3", vsh=15.0, vsl=-15.0)
        self.assertEqual(r["slots"][0]["q_sf"], 10.0)
        self.assertEqual(r["slots"][1]["q_sf"], -4.0)
        self.assertEqual(r["slots"][2]["q_sf"], 0.0)
        self.assertEqual(r["refresh_frames"], 24)

    def test_weak_rails_come_from_pwr_in_custom_and_header_in_stock(self):
        e = table({0: [("W", 10), ("", 1)]})
        custom = s6_charge.charge(e, pwr="3F 00 00 14 00 00", panel="7.3", mode="custom")
        self.assertEqual(custom["rails_V"]["VSL_LV"], -5.0)                 # 3.0 + 0.1 x 0x14
        stock = s6_charge.charge(e, panel="7.3", mode="stock")
        self.assertEqual(stock["rails_V"]["VSL_LV"], -7.2)                  # header byte 2 = 0x2A
        self.assertAlmostEqual(stock["vcom_dc"], -0.0496 * 20, places=3)   # header byte 5

    def test_frame_time_fixed_and_dynamic_pll(self):
        P = s6_charge.PANELS["7.3"]
        e = table({0: BALANCED}, lutc=[s6_lutc.lutc_row(a=[("0", 24)], rate=5)])
        self.assertAlmostEqual(s6_charge.charge(e, pwr=PWR_73, panel="7.3", pll=0x04)["refresh_s"],
                               24 * P.fixed_ms[4] / 1000, places=4)
        self.assertAlmostEqual(s6_charge.charge(e, pwr=PWR_73, panel="7.3", pll=0x08)["refresh_s"],
                               24 * P.dyn_ms[5] / 1000, places=4)

    def test_lutc_swing_must_balance(self):
        row = s6_lutc.lutc_row(a=[("lo", 12), ("0", 12)])
        g = s6_charge.gate(table({0: BALANCED}, lutc=[row]), pwr=PWR_73, panel="7.3", codes=[0])
        self.assertFalse(g["pass"])
        self.assertTrue(any("LUTC" in f for f in g["fail"]))
        row = s6_lutc.lutc_row(a=[("lo", 12), ("hi", 12)])
        self.assertTrue(s6_charge.gate(table({0: BALANCED}, lutc=[row]), pwr=PWR_73, panel="7.3", codes=[0])["pass"])

    def test_ledger_accumulates(self):
        led = s6_charge.Ledger(panel="7.3", pwr=PWR_73, vsh=15.0, vsl=-15.0)
        e = table({0: [("S", 3), ("", 1)]})
        led.add(e, count=2)
        led.add(e)
        self.assertEqual(led.totals()[0], 9.0)

    def test_wrong_length_is_an_error(self):
        with self.assertRaises(ValueError):
            s6_charge.charge(b"\xff" * 799)


class Lint(unittest.TestCase):
    def lint(self, e, **kw):
        kw.setdefault("pwr", PWR_73)
        return s6_lint.lint_table(e, "7.3", "custom", **kw)

    def test_good_table_passes(self):
        r = self.lint(good())
        self.assertTrue(r.ok, r.text())
        self.assertEqual(r.facts["refresh_frames"], 24)
        self.assertEqual(r.facts["busy_ceiling_s"], 90.0)

    def test_format(self):
        self.assertEqual(rules(s6_lint.lint_table(b"\xff" * 535)), {"format"})

    def test_l0_all_zero_image_fails_fast(self):
        r = self.lint(b"\x00" * 800)
        self.assertEqual(rules(r), {"L0", "L7"})

    def test_l1_busy_timeout(self):
        e = table({k: [("S", 2000), ("", 1), ("K", 2000), ("", 1)] for k in range(8)})
        self.assertIn("L1", rules(self.lint(e, busy_timeout_s=10)))
        self.assertIn("L1", rules(self.lint(e, busy_timeout_s=70), "WARN"))
        self.assertTrue(self.lint(e, busy_timeout_s=120).ok)

    def test_l1_unused_longest_slot_warns(self):
        slots = {k: BALANCED for k in range(8)}
        slots[4] = BALANCED * 2
        r = self.lint(table(slots), codes=[0, 1, 2, 3, 5, 6])
        self.assertIn("L1", rules(r, "WARN"))

    def test_l2_is_a_failure_only_for_stock(self):
        slots = {k: BALANCED for k in range(8)}
        slots[3] = BALANCED + [("", 5)]
        e = table(slots)
        self.assertNotIn("L2", rules(self.lint(e)))
        self.assertIn("L2", rules(s6_lint.lint_table(e, "7.3", "stock")))

    def test_l3_charge_limit(self):
        slots = {k: BALANCED for k in range(8)}
        slots[2] = [("S", 12), ("", 2), ("K", 10), ("", 2)]
        self.assertEqual(rules(self.lint(table(slots))), {"L3"})
        slots[2] = [("S", 11), ("", 2), ("K", 10), ("", 2)]            # |q| = 1.0 is allowed
        self.assertTrue(self.lint(table(slots)).ok)

    def test_l4_end_frame(self):
        slots = {k: BALANCED for k in range(8)}
        slots[5] = [("S", 10), ("", 4), ("K", 10)]
        e = table(slots)
        self.assertEqual(rules(self.lint(e)), {"L4"})
        self.assertNotIn("L4", rules(s6_lint.lint_table(e, "7.3", "stock")))

    def test_l5_pwr(self):
        self.assertEqual(rules(s6_lint.lint_table(good(), "7.3", "custom")), {"L5"})
        self.assertEqual(rules(self.lint(good(), pwr="3F 00 32")), {"L5"})
        self.assertTrue(self.lint(good(), pwr="01 " + PWR_73).ok)
        self.assertIn("L5", rules(self.lint(good(), pwr="3F 00 40 2A 0E 2A"), "WARN"))

    def test_l6_border_slot(self):
        slots = {k: BALANCED for k in range(8) if k != 1}
        e = table(slots)
        self.assertEqual(rules(self.lint(e, codes=[0, 2, 3, 5, 6])), {"L6"})
        self.assertTrue(self.lint(e, codes=[0, 2, 3, 5, 6], cdi=0xFF).ok)       # VBD 7 = floating border

    def test_l7_zero_run(self):
        e = bytearray(good())
        e[700:708] = b"\x00" * 8
        self.assertIn("L7", rules(self.lint(bytes(e), codes=[0])))

    def test_l8_is_opt_in(self):
        e = good()
        self.assertTrue(self.lint(e).ok)
        self.assertEqual(rules(self.lint(e, forbid_bytes=(e[96],))), {"L8"})

    def test_hold_codes_are_not_gated_on_the_709(self):
        slots = {k: BALANCED for k in range(8)}
        slots[4] = [("S", 20), ("", 4)]
        r = s6_lint.lint_table(table(slots), "7.09", "custom", pwr="0F 00 28 2C 28 38")
        self.assertTrue(r.ok, r.text())
        self.assertNotIn(4, r.facts["q_sf"])

    def test_events(self):
        ok = s6_lint.lint_events(["RESET", "TSC", "UPLOAD", "REFRESH"])
        self.assertTrue(ok.ok and not ok.findings)
        self.assertFalse(s6_lint.lint_events(["UPLOAD", "TSC", "REFRESH"]).ok)
        self.assertFalse(s6_lint.lint_events(["UPLOAD", "RESET", "REFRESH"]).ok)
        self.assertTrue(s6_lint.lint_events(["UPLOAD", "TSC", "REFRESH"], tsc_reloads=False).ok)
        self.assertEqual(rules(s6_lint.lint_events(["RESET", "UPLOAD"]), "INFO"), {"S1"})


class Bwry(unittest.TestCase):
    def test_repo_waves_round_trip(self):
        seen = 0
        for f in sorted((REPO / "panels" / "waves").glob("*.bin")):
            e = f.read_bytes()
            if len(e) != bwry535.LEN:
                continue
            self.assertEqual(bwry535.encode(bwry535.decode(e)), e, f.name)
            seen += 1
        self.assertGreaterEqual(seen, 2)

    def test_build_terminates_every_block(self):
        e = bwry535.build(bytes(7), {1: [("S", 30), ("K", 30)], 3: [("W", 10)]}, n_frames_lutc=60)
        t = bwry535.decode(e)
        self.assertEqual(t.frames(), [60, 60, 60, 10, 60, 60])
        self.assertEqual(t.segments(1), [("S", 30), ("K", 30)])
        self.assertEqual(bwry535.encode(t), e)
        self.assertEqual(bwry535.code3("SK"), 5)

    def test_wrong_length(self):
        with self.assertRaises(ValueError):
            bwry535.decode(bytes(534))


class Lzss(unittest.TestCase):
    @staticmethod
    def pack(tokens) -> bytes:
        bits = []
        for t in tokens:
            if t[0] == "L":
                bits += [0] + [(t[1] >> i) & 1 for i in range(8)]
            else:
                bits += [1] + [((t[1] - 3) >> i) & 1 for i in range(8)] + [(t[2] >> i) & 1 for i in range(14)]
        bits += [0] * (-len(bits) % 8)
        return bytes(sum(b << i for i, b in enumerate(bits[o:o + 8])) for o in range(0, len(bits), 8))

    def test_literals(self):
        data = bytes(range(256))
        out, toks, _ = bwry_lzss.decode(bwry_lzss.encode_literals(data), max_out=len(data))
        self.assertEqual(out, data)
        self.assertEqual(len(toks), 256)

    def test_match_copies_overlapping(self):
        out, toks, _ = bwry_lzss.decode(self.pack([("L", 0xAB), ("L", 0xCD), ("M", 7, 2)]), max_out=9)
        self.assertEqual(out, bytes([0xAB, 0xCD] * 4 + [0xAB]))
        self.assertEqual(toks[-1][0], "M")

    def test_bad_distance_stops(self):
        out, toks, pos = bwry_lzss.decode(self.pack([("L", 1), ("M", 3, 5)]))
        self.assertEqual(out, b"\x01")
        self.assertEqual(toks[-1], ("BAD", 9, 5))
        self.assertEqual(pos, 9)


class Cli(unittest.TestCase):
    def test_exit_codes(self):
        import contextlib
        import io
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(lutlint.main(["lint", good().hex(), "--pwr", PWR_73]), 0)
            self.assertEqual(lutlint.main(["lint", good().hex()]), 1)
            self.assertEqual(lutlint.main(["charge", good().hex(), "--pwr", PWR_73, "--panel", "7.3"]), 0)
            self.assertEqual(lutlint.main(["show", good().hex()]), 0)


if __name__ == "__main__":
    unittest.main()
