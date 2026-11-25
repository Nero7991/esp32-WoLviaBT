#!/usr/bin/env bash
# Push build/esp32-wol-via-bt.bin to the board over HTTP. Usage: tools/ota_push.sh <board-ip>
set -euo pipefail
cd "$(dirname "$0")/.."
ip=${1:?usage: tools/ota_push.sh <board-ip>}
pw=$(sed -n 's/^CONFIG_OTA_PASSWORD="\(.*\)"$/\1/p' sdkconfig.local)
user=$(sed -n 's/^CONFIG_OTA_USER="\(.*\)"$/\1/p' sdkconfig)
echo "before: $(curl -fsS --max-time 5 "http://$ip/")"
curl -fsS --max-time 120 -u "$user:$pw" -H 'Content-Type: application/octet-stream' \
     --data-binary @build/esp32-wol-via-bt.bin "http://$ip/ota"
echo
for _ in $(seq 1 30); do
    sleep 2
    if out=$(curl -fsS --max-time 2 "http://$ip/" 2>/dev/null); then echo "after:  $out"; exit 0; fi
done
echo "board did not come back within 60 s" >&2
exit 1
