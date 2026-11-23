# Game capture hook

Game capture injects a hook into the game: `graphics-hook32/64.dll` (also registered as the Vulkan implicit layer
`VK_LAYER_OBS_HOOK` through `obs-vulkan32/64.json`), injected by `inject-helper32/64.exe`, with the D3D8/D3D9/DXGI vtable
offsets from `get-graphics-offsets32/64.exe`. Anti-cheat software and games commonly refuse to load an unsigned hook, and
the hook in `C:\ProgramData\obs-studio-hook` is shared with every OBS Studio installation on the machine, so Knurbs does
not build these binaries. It ships OBS Studio's official ones, unchanged and Authenticode-signed by OBS Project, LLC.

## Where they come from

`plugins/win-capture/cmake/obs-hook.cmake` pins an OBS Studio release. At configure time it downloads the Windows x64
release archive from the OBS Studio GitHub releases page, checks the archive's SHA-256, extracts the eight files below
from `core/win-capture/data/` into `.deps/obs-hook-<version>/data`, and checks each file's SHA-256. Any mismatch fails
the configure. The build copies the files into win-capture's data directory (rundir and install). `KNURBS_OBS_HOOK_ARCHIVE`
points the configure at a local copy of the archive for offline builds (its hash is checked as well).

Pinned: **OBS Studio 33.0.3** (hook version 1.8.8),
`https://github.com/obsproject/obs-studio/releases/download/33.0.3/OBS-Studio-33.0.3-Windows-x64.zip`,
SHA-256 `18a68c325adbda6f6082d5281021a8d1e35a6432842800618e8690b71925c107`.

| File | SHA-256 | Signature |
|---|---|---|
| `graphics-hook32.dll` | `e51b81645df3b06458547d9433eaa6b679b1e8feb214ec14ecdfeb23e2a34b66` | OBS Project, LLC |
| `graphics-hook64.dll` | `1261cf476829354a3831e97bc77b9af6a1340832e3ed4e079dc2b98b79857d2a` | OBS Project, LLC |
| `inject-helper32.exe` | `2bcd432d538ef4a9999ff9be5be1cd9079dff94eb60c7e1ad0886bf6a822d122` | OBS Project, LLC |
| `inject-helper64.exe` | `4ecc338f9232817f19a3f0228bd3de4fb8bddabffa65a94b9227c02a1545bb0d` | OBS Project, LLC |
| `get-graphics-offsets32.exe` | `d761a8abfc2afb8392d561766a06a7d44d072558c49996dc78952ca221b6ad13` | OBS Project, LLC |
| `get-graphics-offsets64.exe` | `46aa1bd9b246d5165a27b597b791b3b6d5e8bf6a1b4008f510bc9995cccebdac` | OBS Project, LLC |
| `obs-vulkan32.json` | `c16e0707ae66ad71e8a0720aeb6e6997a1017f19762333452aef692115a9ab41` | (manifest, unsigned) |
| `obs-vulkan64.json` | `7b6b0813fb58b276847a8583eb5c3f94aee7d7ad0ae3a1ef6133d5d8771f20f4` | (manifest, unsigned) |

The hook protocol of 33.0.3 is the one `game-capture.c` speaks: `shared/obs-hook-config` (`graphics-hook-info.h`,
`graphics-hook-ver.h`, `hook-helpers.h`) is identical to OBS Studio 33.0.3's, as are the hook, inject-helper and
get-graphics-offsets sources still in the tree. Those sources (`plugins/win-capture/graphics-hook`, `inject-helper`,
`get-graphics-offsets`) are kept for reference only; nothing built from them is shipped, and they are no longer part of
the build. With them went the x86 child build (`build_x86`), which existed only for the 32-bit hook binaries; Knurbs
builds for x64 only.

## Installing into ProgramData

On start-up win-capture copies its hook into `C:\ProgramData\obs-studio-hook` (where the Vulkan layer is registered)
only when the hook is validly Authenticode-signed by OBS Project, LLC (`game-capture-hook-install.c`: WinVerifyTrust,
then the signer's CN and O; revocation is not checked, to stay offline) and newer than the one there. It never
downgrades, never copies an unsigned hook, and replaces an unsigned hook it finds there with the signed one unless that
would be a downgrade. It loads the hook from there only when that one is signed too (and not of a newer major version);
otherwise it uses its bundled one. `test_win_capture_hook_install` covers this.

## Bumping the pin

1. Pick the newest stable OBS Studio release whose `shared/obs-hook-config` matches ours (compare
   `graphics-hook-info.h`, `graphics-hook-ver.h` and `hook-helpers.h` at the release tag). A changed protocol needs
   `game-capture.c` updated first.
2. In `obs-hook.cmake`, set `KNURBS_OBS_HOOK_VERSION` and the archive's SHA-256 (the release asset's `digest` on
   GitHub; check it against a download).
3. Configure with `-DKNURBS_OBS_HOOK_PRINT_HASHES=ON`. The configure fails on the old file hashes and prints the new
   ones; check each `.exe`/`.dll` in `.deps/obs-hook-<version>/data` with `Get-AuthenticodeSignature` (Valid, signer
   OBS Project, LLC) before pasting the hashes into `_obs_hook_files` and the table above.
4. Run the game capture cases of `test_vulkan_win_capture` (they require the loaded hook to be the shipped, signed one)
   and `test_win_capture_hook_install`.

## Licence

The hook, inject-helper and get-graphics-offsets are part of OBS Studio, licensed under the GNU General Public
License, version 2 or later, like the rest of Knurbs. Knurbs redistributes OBS Project's signed binaries unchanged;
their complete corresponding source is OBS Studio's, published upstream at
<https://github.com/obsproject/obs-studio/tree/33.0.3> (`plugins/win-capture`, `shared/obs-hook-config`,
`shared/obs-inject-library`) and in the release's `OBS-Studio-33.0.3-Sources.tar.gz`, and is also in this tree.
