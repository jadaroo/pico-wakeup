#!/usr/bin/env bash
# Measure one suspend cycle: how long it slept, what woke it, whether the
# Pico stayed powered, and how much battery it used.
#
#   ./sleep-test.sh deep      # test S3 (needs sudo to switch mode)
#   ./sleep-test.sh s2idle    # test s2idle, for comparison
#   ./sleep-test.sh           # test whatever mode is active
#   ./sleep-test.sh deep 30   # wait 30 s before suspending (close the lid first)
#
# Unplug the charger first if you want a power number. Wake the laptop
# remotely (./mac/wake-thinkpad.sh) after a few minutes. Run this from a
# local terminal, not over SSH: SSH dies while the laptop sleeps.
set -euo pipefail

bat=/sys/class/power_supply/BAT0
orig_mode=$(grep -o '\[[a-z0-9]*\]' /sys/power/mem_sleep | tr -d '[]')
mode=${1:-$orig_mode}
delay=${2:-5}

# pm_debug_messages: the kernel logs which interrupt/GPE ended the sleep.
echo "$mode" | sudo tee /sys/power/mem_sleep >/dev/null
echo 1 | sudo tee /sys/power/pm_debug_messages >/dev/null
trap 'echo "$orig_mode" | sudo tee /sys/power/mem_sleep >/dev/null
      echo 0 | sudo tee /sys/power/pm_debug_messages >/dev/null' EXIT

# ACPI GPE (General Purpose Event) counters: hardware wake signals.
# On this ThinkPad: gpe16 = embedded controller (lid, battery, keys),
# gpe6D = USB controller (the Pico), gpe61 = PCIe/Thunderbolt.
gpes() {
    for f in /sys/firmware/acpi/interrupts/gpe[0-9A-F][0-9A-F]; do
        printf '%s %s\n' "$(basename "$f")" "$(awk '{print $1}' "$f")"
    done
}
gpe_before=$(gpes)

# Every wakeup source with its event counter, one per line: "name count"
snapshot() {
    for w in /sys/class/wakeup/wakeup*; do
        printf '%s %s\n' "$(cat "$w/name")" "$(cat "$w/event_count")"
    done | sort
}

before=$(snapshot)
ok_before=$(cat /sys/power/suspend_stats/success)
e0=$(cat "$bat/energy_now")      # microwatt-hours
start=$(date '+%Y-%m-%d %H:%M:%S')
on_ac=$(cat /sys/class/power_supply/AC/online)

echo "Mode: $(cat /sys/power/mem_sleep)   on charger: $([[ $on_ac == 1 ]] && echo yes || echo no)"
echo "Suspending in $delay s. Wait a few minutes, then wake it remotely."
sleep "$delay"
gpe_before=$(gpes)      # re-read: ignore events from closing the lid
systemctl suspend

# systemctl returns right away; wait until a suspend has completed.
until (( $(cat /sys/power/suspend_stats/success) > ok_before )); do
    sleep 1
done
sleep 3     # let resume logging settle
e1=$(cat "$bat/energy_now")

echo
echo "=== Result ==="
log=$(journalctl -b --since "$start" --no-pager -o short-unix)
t_in=$(grep -m1 "Performing sleep operation" <<<"$log" | cut -d' ' -f1)
t_out=$(grep -m1 "System returned from sleep" <<<"$log" | cut -d' ' -f1)
if [[ -n $t_in && -n $t_out ]]; then
    slept=$(awk -v a="$t_in" -v b="$t_out" 'BEGIN{printf "%d", b-a}')
    echo "Slept for:     ${slept} s"
else
    slept=0
    echo "Slept for:     unknown (no systemd-sleep lines found)"
fi
grep -m1 -oE 'PM: suspend entry \([a-z0-9]+\)' <<<"$log" | sed 's/^/Entered:       /' || true

irq=$(cat /sys/power/pm_wakeup_irq 2>/dev/null || true)
if [[ -n $irq ]]; then
    echo "Woken by IRQ:  $irq ($(awk -v i="$irq:" '$1==i{print $NF}' /proc/interrupts))"
fi

echo "Wake sources that fired:"
diff <(echo "$before") <(snapshot) | awk '/^>/{print "   ", $2}' || true

echo "ACPI GPEs during sleep (count increase):"
join <(echo "$gpe_before") <(gpes) | awk '$3 > $2 {printf "    %s +%d\n", $1, $3 - $2}'

echo "Kernel wake messages:"
journalctl -b -k --since "$start" --no-pager -o cat |
    grep -iE 'wakeup|wake-up|Triggering|GPE|SCI' | grep -v 'PM: suspend' | head -8 | sed 's/^/    /' || true

if grep -qE 'usb 1-4: USB disconnect' <<<"$log"; then
    echo "Pico (usb 1-4): DISCONNECTED during sleep - its port lost power"
else
    echo "Pico (usb 1-4): stayed connected"
fi

if [[ $on_ac == 0 && $slept -gt 60 ]]; then
    awk -v e0="$e0" -v e1="$e1" -v s="$slept" -v full="$(cat "$bat/energy_full")" 'BEGIN{
        wh = (e0 - e1) / 1e6
        printf "Battery used:  %.2f Wh  ->  average %.2f W  (~%.1f %% of the battery per hour)\n",
               wh, wh * 3600 / s, wh * 3600 / s / (full / 1e6) * 100 }'
else
    echo "Battery used:  not measured (on charger, or slept < 60 s)"
fi
