# What's different in this fork

This fork (clawdmeter2) keeps the normal Clawdmeter app and board. It adds
the features below. The [user guide](user-guide.md) still covers installing,
pairing and the basics.

## Several accounts

Add more than one Claude Code folder to `config_dirs`. The order sets the
account numbers: the first folder is account 1, the second is account 2, and
so on.

- The device shows a **#N** badge in the bottom-right corner when there are
  two or more accounts. The clock sits in the top middle, in the same font
  as the cards.
- Each account has its own Clawd. Account 2 uses a blue palette, and its
  Clawd is blue too, with wider, shorter eyes. In his corner seat he does a
  laptop, basketball, soccer and jumping; account 1 has waving, pointing and a
  lurk trip. Each one has his own shows. In pet mode, account 2's Clawd says
  different things, such as "beep!", "tsk." and "hehe~".
- The **status line** follows whether Claude is working in the account on
  screen (this needs the hooks installed for that account). While it works,
  the line looks like Claude Code's: a spinning glyph and a verb in Claude
  Code's spinner colours, orange with a lighter shimmer (blue on account 2).
  The verb itself can't match Claude Code's word: Claude Code picks it at
  random and doesn't share it. Every 15 seconds it swaps for 4 seconds to the
  numbers: time since your prompt and the tokens Claude has written for it,
  such as `✻ 2m 13s · 3.4k tokens` (the same counts as Claude Code's; the
  token count starts again when you send a message mid-turn). When it
  stops, the line goes grey and still and says how long that took, such as
  `✻ Brewed for 1m 23s`. Without hooks the line
  behaves as before.
- The device shows the account you are using. A prompt in an account
  switches to it at once (the account needs the hooks installed); otherwise
  the account whose usage rose last is shown.
- **Swipe** to change page. The pages run left to right: **This device**,
  account #1, account #2. Swipe right (finger moves left to right) to go one
  page right, and swipe left to go one page left. The pages do not wrap, and
  the cards slide across with your finger. After a minute without a touch the
  device goes back to the account you are using and closes the This device
  page.
- To add an account, log in to it once with its own folder, for example
  `CLAUDE_CONFIG_DIR=~/.claude-2 claude`, then add that folder to
  `config_dirs` and install the hooks for it on the app's Hooks page.
- The status line can show **#N has more room**. This appears when the account
  you are on has used at least 75% of its tighter limit (session or week),
  and another account has at least 20 points more room.
- The run-out line (`out in 1h40m`) is no longer shown.

## This device page

The first page, **This device**, shows this board's own stats, laid out like
the `/usage` screens:

- processor: clock speed (MHz), cores, and how busy the UI is;
- memory, PSRAM and app flash;
- chip temperature;
- battery, Bluetooth state, uptime, brightness and firmware.

It refreshes every second. Swipe right to return to account #1.

## Tapping the screen

- **Tap the session card** for a screen laid out like Claude Code's `/usage`:
  "Current session", a bar with "N% used", and "Resets in …". Below it are
  the last 30 days as bars, with the busiest day and the 30-day total.
- **Tap the weekly card** for the same view of "Current week (all models)",
  with this week's tokens and a short line of text below.
- Any tap closes these screens.
- **Tap the clock** to show the date and year for 4 seconds.
- **Tap Clawd** in his corner seat (top-left) to start pet mode.

## Pet mode

In pet mode, Clawd's face fills the screen and reacts to you:

- **Tap** to boop it. Several quick boops annoy it.
- **Rub sideways** to stroke it. It watches your finger.
- **Shake** it to make it dizzy, and **tilt** it to roll its eyes. These need
  an accelerometer, so they do not work on the C6 boards yet.
- It sweats above 90% usage, and it watches a thinking dot while Claude works.
- It gets bored after about 45 seconds without input, sleeps at night, stretches
  in the morning, and sometimes sneezes, hiccups or yawns.
- When the 5-hour window resets, it celebrates with confetti.

## Resting face

When Clawd peeks after a tap on the meters, or during the screensaver, his
face does more than blink. It looks around, hops, grins, gets surprised,
winks, daydreams upward, dozes off, rolls its eyes and wiggles.

- Account 1 is curious and bouncy.
- Account 2 is dreamy and cheeky.
- At night both mostly doze.

## Alerts

When Claude finishes and waits for you, or needs a permission, the device
beeps and shows **Waiting for you**. The chime can be turned off in the app's
Settings.

Leave pet mode by **tapping the top strip** of the screen, by pressing a button,
or by leaving it alone for two minutes. A swipe strokes the face; it does not
leave pet mode.

## Status line facts

While the numbers are live, short facts take turns with the status verb:

- the **#N has more room** hint;
- sessions per machine, such as `pc 2/proxmox 3`, when a session runs on
  another computer (through the relay).

## Pairing a second computer

The board keeps up to three paired computers, but only two Bluetooth links
at a time. Pair the first computer as usual. Then:

1. Hold **PWR** for about 3 seconds and release it, before 6 seconds pass.
   The status line says **Pair another computer now**. The window stays open
   for 3 minutes.
2. Pair the second computer in its Bluetooth settings. The window closes once
   a new computer has joined.
3. To forget every paired computer, hold **PWR** again while the window is
   open. A hold when no window is open opens a new window instead.

Outside a pairing window, a computer that is not already paired is refused.

Boards without a PWR button cannot open a window. On those boards, hold the
pairing hint on the touch screen for 5 seconds to clear all pairings, then
pair again.

## Relay and status line

- **Relay.** Claude Code on a server or another PC can show on this device.
  The Settings page has a **Relay (other machines)** section with an address
  field and a key field (with a **Generate** button). See
  [relay setup](relay-setup.md) for the steps.
- **Status line.** The hooks installer sets each account's Claude Code status
  line to `clawdmeter statusline`. That command saves the account's live
  rate limits for the app, so the device follows usage within seconds. It
  needs Claude Code 2.1.80 or later and a Pro or Max account. If you already
  had a status line, it is saved as `statusline_command`, and its output is
  printed instead of the default `5h 23% · 7d 41%`.

## Config keys

| Key | Effect |
| --- | --- |
| `config_dirs` | Claude Code folders, comma-separated. The order sets the account numbers. |
| `poll_interval = 120` | Recommended without the relay. While Claude works, the shown account is checked about every 2 minutes. The app never checks one account more often than every 2 minutes. |
| `relay_listen` | The address this PC listens on for relay messages, for example `100.64.1.2:8790` (use the Tailscale address). Read at startup. |
| `relay_key` | The shared secret the relay sends. At least 16 characters; a shorter key is ignored. |
| `statusline_command` | A command that `clawdmeter statusline` runs in place of its own line. It gets the same input, and its output is printed. |

## Known limits

- The board allows two Bluetooth connections at a time (`CONFIG_BT_NIMBLE_MAX_CONNECTIONS=2`).
  Two PCs in range cannot both be connected at once.
- No shake or tilt on the C6 boards yet. Their IMU code does not provide the accelerometer reading the pet needs.
- Without the relay, usage on the device updates about every 2 minutes while
  Claude works (with `poll_interval = 120`). When nothing runs, checks are
  slower. The status line on this PC gives faster updates when it is installed.
- Boards without a PWR button cannot open a pairing window (see above).
