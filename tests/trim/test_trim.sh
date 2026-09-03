#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
binary="${MQSIM_BINARY:-${repo_root}/MQSim}"
result="${repo_root}/tests/trim/workload_scenario_1.xml"

cd "${repo_root}"
"${binary}" -i tests/trim/ssdconfig.xml -w tests/trim/workload.xml </dev/null

python3 - "${result}" <<'PY'
import sys
import xml.etree.ElementTree as ET

root = ET.parse(sys.argv[1]).getroot()
flow = root.find("Host/Host.IO_Flow")
ftl = root.find("SSDDevice/SSDDevice.FTL")
assert flow is not None, "missing IO flow statistics"
assert ftl is not None, "missing FTL statistics"

expected_flow = {
    "Request_Count": "6",
    "Read_Request_Count": "0",
    "Write_Request_Count": "2",
    "Trim_Request_Count": "4",
    "Bytes_Transferred": "16384",
    "Bytes_Trimmed": "24576",
}
for key, expected in expected_flow.items():
    element = flow.find(key)
    actual = element.text if element is not None else None
    assert actual == expected, f"{key}: expected {expected}, got {actual}"

assert ftl.attrib.get("Trimmed_Sector_Count") == "16"
assert ftl.attrib.get("Pages_Invalidated_By_Trim") == "1"
assert ftl.attrib.get("Issued_Flash_Program_CMD") == "2"
assert ftl.attrib.get("Issued_Flash_Read_CMD") == "0"
print("TRIM end-to-end test passed")
PY
