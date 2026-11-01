# Relay setup

The relay lets Claude Code run on a server and still show on your Clawdmeter.
This guide assumes three machines, all on Tailscale:

- a Proxmox server (LXC or VM) that runs Claude Code, with two accounts;
- a home Windows workstation running the Clawdmeter app;
- a work laptop running the Clawdmeter app.

The relay sends to both PCs; whichever one runs the app takes the message.

## 1. What it sends

`clawdmeter-relay` runs on the server and sends:

- **Alerts and events:** the hook name (such as `Stop`), the session id, the
  alert kind (such as a permission prompt), and the account number.
- **Live usage:** the 5-hour and 7-day percentages, their reset times, and the
  account number. Claude Code reports these after each response, so they need
  a Pro or Max account.
- **Token totals:** billed tokens per day for the last 35 days, per account.

It never sends prompts, tool calls, or transcript content.

Messages are plain HTTP over Tailscale, which encrypts traffic between your
machines. Each request carries the shared key in an `X-Clawdmeter-Key` header.

## 2. On each PC

Do this on the workstation and on the laptop.
**Make a secret.** Use the same secret on every machine. It must be at least
16 characters. In PowerShell:

```powershell
-join ((48..57)+(97..122) | Get-Random -Count 32 | % {[char]$_})
```

On Linux:

```sh
openssl rand -hex 16
```

**Edit the app config.** The file is `%LOCALAPPDATA%\Clawdmeter\config` on
Windows and `~/.config/clawdmeter/config` on Linux. Add:

```
relay_listen = 100.x.y.z:8790
relay_key = <your secret>
```

Set `relay_listen` to this PC's Tailscale address (`tailscale ip -4`). Do not
use a bare port: that listens on every interface.

**Restart the app.** The listen address is read only at startup.

**Allow the port in Windows Firewall, Tailscale only.** Run this as administrator:

```powershell
New-NetFirewallRule -DisplayName "Clawdmeter relay (Tailscale)" -Direction Inbound -Protocol TCP -LocalPort 8790 -RemoteAddress 100.64.0.0/10 -Action Allow
```

## 3. On the server

**Build the binary on the workstation.** You need Rust and the musl target:

```powershell
rustup target add x86_64-unknown-linux-musl
cd <repo>\desktop
$env:CARGO_TARGET_X86_64_UNKNOWN_LINUX_MUSL_LINKER = "rust-lld"
cargo build -p clawdmeter-relay --release --target x86_64-unknown-linux-musl
```

**Copy it to the server** (the file is in `desktop\target\x86_64-unknown-linux-musl\release\`; replace `proxmox` with your server):

```powershell
scp target\x86_64-unknown-linux-musl\release\clawdmeter-relay root@proxmox:/usr/local/bin/
```

On the server, run `chmod +x /usr/local/bin/clawdmeter-relay`.

**Write the config.** Copy `tools/relay/config.example` to the server, then:

```sh
mkdir -p ~/.config/clawdmeter-relay
cp config.example ~/.config/clawdmeter-relay/config
chmod 600 ~/.config/clawdmeter-relay/config
```

Edit the file and set:

- `targets`: your two PCs, by Tailscale name or address. The port defaults to 8790.
- `key`: the same secret as `relay_key` on each PC.
- `machine`: this server's name (letters, digits, `-` and `_`).
- `accounts`: the two Claude Code config dirs, as full paths (no `~`). The
  order must match `config_dirs` in each PC's app config.

**Test the route and the key:**

```sh
clawdmeter-relay test
```

You want one `ok` line per PC. The command succeeds if any PC accepts.

**Wire up Claude Code:**

```sh
clawdmeter-relay install
```

This adds the hooks and status line to each account's `settings.json`. Your
existing hooks stay, and an old status line moves to `statusline_command`.

**Install the token timer.** Copy `clawdmeter-relay-tokens.service` and
`clawdmeter-relay-tokens.timer` from `tools/relay` to the server's `/root`, then:

```sh
cp clawdmeter-relay-tokens.service clawdmeter-relay-tokens.timer /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now clawdmeter-relay-tokens.timer
```

The timer runs two minutes after boot, then every ten minutes. Check it with
`journalctl -u clawdmeter-relay-tokens.service`.

## 4. Troubleshooting

Start with `clawdmeter-relay test`, which prints one line per PC.

**`unreachable`**: the server could not connect. Check that Tailscale is up
on both ends (`tailscale status`), the app is running, the firewall rule is
in place, `relay_listen` matches `tailscale ip -4`, and `targets` is right.

**`refused, the key doesn't match`**: `key` on the server and `relay_key` on
the PC differ. Copy the secret again, without quotes.

**`HTTP 503`**: the PC has no usable `relay_key`. It is missing or under 16 characters.

**`HTTP 400`**: the PC rejected the message. One side is probably an older build.
Rebuild both from the same source.

**The test works, but nothing shows on the device.** Check the account order.
A mismatch sends events to the wrong account. Make sure the device is near
the PC that runs the app.

**The app log.** The app writes `daemon.log` in its state folder
(`%LOCALAPPDATA%\Clawdmeter\` on Windows, `~/.config/clawdmeter/` on Linux).
Look for `relay: listening on`, `relay: can't listen on` (wrong address or port
in use), `relay: refused a message with a wrong key`, and
`relay: token totals from <machine>`.

The server never prints hook errors, so use `clawdmeter-relay test` to find
out why a message is not getting through.
