#!/usr/bin/env python3
"""Shared data/mapping blocks must preserve each stream through GC and TRIM.

The six domains reuse the same MVPN numbers. Each stream overwrites its own
21 data pages with a finite CMT, reads them, then trims every page. Counts and
effective invalidations are calculated from the trace, not an MQSim snapshot.
MQSim does not model payload bytes; this checks mapping/lifecycle semantics.
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


def run_case(binary, directory, scheduler, sharing, overlap_lpas=False):
    config = ET.parse(FIXTURE / "ssdconfig.xml")
    for key, value in {"CMT_Capacity": 64, "CMT_Sharing_Mode": sharing,
                       "Transaction_Scheduling_Policy": scheduler}.items():
        config.find(".//" + key).text = str(value)
    for node in config.findall(".//Logical_Capacity_In_Sectors"):
        node.text = str(227 * 16)
    for node in config.findall(".//Block_PE_Cycles_Limit"):
        node.text = "1000000"
    config.write(directory / "ssd.xml")
    workload = ET.parse(FIXTURE / "workload.xml")
    scenario = workload.find("IO_Scenario")
    template = copy.deepcopy(scenario[0])
    scenario.clear()
    expected = []
    for stream in range(6):
        writes = 1000 + stream * 100
        rng = random.Random(321 + stream)
        commands = [(p if p < 21 else rng.randrange(21), 0) for p in range(writes)]
        commands += [(p, 1) for p in range(21)] + [(p, 2) for p in range(21)]
        expected.append(commands)
        flow = copy.deepcopy(template)
        trace = directory / f"{stream}.trace"
        flow.find("File_Path").text = str(trace)
        flow.find("Enable_Request_Completion_Log").text = "true"
        scenario.append(flow)
        with trace.open("w") as out:
            for index, (page, operation) in enumerate(commands):
                rid = stream * 10000 + index
                predecessor = rid - 1 if index else -1
                lpa = page if overlap_lpas else stream * 21 + page
                out.write(f"0 0 {lpa * 16} 16 {operation} {rid} {predecessor}\n")
    workload.write(directory / "workload.xml")
    result = subprocess.run([str(binary), "-i", str(directory / "ssd.xml"),
                             "-w", str(directory / "workload.xml")],
                            capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr
    assert "Simulation complete." in result.stdout
    root = ET.parse(directory / "workload_scenario_1.xml")
    flows = root.findall("Host/Host.IO_Flow")
    assert len(flows) == 6
    for stream, flow in enumerate(flows):
        writes = len(expected[stream]) - 42
        for tag, value in {
            "Generated_Request_Count": writes + 42, "Completed_Request_Count": writes + 42,
            "Write_Request_Count": writes, "Read_Request_Count": 21, "Trim_Request_Count": 21,
            "Bytes_Transferred_Write": writes * 8192, "Bytes_Transferred_Read": 21 * 8192,
            "Bytes_Trimmed_Requested": 21 * 8192, "Bytes_Trimmed": 21 * 8192,
        }.items():
            assert int(flow.findtext(tag)) == value, (stream, tag, flow.findtext(tag), value)
        with (directory / f"workload.IO_Flow.No_{stream}.log.completion.csv").open() as handle:
            rows = list(csv.DictReader(handle))
        assert len(rows) == writes + 42
        assert [int(row["request_id"]) for row in rows] == list(range(stream * 10000, stream * 10000 + writes + 42))
        previous_finish = 0
        for row in rows:
            release, submit, finish = (int(row[key]) for key in
                                      ("dependency_release_time_ns", "submit_time_ns", "completion_time_ns"))
            assert previous_finish <= release <= submit <= finish
            previous_finish = finish
    ftl = root.find(".//SSDDevice.FTL")
    assert int(ftl.get("Effective_Trimmed_Sector_Count")) == 6 * 21 * 16
    assert int(ftl.get("Pages_Invalidated_By_Trim")) == 6 * 21
    assert int(ftl.get("GC_Page_Program_Count")) > 0
    queues = list(root.find(".//SSDDevice.TSU"))
    for queue in queues:
        assert queue.get("No_Of_Transactions_Enqueued") == queue.get("No_Of_Transactions_Dequeued")
    mapping = sum(int(q.get("No_Of_Transactions_Enqueued")) for q in queues if "Mapping_Write_TR_Queue" in q.tag)
    assert mapping > 0
    writes = sum(len(commands) - 42 for commands in expected)
    assert int(ftl.get("Issued_Flash_Program_CMD")) == writes + mapping + int(ftl.get("GC_Page_Program_Count"))
    print(f"PASS {scheduler}/{sharing}/overlap_lpas={overlap_lpas}: 6 domains, {writes} writes, 126 reads, 126 effective page trims", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=REPO / "MQSim")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="mqsim-mapping-ownership-") as temporary:
        for scheduler in ("OUT_OF_ORDER", "PRIORITY_OUT_OF_ORDER"):
            for sharing in ("SHARED", "EQUAL_PARTITIONING"):
                for overlap_lpas in (False, True):
                    directory = Path(temporary) / f"{scheduler}-{sharing}-{overlap_lpas}"
                    directory.mkdir()
                    run_case(args.binary.resolve(), directory, scheduler, sharing, overlap_lpas)


if __name__ == "__main__":
    main()
