# Testing

How to check, on the real hardware, that every feature and every recovery
path works. Watch the Pico's log while you test:
`minicom -D /dev/ttyACM0`. Back to the [README](../README.md).

## Features

Run these from another machine (or the laptop itself, for the first three):

| Test | Command | Expect |
|---|---|---|
| Connectivity | `client/wake.sh status` | `done`, exit 0 |
| Key tap (laptop awake) | `client/wake.sh` | `event=wakeup:key-tap-sent`, exit 0 |
| LED | `client/wake.sh toggle` twice | LED off, then on again; at boot: off while joining Wi-Fi, fast blink, then steady |
| Remote sleep | `client/wake.sh sleep` | `sleep:sleep-key-sent`, then `laptop-asleep`; the laptop suspends |
| Remote wake | `client/wake.sh` (laptop asleep) | `remote-wakeup-sent`, then `laptop-awake` within a few seconds |
| Wake + connect | `client/wake.sh && ssh user@<tailscale-ip>` | SSH connects right after `done` |
| Bad command | `mosquitto_pub ... -t pico-wakeup/pico/cmd -m foo` | `event=unknown-command` |

## Recovery paths

These paths only run when something breaks, so they're the least tested.
Each test says what the log should show.

### Wi-Fi loss

1. Turn the router's Wi-Fi off (or move the Pico out of range) for ~1 min.
2. Log: `[wifi] link lost`, then retries with growing delays
   (`retrying in 1000 ms`, `2000 ms`, `4000 ms` ...). The LED blinks slowly.
3. Turn Wi-Fi back on. Log: `[wifi] connected` → `[mqtt] connected` →
   `subscribed`, and the LED goes steady. The status shows `event=online`.
4. While Wi-Fi was down, the broker published `event=offline` (the Last
   Will) about 45 s after the Pico vanished. Check with `client/wake.sh status`
   from outside, or by subscribing to `pico-wakeup/pico/status`.

### Broker refuses the login

1. In EMQX, change the `pico` password (or delete the credential).
2. Restart the Pico (unplug/replug) or wait for a reconnect. Log:
   `[mqtt] connection failed, status 4` (or `5`), with backoff up to 60 s.
   The LED blinks fast.
3. Restore the password. Within one backoff interval it reconnects. The
   backoff resets to 1 s after success.

### Broker drops the connection

1. In EMQX, kick the client: the deployment's client list has a disconnect
   option. Or briefly disable the deployment.
2. Log: `[mqtt] connection lost, status 256`, then a reconnect starting from
   DNS.

### Missed command

1. Turn Wi-Fi off as above and immediately run `client/wake.sh` elsewhere.
2. The client prints `no answer yet, sending again` every 5 s.
3. Turn Wi-Fi back on within the timeout (60 s). After the Pico reconnects,
   one of the resent commands arrives and the client prints `done`.

### Watchdog

There's no command to hang the firmware on purpose. To see the watchdog
work, temporarily add `while (1) {}` to `handle_message()` for some command,
flash it, and send that command. About 8 s later the Pico reboots and logs
`[boot] reset=watchdog-timeout hung-in=mqtt-up/poll`. The incoming message
is handled inside `cyw43_arch_poll()`, so it hangs in the `poll` step. The
next `online` status carries the same information. Remove the test code
afterwards.

### Reboot loop check

After flashing, count USB reconnects for a few minutes. A healthy Pico
connects once and stays:

```fish
journalctl -k --since "-5min" | grep -c 'usb 1-4: USB disconnect'
```

Read the reason for the last restart from the `online` status (`reset=`).
