# Android ANGLE experiment

The game test APK offers system **OpenGL ES** and app-local **Vulkan (ANGLE)**.
`r_backend gl` is the source default; `r_backend vulkan` takes effect after an
application restart. No system driver or global Android graphics setting is
changed. See [renderer startup](../../../docs/renderer-backend.md#android-graphics-api-selection-through-angle)
and [Android workflow](../../../docs/android.md#selectable-angle-vulkan-experiment).

## Current pin

The selected libraries come from the official Android SDK package
`system-images;android-35;default;arm64-v8a`, **revision 2**, fingerprint:

```text
Android/sdk_phone64_arm64/emu64a:15/AE3A.240806.019/12368160:userdebug/test-keys
```

The [official SDK repository index](https://dl.google.com/android/repository/sys-img/android/sys-img2-3.xml)
identifies [arm64-v8a-35_r02.zip](https://dl.google.com/android/repository/sys-img/android/arm64-v8a-35_r02.zip),
769099654 bytes, SHA-1 `2026a06409db630b56711afdbffb457c1dbaed49`.
The library SHA-256 pins below are checked independently on import and every build.

| Library | SHA-256 |
|---|---|
| `libEGL_angle.so` | `244fa6f8fb33081298df4bcf878d3fa33ce90c10d8fbff0c3f2f03a22c1a1411` |
| `libGLESv2_angle.so` | `b95da08d0dc11ffc2cd56685a17a9cdd45ae7113b07e04b504693476eb72d693` |

They report ANGLE 2.1.0 with **unknown embedded git hash**. The fingerprint and
binary hashes identify the artifact; do not invent an ANGLE source commit or
call this the latest upstream build. ELF LOAD alignment is 16 KiB. Loading,
rendering and measurements are verified on Android 12 Honor; that does not
establish compatibility of the whole APK with every Android API or 16 KiB device.

## Prepare once

Use the existing SDK/emulator setup from [Android](../../../docs/android.md).
Start the pinned official image, then import its libraries without modifying
that device:

```sh
python3 tools/android/extract_angle.py --serial emulator-5554
```

The importer requires the exact fingerprint and verifies both library hashes
before changing the cache. `tools/android/fetch_angle.py` rechecks the cache
at `build/android/angle/sdk35/arm64-v8a/` on every game test build. Missing or
corrupt inputs are errors; there is no automatic switch to the older ANGLE.
A direct import of already extracted matching files is also supported:

```sh
python3 tools/android/fetch_angle.py --from-dir /path/to/extracted/libraries
```

`tools/android/build_probe.py --game` includes the verified libraries. Add
`--without-angle` for a GLES-only package, or `--angle-dir /path/to/libraries`
to import the exact pin while building. The standalone shader-probe APK uses
system GLES. Build only the test artifacts required for the current task.

The APK includes the manifest, `LICENSE`, and `NOTICE-sdk35.txt` at
`assets/share/angle/`. The latter contains all twenty notice entries associated
with the two libraries in the image's `/system/etc/NOTICE.xml.gz`, with their
content IDs. Binary files remain in the ignored cache.

## Historical AOSP control

The first trial used AOSP package revision
`9a0834cc8a5f3427dae2f6e87de4dfae50cb781c`, embedded ANGLE revision
`3f647b1bf9a4`, with 4 KiB ELF LOAD alignment. On Honor it produced huge
triangles, disappearing terrain and an internal Vulkan error at particle draws;
combat rendering fell to roughly 2 FPS. The same diagnostic scenario with the
SDK35 libraries completes without that error. The exact internal Vulkan cause
in the legacy binary has not been isolated.

Keep this negative result and complete source/recipes in the
retained trial (`$OPENREALM_EXPERIMENTS_DIR/warcraft-3/optimization-archive/vulkan-backend-20261018/README.md`, external archive).
The old libraries remain separately fetchable for reconstruction:

```sh
python3 tools/android/fetch_angle.py --legacy
```

This writes the separate `build/android/angle/legacy/arm64-v8a/` cache. Current
packaging accepts the current pin; use the archived legacy source/recipe for an
exact old-APK comparison. Do not reuse the failed legacy measurements to predict
current ANGLE or a future native Vulkan renderer.
