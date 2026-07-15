#!/usr/bin/env python3
"""Locate the first differing sequence in two canonical report streams."""

import argparse
import gzip
import itertools
import json
import sys
from pathlib import Path


FIELD_COUNTS = {0: 6, 1: 6, 2: 5, 3: 6, 4: 3, 5: 3}


def canonical_groups(path):
    """Yield validated canonical reports one originating sequence at a time."""
    source = Path(path)
    opener = gzip.open if source.suffix == ".gz" else open
    try:
        with opener(source, "rt", encoding="utf-8") as handle:
            previous_sequence = -1
            current_sequence = None
            current_line = None
            reports = []
            for line_number, raw_line in enumerate(handle, 1):
                line = raw_line.rstrip("\n")
                if not line:
                    raise ValueError(f"{source}:{line_number}: empty canonical report")
                fields = line.split(",")
                try:
                    report_type = int(fields[0])
                    sequence = int(fields[1])
                except (IndexError, ValueError) as error:
                    raise ValueError(
                        f"{source}:{line_number}: malformed canonical report"
                    ) from error
                if (
                    report_type not in FIELD_COUNTS
                    or len(fields) != FIELD_COUNTS[report_type]
                ):
                    raise ValueError(
                        f"{source}:{line_number}: invalid field count for "
                        f"report type {report_type}"
                    )
                if sequence < previous_sequence:
                    raise ValueError(
                        f"{source}:{line_number}: report sequences are not sorted"
                    )
                previous_sequence = sequence

                if current_sequence is not None and sequence != current_sequence:
                    yield current_sequence, current_line, reports
                    reports = []
                    current_line = None
                if current_line is None:
                    current_sequence = sequence
                    current_line = line_number
                reports.append(line)
            if current_sequence is not None:
                yield current_sequence, current_line, reports
    except (OSError, UnicodeError) as error:
        raise ValueError(f"could not read {source}: {error}") from error


def compare_canonical(reference_path, candidate_path):
    """Return a versioned first-divergence artifact for two report streams."""
    matching_sequences = 0
    reference_group = None
    candidate_group = None
    for reference_group, candidate_group in itertools.zip_longest(
        canonical_groups(reference_path), canonical_groups(candidate_path)
    ):
        if reference_group != candidate_group:
            break
        matching_sequences += 1
    else:
        reference_group = None
        candidate_group = None

    divergent_sequences = [
        group[0] for group in (reference_group, candidate_group) if group is not None
    ]
    first_sequence = min(divergent_sequences, default=None)

    def side(path, group):
        reports = []
        line = None
        if group is not None and group[0] == first_sequence:
            _, line, reports = group
        return {
            "path": str(path),
            "first_divergent_line": line,
            "reports": reports,
        }

    return {
        "schema_version": 1,
        "artifact_type": "matching-engine-benchmark.canonical-divergence",
        "conformant": first_sequence is None,
        "first_divergent_sequence": first_sequence,
        "matching_sequences": matching_sequences,
        "reference": side(reference_path, reference_group),
        "candidate": side(candidate_path, candidate_group),
    }


def print_human(result):
    if result["conformant"]:
        print("Canonical report streams are identical")
        return
    print(f"First divergent sequence: {result['first_divergent_sequence']}")
    for name in ("reference", "candidate"):
        side = result[name]
        line = side["first_divergent_line"]
        location = f"{side['path']}:{line}" if line is not None else side["path"]
        print(f"{name.title()} ({location}):")
        if side["reports"]:
            for report in side["reports"]:
                print(f"  {report}")
        else:
            print("  <no reports for this sequence>")


def build_parser():
    parser = argparse.ArgumentParser(
        description="locate the first divergent sequence in canonical report streams"
    )
    parser.add_argument("reference", help="reference canonical output (.txt or .gz)")
    parser.add_argument("candidate", help="candidate canonical output (.txt or .gz)")
    parser.add_argument("--json", action="store_true", help="print only JSON to stdout")
    parser.add_argument("--json-output", help="also write the versioned JSON artifact here")
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        result = compare_canonical(args.reference, args.candidate)
        encoded = json.dumps(result, indent=2, sort_keys=True) + "\n"
        if args.json_output:
            Path(args.json_output).write_text(encoded, encoding="utf-8")
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    if args.json:
        print(encoded, end="")
    else:
        print_human(result)
        if args.json_output:
            print(f"JSON artifact: {args.json_output}")
    return 0 if result["conformant"] else 1


if __name__ == "__main__":
    sys.exit(main())
