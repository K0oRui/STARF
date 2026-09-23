# integrity bypass & proxy loaders

For games that hash-check their own files (anti-tamper / DRM self-integrity) and refuse to run when they spot a modified `steam_api64.dll` or a patched `.exe`. This is the part most people get wrong, so read it before you start swapping DLLs.

Most games do **not** need any of this. Drop STAR in as `steam_api64.dll` and you're done (see `SETUP.md`). Only reach for this when the game crashes, exits silently on launch, or complains about modified files *after* you replace the Steam DLL.

---

## how it works

Two moving parts:

**1. The redirect hooks.** Built into STAR in `src/core/integrity_hooks.cpp`

STAR exports `STAR_install_integrity_hooks`. Once called, it hooks the file APIs (`CreateFileW/A`, `GetFileAttributesW/A`, `GetFileAttributesExW/A`) and watches for the game opening or stat-ing three things:

- its own `.exe` (matched by name)
- `steam_api64.dll`
- `steam_api.dll`

When it sees one of those and a `<file>.bak` sits next to it, STAR redirects the call to the `.bak`. Everything else passes through untouched.

So the integrity check reads the pristine `.bak` copy, hashes it, and passes. The process itself runs the modified file in memory.

The redirect is opt-in per file. No `.bak`, no redirect. Only lay down a `.bak` for the files the game actually checks.

The hooks only work if they are installed before the game runs its check.

STAR installs them itself from its own `DllMain`. As soon as `steam_api64.dll` loads, it spins up a background thread and installs the hooks. No extra files, no proxy. For the common case (games that statically import `steam_api64.dll`, so it loads at process init) this is all you need: **one DLL, done.**

**2. The proxy loaders.** `version.dll` and `baselib.dll`, fallback only.

Sometimes STAR itself loads too late. A game that calls `LoadLibrary` on `steam_api64.dll` at runtime, after its startup check already ran, misses the window. In that case STAR's own `DllMain` fires too late to matter.

The proxy loaders fix the timing by loading earlier than the check. The game pulls them in at process init. On load each spins up a thread, finds `steam_api64.dll` even in subfolders, calls `LoadLibrary` on it, then calls `STAR_install_integrity_hooks`. Then it forwards its real exports elsewhere, so the game sees a normal working DLL.

`STAR_install_integrity_hooks` is idempotent, so STAR and a proxy can both call it. Whoever gets there first wins. The rest are no-ops.

The same install also hooks il2cpp `SteamManager.Awake` in `GameAssembly.dll` titles and forces `enable` on. It also bypasses the relay status crash. Mono builds still need the `UNITY_PATCH.md` binary patch.

Only add a proxy if the single DLL is not enough. That means the check still fails and `star.log` shows no redirect lines.

---

## which proxy to use

| proxy | use when | forwards to |
|-------|----------|-------------|
| `version.dll` | generic - any game that imports `version.dll` (most do) | `C:\Windows\System32\version.dll` |
| `baselib.dll` | Unity **il2cpp** games (they ship `baselib.dll`), or when `version.dll` loads too late | `baselib_original.dll` (you rename the original) |

Start with `version.dll`. Only move to `baselib.dll` if the exe check still fires. Unity il2cpp titles load `baselib.dll` before almost anything else, so it hooks earlier.

You only need **one** of them.

> Both proxies load `steam_api64.dll` or `steam_api.dll` by name and scan subfolders if it is not in the root. Standard proxies are 64-bit. For 32-bit games, build a 32-bit proxy that finds `steam_api.dll`.


---

## deploy: version.dll

1. Build `version.dll` (the `version_proxy` target).
2. Drop it in the game folder, next to the exe.
3. Back up the files the game checks by copying them to `.bak`:

```
copy steam_api64.dll steam_api64.dll.bak   :: BEFORE you overwrite it with STAR
copy game.exe        game.exe.bak          :: only if the exe is checked / patched
```

   Order matters. Make `steam_api64.dll.bak` from the original Steam DLL, then replace `steam_api64.dll` with STAR.
4. Drop STAR in as `steam_api64.dll` (plus its `STAR/` folder - see `SETUP.md`).

Final layout:

```
game.exe
game.exe.bak            <- pristine, only if exe is checked
version.dll             <- proxy loader
steam_api64.dll         <- STAR
steam_api64.dll.bak     <- pristine original Steam DLL
STAR/
  ...
```

`version.dll` forwards to the real one in `System32`, so nothing else breaks.

---

## deploy: baselib.dll

Same idea. Unity's `baselib.dll` is a real dependency with real exports, so you cannot forward to a system copy. You forward to the original, renamed.

Read the regen bit below first. Baselib exports differ per Unity version. Unless the game matches the one checked in, regen before you build, or the game will not start.

1. Regenerate + build (see below), so you have a matching `baselib.dll`.
2. Rename the game's own `baselib.dll` > `baselib_original.dll`.
3. Drop your built `baselib.dll` in as `baselib.dll`.
4. Lay down `.bak` backups exactly as with version.dll above.

Layout. The proxy finds `steam_api64.dll` on its own, even in subfolders, so leave it where the game keeps it.

```
[Game Root Directory]
├── baselib.dll             <- proxy loader
├── baselib_original.dll    <- the game's real baselib, renamed
└── (any other game files...)

[Plugins Directory] (typically GameName_Data/Plugins/x86_64/)
├── steam_api64.dll         <- STAR (emulator)
└── steam_api64.dll.bak     <- pristine original Steam DLL
```


### the export list is version-specific - regenerate it

The proxy forwards a list of mangled il2cpp symbols to `baselib_original.dll`. That list lives in `src/proxy/baselib.def`, and it came from one specific `baselib.dll`. A different Unity version exports a different set of symbols. If the game's `baselib.dll` exports something the `.def` doesn't forward, the game fails to start with an unresolved-import error.

For a Unity game with a different baselib, regen the `.def` from that game's actual DLL, then build. `tools/gen_baselib_proxy.py` does both, pure Python with no dumpbin and no dev prompt.

```
python tools/gen_baselib_proxy.py "C:\Games\Whatever\GameName_Data\Plugins\x86_64\baselib.dll" --build
```

It reads the export table straight out of the DLL, rewrites `src/proxy/baselib.def`, and builds the `baselib_proxy` target. Drop `--build` if you just want the `.def` regenerated and will build yourself.

The checked-in `src/proxy/baselib.def` is a sane default from a common Unity build. If it matches your game, build straight away. When in doubt, regen. It is one command and idempotent.

Ordinal-only exports are not handled. That is a nameless baselib, which normal Unity builds do not do. The tool errors instead of guessing.

---

## verify it worked

`STAR/star.log` shows the redirects as they happen:

```
CreateFileW: Redirecting ...\steam_api64.dll to ...\steam_api64.dll.bak
GetFileAttributesW: Redirecting ...\game.exe to ...\game.exe.bak
```

No redirect lines but the check still fails means the hooks installed too late. Try the other proxy, or confirm the `.bak` files sit next to the real ones.

---

## common mistakes

**`.bak` made from the wrong file.** `steam_api64.dll.bak` has to be the *original* Steam DLL, not a copy of STAR. If you back up after overwriting, you're serving the game the modified hash and the check still fails.

**Game won't start after adding `baselib.dll`.** Export list doesn't match the game's Unity version. Regenerate it (above).

**Nothing gets redirected.** No `.bak` next to the real file means the hooks do nothing by design. Check spelling and location.

**Redirect fires but check still fails.** The game hashes a file STAR does not watch. Only the exe, `steam_api64.dll`, and `steam_api.dll` are covered. Anything else needs a new match rule in `resolve_redirect_w` and `resolve_redirect_a`.
