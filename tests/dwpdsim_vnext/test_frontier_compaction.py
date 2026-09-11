#!/usr/bin/env python3
"""Public trace regressions for reclaiming idle, partially written frontiers.

Two flows share a 16-block plane. Flow 0 writes 104 pages, optionally
overwrites them, then trims its last eight pages before flow 1 writes 104.
e936f80 stalls; reclaiming/compacting idle frontiers must complete the trace.
"""
import argparse
import copy
import csv
import random
import subprocess
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FIXTURE = REPO / "tests/dwpdsim_vnext"


def run_case(binary, directory, scheduler, seed, interval):
    config = ET.parse(FIXTURE / "ssdconfig.xml")
    config.find(".//CMT_Capacity").text = "64"
    config.find(".//Transaction_Scheduling_Policy").text = scheduler
    for node in config.findall(".//Logical_Capacity_In_Sectors"):
        node.text = str(227 * 16)
    config.write(directory / "ssd.xml")
    workload = ET.parse(FIXTURE / "workload.xml")
    scenario = workload.find("IO_Scenario")
    template = copy.deepcopy(scenario[0])
    scenario.clear()
    rng = random.Random(seed)
    writes = list(range(104)) + [rng.randrange(104) for _ in range(seed * 20)]
    commands = [[(page, 0) for page in writes] + [(page, 2) for page in range(96, 104)],
                [(page, 0) for page in range(104, 208)]]
    request_id = 0
    for stream, items in enumerate(commands):
        flow = copy.deepcopy(template)
        trace = directory / f"{stream}.trace"
        flow.find("File_Path").text = str(trace)
        flow.find("Enable_Request_Completion_Log").text = "true"
        scenario.append(flow)
        with trace.open("w") as handle:
            for page, operation in items:
                handle.write(f"{request_id * interval} 0 {page * 16} 16 {operation} {request_id} {request_id - 1}\n")
                request_id += 1
    workload.write(directory / "workload.xml")
    result = subprocess.run([str(binary), "-i", str(directory / "ssd.xml"),
                             "-w", str(directory / "workload.xml")],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr
    assert "Simulation complete." in result.stdout
    root = ET.parse(directory / "workload_scenario_1.xml")
    flows = root.findall("Host/Host.IO_Flow")
    assert len(flows) == 2
    completions = {}
    for stream, flow in enumerate(flows):
        count = len(commands[stream])
        write_count, trim_count = (len(writes), 8) if stream == 0 else (104, 0)
        for tag, expected in {
            "Generated_Request_Count": count, "Completed_Request_Count": count,
            "Write_Request_Count": write_count, "Read_Request_Count": 0,
            "Trim_Request_Count": trim_count, "Bytes_Transferred_Write": write_count * 8192,
            "Bytes_Trimmed_Requested": trim_count * 8192,
        }.items():
            assert int(flow.findtext(tag)) == expected, (tag, flow.findtext(tag), expected)
        with (directory / f"workload.IO_Flow.No_{stream}.log.completion.csv").open() as handle:
            rows = list(csv.DictReader(handle))
        assert len(rows) == count
        for row in rows:
            rid = int(row["request_id"])
            assert rid not in completions
            completions[rid] = row
    assert set(completions) == set(range(request_id))
    for rid, row in completions.items():
        release, submit, finish = (int(row[key]) for key in
                                  ("dependency_release_time_ns", "submit_time_ns", "completion_time_ns"))
        assert release <= submit <= finish
        if rid:
            assert release >= int(completions[rid - 1]["completion_time_ns"])
    ftl = root.find(".//SSDDevice.FTL")
    assert int(ftl.get("GC_Execution_Count")) > 0
    assert int(ftl.get("Effective_Trimmed_Sector_Count")) == 8 * 16
    queues = list(root.find(".//SSDDevice.TSU"))
    for queue in queues:
        assert queue.get("No_Of_Transactions_Enqueued") == queue.get("No_Of_Transactions_Dequeued")
    mapping = sum(int(q.get("No_Of_Transactions_Enqueued")) for q in queues if "Mapping_Write_TR_Queue" in q.tag)
    assert int(ftl.get("Issued_Flash_Program_CMD")) == len(writes) + 104 + int(ftl.get("GC_Page_Program_Count")) + mapping
    expected_bytes = (len(writes) + 104) * 8192
    for path in (".//SSDDevice.Pool", ".//SSDDevice.Channel"):
        assert sum(int(node.get("Host_Write_Bytes")) for node in root.findall(path)) == expected_bytes
    print(f"PASS {scheduler}/seed={seed}/interval={interval}: {request_id} completions", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=REPO / "MQSim")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="mqsim-frontier-compaction-") as temporary:
        for scheduler in ("OUT_OF_ORDER", "PRIORITY_OUT_OF_ORDER"):
            for seed in (0, 1, 3):
                for interval in (0, 1000000):
                    directory = Path(temporary) / f"{scheduler}-{seed}-{interval}"
                    directory.mkdir()
                    run_case(args.binary.resolve(), directory, scheduler, seed, interval)


if __name__ == "__main__":
    main()
