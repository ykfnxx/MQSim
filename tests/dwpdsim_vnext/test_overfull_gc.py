#!/usr/bin/env python3
"""Tight-capacity GC/frontier and mapping-writeback liveness regressions.

The original 208-page, 5000-write burst fails at 4b2a98b with four AMU
overfull waiters. The 512-write prefix and spaced 192-page workload expose
mapping allocation from an empty pool. Expected host results are derived
from the input, not recorded from the simulator. No private trace is needed.
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
CASES = {
    "gc-frontier-burst": (208, 5000, 0, 1, 1),
    "mapping-prefix": (208, 512, 0, 1, 1),
    "mapping-spaced": (192, 5000, 1000000, 1, 1),
    "six-channel": (192, 5000, 0, 2, 3),
}


def run_case(binary, directory, scheduler, case):
    pages, count, interval, flows, channels_per_pool = CASES[case]
    pages *= channels_per_pool
    count *= channels_per_pool
    config = ET.parse(FIXTURE / "ssdconfig.xml")
    for key, value in {
        "Transaction_Scheduling_Policy": scheduler,
        "CMT_Capacity": 64,
        "Flash_Channel_Count": channels_per_pool * 2,
        "Measurement_End_Time_Ns": 10**15,
    }.items():
        config.find(".//" + key).text = str(value)
    for index, pool in enumerate(config.findall(".//Flash_Pool_Parameter_Set")):
        pool.find("Channel_IDs").text = ",".join(
            str(index * channels_per_pool + channel) for channel in range(channels_per_pool)
        )
        pool.find("Logical_Capacity_In_Sectors").text = str(227 * 16 * channels_per_pool)
    for limit in config.findall(".//Block_PE_Cycles_Limit"):
        limit.text = "1000000"
    config.write(directory / "ssd.xml")

    workload = ET.parse(FIXTURE / "workload.xml")
    scenario = workload.find("IO_Scenario")
    template = copy.deepcopy(scenario[0])
    scenario.clear()
    predecessors = {}
    for stream in range(flows):
        flow = copy.deepcopy(template)
        flow.find("Pool_ID").text = ("slc", "tlc")[stream]
        trace = directory / f"{stream}.trace"
        flow.find("File_Path").text = str(trace)
        flow.find("Enable_Request_Completion_Log").text = "true"
        scenario.append(flow)
        rng = random.Random(321 + stream)
        previous = [-1] * pages
        with trace.open("w") as handle:
            for index in range(count):
                page = index if index < pages else rng.randrange(pages)
                request_id = stream * count + index
                predecessors[request_id] = previous[page]
                handle.write(f"{index * interval} 0 {page * 16} 16 0 {request_id} {previous[page]}\n")
                previous[page] = request_id
    workload.write(directory / "workload.xml")
    result = subprocess.run(
        [str(binary), "-i", str(directory / "ssd.xml"), "-w", str(directory / "workload.xml")],
        cwd=REPO, capture_output=True, text=True, timeout=60,
    )
    assert result.returncode == 0, f"{scheduler}/{case}: {result.stderr}"
    assert "Simulation complete." in result.stdout
    root = ET.parse(directory / "workload_scenario_1.xml")
    reported = root.findall("Host/Host.IO_Flow")
    assert len(reported) == flows
    for flow in reported:
        for tag, expected in {
            "Generated_Request_Count": count, "Completed_Request_Count": count,
            "Write_Request_Count": count, "Read_Request_Count": 0, "Trim_Request_Count": 0,
            "Bytes_Transferred_Write": count * 8192, "Measurement_Host_Write_Bytes": count * 8192,
        }.items():
            assert int(flow.findtext(tag)) == expected, (case, tag, flow.findtext(tag), expected)

    completions = {}
    for stream in range(flows):
        with (directory / f"workload.IO_Flow.No_{stream}.log.completion.csv").open() as handle:
            rows = list(csv.DictReader(handle))
        assert len(rows) == count
        for row in rows:
            request_id = int(row["request_id"])
            assert request_id not in completions
            completions[request_id] = row
    assert set(completions) == set(predecessors)
    for request_id, row in completions.items():
        release, submit, finish = (int(row[key]) for key in
                                  ("dependency_release_time_ns", "submit_time_ns", "completion_time_ns"))
        assert release <= submit <= finish
        previous = predecessors[request_id]
        if previous != -1:
            assert release >= int(completions[previous]["completion_time_ns"])

    ftl = root.find(".//SSDDevice.FTL")
    gc_programs = int(ftl.get("GC_Page_Program_Count"))
    mapping_programs = int(ftl.get("Issued_Flash_Program_CMD_For_Mapping"))
    assert int(ftl.get("GC_Execution_Count")) > 0
    assert gc_programs > 0 and mapping_programs > 0
    queues = list(root.find(".//SSDDevice.TSU"))
    # Legacy mapping counters include writebacks merged into a GC mapping
    # relocation at barrier release. Only TSU submissions are separate NAND I/O.
    physical_mapping_programs = sum(int(queue.get("No_Of_Transactions_Enqueued"))
                                    for queue in queues if "Mapping_Write_TR_Queue" in queue.tag)
    assert 0 < physical_mapping_programs <= mapping_programs
    assert int(ftl.get("Issued_Flash_Program_CMD")) == count * flows + gc_programs + physical_mapping_programs
    for queue in queues:
        assert queue.get("No_Of_Transactions_Enqueued") == queue.get("No_Of_Transactions_Dequeued"), queue.attrib
    pools = root.findall(".//SSDDevice.Pool")
    channels = root.findall(".//SSDDevice.Channel")
    for field in ("Host_Write_Bytes", "Measurement_Host_Write_Bytes"):
        assert sum(int(pool.get(field)) for pool in pools) == count * flows * 8192
    assert sum(int(channel.get("Host_Write_Bytes")) for channel in channels) == count * flows * 8192
    print(f"PASS {scheduler}/{case}: {count * flows} completions, {gc_programs} GC programs, {mapping_programs} mapping programs")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=REPO / "MQSim")
    parser.add_argument("--case", choices=CASES)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="mqsim-overfull-gc-") as temporary:
        for scheduler in ("OUT_OF_ORDER", "PRIORITY_OUT_OF_ORDER"):
            for case in ([args.case] if args.case else CASES):
                directory = Path(temporary) / f"{scheduler}-{case}"
                directory.mkdir()
                run_case(args.binary.resolve(), directory, scheduler, case)


if __name__ == "__main__":
    main()
