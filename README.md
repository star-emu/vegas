<p align="center">
  <img alt="VEGAS" src="./vegas_banner.gif" width="80%">
  <br>
  <em>star engine rebased and rebranded; vegas</em>
</p>

# VEGAS — DXVK v2.7.3
### Adreno-Tuned DXVK for Android Emulation (Star Emulator / Winlator)

VEGAS is a specialized performance fork of DXVK targeting **Qualcomm Adreno GPUs** on mobile. It features a tier-based auto-tuning engine, FSR 1.0 compute upscaling, motion-compensated frame generation, and dynamic driver safeguards — all behind a single master switch.

---

## Key Features

### Master Switch (`dxvk.enableStarProfile`)
All VEGAS features are gated behind one Tristate option:
- **Auto** — Enable on Adreno GPUs only (default)
- **True** — Force-enable all features
- **False** — Hard-disable everything

### Tier-Based Auto-Tuning
Adreno GPUs are classified into 3 tiers from the KGSL device model:

| Tier | Adreno GPUs | Performance Target |
|------|-------------|-------------------|
| 1 | 506, 508, 509, 512, 610, 615, 616, 618, 619, 620 | GTX 1050 Ti |
| 2 | 630, 640, 642L, 650, 660, 680, 690 | GTX 1070 |
| 3 | 7xx series, 8xx series, 830, 840 | RTX 3060 |

Each tier receives tuned draw thresholds, HAAE pacing, frame generation eligibility, VRAM budgets, and governor cap multipliers automatically.

### FSR 1.0 Compute Upscaler (`vegas.enableUpscaler`)
Full FSR 1.0 EASU compute pipeline:
- **Auto** — Upscales when render resolution < swapchain resolution
- **True** — Always upscale (half-resolution quadrants)
- **False** — Disabled

Uses push constants with a 2-binding descriptor set, dispatched via blit + fence sync.

### 3-Pass Motion-Compensated Frame Generation
Available on Tier 2 (≤29 ms frametime) and Tier 3 (≤33 ms frametime):
1. **Motion estimation** — Block SAD on prev/cur frames → raw motion vectors
2. **Median filter** — 3×3 spatial denoise on motion field
3. **Warp + blend** — Warp prev frame by filtered motion, alpha-blend at 0.5

Compute-only pipeline. Disabled on Tier 1 (insufficient compute budget).

### Adaptive Governor (`tuneThreshold`)
EMA-smoothed frame-time telemetry with **15-frame cooldown** for responsive load balancing:
- **Tier 1** — Cap at 1.5× base (TBDR tile safety on low-end)
- **Tier 2** — Cap at 2.5× base (balanced)
- **Tier 3** — Cap at 3.0× base (aggressive batching)

GPU load is derived from the `frameTime / targetFrameTime` ratio (6 continuous levels from 0.25 to 0.96), enabling accurate overheating detection instead of a hardcoded estimate.

### Dynamic VRAM & GPU Mask
- `applyVramSwap()` — Sets `dxgi.maxDeviceMemory` to 40% of system RAM (clamped 1–4 GB)
- `applyGpuMask()` — Maps Adreno tier to a compatible NVIDIA vendor/device ID

### Bind Skip Optimization
Skips redundant `vkCmdBindPipeline` calls when no dynamic state has changed — reduces CPU overhead on the draw call path.

### HUD Performance Colors
Graph coloring via `getGraphColor()`:
`green` (normal) → `yellow` (lagging) → `orange` (stuttering) → `red` (overheating)

### VegasHud Overlay (`vegas.enableHud`)
Standalone dynamic overlay — independent of `DXVK_HUD`:
- Always at **top-right**, always visible
- Tier, GPU load, frame time, performance state
- Active features (FSR, frame generation)
- Numeric frame-time history (last 6 frames)
- Color-coded by performance state (green/yellow/orange/red)

Controlled by `vegas.enableHud = Auto | True | False`. Defaults to **Auto** (enabled on Adreno).

---

## Installation

### Via Star Emulator
1. Open Star Emulator
2. Go to **Contents** menu
3. Install the `dxvk-2.7.3.wcp` package

### Manual (Winlator / Other)
Place `vegas/dxvk.conf` (or the root `dxvk.conf`) in any of these paths:
- `/storage/emulated/0/Winlator/`
- `/storage/emulated/0/Download/`
- `/storage/emulated/0/`

The `vegas/` directory in this repo contains a clean, focused config file
with all Vegas options pre-configured and documented. Copy it as `dxvk.conf`
to one of the paths above.

Or set `DXVK_CONFIG_FILE` to your config path.

---

## Configuration

```ini
# Master switch: Auto (Adreno only), True (force-on), False (force-off)
dxvk.enableStarProfile = Auto

# VegasHud overlay: Auto (default on Adreno), True, False
vegas.enableHud = Auto

# FSR 1.0 upscaler: Auto, True, False
vegas.enableUpscaler = Auto

# Manual tier override (advanced): 0=auto, 1=low-end, 2=mid, 3=high-end
vegas.forceTier = 0

# Compiler thread count (advanced): 0=auto (max 4 on ARM64)
dxvk.numCompilerThreads = 0
```

All other parameters (thresholds, bind skip, HAAE pacing, quality scaling) are auto-tuned by the VEGAS engine. For a complete config reference, see `vegas/dxvk.conf` in this repository.

---

## Build from Source

```bash
git clone --recursive https://github.com/isygold/Vegas-Private.git
cd Vegas-Private

# Android cross-build (requires NDK r26+ and Meson 0.58+)
meson setup --cross-file build-android-aarch64.txt \
  --buildtype release --prefix /output/dir build
cd build
ninja install
```

The output DLLs (`d3d9.dll`, `d3d11.dll`, `dxgi.dll`, etc.) are placed in `/output/dir/bin/`.

---

## Changelog (VEGAS)

| Commit | Feature |
|--------|---------|
| `[current]` | **VegasHud:** standalone top-right overlay with `vegas.enableHud` config, tier/load/ft display, performance state colors, numeric FT history |
| `bea5128` | **Bleeding-edge:** tier-based governor caps (1.5×/2.5×/3.0×), 15-frame cooldown, real GPU load from ftRatio, HAAE threshold fix (T1:50/T2:100/T3:150), ARM64 compiler thread cap (max 4) |
| `1e2b208` | Remove per-shader zero-init log spam (3378 lines → 1 summary) |
| `c99f219` | Respect `BufferCount ≥ 2` regardless of swap effect (fixes Tomb Raider DISCARD-mode) |
| `8e5ebf9` | Include `FLIP_DISCARD` in flip-model backbuffer count check |
| `08863e2` | Use `small_vector` for temp backbuffers (avoids heap alloc on present path) |
| `8f957e5` | **Phase 1–3:** tier classifier, TBDR-safe governor, 60s shader cache flush, async+GPL compat, zero-init tier-aware, FSR upscaler, 3-pass framegen |
| `87c7c09` | Short-circuit framegen dispatch when VkDevice/VkQueue unavailable |
| `ea83294` | Add `vegas.enableUpscaler` config option to DxgiOptions |

---

## Notes

- **Tier 1 (Adreno 5xx/6xx low-end):** Frame generation disabled. FSR available but not recommended at very low resolutions.
- **VegasHud:** The overlay (`vegas.enableHud`) is independent of `DXVK_HUD` — both can be active simultaneously without conflict.
- **BCn→ASTC transcoder:** Implemented but deferred — the simplified encoder produces visual quality loss that outweighs the narrow benefit (only helps old Qualcomm blob, not Turnip). Available in code for future developers who want to integrate a proper encoder (e.g., `ispc_texcomp`).
- **Turnip driver:** Use Mesa 25.x+ with Vulkan 1.3 support for descriptor indexing and push constants.
- **Synthetic benchmarks:** May show lower FPS than stock due to draw thresholds. Judge performance by actual gameplay smoothness.
- **GPU-bound workloads:** VSync-off provides negligible gain when the GPU is already saturated (17+ ms frame times).

---

## Credits

- **Lead Developer:** isygold
- **Base Project:** DXVK v2.7.1+ by doitsujin
- **License:** zlib/libpng

---

## Upstream DXVK Reference

For desktop/Wine usage, driver notes, HUD reference, debugging, and full build instructions, see the [upstream DXVK README](https://github.com/doitsujin/dxvk). Key highlights preserved below:

### HUD (Android)
`DXVK_HUD=devinfo,fps` or `DXVK_HUD=full` in your container environment. Common options:
- `devinfo` — GPU name + driver version
- `fps` — Current frame rate
- `frametimes` — Frame time graph
- `gpuload` — Estimated GPU load
- `compiler` — Shader compiler activity
- `drawcalls` — Draw calls per frame

### Debugging (Android)
- `DXVK_LOG_LEVEL=warn` — Reduce log verbosity
- `DXVK_LOG_LEVEL=debug` — Verbose logging for troubleshooting
- `DXVK_CONFIG="dxgi.syncInterval = 0"` — Set config via environment variable

### Device Filter
`DXVK_FILTER_DEVICE_NAME="Adreno"` to select a specific Vulkan device if multiple GPUs are present.

### Anti-Cheat Warning
Modifying Direct3D libraries in multiplayer games may result in account bans. **Use at your own risk.**
