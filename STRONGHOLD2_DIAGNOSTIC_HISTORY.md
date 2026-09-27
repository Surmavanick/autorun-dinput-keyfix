# Stronghold 2 Steam Edition on Autorun/wine-nx

Last updated: 2026-09-28 after Switch probe run 8. This is the resume point for the next session.

## Current decision

Do exactly one more Switch run (run 9). If its synchronized menu readback and conditional workaround do not reveal or
fix the missing image, stop treating this as a game-configuration problem. The remaining work is wine-nx/wined3d
runtime debugging and a minimal upstream reproduction.

## Symptom and environment

- Nintendo Switch, Autorun test build 4, `wine-nx nx-wow64-dynarec-248`, Mesa 26.2.2.
- Stronghold 2 Steam Edition v1.5 starts and has audio. The loading image and ID3DXFont text can render, but frontend
  video/3D backgrounds and the game world are black.
- Current renderer is WineD3D/OpenGL through `nvc0`; the same symptom also occurred with DXVK/NVK.
- Game directory on SD: `switch/wine/drive_c/Stronghold 2/`.
- MTP base URI: `mtp://Nintendo_Nintendo_Switch_XAW00000000000/SD%20Card/`.

Already excluded: loader/runtime DLLs, Steam emulator, DXVK vs WineD3D, shader versions including fixed-function,
precompiled effects, 32-bit address space, strict Box64 options, MSAA, low graphics settings, and curtain-video removal.
Do not repeat those tests.

## What the probes established

- All six synthetic D3D9 paths render: DrawPrimitiveUP, managed/dynamic VBs, managed/dynamic textures, and
  SYSTEMMEM-to-DEFAULT UpdateTexture.
- Run 5 proved private VB replay plus the captured transforms can render. Its selected 256x256 managed texture had an
  all-zero CPU snapshot, but that did not prove all textures were zero.
- Run 6 captured the visible startup splash. The 256x1024 A8R8G8B8 texture was nonzero and exactly matched PC
  (`hash=7e0aaadd`, RGB nonzero 187,885, alpha nonzero 189,900). The 1024x1024 X8R8G8B8 texture was also nonzero.
- Run 7 retained original resources and replayed snapshots. It again sampled the six-draw startup splash, not the
  black menu. The game VB was DEFAULT `DYNAMIC|WRITEONLY` (`usage=0x208`, snapshot `hash=1cef53ed`). Original and
  replay calls returned S_OK, but they shared one scene with later READONLY locks, so GPU completion was ambiguous.

## Run 8 result: configuration miss, not a rendering verdict

Run 8 loaded successfully with `GameMinDraws=100`, ran about 357 seconds / 8,716 frames, and exited cleanly with
`+`+`-`. There was no crash, device loss, GPU fault, or OOM; heap peaked around 887 MB. The long startup stall was real:
almost no frame progress from roughly 15 to 116 seconds, then normal ~40 FPS from about 147 seconds onward.

The run-8 self-test never armed. `GameMinDraws` is a per-frame count, while observed game frames were:

- splash: 5-7 draws / 48-50 primitives;
- intermediate frontend: 41-42 draws / 406 primitives;
- submenu transition: 34-35 draws / 402 primitives;
- stable black menu: exactly 32 draws / 64 primitives.

Therefore waiting longer could never satisfy `GameMinDraws=100`. The log contains no `GAMECAP`, `RUN8`,
`GetRenderTargetData`, `VERDICT`, or workaround lines. The user's observation that nothing changed is expected and says
nothing about texture versus VB. The generic probe matrix still completed with S_OK. The only persistent D3D9 failure
was the already-known harmless `BeginStateBlock -> D3DERR_INVALIDCALL` (94/1777 and then frozen).

Local full logs:

- `verification/stronghold2-probe-20260927/run8-menu-readback/d3d9log-switch-run8.txt`
- `verification/stronghold2-probe-20260927/run8-menu-readback/Stronghold2-switch-run8.log`
- `verification/stronghold2-probe-20260927/run8-menu-readback/autorun-runtime-switch-run8.log`

## Run 9 is already deployed and verified

The DLL is unchanged from run 8:

`d3d9.dll` SHA-256 `31cae781e634d4730107ebd9f6ef199d548e86f5d621f34ac9562b5c1cfba1fa`

The only change is the deployed, MTP-readback-verified INI:

```ini
[d3d9log]
NoMSAA=1
Probe=1
GameForce=0
GameMinDraws=0
```

`GameMinDraws=0` selects the logger's exact built-in stable-menu signature: two consecutive frames at
32 draws / 64 primitives. It skips the six-draw splash and all 34-42-draw intermediate screens.

Local run-9 artifacts and the before/deployed INIs are under:

`verification/stronghold2-probe-20260927/run9-menu-32x64/`

## Exact next test

1. Start Stronghold 2 on the Switch.
2. Wait through the long black startup; it can take around four minutes before the frontend fills in.
3. Once the stable main menu appears, do not press anything for 20 seconds.
4. Observe whether the background changes after the probe runs.
5. Optionally enter Single Player -> Free Build, wait another 15 seconds.
6. Exit cleanly with `+`+`-`, reconnect USB, and pull `d3d9log.txt`, `Stronghold2.log`, and `autorun_runtime.log`.

Search the D3D9 log first for:

```text
GAMECAP stable frame accepted
RUN8 menu selftest begin
RUN8 ORIGINAL readback
RUN8 REPLAY readback
RUN8 VERDICT
RUN8 FIX
```

Interpretation:

- `texture=BAD`: original texture pixels were missing but replay pixels existed; captured-texture substitution is
  enabled in that same run.
- `texture=OK`, `vb=BAD`: original VB pixels differed/missed while replay worked; guarded DP-to-DPUP bypass is enabled.
- both `OK`: captured original resources reach the GPU; the fault lies in another resource/state/video-composition path.
- `INCONCLUSIVE`: use the raw `primary_nonclear`, `any_nonclear`, `vb_exact`, and `control_exact` counts. In particular,
  an all-zero CPU snapshot can make replay blank even if the original GPU copy was meaningful.

After this run, do not ask the user for more broad trial-and-error. Either promote a proven workaround into a clean
non-probe DLL, or prepare a minimal Autorun/wine-nx bug report from the synchronized readback evidence.

## Workspace safety

Never run `rg --files --follow` here. The Wine prefix contains `dosdevices/z:` pointing at `/`, which previously made
the search recursively traverse the filesystem and pin every laptop CPU core. The workspace `.ignore` and
`.vscode/settings.json` exclude that prefix and disable symlink-following searches/watchers.
