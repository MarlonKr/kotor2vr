"""Synthetic tests for tools/re/analyze_render_trace.py."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "re" / "analyze_render_trace.py"
SPEC = importlib.util.spec_from_file_location("k2vr_render_trace_parser", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
PARSER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = PARSER
SPEC.loader.exec_module(PARSER)


def call(
    hook: str,
    phase: str,
    call_id: int,
    parent: int,
    depth: int,
    qpc: int,
    *,
    tid: int = 7,
    duration_qpc: int = 0,
) -> dict[str, object]:
    return {
        "type": "call",
        "hook": hook,
        "phase": phase,
        "call_id": call_id,
        "parent": parent,
        "depth": depth,
        "tid": tid,
        "qpc": qpc,
        "this": "0x00123456",
        "duration_qpc": duration_qpc,
        "duration_us": duration_qpc,
    }


class TraceParserTests(unittest.TestCase):
    def write_lines(self, directory: str, lines: list[object]) -> Path:
        path = Path(directory) / "trace.jsonl"
        with path.open("w", encoding="utf-8") as stream:
            for item in lines:
                if isinstance(item, str):
                    stream.write(item + "\n")
                else:
                    stream.write(json.dumps(item) + "\n")
        return path

    def test_newest_session_chain_pairing_counts_and_summary(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_lines(
                directory,
                [
                    {"type": "session", "pid": 1, "qpc_frequency": 10},
                    call("old", "entry", 99, 0, 0, 1),
                    {"type": "session", "pid": 2, "qpc_frequency": 1_000_000,
                     "hooks": 3, "duration_ms": 600000},
                    call("camera+0x08", "entry", 1, 0, 0, 100),
                    call("scene+0x18", "entry", 2, 1, 1, 120),
                    call("scene+0xB8", "entry", 3, 2, 2, 140),
                    call("scene+0xB8", "exit", 3, 2, 2, 180,
                         duration_qpc=40),
                    call("scene+0x18", "exit", 2, 1, 1, 200,
                         duration_qpc=80),
                    call("camera+0x08", "exit", 1, 0, 0, 250,
                         duration_qpc=150),
                    '{"type":"call",',
                    {"type": "summary", "written": 6, "drops": 2,
                     "reason": "explicit"},
                ],
            )
            report = PARSER.analyze_trace(path)

        self.assertEqual(2, report["session"]["pid"])
        self.assertEqual(2, report["source"]["sessions_found"])
        self.assertEqual(6, report["counts"]["valid_call_records"])
        self.assertEqual(2, report["counts"]["by_hook"]["scene+0x18"]["total"])
        self.assertEqual(6, report["counts"]["by_thread"]["7"]["total"])
        self.assertEqual(3, report["pairs"]["coherent_pairs"])
        self.assertEqual(0, report["pairs"]["unbalanced_id_count"])
        self.assertEqual(1, report["parent_chain"]["complete_count"])
        self.assertEqual(150, report["timespan"]["span_qpc"])
        self.assertEqual(0.00015, report["timespan"]["span_seconds"])
        self.assertEqual(2, report["summary"]["drops"])
        self.assertTrue(report["summary"]["written_matches_observed"])
        self.assertEqual(1, report["input_quality"]["malformed_json_line_count"])
        self.assertEqual(40.0, report["durations"]["by_hook"]["scene+0xB8"]["p99_us"])

    def test_duplicate_unbalanced_and_mismatched_pairs_are_reported(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_lines(
                directory,
                [
                    {"type": "session", "pid": 12, "qpc_frequency": 1000},
                    call("camera+0x08", "entry", 1, 0, 0, 10),
                    call("camera+0x08", "entry", 1, 0, 0, 11),
                    call("camera+0x08", "exit", 1, 0, 0, 20),
                    call("scene+0x18", "exit", 2, 1, 1, 30),
                    call("scene+0xB8", "entry", 3, 2, 2, 40),
                    call("scene+0x18", "entry", 4, 0, 0, 50),
                    call("scene+0xB8", "exit", 4, 0, 0, 60),
                ],
            )
            report = PARSER.analyze_trace(path)

        self.assertEqual(1, report["pairs"]["duplicate_id_count"])
        self.assertEqual(3, report["pairs"]["unbalanced_id_count"])
        self.assertEqual(1, report["pairs"]["pair_mismatch_count"])
        self.assertEqual(0, report["pairs"]["coherent_pairs"])
        self.assertEqual(0, report["parent_chain"]["complete_count"])

    def test_missing_or_invalid_session_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            missing = self.write_lines(directory, [{"type": "summary", "drops": 0}])
            with self.assertRaises(PARSER.TraceAnalysisError):
                PARSER.analyze_trace(missing)

            invalid = self.write_lines(
                directory,
                [{"type": "session", "pid": 1, "qpc_frequency": 0}],
            )
            with self.assertRaises(PARSER.TraceAnalysisError):
                PARSER.analyze_trace(invalid)

    def test_cli_writes_json_and_returns_nonzero_for_invalid_session(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            valid = self.write_lines(
                directory,
                [{"type": "session", "pid": 5, "qpc_frequency": 1000}],
            )
            output = Path(directory) / "report.json"
            completed = subprocess.run(
                [sys.executable, str(MODULE_PATH), str(valid),
                 "--json-output", str(output)],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, completed.returncode, completed.stderr)
            self.assertEqual(5, json.loads(output.read_text(encoding="utf-8"))["session"]["pid"])

            invalid = self.write_lines(directory, ["not JSON"])
            completed = subprocess.run(
                [sys.executable, str(MODULE_PATH), str(invalid)],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(0, completed.returncode)
            self.assertIn("ERROR:", completed.stderr)


if __name__ == "__main__":
    unittest.main()
