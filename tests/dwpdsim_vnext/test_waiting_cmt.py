#!/usr/bin/env python3
"""A mapping completion must wake requests even if their CMT slot was evicted.

Six/eight flows over two pools share a 64-byte CMT and issue burst I/O to
multiple mapping pages. Seed 321 with six flows reproduced orphaned writes
on streams 3 and 5 before the fix. Host/byte/TRIM expectations come from the
trace generator, not from a snapshot of the failing simulator.
"""

import argparse
import tempfile
from pathlib import Path

from stress_replay import REPO, run_case


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=REPO / "MQSim")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="mqsim-waiting-cmt-") as temporary:
        for scheduler in ("OUT_OF_ORDER", "PRIORITY_OUT_OF_ORDER"):
            for flows in (6, 8):
                result = run_case(
                    Path(temporary), args.binary.resolve(), f"flows{flows}", scheduler,
                    count=100000, flows=flows, working_set_pages=8192,
                    cmt=64, interval=1000, seed=321,
                )
                assert result["status"] == "PASS", result["errors"]
                assert result["mapping_reads"] > 0
                assert result["mapping_programs"] > 0
    print("4/4 evicted-WAITING-slot regressions passed")


if __name__ == "__main__":
    main()
