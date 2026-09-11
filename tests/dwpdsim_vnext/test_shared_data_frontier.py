#!/usr/bin/env python3
"""No-TRIM regressions for shared-plane data frontier fragmentation.

All cases use 16 blocks x 16 pages, CMT=64 and the same 227-page namespace.
Vary live pages as well as write count: a large request count over a small
working set alone does not expose the per-stream partial-block stall.
"""
import argparse
import sys
import tempfile
from pathlib import Path


def main():
    sys.dont_write_bytecode = True
    from test_overfull_gc import CASES, REPO, run_case

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=REPO / "MQSim")
    parser.add_argument("--count", type=int, default=20000)
    parser.add_argument("--seed", type=int, default=321)
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()
    if args.count < 112:
        parser.error("--count must be >= 112 to initialize every working set")
    selected = {"shared-data-minimal": (69, 256, 0, 3, 1)}
    for flows, sizes in ((2, (104, 108, 112)), (3, (64, 69, 74)),
                         (4, (48, 52, 56)), (6, (32, 34, 36)), (8, (24, 26, 28))):
        for pages in sizes:
            selected[f"shared-data-{flows}-{pages}"] = (pages, args.count, 0, flows, 1)
    CASES.update(selected)
    with tempfile.TemporaryDirectory(prefix="mqsim-shared-data-") as temporary:
        for scheduler in ("OUT_OF_ORDER", "PRIORITY_OUT_OF_ORDER"):
            for case in selected:
                directory = Path(temporary) / f"{scheduler}-{case}"
                directory.mkdir()
                run_case(args.binary.resolve(), directory, scheduler, case, args.seed, args.timeout)
    print(f"{2 * len(selected)} shared-data frontier checks passed", flush=True)


if __name__ == "__main__":
    main()
