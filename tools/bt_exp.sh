#!/usr/bin/env bash
# Bench helper for the handover experiments. Usage:
#   tools/bt_exp.sh arm [role]        BC-250 BT off, board accepting + scanning (role central|peripheral)
#   tools/bt_exp.sh drop <query>      wait until held, send POST /bt?<query>, watch 60 s for a re-page
#   tools/bt_exp.sh restore           board reject+auto, BC-250 BT on
set -euo pipefail
cd "$(dirname "$0")/.."
B=http://192.168.97.142; H=ocollaco@192.168.97.133
pw=$(sed -n 's/^CONFIG_OTA_PASSWORD="\(.*\)"$/\1/p' sdkconfig.local)
post() { curl -sS -u "admin:$pw" -X POST "$B/bt?$1"; }
bt() { curl -sS "$B/" | python3 -c "import json,sys; b=json.load(sys.stdin)['bt']; print($1)"; }
LOG=logs/bt-$(date -u +%Y%m%d).log
case "$1" in
arm)
    ssh -o BatchMode=yes $H 'bluetoothctl power off' >/dev/null
    post "mode=accept&scan=on&handover=off&role=${2:-central}"
    bt '{k:b[k] for k in ("mode","scanning","holding")}' ;;
drop)
    for _ in $(seq 1 300); do [ "$(bt 'b["holding"]')" = True ] && break; sleep 1; done
    bt '"held_ms=%d reports=%d" % (b["held_ms"], b["hid_reports"])'
    n0=$(wc -l < "$LOG")
    echo "$(date +%T) POST /bt?$2 -> $(post "$2")"
    sleep 60
    tail -n +"$((n0 + 1))" "$LOG" | grep -E "bt_wake: (DROP|HOLD|CONN|DISCONN|REPAGE|L2CAP|EVENT|ENCRYPT|LINK|first)" | cut -c12-23,40- ;;
restore)
    post "mode=reject&scan=auto&role=central"
    ssh -o BatchMode=yes $H 'bluetoothctl power on' >/dev/null
    bt '{k:b[k] for k in ("mode","scan_policy","scanning","host_up")}' ;;
esac
