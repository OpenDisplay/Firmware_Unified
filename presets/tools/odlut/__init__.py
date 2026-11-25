"""Offline tools for e-paper waveform tables: codecs, a charge calculator and a linter.

s6_codec    800 B Spectra 6 0x20 image <-> rows / segment lists (exact byte round trip)
s6_lutc     the LUTC (VCOM) block: physical fields, timeline, table builders
s6_charge   per-slot charge report, the charge gate, a cumulative ledger
s6_lint     table rules L0-L8 and the upload-ordering rule
bwry535     535 B BWRY register LUT codec
bwry_lzss   decompressor for the BWRY controllers' compressed waveform store

Standard library only. Nothing here talks to hardware.
"""
