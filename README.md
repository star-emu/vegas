<img width="1280" height="720" alt="20260505_152432" src="https://github.com/user-attachments/assets/495a3b98-5328-4aae-8d2e-2a6656dda567" />



# 🎰 VEGAS: DXVK v2.7.3
### **Adreno-Tuned DXVK Fork for Android Emulation (Star Emulator / Winlator)**

**VEGAS** (formerly Star Engine) is a specialized performance modification of DXVK designed for **Qualcomm Adreno GPUs** in mobile environments. It brings a tier-based auto-tuning engine, FSR 1.0 compute upscaling, motion-compensated frame generation, and dynamic driver safety features — all controlled by a single master switch.

---

## 🎯 Key Features

### 🧠 Star Profile Master Switch (`dxvk.enableStarProfile`)
All Vegas features are gated behind one Tristate option:
- **Auto** — Enable on Adreno GPUs, disable on all others (default)
- **True** — Force-enable all features (for testing on non-Adreno)
- **False** — Hard-disable everything (emergency escape hatch)

### 🏎️ Tier-Based Auto-Tuning
Adreno GPUs are classified into 3 tiers via sysfs (`/sys/class/kgsl/kgsl-3d0/gpu_model`):

| Tier | GPUs | Persona |
|------|------|---------|
| 1 | Adreno 610, 619 | GTX 1050 Ti |
| 2 | Adreno 640, 642L, 650, 660 | GTX 1070 |
| 3 | Adreno 7xx, 8xx | RTX 3060 |

Each tier gets tuned draw thresholds, frame gen eligibility, HAAE quality scaling, and VRAM budgets automatically.

### 🔬 FSR 1.0 Compute Upscaler (`vegas.enableUpscaler`)
Full FSR 1.0 EASU compute pipeline:
- **Auto** — Upscale when render resolution < swapchain resolution
- **True** — Always upscale (half-resolution quadrants)
- **False** — Disabled

Uses push constants + 2-binding descriptor set, dispatch with blit + fence sync.

### 🎞️ 3-Pass Motion-Compensated Frame Generation
Available on Tier 2 (≤29ms frametime) and Tier 3 (≤33ms frametime):
1. **Motion estimation** — Block SAD on prev/cur frames → raw motion vectors
2. **Median filter** — 3×3 spatial denoise on motion field
3. **Warp + blend** — Warp prev frame by filtered motion, alpha-blend at weight 0.5

Compute-only pipeline with shared 4-binding descriptor layout.

### 🛡️ Adaptive Governor (`tuneThreshold`)
EMA-smoothed frame-time telemetry with **15-frame cooldown** (down from 120) provides 8× faster governor response. Tier-based cap multipliers (T1=1.5×, T2=2.5×, T3=3.0×) ensure optimal batching for each GPU class. Real GPU load is estimated from `frameTime/targetFrameTime` ratio (6 continuous levels from 0.25 to 0.96) rather than a hardcoded value, enabling accurate overheating detection.

### 📊 Dynamic VRAM & GPU Mask
- `applyVramSwap(Config&)` — Sets `dxgi.maxDeviceMemory` to 40% of system RAM (clamped 1–4 GB)
- `applyGpuMask(Config&)` — Maps Adreno tier to NVIDIA vendor/device ID for game compatibility

### 🧹 Bind Skip Optimization
Skips redundant `vkCmdBindPipeline` calls when no dynamic state has changed — reduces CPU overhead.

### 🎨 HUD Performance Colors
Graph coloring via `getGraphColor()`: green (normal) → yellow (lagging) → orange (stuttering) → red (overheating).

---

## 🛠️ Installation

### Via Star Emulator
1. Open Star Emulator
2. Navigate to **Contents** menu
3. Install the `dxvk-2.7.3.wcp` file

### Manual Setup
Place `dxvk.conf` in any of these paths:
- `/storage/emulated/0/Winlator/`
- `/storage/emulated/0/Download/`
- `/storage/emulated/0/`

Or set `DXVK_CONFIG_FILE` env var to your config path.

---

## ⚙️ Configuration

```ini
# Master switch: Auto (Adreno only), True (force-on), False (force-off)
dxvk.enableStarProfile = Auto

# FSR 1.0 upscaler: Auto, True, False
vegas.enableUpscaler = Auto
```

All other parameters (thresholds, tier, bind skip, HAAE, quality scaling) are auto-tuned by the Vegas engine.

---

## 🧱 Build from Source

```bash
git clone --recursive https://github.com/isygold/Star-Engine-DXVK-Releases.git
cd Star-Engine-DXVK-Releases

# Android cross-build (requires NDK + Meson)
meson setup --cross-file build-android-aarch64.txt \
  --buildtype release --prefix /output/dir build
cd build
ninja install
```

See the [Upstream DXVK Documentation](#upstream-dxvk-documentation) section below for full build instructions.

---

## 📜 Changelog (Vegas)

| Commit | Feature |
|--------|---------|
| `5b7bcd5` | **Bleeding-edge:** tier-based governor caps (1.5×/2.5×/3.0×), 15-frame cooldown, real GPU load via ftRatio, HAAE threshold fix, ARM64 compiler thread cap (max 4) |
| `a8c994e` | **Phase 1/2:** Custom tier classifier (classifyAdrenoTier), TBDR-safe governor, 60s shader cache flush, swapchain BufferCount fix, async+GPL compat, zero-init tier-aware, log cleanup (3378→1 line) |
| `1fe8861` | GPL disabled when async pipeline compilation is active |
| `1346828` | Respect BufferCount ≥2 regardless of swap effect (fixes Tomb Raider DISCARD mode) |
| `c7be5c3` | Master switch + dead code removal + conf docs |
| `c006dc3` | Framegen first-frame dead code fix |
| `7da716a` | 3-pass framegen integration |
| `42bcbe7` | FSR intermediate target + barrier validation |
| (earlier) | FSR SPIR-V + compute pipeline + HAAE base |

---

## 📝 Notes

- **Tier 1 (Adreno 610/619):** Frame generation disabled — compute budget insufficient for 3-pass. FSR available but not recommended.
- **BCn→ASTC transcoder:** Implemented but gated — will be enabled when image upload pipeline is wired.
- **Turnip driver:** Use a recent Turnip (Mesa 25.x+) for best results. The Vulkan 1.3 path is required for descriptor indexing and push constants.
- **Container tests:** Built-in benchmarks may show lower FPS due to draw thresholds. Judge by actual gameplay smoothness.
- **CHANGES_AND_FIXES.txt:** See the companion file for a complete, game-by-game breakdown of all stability and performance fixes.

---

## 📜 Credits

- **Lead Developer:** ISYGOLD
- **Base Project:** DXVK v2.7.1+ by doitsujin
- **License:** zlib/libpng

---

## Upstream DXVK Documentation

The following sections are reproduced from the upstream DXVK README for reference.

### How to use (Desktop/Wine)

In order to install a DXVK package obtained from the [release](https://github.com/doitsujin/dxvk/releases) page into a given wine prefix, copy or symlink the DLLs into the following directories as follows, then open `winecfg` and manually add `native` DLL overrides for `d3d8`, `d3d9`, `d3d10core`, `d3d11` and `dxgi` under the Libraries tab.

In a default Wine prefix that would be as follows:
```
export WINEPREFIX=/path/to/wineprefix
cp x64/*.dll $WINEPREFIX/drive_c/windows/system32
cp x32/*.dll $WINEPREFIX/drive_c/windows/syswow64
winecfg
```

For a pure 32-bit Wine prefix (non default) the 32-bit DLLs instead go to the `system32` directory:
```
export WINEPREFIX=/path/to/wineprefix
cp x32/*.dll $WINEPREFIX/drive_c/windows/system32
winecfg
```

Verify that your application uses DXVK instead of wined3d by enabling the HUD (see notes below).

In order to remove DXVK from a prefix, remove the DLLs and DLL overrides, and run `wineboot -u` to restore the original DLL files.

Tools such as Steam Play, Lutris, Bottles, Heroic Launcher, etc will automatically handle setup of dxvk on their own when enabled.

#### DLL dependencies
Listed below are the DLL requirements for using DXVK with any single API.

- d3d8: `d3d8.dll` and `d3d9.dll`
- d3d9: `d3d9.dll`
- d3d10: `d3d10core.dll`, `d3d11.dll` and `dxgi.dll`
- d3d11: `d3d11.dll` and `dxgi.dll`

### Notes on Vulkan drivers
Before reporting an issue, please check the [Wiki](https://github.com/doitsujin/dxvk/wiki/Driver-support) page on the current driver status and make sure you run a recent enough driver version for your hardware.

### Online multi-player games
Manipulation of Direct3D libraries in multi-player games may be considered cheating and can get your account **banned**. This may also apply to single-player games with an embedded or dedicated multiplayer portion. **Use at your own risk.**

### HUD
The `DXVK_HUD` environment variable controls a HUD which can display the framerate and some stat counters. It accepts a comma-separated list of the following options:
- `devinfo`: Displays the name of the GPU and the driver version.
- `fps`: Shows the current frame rate.
- `frametimes`: Shows a frame time graph.
- `submissions`: Shows the number of command buffers submitted per frame.
- `drawcalls`: Shows the number of draw calls and render passes per frame.
- `pipelines`: Shows the total number of graphics and compute pipelines.
- `descriptors`: Shows the number of descriptor pools and descriptor sets.
- `memory`: Shows the amount of device memory allocated and used.
- `allocations`: Shows detailed memory chunk suballocation info.
- `gpuload`: Shows estimated GPU load. May be inaccurate.
- `version`: Shows DXVK version.
- `api`: Shows the D3D feature level used by the application.
- `cs`: Shows worker thread statistics.
- `compiler`: Shows shader compiler activity
- `samplers`: Shows the current number of sampler pairs used *[D3D9 Only]*
- `ffshaders`: Shows the current number of shaders generated from fixed function state *[D3D9 Only]*
- `swvp`: Shows whether or not the device is running in software vertex processing mode *[D3D9 Only]*
- `scale=x`: Scales the HUD by a factor of `x` (e.g. `1.5`)
- `opacity=y`: Adjusts the HUD opacity by a factor of `y` (e.g. `0.5`, `1.0` being fully opaque).

Additionally, `DXVK_HUD=1` has the same effect as `DXVK_HUD=devinfo,fps`, and `DXVK_HUD=full` enables all available HUD elements.

### Logs
When used with Wine, DXVK will print log messages to `stderr`. Additionally, standalone log files can optionally be generated by setting the `DXVK_LOG_PATH` variable, where log files in the given directory will be called `app_d3d11.log`, `app_dxgi.log` etc., where `app` is the name of the game executable.

On Windows, log files will be created in the game's working directory by default, which is usually next to the game executable.

### Device filter
Some applications do not provide a method to select a different GPU. In that case, DXVK can be forced to use a given device:
- `DXVK_FILTER_DEVICE_NAME="Device Name"` Selects devices with a matching Vulkan device name, which can be retrieved with tools such as `vulkaninfo`. Matches on substrings, so "VEGA" or "AMD RADV VEGA10" is supported if the full device name is "AMD RADV VEGA10 (LLVM 9.0.0)", for example. If the substring matches more than one device, the first device matched will be used.
- `DXVK_FILTER_DEVICE_UUID="00000000000000000000000000000001"` Selects a device by matching its Vulkan device UUID, which can also be retrieved using tools such as `vulkaninfo`. The UUID must be a 32-character hexadecimal string with no dashes. This method provides more precise selection, especially when using multiple identical GPUs.

**Note:** If the device filter is configured incorrectly, it may filter out all devices and applications will be unable to create a D3D device.

### Debugging
The following environment variables can be used for **debugging** purposes.
- `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` Enables Vulkan debug layers. Highly recommended for troubleshooting rendering issues and driver crashes. Requires the Vulkan SDK to be installed on the host system.
- `DXVK_LOG_LEVEL=none|error|warn|info|debug` Controls message logging.
- `DXVK_LOG_PATH=/some/directory` Changes path where log files are stored. Set to `none` to disable log file creation entirely, without disabling logging.
- `DXVK_DEBUG=markers|validation` Enables use of the `VK_EXT_debug_utils` extension for translating performance event markers, or to enable Vulkan validation, respecticely.
- `DXVK_CONFIG_FILE=/xxx/dxvk.conf` Sets path to the configuration file.
- `DXVK_CONFIG="dxgi.hideAmdGpu = True; dxgi.syncInterval = 0"` Can be used to set config variables through the environment instead of a configuration file using the same syntax. `;` is used as a seperator.
- `DXVK_SHADER_CACHE=0`: Disables the internal shader cache.
- `DXVK_SHADER_CACHE_PATH=/some/directory`: Path to internal shader cache files. By default, this will use `%LOCALAPPDATA%/dxvk` in a Windows or Wine environment, and `$HOME/.cache` or `$XDG_CACHE_HOME` in a native Linux environment.

### Graphics Pipeline Library
On drivers which support `VK_EXT_graphics_pipeline_library` Vulkan shaders will be compiled at the time the game loads its D3D shaders, rather than at draw time. This reduces or eliminates shader compile stutter in many games when compared to the previous system.

In games that load their shaders during loading screens or in the menu, this can lead to prolonged periods of very high CPU utilization, especially on weaker CPUs. For affected games it is recommended to wait for shader compilation to finish before starting the game to avoid stutter and low performance. Shader compiler activity can be monitored with `DXVK_HUD=compiler`.

**Note:** Games which only load their D3D shaders at draw time (e.g. most Unreal Engine games) will still exhibit some stutter, although it should still be less severe than without this feature.

### Build instructions (Desktop/Cross-compile)

In order to pull in all submodules that are needed for building, clone the repository using the following command:
```
git clone --recursive https://github.com/doitsujin/dxvk.git
```

#### Requirements:
- [wine 7.1](https://www.winehq.org/) or newer
- [Meson](https://mesonbuild.com/) build system (at least version 0.58)
- [Mingw-w64](https://www.mingw-w64.org) compiler and headers (at least version 10.0)
- [glslang](https://github.com/KhronosGroup/glslang) compiler

#### Building DLLs

**The simple way**
Inside the DXVK directory, run:
```
./package-release.sh master /your/target/directory --no-package
```

This will create a folder `dxvk-master` in `/your/target/directory`, which contains both 32-bit and 64-bit versions of DXVK, which can be set up in the same way as the release versions as noted above.

In order to preserve the build directories for development, pass `--dev-build` to the script. This option implies `--no-package`. After making changes to the source code, you can then do the following to rebuild DXVK:
```
# change to build.32 for 32-bit
cd /your/target/directory/build.64
ninja install
```

**Compiling manually**
```
# 64-bit build. For 32-bit builds, replace
# build-win64.txt with build-win32.txt
meson setup --cross-file build-win64.txt --buildtype release --prefix /your/dxvk/directory build.w64
cd build.w64
ninja install
```

The D3D8, D3D9, D3D10, D3D11 and DXGI DLLs will be located in `/your/dxvk/directory/bin`.

#### Build troubleshooting
DXVK requires threading support from your mingw-w64 build environment. If you are missing this, you may see "error: 'std::cv_status' has not been declared" or similar threading related errors.

On Debian and Ubuntu, this can be resolved by using the posix alternate, which supports threading. For example, choose the posix alternate from these commands:
```
update-alternatives --config x86_64-w64-mingw32-gcc
update-alternatives --config x86_64-w64-mingw32-g++
update-alternatives --config i686-w64-mingw32-gcc
update-alternatives --config i686-w64-mingw32-g++
```
For non debian based distros, make sure that your mingw-w64-gcc cross compiler does have `--enable-threads=posix` enabled during configure. If your distro does ship its mingw-w64-gcc binary with `--enable-threads=win32` you might have to recompile locally or open a bug at your distro's bugtracker to ask for it.

### DXVK Native

DXVK Native is a version of DXVK which allows it to be used natively without Wine.

This is primarily useful for game and application ports to either avoid having to write another rendering backend, or to help with port bringup during development.

[Release builds](https://github.com/doitsujin/dxvk/releases) are built using the Steam Runtime.

#### How does it work?

DXVK Native replaces certain Windows-isms with a platform and framework-agnostic replacement, for example, `HWND`s can become `SDL_Window*`s, etc. All it takes to do that is to add another WSI backend.

**Note:** DXVK Native requires a backend to be explicitly set via the `DXVK_WSI_DRIVER` environment variable. The current built-in options are `SDL3`, `SDL2`, and `GLFW`.

DXVK Native comes with a slim set of Windows header definitions required for D3D9/11 and the MinGW headers for D3D9/11. In most cases, it will end up being plug and play with your renderer, but there may be certain teething issues such as:
- `__uuidof(type)` is supported, but `__uuidof(variable)` is not supported. Use `__uuidof_var(variable)` instead.
