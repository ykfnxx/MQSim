#!/usr/bin/env python3
"""Writes must use an existing data frontier even at the free-block reserve.

One SLC plane has 16 blocks x 16 pages. With a small mapping cache, the
sequential fill reaches the one-free-block reserve while its data frontier
still has room. There are no invalid data pages to reclaim. Rejecting writes
at that point leaves AMU requests waiting for an erase that cannot occur.
"""

import argparse
import subprocess
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FIXTURE = REPO / "tests/dwpdsim_vnext"


def run_case(binary, directory, scheduler, count):
    config = ET.parse(FIXTURE / "ssdconfig.xml")
    for key, value in {
        "Transaction_Scheduling_Policy": scheduler,
        "CMT_Capacity": 64,
        "Measurement_End_Time_Ns": 10**15,
    }.items():
        config.find(".//" + key).text = str(value)
    for capacity in config.findall(".//Logical_Capacity_In_Sectors"):
        capacity.text = str(227 * 16)  # Within the configured 10% OP limit.
    for limit in config.findall(".//Block_PE_Cycles_Limit"):
        limit.text = "1000000"
    config.write(directory / "ssd.xml")
    trace = directory / "io.trace"
    trace.write_text("".join(
        f"{i * 1000} 0 {i * 16} 16 0 {i} -1\n" for i in range(count)
    ))
    workload = ET.parse(FIXTURE / "workload.xml")
    scenario = workload.find("IO_Scenario")
    for flow in list(scenario)[1:]:
        scenario.remove(flow)
    scenario[0].find("File_Path").text = str(trace)
    scenario[0].find("Enable_Request_Completion_Log").text = "false"
    workload.write(directory / "workload.xml")
    result = subprocess.run(
        [str(binary), "-i", str(directory / "ssd.xml"),
         "-w", str(directory / "workload.xml")],
        cwd=REPO, capture_output=True, text=True, timeout=30,
    )
    assert result.returncode == 0, f"{scheduler}, count={count}: {result.stderr}"
    root = ET.parse(directory / "workload_scenario_1.xml")
    flow = root.find("Host/Host.IO_Flow")
    assert int(flow.findtext("Generated_Request_Count")) == count
    assert int(flow.findtext("Completed_Request_Count")) == count
    assert int(flow.findtext("Write_Request_Count")) == count
    assert int(flow.findtext("Bytes_Transferred_Write")) == count * 8192
    ftl = root.find(".//SSDDevice.FTL")
    assert int(ftl.get("Issued_Flash_Program_CMD_For_Mapping")) > 0
    for queue in root.find(".//SSDDevice.TSU"):
        assert queue.get("No_Of_Transactions_Enqueued") == queue.get("No_Of_Transactions_Dequeued"), queue.attrib
    print(f"PASS {scheduler}: {count}/{count} sequential writes drained")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=REPO / "MQSim")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="mqsim-frontier-drain-") as temporary:
        for scheduler in ("OUT_OF_ORDER", "PRIORITY_OUT_OF_ORDER"):
            # 210 is the first blocked request in the original reproducer;
            # 224 consumes the remaining data frontier without a new block.
            for count in (210, 224):
                directory = Path(temporary) / f"{scheduler}-{count}"
                directory.mkdir()
                run_case(args.binary.resolve(), directory, scheduler, count)


if __name__ == "__main__":
    main()
