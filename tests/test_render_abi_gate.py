"""Offline negative tests for tools/re/analyze_k2_render_abi.py."""

from __future__ import annotations

import importlib.util
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "re" / "analyze_k2_render_abi.py"
SPEC = importlib.util.spec_from_file_location("k2vr_render_abi", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
ABI = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = ABI
SPEC.loader.exec_module(ABI)


def decode(code: bytes, address: int = 0x00401000):
    return list(ABI.make_decoder(skipdata=False).disasm(code, address))


class FunctionContractTests(unittest.TestCase):
    def validate(self, code: bytes, expected_start: bytes = b"\x55\x8B\xEC"):
        start = 0x00401000
        return ABI.validate_function_bytes(
            code,
            start_va=start,
            end_va_exclusive=start + len(code),
            expected_return_va=start + len(code) - 1,
            expected_start=expected_start,
        )

    def test_argumentless_plain_ret_passes(self) -> None:
        result = self.validate(b"\x55\x8B\xEC\x90\x5D\xC3")
        self.assertTrue(result["passed"], result)
        self.assertEqual([], result["callerStackArgumentAccesses"])

    def test_ret_immediate_fails_closed(self) -> None:
        code = b"\x55\x8B\xEC\x5D\xC2\x04\x00"
        start = 0x00401000
        result = ABI.validate_function_bytes(
            code,
            start_va=start,
            end_va_exclusive=start + len(code),
            expected_return_va=start + 4,
            expected_start=b"\x55\x8B\xEC",
        )
        self.assertFalse(result["passed"])
        self.assertIn("callee-stack-pop-detected", result["failures"])

    def test_ebp_caller_argument_access_fails_closed(self) -> None:
        result = self.validate(b"\x55\x8B\xEC\x8B\x45\x08\x5D\xC3")
        self.assertFalse(result["passed"])
        self.assertIn(
            "caller-stack-argument-access-detected", result["failures"]
        )

    def test_wrong_start_bytes_fail_closed(self) -> None:
        result = self.validate(
            b"\x55\x8B\xEC\x90\x5D\xC3", expected_start=b"\x53\x8B\xEC"
        )
        self.assertFalse(result["passed"])
        self.assertIn("start-bytes-mismatch", result["failures"])

    def test_second_return_fails_closed(self) -> None:
        result = self.validate(b"\x55\x8B\xEC\xC3\x90\xC3")
        self.assertFalse(result["passed"])
        self.assertIn("return-count-not-one", result["failures"])

    def test_simple_ebp_alias_argument_access_fails_closed(self) -> None:
        # mov ecx,ebp; add ecx,8; mov eax,[ecx]
        result = self.validate(
            b"\x55\x8B\xEC\x8B\xCD\x83\xC1\x08\x8B\x01\x5D\xC3"
        )
        self.assertFalse(result["passed"])
        self.assertIn(
            "caller-stack-argument-access-detected", result["failures"]
        )

    def test_external_jump_fails_bounded_cfg(self) -> None:
        result = self.validate(b"\x55\x8B\xEC\xEB\x7F\x5D\xC3")
        self.assertFalse(result["passed"])
        self.assertIn("jump-leaves-function-interval", result["failures"])

    def test_indirect_jump_fails_bounded_cfg(self) -> None:
        result = self.validate(b"\x55\x8B\xEC\xFF\xE0\x5D\xC3")
        self.assertFalse(result["passed"])
        self.assertIn("indirect-jump-prevents-bounded-cfg", result["failures"])

    def test_branch_into_instruction_middle_fails(self) -> None:
        # The jump lands on the second byte of the two-byte NOP.
        result = self.validate(b"\x55\x8B\xEC\xEB\x01\x66\x90\x5D\xC3")
        self.assertFalse(result["passed"])
        self.assertIn(
            "branch-target-not-instruction-boundary", result["failures"]
        )


class ReturnUseTests(unittest.TestCase):
    def classify(self, trailing: bytes) -> str:
        instructions = decode(b"\xFF\xD0" + trailing)
        return ABI.analyze_eax_result(instructions, 0)["classification"]

    def test_full_eax_overwrite_means_ignored(self) -> None:
        self.assertEqual("ignored", self.classify(b"\x31\xC0\xC3"))

    def test_eax_read_means_consumed(self) -> None:
        self.assertEqual("consumed", self.classify(b"\x85\xC0\xC3"))

    def test_live_eax_at_return_means_propagated(self) -> None:
        self.assertEqual("propagated", self.classify(b"\xC3"))

    def test_partial_al_write_is_not_a_full_kill(self) -> None:
        self.assertEqual("consumed", self.classify(b"\xB0\x01\xC3"))

    def test_subsequent_call_clobbers_candidate_result(self) -> None:
        self.assertEqual(
            "ignored", self.classify(b"\xE8\x00\x00\x00\x00\xC3")
        )


class CallsiteDiscoveryTests(unittest.TestCase):
    def test_direct_candidate_target_is_found_and_checked(self) -> None:
        start = 0x00401000
        target = ABI.CANDIDATES[0].target_va
        relative = target - (start + 5)
        code = b"\xE8" + struct.pack("<i", relative) + b"\x31\xC0\xC3"
        result = ABI.discover_callsites(start, code)
        self.assertEqual(1, len(result["directTargetCalls"]))
        self.assertEqual(
            "ignored",
            result["directTargetCalls"][0]["eaxResult"]["classification"],
        )

    def test_virtual_slot_shape_never_claims_class_identity(self) -> None:
        # mov edx,[ecx]; mov eax,[edx+18h]; call eax; xor eax,eax; ret
        instructions = decode(b"\x8B\x11\x8B\x42\x18\xFF\xD0\x31\xC0\xC3")
        shape = ABI.virtual_slot_shape(instructions, 2)
        self.assertIsNotNone(shape)
        self.assertEqual(0x18, shape["slotOffset"])
        self.assertTrue(shape["hasNearbyVptrLoad"])
        self.assertNotIn("candidate", shape)

    def test_non_slot_indirect_call_is_not_misclassified(self) -> None:
        instructions = decode(b"\xFF\xD0\x31\xC0\xC3")
        self.assertIsNone(ABI.virtual_slot_shape(instructions, 0))


class FailClosedInputTests(unittest.TestCase):
    def test_unpinned_decoder_version_is_rejected(self) -> None:
        with mock.patch.object(ABI.capstone, "__version__", "unexpected"):
            with self.assertRaisesRegex(
                ValueError, "Refusing unpinned analysis dependency"
            ):
                ABI.validate_dependency_versions()

    def test_unknown_hash_is_rejected_before_pe_analysis(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "unknown.exe"
            path.write_bytes(b"not the pinned game executable")
            with self.assertRaisesRegex(ValueError, "Refusing unknown executable"):
                ABI.analyze(path)

    def test_vtable_mismatch_is_a_candidate_failure(self) -> None:
        candidate = ABI.CANDIDATES[0]

        class FakeImage:
            def u32_at_va(self, address: int):
                if address == candidate.vtable_va - 4:
                    return candidate.complete_object_locator_va
                if address == candidate.cell_va:
                    return candidate.target_va + 1
                return None

            def section_name(self, address: int, size: int = 1):
                return ".rdata" if address == candidate.vtable_va else ".text"

            def bytes_at_va(self, address: int, size: int):
                if address == candidate.target_va and size == len(candidate.expected_start):
                    return candidate.expected_start
                return None

            def pointer_occurrences(self, value: int):
                return [
                    {
                        "rawOffset": "0x00000000",
                        "mappedVa": ABI.hex_va(candidate.cell_va),
                    }
                ]

        result = ABI.validate_candidate(FakeImage(), candidate)
        self.assertFalse(result["passed"])
        self.assertIn("vtable-cell-target-mismatch", result["failures"])


if __name__ == "__main__":
    unittest.main()
