#!/usr/bin/env python3
"""Replay mapping-cache eviction workloads through both TSU schedulers."""

import os
from pathlib import Path
import subprocess
import tempfile
import xml.etree.ElementTree as ET


REPO = Path(__file__).resolve().parents[2]
FIXTURE = REPO / "tests" / "dwpdsim_vnext"
BINARY = Path(os.environ.get("MQSIM_BINARY", REPO / "MQSim")).resolve()
REQUEST_COUNT = 4096


def run_case(directory, scheduler, gc_pressure):
    config = ET.parse(FIXTURE / "ssdconfig.xml")
    config.find(".//Transaction_Scheduling_Policy").text = scheduler
    # The fixture's 3/7 erase limits are for wear-limit smoke tests.
    for limit in config.findall(".//Block_PE_Cycles_Limit"):
        limit.text = "100000"
    if gc_pressure:
        config.find(".//CMT_Capacity").text = "64"
        working_set_pages = 128
    else:
        # Keep the normal CMT capacity and exceed it with distinct pages.
        config.find(".//Block_No_Per_Plane").text = "512"
        for capacity in config.findall(".//Logical_Capacity_In_Sectors"):
            capacity.text = "65536"
        working_set_pages = REQUEST_COUNT
    config.write(directory / "ssd.xml")

    trace = directory / "write.trace"
    trace.write_text("".join(
        f"{index * 1000000} 0 {(index % working_set_pages) * 16} 16 0 {index} -1\n"
        for index in range(REQUEST_COUNT)
    ))
    workload = ET.parse(FIXTURE / "workload.xml")
    scenario = workload.find("IO_Scenario")
    for flow in list(scenario)[1:]:
        scenario.remove(flow)
    scenario[0].find("File_Path").text = str(trace)
    scenario[0].find("Enable_Request_Completion_Log").text = "false"
    workload.write(directory / "workload.xml")

    completed = subprocess.run(
        [str(BINARY), "-i", str(directory / "ssd.xml"),
         "-w", str(directory / "workload.xml")],
        cwd=REPO, text=True, capture_output=True, timeout=30,
    )
    assert completed.returncode == 0, completed.stdout + completed.stderr
    result = ET.parse(directory / "workload_scenario_1.xml")
    flow = result.find("Host/Host.IO_Flow")
    assert int(flow.findtext("Generated_Request_Count")) == REQUEST_COUNT
    assert int(flow.findtext("Completed_Request_Count")) == REQUEST_COUNT
    assert int(flow.findtext("Bytes_Transferred_Write")) == REQUEST_COUNT * 8192

    queues = result.findall(".//SSDDevice.TSU.Mapping_Write_TR_Queue")
    enqueued = sum(int(queue.get("No_Of_Transactions_Enqueued")) for queue in queues)
    assert enqueued > 1, "workload must exercise repeated mapping writebacks"
    for queue in result.find(".//SSDDevice.TSU"):
        assert queue.get("No_Of_Transactions_Enqueued") == queue.get("No_Of_Transactions_Dequeued"), queue.attrib
    ftl = result.find(".//SSDDevice.FTL")
    assert int(ftl.get("Issued_Flash_Read_CMD_For_Mapping")) > 0
    if gc_pressure:
        assert int(ftl.get("GC_Execution_Count")) > 0
    print(f"{scheduler}, gc_pressure={gc_pressure}: {REQUEST_COUNT} requests completed, "
          f"{enqueued} mapping writebacks drained")


def main():
    with tempfile.TemporaryDirectory(prefix="mqsim-tsu-drain-") as temporary:
        for scheduler in ("OUT_OF_ORDER", "PRIORITY_OUT_OF_ORDER"):
            for gc_pressure in (False, True):
                directory = Path(temporary) / f"{scheduler}-{gc_pressure}"
                directory.mkdir()
                run_case(directory, scheduler, gc_pressure)


if __name__ == "__main__":
    main()
