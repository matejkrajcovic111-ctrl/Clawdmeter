# BLE protocol

Usage readings and hook events reach the app independently. The app selects
an account, builds a JSON payload, and writes it to the device over Bluetooth
LE. The firmware updates the meters and passes activity to Clawd's director.
Keyboard button reports use the separate HID service.

## Service and characteristics

The device advertises a custom GATT service next to the standard HID keyboard service:

|                                | UUID                                   |
| ------------------------------ | -------------------------------------- |
| **Data service**               | `4c41555a-4465-7669-6365-000000000001` |
| RX characteristic (write)      | `4c41555a-4465-7669-6365-000000000002` |
| TX characteristic (notify)     | `4c41555a-4465-7669-6365-000000000003` |
| REQ characteristic (notify)    | `4c41555a-4465-7669-6365-000000000004` |
| **HID service**                | `00001812-0000-1000-8000-00805f9b34fb` |

The host writes one complete JSON object to RX per write. The app limits
payloads to 500 bytes; the firmware has a 512-byte receive buffer, including
the terminating null byte. The device notifies `0x01` on REQ when it has no
data yet, asking for a poll.
It sends `{"dismiss":true}` on TX when a tap or the device's 2-minute timeout
ends an alert, so the app clears its corresponding alert too. The app expires
an alert nothing cleared after the same 2 minutes (a test alert after 30
seconds) and sends `x` if the device still shows it.

```json
{"s":45,"sr":120,"w":28,"wr":7200,"st":"allowed","acct":"pro","ok":true,"a":2,"e":"p","es":14,"n":1}
```

| Field | Meaning |
| --- | --- |
| `s`, `w` | Session and weekly % used. |
| `sr`, `wr` | Minutes until each resets; `-1` = window not started / unknown. |
| `st` | `allowed`, `limited` or `overage` (stored, not shown). |
| `acct` | `pro`, or `ent` for a spend-capped account: `s` is then the spend %, and `tp` (% of the billing period elapsed), `pd` (period length in days) and `rd` (reset date, e.g. `"Nov 1"`) fill the second card. |
| `t`, `tf` | Local wall-clock epoch and 12/24-hour format, when the clock is on. |
| `c` | `1` = the reset chime may sound. |
| `ok` | `false` alone = no usable account data; do not treat this as a zero reading. When every account is logged out, `ok:false` may still carry `ac` and `o` (each entry with `off:1`, no `ai`): the device keeps those pages, dimmed, and shows no top-level numbers. |
| `b`, `m`, `x` | Attention: alert chime, message (ASCII), end the alert. The app sends a fixed line per alert kind in `m`: `Question for you`, `Permission needed!` (also for other notification types), `Waiting for you!` (idle) or `MCP request!`; the test alert sends `Clawdmeter test alert`. The firmware shows whatever `m` it gets. |
| `a`, `e`, `es`, `n` | Fast clock: activity 0–3, latest hook event (`p` prompt, `s` stop, `b` back after a gap) and its sequence number, active sessions. |
| `sm` | Optional string (<= 28 bytes): active sessions per machine, e.g. `pc 2 · proxmox 3` (local first; `+N` = sessions of machines cut for space). Sent beside `n` only when a session runs on another machine; the first field dropped when a write is too big. |
| `rp` | `1` = the usage numbers are a replay, not a fresh poll (heartbeats, and writes between polls for an event, `a`/`n` change, alert or setting); the rate math and reset detection skip them. Fast-clock fields in the same write still count: a new `es` fires. |
| `lv` | How lively Clawd is, `1` (No-nonsense) to `5` (All-nonsense); `3` is the default. Sent with every data payload and right away when the setting changes. The device stores the last value in NVS, so it applies from boot before the app connects; absent = keep the stored value. |
| `aa` | How long Clawd points and waves for an alert, in seconds: `120` (the default, the whole alert), `30`, `10`, or `0` = no animation. Only the animation: the alert's message, tap, `x`, timeout and dismiss notify are unchanged. Sent like `lv` and stored the same way (clamped to 0–120); absent = keep the stored value. |
| `pm` | Pace marker: `1` draws a line on the session and weekly bars at the elapsed share of each window (5 hours, 7 days) and colours the fill by pace (green at or behind it, amber ahead, red at 100%); `0` keeps the plain 50/80% colours. A bar with no reset time (`sr`/`wr` = -1) and spend-capped accounts keep the plain colours. Sent like `lv` and stored the same way; absent = keep the stored value. |

### Several accounts

With two or more accounts in `config_dirs`, the usage fields belong to the
active account, and the others travel along so the device can swipe between
them. Old firmware ignores these keys.

| Field | Meaning |
| --- | --- |
| `ai` | Number of the account the usage fields belong to, 1–9: its place in `config_dirs`. Sent only with two or more accounts. Without `ai`, the usage is for the only account. |
| `ac` | How many accounts are configured, 1–9. Sent with `ai`. |
| `wk` | `1` while Claude is working in the account the usage fields belong to, `0` when not: the device runs its status-line spinner only while working. Working means a session's last hook event was a prompt or a finished tool, less than 10 minutes ago; a Stop, a notification or a question waiting on you ends it. Sent once hooks have been seen (with one account too); a change is written at once. Absent = unknown, shown as not working. |
| `th` | `1` while Claude is thinking, `0` when working but not thinking. Sent right after `wk`, and never `1` with `wk` = 0; the device colours its status line amber while it is `1`. A guess from the session transcript, which only records finished blocks: a prompt or a tool result means thinking, a reply whose last block is `thinking` does too, a `text` or `tool_use` block ends it. Absent when `wk` is absent. Old firmware ignores it. |
| `wt` | Output tokens of that account's latest prompt so far, counted like Claude Code's spinner: the replies' `output_tokens` in the session transcript since the prompt. Sent once hooks have been seen; a change goes out at most every 3 seconds. |
| `wd` | Seconds since that account's latest prompt, so the device's working timer matches Claude Code's after a reboot or a swipe. Sent with `wt`. |
| `o` | The other accounts' last readings, as an array of up to 3 objects `{"i":2,"s":9,"sr":10,"w":11,"wr":12,"wk":0,"th":0,"wt":512,"wd":40,"off":1}`. `wk`, `th`, `wt` and `wd` are that account's, like the top-level ones. `off` is sent right after `wd`, only as `1`: nobody is logged into this account on any watched folder, the numbers are its last reading, and the device dims the page. Absent otherwise; old firmware ignores it. `i` is the account number (1–9), `s` and `w` are % used, `sr` and `wr` are minutes until each reset (`-1` = not started or unknown). `w` is `-1` when the weekly figure is unknown (never `0`: no data is not zero). For a spend-capped account, `s` is the spend % and `w`/`sr`/`wr` are `-1`. Accounts with no reading yet are left out. The device keeps the first 3 entries. When the write is still too big after `sm` is dropped, each entry loses `wt`, then `wd`, then `th`, then `wk`; then entries that are not offline are dropped (before the alert text `m` is shortened). Offline entries are never dropped. |

### History message

Token history is a separate message on the RX characteristic, not a usage
payload. It has its own `h` key, so the device can tell the two apart.

```json
{"h":1,"i":2,"d":[ 30 integers ],"wt":1834000,"j":"..."}
```

| Field | Meaning |
| --- | --- |
| `h` | Always `1`. |
| `i` | Account number, 1–9. The device ignores other values. |
| `d` | 30 integers: billed tokens per day, in thousands (rounded to the nearest thousand), oldest first, last entry = today. Fewer than 30 are padded with zeros on the oldest side; more than 30 keep the newest 30. |
| `wt` | Billed tokens over the last 7 days, today included. |
| `j` | One short line of text for the weekly card. It changes once a day. |

The whole message is at most 500 bytes. If it is longer, `j` is dropped.

The app scans the transcripts every 10 minutes while connected, and scans
right after each connection. It sends an account's history only when its
numbers changed since the last send. After a reconnect it sends every
account again. If a write fails, it sends them all again on the next scan.
History messages wait until any pending usage write has gone out.
