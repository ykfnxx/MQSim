#!/usr/bin/env python3
"""Multi-translation-page regression with independently counted host/TRIM results.

Unlike the 64-128-page stress cases, these inputs span multiple mapping pages.
The expected result is full drain, exact host/TRIM accounting, and evidence
that mapping I/O and GC actually ran. No current simulator output is blessed.
"""

import argparse
import tempfile
from pathlib import Path

from stress_replay import REPO, run_case


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=REPO / "MQSim")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="mqsim-mapping-scale-") as temporary:
        for scheduler in ("OUT_OF_ORDER", "PRIORITY_OUT_OF_ORDER"):
            for name, options in (
                ("mapping-mixed", {}),
                ("mapping-burst-trim", {"pattern": "trim", "interval": 1000}),
                ("mapping-shared", {"flows": 4}),
            ):
                result = run_case(
                    Path(temporary), args.binary.resolve(), name, scheduler,
                    count=100000, working_set_pages=8192, cmt=256, **options,
                )
                assert result["status"] == "PASS", result["errors"]
                for field in ("mapping_reads", "mapping_programs", "gc_count"):
                    assert result[field] > 0, f"{name}: did not exercise {field}"
    print("6/6 multi-mapping-page regressions passed")


if __name__ == "__main__":
    main()
