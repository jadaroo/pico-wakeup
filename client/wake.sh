#!/usr/bin/env bash
# Send a command to the Pico and wait until it has taken effect.
#
#   ./wake.sh            # wake the laptop, wait until it's awake
#   ./wake.sh sleep      # put the laptop to sleep, wait until it's asleep
#   ./wake.sh status     # print the current status
#   ./wake.sh toggle     # toggle the Pico's LED
#
# Exit status: 0 = done, 1 = no confirmation before the timeout,
#              2 = the Pico reported a failure, 64 = usage error.
# So this works:  ./wake.sh && ssh user@laptop
#
# Needs the mosquitto clients: brew install mosquitto (macOS),
# sudo pacman -S mosquitto (Arch), sudo apt install mosquitto-clients (Debian/Ubuntu)
# Reads MQTT_HOST, MQTT_USER, MQTT_PASS from wakeup.env next to this script
# (copy wakeup.env.example), or from the file named by $WAKEUP_ENV.
# Optional: WAKE_TIMEOUT (seconds, default 60).
set -euo pipefail

env_file="${WAKEUP_ENV:-$(dirname "$0")/wakeup.env}"
# shellcheck source=wakeup.env.example
source "$env_file"

cmd="${1:-wakeup}"
timeout="${WAKE_TIMEOUT:-60}"
resend_every=5
conn=(-h "$MQTT_HOST" -p 8883 --tls-use-os-certs -u "$MQTT_USER" -P "$MQTT_PASS")

# For each command: the status event that means "done", and whether it's safe
# to send again if the Pico doesn't answer (it may be reconnecting and miss
# the first one; the broker doesn't queue messages for it).
case "$cmd" in
    wakeup) done_re='event=(laptop-awake|wakeup:key-tap-sent) '; resend=1 ;;
    sleep)  done_re='event=(laptop-asleep|sleep:already-asleep) '; resend=1 ;;
    status) done_re='event=status ';  resend=0 ;;
    toggle) done_re='event=toggle ';  resend=0 ;;   # resending would toggle twice
    *) echo "usage: $0 [wakeup|sleep|status|toggle]" >&2; exit 64 ;;
esac
fail_re='event=(wakeup|sleep):(remote-wakeup-not-enabled-by-host|usb-not-mounted)|event=unknown-command'

# Subscribe before publishing so the reply can't be missed. -R skips the
# retained (possibly old) status, so only live replies count.
tmp=$(mktemp -d)
fifo="$tmp/status"
mkfifo "$fifo"
mosquitto_sub "${conn[@]}" -i "client-sub-$$" -t pico-wakeup/pico/status -R -v >"$fifo" &
sub_pid=$!
trap 'kill "$sub_pid" 2>/dev/null; rm -rf "$tmp"' EXIT
exec 3<"$fifo"
sleep 1     # let the subscription become active

send() {
    echo "-> $cmd"
    mosquitto_pub "${conn[@]}" -i "client-pub-$$" -t pico-wakeup/pico/cmd -q 1 -m "$cmd"
}

send
start=$SECONDS
last_send=$SECONDS
answered=0

while (( SECONDS - start < timeout )); do
    if read -r -t 1 -u 3 line; then
        payload=${line#* }
        echo "<- $payload"
        answered=1
        if [[ "$payload " =~ $fail_re ]]; then
            echo "failed: the Pico reported an error" >&2
            exit 2
        fi
        if [[ "$payload " =~ $done_re ]]; then
            echo "done after $(( SECONDS - start )) s"
            exit 0
        fi
    elif ! kill -0 "$sub_pid" 2>/dev/null; then
        echo "error: lost the connection to the broker" >&2
        exit 1
    fi
    if (( resend && !answered && SECONDS - last_send >= resend_every )); then
        echo "   no answer yet, sending again"
        send
        last_send=$SECONDS
    fi
done

if (( answered )); then
    echo "timeout: the Pico answered, but no confirmation within $timeout s" >&2
else
    echo "timeout: no answer from the Pico within $timeout s (is it online?)" >&2
fi
exit 1
