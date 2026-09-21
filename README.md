# STAR

```
  .-')    .-') _      ('-.     _  .-')
 ( OO ). (  OO) )    ( OO ).-.( \( -O )
(_)---\_)/     '._   / . --. / ,------.
/    _ | |'--...__)  | \-.  \  |   /`. '
\  :` `. '--.  .--'.-'-'  |  | |  /  | |
 '..`''.)   |  |    \| |_.'  | |  |_.' |
.-._)   \   |  |     |  .-.  | |  .  '.'
\       /   |  |     |  | |  | |  |\  \
 `-----'    `--'     `--' `--' `--' '--'
```

Steam API emulator. Drops in as `steam_api.dll` / `steam_api64.dll` and pretends to be Steam. Your game never knows the difference.

---

## what it does

- Implements the full Steamworks SDK: user, friends, stats, achievements, apps, networking, UGC, inventory, remote storage, the whole thing
- In-game overlay (Shift+Tab) built on ImGui, hooks whatever graphics API your game uses
- Animated achievement notifications slide in from the bottom-right when you unlock stuff
- Saves stats and achievements locally so they persist across sessions
- Reads all config from a `STAR/` folder next to the DLL

---

## setup

Drop the DLL next to the game executable. Create a `STAR/` folder there with your config files.

**Minimum viable setup:**

```
game.exe
steam_api64.dll   <- this thing
STAR/
  identity.star
  game.star
```

**`STAR/identity.star`**
```ini
display_name = YourName
xuid = 76561198000000000
locale = english
```

**`STAR/game.star`**
```ini
beta = false
branch = public
dlc.unlock_all = false
```

**`STAR/overlay.star`** (optional, overlay is on by default)
```ini
enabled = true
```

App ID is read from `STAR/steam_appid.txt` (or `steam_appid.txt` in the game directory as a fallback).

---

## achievements

Put an `achievements.json` in your `STAR/` folder:

```json
[
  {
    "name": "ACH_WIN_ONE_GAME",
    "displayName": "Winner",
    "description": "Win your first game.",
    "icon": "icons/ach_win.png",
    "hidden": "0"
  }
]
```

Icon paths are relative to the `STAR/` directory. You can unlock and reset achievements live from the overlay.

**Achievement sound** (optional): drop an MP3 at `STAR/Sounds/achievement.mp3` and it plays automatically whenever an achievement is unlocked or test-fired from the overlay.

---

## overlay

`Shift+Tab` to toggle. Slides in from the right.

Shows your account info, achievement progress, and a scrollable list of every achievement with Unlock/Reset buttons per row. Hit "Test notify" to fire a fake achievement notification so you can see how it looks without actually unlocking anything.

Supports DX7, DX8, DX9, DX10, DX11, DX12 (x64), OpenGL, Vulkan, and GDI. Hooks frame presentation via MinHook, no game cooperation needed. Hostile titles (crashes, device loss, fence timeouts) automatically fall back to a separate external overlay window; STAR retries hooks after a few launches with exponential backoff and heals itself when a retry succeeds. See `SETUP.md` for the `mode`, `fallback_count`, and `fallback_level` keys.

The first qualifying presentation on the main game window locks the game API for
that process. Other APIs pass through without changing overlay rendering, input
ownership, or screenshots. Resizing and device/context recreation rebuild only
the selected backend. GDI requires two seconds of sustained drawing before it
can claim selection, to avoid treating incidental startup painting as the renderer.
External mode keeps this game API selection but draws its separate window with
its own DX9/DX11 device. Unity games loaded after graphics initialization recover
their exact DX12 swapchain/queue or Vulkan device/queue through Unity's native
plugin interface. Vulkan hooks also cover instance/device function lookups.
If the required ownership information remains unavailable, auto mode uses the
external path; STAR does not guess GPU handles.

---

## building

Requires MSVC and CMake 3.16+. All dependencies pull in automatically via FetchContent.

**Both architectures, packaged to `dist/`:**
```
.\build.ps1
```

**x64 (steam_api64.dll):**
```
cmake --preset x64
cmake --build --preset x64-release
```

**x86 (steam_api.dll):**
```
cmake --preset x86
cmake --build --preset x86-release
```

Output lands in `build/x64/Release/steam_api64.dll` or `build/x86/Release/steam_api.dll`. `cmake --install build/x64 --config Release --prefix dist` copies it to `dist/`.

---

To run the standalone regression checks:

```powershell
cmake --preset x64 -DSTAR_BUILD_TESTS=ON
cmake --build --preset x64-release
ctest --test-dir build/x64 -C Release --output-on-failure
```

Use the x86 preset and build directory for the 32-bit checks. These tests cover
queued I/O, PNG encoding/decoding, notification ownership, draw reuse, bulk
achievements, pixel/GDI helpers, and API selection across every backend. Native
DLL tests also interleave DX9, DX10, DX11, DX12, OpenGL, Vulkan, and GDI with
helper APIs and exercise resize/reset/context recreation, Vulkan function
lookups, and Unity graphics objects created before DLL loading. The external
overlay test grows and shrinks real GPU/readback surfaces offscreen. Native checks skip
when the required graphics runtime is unavailable; they do not replace testing
inside a game.

## logs

`STAR/star.log` next to the DLL. Falls back to `%TEMP%/star.log` if that directory isn't writable. The log tells you what app ID loaded, how many achievements came in, and whether the overlay hooks fired correctly. Routine flat-API calls are omitted by default; set `STAR_TRACE_API=1` before launching the game to include them. Writes are buffered and flushed on the next log message after one second, or at `SteamAPI_Shutdown`.

---

## config files

All `.star` files are INI-format. On first load, STAR stamps a small ASCII art header into each one. That's intentional, don't delete it unless you want it back.

| file | what it controls |
|------|-----------------|
| `identity.star` | display name, Steam ID, language |
| `game.star` | app ID, DLC config, beta branch |
| `languages.star` | list of languages the game claims to support |
| `overlay.star` | enable/disable the overlay |
| `achievements.json` | achievement definitions |
| `Sounds/achievement.mp3` | sound played on achievement unlock (optional) |

---

## misc

- `SteamAPI_IsSteamRunning()` always returns `true`. You're welcome.
- Networking interfaces exist but don't actually network anything.
- Inventory and UGC are stubs, they won't crash but won't do much.
- Game server stuff returns success and does nothing. Fine for most games.
