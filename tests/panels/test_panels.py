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
    """(class, view) for every target class of an entry, plus any alternatives (none are emitted
    since schema_minor 2; the field stays in the schema)."""
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
        self.assertGreaterEqual(PRESETS["schema_minor"], 2)
        # od_epd streams sequences from storage: one step cap everywhere, BG22 only lacks WINDOW/FILL
        self.assertEqual(CLASSES["bg22"]["seq_max"], 512)
        self.assertEqual(CLASSES["bg22"]["max_steps"], 128)
        self.assertFalse(CLASSES["bg22"]["window"])
        self.assertEqual(CLASSES["default"]["seq_max"], 512)
        self.assertEqual(CLASSES["default"]["max_steps"], 128)
        self.assertTrue(CLASSES["default"]["window"])
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
                self.assertFalse(bg)
                self.assertEqual(e.get("alternatives") or [], [])   # field kept, never emitted

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


class Mode0Invariant(unittest.TestCase):
    """Mode 0 is the fallback for clients that know nothing about modes (docs/
    CONFIG_DRIVEN_DISPLAY.md, "Mode conventions"): every preset's first 0xAE with mode_index 0
    exists, is single-pass, renders 0x20's color_scheme (or 0xFF), and that scheme is the one a
    2.x client derives from the legacy entry, so the 2.x frame is what mode 0 consumes. The
    names "fast" and "partial" are reserved for those refresh kinds."""

    def test_every_preset(self):
        for key, e in PRESETS["panels"].items():
            for cls, v in views(e):
                if not v["packets"]:
                    continue
                with self.subTest(key=key, cls=cls, variant=v.get("variant")):
                    pk = packets(v)
                    modes = [b for _, i, b in pk if i == P.PKT_MODE]
                    mode0 = next((m for m in modes if m[1] == 0), None)
                    self.assertIsNotNone(mode0, "no 0xAE with mode_index 0")
                    self.assertEqual(mode0[15], 1, "mode 0 must be single-pass")
                    self.assertIn(mode0[14], (0xFF, e["color_scheme"]))
                    if e["legacy"].get("color_scheme") is not None:
                        self.assertEqual(e["color_scheme"], e["legacy"]["color_scheme"])
                    for m in modes:
                        name = m[2:14].split(b"\0", 1)[0].decode().lower()
                        if name in ("fast", "partial"):
                            meta = next(x for x in v["modes"] if x["index"] == m[1])
                            self.assertEqual(meta["refresh"], name)
                    self.assertEqual(len({m[1] for m in modes}), len(modes), "duplicate mode_index")


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
                    ref = [tok(t) for t in st["flows"]["sleep"]]
                    bleed = P.POST_SLEEP_BLEED.get(e["name"])
                    if bleed:
                        # the 2.x firmware's post-sleep bleed, outside bb_epaper's stream
                        self.assertEqual(got[-1:], [("DELAY", bleed)])
                        ref.append(("DELAY", bleed))
                    else:
                        self.assertNotIn(("DELAY", 200), got[-1:])
                    self.assertEqual(G.compare(ref, got)["equal"], v["status"] == "equivalent")
        self.assertGreaterEqual(n, 200)

    def test_default_window_mode_refused_by_bg22(self):
        """The BG22-built interpreter really refuses what the bg22 view leaves out (WINDOW/FILL)."""
        e = PRESETS["panels"]["0x0001"]
        m = next(m for m in e["modes"] if m["window"])
        self.assertFalse(any(x["window"] for x in e["targets"]["bg22"]["modes"]))
        rc, _ = P.replay(bytes.fromhex(e["framed_hex"]), str(m["index"]), "bg22", [e["width"], e["height"]])
        self.assertNotEqual(rc, 0)

    def test_long_double_init_runs_on_bg22(self):
        """Full flows longer than the old 128 B BG22 cap (the doubled bb_epaper init) now run there
        unchanged: same full-mode packet as the default class, replayed by the BG22 build."""
        e = PRESETS["panels"]["0x000C"]
        self.assertEqual(e["targets"]["bg22"]["status"], "equivalent")
        full = e["modes"][0]
        self.assertEqual(full["name"], "full")
        self.assertGreater(full["seq_bytes"], 128)
        rc, _ = P.replay(bytes.fromhex(e["framed_hex"]), "0", "bg22", [e["width"], e["height"]])
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
class DisplayPresets(unittest.TestCase):
    """tests/host/display_presets.inc is copied from this database; catch it drifting."""
    WANT = (("EP29YR", "0x001E", None), ("EP42B", "0x0002", "bg22"), ("EP74R", "0x0024", None))

    def test_inc_matches_database(self):
        import re
        src = (REPO / "tests" / "host" / "display_presets.inc").read_text()
        for name, pid, cls in self.WANT:
            m = re.search(r"k_preset_%s\[\] = \{(.*?)\};" % name, src, re.S)
            self.assertIsNotNone(m, name)
            got = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1))).hex()
            ent = PRESETS["panels"][pid]
            want = ent["framed_hex"] if cls is None else ent["targets"][cls]["framed_hex"]
            self.assertEqual(got, want, f"{name} ({pid}, {cls or 'default'}) is stale")


class NativeControllers(unittest.TestCase):
    """0xB3 for the IT8951 / FastEPD parallel ids: the ESP32 firmware selects those drivers from
    this packet only, so every legacy id that had one needs an entry with the same parameters."""
    # id: (class, flags, vcom_mv, parallel_panel, miso, en, ite_en)
    WANT = {3000: (1, 0x05, 1500, 0, 8, 11, 21), 3001: (1, 0x07, 1500, 0, 8, 11, 21),
            3002: (2, 0, 0, 1, 0xFF, 0xFF, 0xFF), 3003: (2, 0, 0, 2, 0xFF, 0xFF, 0xFF),
            3004: (2, 0, 0, 3, 0xFF, 0xFF, 0xFF), 3005: (1, 0x04, 0, 0, 13, 23, 23)}

    def test_entries(self):
        nc = PRESETS["native_controllers"]
        self.assertEqual({e["panel_ic_type"] for e in nc.values()}, set(self.WANT))
        for key, e in nc.items():
            self.assertEqual(key, "0x%04X" % e["panel_ic_type"])
            (pk,) = e["packets"]
            b = bytes.fromhex(pk["body_hex"])
            self.assertEqual((pk["id"], pk["number"], len(b)), (0xB3, 0, 12), key)
            self.assertEqual(e["framed_hex"], P.frame(0, 0xB3, b).hex(), key)
            self.assertEqual((b[0], b[1], b[6], b[11]), (0, 0, 0x7F, 0), key)
            got = (b[2], b[3], b[4] | b[5] << 8, b[7], b[8], b[9], b[10])
            self.assertEqual(got, self.WANT[e["panel_ic_type"]], key)
            self.assertNotIn(key, PRESETS["panels"])


class Stale(unittest.TestCase):
    def test_generated_is_current(self):
        import subprocess
        r = subprocess.run([sys.executable, str(P.PANELS / "tools" / "gen_all.py"), "--check"],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=1)
