<img width="1280" height="720" alt="20260505_152432" src="https://github.com/user-attachments/assets/e73f1d6f-7935-46d8-83ec-d903eaaae0db" />

# DXVK — StarEngine (Vegas Edition)

**Version 2.7.3-vegas** — A GPLAsync/DXVK fork with Adreno-optimized enhancements for mobile/ARM64 gaming via Wine. Built on the shoulders of [GPLAsync](https://github.com/ishitatsuyuki/GPLAsync) by ishitatsuyuki and the excellent [DXVK](https://github.com/doitsujin/dxvk) project by doitsujin.

> **⚠️ Important:** This is a downstream fork. All improvements specific to this build are additive and fully config-gated — stock DXVK behavior is preserved when no StarEngine config options are set.

> **⚠️ Unity Compatibility:** This build is stable for non-Unity games (Unreal Engine, Source, etc.) but is currently unstable / non-functional for Unity-based titles on Adreno.

---

## StarEngine Features

StarEngine features activate on any GPU where `personaTier()` ≥ 1. This happens either automatically on Adreno GPUs, or when the `dxvk.starPersona` config is set. All features can be overridden via `dxvk.conf` or environment variables.

> **💡 No config file needed on Adreno:** All features auto-enable with tier-based defaults. A `dxvk.conf` is only required to override defaults, enable StarEngine on non-Adreno GPUs (via `dxvk.starPersona`), or fine-tune settings for testing.

| # | Feature | Description | Config Key |
|---|---------|-------------|------------|
| 1 | **VRAM ROM-Swap** | Inflates device-local heap as % of total system RAM (15-25% based on tier, capped at 2-4 GB). Prevents texture LOD pop-in on shared-memory devices. | `dxvk.starVramMultiplier` |
| 2 | **Aspect Ratio Correction** | Automatically letterboxes/pillarboxes the viewport to 16:9 on non-standard displays (tablets, foldables). | — |
| 3 | **FSR 1.0 (EASU)** | Compute-shader-based upscaler dispatched before presentation. Sharpens swapchain images. | `dxvk.starEnableFsr` |
| 4 | **LSFG Frame Doubling** | Second `vkQueuePresentKHR` when frame time exceeds threshold, doubling perceived framerate. | `dxvk.starEnableLsfg`, `dxvk.starLsfgThresholdMs` |
| 5 | **Zero-Mapped Memory** | Forces zero-initialization of mapped buffer allocations, avoiding GPU page faults on Vulkan drivers. Auto-enabled on Adreno unless user explicitly set the option. | `dxvk.zeroMappedMemory` |
| 6 | **ZeroInit Shaders** | Zero-initializes workgroup memory in compute shaders via `VK_PIPELINE_CREATE_2_ENABLE_WORKGROUP_MEMORY_ZERO_INIT_BIT`. Fixes Unity engine crashes. | — |
| 7 | **Adaptive HUD** | Color-coded performance label (Normal/Lagging/Stuttering/Overheating) relative to target frame rate. Shown via `DXVK_HUD=starengine`. | `dxvk.hud = starengine` |
| 8 | **ASTC Texture Compression** | *(Future — planned for a stable release)* Transcodes BCn→ASTC at texture upload to save ~50-75% bandwidth on Adreno's native TMU. Pipeline is fully implemented but not yet wired into image upload paths. | — |
| 9 | **Adaptive Draw-Command Flushing** | Counts draws since last command buffer submit. When count exceeds a per-tier threshold (600/1200/2000), spills the render pass and flushes. **Bind-Skip**: Skips redundant `vkCmdBindPipeline` calls when pipeline handle hasn't changed. **AdrenoGovernor**: Dynamically tunes the flush threshold based on GPU load and frame time. **D3D9**: Higher thresholds (1500/3000/5000) for D3D9 titles. | — |
| 10 | **Async Pipeline Compilation** | Compiles pipelines asynchronously on a worker thread. If a pipeline isn't ready at draw time, the draw call is skipped rather than stalling the render thread. Reduces shader compilation hitches. | `dxvk.enableAsync` |

### Configuration (`dxvk.conf`)

```ini
# GPU identity persona. 0 = AUTO (detect from real GPU).
# 1 = GTX 1050 Ti, 2 = GTX 1660, 3 = RTX 3060 Laptop.
# AUTO mode reads the real Adreno model number and self-masks to
# the matching NVIDIA persona with correct tier optimizations.
dxvk.starPersona          = 0

# FSR 1.0 upscaler (Auto = on if persona/tier enabled, off otherwise)
dxvk.starEnableFsr        = Auto

# LSFG frame doubling (Auto = on if persona/tier enabled, off otherwise)
dxvk.starEnableLsfg       = Auto

# LSFG frame time threshold in ms before doubling triggers.
# 0 = per-tier default (off for tier 1, >29 ms for tier 2, >33 ms for tier 3)
dxvk.starLsfgThresholdMs  = 0

# VRAM multiplier. 0 = per-tier default (% of system RAM, capped).
# 1.0 = report real hardware size. 2.0 = double.
dxvk.starVramMultiplier   = 0

# Async pipeline compilation (Auto = on if Adreno, off otherwise)
# Draw calls are skipped if the pipeline isn't ready yet.
dxvk.enableAsync          = Auto
```

### Low-level DXGI Options (Persona Masking)

These are the raw options that `dxvk.starPersona` sets internally. Can also be used directly:

```ini
dxgi.customVendorId       = 0x10DE
dxgi.customDeviceId       = 0x1C82
dxgi.customDeviceDesc     = "NVIDIA GeForce GTX 1050 Ti (StarEngine)"
```

### Tier Classification

| Tier | Models | VRAM % (cap) | LSFG Threshold | ZeroInit |
|------|--------|--------------|----------------|----------|
| 1 (Budget) | Adreno 6xx (610, 618–719) | 15% (up to 2 GB) | Disabled | Enabled |
| 2 (Mid)   | Adreno 720–739 | 20% (up to 3 GB) | >29 ms | Enabled |
| 3 (Elite) | Adreno 740+    | 25% (up to 4 GB) | >33 ms | Enabled |

### Persona Masking

When `dxvk.starPersona` is set (or auto-detected), the engine masks the GPU identity to the corresponding NVIDIA model and derives the optimization tier from the persona:

| Persona | Tier | GPU Reported | VRAM Ratio | LSFG | Draw Flush Threshold |
|---------|------|-------------|------------|------|---------------------|
| 1 | 1 | NVIDIA GeForce GTX 1050 Ti | 15% / 2 GB | off | 600 draws |
| 2 | 2 | NVIDIA GeForce GTX 1660 | 20% / 3 GB | >29 ms | 1200 draws |
| 3 | 3 | NVIDIA GeForce RTX 3060 Laptop | 25% / 4 GB | >33 ms | 2000 draws |
| AUTO | auto | Matches real Adreno class | per matched tier | per matched tier | per matched tier |

### Adaptive HUD Colors

| State | Color | Condition |
|-------|-------|-----------|
| NORMAL | Green | frame time ≤ 1.5× target |
| LAGGING | Yellow | frame time > 1.5× target |
| STUTTERING | Orange | Delta > 0.75× target |
| OVERHEATING | Red | Load ≥ 95% + frame time > 3× target |

## Diagnostic & Stability Improvements

| Improvement | Description |
|-------------|-------------|
| Adreno detection ordering | Adreno is now detected before persona masking, ensuring all Adreno-specific memory and optimization workarounds activate on real hardware |
| Persona gating | GPU identity masking applies only at the API query layer — the Vulkan driver sees the real Adreno, fixing memory type selection and host-visible heap access |
| Memory allocator diagnostics | Failed `vkAllocateMemory` calls now log the returned `VkResult`, surfacing driver-level OOM errors in the output |
| Unity compatibility gate | VRAM inflation is deferred for Unity titles to avoid memory pressure on shared-memory systems |
| KGSL allocation cap | Maximum allocation chunk size capped at 64 MiB on Adreno KGSL drivers to prevent driver-level allocation failures |

<img width="2400" height="1080" alt="Screenshot_2026-05-31-20-01-21-510_com winlator star" src="https://github.com/user-attachments/assets/adbb727e-4cd5-4c72-8cc7-9db9a628cf9c" />
<img width="2400" height="1080" alt="Screenshot_2026-05-31-20-07-58-541_com winlator star" src="https://github.com/user-attachments/assets/f3af902c-69db-4f73-ad0c-39ec56c8ccd6" />


---

## Upstream README

**DXVK** is a Vulkan-based translation layer for Direct3D 8/9/10/11 which allows running 3D applications on Linux using Wine.

For the current status of the project, please refer to the [project wiki](https://github.com/doitsujin/dxvk/wiki).

The most recent development builds can be found [here](https://github.com/doitsujin/dxvk/actions/workflows/artifacts.yml?query=branch%3Amaster).

Release builds can be found [here](https://github.com/doitsujin/dxvk/releases).

## How to use

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

Tools such as Steam Play, Lutris, Bottles, Heroic Launcher, etc will automatically handle setup of DXVK on their own when enabled.

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
- `starengine`: Shows StarEngine version, tier, persona, FSR/LSFG status, and VRAM configuration *[StarEngine]*
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
- `DXVK_DEBUG=markers|validation` Enables use of the `VK_EXT_debug_utils` extension for translating performance event markers, or to enable Vulkan validation, respective.
- `DXVK_CONFIG_FILE=/xxx/dxvk.conf` Sets path to the configuration file.
- `DXVK_CONFIG="dxgi.hideAmdGpu = True; dxgi.syncInterval = 0"` Can be used to set config variables through the environment instead of a configuration file using the same syntax. `;` is used as a separator.
- `DXVK_SHADER_CACHE=0` Disables the internal shader cache.
- `DXVK_SHADER_CACHE_PATH=/some/directory` Path to internal shader cache files. By default, this will use `%LOCALAPPDATA%/dxvk` in a Windows or Wine environment, and `$HOME/.cache` or `$XDG_CACHE_HOME` in a native Linux environment.

### Graphics Pipeline Library

On drivers which support `VK_EXT_graphics_pipeline_library` Vulkan shaders will be compiled at the time the game loads its D3D shaders, rather than at draw time. This reduces or eliminates shader compile stutter in many games when compared to the previous system.

In games that load their shaders during loading screens or in the menu, this can lead to prolonged periods of very high CPU utilization, especially on weaker CPUs. For affected games it is recommended to wait for shader compilation to finish before starting the game to avoid stutter and low performance. Shader compiler activity can be monitored with `DXVK_HUD=compiler`.

**Note:** Games which only load their D3D shaders at draw time (e.g. most Unreal Engine games) will still exhibit some stutter, although it should still be less severe than without this feature.

## Build instructions

In order to pull in all submodules that are needed for building, clone the repository using the following command:
```
git clone --recursive https://github.com/doitsujin/dxvk.git
```

### Requirements:
- [wine 7.1](https://www.winehq.org/) or newer
- [Meson](https://mesonbuild.com/) build system (at least version 0.58)
- [Mingw-w64](https://www.mingw-w64.org) compiler and headers (at least version 10.0)
- [glslang](https://github.com/KhronosGroup/glslang) compiler

### Building DLLs

#### The simple way
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

#### Compiling manually
```
# 64-bit build. For 32-bit builds, replace
# build-win64.txt with build-win32.txt
meson setup --cross-file build-win64.txt --buildtype release --prefix /your/dxvk/directory build.w64
cd build.w64
ninja install
```

The D3D8, D3D9, D3D10, D3D11 and DXGI DLLs will be located in `/your/dxvk/directory/bin`.

### Build troubleshooting

DXVK requires threading support from your mingw-w64 build environment. If you are missing this, you may see "error: 'std::cv_status' has not been declared" or similar threading related errors.

On Debian and Ubuntu, this can be resolved by using the posix alternate, which supports threading. For example, choose the posix alternate from these commands:
```
update-alternatives --config x86_64-w64-mingw32-gcc
update-alternatives --config x86_64-w64-mingw32-g++
update-alternatives --config i686-w64-mingw32-gcc
update-alternatives --config i686-w64-mingw32-g++
```
For non debian based distros, make sure that your mingw-w64-gcc cross compiler does have `--enable-threads=posix` enabled during configure. If your distro does ship its mingw-w64-gcc binary with `--enable-threads=win32` you might have to recompile locally or open a bug at your distro's bugtracker to ask for it.

# DXVK Native

DXVK Native is a version of DXVK which allows it to be used natively without Wine.

This is primarily useful for game and application ports to either avoid having to write another rendering backend, or to help with port bringup during development.

[Release builds](https://github.com/doitsujin/dxvk/releases) are built using the Steam Runtime.

### How does it work?

DXVK Native replaces certain Windows-isms with a platform and framework-agnostic replacement, for example, `HWND`s can become `SDL_Window*`s, etc. All it takes to do that is to add another WSI backend.

**Note:** DXVK Native requires a backend to be explicitly set via the `DXVK_WSI_DRIVER` environment variable. The current built-in options are `SDL3`, `SDL2`, and `GLFW`.

DXVK Native comes with a slim set of Windows header definitions required for D3D9/11 and the MinGW headers for D3D9/11. In most cases, it will end up being plug and play with your renderer, but there may be certain teething issues such as:
- `__uuidof(type)` is supported, but `__uuidof(variable)` is not supported. Use `__uuidof_var(variable)` instead.

---

*StarEngine (Vegas Edition) is a downstream fork of GPLAsync/DXVK. All upstream credit belongs to ishitatsuyuki (GPLAsync) and the DXVK contributors.*

> **For developers:** See [`DEV_README.md`](DEV_README.md) for the complete file-by-file mapping of all StarEngine changes — every modified function, line number, and feature.
