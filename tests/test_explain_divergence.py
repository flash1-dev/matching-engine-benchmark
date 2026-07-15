#!/usr/bin/env python3

import gzip
import importlib.util
import json
import tempfile
import unittest
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts" / "explain_divergence.py"
SPEC = importlib.util.spec_from_file_location("explain_divergence", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class ExplainDivergenceTests(unittest.TestCase):
    def test_finds_first_differing_sequence_with_missing_report(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reference = root / "reference.txt.gz"
            candidate = root / "candidate.txt"
            with gzip.open(reference, "wt", encoding="utf-8") as handle:
                handle.write("0,0,0,1,100,10\n1,2,100,3,1,2\n2,2,0,2,100\n")
            candidate.write_text("0,0,0,1,100,10\n2,2,0,2,100\n", encoding="utf-8")

            result = MODULE.compare_canonical(reference, candidate)

            self.assertFalse(result["conformant"])
            self.assertEqual(result["first_divergent_sequence"], 2)
            self.assertEqual(result["matching_sequences"], 1)
            self.assertEqual(result["reference"]["first_divergent_line"], 2)
            self.assertEqual(result["candidate"]["first_divergent_line"], 2)
            self.assertEqual(result["candidate"]["reports"], ["2,2,0,2,100"])

    def test_cli_writes_json_and_uses_conformance_exit_codes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reference = root / "reference.txt"
            candidate = root / "candidate.txt"
            artifact = root / "divergence.json"
            reference.write_text("0,0,0,1,100,10\n", encoding="utf-8")
            candidate.write_text("0,0,0,1,100,10\n", encoding="utf-8")

            with redirect_stdout(StringIO()):
                exit_code = MODULE.main(
                    [str(reference), str(candidate), "--json-output", str(artifact)]
                )

            self.assertEqual(exit_code, 0)
            self.assertTrue(json.loads(artifact.read_text(encoding="utf-8"))["conformant"])

            candidate.write_text("0,0,0,1,100,11\n", encoding="utf-8")
            with redirect_stdout(StringIO()):
                exit_code = MODULE.main(
                    [str(reference), str(candidate), "--json-output", str(artifact)]
                )

            result = json.loads(artifact.read_text(encoding="utf-8"))
            self.assertEqual(exit_code, 1)
            self.assertFalse(result["conformant"])
            self.assertEqual(result["first_divergent_sequence"], 0)

    def test_rejects_malformed_stream(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "canonical.txt"
            source.write_text("0,1,0,1,100,10\n\n", encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "empty canonical report"):
                MODULE.compare_canonical(source, source)


if __name__ == "__main__":
    unittest.main()
