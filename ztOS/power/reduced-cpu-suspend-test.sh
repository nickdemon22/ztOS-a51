#!/bin/sh
# Bounded SICD diagnostic with one CPU per cluster left online.
set -eu

SELF=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
READY=/sys/class/sec/sensorhub/ready
TIMEOUT=600
DEADLINE=$(( $(date +%s) + TIMEOUT ))
changed_cpus=

case "${1-}" in
    --connected) connected=1 ;;
    '') connected=0 ;;
    *) echo 'Usage: reduced-cpu-suspend-test.sh [--connected]' >&2; exit 2 ;;
esac
set -- python3 -u "$SELF/suspend-test.py" --sicd --cpd \
    --allow-mif-sleep --seconds 10
if [ "$connected" = 0 ]; then
    set -- "$@" --wait-unplug 600 --battery-only --ignore-host-req-wake
fi
# Validate the exact command before waiting or changing CPU state.
"$@" --validate-only

if [ "$connected" = 0 ]; then
echo '{"phase":"waiting_for_unplug","timeout":600}'
while :; do
    online=0
    for supply in /sys/class/power_supply/*/online; do
        [ -f "$supply" ] || continue
        [ "$(cat "$supply")" = 1 ] && online=1
    done
    [ "$online" -eq 0 ] && break
    [ "$(date +%s)" -lt "$DEADLINE" ] || {
        echo '{"phase":"unplug_timeout"}'
        exit 1
    }
    sleep 1
done

# Let suspend-test.py wait for the unplug transition and its 15-second settle
# period after the reduced CPU set is in place.
for supply in /sys/class/power_supply/*/online; do
    [ -f "$supply" ] || continue
    [ "$(cat "$supply")" = 0 ] || {
        echo '{"phase":"power_returned_before_test"}'
        exit 1
    }
done
fi
[ "$(cat "$READY")" = 1 ] || {
    echo '{"phase":"sensorhub_not_ready"}'
    exit 1
}

restore_cpus() {
    for cpu in $changed_cpus; do
        echo 1 > "/sys/devices/system/cpu/cpu$cpu/online"
    done
}
trap restore_cpus EXIT
trap 'restore_cpus; exit 1' HUP INT TERM

# zrn-hotplug protects the last online CPU in each cluster, so retain cpu0
# and cpu4 and remove the other little cores for this one bounded trial.
for cpu in 1 2 3; do
    online_file="/sys/devices/system/cpu/cpu$cpu/online"
    if [ "$(cat "$online_file")" = 1 ]; then
        changed_cpus="$changed_cpus $cpu"
        echo 0 > "$online_file"
    fi
done
echo "{\"phase\":\"reduced_cpus\",\"offline\":\"$changed_cpus\"}"

"$@"

restore_cpus
trap - EXIT HUP INT TERM
echo '{"phase":"cpus_restored"}'
