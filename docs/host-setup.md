# Host setup (the laptop being woken)

What the laptop needs so the Pico can wake it, and so it goes back to sleep by
itself afterwards. Written for a ThinkPad T14 Gen 1 (Intel) running CachyOS
with niri + DankMaterialShell, but the ideas apply to any Linux laptop.
Back to the [README](../README.md).

## 1. Plug the Pico into a port that stays powered

The Pico draws its power from the laptop's USB port. That port must stay
powered while the laptop sleeps.

To check: put the laptop to sleep and run `client/wake.sh status` from another
machine.

| You see | Meaning |
|---|---|
| `usb=suspended` | the port kept the Pico powered (good) |
| `event=offline` | the port lost power; the broker published the Pico's Last Will |

If the port loses power, use another port, or enable **Always On USB** in the
BIOS (Config → USB).

## 2. Check that wakeup is allowed

Linux lets USB **boot keyboards** wake the system by default. The firmware
presents itself as one, so this usually needs no action. To check:

```fish
cat /sys/bus/usb/devices/(grep -l 4012 /sys/bus/usb/devices/*/idProduct | xargs dirname)/power/wakeup
```

It must print `enabled`. The USB controller also has to be an ACPI wake source
(ACPI is the firmware interface that handles power states). Check this with
`grep XHC /proc/acpi/wakeup`, which should show `*enabled`.

## 3. Sleep mode: keep `s2idle`

Linux offers two kinds of suspend-to-RAM, listed in `/sys/power/mem_sleep`:

| Mode | What it is | Result on this ThinkPad |
|---|---|---|
| `s2idle` | the CPU idles in its deepest state, devices suspended | **Works.** Slept 19 min until the Pico woke it; **0.51 W**, about 1.2 % of the battery per hour |
| `deep` (S3) | the firmware powers almost everything off | **Unreliable.** With the lid closed it wakes up by itself after ~4–9 s (the wake signal comes from the USB controller); the Thunderbolt controller fails on every resume |

`deep` would save at most ~0.1–0.2 W, so it isn't worth it. Stay on `s2idle`,
which is the default here.

To measure a sleep cycle yourself, unplug the charger, run the command below
from a local terminal, then wake the laptop remotely:

```fish
host/sleep-test.sh s2idle      # or: host/sleep-test.sh deep 30
```

It reports:
- how long the laptop slept
- which wake source and ACPI GPE (General Purpose Event, the hardware wake
  signal) ended the sleep
- whether the Pico stayed connected
- the average power draw.

## 4. The lid

On this machine logind ignores the lid
(`/etc/systemd/logind.conf.d/10-lid.conf` sets `HandleLidSwitch*=ignore`).
That means:

- **Closing the lid doesn't suspend the laptop.** Suspend happens through
  auto-suspend (below), `systemctl suspend`, or the DMS menu.
- **After a remote wake, the laptop stays awake even with the lid closed**,
  until auto-suspend puts it back to sleep.

On a system where the lid *isn't* ignored, logind suspends again shortly
after waking with the lid closed. Set `HandleLidSwitchExternalPower=ignore`
if you want remote use while docked.

## 5. Auto-suspend

Without this, the laptop would stay awake forever after a remote wake. It
uses logind's built-in `IdleAction`, which suspends only when **every** login
session is idle:

- `host/swayidle.service` (user service) reports the desktop session as idle
  after **5 min** without keyboard/mouse input. logind can't see Wayland
  input on its own.
- `host/20-idle-suspend.conf` (logind drop-in) sets `IdleAction=suspend`
  after **10 more minutes** of everything being idle.
- **An SSH session is never idle** in logind's eyes, so the laptop stays
  awake while you're connected.

| Situation | Suspends after |
|---|---|
| You walk away from the laptop | ~15 min after the last keyboard/mouse input |
| Remote wake, nobody connects | ~15 min (the Pico's Shift tap counts as input) |
| Connected over SSH | never; ~10 min after the last SSH session closes |
| A video is playing | never (players block idle) |

Why not DMS's own "suspend after" setting? It only watches keyboard and mouse
input, so it would suspend the laptop in the middle of an SSH session.

### Install

```fish
sudo pacman -S swayidle
cp host/swayidle.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now swayidle.service
sudo install -m 644 host/20-idle-suspend.conf /etc/systemd/logind.conf.d/
sudo systemctl kill -s HUP systemd-logind      # reload config, keeps sessions
```

### Verify

```fish
busctl get-property org.freedesktop.login1 /org/freedesktop/login1 \
    org.freedesktop.login1.Manager IdleAction          # -> s "suspend"
loginctl show-session (loginctl list-sessions --no-legend | awk '$7=="tty1"{print $1}') -p IdleHint
```

The second command prints `IdleHint=yes` after 5 min without input.

### Keep it awake for a long task

For a task that keeps running after you disconnect (e.g. in tmux):

```fish
systemd-inhibit --what=idle --why="long build" <command>
```

### Change the timings

| What | Where |
|---|---|
| desktop idle time | `idlehint 300` in `~/.config/systemd/user/swayidle.service` |
| delay before suspending | `IdleActionSec` in `/etc/systemd/logind.conf.d/20-idle-suspend.conf` |

## 6. Remote sleep (`wake.sh sleep`)

The Pico has a second HID interface with the standard "System Sleep" key.
Linux maps it to `KEY_SLEEP`, and logind handles that with
`HandleSuspendKey=suspend` (the default). No setup is needed. To check that
nothing else has taken over the key:

```fish
busctl get-property org.freedesktop.login1 /org/freedesktop/login1 \
    org.freedesktop.login1.Manager HandleSuspendKey     # -> s "suspend"
systemd-inhibit --list | grep handle-suspend-key        # should print nothing
```

On this machine niri only takes over the *power* key
(`handle-power-key`), so the Sleep key reaches logind.

## 7. Reflash the Pico without sudo (optional)

`picotool load -f` reboots the running firmware into BOOTSEL through its
reset interface. The Pico's USB device node is root-only by default, so that
needs `sudo`. This udev rule gives the logged-in user access instead:

```fish
sudo install -m 644 host/60-pico-wakeup.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
```

The `uaccess` tag means "the user at the local seat may use this device".
It only works in rules numbered below 73, hence `60-`.

## 8. Remote access after waking

Waking only helps if you can then reach the laptop. Use Tailscale and SSH to
its Tailscale IP. Tailscale reconnects a few seconds after resume:

```sh
ssh <user>@<tailscale-ip>
```
