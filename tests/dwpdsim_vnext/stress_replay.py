#!/usr/bin/env python3
"""Deterministic multi-flow SSD replay with independent completion and TRIM checks."""

import argparse
import copy
import json
import random
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FIXTURE = REPO / "tests/dwpdsim_vnext"


def run_case(
    base,
    binary,
    name,
    scheduler,
    count,
    pattern="mixed",
    flows=1,
    interval=1000000,
    sharing="SHARED",
    cmt=64,
    dies=1,
    planes=1,
    seed=321,
    static_wl=None,
    channels=2,
    working_set_pages=None,
    timeout=45,
):
    if working_set_pages is not None and count < working_set_pages * flows:
        raise ValueError("count must cover every working-set page in every flow")
    directory = base / f"{scheduler}-{name}"
    directory.mkdir(parents=True, exist_ok=True)
    config = ET.parse(FIXTURE / "ssdconfig.xml")
    pages = working_set_pages or 128 // ((flows + 1) // 2)
    if working_set_pages is not None:
        # Each pool has a disjoint range per resident flow. Keep physical
        # headroom for data, mapping pages and GC; do not just enlarge the trace.
        pool_pages = pages * ((flows + 1) // 2)
        pages_per_plane_block = 16 * dies * planes
        blocks = max(
            16,
            (2 * pool_pages + pages_per_plane_block - 1) // pages_per_plane_block,
        )
        config.find(".//Block_No_Per_Plane").text = str(blocks)
        for pool in config.findall(".//Flash_Pool_Parameter_Set"):
            pool.find("Logical_Capacity_In_Sectors").text = str(pool_pages * 16)
    for key, value in {
        "Transaction_Scheduling_Policy": scheduler,
        "CMT_Capacity": cmt,
        "CMT_Sharing_Mode": sharing,
        "Die_No_Per_Chip": dies,
        "Plane_No_Per_Die": planes,
        "Measurement_End_Time_Ns": 10**15,
        "Flash_Channel_Count": channels,
    }.items():
        config.find(".//" + key).text = str(value)
    for limit in config.findall(".//Block_PE_Cycles_Limit"):
        limit.text = "1000000"
    if channels != 2:
        for pool in config.findall(".//Flash_Pool_Parameter_Set"):
            pool.find("Channel_IDs").text = (
                str(channels - 1)
                if pool.findtext("Pool_ID") == "slc"
                else ",".join(str(i) for i in range(channels - 1))
            )
    if static_wl is not None:
        for key, value in {
            "Ideal_Mapping_Table": "true",
            "Dynamic_Wearleveling_Enabled": "true",
            "Static_Wearleveling_Enabled": str(static_wl).lower(),
            "Static_Wearleveling_Threshold": "1",
        }.items():
            config.find(".//" + key).text = value
    config.write(directory / "ssd.xml")
    workload = ET.parse(FIXTURE / "workload.xml")
    scenario = workload.find("IO_Scenario")
    template = copy.deepcopy(scenario[0])
    for flow in list(scenario):
        scenario.remove(flow)
    trace_handles = []
    expected = [
        {
            "requests": 0,
            "reads": 0,
            "writes": 0,
            "trims": 0,
            "read_bytes": 0,
            "write_bytes": 0,
            "trim_bytes": 0,
        }
        for _ in range(flows)
    ]
    state = [[0] * pages for _ in range(flows)]
    predecessors = [[-1] * pages for _ in range(flows)]
    effective_trim_sectors = 0
    rng = random.Random(seed)
    for stream in range(flows):
        flow = copy.deepcopy(template)
        flow.find("Pool_ID").text = "slc" if stream % 2 == 0 else "tlc"
        flow.find("File_Path").text = str(directory / f"flow-{stream}.trace")
        flow.find("Enable_Request_Completion_Log").text = "false"
        scenario.append(flow)
        trace_handles.append((directory / f"flow-{stream}.trace").open("w"))
    workload.write(directory / "workload.xml")
    for index in range(count):
        stream = index % flows
        local_index = index // flows
        page = (
            local_index % pages
            if pattern == "sequential" or local_index < pages
            else rng.randrange(pages)
        )
        operation, offset, sectors = 0, 0, 16
        if pattern == "hot" and local_index >= pages:
            page = 32 + local_index % 32
        if local_index >= pages:
            if pattern in ("mixed", "trim", "partial"):
                operation = rng.choices(
                    [0, 1, 2], [55, 25, 20] if pattern == "trim" else [75, 25, 0]
                )[0]
            if pattern == "partial":
                sectors = 8
                offset = 8 * rng.randrange(2)
        if pattern == "unmapped-read":
            operation = 1
        elif pattern == "read-trim-write":
            operation = (
                1 if local_index < pages else rng.choices([0, 1, 2], [30, 40, 30])[0]
            )
        mask = ((1 << sectors) - 1) << offset
        if operation == 1 and pattern in ("unmapped-read", "read-trim-write"):
            # MQSim initializes an unmapped page on its first read, without
            # issuing a NAND program. Reflect that existing model in the oracle.
            if state[stream][page] == 0:
                state[stream][page] = mask
        elif operation == 1 and state[stream][page] & mask != mask:
            operation = 0
        if operation == 0:
            state[stream][page] |= mask
        elif operation == 2:
            effective_trim_sectors += (state[stream][page] & mask).bit_count()
            state[stream][page] &= ~mask
        e = expected[stream]
        e["requests"] += 1
        e[{0: "writes", 1: "reads", 2: "trims"}[operation]] += 1
        e[{0: "write_bytes", 1: "read_bytes", 2: "trim_bytes"}[operation]] += (
            sectors * 512
        )
        lba = (page + (stream // 2) * pages) * 16 + offset
        trace_handles[stream].write(
            f"{index * interval} 0 {lba} {sectors} {operation} {index} {predecessors[stream][page]}\n"
        )
        predecessors[stream][page] = index
    for handle in trace_handles:
        handle.close()
    record = {
        "name": name,
        "scheduler": scheduler,
        "io_count": count,
        "pattern": pattern,
        "flows": flows,
        "interval_ns": interval,
        "sharing": sharing,
        "cmt": cmt,
        "dies": dies,
        "planes": planes,
        "seed": seed,
        "static_wl": static_wl,
        "channels": channels,
        "working_set_pages_per_flow": pages,
        "expected": expected,
        "expected_effective_trim_sectors": effective_trim_sectors,
    }
    (directory / "input.json").write_text(json.dumps(record, indent=2))
    start = time.monotonic()
    with (
        (directory / "stdout.log").open("w") as out,
        (directory / "stderr.log").open("w") as err,
    ):
        try:
            completed = subprocess.run(
                [
                    str(binary),
                    "-i",
                    str(directory / "ssd.xml"),
                    "-w",
                    str(directory / "workload.xml"),
                ],
                cwd=REPO,
                stdout=out,
                stderr=err,
                timeout=timeout,
                check=False,
            )
            record["returncode"] = completed.returncode
        except subprocess.TimeoutExpired:
            record["returncode"] = "timeout"
    record["elapsed_s"] = round(time.monotonic() - start, 3)
    record["errors"] = []
    if record["returncode"] != 0:
        record["errors"].append(
            (directory / "stderr.log").read_text()[-1500:] or str(record["returncode"])
        )
    else:
        root = ET.parse(directory / "workload_scenario_1.xml")
        reported_flows = root.findall("Host/Host.IO_Flow")
        if sorted(int(f.findtext("Flow_ID")) for f in reported_flows) != list(range(flows)):
            record["errors"].append("missing, duplicate or unexpected flow IDs")
        for f in root.findall("Host/Host.IO_Flow"):
            stream = int(f.findtext("Flow_ID"))
            if stream not in range(flows):
                continue
            for tag, key in [
                ("Generated_Request_Count", "requests"),
                ("Completed_Request_Count", "requests"),
                ("Read_Request_Count", "reads"),
                ("Write_Request_Count", "writes"),
                ("Trim_Request_Count", "trims"),
                ("Bytes_Transferred_Write", "write_bytes"),
                ("Bytes_Transferred_Read", "read_bytes"),
                ("Bytes_Trimmed_Requested", "trim_bytes"),
            ]:
                value = int(f.findtext(tag))
                if value != expected[stream][key]:
                    record["errors"].append(
                        f"flow {stream} {tag}: {value} != {expected[stream][key]}"
                    )
        for pool in root.findall(".//SSDDevice.Pool"):
            pool_id = pool.get("ID")
            pool_channels = [
                channel
                for channel in root.findall(".//SSDDevice.Channel")
                if channel.get("Pool_ID") == pool_id
            ]
            pool_flows = [
                flow
                for flow in root.findall("Host/Host.IO_Flow")
                if flow.findtext("Pool_ID") == pool_id
            ]
            for pool_field, flow_field in (
                ("Host_Read_Bytes", "Bytes_Transferred_Read"),
                ("Host_Write_Bytes", "Bytes_Transferred_Write"),
                ("Requested_Trim_Bytes", "Bytes_Trimmed_Requested"),
                ("Measurement_Host_Write_Bytes", "Measurement_Host_Write_Bytes"),
            ):
                flow_total = sum(int(flow.findtext(flow_field)) for flow in pool_flows)
                if int(pool.get(pool_field)) != flow_total:
                    record["errors"].append(
                        f"pool {pool_id} {pool_field} differs from flows"
                    )
            for field in (
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
                "Measurement_Total_Block_Erase_Count",
                "Measurement_Flash_Programmed_Bytes",
                "Physical_Capacity_Bytes",
            ):
                channel_total = sum(
                    int(channel.get(field)) for channel in pool_channels
                )
                if int(pool.get(field)) != channel_total:
                    record["errors"].append(
                        f"pool {pool_id} {field}: {pool.get(field)} != channels {channel_total}"
                    )
        ftl = root.find(".//SSDDevice.FTL")
        record["gc_count"] = int(ftl.get("GC_Execution_Count"))
        record["mapping_reads"] = int(ftl.get("Issued_Flash_Read_CMD_For_Mapping"))
        record["mapping_programs"] = int(ftl.get("Issued_Flash_Program_CMD_For_Mapping"))
        record["wl_count"] = int(ftl.get("Total_WL_Executions"))
        if static_wl is False and record["wl_count"] != 0:
            record["errors"].append("static wear leveling ran while disabled")
        if static_wl is True and count >= 1000 and record["wl_count"] == 0:
            record["errors"].append("static wear leveling was not exercised")
        record["gc_page_programs"] = int(ftl.get("GC_Page_Program_Count"))
        # This single-plane read-only case may write mapping pages, but
        # online data initialization must not issue a NAND program.
        if pattern == "unmapped-read" and int(
            ftl.get("Issued_Flash_Program_CMD")
        ) != int(ftl.get("Issued_Flash_Program_CMD_For_Mapping")):
            record["errors"].append("unmapped reads issued data-page programs")
        record["effective_trim_sectors"] = int(
            ftl.get("Effective_Trimmed_Sector_Count")
        )
        if record["effective_trim_sectors"] != effective_trim_sectors:
            record["errors"].append(
                f"effective trim: {record['effective_trim_sectors']} != {effective_trim_sectors}"
            )
        for q in root.find(".//SSDDevice.TSU"):
            if q.get("No_Of_Transactions_Enqueued") != q.get(
                "No_Of_Transactions_Dequeued"
            ):
                record["errors"].append(str(q.attrib))
    record["status"] = "FAIL" if record["errors"] else "PASS"
    (directory / "result.json").write_text(json.dumps(record, indent=2))
    print(
        f"{record['status']} {scheduler} {name}: {record['elapsed_s']}s "
        + "; ".join(record["errors"])[:220],
        flush=True,
    )
    return record


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=REPO / "build/io-stress-run")
    parser.add_argument("--binary", type=Path, default=REPO / "MQSim")
    parser.add_argument("--count", type=int, default=50000)
    parser.add_argument("--filter", default="")
    parser.add_argument("--seed", type=int, default=321)
    parser.add_argument("--working-set-pages", type=int, help="Logical NAND pages per flow (not DWPDSim KV blocks); scales SSD capacity")
    parser.add_argument("--timeout", type=float, default=45, help="Seconds per simulator run")
    args = parser.parse_args()
    if args.count < 128:
        parser.error("--count must be at least 128 to initialize the working set")
    if args.working_set_pages is not None and args.working_set_pages < 64:
        parser.error("--working-set-pages must be at least 64")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    cases = [
        ("sequential", {"pattern": "sequential"}),
        ("random-write", {"pattern": "random"}),
        ("mixed", {}),
        ("partial", {"pattern": "partial"}),
        ("unmapped-read", {"pattern": "unmapped-read"}),
        ("read-trim-write", {"pattern": "read-trim-write"}),
        ("trim", {"pattern": "trim"}),
        ("burst", {"interval": 1000, "cmt": 256}),
        ("two-pools", {"flows": 2, "cmt": 256}),
        ("four-flows", {"flows": 4, "cmt": 256}),
        ("four-partitioned", {"flows": 4, "cmt": 256, "sharing": "EQUAL_PARTITIONING"}),
        ("multi-die-plane", {"dies": 2, "planes": 2}),
        ("burst-trim", {"pattern": "trim", "interval": 1000, "cmt": 256}),
        ("static-wl-on", {"pattern": "hot", "flows": 2, "static_wl": True}),
        ("static-wl-off", {"pattern": "hot", "flows": 2, "static_wl": False}),
        (
            "static-wl-multi-channel",
            {"pattern": "hot", "static_wl": True, "channels": 16},
        ),
    ]
    results = []
    for scheduler in ("OUT_OF_ORDER", "PRIORITY_OUT_OF_ORDER"):
        for name, options in cases:
            if args.filter and args.filter not in name:
                continue
            results.append(
                run_case(
                    args.output,
                    args.binary.resolve(),
                    name,
                    scheduler,
                    args.count,
                    seed=args.seed,
                    working_set_pages=args.working_set_pages,
                    timeout=args.timeout,
                    **options,
                )
            )
            (args.output / "summary.json").write_text(json.dumps(results, indent=2))
    print(
        f"{sum(r['status'] == 'PASS' for r in results)}/{len(results)} passed",
        flush=True,
    )
    return 0 if results and all(r["status"] == "PASS" for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
