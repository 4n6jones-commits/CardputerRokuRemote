# CardputerRokuRemote

A Roku remote for the M5Stack Cardputer ADV, built as a Launcher app.

**How it actually works:** the real RC-AL2 pairs with a Roku over a
proprietary RF link the Cardputer has no compatible radio for, so this
doesn't clone the *radio protocol* - it clones the *remote's function* using
Roku's External Control Protocol (ECP), the same local-network HTTP API the
official Roku mobile app uses. That means:

- The Roku and the Cardputer must be on the **same WiFi network**.
- On the Roku: **Settings > System > Advanced system settings > Control by
  mobile apps** must be enabled (it's on by default).

## Setup

1. Install via the Launcher: copy the app `.bin` to `downloads/` on the SD
   card (e.g. `downloads/CardputerRokuRemote.1.3.0.bin`), or flash
   `firmware/CardputerRokuRemote_0x0.bin` (8MB full-flash image) at `0x0`.
2. First boot walks through WiFi setup same as CardputerRadio.
3. The screen will prompt you to press `n` to find the Roku on the network.
   It tries SSDP auto-discovery first; if nothing answers (some hotspots
   don't pass multicast, and some older Rokus never reply), it scans the
   local /24 for port 8060 and confirms the hit is a Roku. Only if both
   fail does it ask you to type the IP (Settings > Network > About on the
   Roku shows it).

Both WiFi and the Roku itself can be switched at any time, not just during
setup - `w` always reopens the WiFi picker, `n` always re-runs Roku
discovery/entry, even mid-use.

## Controls

The screen shows a purple D-pad + OK button, with Back/Home/Options below it
and Rewind/Play/FF/Replay along the bottom - buttons flash white on a
confirmed send, red if the Roku didn't respond.

| Key | Action |
|---|---|
| `;`/`u`  `.`/`d`  `,`/`l`  `/`/`r` | Up / Down / Left / Right (D-pad) - hold to repeat |
| Enter | Select (OK) |
| Backspace (Del) | Back |
| `h` | Home |
| `o` | Options (`*`) |
| `i` | Instant Replay |
| `[` | Rewind |
| `f` | Fast Forward |
| `p` | Play / Pause |
| `w` | Switch WiFi network |
| `n` | Find / switch Roku device |
| Tab | Typing mode (see below) |

### Typing mode

Press **Tab** on the remote screen to type straight into a Roku search box
or on-screen keyboard - each key is sent as an ECP `Lit_` keypress. The
Cardputer shows what's been sent (red if a character didn't reach the
Roku). In this mode **Del** sends Backspace, **Enter** sends Enter, and
**Tab** returns to the remote.

No separate volume/mute keys - the real RC-AL2 doesn't have them either,
since a plug-in streaming stick has no volume control of its own.

## Notes

- Every keypress opens a fresh short-lived TCP connection to
  `http://<roku-ip>:8060/keypress/<Key>` - the same pattern the official app
  and every other ECP client use. Expect ~100-300ms latency per press on a
  normal home network.
- A full-screen banner replaces the remote UI whenever WiFi is disconnected
  or no Roku has been set yet, telling you exactly which key to press - the
  normal screen stays button-only, no IP addresses or status text cluttering
  it during regular use.
- No external Arduino libraries required - built entirely on the ESP32 core
  (WiFi, WiFiUdp, Preferences).
