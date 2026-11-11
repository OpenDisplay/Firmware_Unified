#!/usr/bin/env python3
"""Emit tests/vectors/sec2_vectors.json (py-opendisplay's independent reference, scripts/
gen_sec2_vectors.py) as a C include for the firmware host tests.

Only the groups the firmware can check without a client stack are emitted: V1 KDF, V8 code,
V10 secrets-need-a-key, V11 0x27 at rest / read view, V13 setup URL. Hex stays hex; the test
decodes it. Usage: gen_sec2_vectors.py IN.json OUT.inc
"""
import json
import sys


def c_str(s):
    return '"' + s + '"' if s is not None else "NULL"


def main():
    src, out = sys.argv[1], sys.argv[2]
    with open(src, encoding="utf-8") as f:
        d = json.load(f)
    lines = ["/* GENERATED from tests/vectors/sec2_vectors.json by gen_sec2_vectors.py. */"]

    lines.append("static const struct v1 V1[] = {")
    for v in d["V1_kdf"]:
        lines.append("    { %s, %s, %s, %s }," % (c_str(v["k_in"]), c_str(v["label"]),
                                               c_str(v["context"]), c_str(v["out"])))
    lines.append("};")

    lines.append("static const struct v8 V8[] = {")
    for v in d["V8_code"]:
        lines.append("    { %s, %s }," % (c_str(v["code_raw"]), c_str(v["code"])))
    lines.append("};")

    lines.append("static const struct v10 V10[] = {")
    for v in d["V10_secrets"]:
        verdict = -1 if v["verdict"] == "accept" else int(v["verdict"], 16)
        stored = "".join(v.get("stored_secret_packets") or []) or None
        lines.append("    { %s, %s, %s, %d, %s, %s, %d }," % (
            c_str(v["case"]), c_str(v["device"]), c_str(v["blob"]), verdict,
            c_str(stored), c_str(v.get("k_dev")), 1 if v.get("frames") else 0))
    lines.append("};")

    lines.append("static const struct v11 V11[] = {")
    for v in d["V11_config_at_rest"]:
        lines.append("    { %s, %s, %s, %s, %s, %s, %s, %s }," % (
            c_str(v["case"]), c_str(v.get("k_dev")), c_str(v.get("uid")),
            c_str(v.get("stored_before") or v.get("stored")), c_str(v.get("written")),
            c_str(v.get("stored_after")), c_str(v.get("read_image_frame")),
            "true" if "CONFIG_CLEAR" in v["case"] else "false"))
    lines.append("};")

    lines.append("static const struct v13 V13[] = {")
    for v in d["V13_setup_url"]:
        p = v["params"]
        lines.append("    { '%s', 0x%sU, %d, %d, %d, %d, %d, %d, %d, %s }," % (
            p["k"], p["i"], p["t"], p["v"][0], p["v"][1], p["v"][2], p["b"] or 0, p["d"] or 0,
            p["p"] or 0, c_str(v["url"])))
    lines.append("};")

    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
