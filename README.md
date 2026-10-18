# Swiftify

a spotify player for desktop, built with Qt Quick on top of a Rust core that
wraps [librespot](https://github.com/librespot-org/librespot). it follows
Material 3 Expressive (heavy inspo from end4) and has an immersive player,
synced lyrics, visualizers, an equalizer, and local files with local playlists
where u can edit the tracks metadata.
![Alt text](/assets/home.png)
this apps purpose is to replace spotifys heavy webview client, but its also mostly eyecandy lol
it runs significantly lighter than the spotify client.

**EVEN WITH THE EYECANDY TURNED ON....** it is lighter than spotify + spotifys renderers both on cpu AND ram!!!!!

id recommend turning the visualizers and background off if the app runs in the background tho lol

***anyways runs on linux (made for wayland and hyprland) macos, and windows.***

## previews
first image shows equalizer, tempo and pitch settings. second picture shows how the immersive player and the visualizer/lyrics look
<table border="0">
  <tr>
    <td><img src="/assets/eq.png" alt="equalizer and tempo" width="400"/></td>
    <td><img src="/assets/immersive.png" alt="immersive player with visualizers" width="400"/></td>
  </tr>
</table>

> ### Note
> i will probably not be maintaining it, if you are interested in taking over this project you can send me an email

- [Before you start](#before-you-start)
- [Building from source](#building-from-source)
  - [Build requirements](#build-requirements)
  - [macOS](#macos)
  - [Linux](#linux)
  - [Windows](#windows)
- [Command line](#command-line)

## Before you start

- **spotify premium.** playback goes through librespot, and that only works with premium.
- **a spotify client id.** your library, playlists and search come from the spotify
  web api through your own developer app. the first launch walks u through it:
  make an app in the [spotify developer dashboard](https://developer.spotify.com/dashboard),
  add `http://127.0.0.1:8898/login` as a redirect uri, and paste the apps client id
  into swiftify. you dont need the client secret.

### "spotify could not load this track" (soloist)

spotify refuses librespot the keys to decrypt songs on newer accounts (librespot's
logs say `error audio key 0 1`), so every song fails. on linux swiftify can play
spotify's songs through [spotify soloist](https://developer.spotify.com/documentation/soloist)
instead, spotify's own connect client, which isnt affected.

1. install soloist from its [getting started guide](https://developer.spotify.com/documentation/soloist/tutorials/getting-started)
   (`~/.local/bin/soloist` is fine)
2. make a soloist api key in the [developer dashboard](https://developer.spotify.com/dashboard/soloist)
   and put it in `~/.config/soloist/api-key` (or `SOLOIST_API_KEY`)
3. start swiftify. it starts soloist itself, as a connect device called "Swiftify".
   the first time, pick "Swiftify" once in the devices list of the spotify desktop app on
   the same computer (or the phone app), so soloist is signed in
4. press play in swiftify

with soloist, swiftify makes a sink called "Swiftify (Soloist)" for soloist to play into,
and plays that on through its own output, so the equalizer and the visualizers still work.
this needs pipewire, under pulseaudio alone soloist plays straight to the speakers.
`SWIFTIFY_SOLOIST_ROUTE=off` does that on purpose. tempo and pitch only work on your own
music files, and crossfade waits for each song to end. soloist builds expire after 90 days,
download a new one when it says so.
`SWIFTIFY_PLAYBACK=librespot swiftify` goes back to librespot alone.

if spotify on your phone doesnt list "Swiftify" for step 3, a firewall (ufw) is probably
dropping the pairing connection; pair from the spotify desktop app on the same computer.

## Building from source

### Discord Rich Presence

**Settings → Account → Discord Rich Presence** optionally shares the current track,
artist, public Spotify cover and progress with the running Discord **desktop** app.
It is off by default. Activity requests use the listening type and the name
**Swiftify**. A pause keeps the track visible with **Paused**, without
running timestamps; resuming restores progress. Disabling RPC clears the activity.
This is Rich Presence, not Spotify's official linked-account activity; it does not
enable Listen Along. Some Discord versions may prefer the application's registered
name over the requested activity name; in that case, name your registered
application Swiftify in the Developer Portal. Discord's own activity privacy
settings must also allow displaying activity.

This build has no shared Discord application registered for Swiftify yet. Create
an application named Swiftify at <https://discord.com/developers/applications> and
paste its numeric **Application ID** into the field, then press Enter or leave the
field. This is a public identifier, **not** a bot token, client secret, or Spotify
Client ID. Applications such as Sonora ship their developer's shared Application ID,
which is why their users don't need this step; do not reuse another app's ID.

RPC uses local IPC and automatically reconnects when Discord starts again. There
is no additional SDK or dependency. Local file paths and private cover URLs are
never included. The RPC protocol has automated tests against a fake local Discord
server; the transport fixtures support Unix sockets and Windows named pipes.
Testing the real Discord status requires a valid application ID and Discord.

On Linux, native sockets and common Flatpak/Canary/PTB/Vesktop/Snap layouts are
searched under the runtime and temporary directories. Sandbox permissions still
need to allow communication, and clients such as Vesktop need their RPC integration
enabled. Account settings distinguish missing sockets, permission failures, handshake
timeouts and Discord rejection codes instead of silently retrying every failure.
For transport diagnostics, launch with `QT_LOGGING_RULES="swiftify.discord.debug=true"`;
logs contain only transport/error codes, not activity payloads or credentials.

### Interface language

In **Settings → Appearance → Language**, choose **System**, **English**, or
**Русский**. The preference is saved and applies immediately, without restarting.
Track metadata, playlist names, search queries and custom preset names are not translated.

Translations use Qt `qsTr` / `qsTranslate` and an embedded UTF-8 catalog at
`translations/ru.json`; no additional Linguist tools are required. Russian plural
entries contain three forms (one, few, many).

### Windows media widgets

Windows builds with a sufficiently recent SMTC SDK publish track duration and
position through the media-session timeline, not just taskbar progress. Widgets
using the Windows media-session API can read these values; other/custom widget
integrations may need their own support. Runtime verification on Windows is
still required.

### Build requirements

- Qt 6.9 or newer: Qt Base, Qt Declarative and Qt Shader Tools, and on linux Qt WebSockets (for soloist)
- QtKeychain and QCoro 0.10 or newer, both built for Qt 6
- CMake 3.24 or newer, Ninja, and a C++20 compiler
- Rust (stable, from [rustup](https://rustup.rs))

the first cmake run needs internet: it pulls [Corrosion](https://github.com/corrosion-rs/corrosion)
(which builds the rust core as part of the cmake build) and two fonts, Material Symbols
Rounded and Google Sans Flex, each checked against a pinned hash.

the builds below skip the tests with `-DBUILD_TESTING=OFF`. for a manual CMake
build, drop that and run `ctest --test-dir build` if u want them.

### macOS

u need [homebrew](https://brew.sh) and apple's command line tools (`xcode-select --install`, or full xcode). then:

```sh
brew install qtbase qtdeclarative qtshadertools qtkeychain qcoro6 cmake ninja
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
source "$HOME/.cargo/env"

git clone https://github.com/shredos67/swiftify.git
cd swiftify
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build
ditto build/swiftify.app /Applications/Swiftify.app
```

it's signed ad hoc, so no developer account needed.
with full xcode installed the icon gets compiled with `actool` and picks up macos 26's glass,
dark and tinted looks; with just the command line tools it's built from the PNGs.

### Linux

on arch either build a proper package, which installs to `/usr`:

```sh
git clone https://github.com/shredos67/swiftify.git
cd swiftify/packaging/arch
makepkg -si
```
or use cmake:

```sh
# replace with your package manager if not on arch
sudo pacman -S --needed git qt6-base qt6-declarative qt6-shadertools qt6-wayland qt6-websockets qtkeychain-qt6 qcoro libpulse alsa-lib openssl cmake ninja rust pkgconf

git clone https://github.com/shredos67/swiftify.git
cd swiftify
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build
cmake --install build
```

on debian/ubuntu (or derivatives with Qt 6.8), use the build script that handles
compatibility patches automatically:

```sh
sudo apt-get install git qt6-base-dev qt6-declarative-dev qt6-shader-baker \
    qt6-shadertools-dev qt6-wayland qt6-websockets-dev qtkeychain-qt6-dev qcoro-qt6-dev \
    libpulse-dev libasound2-dev libssl-dev cmake ninja-build pkg-config
# install rust from https://rustup.rs if you dont have it

git clone https://github.com/shredos67/swiftify.git
cd swiftify
./tools/build-debian.sh
```

or with `--install-deps` to install the dependencies automatically:

```sh
git clone https://github.com/shredos67/swiftify.git
cd swiftify
./tools/build-debian.sh --install-deps
```

anywhere else, install the same reqs (Qt 6.9+ with D-Bus, OpenGL and Wayland, QtKeychain
and QCoro for Qt 6, libpulse, plus the ALSA, OpenSSL and pkg-config dev packages), then
run the cmake commands above. if you have Qt 6.8, use `tools/build-debian.sh` which
applies the necessary compatibility patches.

`cmake --install` drops `swiftify` in `~/.local/bin` (make sure that's on your `PATH`)
and the desktop entry, icons and fonts in `~/.local/share`. 

your login gets saved in the secret service through QtKeychain, so u need something
providing one: gnome-keyring, kwallet or keepassxc.

on linux it also hooks into MPRIS (media keys, `playerctl`, shell media widgets),
logind (pauses before the machine sleeps), pipewire/pulseaudio device changes, and
the tray. `-DSWIFTIFY_DESKTOP_INTEGRATION=OFF` turns those off.

### Windows

**because most Windows users cant even use their own OS, i made a script that handles building so it's as simple as possible :)**

first install Qt 6.9+ with *Qt Shader Tools*, CMake and Ninja from the Qt online installer,
Rust from [rustup](https://rustup.rs), and Git. choose either Qt for *MSVC 2022 64-bit*
with Visual Studio 2022 17.1+ (or Build Tools) and its *Desktop development with C++*
workload, or Qt for *MinGW 64-bit* with its matching MinGW toolchain.

then run this in Windows PowerShell:

```powershell
git clone https://github.com/shredos67/swiftify.git
Set-Location swiftify
.\tools\build-windows.ps1
```

for a different Qt kit, toolchain location or install directory, pass overrides:

```powershell
.\tools\build-windows.ps1 -QtPath 'C:\Qt\6.11.2\msvc2022_64'
.\tools\build-windows.ps1 -QtPath 'C:\Qt\6.9.3\mingw_64' -MingwPath 'C:\Qt\Tools\mingw1310_64\bin' -InstallPrefix "$env:LOCALAPPDATA\Programs\Swiftify"
```

adjust the example paths to your installed versions, MinGW must match your Qt
kit, not just the example folder name. 

**the toolchain decides one feature:** the Windows media controls (the media overlay,
lock screen and media keys showing what's playing) need `SystemMediaTransportControls`
in the compiler's Windows headers.

| toolchain | media controls |
|---|---|
| MSVC 2022 | yes |
| Qt's MinGW 13.1 (`mingw1310_64`, mingw-w64 runtime v11) | no, everything else works |
| a newer mingw-w64 (tested: GCC 16.1, runtime v14) | yes |

when they're left out, cmake warns `building without the Windows media controls`.
for every feature, build with MSVC. a newer MinGW works too, but it isn't the one
your Qt kit was built with, so the script warns about the mismatch.

quit a running installed copy through its tray menu before updating it.

if PowerShell blocks the script, review it first, then run
`powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\build-windows.ps1`.
this applies only to that process, not your machine's execution policy.

start the installed app with:

```powershell
& "$env:LOCALAPPDATA\Programs\Swiftify\bin\swiftify.exe"
```

font downloads are hash-checked on every configure; on Windows, a failed CMake
download is retried with `curl.exe` using the system certificate store.

## Command line

only one swiftify runs at a time. launching it again just passes the command to the one
that's already running, so these work great as keybinds:

| flag | |
|---|---|
| `--immersive` | open the immersive player |
| `--play-pause` | play or pause |
| `--next` | next song |
| `--previous` | previous song |

on hyprland for example:

```ini
bind = SUPER SHIFT, M, exec, swiftify --immersive
bind = , XF86AudioPlay, exec, swiftify --play-pause
```
