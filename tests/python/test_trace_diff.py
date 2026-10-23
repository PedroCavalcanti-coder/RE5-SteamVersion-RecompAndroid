#!/usr/bin/env python3
"""test_trace_diff.py - Testes unitários para o script trace_diff.py."""

import os
import struct
import tempfile
import unittest

import sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "scripts"))
import trace_diff


def make_packet(frame=0, hook=1, char=0, dt=1.0/60.0, stick=(0.0, 0.0),
                pos=(527.87, -133.15, 226.96), move=(0.0, 0.0, 0.0),
                flags=0x30001000, mask=0x3FFFFF00, crc=0x12345678):
    pkt = struct.pack(
        trace_diff.PACKET_FMT,
        trace_diff.RE5_TRACE_MAGIC,
        frame,
        hook,
        char,
        dt,
        stick[0],
        stick[1],
        pos[0], pos[1], pos[2],
        pos[0], pos[1], pos[2],
        move[0], move[1], move[2],
        flags,
        mask,
        35.0,
        0.70710678,
        0.57357644,
        crc,
        b"\x00" * 40
    )
    return pkt


class TestTraceDiff(unittest.TestCase):

    def test_packet_unpack(self):
        raw = make_packet(frame=10, hook=1, char=0, stick=(0.0, 1.0))
        pkt = trace_diff.Packet(raw)
        self.assertEqual(pkt.magic, trace_diff.RE5_TRACE_MAGIC)
        self.assertEqual(pkt.frameIndex, 10)
        self.assertEqual(pkt.hookSite, 1)
        self.assertEqual(pkt.characterId, 0)
        self.assertAlmostEqual(pkt.inputStickY, 1.0, places=5)
        self.assertAlmostEqual(pkt.pos[0], 527.87, places=2)

    def test_compare_identical(self):
        raw = make_packet()
        p1 = trace_diff.Packet(raw)
        p2 = trace_diff.Packet(raw)
        diffs = trace_diff.compare_packets(p1, p2)
        self.assertEqual(len(diffs), 0)

    def test_scenario_a_divergence(self):
        # Divergência no frame 0 em filterMask
        p1 = trace_diff.Packet(make_packet(frame=0, mask=0x3FFFFF00))
        p2 = trace_diff.Packet(make_packet(frame=0, mask=0x00000000))
        diffs = trace_diff.compare_packets(p1, p2)
        self.assertTrue(any(d[0] == "filterMask" for d in diffs))

        title, _ = trace_diff.classify_scenario(0, 1, [d[0] for d in diffs])
        self.assertIn("Cenário A", title)

    def test_scenario_b_divergence(self):
        # Divergência no Pre-hook (moveVec já chega diferente)
        p1 = trace_diff.Packet(make_packet(frame=125, hook=1, move=(0.05, 0.0, 0.0)))
        p2 = trace_diff.Packet(make_packet(frame=125, hook=1, move=(0.00, 0.0, 0.0)))
        diffs = trace_diff.compare_packets(p1, p2)
        self.assertTrue(any(d[0] == "moveVec[X]" for d in diffs))

        title, _ = trace_diff.classify_scenario(125, 1, [d[0] for d in diffs])
        self.assertIn("Cenário B", title)

    def test_scenario_c_divergence(self):
        # Pre é igual, mas Post diverge
        p1 = trace_diff.Packet(make_packet(frame=130, hook=2, pos=(530.0, -133.0, 227.0)))
        p2 = trace_diff.Packet(make_packet(frame=130, hook=2, pos=(527.8, -133.0, 227.0)))
        diffs = trace_diff.compare_packets(p1, p2)
        self.assertTrue(any(d[0] == "pos[X]" for d in diffs))

        title, _ = trace_diff.classify_scenario(130, 2, [d[0] for d in diffs])
        self.assertIn("Cenário C", title)


if __name__ == "__main__":
    unittest.main()
