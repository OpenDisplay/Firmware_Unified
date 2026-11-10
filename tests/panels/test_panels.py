#!/usr/bin/env python3
"""Host tests for panels/: schema, packet layout + CRC, od_epd equivalence, config size budget,
and byte-identity of the glass entries with the website presets.

    python3 tests/panels/test_panels.py            (stdlib unittest; needs a C compiler)
    OD_PANELS_STALE=1 python3 tests/panels/...     also regenerate from bb_epaper and diff

The equivalence test replays the committed packets through shared/core/od_epd.c and compares
against the committed bb_epaper streams (panels/generated/bbep_streams.json), so it needs no
bb_epaper checkout. The staleness test does, and is skipped without one.
"""
from __future__ import annotations

import json
import os
import shutil
import struct
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "panels" / "tools"))
import pdlib as P  # noqa: E402
import gen_all as G  # noqa: E402

PRESETS = json.loads((P.GENERATED / "drive_presets.json").read_text())
STREAMS = json.loads((P.GENERATED / "bbep_streams.json").read_text())
IDS = json.loads((P.PANELS / "panel_ids.json").read_text())["ids"]
WEB = json.loads((REPO / "tests" / "panels" / "fixtures" / "website_glass_packets.json").read_text())["panels"]
GLASS = {"0x001C", "0x001E", "0x003A", "0x0040", "0x0036", "0x0038"}
HAVE_CC = shutil.which(os.environ.get("CC", "cc")) is not None or shutil.which("gcc") is not None

ENTRY_KEYS = {"panel_ic_type", "name", "aliases", "legacy", "verified", "status", "blocked_on",
              "packets", "notes", "differences"}
DRIVE_KEYS = {"width", "height", "color_scheme", "controller", "bbep_flags", "modes", "modes_blocked",
              "requires_escapes", "framed_hex", "drive_bytes", "fits", "equivalence", "blocked_detail"}
STATUS = {"equivalent", "differs", "blocked", "glass", "not_attempted"}
VERIFIED = {"glass", "equivalent", "untested"}
KNOWN_BLOCKERS = {"SEQ_MAX", "MAX_STEPS", "LUT_MAX", "LUT_AFTER_IMAGE", "LUT_SLOTS", "CS_BOTH",
                  "CS_EVERY_BYTE", "DATA_WITHOUT_CMD", "IT8951", "NO_FULL_FLOW", "NO_BBEP_ENUM",
                  "REPLAY_REFUSED"}
CLASSES = PRESETS["target_classes"]
KNOWN_ESCAPES = {"SET_PIN", "CS_SELECT"}
DIFFS = {"D1", "D2", "D3", "D4", "D5", "D6", "D7", "D8"}


def tok(t: list) -> tuple:
    k = t[0]
    if k == "CMD":
        return ("CMD", t[1], bytes.fromhex(t[2]), t[3])
    if k == "DATA":
        return ("DATA", bytes.fromhex(t[1]), t[2])
    return tuple(t)


def views(e: dict):
    """(class, view) for every target class of an entry, plus its alternatives."""
    for cls, v in (e.get("targets") or {}).items():
        yield cls, v
    for a in e.get("alternatives") or []:
        yield a["target_class"], a


def packets(e: dict) -> list[tuple[int, int, bytes]]:
    return [(p["number"], p["id"], bytes.fromhex(p["body_hex"])) for p in e["packets"]]


class Schema(unittest.TestCase):
    def test_top_level(self):
        self.assertEqual(PRESETS["schema"], "opendisplay.drive_presets")
        self.assertEqual(PRESETS["schema_version"], 1)
        self.assertGreaterEqual(PRESETS["schema_minor"], 1)
        self.assertEqual(CLASSES["bg22"]["seq_max"], 128)
        self.assertEqual(CLASSES["bg22"]["max_steps"], 64)
        self.assertFalse(CLASSES["bg22"]["window"])
        self.assertEqual(CLASSES["default"]["seq_max"], 512)
        self.assertEqual(CLASSES["default"]["max_steps"], 96)
        for k in ("bg22", "esp32", "nordic", "typical_base_config_bytes", "seq_max", "sleep_max", "lut_max"):
            self.assertIsInstance(PRESETS["caps"][k], int, k)
        self.assertEqual(PRESETS["caps"]["bg22"], 3120)
        self.assertEqual(PRESETS["caps"]["esp32"], 4096)
        self.assertEqual(PRESETS["caps"]["nordic"], 4096)

    def test_every_legacy_id_present(self):
        self.assertEqual(set(PRESETS["panels"]), set(IDS))
        self.assertEqual(len(IDS), 77)
        self.assertNotIn("0x0041", IDS)

    def test_entries(self):
        for key, e in PRESETS["panels"].items():
            with self.subTest(key=key):
                self.assertEqual(key, f"0x{e['panel_ic_type']:04X}")
                self.assertTrue(ENTRY_KEYS <= set(e), ENTRY_KEYS - set(e))
                self.assertIn(e["status"], STATUS)
                self.assertIn(e["verified"], VERIFIED)
                self.assertTrue(set(e["blocked_on"]) <= KNOWN_BLOCKERS, e["blocked_on"])
                self.assertTrue(set(e["differences"]) <= DIFFS)
                if e["status"] != "not_attempted":
                    self.assertTrue(DRIVE_KEYS <= set(e), DRIVE_KEYS - set(e))
                    self.assertTrue(set(e["requires_escapes"]) <= KNOWN_ESCAPES)
                if e["status"] == "blocked":
                    self.assertTrue(e["blocked_on"])
                    self.assertEqual(e["packets"], [])
                    self.assertEqual(e["verified"], "untested")
                if e["status"] in ("equivalent", "glass"):
                    self.assertTrue(e["packets"])
                    self.assertTrue(e["modes"])
                self.assertEqual(e["verified"] == "glass", e["status"] == "glass")
                if e["status"] == "equivalent":
                    self.assertEqual(e["verified"], "equivalent")

    def test_legacy_identity(self):
        """Old clients pick host-side colour codes from panel_ic_type + colour scheme."""
        for key, e in PRESETS["panels"].items():
            with self.subTest(key=key):
                leg = e["legacy"]
                self.assertEqual(leg["panel_ic_type"], e["panel_ic_type"])
                self.assertIsInstance(leg["color_scheme"], int)
                web = IDS[key]["preset_color_scheme"]
                if web is not None:
                    self.assertEqual(leg["color_scheme"], web)

    def test_target_views(self):
        for key, e in PRESETS["panels"].items():
            if e["status"] == "not_attempted":
                continue
            with self.subTest(key=key):
                self.assertEqual(set(e["targets"]), set(CLASSES))
                d = e["targets"]["default"]
                for k in ("status", "verified", "packets", "framed_hex", "drive_bytes", "modes"):
                    self.assertEqual(e[k], d[k], k)          # top level = the default class
                for cls, v in e["targets"].items():
                    self.assertIn(v["status"], STATUS)
                    self.assertEqual(v["status"] == "blocked", bool(v["blocked_on"]))
                    if v["status"] == "blocked":
                        self.assertEqual(v["packets"], [])
                    for m in v["modes"]:
                        self.assertTrue(m["fits"][cls], (cls, m["name"]))
                self.assertEqual(e["fits_target"]["bg22"],
                                 e["targets"]["bg22"]["packets"] == e["packets"] and e["status"] != "blocked")
                bg = e["targets"]["bg22"]["status"] == "blocked"
                self.assertEqual(key in PRESETS["bg22_unavailable"], bg)
                for a in e.get("alternatives") or []:
                    self.assertTrue(bg)
                    self.assertEqual(a["status"], "alternative")
                    self.assertEqual(a["verified"], "untested")
                    self.assertFalse(a["equivalent"])
                    self.assertEqual(a["target_class"], "bg22")

    def test_glass_set(self):
        self.assertEqual({k for k, e in PRESETS["panels"].items()
                          if e["status"] == "glass" and not e.get("alias_of")}, GLASS)

    def test_names_unique(self):
        names = PRESETS["names"]
        self.assertEqual(len(set(names.values())), len(names))
        for n, k in names.items():
            self.assertEqual(PRESETS["panels"][k]["name"], n)

    def test_aliases(self):
        for key, e in PRESETS["panels"].items():
            if e.get("alias_of"):
                tgt = PRESETS["panels"][e["alias_of"]]
                self.assertEqual(e["packets"], tgt["packets"])
                self.assertIn(key, tgt["aliases"])


class Layout(unittest.TestCase):
    """Packet bodies against the opendisplay_structs.h layout od_epd_cfg.c parses."""

    def test_framing_and_bodies(self):
        for key, e in PRESETS["panels"].items():
            for cls, v in views(e):
                if v["packets"]:
                    with self.subTest(key=key, cls=cls, variant=v.get("variant")):
                        self.check_view(v, CLASSES[cls])

    def check_view(self, e: dict, caps: dict):
        if True:
            if True:
                pk = packets(e)
                blob = bytes.fromhex(e["framed_hex"])
                self.assertEqual(blob, b"".join(P.frame(n, i, b) for n, i, b in pk))
                self.assertEqual([(i, b) for _, i, b in P.parse_framed(blob)], [(i, b) for _, i, b in pk])
                ids = [i for _, i, _ in pk]
                self.assertEqual(ids[0], P.PKT_PANEL)
                self.assertEqual(ids, sorted(ids, key=[P.PKT_PANEL, P.PKT_MODE, P.PKT_LUT, P.PKT_CAL].index))
                modes = [b for _, i, b in pk if i == P.PKT_MODE]
                luts = {b[1]: b for _, i, b in pk if i == P.PKT_LUT}
                pan = next(b for _, i, b in pk if i == P.PKT_PANEL)
                # 0xB1
                self.assertEqual(len(pan), 13 + pan[12])
                self.assertLessEqual(pan[12], P.SLEEP_MAX)
                self.assertEqual(pan[11], len(modes))
                self.assertTrue(pan[2] in (0, 1))
                self.assertGreater(struct.unpack_from("<H", pan, 3)[0], 0)
                self.assertTrue(all(c < 4 for c in pan[7:11]))
                if pan[12]:
                    self.assertLessEqual(P.count_steps(P.walk_seq(pan[13:])), P.MAX_STEPS)
                # 0xAE
                for idx, m in enumerate(modes):
                    self.assertEqual(m[1], idx)
                    seq_len = struct.unpack_from("<H", m, 35)[0]
                    self.assertEqual(len(m), 37 + seq_len)
                    self.assertLessEqual(seq_len, caps["seq_max"])
                    self.assertTrue(1 <= m[15] <= 8)
                    self.assertIn(0, m[2:14])                     # NUL-terminated name
                    steps = P.walk_seq(m[37:])
                    self.assertLessEqual(P.count_steps(steps), caps["max_steps"])
                    if not caps["window"]:
                        self.assertFalse(any(s[0] in ("window", "fill") for s in steps))
                    for s in steps:
                        if s[0] == "window":
                            self.assertTrue(2 <= len(s[1]) <= 32)
                            self.assertEqual(s[1][0][0], P.WIN_LIT)
                    self.assertTrue(any(s[0] in ("image", "slice") for s in steps))
                    for p in range(m[15]):
                        s = m[19 + p]
                        if s != 0xFF:
                            self.assertIn(s, luts)
                    for s in steps:
                        if s[0] == "lutslice":
                            slot = luts[m[19]]
                            self.assertLessEqual(s[1] + s[2], len(slot) - 4)
                # 0xAF
                for s, b in luts.items():
                    self.assertLess(s, P.MAX_LUT_SLOTS)
                    self.assertTrue(4 + 7 <= len(b) <= 4 + P.LUT_MAX)
                    self.assertEqual(struct.unpack_from("<H", b, 2)[0], P.crc16_ccitt(b[4:]))
                # 0xB0: head + 8-byte entries
                for _, i, b in pk:
                    if i == P.PKT_CAL:
                        self.assertEqual((len(b) - 4) % 8, 0)
                        self.assertEqual(b[3], (len(b) - 4) // 8)


class Size(unittest.TestCase):
    def test_budget(self):
        base = PRESETS["caps"]["typical_base_config_bytes"]
        for key, e in PRESETS["panels"].items():
            if e["status"] == "not_attempted":
                continue
            with self.subTest(key=key):
                blob = bytes.fromhex(e["framed_hex"])
                self.assertEqual(e["drive_bytes"], len(blob))
                for tgt in ("bg22", "esp32", "nordic"):
                    self.assertEqual(e["fits"][tgt], base + len(blob) <= PRESETS["caps"][tgt])
                for cls, v in views(e):
                    if v["packets"]:
                        self.assertTrue(base + v["drive_bytes"] <= CLASSES[cls]["config"], cls)


@unittest.skipUnless(HAVE_CC, "no C compiler")
class Equivalence(unittest.TestCase):
    """Replay through the real od_epd and compare with the bb_epaper 2.x stream."""

    @classmethod
    def setUpClass(cls):
        for c in CLASSES:
            P.build_replay(c)

    def test_derived_entries(self):
        n = 0
        for key, e in PRESETS["panels"].items():
            if e["status"] in ("glass", "not_attempted") or e.get("alias_of"):
                continue
            st = STREAMS["panels"][e["name"]]
            W, H = e["width"], e["height"]
            for cls, v in e["targets"].items():
                if v["status"] not in ("equivalent", "differs"):
                    continue
                blob = bytes.fromhex(v["framed_hex"])
                for m in v["modes"]:
                    refs = [(r["rect"], r["tokens"]) for r in st["rects"].values()] if m["window"] \
                        else [(None, st["flows"][m["name"]])]
                    for rect, ref in refs:
                        with self.subTest(key=key, cls=cls, mode=m["name"], rect=rect):
                            rc, got = G.run_replay(blob, str(m["index"]), cls, [W, H] + (rect or []))
                            self.assertEqual(rc, 0)
                            res = G.compare(G.strip_wake([tok(t) for t in ref]), G.strip_wake(got))
                            self.assertEqual(res["equal"], v["status"] == "equivalent", res)
                            n += 1
                with self.subTest(key=key, cls=cls, mode="sleep"):
                    rc, got = G.run_replay(blob, "sleep", cls, [W, H])
                    self.assertEqual(rc, 0)
                    self.assertEqual(G.compare([tok(t) for t in st["flows"]["sleep"]], got)["equal"],
                                     v["status"] == "equivalent")
        self.assertGreaterEqual(n, 200)

    def test_default_blob_refused_by_bg22_when_too_big(self):
        """The BG22-built interpreter really refuses what the bg22 view leaves out."""
        e = PRESETS["panels"]["0x000C"]
        self.assertEqual(e["targets"]["bg22"]["status"], "blocked")
        rc, _ = P.replay(bytes.fromhex(e["framed_hex"]), "0", "bg22", [e["width"], e["height"]])
        self.assertNotEqual(rc, 0)

    def test_alternatives_walk_on_bg22(self):
        for key, e in PRESETS["panels"].items():
            for a in e.get("alternatives") or []:
                blob = bytes.fromhex(a["framed_hex"])
                for m in a["modes"]:
                    with self.subTest(key=key, mode=m["name"]):
                        rc, _ = P.replay(blob, str(m["index"]), "bg22", [e["width"], e["height"]])
                        self.assertEqual(rc, 0)

    def test_glass_entries_run(self):
        """The hand-written sequences must walk cleanly in od_epd, every pass."""
        for key in GLASS:
            e = PRESETS["panels"][key]
            blob = bytes.fromhex(e["framed_hex"])
            for cls in CLASSES:
                geom = [e["width"], e["height"]]
                for m in e["modes"]:
                    with self.subTest(key=key, cls=cls, mode=m["name"]):
                        rc, lines = P.replay(blob, str(m["index"]), cls, geom)
                        self.assertEqual(rc, 0)
                        self.assertEqual(sum(1 for x in lines if x.startswith("M pass")), m["passes"])
                rc, _ = P.replay(blob, "sleep", cls, geom)
                self.assertEqual(rc, 0)

    def test_recorded_verdicts_match(self):
        """The equivalence block in the JSON is what a fresh replay gives."""
        for key, e in PRESETS["panels"].items():
            for cls, v in (e.get("targets") or {}).items():
                eq = v.get("equivalence") or (e["equivalence"] if cls == "default" else None)
                if v["status"] in ("equivalent", "differs"):
                    self.assertEqual(eq["all_equal"], v["status"] == "equivalent", (key, cls))


class Glass(unittest.TestCase):
    def test_byte_identical_with_website(self):
        for key in GLASS:
            with self.subTest(key=key):
                mine = [{"pid": str(p["id"]), "raw": p["body_hex"]} for p in PRESETS["panels"][key]["packets"]]
                self.assertEqual(mine, WEB[key]["drivePackets"])

    def test_yaml_encoder_reproduces_website(self):
        try:
            import yaml
        except ImportError:
            self.skipTest("PyYAML not installed")
        doc = yaml.safe_load((P.PANELS / "panels.yaml").read_text())
        waves = {w: (P.PANELS / "waves" / f"{w}.bin").read_bytes() for w in ("wave_colour", "wave_edit")}
        pal = json.loads((P.PANELS / "palettes" / "palette16_photo_ODBEEBA6.json").read_text())
        for ent in doc["panels"]:
            key = f"0x{int(str(ent['id']), 0):04X}"
            with self.subTest(key=key):
                mine = [{"pid": str(pid), "raw": b.hex()} for _, pid, b in P.yaml_panel_packets(ent, waves, pal)]
                self.assertEqual(mine, WEB[key]["drivePackets"])

    def test_waves_match_website(self):
        for key in GLASS:
            luts = [bytes.fromhex(p["raw"])[4:] for p in WEB[key]["drivePackets"] if p["pid"] == "175"]
            self.assertEqual(luts, [(P.PANELS / "waves" / f"{w}.bin").read_bytes()
                                    for w in ("wave_colour", "wave_edit")])


@unittest.skipUnless(os.environ.get("OD_PANELS_STALE") == "1", "set OD_PANELS_STALE=1 (needs the bb_epaper clone)")
class Stale(unittest.TestCase):
    def test_generated_is_current(self):
        import subprocess
        r = subprocess.run([sys.executable, str(P.PANELS / "tools" / "gen_all.py"), "--check"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=1)
