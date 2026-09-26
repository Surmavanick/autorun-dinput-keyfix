# autorun-dinput-keyfix

A tiny ASI plugin that makes **DirectInput keyboard input work under [Autorun](https://github.com/autorunhq/autorun)** (Wine on the Nintendo Switch) for games that read gameplay keys through DirectInput.

Autorun turns the Joy-Con into keyboard and mouse events ("Autorun's controls", `NAME.keys.txt`). Those events reach `WM_KEYDOWN`, `GetAsyncKeyState` and `GetKeyboardState`, but on the current runtime (test build 4, `nx-wow64-dynarec-248`, `wine-11.0-nx`) they never reach a DirectInput keyboard device. Games that drive their menus with window messages and their gameplay with DirectInput scan codes therefore work in the menus and are dead in-game. Need for Speed: Hot Pursuit 2 (2002, DirectInput 7) is the reproduction case; the runtime issue is tracked in [autorunhq/autorun#39](https://github.com/autorunhq/autorun/issues/39).

The plugin, loaded by [ThirteenAG's Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader), bridges the gap:

1. hooks `DINPUT.dll!DirectInputCreateA` in the game's import table,
2. patches `IDirectInput7A::CreateDevice/CreateDeviceEx` on the returned object,
3. for `GUID_SysKeyboard` devices patches `Acquire`, `GetDeviceState`, `GetDeviceData` and `SetCooperativeLevel`,
4. overlays the 256-byte DIK state (and synthesizes buffered `DIDEVICEOBJECTDATA` events) from `GetAsyncKeyState`, `GetKeyboardState` and, where the hook can be installed, `WM_KEYDOWN/WM_KEYUP`; virtual keys are mapped to DIK codes including the extended keys (arrows, Home/End, Insert/Delete, right Ctrl/Alt);
5. masks `Acquire` failures and writes everything to `scripts/NFSHP2KeyFix.log` next to the plugin.

Only `user32.dll` and `kernel32.dll` are used. No dinput.h needed.

## Files

| Path | What |
|---|---|
| `keyfix.c` | the plugin |
| `bin/NFSHP2KeyFix.asi` | prebuilt 32-bit DLL (MD5 in `bin/MD5SUMS`) |
| `harness.c` | desktop-Wine test program: imports `DirectInputCreateA` like a game, creates the keyboard device, injects keys with `keybd_event` and prints what DirectInput reports |
| `build.sh` | builds both with [zig](https://ziglang.org) (`zig cc -target x86-windows-gnu`); any i686 MinGW works too |
| `examples/NFSHP2.keys.txt` | Autorun per-game Joy-Con map used with NFS Hot Pursuit 2 |
| `logs/` | Autorun logs from the Switch: the race-without-input run, the working run, and the plugin's own log |

## Install (Need for Speed: Hot Pursuit 2 on Autorun)

1. Ultimate ASI Loader in the game folder, named after a DLL the game imports. For NFS HP2 the [HP2WSFix](https://github.com/ThirteenAG/HP2WSFix) package already ships it as `d3d8.dll`: rename it to `dinput.dll`, keep its `global.ini` and `scripts/` (this also gives you the widescreen fix).
2. Tell Wine to use it, in `switch/wine/registry/user.reg`:
   ```
   [Software\\Wine\\DllOverrides]
   "dinput"="native,builtin"
   ```
3. Copy `bin/NFSHP2KeyFix.asi` to `<game>/scripts/`.
4. Put `examples/NFSHP2.keys.txt` next to `NFSHP2.exe` and keep the game on Autorun's controls (`own-controls=0`; the game's own controls mode switches key injection off entirely). In Autorun A and B are always the mouse buttons; Minus is mapped to Enter for the menus.
5. Start the game. `scripts/NFSHP2KeyFix.log` shows the hooks and, every 3 seconds, which sources deliver keys.

Other DirectInput 7 games: the same DLL works unchanged as long as the game imports `DirectInputCreateA` from `dinput.dll` and uses the standard 256-byte keyboard format. For DirectInput 8 games change the import to `DINPUT8.dll` / `DirectInput8Create` in `keyfix.c` (`IDirectInput8A::CreateDevice` is slot 3 as well; there is no `CreateDeviceEx`).

## What the logs show

From `logs/NFSHP2KeyFix.log` (Switch, working run):

```
WH_GETMESSAGE hook on thread 4: FAILED (err 1)
CreateDevice(SysKeyboard) -> 00000000 dev=013333c4
Acquire(kbd 013333c4) -> 00000000
stats: ... frames with keys from msg=0 async=1084 kbstate=1092 original=0 | now=[1c ]
```

`original=0` means the real DirectInput device never reported a key during the whole session, while `GetAsyncKeyState` / `GetKeyboardState` saw them in more than a thousand frames. `SetWindowsHookExA(WH_GETMESSAGE)` fails on the runtime with error 1, so the message-hook source is unavailable there; the other two are enough.

On desktop Wine 10 (`harness.c`) the original device does see `keybd_event`-injected keys, but a virtual key sent without a scan code and without `KEYEVENTF_EXTENDEDKEY` arrives as `DIK_NUMPAD8` (0x48) instead of `DIK_UP` (0xC8), which is the second half of the problem for arrow keys.

## Build

```bash
ZIG=/path/to/zig ./build.sh
```

## License

MIT, see `LICENSE`.

## Also in this repo

### dinput8.dll (DirectInput 8 mouse + keyboard bridge)

`dibridge8.c` builds a drop-in `dinput8.dll` proxy for DirectInput 8 games (`bin/dinput8.dll`, exports via `dinput8.def`). It forwards `DirectInput8Create` to the system DLL and wraps the keyboard *and* mouse devices: the keyboard is fed like the ASI above, the mouse gets relative X/Y from `GetCursorPos` movement and buttons 0..2 from `VK_LBUTTON/VK_RBUTTON/VK_MBUTTON`, both as immediate state and as buffered events. Needs `"dinput8"="native,builtin"` under `[Software\\Wine\\DllOverrides]` in `switch/wine/registry/user.reg`. Log: `dinput8-bridge.log` next to the DLL. Verified on desktop Wine with Stronghold Crusader 2 (Havok Vision engine, buffered `GetDeviceData` for both devices); on the Switch the game itself did not get to a frame for other reasons (missing `glu32.dll`, `d3dx9_43.dll`, `D3DCompiler_43.dll` in the runtime), so treat the Switch side as untested.

### D3D8Fps.asi (on-screen FPS counter for Direct3D 8 games)

`d3d8fps.c` → `bin/D3D8Fps.asi`, loaded by the Ultimate ASI Loader from `scripts/`. It hooks `Direct3DCreate8` through the executable's import table, patches `IDirect3DDevice8::Present`/`Reset`, draws the frame rate as 7-segment digits in the top-left corner with `DrawPrimitiveUP` (only the touched states are saved/restored through a recorded state block, no D3DX, no fonts) and, optionally, **caps the frame rate**: `D3D8Fps.ini` next to it takes `Limit=30` (0 = off), `ShowCounter=1` and `NoVSync=1` (with a limit, the presentation interval is forced to IMMEDIATE so the pacing is exact instead of colliding with vblank; measured 150 frames per 5 s at `Limit=30`). Useful on Autorun where WineD3D has neither a HUD nor a limiter (Autorun's own frame limit only covers Vulkan/DXVK). Log: `D3D8Fps.log` with 5-second statistics. Tested with NFS Hot Pursuit 2.

Build everything: `ZIG=/path/to/zig ./build.sh`
