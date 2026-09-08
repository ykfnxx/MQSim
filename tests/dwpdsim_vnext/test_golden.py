#!/usr/bin/env python3
"""Canonical golden regressions for MQSim's DWPDSim-specific behavior."""

import argparse
import copy
import csv
import difflib
import json
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
FIXTURE = REPO / "tests/dwpdsim_vnext"
EXPECTED_PATH = FIXTURE / "golden_expected.json"

FLOW_FIELDS = (
    "Flow_ID",
    "Pool_ID",
    "Generated_Request_Count",
    "Completed_Request_Count",
    "Read_Request_Count",
    "Write_Request_Count",
    "Trim_Request_Count",
    "Bytes_Transferred_Read",
    "Bytes_Transferred_Write",
    "Bytes_Trimmed_Requested",
    "Measurement_Host_Write_Bytes",
)

FTL_FIELDS = (
    "Issued_Flash_Read_CMD",
    "Issued_Flash_Program_CMD",
    "Issued_Flash_Erase_CMD",
    "Issued_Flash_Read_CMD_For_Mapping",
    "Issued_Flash_Program_CMD_For_Mapping",
    "CMT_Hits",
    "CMT_Misses",
    "Total_CMT_Queries",
    "Received_Trim_Command_Count",
    "Requested_Trim_Sector_Count",
    "Effective_Trimmed_Sector_Count",
    "Pages_Invalidated_By_Trim",
    "GC_Execution_Count",
    "GC_Page_Read_Count",
    "GC_Page_Program_Count",
    "Total_WL_Executions",
)

POOL_FIELDS = (
    "ID",
    "Media_Profile_ID",
    "Channel_IDs",
    "Host_Read_Bytes",
    "Host_Write_Bytes",
    "Requested_Trim_Bytes",
    "Effective_Trimmed_Bytes",
    "Received_Trim_Command_Count",
    "Requested_Trim_Sector_Count",
    "Effective_Trimmed_Sector_Count",
    "Pages_Invalidated_By_Trim",
    "Flash_Read_Command_Count",
    "Flash_Program_Command_Count",
    "Flash_Erase_Command_Count",
    "GC_Execution_Count",
    "GC_Page_Read_Count",
    "GC_Page_Program_Count",
    "Total_Block_Erase_Count",
    "Max_Block_Erase_Count",
    "Measurement_Total_Block_Erase_Count",
    "Measurement_Max_Block_Erase_Count",
    "Measurement_Host_Write_Bytes",
    "Measurement_Flash_Programmed_Bytes",
    "Logical_Capacity_Bytes",
    "Physical_Capacity_Bytes",
    "PE_Cycle_Limit",
)

CHANNEL_FIELDS = (
    "ID",
    "Pool_ID",
    "Media_Profile_ID",
    "Host_Read_Bytes",
    "Host_Write_Bytes",
    "Requested_Trim_Bytes",
    "Effective_Trimmed_Bytes",
    "Requested_Trim_Sector_Count",
    "Effective_Trimmed_Sector_Count",
    "Flash_Read_Command_Count",
    "Flash_Program_Command_Count",
    "Flash_Erase_Command_Count",
    "Total_Block_Erase_Count",
    "Max_Block_Erase_Count",
    "Measurement_Total_Block_Erase_Count",
    "Measurement_Max_Block_Erase_Count",
    "Measurement_Flash_Programmed_Bytes",
    "Logical_Capacity_Bytes",
    "Physical_Capacity_Bytes",
    "PE_Cycle_Limit",
)

CONFIG_FIELDS = (
    "Simulator_Version",
    "Statistics_ABI_Version",
    "Configuration_Hash",
    "Configuration_Hash_Algorithm",
    "Time_Unit",
    "Measurement_Start_Time_Ns",
    "Measurement_End_Time_Ns",
)


def scalar(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return value


def fields_from_elements(elements, fields, key):
    records = []
    for element in elements:
        record = {}
        for field in fields:
            value = element.get(field)
            if value is None:
                value = element.findtext(field)
            if value is None:
                raise AssertionError(f"missing golden field {field}")
            record[field] = scalar(value)
        records.append(record)
    return sorted(records, key=lambda record: scalar(record[key]))


def run_mqsim(binary, directory, config, workload):
    completed = subprocess.run(
        [str(binary), "-i", str(config), "-w", str(workload)],
        cwd=REPO,
        text=True,
        capture_output=True,
        timeout=60,
        check=False,
    )
    if completed.returncode != 0:
        raise AssertionError(completed.stdout + completed.stderr)
    result = directory / f"{workload.stem}_scenario_1.xml"
    return ET.parse(result).getroot()


def workload_with_flows(trace_paths, pool_ids, completion_log=False):
    workload = ET.parse(FIXTURE / "workload.xml")
    scenario = workload.find("IO_Scenario")
    template = copy.deepcopy(scenario[0])
    for flow in list(scenario):
        scenario.remove(flow)
    for trace_path, pool_id in zip(trace_paths, pool_ids):
        flow = copy.deepcopy(template)
        flow.find("File_Path").text = str(trace_path)
        flow.find("Pool_ID").text = pool_id
        flow.find("Enable_Request_Completion_Log").text = str(completion_log).lower()
        scenario.append(flow)
    return workload


def canonical_result(root, directory, completion_flow_count=0):
    configuration = root.find("SSDDevice/SSDDevice.Configuration")
    ftl = root.find("SSDDevice/SSDDevice.FTL")
    if configuration is None or ftl is None:
        raise AssertionError("missing configuration or FTL result")
    result = {
        "configuration": {
            field: (
                configuration.get(field)
                if field == "Configuration_Hash"
                else scalar(configuration.get(field))
            )
            for field in CONFIG_FIELDS
        },
        "flows": fields_from_elements(
            root.findall("Host/Host.IO_Flow"), FLOW_FIELDS, "Flow_ID"
        ),
        "ftl": {field: scalar(ftl.get(field)) for field in FTL_FIELDS},
        "pools": fields_from_elements(
            root.findall("SSDDevice/SSDDevice.Pool"), POOL_FIELDS, "ID"
        ),
        "channels": fields_from_elements(
            root.findall("SSDDevice/SSDDevice.Channel"), CHANNEL_FIELDS, "ID"
        ),
    }
    if completion_flow_count:
        rows = []
        for flow_id in range(completion_flow_count):
            path = directory / f"workload.IO_Flow.No_{flow_id}.log.completion.csv"
            with path.open(newline="") as handle:
                rows.extend(csv.DictReader(handle))
        rows_by_id = {int(row["request_id"]): row for row in rows}
        predecessors = {0: (), 1: (), 2: (0,), 3: (1,), 4: (2, 3), 5: (4,), 6: (5,)}
        result["completions"] = []
        for request_id in sorted(rows_by_id):
            row = rows_by_id[request_id]
            arrival = int(row["arrival_time_ns"])
            release = int(row["dependency_release_time_ns"])
            predecessor_completion = max(
                (int(rows_by_id[item]["completion_time_ns"]) for item in predecessors[request_id]),
                default=arrival,
            )
            result["completions"].append(
                {
                    "request_id": request_id,
                    "flow_id": int(row["flow_id"]),
                    "operation": row["operation"],
                    "arrival_time_ns": arrival,
                    "waited_for_dependency": release > arrival,
                    "released_after_predecessors": release >= predecessor_completion,
                    "submitted_at_release": int(row["submit_time_ns"]) == release,
                }
            )
    return result


def integration_case(binary, base):
    directory = base / "integration"
    directory.mkdir()
    shutil.copyfile(FIXTURE / "ssdconfig.xml", directory / "ssdconfig.xml")
    traces = []
    for name in ("slc.trace", "empty.trace", "tlc.trace"):
        target = directory / name
        shutil.copyfile(FIXTURE / name, target)
        traces.append(target)
    workload = workload_with_flows(traces, ["slc", "slc", "tlc"], True)
    workload.write(directory / "workload.xml")
    root = run_mqsim(
        binary, directory, directory / "ssdconfig.xml", directory / "workload.xml"
    )
    return canonical_result(root, directory, completion_flow_count=3)


def generated_case(binary, base, name, trace_contents, pool_ids, config_changes):
    directory = base / name
    directory.mkdir()
    config = ET.parse(FIXTURE / "ssdconfig.xml")
    for path, value in config_changes.items():
        element = config.find(path)
        if element is None:
            raise AssertionError(f"unknown config path {path}")
        element.text = str(value)
    for limit in config.findall(".//Block_PE_Cycles_Limit"):
        limit.text = "100000"
    config.write(directory / "ssdconfig.xml")
    traces = []
    for index, contents in enumerate(trace_contents):
        trace = directory / f"flow-{index}.trace"
        trace.write_text(contents)
        traces.append(trace)
    workload = workload_with_flows(traces, pool_ids)
    workload.write(directory / "workload.xml")
    return run_mqsim(
        binary, directory, directory / "ssdconfig.xml", directory / "workload.xml"
    )


def measurement_case(binary, base):
    root = generated_case(
        binary,
        base,
        "measurement-window",
        ["0 0 0 16 0 0 -1\n100000 0 16 16 0 1 -1\n"],
        ["slc"],
        {
            ".//Ideal_Mapping_Table": "true",
            ".//Measurement_Start_Time_Ns": 50000,
            ".//Measurement_End_Time_Ns": 200000,
        },
    )
    flow = root.find("Host/Host.IO_Flow")
    pools = {pool.get("ID"): pool for pool in root.findall(".//SSDDevice.Pool")}
    channels = {
        channel.get("ID"): channel
        for channel in root.findall(".//SSDDevice.Channel")
    }
    return {
        "flow": {
            field: scalar(flow.findtext(field))
            for field in (
                "Generated_Request_Count",
                "Completed_Request_Count",
                "Write_Request_Count",
                "Bytes_Transferred_Write",
                "Measurement_Host_Write_Bytes",
                "Average_Response_Time_Write",
            )
        },
        "slc_pool": {
            field: scalar(pools["slc"].get(field))
            for field in (
                "Host_Write_Bytes",
                "Measurement_Host_Write_Bytes",
                "Flash_Program_Command_Count",
                "Measurement_Flash_Programmed_Bytes",
            )
        },
        "slc_channel": {
            field: scalar(channels["0"].get(field))
            for field in (
                "Host_Write_Bytes",
                "Flash_Program_Command_Count",
                "Measurement_Flash_Programmed_Bytes",
            )
        },
    }


def media_profile_latency_case(binary, base):
    results = {}
    for pool_id in ("slc", "tlc"):
        root = generated_case(
            binary,
            base,
            f"media-{pool_id}",
            ["0 0 0 16 0 0 -1\n"],
            [pool_id],
            {".//Ideal_Mapping_Table": "true"},
        )
        flow = root.find("Host/Host.IO_Flow")
        pools = {
            pool.get("ID"): pool for pool in root.findall(".//SSDDevice.Pool")
        }
        results[pool_id] = {
            "Average_Response_Time_Write": scalar(
                flow.findtext("Average_Response_Time_Write")
            ),
            "Flash_Program_Command_Count": scalar(
                pools[pool_id].get("Flash_Program_Command_Count")
            ),
            "Media_Profile_ID": pools[pool_id].get("Media_Profile_ID"),
            "PE_Cycle_Limit": scalar(pools[pool_id].get("PE_Cycle_Limit")),
        }
    return results


def shared_pool_case(binary, base):
    trace = "0 0 2032 16 0 {request_id} -1\n"
    root = generated_case(
        binary,
        base,
        "shared-pool",
        [trace.format(request_id=0), trace.format(request_id=1)],
        ["slc", "slc"],
        {".//Ideal_Mapping_Table": "true"},
    )
    flows = root.findall("Host/Host.IO_Flow")
    pools = {pool.get("ID"): pool for pool in root.findall(".//SSDDevice.Pool")}
    channels = {
        channel.get("ID"): channel
        for channel in root.findall(".//SSDDevice.Channel")
    }
    return {
        "flows": [
            {
                field: scalar(flow.findtext(field))
                for field in (
                    "Flow_ID",
                    "Pool_ID",
                    "Generated_Request_Count",
                    "Completed_Request_Count",
                    "Write_Request_Count",
                    "Bytes_Transferred_Write",
                )
            }
            for flow in flows
        ],
        "slc_pool": {
            field: scalar(pools["slc"].get(field))
            for field in (
                "Host_Write_Bytes",
                "Flash_Program_Command_Count",
                "Measurement_Flash_Programmed_Bytes",
                "Logical_Capacity_Bytes",
            )
        },
        "slc_channel": {
            field: scalar(channels["0"].get(field))
            for field in ("Host_Write_Bytes", "Flash_Program_Command_Count")
        },
    }


def stress_goldens(binary, base):
    cases = (("mixed", 512), ("static-wl", 1000))
    results = {}
    for case_filter, count in cases:
        output = base / f"stress-{case_filter}"
        completed = subprocess.run(
            [
                "python3",
                str(FIXTURE / "stress_replay.py"),
                "--binary",
                str(binary),
                "--count",
                str(count),
                "--filter",
                case_filter,
                "--output",
                str(output),
            ],
            cwd=REPO,
            text=True,
            capture_output=True,
            timeout=120,
            check=False,
        )
        if completed.returncode != 0:
            raise AssertionError(completed.stdout + completed.stderr)
        for directory in sorted(path for path in output.iterdir() if path.is_dir()):
            root = ET.parse(directory / "workload_scenario_1.xml").getroot()
            ftl = root.find(".//SSDDevice.FTL")
            results[directory.name] = {
                field: scalar(ftl.get(field))
                for field in (
                    "Issued_Flash_Read_CMD",
                    "Issued_Flash_Program_CMD",
                    "Issued_Flash_Erase_CMD",
                    "Issued_Flash_Program_CMD_For_Mapping",
                    "GC_Execution_Count",
                    "GC_Page_Read_Count",
                    "GC_Page_Program_Count",
                    "Total_WL_Executions",
                )
            }
    return results


def collect_actual(binary):
    with tempfile.TemporaryDirectory(prefix="mqsim-golden-") as temporary:
        base = Path(temporary)
        return {
            "integration": integration_case(binary, base),
            "media_profile_latency": media_profile_latency_case(binary, base),
            "measurement_window": measurement_case(binary, base),
            "shared_pool_namespace": shared_pool_case(binary, base),
            "gc_and_wear_leveling": stress_goldens(binary, base),
        }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=REPO / "MQSim")
    parser.add_argument(
        "--print-actual",
        action="store_true",
        help="print canonical current output; never rewrites the committed golden",
    )
    args = parser.parse_args()
    actual = collect_actual(args.binary.resolve())
    if args.print_actual:
        print(json.dumps(actual, indent=2, sort_keys=True))
        return 0
    expected_document = json.loads(EXPECTED_PATH.read_text())
    expected = expected_document["cases"]
    if actual != expected:
        expected_text = json.dumps(expected, indent=2, sort_keys=True).splitlines()
        actual_text = json.dumps(actual, indent=2, sort_keys=True).splitlines()
        print("\n".join(difflib.unified_diff(expected_text, actual_text, "golden", "actual")))
        return 1
    print(f"MQSim DWPDSim canonical golden tests passed ({len(actual)} groups)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
