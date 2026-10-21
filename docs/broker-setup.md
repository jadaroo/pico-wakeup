# Broker setup (EMQX Cloud Serverless)

Step-by-step setup of the free cloud MQTT broker that the client and the Pico
both connect to. Back to the [README](../README.md).
## What you're setting up

The **broker** is the server that passes messages between the Mac and the
Pico. It has to stay awake while the ThinkPad sleeps, and be reachable from
anywhere, so it runs in the cloud. EMQX calls your broker a **deployment**.
(HiveMQ calls the same thing a "cluster".)

The Serverless plan has a free monthly quota. One Pico connected 24/7 plus a
few wakeups from the Mac is a tiny load. With the **Spend Limit left at 0**,
EMQX can never charge you: it only uses the free quota.

> HiveMQ Cloud's free Serverless plan, used in an earlier version of this
> README, no longer accepts new sign-ups and shuts down on 2026-12-31.

## Step 1: Create an account

1. Open <https://www.emqx.com/en/cloud> and click **Get Started Free**
   (or **Sign Up**).
2. Sign up with email, Google or GitHub. If you use email, click the link in
   the verification email.
3. You land in the EMQX Cloud console (<https://cloud-intl.emqx.com/console/>).

## Step 2: Create a project (first time only)

EMQX groups deployments into **projects**. A new account has to create one
before it can deploy anything.

1. If the console asks you to create a project, do it. Name it e.g. `home`.
2. If you already see a project (a "Default Project" is sometimes created
   automatically), skip this step.

## Step 3: Create the Serverless deployment

1. On the **Overview** page, click **New Deployment**. Alternatively, open
   your project card and click **New Deployment** there.
2. In **Choose Plan**, select **Serverless**.
3. Leave **Spend Limit** at **`0`**. This is what keeps it free.
4. Choose a region if asked. Pick the one closest to home, e.g. Frankfurt
   (`eu-central-1`).
5. Under **Deployment Name & Project**, enter a name, e.g. `wakeup`.
6. Tick the box accepting the **EMQX Serverless Usage Terms**, then click
   **Deploy**.
7. Wait until the deployment's status on the **EMQX Brokers** page shows
   **Running**. This takes about a minute.

## Step 4: Copy the connection address

1. Click the deployment card to open its **Overview** page.
2. Under **Connection Information**, find:
   - **Address**, e.g. `a1b2c3d4.ala.eu-central-1.emqxsl.com`. Copy it
     *exactly*, without `mqtts://` and without a port. It goes into:
     - `firmware/secrets.h` → `MQTT_BROKER_HOST`
     - `client/wakeup.env` → `MQTT_HOST`
   - **Ports**: we use **8883** (MQTT over TLS). 8084 is WebSockets, for
     browsers. Serverless has no unencrypted port 1883 at all.
   - **CA Certificate**: you don't need to download it. It's DigiCert Global
     Root G2, which is already embedded in `ca_cert.h` and already in the
     Mac's and Linux's system trust stores.

## Step 5: Create the two logins

EMQX doesn't allow anonymous clients, so every device needs a username and
password.

1. In the deployment's left menu, open **Access Control** → **Authentication**.
2. Click **Add**, enter username `pico` and a password, and confirm.
3. Click **Add** again for username `mac` with a *different* password.

Why two logins, and how to pick passwords:
- **One per device.** If the Mac (or its password) is ever lost, delete only
  `mac`. The Pico keeps working and doesn't need reflashing.
- **Use long random passwords**: run `openssl rand -base64 24` and paste the
  result. Anyone with the `mac` login can wake your laptop.
- **Topic permissions.** If you never add rules under **Access Control** →
  **Authorization**, all topics are allowed, which is what we want. If you
  add rules later, both logins must be allowed to publish *and* subscribe
  under `pico-wakeup/#`. Otherwise the Pico logs `subscribe failed` and keeps
  reconnecting.

## Step 6: Test it from the ThinkPad (before flashing anything)

The ThinkPad already has `mosquitto_sub`/`mosquitto_pub`. Replace the address
and passwords with yours. The leading space keeps each command out of fish
history, so the passwords aren't saved:

```fish
# Terminal 1: listen to everything under pico-wakeup/ as the "pico" user
 mosquitto_sub -h a1b2c3d4.ala.eu-central-1.emqxsl.com -p 8883 --tls-use-os-certs \
     -u pico -P 'PICO_PASSWORD' -t 'pico-wakeup/#' -v -d

# Terminal 2: send a command as the "mac" user
 mosquitto_pub -h a1b2c3d4.ala.eu-central-1.emqxsl.com -p 8883 --tls-use-os-certs \
     -u mac -P 'MAC_PASSWORD' -t pico-wakeup/pico/cmd -m wakeup -d
```

Terminal 1 should print `pico-wakeup/pico/cmd wakeup`. That proves:
- the address is right
- TLS verification works: `--tls-use-os-certs` checks the broker's
  certificate against the system CAs, the same check the Pico does with
  `ca_cert.h`
- both logins can publish and subscribe.

In the console, the deployment's **Overview** page should now show
connections. Once `client/wakeup.env` is filled in, `client/wake.sh status`
also works from here.

## Step 7: If something fails

| What you see | Meaning | Fix |
|---|---|---|
| `Connection Refused: not authorised` / `bad user name or password` | TLS worked, the login didn't | Re-check username/password, or re-add it under Access Control → Authentication |
| `Error: Protocol error`, certificate or handshake errors | TLS failed | Use `-p 8883` and `--tls-use-os-certs`; check the address has no typo |
| Hangs, then times out | Port 8883 is blocked on this network, or the deployment is stopped | See the next section; check the deployment shows **Running** |
| `Unknown host` / `lookup failed` | Wrong address | Copy it again from Connection Information, without `mqtts://` |

On the Pico's serial output, the same problems show up as:
- `[mqtt] connection failed, status 4` or `status 5`: bad login
- `status 256`: TCP/TLS failed
- `[dns] dns lookup failed`: wrong address.

## University / public networks

Some campus networks block outgoing port 8883. The Pico is on your home Wi-Fi,
so this only affects the Mac. If the script hangs at university:
- use your phone's hotspot for the Mac, or
- use a browser MQTT client over WebSockets (port 8084, rarely blocked).
  [MQTTX Web](https://mqttx.app/web-client) works: connect to
  `wss://<address>:8084/mqtt` with the `mac` login, then publish `wakeup` to
  `pico-wakeup/pico/cmd`.

## Good to know

- **Inactivity stop.** EMQX stops a Serverless deployment after **30 days
  with no client connections**, and may delete it 30 days after that. The
  Pico is always connected, so this only happens if it's unplugged for a
  month. If it does, re-enable the deployment in the console.
- **Retained status.** The Pico publishes status *retained*: the broker keeps
  the last message and hands it to every new subscriber immediately. That's
  why the client script's first line can be old. Check `uptime=`, or wait for
  the live reply after it.
- **`event=offline`** is the Pico's *Last Will*. The broker publishes it by
  itself about 45 s (1.5 × the 30 s keepalive) after the Pico disappears
  without disconnecting.
- **Clearing the retained status.** Publish an empty retained message:
  ```fish
   mosquitto_pub -h <address> -p 8883 --tls-use-os-certs -u mac -P '...' \
       -t pico-wakeup/pico/status -r -n
  ```
- **The console changes sometimes.** If a button is named slightly
  differently, look in the deployment's left menu: **Overview** (address)
  and **Access Control** (logins).

