#!/usr/bin/env bash
# Send a command to the Pico (default: "wakeup") and print its status replies.
#
#   ./wake-thinkpad.sh            # wake the ThinkPad
#   ./wake-thinkpad.sh status     # just ask for the current status
#   ./wake-thinkpad.sh toggle     # toggle the Pico's LED
#
# Needs: brew install mosquitto
# Reads MQTT_HOST, MQTT_USER, MQTT_PASS from wakeup.env next to this script
# (copy wakeup.env.example), or from the file named by $WAKEUP_ENV.
set -euo pipefail

env_file="${WAKEUP_ENV:-$(dirname "$0")/wakeup.env}"
# shellcheck source=wakeup.env.example
source "$env_file"

cmd="${1:-wakeup}"
conn=(-h "$MQTT_HOST" -p 8883 --tls-use-os-certs -u "$MQTT_USER" -P "$MQTT_PASS")

# Subscribe first so the reply isn't missed. The first line printed is the
# *retained* status (the last thing the Pico reported, possibly old);
# anything after it is the Pico's live response.
echo "Listening on pico-wakeup/pico/status for 15 s..."
mosquitto_sub "${conn[@]}" -i "mac-sub-$$" -t pico-wakeup/pico/status -v -W 15 &
sub_pid=$!
sleep 2

echo "Sending \"$cmd\" to pico-wakeup/pico/cmd"
mosquitto_pub "${conn[@]}" -i "mac-pub-$$" -t pico-wakeup/pico/cmd -q 1 -m "$cmd"

wait "$sub_pid" || true     # -W timeout ends mosquitto_sub with a non-zero status
