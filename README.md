<p align="center">
  <img alt="VEGAS" src="./vegas_banner.gif" width="80%">
  <br>
  <em>star engine rebased and rebranded; vegas</em>
</p>

# VEGAS — GPLAsync/DXVK v2.7.3
[![Sponsor](https://img.shields.io/badge/Sponsor-%24?logo=github&style=flat)](https://github.com/sponsors/isygold)

### Adreno-Tuned DXVK (via GPLAsync) for Android Emulation (Star Emulator / Winlator)

VEGAS is a specialized performance fork of GPLAsync 2.7.1 (itself a DXVK fork) targeting **Qualcomm Adreno GPUs** on mobile. It features a tier-based auto-tuning engine, FSR 1.0 compute upscaling, motion-compensated frame generation, and dynamic driver safeguards — all behind a single master switch.

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

### FSR 1.0 Compute Upscaler — Async Dispatch (`vegas.enableUpscaler`)
Full FSR 1.0 EASU compute pipeline with **non-blocking async dispatch**:
- **Auto** — Upscales when render resolution < swapchain resolution
- **True** — Always upscale (half-resolution quadrants)
- **False** — Disabled

Uses leegao's **DxvkFence timeline semaphore** for zero-CPU-blocking dispatch:
- `fsrUpscaleAsync()` submits EASU compute, signals DxvkFence, returns immediately
- `fsrTryBlitResult()` non-blocking `getValue()` check → ~0.1ms sync blit
- `fsrDrain()` blocking wait only on resize (never on hot path)

Eliminates 0.5-1.0ms CPU stall on every upscaled frame. GPU load improved
**40-60% → 70-85%** on low-end Adreno (Tomb Raider 2013 validated).

### 3-Pass Motion-Compensated Frame Generation
Available on Tier 2 (≤29 ms frametime) and Tier 3 (≤33 ms frametime):
1. **Motion estimation** — Block SAD on prev/cur frames → raw motion vectors
2. **Median filter** — 3×3 spatial denoise on motion field
3. **Warp + blend** — Warp prev frame by filtered motion, alpha-blend at 0.5

Compute-only pipeline. Disabled on Tier 1 (insufficient compute budget).

### GPU BCn→ASTC Compute Transcoder (`release-v2` branch)
Two-pass compute pipeline that converts BCn (DXT) compressed textures to ASTC 4×4
before GPU upload — entirely on-device, no CPU transcoding:
- **Pass 1** — `vegas_bcn_decode.comp` decodes BC1–BC7 blocks to RGBA8 texels (scratch SSBO)
- **Pass 2** — `astc_enc_leegao.comp` (PCA-based) re-encodes RGBA8 → ASTC 4×4 blocks in-place
- Covers **BC1 through BC7** (BC1/BC4 use 2× staging allocation for 8→16 B/block expansion)
- Blocking submit (`vkWaitForFences`) per mip/layer before upload copy — zero DXVK state touched
- Requires Mesa Turnip with ASTC decode support; no benefit on Qualcomm proprietary blob
- File: `src/dxvk/dxvk_vegas.cpp` → `gpuTranscodeImageData()` (~line 1950)

### Adaptive Governor — TBDR-Inverted (`tuneThreshold`)
EMA-smoothed frame-time telemetry with **15-frame cooldown** for responsive load balancing:
- **GPU-bound** (load>0.90, ft>25ms) → **RAISE** threshold (batch more, amortize submission overhead)
- **CPU-bound** (load<0.40, ft>12ms) → **LOWER** threshold (flush earlier, TBDR tile pacing)
- **Balanced** → reset to base

**Why inverted?** Desktop DXVK raises the threshold for BOTH CPU-bound and GPU-bound
scenarios. On TBDR Adreno, raising the threshold when CPU-bound makes the problem
worse — more draws accumulate in the tile buffer, increasing driver overhead and
starving the GPU. The inverted path correctly **reduces** the threshold when
CPU-bound to force earlier flushes.

**Cap multipliers:** T1=2.0× (100→200), T2=2.0× (200→400), T3=1.7× (350→595)

GPU load is derived from the `frameTime / targetFrameTime` ratio (6 continuous levels
from 0.25 to 0.96). Planned replacement with real `gpuIdleTicks()` (Fix 3).

### Dynamic VRAM & GPU Mask
- `applyVramSwap()` — Sets `dxgi.maxDeviceMemory` to 40% of system RAM (clamped 1–4 GB)
- `applyGpuMask()` — Maps Adreno tier to a compatible NVIDIA vendor/device ID

### Bind Skip Optimization
Skips redundant `vkCmdBindPipeline` calls when no dynamic state has changed — reduces CPU overhead on the draw call path.

### HUD Performance Colors
The upstream DXVK **frametime graph** (`DXVK_HUD=frametimes`) is color-coded by
performance state in real time:
- **Green** (Normal) — smooth sailing
- **Yellow** (Lagging) — frame time exceeds 1.5× target
- **Orange** (Stuttering) — frame-to-frame delta > 1.25× target
- **Red** (Overheating) — GPU load ≥95% AND frame time ≥ 3× target

The current state label (NORMAL / LAGGING / STUTTERING / OVERHEATING) is drawn
at the top-left of the graph in the same color. This replaces the standalone
VegasHud overlay — no separate HUD configuration needed.

---

## Installation

### WCP Package Types
Each release provides **two** WCP packages with identical DLLs but different metadata:

| Package | `type` field | For |
|---------|-------------|-----|
| `dxvk-2.7.3-vegas-*.wcp` | `"DXVK"` | **Stock Winlator** and general Android DXVK use |
| `vegas-2.7.3-*.wcp` | `"VEGAS"` | **Star Emulator** (latest build) |

### Via Star Emulator
1. Open Star Emulator
2. Go to **Contents** menu
3. Install the `vegas-2.7.3-*.wcp` package (VEGAS-native type)

### Via Stock Winlator
1. Download the `dxvk-2.7.3-vegas-*.wcp` package
2. Install it as a standard DXVK WCP package in Winlator

### Manual Configuration
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

| Branch | Commit | Feature |
|--------|--------|---------|
| `release-v2` | `19c9a86` | **Batch 5/5 — BC1/BC4 staging buffer resize.** 2× staging allocation for 8→16 B/block ASTC expansion. All BCn formats now transcode. |
| `release-v2` | `12b00bb` | **Batch 4/5 — Upload path wiring.** `getAstcFormat()` → 4×4 exclusively. `InitDeviceLocalTexture()` calls `gpuTranscodeImageData()` after pack for BC3/5/7. |
| `release-v2` | `bb7b8b1` | **Batch 3/5 — Format swap in createImage.** `DxvkImageCreateInfo::originalFormat` stashed; image format swapped to ASTC when eligible. |
| `release-v2` | `766b8c1` | **Batch 2/5 — gpuTranscodeImageData dispatch.** Two-pass compute: decode BCn→RGBA8, encode RGBA8→ASTC 4×4. Independent submit + fence wait. |
| `release-v2` | `fc06f55` | **Batch 1/5 — Pipeline init infrastructure.** `createStaticSsbo()`, `initTranscoderPipeline()`, decode + encode pipelines with separate DSLs. |
| `release-v2` | `41bb85b` | **Step 2 — leegao's ASTC encoder.** `astc_enc_leegao.comp` (PCA-based, 4×4) compiled to SPIR‑V. LUT data (47 KB total) as C headers. |
| `release-v2` | `22d4ba5` | **Step 1 — BCn decode shader.** `vegas_bcn_decode.comp` compiled to SPIR‑V v1.5, C header generated. |
| `vegas` | `HEAD` | **Remove VegasHud overlay.** Color-coded frametime graph replaces standalone overlay. `DXVK_HUD=frametimes` now shows dynamic colors (green→yellow→orange→red) and state label |
| `07914da` | **VegasHud:** positioning fix (char width 8→10.5px), ASCII bar graph (20-bar × 4-level), leegao credits |
| `24e48a3` | **Stable release:** WCP versionCode 0→1, WCP CI checkout fix |
| `735e09e` | **WCP CI:** remove vkResetCommandBuffer from async FSR (not in FsrVulkanFuncs) |
| `8128a1b` | **Fix 2 + Fix 1 + Fix 4:** Async FSR via DxvkFence (leegao), TBDR-aware thresholds (T1=100/T2=200/T3=350), inverted TBDR governor (lower when CPU-bound) |
| `bea5128` | **Bleeding-edge:** tier-based governor, 15-frame cooldown, GPU load from ftRatio, HAAE thresholds (T1:30/T2:50/T3:80), ARM64 compiler thread cap (max 4), zero-init tier-aware |
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
- **Performance state coloring:** The upstream frametime graph (`DXVK_HUD=frametimes`) now reflects the current Vegas performance state in real time — no separate HUD overlay needed.
- **GPU BCn→ASTC transcoder (release-v2):** Active two-pass compute pipeline on `release-v2` branch. BCn→RGBA8 decode + leegao's PCA-based ASTC 4×4 encoder runs before GPU upload. Covers BC1–BC7. Requires Mesa Turnip (proprietary blob does not support ASTC decode). See "GPU BCn→ASTC Compute Transcoder" under Key Features.
- **Turnip driver:** Use Mesa 25.x+ with Vulkan 1.3 support for descriptor indexing and push constants.
- **Synthetic benchmarks:** May show lower FPS than stock due to draw thresholds. Judge performance by actual gameplay smoothness.
- **GPU-bound workloads:** VSync-off provides negligible gain when the GPU is already saturated (17+ ms frame times).

---

## Credits

- **Lead Developer:** isygold

- **Lead Developer:** isygold
- **Base Project:** DXVK v2.7.1 by doitsujin
- **Upstream Parent:** GPLAsync v2.7.1 by ishitatsuyuki (async pipeline compilation foundation)
- **Timeline Semaphore (DxvkFence):** leegao — enabled non-blocking async FSR dispatch on Turnip
- **ASTC GPU Encoder (astc_enc_leegao):** leegao — PCA-based RGBA8→ASTC 4×4 compute shader with 2-partition mode support; ported and integrated by isygold
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
