#!/usr/bin/env bash
set -Eeuo pipefail
base=${BASE_URL:-http://127.0.0.1:8000}
curl --fail --silent "$base/redfish/v1/" >/dev/null
curl --fail --silent "$base/metrics" | grep -q '^bmc_sel_records '
echo "QEMU management integration: passed"
