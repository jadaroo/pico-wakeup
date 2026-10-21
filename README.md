# pico-wakeup

[![Build firmware](https://github.com/jadaroo/pico-wakeup/actions/workflows/build.yml/badge.svg)](https://github.com/jadaroo/pico-wakeup/actions/workflows/build.yml)

**Wake a sleeping laptop from anywhere with a Raspberry Pi Pico W, and put it
back to sleep.**

The Pico W stays plugged into the laptop and pretends to be a USB keyboard.
It keeps a TLS connection to a cloud MQTT broker. When you send `wakeup` from
your phone or another computer, the Pico sends a USB remote-wakeup signal:
the same thing that happens when you press a key on a sleeping laptop.

```
 client (anywhere)            cloud                  home
┌──────────────┐  TLS   ┌──────────────┐  TLS   ┌─────────┐  USB   ┌──────────────┐
│ client/      │ ─────▶ │ MQTT broker  │ ◀───── │ Pico W  │ ─────▶ │ laptop       │
│ wake.sh      │ ◀───── │ (EMQX Cloud) │ ─────▶ │ on Wi-Fi│        │ (asleep)     │
└──────────────┘ status └──────────────┘  cmd   └─────────┘ wakeup └──────────────┘
```

- **Works from anywhere.** Both sides connect *out* to the broker, so there
  are no ports to open on your home router.
- **Nothing to install on the laptop for waking.** It uses the standard USB
  keyboard wake path the laptop already supports.
- **Secure.** TLS with certificate and hostname verification, and a separate
  login for each device.
- **Sleep, too.** `sleep` presses the standard USB "System Sleep" key, so
  the laptop suspends without any extra software.
- **Confirms it worked.** The client waits until the laptop is actually awake
  (or asleep), resends if the Pico missed the command, and returns an exit
  code, so `./wake.sh && ssh laptop` just works.
- **Reports its state.** The Pico publishes whether the laptop is asleep or
  awake, and the broker announces `offline` if the Pico disappears. Its LED
  shows the connection state.
- **Recovers by itself** from Wi-Fi and broker outages, with exponential
  backoff and a watchdog.

> Built on a ThinkPad T14 Gen 1 (CachyOS, Linux) as an embedded learning
> project. The Pico side is generic; the host notes are Linux-specific.

## Contents

- [Hardware and software](#hardware-and-software)
- [Repository layout](#repository-layout)
- [Setup](#setup)
- [Usage](#usage)
- [Troubleshooting](#troubleshooting)
- [Limitations](#limitations)
- [Documentation](#documentation)
- [License](#license)

## Hardware and software

| | |
|---|---|
| Board | Raspberry Pi **Pico W** (RP2040 + CYW43439 Wi-Fi, 2.4 GHz only) |
| Host | Any Linux laptop that can wake from USB (tested: ThinkPad T14 Gen 1, s2idle) |
| Broker | **EMQX Cloud Serverless** (free tier), or any MQTT broker with TLS on 8883 |
| Client | Anything with `mosquitto_pub`/`mosquitto_sub`: macOS, Linux, or a phone MQTT app |
| Build tools | Pico SDK 2.x, CMake, Ninja, `arm-none-eabi-gcc`, `picotool` |

## Repository layout

```
pico-wakeup/
├── firmware/                   runs on the Pico W
│   ├── CMakeLists.txt
│   ├── pico_sdk_import.cmake
│   ├── flash.sh                flash without pressing BOOTSEL
│   ├── secrets.h.example       copy to secrets.h (gitignored)
│   ├── src/
│   │   ├── main.c              main loop, Wi-Fi/DNS/MQTT state machine, commands
│   │   ├── usb_keyboard.c/.h   remote wakeup, Sleep key, key tap, USB suspend tracking
│   │   ├── usb_descriptors.c   USB serial + keyboard + system control + reset
│   │   └── ca_cert.h           broker CA (DigiCert Global Root G2)
│   └── config/
│       ├── lwipopts.h          TCP/IP stack settings
│       ├── mbedtls_config.h    TLS settings
│       └── tusb_config.h       USB stack settings
├── client/                     runs on the machine sending "wakeup"
│   ├── wake.sh
│   └── wakeup.env.example      copy to wakeup.env (gitignored)
├── host/                       runs on the laptop being woken
│   ├── swayidle.service        report desktop idle to logind
│   ├── 20-idle-suspend.conf    auto-suspend when everything is idle
│   ├── 60-pico-wakeup.rules    let picotool reflash the Pico without sudo
│   └── sleep-test.sh           measure a sleep cycle: duration, wake source, power
├── .github/workflows/
│   └── build.yml               CI: compile the firmware on every push
└── docs/
    ├── broker-setup.md         EMQX account, deployment and logins, step by step
    ├── host-setup.md           USB power, wakeup checks, sleep modes, auto-suspend
    ├── how-it-works.md         design, firmware walkthrough, MQTT/USB/TLS details
    └── testing.md              how to test each feature and recovery path
```

## Setup

### 1. Broker

Create a free EMQX Cloud Serverless deployment with two logins, `pico` and
`mac`, and note its address. Full step-by-step instructions:
**[docs/broker-setup.md](docs/broker-setup.md)**.

### 2. Firmware

```fish
cd firmware
cp secrets.h.example secrets.h      # then edit: Wi-Fi + broker address + pico login
cmake -S . -B build -G Ninja        # needs PICO_SDK_PATH
cmake --build build
```

To flash:
1. **First time:** hold **BOOTSEL** while plugging the Pico into the laptop,
   then run `sudo ./flash.sh`.
2. **Every time after:** just `sudo ./flash.sh`. No button needed: it uses
   the firmware's reset interface to reboot the Pico into BOOTSEL. It passes
   `--vid 0xcafe --pid 0x4012` because picotool only looks for Raspberry Pi's
   own USB ID by default. With `host/60-pico-wakeup.rules` installed, you can
   drop the `sudo`.

The LED shows how far the Pico got:

| LED | Meaning |
|---|---|
| off | no Wi-Fi yet (joining) |
| fast blink (4 Hz) | Wi-Fi up, connecting to the broker |
| steady on | connected (`toggle` switches it off/on) |

Watch the debug output with `minicom -D /dev/ttyACM0` (quit: Ctrl-A, then X).
A healthy start looks like:

```
=== Pico W wakeup ===
[wifi] connecting to "home-wifi"
[wifi] connected, IP 192.168.1.57
[dns] xxxxxxxx.ala.eu-central-1.emqxsl.com -> 3.120.x.x
[mqtt] connecting to xxxxxxxx.ala.eu-central-1.emqxsl.com (3.120.x.x:8883, TLS) as pico-1a2b
[mqtt] connected after 913 ms (longest step so far 859 ms)
[mqtt] -> pico-wakeup/pico/status: event=online connect-ms=913 longest-step-ms=859 reset=reboot led=on usb=active uptime=8
[mqtt] subscribed to pico-wakeup/pico/cmd
```

### 3. Host (the laptop)

Plug the Pico into a USB port that stays powered during sleep. Then install
auto-suspend, so the laptop goes back to sleep after a remote wake:

```fish
sudo pacman -S swayidle
cp host/swayidle.service ~/.config/systemd/user/
systemctl --user daemon-reload && systemctl --user enable --now swayidle.service
sudo install -m 644 host/20-idle-suspend.conf /etc/systemd/logind.conf.d/
sudo systemctl kill -s HUP systemd-logind
```

Details, checks and sleep-mode measurements:
**[docs/host-setup.md](docs/host-setup.md)**.

### 4. Client

```sh
brew install mosquitto              # macOS; Linux: pacman -S mosquitto / apt install mosquitto-clients
cd client
cp wakeup.env.example wakeup.env    # then edit: broker address + mac login
./wake.sh status
```

To get the client onto a Mac, clone the repository there, or copy just the
folder: `scp -r user@laptop:path/to/pico-wakeup/client ~/wakeup-client`.

## Usage

```sh
./wake.sh             # wake the laptop, wait until it's awake
./wake.sh sleep       # put the laptop to sleep, wait until it's asleep
./wake.sh status      # print the current status
./wake.sh toggle      # toggle the Pico's LED (a harmless connectivity test)

./wake.sh && ssh user@laptop      # connect as soon as it's up
```

The script subscribes to the status topic, sends the command, and prints the
Pico's live replies until the command has taken effect:

```
-> wakeup
<- event=wakeup:remote-wakeup-sent led=on usb=suspended uptime=4371
<- event=laptop-awake led=on usb=active uptime=4372
done after 2 s
```

If the Pico doesn't answer (e.g. it's reconnecting and missed the message),
`wakeup` and `sleep` are sent again every 5 s. `toggle` isn't, since that
would toggle twice. It gives up after 60 s (`WAKE_TIMEOUT=120 ./wake.sh` to
change).

| Exit status | Meaning |
|---|---|
| 0 | done: the laptop is awake / asleep, or the reply arrived |
| 1 | timeout: no confirmation (Pico offline, broker unreachable) |
| 2 | the Pico reported a failure (e.g. `remote-wakeup-not-enabled-by-host`) |
| 64 | unknown command |

| Field | Values |
|---|---|
| `event` | `online`, `offline` (Last Will), `laptop-asleep`, `laptop-awake`, `wakeup:<result>`, `sleep:<result>`, `toggle`, `status`, `unknown-command` |
| `wakeup:<result>` | `remote-wakeup-sent` (laptop was asleep), `key-tap-sent` (laptop was awake), `remote-wakeup-not-enabled-by-host`, `usb-not-mounted` |
| `sleep:<result>` | `sleep-key-sent`, `already-asleep`, `usb-not-mounted` |
| `led` | `on`, `off` |
| `usb` | `active`, `suspended` (laptop asleep), `unmounted` |
| `uptime` | seconds since the Pico booted |
| `connect-ms`, `longest-step-ms` | (`online` only) how long TCP + TLS + MQTT took, and the longest the main loop blocked |
| `reset` | (`online` only) why the Pico last restarted: `power-on`, `reboot` (e.g. flashing), `watchdog-timeout` (it hung; then also `hung-in=<state>/<step>`), `run-pin` |

Topics: commands go to `pico-wakeup/pico/cmd`, status comes back on
`pico-wakeup/pico/status` (retained, so a new subscriber immediately gets the last
one). Any MQTT client works, e.g. a phone app with a "wakeup" button.

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `wake.sh` prints `Connection Refused: not authorised` | wrong broker login | Check `client/wakeup.env`; re-add the login in EMQX |
| `wake.sh` hangs, then times out (no error) | port 8883 blocked on this network (common at universities) | Phone hotspot, or a browser client over WebSockets (`wss://<address>:8084/mqtt`) |
| `wake.sh`: `no answer from the Pico` (exit 1) | Pico offline | Check its LED and serial output |
| Status shows `reset=watchdog-timeout` | the firmware hung | `hung-in=` names the state and loop step; see [diagnostics](docs/how-it-works.md#diagnostics) |
| `picotool` says no device found | it only looks for Raspberry Pi's USB ID | Use `firmware/flash.sh` (passes `--vid`/`--pid`) |
| `sleep` sends the key but the laptop stays awake | something handles the Sleep key instead of logind | `systemd-inhibit --list` for a `handle-suspend-key` inhibitor; check `HandleSuspendKey` in logind |
| Serial: `[wifi] ... wrong password` / `SSID not found` | Wi-Fi settings, or a 5 GHz-only network | Fix `secrets.h`; the Pico W is 2.4 GHz only |
| Serial: `[mqtt] connection failed, status 4` or `5` | wrong Pico login | Fix `MQTT_USERNAME`/`MQTT_PASSWORD` in `secrets.h` |
| Serial: `[mqtt] connection failed, status 256` | TCP/TLS failed | Check `MQTT_BROKER_HOST` (bare hostname, no `mqtts://`) |
| Status shows `event=offline` while the laptop sleeps | the USB port loses power in sleep | Another port, or **Always On USB** in the BIOS |
| `wakeup:remote-wakeup-not-enabled-by-host` | Linux didn't grant wakeup | See [host setup §2](docs/host-setup.md#2-check-that-wakeup-is-allowed) |
| Laptop wakes up by itself right after suspending | `deep` sleep mode | Use `s2idle`; see [host setup §3](docs/host-setup.md#3-sleep-mode-keep-s2idle) |

Broker errors in detail: [docs/broker-setup.md](docs/broker-setup.md#step-7-if-something-fails).

## Limitations

- **Wakes from sleep only**, not from power-off or hibernate.
- **The Pico needs home Wi-Fi** (2.4 GHz) and a USB port that stays powered
  during sleep.
- **Certificate dates aren't checked.** The Pico has no clock; the signature
  chain and hostname are checked.
- **If the broker's CA changes**, `firmware/src/ca_cert.h` must be updated
  and the firmware reflashed. DigiCert Global Root G2 is valid until 2038.
- **Anyone with the client login can wake the laptop or put it to sleep.**
  Use long random passwords and topic permissions; see
  [broker setup](docs/broker-setup.md#step-8-optional-restrict-each-login-to-its-topics).
- **The broker doesn't queue commands for an offline Pico.** lwIP's MQTT
  client always uses a clean session, which is why the client resends.

## Documentation

- **[docs/broker-setup.md](docs/broker-setup.md):** create the EMQX
  deployment and logins, test them, and fix connection errors.
- **[docs/host-setup.md](docs/host-setup.md):** USB power, wakeup permission,
  why `s2idle` and not `deep` (with measurements), the lid, and auto-suspend.
- **[docs/how-it-works.md](docs/how-it-works.md):** the design explained:
  MQTT features used, the main loop and state machine, lwIP's two incoming
  callbacks, the locking rules for the poll arch, USB remote wakeup and the
  Sleep key, the client's resending, TLS, and the design decisions.
- **[docs/testing.md](docs/testing.md):** a checklist for every feature, and
  how to trigger each recovery path (Wi-Fi loss, refused login, dropped
  connection, missed command, watchdog).

References: [pico-examples MQTT client](https://github.com/raspberrypi/pico-examples/tree/master/pico_w/wifi/mqtt),
[TinyUSB HID composite example](https://github.com/raspberrypi/pico-examples/tree/master/usb/device/dev_hid_composite),
[lwIP MQTT client](https://www.nongnu.org/lwip/2_1_x/group__mqtt.html).

## License

[MIT](LICENSE). Two files in this repository aren't ours:

- **`firmware/pico_sdk_import.cmake`** is copied from the Pico SDK, under
  BSD-3-Clause © Raspberry Pi (Trading) Ltd. The notice is kept in the file.
- **`firmware/src/ca_cert.h`** contains DigiCert's public root certificate,
  which is published for anyone to use.

The firmware links the Pico SDK, lwIP, mbedTLS, TinyUSB and the CYW43 driver
at build time; each is under its own license in the SDK.
