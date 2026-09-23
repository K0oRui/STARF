# unity steam patching

Some Unity builds ship Steamworks.NET but with Steam turned off. Common in DRM-free releases on itch.io, GOG, or standalone launchers.

In those builds `SteamManager` has `enable = false`. It returns early in `Awake()`, so the game never calls `SteamAPI.Init()` and never loads `steam_api64.dll`. No achievements, no STAR overlay.

Patch the game to force Steam on.

This patch targets Mono builds, where the code ships as `Assembly-CSharp.dll`. Il2cpp builds load `GameAssembly.dll` instead. STAR hooks those at runtime and forces `enable` on, so try STAR unpatched first on an il2cpp game.

---

## how it works

Standard Steamworks.NET games use a script called `SteamManager`. At startup its `Awake()` checks the `enable` field:

```csharp
protected virtual void Awake() {
    if (!enable) {
        return;
    }
    ...
}
```

The C# ships compiled in a DLL like `Assembly-CSharp.dll`, so patch the compiled CIL bytes to skip the early return.

### CIL byte tweak

`if (!enable) return;` compiles to this CIL:

```il
ldarg.0             // 02
ldfld bool enable   // 7B [4-byte token]
brtrue.s skip_ret   // 2D 01
ret                 // 2A
```

Replace those 9 bytes with code that always skips the return:

```il
ldc.i4.1            // 17
brtrue.s skip_ret   // 2D 01
ret                 // 2A
nop, nop, nop, ...  // 00 00 00 00 00 (padding to match length)
```

Now `SteamManager.Awake()` keeps going. It calls `SteamAPI.Init()` and loads STAR's `steam_api64.dll`.

---

## automated patching

A script in the repo does this for you.

### prerequisite

Put your App ID in `steam_appid.txt` next to the game exe. Steamworks.NET needs it to init.

### usage

Pass the game folder or the assembly DLL directly:

```bash
python tools/patch_unity_steam.py "D:\Games\My Unity Game"
```

The script will:

1. Find the assembly DLL, like `Assembly-CSharp.dll`.
2. Compile a small C# helper with the built-in `csc.exe` to find the exact metadata token for `enable`. Token numbers differ per build.
3. Search and replace the byte pattern in the DLL.
4. Back up the original as `.bak`.

---

## manual patching

If you would rather patch by hand:

1. Open the Managed folder, usually `<GameName>_Data\Managed\`.
2. Open `Assembly-CSharp.dll` in dnSpy or ILSpy.
3. Find the `SteamManager` class and open `Awake`.
4. Right-click in `Awake` and pick `Edit Method Body`, in dnSpy.
5. Find the `enable` check at the top.
6. Change it to push constant 1 onto the stack, or drop the branch so it never hits `ret`.
7. Save the module.
