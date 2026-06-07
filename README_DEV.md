# VEGAS Developer Guide — DXVK v2.7.3 Fork

Welcome to the VEGAS (formerly Star Engine) developer documentation.
This guide is for DXVK developers working on the VEGAS codebase — it maps
every feature to its exact file, function, and line numbers so you can
navigate and modify the code efficiently.

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [Source Map: Every File & What It Does](#2-source-map)
3. [Feature Reference: Lines & Functions](#3-feature-reference)
   - 3.1  Tier Classification
   - 3.2  Adaptive Governor
   - 3.3  GPU Pacing (HAAE)
   - 3.4  Pipeline Bind-Skip
   - 3.5  Shader Zero-Init
   - 3.6  Shader Cache Periodic Flush
   - 3.7  Compiler Thread Cap
   - 3.8  Swapchain Buffer Count Fix
   - 3.9  FSR 1.0 Upscaler
   - 3.10 Frame Generation (3-Pass)
    - 3.11 BCn→ASTC Transcoder (Gated)
    - 3.12 GPU Persona & VRAM Masking
    - 3.13 Performance Analysis & Logging
    - 3.14 VegasHud Overlay
4. [Config Options Reference](#4-config-options)
5. [Adding a New Feature](#5-adding-a-new-feature)
6. [Testing Methodology](#6-testing-methodology)
7. [Common Pitfalls](#7-common-pitfalls)

---

## 1. Architecture Overview

VEGAS extends DXVK with ~55 static functions and ~40 static variables that
modify Vulkan command buffer dispatch, swapchain behavior, shader compilation,
GPU pacing, and HUD rendering — all gated behind a single master switch
(`dxvk.enableStarProfile`).

### Integration Points

```
 Game (D3D11/D3D9)
     │
     ▼
 ┌──────────────────────────────┐
 │  d3d11_swapchain.cpp         │  backbuffer creation fix
 │  dxgi_swapchain.cpp          │  async FSR (tryBlit→upscaleAsync),
 │                              │  framegen, governor, aspect ratio,
 │                              │  pushMetrics() for VegasHud
 ├──────────────────────────────┤
 │  dxvk_context.cpp            │  draw()/drawIndexed() flush, bindSkip
 │  dxvk_device.cpp             │  HAAE submission throttle
 │  dxvk_pipemanager.cpp        │  compiler thread cap
 │  dxvk_shader_cache.cpp       │  periodic cache flush
 │  dxvk_swapchain_blitter.cpp  │  VegasHud rendering (composite + direct)
 ├──────────────────────────────┤
 │  dxvk_vegas.cpp              │  ALL feature logic, decision helpers,
 │  dxvk_vegas.h                │  metrics push + FT history ring buffer
 │  dxvk_vegas_hud.cpp          │  VegasHud overlay: text + ASCII bar graph
 │  dxvk_vegas_hud.h            │  VegasHud class declaration
 │  dxvk_fence.h/.cpp           │  DxvkFence timeline semaphore (leegao)
 │  dxvk_options.h/.cpp         │  config option declarations
 ├──────────────────────────────┤
 │  star_fsr_spv.h              │  FSR 1.0 EASU SPIR-V
 │  star_fg_spv.h               │  Framegen 3-pass SPIR-V
 └──────────────────────────────┘
```

### Data Flow

```
InitializeProfile(DxvkDevice*)
  → detect Adreno, classify tier, bake thresholds
  → store VkDevice/VkQueue for FSR/FG
  → called once from initVegasProfile() in DxvkContext

Per-Frame (PresentBase):
  measure frameTime
  → compute GPU load from frameTime/target ratio
  → analyzePerformance() → tuneThreshold() → TBDR-inverted governor adjusts draw
     threshold: raises when GPU-bound (load>0.90, ft>25ms), lowers when CPU-bound
     (load<0.40, ft>12ms), resets to base on balanced load
  → shouldUpscale() → fsrUpscaleAsync() (non-blocking EASU compute, signals
     DxvkFence timeline semaphore, returns immediately)
  → fsrTryBlitResult() (non-blocking getValue() check, blits completed
     intermediate → swapchain, ~0.1ms sync blit)
  → needsFrameGen() → framegenDispatch() if eligible
  → pushMetrics() → stores gpuLoad, frameTime, perfState, fsrActive, fgActive
                     in Vegas static members for VegasHud consumption

Per-Present (DxvkSwapchainBlitter::present):
  → m_hud->update/render()          (DXVK HUD, top-left)
  → m_vegasHud->render()            (VegasHud, top-right, reads Vegas statics)

Per-Draw (draw/drawIndexed):
  shouldFlush(drawCount) → spill render pass + flush command list if over threshold
  shouldSkipBind() → skip vkCmdBindPipeline if same handle

Per-Submit (submitCommandList):
  shouldSubmitHaae() → inject empty fence submit for GPU pacing
```

---

## 2. Source Map

### Core Vegas Module

| File | Purpose | Key Contents |
|------|---------|--------------|
| `src/dxvk/dxvk_vegas.h` | All declarations | `Vegas` class (55+ static methods), `VegasProfile` struct, `VegasPerformanceState` enum, `VegasFsrConstants` struct, 40+ static member variables including FT history ring buffer |
| `src/dxvk/dxvk_vegas.cpp` | All implementations | ~3650 lines. Tier classifier, TBDR-inverted governor, async FSR via DxvkFence, FG dispatch, BCn→ASTC transcoder, pushMetrics() + getters, static variable definitions, anonymous namespace helpers |
| `src/dxvk/dxvk_vegas_hud.h` | VegasHud declaration | `VegasHud` class with own `HudRenderer` instance, config-aware enable, color constants |
| `src/dxvk/dxvk_vegas_hud.cpp` | VegasHud implementation | ~216 lines. 5-element right-aligned overlay: header, tier/load/ft, perf state + features, numeric FT history, ASCII bar graph |
| `src/dxvk/dxvk_fence.h` | DxvkFence timeline semaphore (leegao) | Non-blocking GPU completion check via `getValue()` for async FSR |
| `src/dxvk/dxvk_fence.cpp` | DxvkFence implementation | Timeline semaphore wrapper with `getValue()`, `wait()`, `handle()` |

### DXVK Integration Points

| File | Lines | What Vegas Does There |
|------|-------|-----------------------|
| `src/dxvk/dxvk_context.cpp` | 83-103, 839-862, 949-973, 5891-5904, 9500-9519 | HUD version override, draw/drawIndexed flush, bindSkip, initVegasProfile |
| `src/dxvk/dxvk_context.h` | 830-834 | `m_vegasProfile`, `m_drawsSinceSubmit` (atomic), `initVegasProfile()`, `checkAsyncCompilationCompat()` |
| `src/dxvk/dxvk_device.cpp` | 628-636 | HAAE submission throttle in `submitCommandList()` |
| `src/dxvk/dxvk_pipemanager.cpp` | 95-102 | ARM64 compiler thread cap |
| `src/dxvk/dxvk_shader_cache.cpp` | 444-502 | 60s periodic flush in `runWriter()` |
| `src/dxvk/dxvk_fence.h` | all | DxvkFence timeline semaphore class declaration |
| `src/dxvk/dxvk_fence.cpp` | all | Timeline semaphore wrapper: `getValue()`, `wait()`, `handle()` |
| `src/dxvk/dxvk_swapchain_blitter.h` | 9, 214, 234 | `#include "dxvk_vegas_hud.h"`, `unique_ptr<VegasHud>` member |
| `src/dxvk/dxvk_swapchain_blitter.cpp` | 10, 16-18, 72, 117-119, 392-395, 423-425, 434-435 | VegasHud creation in constructor, render calls in present() and renderHudImage() |
| `src/dxvk/dxvk_options.h` | 76-91 | Vegas config options (including vegas.enableHud) |
| `src/dxvk/dxvk_options.cpp` | 26-29 | Config parsing |

### DXGI/D3D11 Integration Points

| File | Lines | What Vegas Does There |
|------|-------|-----------------------|
| `src/dxgi/dxgi_swapchain.cpp` | 348-503 | frame timing + governor (unconditional analysis), FSR dispatch, framegen dispatch, pushMetrics() for VegasHud |
| `src/dxgi/dxgi_swapchain.h` | 205-210 | Vegas state members (m_lastPresentTime, m_lastPerfState, m_needsFrameGen, etc.) |
| `src/dxgi/dxgi_options.h` | 63-64 | `vegasEnableUpscaler` + `vegasEnableHud` Tristate options |
| `src/d3d11/d3d11_swapchain.cpp` | — | Backbuffer count fix (respect BufferCount ≥2) |

### SPIR-V Shaders

| File | Purpose |
|------|---------|
| `src/dxvk/star_fsr_spv.h` | FSR 1.0 EASU compute shader (dxvk_fsr_easu_code[]) |
| `src/dxvk/star_fg_spv.h` | 3-pass framegen shaders: dxvk_fg_motion_code[], dxvk_fg_median_code[], dxvk_fg_warp_code[] |

---

## 3. Feature Reference

### 3.1 Tier Classification

**File:** `src/dxvk/dxvk_vegas.cpp`

| Function | Lines | Purpose |
|----------|-------|---------|
| `classifyAdrenoTier(const char* deviceName)` | 89-122 | Parses device name → tier 1/2/3 |
| `Vegas::initializeProfile(DxvkDevice*)` | 893-989 | Master init: calls classifyAdrenoTier, applies vegasForceTier override |
| `Vegas::getTier()` | 995 | Returns `s_tier` |

**Tier Mapping (TBDR-aware — halved from desktop defaults):**

| Condition | Tier | Draw Threshold (base) | HAAE Threshold | Cap Multiplier |
|-----------|------|----------------------|----------------|----------------|
| gen ≤ 5, or 6xx < 620 | 1 (entry) | 100 | 30 | 2.0× |
| 6xx 620-689, or 7xx < 730 | 2 (mid) | 200 | 50 | 2.0× |
| 690+, 7xx ≥ 730, or 8xx+ | 3 (high) | 350 | 80 | 1.7× |

**Config override:** `vegas.forceTier = 0` (auto), `1`/`2`/`3` (manual)

**D3D9 override:** D3D9 games issue more draw calls per frame; thresholds are
higher but still TBDR-aware: `{300, 500, 800}` for D3D9 games.

---

### 3.2 Adaptive Governor (TBDR-Inverted)

**File:** `src/dxvk/dxvk_vegas.cpp`

| Function | Lines | Purpose |
|----------|-------|---------|
| `tuneThreshold(uint32_t&, float, float, uint32_t)` | 209-243 | TBDR-inverted governor: lowers threshold when CPU-bound, raises when GPU-bound |
| `tuneThreshold(float, float)` | 247-269 | Self-contained: EMA smoothing + 15-frame cooldown → delegates to 4-arg |

**Called from:** `src/dxgi/dxgi_swapchain.cpp` line 378 (`PresentBase`)

**Governor Logic (TBDR-inverted):**
```
if (load > 0.90f AND frameTime > 25.0f)   → cap (GPU-bound, RAISE threshold)
                                           → batching more amortizes submission overhead
if (load < 0.40f AND frameTime > 12.0f)    → floor (CPU-bound, LOWER threshold)
                                           → over-batching starves TBDR; flush earlier
else                                       → base (balanced, reset)
```

In desktop DXVK, the governor raises the threshold for both
CPU-bound AND GPU-bound scenarios. On TBDR Adreno, raising the
threshold when CPU-bound makes the problem WORSE — more draws
accumulate in the tile buffer, increasing driver overhead and
starving the GPU. The inverted path (load<0.40, ft>12ms) correctly
**reduces** the threshold to force earlier flushes.

**Cap Multipliers:**
- Tier 1: 2.0× (100 → 200 max)
- Tier 2: 2.0× (200 → 400 max)
- Tier 3: 1.7× (350 → 595 max)

**Floor (CPU-bound flush):** `max(50, base/2)` — ensures the GPU
starts tiling early when the CPU is the bottleneck.

**EMA Smoothing:** `s_smoothFt = s_smoothFt * 0.9 + frameTime * 0.1`
**Cooldown:** 15 frames (~250ms at 60fps)

---

### 3.3 GPU Pacing (HAAE)

**File:** `src/dxvk/dxvk_vegas.cpp`

| Function | Lines | Purpose |
|----------|-------|---------|
| `shouldSubmitHaae(uint32_t&, uint32_t)` | 1009-1016 | Returns true when accumulated draws ≥ threshold |

**Called from:** `src/dxvk/dxvk_device.cpp` line 632 (`submitCommandList`)

**Threshold per tier:** `{50, 100, 150}` — Tier 1 gets MOST frequent pacing.

**Behavior:** When triggered, submits an empty `DxvkSubmitInfo` with a fence.
This acts as a GPU pacemaker, preventing the submission queue from growing
too large on TBDR architectures.

---

### 3.4 Pipeline Bind-Skip

**File:** `src/dxvk/dxvk_vegas.cpp`

| Function | Lines | Purpose |
|----------|-------|---------|
| `shouldSkipBind()` | 1005-1007 | Returns `s_enabled && s_bindSkipEnabled` |
| `isBindSkipEnabled()` | 992 | Returns `s_bindSkipEnabled` |

**Enabled:** On Adreno (set during `initializeProfile`, lines 920/933)
**Checked in:** `src/dxvk/dxvk_context.cpp` line 5891 (`updateGraphicsPipelineState`)

**What it skips:** `vkCmdBindPipeline` when `pipelineInfo.handle == m_vegasProfile.lastBoundVkPipeline`
AND `GpDirtyPipelineState` flag is NOT set.

**State tracking:** `m_vegasProfile.lastBoundVkPipeline` is reset to `VK_NULL_HANDLE`
on `beginRecording()` (line 124) and `flushCommandList()` (line 187).

---

### 3.5 Shader Zero-Init

**File:** `src/dxvk/dxvk_vegas.cpp`

| Function | Lines | Purpose |
|----------|-------|---------|
| `shouldZeroInit(uint32_t tier)` | 233-235 | Returns `tier < 3` |

**Behavior:**
- Tier 1/2 → zero-init ON (safety against Turnip hangs from uninitialized workgroup memory)
- Tier 3 → zero-init OFF (~1-2% shader performance gain)

---

### 3.6 Shader Cache Periodic Flush

**File:** `src/dxvk/dxvk_shader_cache.cpp`

| Function | Lines | Purpose |
|----------|-------|---------|
| `runWriter()` | 444-502 | Writer thread loop |

**The Vegas change:** Lines 454-461 — the `wait_for` uses a 60-second timeout:
```cpp
m_writeCond.wait_for(lock, std::chrono::seconds(60), ...)
```

This replaces the indefinite `wait()` from upstream, ensuring that partially-filled
cache batches are flushed even if the emulator is killed (SIGKILL). Previously,
all pending writes were lost on unclean shutdown.

---

### 3.7 Compiler Thread Cap

**File:** `src/dxvk/dxvk_pipemanager.cpp`

| Function | Lines | Purpose |
|----------|-------|---------|
| `startWorkers()` | 83-131 | Spawns shader compiler threads |

**The Vegas change:** Lines 95-102 (`#ifdef __aarch64__`):
```cpp
if (m_device->config().numCompilerThreads <= 0)
    workerCount = std::min(workerCount, 4u);
```

Caps auto-detected compiler threads to 4 on ARM64 to prevent CPU contention
on big.LITTLE SoCs. User override via `dxvk.numCompilerThreads` is still
respected (checked at line 104).

---

### 3.8 Swapchain Buffer Count Fix

**File:** `src/d3d11/d3d11_swapchain.cpp`

| Function | Lines | Purpose |
|----------|-------|---------|
| `CreateBackBuffers()` | — | Creates D3D11 backbuffers for the swapchain |

**The fix:** Changed backbuffer allocation to check `m_desc.BufferCount >= 2`
directly instead of checking the swap effect type. Previously, only flip-model
effects (FLIP_DISCARD, FLIP_SEQUENTIAL) got multiple buffers; DISCARD mode
with BufferCount=2 was treated as single-buffer, causing "GetImage: Invalid
buffer ID" errors (e.g., Tomb Raider: 2813 errors → 0).

**Also uses** `small_vector<Com<D3D11Texture2D, false>, 4>` for the temp
backbuffer array and atomically swaps via `std::move` to prevent race
conditions on resize.

---

### 3.9 FSR 1.0 Upscaler — Async Dispatch

**File:** `src/dxvk/dxvk_vegas.cpp`, `src/dxvk/dxvk_fence.h/.cpp`

| Function | Lines | Purpose |
|----------|-------|---------|
| `Vegas::fsrUpscaleAsync(...)` | 1409-1776 | **Async** EASU compute dispatch — signals DxvkFence, returns immediately |
| `Vegas::fsrTryBlitResult(...)` | 1300-1350 | Non-blocking `getValue()` check → blits completed intermediate to swapchain |
| `Vegas::fsrDrain(...)` | 1355-1390 | Blocking wait for in-flight async compute (called by `ensureFsrIntermediate` on resize) |
| `Vegas::calculateFsrConstants(...)` | 335-340 | Computes EASU push constants |
| `Vegas::shouldUpscale(Tristate, ...)` | 1018-1025 | Resolves Tristate + extent check |
| `initFsrPipeline(VkDevice)` | 1186-1305 | Creates FSR compute pipeline (one-time) |
| `ensureFsrIntermediate(VkDevice, VkExtent3D)` | 1311-1406 | Manages intermediate storage image |

**Called from:** `src/dxgi/dxgi_swapchain.cpp` lines 399-448 (`PresentBase`)

**Async dispatch pattern (leegao's DxvkFence):**

1. **`fsrUpscaleAsync()`** submits EASU compute with a timeline semaphore signal,
   then returns *immediately* — does NOT wait for GPU completion.
2. **`fsrTryBlitResult()`** on the *next* frame: non‑blocking `getValue()` check
   → if the compute shader finished, blits the intermediate image to swapchain
   (~0.1ms sync blit). If not yet done, skips the blit (1‑frame upscale latency).
3. **`fsrDrain()`** does a blocking `wait()` — called only on resize to safely
   destroy the intermediate image while no compute is in flight.

The `getValue()`‑only hot‑path avoids **Turnip‑kgsl timeline emulation bug**
(naive `wait()` over‑waits on intermediate values). This gives:
- **Zero CPU blocking** on the hot path
- **~0.1ms sync blit** vs 0.5‑1.0ms synchronous EASU
- **GPU load improved 40‑60% → 70‑85%** on Tomb Raider 2013 (low‑end Adreno)

**Persistent command pool/buffer:** The async submit uses a dedicated command
pool and buffer that live across frames — destroying a pool while a submitted
command buffer is pending is illegal per Vulkan spec.

**DxvkFence API:**
- `getValue()` — non‑blocking completion check (hot path)
- `wait(value)` — blocking wait (resize only)
- `handle()` — raw `VkSemaphore` for `VkSubmitInfo` pNext

**Pipeline:** FSR 1.0 EASU compute → intermediate image (R8G8B8A8_UNORM,
STORAGE+TRANSFER_SRC) → blit to swapchain image.

**Guard:** Only dispatches on UNORM swapchain formats. Fail-closed (returns
false on any error, never crashes the frame).

**Config:** `vegas.enableUpscaler = Auto` (upscales when src.width < dst.width)

**Intermediate image:** Created in device-local memory. Recreated on resolution
change. Destroyed on FSR teardown. `fsrDrain()` ensures no compute is in flight
before destruction.

---

### 3.10 Frame Generation (3-Pass)

**File:** `src/dxvk/dxvk_vegas.cpp`

| Function | Lines | Purpose |
|----------|-------|---------|
| `Vegas::framegenDispatch(...)` | 2141-2911 | Full 3-pass motion-compensated FG |
| `Vegas::isFrameGenReady()` | 2126-2128 | Returns `s_device != nullptr` |
| `Vegas::needsFrameGen(float, uint32_t)` | 329-333 | Tier-based eligibility |
| `initFgPipeline(VkDevice)` | 1817-1969 | Creates 3 compute pipelines (one-time) |
| `ensureFgIntermediateImages(...)` | 1975-2118 | Manages 4 intermediate images |

**Called from:** `src/dxgi/dxgi_swapchain.cpp` lines 450-482 (`PresentBase`)

**3 Passes:**
1. **Motion search** (FG_PASS_MOTION: 16×16 tiles) — block SAD between prev/cur frames
2. **Median filter** (FG_PASS_MEDIAN: 8×8 tiles) — 3×3 spatial denoise on motion field
3. **Warp + blend** (FG_PASS_WARP: 8×8 tiles) — warp prev frame by filtered motion, blend at weight 0.5

**Eligibility:**
- Tier 1: never (compute budget insufficient)
- Tier 2: frameTime ≤ 29ms (≥34 FPS headroom)
- Tier 3: frameTime ≤ 33ms (≥30 FPS headroom)

**Guards:** Only on UNORM formats, only when VkDevice/VkQueue are valid,
fail-closed on any error.

---

### 3.11 BCn→ASTC Transcoder (Gated)

**File:** `src/dxvk/dxvk_vegas.cpp`

**Status:** ⚠️ GATED — code is implemented but NOT wired into the upload pipeline.
See comment at lines 499-518 for rationale.

| Component | Lines | Purpose |
|-----------|-------|---------|
| `formatIsBcn(VkFormat)` | 387-409 | Returns true for all 16 BCn formats |
| `getAstcFormat(VkFormat)` | 411-449 | Maps BCn→ASTC (BC1→6×6, BC2/3/5/7→5×5, BC4→6×6) |
| `shouldTranscodeFormat(...)` | 456-487 | Returns ASTC format if eligible (usage, size, support checks) |
| `decodeBC1(...)` | 523-549 | CPU decoder for BC1 (4×4 from 8 bytes) |
| `decodeBC3(...)` | 574-580 | CPU decoder for BC3 (BC1 + BC4 alpha) |
| `decodeBC4(...)` | 570-572 | CPU decoder for BC4 (single-channel) |
| `decodeBC5(...)` | 582-590 | CPU decoder for BC5 (two-channel) |
| `decodeBC7(...)` | 592-619 | CPU decoder for BC7 (mode 0) |
| `encodeAstcBlock(...)` | 684-775 | Simplified ASTC encoder (1 partition, LDR) |
| `transcodeImageData(...)` | 779-886 | Full BCn→ASTC conversion: decode→pixels→encode |

**To activate, you would need to:**
1. In `DxvkDevice::createImage()`: swap `createInfo.format` to the ASTC format
2. In `DxvkContext::uploadImageFb|Hw()`: call `transcodeImageData()` on staging buffer
3. Verify block-size alignment on real Adreno 6xx/7xx hardware

**Block-size risk:** BCn is always 4×4 blocks. ASTC uses 5×5 or 6×6 blocks.
Most game textures are power-of-2 (512, 1024, 2048) which are NOT multiples
of 5 or 6. The partial edge texels at the right/bottom may cause issues on
Turnip. This must be verified on hardware before activation.

---

### 3.12 GPU Persona & VRAM Masking

**File:** `src/dxvk/dxvk_vegas.cpp`

| Function | Lines | Purpose |
|----------|-------|---------|
| `applyGpuMask(Config&)` | 285-323 | Maps Adreno tier → NVIDIA vendor/device ID |
| `applyVramSwap(Config&)` | 272-282 | Sets VRAM to 40% of system RAM |

**GPU Persona Mapping:**

| Tier | NVIDIA Persona | Vendor ID | Device ID |
|------|---------------|-----------|-----------|
| 1 | GTX 1050 Ti | 10de | 1c82 |
| 2 | GTX 1070 | 10de | 1b81 |
| 3 | RTX 3060 | 10de | 2503 |

**Sources:** Reads `/sys/class/kgsl/kgsl-3d0/gpu_model` on Android (sysfs).

**VRAM:** Sets `dxgi.maxDeviceMemory` to 40% of total RAM (clamped 1-4 GB),
`dxgi.maxSharedMemory` to half that.

---

### 3.13 Performance Analysis & Logging

**File:** `src/dxvk/dxvk_vegas.cpp`

| Function | Lines | Purpose |
|----------|-------|---------|
| `analyzePerformance(float, float, float)` | 343-363 | Classifies frame as Normal/Lagging/Stuttering/Overheating |
| `getGraphColor(VegasPerformanceState)` | 365-373 | Maps state → HEX color |
| `getStatusString(VegasPerformanceState)` | 376-384 | Maps state → "NORMAL" string |

**Thresholds:**
- Overheating: load ≥ 0.95 AND frameTime ≥ 3.0× target
- Stuttering: frame-to-frame delta > 1.25× target
- Lagging: frameTime ≥ 1.5× target
- Normal: everything else

**GPU Load Estimate** (in `PresentBase`, `dxgi_swapchain.cpp` lines 355-371):

⚠️ *Current metric: ftRatio-based — planned for replacement with real GPU idle
ticks (`DxvkSubmissionQueue::gpuIdleTicks()`). The ftRatio proxy works well
enough for the TBDR-inverted governor to make correct decisions (Fix 4), but
the HUD load percentage is an approximation.*
```
ftRatio = frameTime / targetFrameTime
> 2.0  → 0.96 (overheating)
> 1.5  → 0.92 (badly lagging)
> 1.2  → 0.85 (saturated)
> 0.9  → 0.65 (near capacity)
> 0.5  → 0.40 (some headroom)
else   → 0.25 (lots of headroom)
```

**Planned enhancement (Fix 3):** Replace with `gpuIdleTicks()` delta for
accurate load display and governor input. Requires plumbing the tick delta
through the present path and storing previous tick for per-frame computation.

**Log Line Format:**
```
Vegas: Perf=LAGGING ftRatio=1.5 load=0.92 frameTime=25.0ms frameGen=no
```

---

### 3.14 VegasHud Overlay

**New files:** `src/dxvk/dxvk_vegas_hud.h`, `src/dxvk/dxvk_vegas_hud.cpp`

**Wired into:** `src/dxvk/dxvk_swapchain_blitter.cpp` (present path)

| Component | Location | Purpose |
|-----------|----------|---------|
| `VegasHud` class | `dxvk_vegas_hud.h:16-65` | Standalone overlay with own `HudRenderer`, config-aware enable |
| `VegasHud::render()` | `dxvk_vegas_hud.cpp:35-190` | Renders 4-line right-aligned overlay + compact ASCII bar graph: header, tier/load/ft, perf state + features, numeric FT history, frame-time bar chart |
| `Vegas::pushMetrics()` | `dxvk_vegas.cpp:2950-2964` | Called from `PresentBase` — stores gpuLoad, frameTime, perfState, fsrActive, fgActive |
| `Vegas::s_ftHistory[]` | `dxvk_vegas.cpp:80` | Circular buffer of last 60 frame times for HUD consumption |

**Data Pipeline:**
```
dxgi_swapchain.cpp:PresentBase
  → Vegas::pushMetrics(gpuLoad, frameTime, perfState, fsrActive, fgActive)
    → writes to Vegas static members (s_lastGpuLoad, s_ftHistory, etc.)

dxvk_swapchain_blitter.cpp:present
  → m_vegasHud->render(ctx, dstView)
    → HudRenderer::beginFrame()
    → draws 4 text lines using Vega statics
    → HudRenderer::flushDraws()
    → HudRenderer::endFrame()
```

**Layout (top-right corner):**
```
VEGAS v1.0              ← cyan header
T3  72%  16.7ms          ← tier / load / frame time (color-coded by perf state)
NORMAL  FSR  FG          ← state + active features
FT: 16.7 15.2 18.1 ...   ← numeric history (last 6 frames)
 #####  @@@ ### @@@      ← compact ASCII bar graph (20 bars × 4 levels)
   ### @@@ ### @@@
   ### @@@ ### @@@
  ################
```

**Config:** `vegas.enableHud = Auto` (enabled on Adreno when StarProfile active)

**Design decisions:**
- Completely independent of `DXVK_HUD` — uses its own `HudRenderer` instance
- Both HUDs can be active simultaneously without conflict
- Renders inside the existing render pass (no extra `cmdBeginRendering`)
- Composite path: baked into the HUD composition image alongside DXVK HUD
- Non-composite path: renders directly onto the swapchain image
- Bar graph uses stacked ASCII characters (`#`, `@`, `!`) to form 20-bar × 4-level columns; bitmap font lacks Unicode block chars so density encoding via character choice is used instead
- Bar heights map to 4 discrete levels (0–12.5ms, 12.5–25ms, 25–37.5ms, 37.5–50ms); characters `#` (normal), `@` (warning), `!` (critical) encode both height and frame-time quality

**File list for the subsystem:**
| File | Lines |
|------|-------|
| `src/dxvk/dxvk_vegas.h` | metrics members + getters (~30 lines) |
| `src/dxvk/dxvk_vegas.cpp` | pushMetrics() + FT history + getters (~65 lines) |
| `src/dxvk/dxvk_vegas_hud.h` | class declaration (67 lines) |
| `src/dxvk/dxvk_vegas_hud.cpp` | implementation (~195 lines) |
| `src/dxvk/dxvk_swapchain_blitter.h` | include + unique_ptr member (+3 lines) |
| `src/dxvk/dxvk_swapchain_blitter.cpp` | construction + render calls (+18 lines) |

## 4. Config Options

| Config Key | Type | Default | Declared In | Read In | Purpose |
|---|---|---|---|---|---|
| `dxvk.enableStarProfile` | Tristate | Auto | `dxvk_options.h:79` | `dxvk_options.cpp:26`, `vegas.cpp:898` | Master switch for ALL Vegas features |
| `vegas.enableHud` | Tristate | Auto | `dxvk_options.h:86` | `dxvk_options.cpp:28`, `vegas_hud.cpp:17` | VegasHud overlay: tier/load/ft, perf state, FT history, ASCII bar graph |
| `vegas.enableUpscaler` | Tristate | Auto | `dxvk_options.h:83` | `dxvk_options.cpp:27`, `dxgi_options.cpp:130` | FSR 1.0 spatial upscaler |
| `vegas.forceTier` | int32_t | 0 | `dxvk_options.h:91` | `dxvk_options.cpp:29`, `vegas.cpp:947-949` | Override GPU tier detection |
| `dxvk.enableAsync` | bool | false | `dxvk_options.h:15` | `dxvk_options.cpp:24`, `context.cpp:5874` | Async pipeline compilation |
| `dxvk.numCompilerThreads` | int32_t | 0 | `dxvk_options.h:22` | `dxvk_options.cpp:8` | Override compiler thread count |

---

## 5. Adding a New Feature

### Step-by-step workflow:

1. **Declare in `dxvk_vegas.h`:**
   - Add new static method to the `Vegas` class
   - Add any new static member variables (baked state)
   - Add any new structs/enums needed

2. **Implement in `dxvk_vegas.cpp`:**
   - Define static variables at the top (lines 31-75 area)
   - Implement the method
   - If it needs device information, integrate with `initializeProfile(DxvkDevice*)`
   - If it's a per-frame decision, expose a `shouldX()` or `xDispatch()` method

3. **Wire into the DXVK pipeline:**
   - Find the right integration point (draw, present, submit, create, blitter)
   - Add the Vegas call behind a guard:
     ```cpp
     if (unlikely(Vegas::shouldX(...))) { ... }
     ```
   - For HUD overlays, wire into `DxvkSwapchainBlitter::present()` and
     `renderHudImage()` in `dxvk_swapchain_blitter.cpp`
   - Use `unlikely()` macro for branches that are infrequently taken

4. **Add config option if needed:**
   - Declare in `dxvk_options.h`
   - Read in `dxvk_options.cpp`
   - Use `Tristate` for three-state options (Auto/True/False)

5. **Add logging:**
   - Use `Logger::debug()` for diagnostic messages
   - Wrap in `#ifndef NDEBUG` if the log would be chatty
   - One-time startup logs belong in `initializeProfile()`

6. **Test:**
   - Test on real Adreno hardware (6xx, 7xx if possible)
   - Test with `dxvk.enableStarProfile = False` (feature should be a no-op)
   - Verify no validation errors with Vulkan validation layers

### Pattern: Decision Helper

Most features follow this pattern:
```cpp
// 1. Static decision function (no side effects)
bool Vegas::shouldX(...) {
    return s_enabled && <condition>;
}

// 2. Guard at call site
if (unlikely(Vegas::shouldX(...))) {
    this->doX();
}
```

### Pattern: Static Baked State

Baked state (set once in `initializeProfile()`, never changed):
```cpp
// In dxvk_vegas.h (declaration)
static uint32_t s_xThreshold;

// At top of dxvk_vegas.cpp (definition with default)
uint32_t Vegas::s_xThreshold = 0;

// In initializeProfile() (baking)
s_xThreshold = computeThreshold(s_tier);
```

---

## 6. Testing Methodology

### Hardware Requirements
- Real Adreno 6xx device (preferably 610/619 for Tier 1, 640+ for Tier 2)
- Turnip driver (Mesa 25.x+)
- Star Emulator or Winlator to host the DXVK build

### Test Games
| Game | Engine | What It Tests |
|------|--------|---------------|
| Tomb Raider (2013) | Crystal Dynamics (D3D11) | Swapchain DISCARD mode, governor, draw batching |
| Prey (2017) | Unity (D3D11) | General stability, framepacing |
| (any Unity game) | Unity | FLIP_DISCARD swapchain, descriptor binding |

### What to Monitor
```bash
# Real-time Vegas diagnostics
adb logcat -s "DXVK" | grep -E "Vegas:|GetImage|tuneThreshold|compiler"

# Vulkan validation (if available)
adb logcat -s "DXVK" | grep -E "ERROR|WARN|VUID"
```

### Key Metrics
- **GetImage errors:** Should be 0 (indicates swapchain buffer count bug)
- **tuneThreshold log:** Shows governor adapting threshold dynamically
- **ftRatio:** Should vary between 0.5-2.0 during gameplay
- **VegasHud bar graph:** ASCII bars (`#`/`@`/`!`) should show frame-time distribution; bars shrink under light load, grow under heavy load
- **Compiler threads:** "Using N compiler threads" — should be ≤4 on ARM64

### Regression Checklist
- [ ] Game launches without crash
- [ ] `dxvk.enableStarProfile = False` disables all Vegas features (emergency escape)
- [ ] No new Vulkan validation errors
- [ ] FPS and framepacing are not worse than previous build
- [ ] Config override (`vegas.forceTier`, `dxvk.numCompilerThreads`) works

---

## 7. Common Pitfalls

### "Desktop Vulkan assumptions are invalid on Adreno"
- TBDR architecture hates unbounded draw batching — always cap thresholds
- Storage images on swapchain images will crash Turnip — use intermediate image
- Push constants are preferred over small UBOs
- Device-local memory is limited — watch allocation sizes

### "Static state is thread-unsafe"
- All baked state (`s_*` variables) is written once from `initializeProfile()`
  and read-only afterwards — no synchronization needed
- The one exception: `m_drawsSinceSubmit` (per-context atomic) and
  `m_vegasProfile.lastBoundVkPipeline` (per-context, reset on flush)
- `thread_local` in `tuneThreshold()` and `analyzePerformance()` prevents
  cross-context interference

### "The gated BCn→ASTC transcoder is NOT ready"
- Do NOT enable it without wiring the upload pipeline
- Block-size alignment (4×4 BCn → 5×5/6×6 ASTC) will cause validation errors
- Test each ASTC block size on target hardware before activation

### "Config option namespaces"
- `dxvk.*` — DXVK core options (in `dxvk_options.cpp`)
- `vegas.*` — Vegas-specific options (in `dxvk_options.cpp` and `dxgi_options.cpp`)
- Don't add options under `dxgi.*` unless they're DXGI-specific

### "D3D9 vs D3D11 thresholds"
D3D9 games typically issue more draw calls per frame. The D3D9-aware
`initializeProfile()` override uses higher base thresholds:
`{300, 500, 800}` vs D3D11 `{100, 200, 350}` (both TBDR-tuned).

---

## 8. Credits & Contributors

### Timeline Semaphore (DxvkFence) — leegao

The non-blocking async FSR dispatch (Section 3.9) depends on **leegao's timeline
semaphore wrapper** (`DxvkFence` in `src/dxvk/dxvk_fence.h/.cpp`). This is a
clean-room implementation of `VK_KHR_timeline_semaphore` that provides:

- **`getValue()`** — Non-blocking completion check (used on hot path, avoids
  Turnip-kgsl emulation bug that over-waits on intermediate values)
- **`wait(value)`** — Blocking wait (used only in `fsrDrain()` on resize)
- **`handle()`** — Raw `VkSemaphore` for `VkSubmitInfo` pNext

Without leegao's `DxvkFence`, the async FSR path would block on every frame
with a naive fence wait, destroying the frametime stability that Fix 2 achieves.
The `getValue()`-only hot-path pattern is the key innovation that makes async
FSR viable on Turnip.

**Files:** `src/dxvk/dxvk_fence.h`, `src/dxvk/dxvk_fence.cpp`

### Project Credits

- **Lead Developer:** isygold
- **Base Project:** DXVK v2.7.1+ by doitsujin
- **Timeline Semaphore:** leegao (DxvkFence)
- **License:** zlib/libpng

---

*Last updated: 2026-06-07 | Branch: vegas*
