# FAQ

Answers for Mocktail Plus. Paths are those of the Plus package; a custom
`$XDG_CONFIG_HOME` or `$XDG_STATE_HOME` replaces `~/.config` or
`~/.local/state`.

- [How do I enable Discord RPC?](#how-do-i-enable-discord-rpc)
- [Where can I find logs for a bug report?](#where-can-i-find-logs-for-a-bug-report)
- [How do I start Roblox without the settings window?](#how-do-i-start-roblox-without-the-settings-window)
- [Signing in ends with "Load generic challenge failed". What now?](#signing-in-ends-with-load-generic-challenge-failed-what-now)
- [How do I buy Robux?](#how-do-i-buy-robux)
- [Chat text is invisible while I type](#chat-text-is-invisible-while-i-type)
- [Why does the game use XWayland, or native Wayland, on my NVIDIA card?](#why-does-the-game-use-xwayland-or-native-wayland-on-my-nvidia-card)
- [Why is ETC2 kept, and what does MOCKTAIL_ADVERTISE_ETC2=0 do?](#why-is-etc2-kept-and-what-does-mocktail_advertise_etc20-do)
- [Which of my settings override Roblox's own in-game settings?](#which-of-my-settings-override-robloxs-own-in-game-settings)
- [Can I run multiple Roblox instances?](#can-i-run-multiple-roblox-instances)
- [Can I play in VR?](#can-i-play-in-vr)

## How do I enable Discord RPC?

RPC is disabled by default. In the settings window, open **Integrations**,
turn on **Discord Rich Presence** and press **Save**.

Or edit `~/.config/mocktail/config.yaml` and set `enabled` to `true` under
`integrations.discord_rpc`. If the block is missing, add it under the
existing `integrations` section:

```yaml
integrations:
  discord_rpc:
    enabled: true
```

Open Discord Desktop and start Mocktail. Turn it off the same way.

## Where can I find logs for a bug report?

Mocktail saves logs automatically. Reproduce the issue, close Mocktail, and
attach `~/.local/state/mocktail/logs/latest.log`. The settings window opens
the logs folder from its main menu and from the **About** page, which can
also copy diagnostic info for the report.

## How do I start Roblox without the settings window?

Use the desktop entry's **Play now** action or `mocktail --play`, or turn
off **Show this window on start** on the **Advanced** page
(`launcher.show_on_start: false`). Website joins never show the window. To
open it again, use the **Mocktail Settings** action or `mocktail --launcher`.

## Signing in ends with "Load generic challenge failed". What now?

Roblox may answer a sign-in with a user name and password in its Android app
with a Google Play Integrity device check (challenge type `deviceintegrity`).
Nothing on Linux can pass it: Mocktail has no certified Android device to
show. In upstream Mocktail the check's page waits about a minute and then
shows "Load generic challenge failed".

Plus answers Roblox's integrity questions as "unavailable" (it makes up no
token), and when Roblox asks for the check anyway, it opens roblox.com's
sign-in page in a "Sign in to Roblox" window instead. Sign in there; Mocktail
then restarts by itself and Roblox starts signed in. The session log shows
such a check as `[webview] Roblox asked for deviceintegrity verification,
which Linux cannot provide; opening website sign-in instead`.

Other ways in, on the settings window's **Accounts** page:

- **Sign in on the Roblox website** signs in on roblox.com before Roblox
  starts and saves the account.
- **Sign in inside Roblox** starts Roblox signed out so you can use Quick
  Log In with a device where you are already signed in.
- **Sign-in method: Website window** (`account.sign_in: browser`) opens
  Mocktail's roblox.com window when Roblox starts without a working session
  (website joins excepted).

## How do I buy Robux?

In your web browser. Mocktail has no Google Play Billing, so it cannot buy
inside the app. Choosing a Robux package on Roblox's Robux page opens
`https://www.roblox.com/upgrades/robux` in your default browser, and the page
says "Opened the Robux page in your web browser. Finish your purchase
there." A purchase prompt inside an experience opens the same page, shows a
desktop notification and tells the game nothing was bought; after buying
Robux in the browser, try the purchase in the experience again.

The browser uses its own roblox.com sign-in, so sign in there with the same
account. If no browser can be opened, the page says "Couldn't open your web
browser. Buy Robux at roblox.com/upgrades/robux."

## Chat text is invisible while I type

Fixed. Text typed into text boxes such as the chat bar could stay
invisible in the Mocktail 1.0.4 release. Upstream fixed this on `main` after
1.0.4 (commits `0548ebd`, `1c11336` and `b37497d`), and Plus is built from
that `main`, so chat text shows while you type. Plus also runs each call into
Roblox's TextBox code in its own JNI local frame, as Android does, and polls
the text box position at most once every 50 ms while it has focus. If text
still does not show, attach `latest.log` to a bug report.

## Why does the game use XWayland, or native Wayland, on my NVIDIA card?

NVIDIA's Wayland presentation is reliable only with explicit sync. Upstream
Mocktail always runs NVIDIA cards with direct Vulkan through XWayland on a
Wayland session that has XWayland. Plus uses native Wayland automatically
when the driver is 555 or newer with explicit sync left on
(`__NV_DISABLE_EXPLICIT_SYNC` unset or `0`), there is no Intel or AMD card
beside the NVIDIA one, the compositor offers explicit sync, and either the
compositor is Hyprland or the game does not wait for the display (vertical
sync off, or Automatic with an unlimited frame rate). Otherwise it keeps
XWayland.
The full list is in the README's [NVIDIA](README.md#nvidia) section.

To see what was chosen and why, look at the Display page's **Display
server** row, or for a `[window] NVIDIA` line in `latest.log`. To choose
yourself, set **Display server** to Wayland or X11 (`display.server: wayland`
or `x11`).

## Why is ETC2 kept, and what does MOCKTAIL_ADVERTISE_ETC2=0 do?

NVIDIA's Vulkan driver has no ETC2 textures, so Mocktail tells Roblox ETC2
is supported and decodes those textures on the CPU into uncompressed ones.
`MOCKTAIL_ADVERTISE_ETC2=0` stops advertising ETC2; Roblox then uses DXT/BC
textures, which NVIDIA supports natively. That saves video memory, but in a
test it was much slower:

| Same place and server | ETC2 (default) | `MOCKTAIL_ADVERTISE_ETC2=0` |
|---|---|---|
| Main menu | 113–123 fps | 53–56 fps |
| In game | 158–164 fps | about 62 fps |
| CPU use | about 2.3 cores | about 3.7 cores |
| Video memory of the game | about 1165 MiB | about 660–700 MiB |

(Roblox 2.738.1397, Greenville RP, RTX 3060 Laptop GPU, driver 615.71.09,
Hyprland.) The textures looked the same. The extra work with DXT/BC is on
Roblox's own render thread inside `libroblox`, and it stays with Mocktail's
performance preset off (123 fps with ETC2 against 38 fps with DXT/BC). So
ETC2 stays advertised by default. Try `MOCKTAIL_ADVERTISE_ETC2=0` only if
video memory is what limits you.

## Which of my settings override Roblox's own in-game settings?

The settings window shows it. A row whose value replaces one of Roblox's own
settings has an **Overrides Roblox setting** badge, and its info button says
what is overridden and how to give it back. The **Advanced** page lists all
of them under **What Mocktail decides and what Roblox decides** (the
Performance page links there). Depending on your settings these are:

- Roblox's **Graphics Quality** slider, while Mocktail's performance preset
  forces a level (`engine.graphics_quality`, see the
  [README](README.md#configuration)).
- Anti-aliasing, textures and level of detail: the performance preset
  (`performance.physics_worker_mode: throughput`, the default, or `auto`
  with `multithreaded_rendering`) turns MSAA off, caps the texture budget at
  128 MB and uses low-end level of detail.
- Roblox's **Maximum Frame Rate**, when `graphics.frame_rate_limit` sets a
  target.
- Roblox's **Theme**, when `appearance.theme` is not `roblox`.
- Roblox's **Fullscreen** setting, when `display.start_mode` is not
  `remember`.
- Voice chat, when the microphone is disabled.
- The device and interface, through the device profile (the PC profile
  shows Roblox's desktop interface).

The same group shows what is left to Roblox (volume, camera, chat and
anything not overridden) and the client settings Mocktail sets at every
start, such as Roblox's crash uploads being off.

## Can I run multiple Roblox instances?

No. Mocktail runs one Roblox client at a time, and Plus does not change
that; it can save several accounts, but plays one at a time. Multi-instance
launching would mean working around Roblox's restrictions on running several
clients. The
[Roblox Terms of Use](https://en.help.roblox.com/hc/en-us/articles/115004647846-Roblox-Terms-of-Use)
prohibit bypassing technical protections, and violating the terms can get
your account suspended.

Bloxstrap brought the option back in
[v2.9.0](https://github.com/bloxstraplabs/bloxstrap/releases/tag/v2.9.0)
with a "use at your own risk" warning, then removed it in
[v2.10.0](https://github.com/bloxstraplabs/bloxstrap/releases/tag/v2.10.0)
after Roblox added measures against running multiple clients.

## Can I play in VR?

VR is experimental and lives on upstream's `vr` branch, which does not have
the Plus changes. Install the [build dependencies](README.md#building-from-source),
then build that branch:

```bash
git clone --recurse-submodules --branch vr https://github.com/komaruworld/mocktail.git mocktail-vr
cd mocktail-vr
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMOCKTAIL_ENABLE_VR=ON
cmake --build build -j4
```

Start WiVRn or SteamVR/ALVR, connect your headset, then run:

```bash
./build/mocktail -vr
```
