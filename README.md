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

1. Install via the Launcher (`downloads/CardputerRokuRemote.1.1.0.bin` on
   the SD card, already copied there).
2. First boot walks through WiFi setup same as CardputerRadio.
3. The screen will prompt you to press `n` to find the Roku on the network
   (tries auto-discovery first via SSDP, falls back to typing in the IP
   address if that doesn't find it - Settings > Network > About on the Roku
   shows its IP).

Both WiFi and the Roku itself can be switched at any time, not just during
setup - `w` always reopens the WiFi picker, `n` always re-runs Roku
discovery/entry, even mid-use.

## Controls

The screen shows a purple D-pad + OK button, with Back/Home/Options below it
and Rewind/Play/FF/Replay along the bottom - buttons flash white on a
confirmed send, red if the Roku didn't respond.

| Key | Action |
|---|---|
| `;`/`u`  `.`/`d`  `,`/`l`  `/`/`r` | Up / Down / Left / Right (D-pad) |
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
