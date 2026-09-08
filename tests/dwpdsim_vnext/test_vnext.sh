#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
binary="${MQSIM_BINARY:-${repo_root}/MQSim}"

cd "${repo_root}"
rm -f tests/dwpdsim_vnext/workload_scenario_1.xml tests/dwpdsim_vnext/*.completion.csv
"${binary}" -i tests/dwpdsim_vnext/ssdconfig.xml -w tests/dwpdsim_vnext/workload.xml >/dev/null

python3 - "${binary}" <<'PY'
import csv
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import xml.etree.ElementTree as ET

repo = Path.cwd()
binary = Path(sys.argv[1]).resolve()
fixture = repo / "tests/dwpdsim_vnext"


def completion_rows(flow_id):
    path = fixture / f"workload.IO_Flow.No_{flow_id}.log.completion.csv"
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


rows = completion_rows(0) + completion_rows(1) + completion_rows(2)
by_id = {int(row["request_id"]): row for row in rows}
assert sorted(by_id) == list(range(7))
assert completion_rows(1) == []

completion = {request_id: int(row["completion_time_ns"]) for request_id, row in by_id.items()}
release = {request_id: int(row["dependency_release_time_ns"]) for request_id, row in by_id.items()}
submit = {request_id: int(row["submit_time_ns"]) for request_id, row in by_id.items()}
assert release[2] >= completion[0]
assert release[3] >= completion[1]
assert release[4] >= max(completion[2], completion[3])
assert release[5] >= completion[4]
assert release[6] >= completion[5]
assert all(submit[request_id] >= release[request_id] for request_id in by_id)
assert completion[1] > completion[0]

result = ET.parse(fixture / "workload_scenario_1.xml").getroot()
flows = {int(flow.findtext("Flow_ID")): flow for flow in result.findall("Host/Host.IO_Flow")}
assert sorted(flows) == [0, 1, 2]
assert [flows[index].findtext("Pool_ID") for index in range(3)] == ["slc", "slc", "tlc"]
for index, expected in enumerate((4, 0, 3)):
    flow = flows[index]
    assert int(flow.findtext("Generated_Request_Count")) == expected
    assert int(flow.findtext("Completed_Request_Count")) == expected
    for field in ("Bytes_Transferred", "Bytes_Transferred_Read", "Bytes_Transferred_Write",
                  "Bytes_Trimmed", "Bytes_Trimmed_Requested", "Measurement_Host_Write_Bytes"):
        int(flow.findtext(field))
    for field in ("IOPS", "IOPS_Read", "IOPS_Write", "IOPS_Trim", "Bandwidth"):
        assert math.isfinite(float(flow.findtext(field)))

configuration = result.find("SSDDevice/SSDDevice.Configuration")
assert configuration is not None
assert configuration.attrib["Statistics_ABI_Version"] == "1"
assert configuration.attrib["Time_Unit"] == "nanosecond"
assert configuration.attrib["Configuration_Hash_Algorithm"] == "fnv1a64-raw-xml"
hash_value = 14695981039346656037
for byte in (fixture / "ssdconfig.xml").read_bytes():
    hash_value ^= byte
    hash_value = (hash_value * 1099511628211) & ((1 << 64) - 1)
assert int(configuration.attrib["Configuration_Hash"]) == hash_value

ftl = result.find("SSDDevice/SSDDevice.FTL")
assert ftl is not None
for field in ("Received_Trim_Command_Count", "Requested_Trim_Sector_Count",
              "Effective_Trimmed_Sector_Count", "Pages_Invalidated_By_Trim",
              "GC_Execution_Count", "GC_Page_Read_Count", "GC_Page_Program_Count"):
    int(ftl.attrib[field])

pools = {pool.attrib["ID"]: pool for pool in result.findall("SSDDevice/SSDDevice.Pool")}
assert sorted(pools) == ["slc", "tlc"]
assert pools["slc"].attrib["Channel_IDs"] == "0"
assert pools["tlc"].attrib["Channel_IDs"] == "1"
assert int(pools["slc"].attrib["PE_Cycle_Limit"]) == 3
assert int(pools["tlc"].attrib["PE_Cycle_Limit"]) == 7
for pool in pools.values():
    for field in ("Host_Read_Bytes", "Host_Write_Bytes", "Requested_Trim_Bytes",
                  "Effective_Trimmed_Bytes", "Requested_Trim_Sector_Count",
                  "Effective_Trimmed_Sector_Count", "Flash_Read_Command_Count",
                  "Flash_Program_Command_Count", "Flash_Erase_Command_Count",
                  "Measurement_Host_Write_Bytes", "Measurement_Flash_Programmed_Bytes",
                  "Logical_Capacity_Bytes", "Physical_Capacity_Bytes"):
        assert int(pool.attrib[field]) >= 0
    assert int(pool.attrib["Measurement_Host_Write_Bytes"]) > 0
    assert int(pool.attrib["Measurement_Flash_Programmed_Bytes"]) > 0

for ftl_field, pool_field in (
        ("Received_Trim_Command_Count", "Received_Trim_Command_Count"),
        ("Requested_Trim_Sector_Count", "Requested_Trim_Sector_Count"),
        ("Effective_Trimmed_Sector_Count", "Effective_Trimmed_Sector_Count"),
        ("Pages_Invalidated_By_Trim", "Pages_Invalidated_By_Trim"),
        ("GC_Execution_Count", "GC_Execution_Count"),
        ("GC_Page_Read_Count", "GC_Page_Read_Count"),
        ("GC_Page_Program_Count", "GC_Page_Program_Count")):
    assert int(ftl.attrib[ftl_field]) == sum(int(pool.attrib[pool_field]) for pool in pools.values())

channels = {int(channel.attrib["ID"]): channel for channel in result.findall("SSDDevice/SSDDevice.Channel")}
assert channels[0].attrib["Pool_ID"] == "slc"
assert channels[1].attrib["Pool_ID"] == "tlc"
assert int(channels[0].attrib["PE_Cycle_Limit"]) == 3
assert int(channels[1].attrib["PE_Cycle_Limit"]) == 7
for channel in channels.values():
    for field in ("Host_Read_Bytes", "Host_Write_Bytes", "Requested_Trim_Bytes",
                  "Effective_Trimmed_Bytes", "Flash_Read_Command_Count",
                  "Flash_Program_Command_Count", "Flash_Erase_Command_Count",
                  "Measurement_Flash_Programmed_Bytes", "Logical_Capacity_Bytes",
                  "Physical_Capacity_Bytes"):
        assert int(channel.attrib[field]) >= 0


def workload_xml(trace_paths, pools_for_flows):
    flows_xml = []
    for trace_path, pool_id in zip(trace_paths, pools_for_flows):
        flows_xml.append(f"""
        <IO_Flow_Parameter_Set_Trace_Based>
            <Priority_Class>HIGH</Priority_Class>
            <Device_Level_Data_Caching_Mode>TURNED_OFF</Device_Level_Data_Caching_Mode>
            <Pool_ID>{pool_id}</Pool_ID>
            <Initial_Occupancy_Percentage>0</Initial_Occupancy_Percentage>
            <File_Path>{trace_path}</File_Path>
            <Percentage_To_Be_Executed>100</Percentage_To_Be_Executed>
            <Relay_Count>1</Relay_Count>
            <Time_Unit>NANOSECOND</Time_Unit>
            <Trace_Format>DWPDSIM_DEPENDENCY_V1</Trace_Format>
            <Enable_Request_Completion_Log>false</Enable_Request_Completion_Log>
        </IO_Flow_Parameter_Set_Trace_Based>""")
    return "<?xml version=\"1.0\"?>\n<MQSim_IO_Scenarios><IO_Scenario>" + "".join(flows_xml) + "</IO_Scenario></MQSim_IO_Scenarios>\n"


def run_case(directory, trace_contents, expected_error=None, large_namespace=False, flow_count=1):
    traces = []
    for index in range(flow_count):
        path = directory / f"flow_{index}.trace"
        path.write_text(trace_contents if index == 0 else "")
        traces.append(path)
    workload = directory / "workload.xml"
    pools_for_flows = ["slc" if index < max(1, flow_count - 1) else "tlc" for index in range(flow_count)]
    workload.write_text(workload_xml(traces, pools_for_flows))
    config = directory / "ssdconfig.xml"
    config_text = (fixture / "ssdconfig.xml").read_text()
    if large_namespace:
        config_text = config_text.replace("<Block_No_Per_Plane>16</Block_No_Per_Plane>",
                                          "<Block_No_Per_Plane>512</Block_No_Per_Plane>")
        config_text = config_text.replace("<Logical_Capacity_In_Sectors>2048</Logical_Capacity_In_Sectors>",
                                          "<Logical_Capacity_In_Sectors>65536</Logical_Capacity_In_Sectors>")
    config.write_text(config_text)
    completed = subprocess.run([str(binary), "-i", str(config), "-w", str(workload)],
                               cwd=repo, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if expected_error is None:
        assert completed.returncode == 0, completed.stdout + completed.stderr
        return directory / "workload_scenario_1.xml"
    assert completed.returncode != 0
    assert expected_error in completed.stderr, completed.stdout + completed.stderr


with tempfile.TemporaryDirectory(prefix="mqsim-vnext-") as temporary:
    temporary = Path(temporary)
    invalid_cases = {
        "duplicate": ("0 0 0 1 0 0 -1\n1 0 1 1 0 0 -1\n", "Duplicate DWPDSim request id"),
        "missing": ("0 0 0 1 0 0 9\n", "Missing predecessor 9"),
        "self": ("0 0 0 1 0 0 0\n", "depends on itself"),
        "cycle": ("0 0 0 1 0 0 1\n1 0 1 1 0 1 0\n", "Cycle in DWPDSim dependency graph"),
        "device": ("0 1 0 1 0 0 -1\n", "device_id must be 0"),
        "too_large": ("0 0 0 65536 2 0 -1\n", "invalid sector count"),
    }
    for name, (trace, message) in invalid_cases.items():
        case_dir = temporary / name
        case_dir.mkdir()
        run_case(case_dir, trace, message, large_namespace=name == "too_large")

    boundary_dir = temporary / "boundary"
    boundary_dir.mkdir()
    boundary_result = run_case(boundary_dir, "0 0 0 65535 2 0 -1\n", large_namespace=True)
    boundary_flow = ET.parse(boundary_result).getroot().find("Host/Host.IO_Flow")
    assert int(boundary_flow.findtext("Bytes_Trimmed_Requested")) == 65535 * 512

    all_empty_dir = temporary / "all_empty"
    all_empty_dir.mkdir()
    all_empty_result = run_case(all_empty_dir, "", flow_count=3)
    for flow in ET.parse(all_empty_result).getroot().findall("Host/Host.IO_Flow"):
        assert int(flow.findtext("Generated_Request_Count")) == 0
        assert int(flow.findtext("Completed_Request_Count")) == 0
        assert math.isfinite(float(flow.findtext("IOPS")))

    too_many_dir = temporary / "too_many_flows"
    too_many_dir.mkdir()
    run_case(too_many_dir, "", "at most 8 I/O flows", flow_count=9)

    source = temporary / "nvme_lba_roundtrip.cpp"
    source.write_text(textwrap.dedent("""
        #include <cassert>
        #include <cstdint>
        #include "ssd/Host_Interface_Defs.h"
        int main() {
            const uint64_t values[] = {0xffffffffULL, 0x100000000ULL, 0x123456789abcdef0ULL};
            for (uint64_t value : values) {
                Submission_Queue_Entry entry = {};
                Set_NVMe_LBA(entry, value);
                assert(Get_NVMe_LBA(entry) == value);
            }
        }
    """))
    executable = temporary / "nvme_lba_roundtrip"
    subprocess.run(["g++", "-std=c++11", "-Isrc", str(source), "-o", str(executable)], cwd=repo, check=True)
    subprocess.run([str(executable)], check=True)

print("DWPDSim vNext dependency, pool, statistics, validation, empty-flow, and NVMe LBA tests passed")
PY

MQSIM_BINARY="${binary}" python3 tests/dwpdsim_vnext/test_tsu_drain.py
python3 tests/dwpdsim_vnext/test_golden.py --binary "${binary}"
python3 tests/dwpdsim_vnext/test_mapping_scale.py --binary "${binary}"
python3 tests/dwpdsim_vnext/test_frontier_drain.py --binary "${binary}"
python3 tests/dwpdsim_vnext/test_waiting_cmt.py --binary "${binary}"
python3 tests/dwpdsim_vnext/stress_replay.py --binary "${binary}" --count 1000 \
  --output build/vnext-stress-regression
