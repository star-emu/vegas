#include "dxvk_vegas.h"
#include "dxvk_device.h"
#include "dxvk_adapter.h"
#include "../util/config/config.h"

#include "star_fsr_spv.h"
#include "star_fg_spv.h"

// GPU BCn→ASTC transcoder SPIR-V + LUT data
#include "vegas_bcn_decode_spv.h"
#include "vegas_astc_enc_spv.h"
#include "vegas_astc_lut2_packed.h"
#include "vegas_astc_lut_2p_4x4_s2.h"

#ifndef _WIN32
#include <dlfcn.h>
#endif

#include <algorithm>
#include <string>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace dxvk {

  // ============================================================
  // Baked state — all values determined by initializeProfile()
  // ============================================================
  bool     Vegas::s_initialized    = false;
  bool     Vegas::s_enabled        = false;
  bool     Vegas::s_bindSkipEnabled = false;
  uint32_t Vegas::s_tier           = 0;
  uint32_t Vegas::s_drawThreshold  = 150;
  uint32_t Vegas::s_haaeThreshold  = 65;

  // Vulkan state — populated by initializeProfile(DxvkDevice*)
  void*    Vegas::s_device           = nullptr;
  uint64_t Vegas::s_physicalDevice   = 0;
  uint64_t Vegas::s_vkQueue         = 0;
  uint32_t Vegas::s_queueFamily     = 0;
  // FSR pipeline cache
  uint64_t Vegas::s_fsrPipeline       = 0;
  uint64_t Vegas::s_fsrPipelineLayout = 0;
  uint64_t Vegas::s_fsrDescSetLayout  = 0;
  uint64_t Vegas::s_fsrDescPool       = 0;
  bool     Vegas::s_fsrInitialized    = false;
  // FSR intermediate target
  uint64_t Vegas::s_fsrInterImage    = 0;
  uint64_t Vegas::s_fsrInterMemory   = 0;
  uint32_t Vegas::s_fsrInterW        = 0;
  uint32_t Vegas::s_fsrInterH        = 0;

  // DxvkDevice stored for DxvkFence creation
  DxvkDevice*  Vegas::s_dxvkDevice      = nullptr;
  // Async FSR state
  void*        Vegas::s_fsrFence        = nullptr;
  uint64_t     Vegas::s_fsrNextValue    = 0;
  bool         Vegas::s_fsrInFlight     = false;
  uint32_t     Vegas::s_fsrResultW      = 0;
  uint32_t     Vegas::s_fsrResultH      = 0;
  uint64_t     Vegas::s_fsrLastSrcView  = 0;
  uint64_t     Vegas::s_fsrInterView    = 0;
  uint64_t     Vegas::s_fsrAsyncCmdPool = 0;
  uint64_t     Vegas::s_fsrAsyncCmdBuf  = 0;

  // Framegen resources
  uint64_t Vegas::s_fgPipeline[3]     = {0, 0, 0};
  uint64_t Vegas::s_fgPipelineLayout  = 0;
  uint64_t Vegas::s_fgDescSetLayout   = 0;
  uint64_t Vegas::s_fgDescPool        = 0;
  bool     Vegas::s_fgInitialized     = false;

  // GPU Transcoder resources
  uint64_t Vegas::s_tcDecodePipeline       = 0;
  uint64_t Vegas::s_tcEncodePipeline       = 0;
  uint64_t Vegas::s_tcDecodePipelineLayout = 0;
  uint64_t Vegas::s_tcEncodePipelineLayout = 0;
  uint64_t Vegas::s_tcDecodeDescLayout     = 0;
  uint64_t Vegas::s_tcEncodeDescLayout     = 0;
  uint64_t Vegas::s_tcDescPool             = 0;
  bool     Vegas::s_tcInitialized          = false;
  uint64_t Vegas::s_tcLut2Buffer           = 0;
  uint64_t Vegas::s_tcLut2Memory           = 0;
  uint64_t Vegas::s_tcLutS2Buffer          = 0;
  uint64_t Vegas::s_tcLutS2Memory          = 0;
  uint64_t Vegas::s_tcScratchBuffer        = 0;
  uint64_t Vegas::s_tcScratchMemory        = 0;
  uint32_t Vegas::s_tcScratchW             = 0;
  uint32_t Vegas::s_tcScratchH             = 0;

  // Framegen intermediate images
  bool     Vegas::s_fgPrevValid       = false;
  uint64_t Vegas::s_fgPrevImage       = 0;
  uint64_t Vegas::s_fgPrevMemory      = 0;
  uint32_t Vegas::s_fgPrevW           = 0;
  uint32_t Vegas::s_fgPrevH           = 0;
  uint64_t Vegas::s_fgMotionImage     = 0;
  uint64_t Vegas::s_fgMotionMemory    = 0;
  uint64_t Vegas::s_fgMotionFiltered  = 0;
  uint64_t Vegas::s_fgMotionFMemory   = 0;
  uint64_t Vegas::s_fgOutputImage     = 0;
  uint64_t Vegas::s_fgOutputMemory    = 0;
  uint32_t Vegas::s_fgMotionW         = 0;
  uint32_t Vegas::s_fgMotionH         = 0;

  // VegasHud metrics
  float    Vegas::s_ftHistory[FT_HISTORY_SIZE] = {};
  uint32_t Vegas::s_ftHead             = 0;
  float    Vegas::s_lastGpuLoad        = 0.0f;
  VegasPerformanceState Vegas::s_lastPerfState = VegasPerformanceState::Normal;
  float    Vegas::s_lastFrameTime      = 0.0f;
  bool     Vegas::s_fsrActive          = false;
  bool     Vegas::s_fgActive           = false;



  // ============================================================
  // Adreno GPU tier classifier — replaces opaque upstream
  // getStarEnginePersona(). Parses device name to extract the
  // model number, then maps it to a tier based on actual GPU
  // performance characteristics on TBDR architectures.
  //
  // Tier 1 (entry):  Adreno 5xx, 6xx < 620  (drawThr=600)
  // Tier 2 (mid):    Adreno 6xx 620-689, 7xx < 730  (drawThr=1200)
  // Tier 3 (high):   Adreno 690+, 7xx >= 730, 8xx+  (drawThr=2000)
  // ============================================================
  static uint32_t classifyAdrenoTier(const char* deviceName) {
      // Find first digit in device name; model# is always the
      // numeric suffix of the "Adreno NNN" token.
      const char* p = deviceName;
      while (*p != '\0' && !std::isdigit(static_cast<unsigned char>(*p)))
          p++;
      if (*p == '\0') return 2;  // no digits → conservative fallback

      uint32_t model = 0;
      while (*p != '\0' && std::isdigit(static_cast<unsigned char>(*p))) {
          model = model * 10 + static_cast<uint32_t>(*p - '0');
          // Clamp to avoid overflow on very long digit runs
          if (model > 999) break;
          p++;
      }

      uint32_t gen = model / 100;  // e.g. 6 from 610, 7 from 730

      if (gen <= 5) return 1;                          // 5xx → entry

      if (gen == 6) {
          if (model < 620) return 1;                   // 600-619 → entry
          if (model < 690) return 2;                   // 620-689 → mid
          return 3;                                     // 690+    → high
      }

      if (gen == 7) {
          if (model < 730) return 2;                   // 700-729 → mid
          return 3;                                     // 730+    → high
      }

      // 8xx+ → high
      return 3;
  }

  void Vegas::initializeProfile(uint32_t& threshold, bool& enabled, bool& bindSkip, uint32_t& tier, DxvkDevice* device) {
      if (device == nullptr || device->adapter() == nullptr) return;
      auto& props = device->adapter()->deviceProperties().core.properties;

      static constexpr const char* adrenoStr = "adreno";
      static constexpr size_t adrenoLen = 6;
      bool isAdreno = false;

#ifndef _WIN32
      if (device->adapter()->isAdreno()) {
          isAdreno = true;
      } else
#endif
      {
          for (size_t i = 0; i < VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - adrenoLen && props.deviceName[i] != '\0'; ++i) {
              size_t j = 0;
              for (; j < adrenoLen; ++j) {
                  if (std::tolower(static_cast<unsigned char>(props.deviceName[i + j])) != adrenoStr[j]) break;
              }
              if (j == adrenoLen) {
                  isAdreno = true;
                  break;
              }
          }
      }

      if (isAdreno) {
          enabled = true;
          bindSkip = true;
          tier = classifyAdrenoTier(props.deviceName);
          // VEGAS: Set default tier-based threshold (fallback)
          // TBDR-aware: Adreno (tile-based) benefits from EARLIER flushes
          // to avoid tile buffer overflow. Desktop thresholds (600-2000)
          // cause tile thrashing on mobile. Halved for TBDR safety.
          static constexpr uint32_t defaultThresholds[] = {100, 200, 350};
          threshold = (tier >= 1 && tier <= 3) ? defaultThresholds[tier - 1] : 100;
      }
  }

  void Vegas::initializeProfile(uint32_t& threshold, bool& enabled, bool& bindSkip, uint32_t& tier, DxvkDevice* device, bool isD3D9) {
      if (device == nullptr) return;
      initializeProfile(threshold, enabled, bindSkip, tier, device);

      if (isD3D9 && enabled) {
          // D3D9 on TBDR: D3D9 draw calls are typically larger (more vertices
          // per call) so thresholds are higher than D3D11, but still TBDR-aware.
          static constexpr uint32_t d3d9ThresholdTable[] = {300, 500, 800};
          if (tier >= 1 && tier <= 3) {
              threshold = d3d9ThresholdTable[tier - 1];
          } else {
              threshold = 300;
          }
      }
  }

  // VEGAS: Governor-style tiered threshold (AdrenoGovernor logic)
  //
  // Bleeding-edge tier-aware design:
  //   - Tier 1 (entry):  conservative 1.5× cap — TBDR tile overflow protection
  //   - Tier 2 (mid):    balanced 2.5× cap
  //   - Tier 3 (high):   aggressive 3.0× cap — high-end can batch deeper
  //   - Resets to base on moderate load to prevent sticky high thresholds
  //   - Low-load path requires sustained frame time > 8 ms (avoids transient spikes)
  void Vegas::tuneThreshold(uint32_t& threshold, float load, float frameTime, uint32_t tier) {
      // TBDR-aware base thresholds — Adreno tile-based renderers need
      // frequent flushes to avoid tile buffer overflow. Halved from
      // desktop values.
      static constexpr uint32_t baseThresholds[] = { 100, 200, 350 };
      uint32_t base = (tier >= 1 && tier <= 3) ? baseThresholds[tier - 1] : 100;

      // Tier-based cap multiplier (TBDR: conservative caps to prevent
      // tile buffer thrashing at high batch counts)
      static constexpr float capMultipliers[] = { 2.0f, 2.0f, 1.7f };
      float multiplier = (tier >= 1 && tier <= 3) ? capMultipliers[tier - 1] : 2.0f;
      uint32_t cap = static_cast<uint32_t>(base * multiplier);

      // TBDR-aware governor logic:
      //
      // High sustained GPU load (load>0.90, ft>25ms):
      //   → GPU-bound, batch more to amortize submission overhead
      //
      // Low GPU load with high frame time (load<0.40, ft>12ms):
      //   → CPU-bound! TBDR driver overhead from over-batching is
      //     starving the GPU. REDUCE threshold aggressively to let
      //     GPU start tiling earlier.
      //
      // Everything else: reset to base to prevent sticky thresholds.
      if (load > 0.90f && frameTime > 25.0f) {
          // GPU-bound — batch more draws
          threshold = cap;
      } else if (load < 0.40f && frameTime > 12.0f) {
          // CPU-bound — flush more frequently for TBDR pacing
          threshold = std::max(50u, base / 2);
      } else {
          // Balanced — reset to base
          threshold = base;
      }
  }

  // Self-contained overload — delegates to the 4-arg form with internal state.
  // Applies EMA smoothing and cooldown to prevent oscillation.
  void Vegas::tuneThreshold(float load, float frameTime) {
      // 1. EMA smoothing — dampen frame-time jitter
      thread_local float s_smoothFt = 16.6f;
      s_smoothFt = s_smoothFt * 0.9f + frameTime * 0.1f;

      // 2. Frame-count cooldown — re-evaluate at most once every 15 calls
      //    (~250 ms at 60 fps, ~500 ms at 30 fps).
      //    Reduced from 30 → 15 for faster governor response.
      thread_local uint32_t s_framesSinceAdj = 0;
      s_framesSinceAdj++;
      if (s_framesSinceAdj < 15)
          return;
      s_framesSinceAdj = 0;

      // 3. Apply and log if threshold actually changed
      uint32_t oldThresh = s_drawThreshold;
      tuneThreshold(s_drawThreshold, load, s_smoothFt, s_tier);
      if (s_drawThreshold != oldThresh) {
          Logger::debug(str::format(
              "Vegas: tuneThreshold ", oldThresh, " -> ", s_drawThreshold,
              " load=", load, " smoothFt=", s_smoothFt, "ms tier=", s_tier));
      }
  }

  // VEGAS: Tier-aware zero-init for shader workgroup memory.
  // Tier 1/2 (entry/mid) retain zero-init for Turnip stability;
  // low-end Adreno GPUs are more susceptible to hangs from
  // uninitialized workgroup memory.
  // Tier 3 (high-end) skips zero-init for ~1-2% shader perf gain.
  bool Vegas::shouldZeroInit(uint32_t tier) {
      return tier < 3;
  }


  void Vegas::calculateAspectRatio(uint32_t w, uint32_t h, float& outX, float& outY) {
    if (w == 0 || h == 0) { outX = 1.0f; outY = 1.0f; return; }
    constexpr float targetRatio = 16.0f / 9.0f;
    float currentRatio = static_cast<float>(w) / static_cast<float>(h);

    if (currentRatio > targetRatio) {
        outX = targetRatio / currentRatio;
        outY = 1.0f;
    } else if (currentRatio < targetRatio) {
        outX = 1.0f;
        outY = currentRatio / targetRatio;
    } else {
        outX = 1.0f;
        outY = 1.0f;
    }
  }

  uint64_t Vegas::getSystemRamMB() {
#ifdef _WIN32
      MEMORYSTATUSEX statex;
      statex.dwLength = sizeof(statex);
      GlobalMemoryStatusEx(&statex);
      return statex.ullTotalPhys / (1024 * 1024);
#else
      static const long pageSize = sysconf(_SC_PAGE_SIZE);
      long pages = sysconf(_SC_PHYS_PAGES);
      return static_cast<uint64_t>(pages) * static_cast<uint64_t>(pageSize) / (1024ULL * 1024ULL);
#endif
  }

  // ============================================================
  // Config-load-time overloads (self-aware, no Vk device needed)
  // ============================================================

  void Vegas::applyVramSwap(Config& config) {
    uint64_t totalRamMB = getSystemRamMB();
    uint32_t vramReport = static_cast<uint32_t>(totalRamMB * 0.40);

    // Safety bounds: 1 GB min, 4 GB max
    if (vramReport < 1024)  vramReport = 1024;
    if (vramReport > 4096)  vramReport = 4096;

    config.setOption("dxgi.maxDeviceMemory", std::to_string(vramReport));
    config.setOption("dxgi.maxSharedMemory",  std::to_string(vramReport / 2));
  }


  void Vegas::applyGpuMask(Config& config) {
    // Self-aware: detect GPU tier without Vk device.
    // Tries Android sysfs; falls back to safe default.
    uint32_t tier = 2; // mid-range default (GTX 1070)

    FILE* f = std::fopen("/sys/class/kgsl/kgsl-3d0/gpu_model", "r");
    if (!f) f = std::fopen("/sys/class/kgsl/kgsl-3d0/devfreq/device/gpu_model", "r");

    if (f) {
      char buf[64] = {0};
      if (std::fgets(buf, int(sizeof(buf)), f)) {
        // Find and parse the Adreno model number
        const char* p = buf;
        while (*p && !std::isdigit(static_cast<unsigned char>(*p))) ++p;
        if (*p) {
          unsigned long model = std::strtoul(p, nullptr, 10);
          if (model >= 700)       tier = 3; // Adreno 7xx/8xx -> RTX 3060
          else if (model >= 640)  tier = 2; // Adreno 640-660 -> GTX 1070
          else                    tier = 1; // Adreno 610/619 -> GTX 1050 Ti
        }
      }
      std::fclose(f);
    }

    // Apply persona based on detected tier
    static constexpr struct { const char* vid; const char* did; } personaTable[4] = {
      {},                                          // [0] unused
      {"10de", "1c82"}, // [1] GTX 1050 Ti
      {"10de", "1b81"}, // [2] GTX 1070
      {"10de", "2503"}, // [3] RTX 3060
    };

    if (tier >= 1 && tier <= 3) {
      config.setOption("dxgi.customVendorId", personaTable[tier].vid);
      config.setOption("dxgi.customDeviceId", personaTable[tier].did);
      config.setOption("dxgi.customDeviceDesc",
        std::string("NVIDIA GeForce (Vegas - Tier ") + std::to_string(tier) + ")");
    }
  }


  // VEGAS: Framegen should activate when there IS headroom (low frame times),
  // not when the GPU is saturated. Inverted from the original logic.
  // Tier 1 (Adreno 610) excluded — compute budget insufficient for 3-pass.
  bool Vegas::needsFrameGen(float frameTime, uint32_t tier) {
      if (tier == 1) return false;
      if (tier == 2) return frameTime <= 29.0f;  // ≥34 FPS headroom
      return frameTime <= 33.0f;                   // ≥30 FPS headroom
  }

  void Vegas::calculateFsrConstants(VegasFsrConstants& c, VkExtent3D src, VkExtent3D dst) {
      c.info[0] = static_cast<float>(src.width)  / static_cast<float>(dst.width);
      c.info[1] = static_cast<float>(src.height) / static_cast<float>(dst.height);
      c.info[2] = 0.5f * c.info[0] - 0.5f;
      c.info[3] = 0.5f * c.info[1] - 0.5f;
  }


  VegasPerformanceState Vegas::analyzePerformance(float load, float frameTime, float targetFrameTime) {
      thread_local float s_prevFrameTime = 16.6f;
      float delta = std::abs(frameTime - s_prevFrameTime);
      s_prevFrameTime = frameTime;

      // Adaptive thresholds relative to target frame time
      // targetFrameTime = 1000.0 / fpsLimit; default 16.667ms = 60 FPS
      float laggingThreshold   = targetFrameTime * 1.5f;
      float overheatThreshold  = targetFrameTime * 3.0f;

      if (load >= 0.95f && frameTime >= overheatThreshold) {
          return VegasPerformanceState::Overheating;
      }
      if (delta > targetFrameTime * 1.25f) {
          return VegasPerformanceState::Stuttering;
      }
      if (frameTime >= laggingThreshold) {
          return VegasPerformanceState::Lagging;
      }
      return VegasPerformanceState::Normal;
  }

  uint32_t Vegas::getGraphColor(VegasPerformanceState state) {
      static constexpr uint32_t colorTable[] = {
          0x00FF00,
          0xFFFF00,
          0xFF8800,
          0xFF0000
      };
      return colorTable[static_cast<int>(state)];
  }


  const char* Vegas::getStatusString(VegasPerformanceState state) {
      static constexpr const char* statusTable[] = {
          "NORMAL",
          "LAGGING",
          "STUTTERING",
          "OVERHEATING"
      };
      return statusTable[static_cast<int>(state)];
  }

  // VEGAS: ASTC helpers — used by shouldTranscodeFormat and gpuTranscodeImageData
  bool Vegas::formatIsBcn(VkFormat format) {
    switch (format) {
      case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
      case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
      case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
      case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
      case VK_FORMAT_BC2_UNORM_BLOCK:
      case VK_FORMAT_BC2_SRGB_BLOCK:
      case VK_FORMAT_BC3_UNORM_BLOCK:
      case VK_FORMAT_BC3_SRGB_BLOCK:
      case VK_FORMAT_BC4_UNORM_BLOCK:
      case VK_FORMAT_BC4_SNORM_BLOCK:
      case VK_FORMAT_BC5_UNORM_BLOCK:
      case VK_FORMAT_BC5_SNORM_BLOCK:
      case VK_FORMAT_BC6H_UFLOAT_BLOCK:
      case VK_FORMAT_BC6H_SFLOAT_BLOCK:
      case VK_FORMAT_BC7_UNORM_BLOCK:
      case VK_FORMAT_BC7_SRGB_BLOCK:
        return true;
      default:
        return false;
    }
  }

  VkFormat Vegas::getAstcFormat(VkFormat bcnFormat) {
    // NOTE: Only ASTC 4×4 block size is supported. The GPU compute encoder
    // (leegao's astc_enc_leegao.comp) is hardcoded for 4×4 blocks with a
    // local_size of (8,8,1). Using 5×5 or 6×6 would require a different
    // shader and would break the block-size alignment guarantee that allows
    // in-place staging buffer transcoding for same-size BCn formats.
    switch (bcnFormat) {
      case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
      case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
      case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
      case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;

      case VK_FORMAT_BC2_UNORM_BLOCK:
        return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
      case VK_FORMAT_BC2_SRGB_BLOCK:
        return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;

      case VK_FORMAT_BC3_UNORM_BLOCK:
        return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
      case VK_FORMAT_BC3_SRGB_BLOCK:
        return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;

      case VK_FORMAT_BC4_UNORM_BLOCK:
      case VK_FORMAT_BC4_SNORM_BLOCK:
        return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;

      case VK_FORMAT_BC5_UNORM_BLOCK:
      case VK_FORMAT_BC5_SNORM_BLOCK:
        return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;

      case VK_FORMAT_BC6H_UFLOAT_BLOCK:
      case VK_FORMAT_BC6H_SFLOAT_BLOCK:
        return VK_FORMAT_UNDEFINED;

      case VK_FORMAT_BC7_UNORM_BLOCK:
        return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
      case VK_FORMAT_BC7_SRGB_BLOCK:
        return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;

      default:
        return VK_FORMAT_UNDEFINED;
    }
  }

  // GPU transcoder — see gpuTranscodeImageData() for the active implementation.
  // Modifies the image format when shouldTranscodeFormat() returns ASTC;
  // Batch 3 wires this in DxvkDevice::createImage().
  VkFormat Vegas::shouldTranscodeFormat(
      VkFormat              originalFormat,
      VkImageUsageFlags     usage,
      VkExtent3D            extent,
      const Rc<DxvkAdapter>& adapter) {
    if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_STORAGE_BIT))
      return VK_FORMAT_UNDEFINED;

    uint64_t pixelCount = uint64_t(extent.width) * extent.height * std::max(extent.depth, 1u);
    uint64_t totalBytes = pixelCount * 4;
    if (totalBytes < (512u * 1024u))
      return VK_FORMAT_UNDEFINED;

    if (!formatIsBcn(originalFormat))
      return VK_FORMAT_UNDEFINED;

    if (originalFormat == VK_FORMAT_BC6H_UFLOAT_BLOCK ||
        originalFormat == VK_FORMAT_BC6H_SFLOAT_BLOCK)
      return VK_FORMAT_UNDEFINED;

    VkFormat astcFormat = getAstcFormat(originalFormat);
    if (astcFormat == VK_FORMAT_UNDEFINED)
      return VK_FORMAT_UNDEFINED;

    VkFormatFeatureFlags2 features = adapter->getFormatFeatures(astcFormat).optimal;
    if (!(features & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT))
      return VK_FORMAT_UNDEFINED;

    return astcFormat;
  }

  // ============================================================
  // GPU BCn→ASTC transcoder (ACTIVE — see gpuTranscodeImageData)
  // ============================================================
  //
  // Replaced the gated CPU transcoder with a GPU compute pipeline:
  //   Pass 1: BCn → RGBA8  (vegas_bcn_decode.comp, custom shader)
  //   Pass 2: RGBA8 → ASTC (leegao's astc_enc_leegao.comp, 4×4)
  //
  // All ASTC target formats now use 4×4 block size so that BC3/BC5/BC7
  // (16 bytes/block) can be transcoded in-place in the staging buffer
  // without re-allocation. BC1/BC4 (8 bytes/block) are skipped at
  // runtime with a debug log — they need a larger staging buffer.
  //
  // Activation path (implemented across Batches 3–4):
  //   a) DxvkDevice::createImage() — swap format to ASTC 4×4,
  //      stash originalFormat in DxvkImageCreateInfo.
  //   b) D3D11Initializer::InitDeviceLocalTexture() — after packing
  //      BCn data into staging buffer, call gpuTranscodeImageData()
  //      to convert BCn→ASTC in-place before the CS upload lambda.
  //
  // Remaining gap: BC1/4 need a larger staging buffer (ASTC 4×4 is
  // 16 B/block vs 8 B/block). Activate by extending the staging
  // buffer allocation when originalFormat.elementSize < 16.
  //
  // The CPU transcoder below is dead code (zero call sites) — kept as
  // a reference implementation for debugging the GPU pipeline.
  // ============================================================

  namespace {

    void decodeBC1(uint8_t dst[16][4], const uint8_t src[8]) {
      uint16_t c0 = src[0] | (uint16_t(src[1]) << 8);
      uint16_t c1 = src[2] | (uint16_t(src[3]) << 8);

      uint8_t r[4], g[4], b[4];
      r[0] = uint8_t((c0 >> 11) * 255 / 31);
      g[0] = uint8_t(((c0 >> 5) & 0x3F) * 255 / 63);
      b[0] = uint8_t((c0 & 0x1F) * 255 / 31);
      r[1] = uint8_t((c1 >> 11) * 255 / 31);
      g[1] = uint8_t(((c1 >> 5) & 0x3F) * 255 / 63);
      b[1] = uint8_t((c1 & 0x1F) * 255 / 31);

      if (c0 > c1) {
        r[2] = (2 * r[0] + r[1]) / 3; g[2] = (2 * g[0] + g[1]) / 3; b[2] = (2 * b[0] + b[1]) / 3;
        r[3] = (r[0] + 2 * r[1]) / 3; g[3] = (g[0] + 2 * g[1]) / 3; b[3] = (b[0] + 2 * b[1]) / 3;
      } else {
        r[2] = (r[0] + r[1]) / 2; g[2] = (g[0] + g[1]) / 2; b[2] = (b[0] + b[1]) / 2;
        r[3] = 0; g[3] = 0; b[3] = 0;
      }

      uint32_t indices = src[4] | (uint32_t(src[5]) << 8) | (uint32_t(src[6]) << 16) | (uint32_t(src[7]) << 24);
      for (int i = 0; i < 16; i++) {
        int idx = (indices >> (2 * i)) & 3;
        dst[i][0] = r[idx]; dst[i][1] = g[idx]; dst[i][2] = b[idx];
        dst[i][3] = (c0 > c1) ? 255u : ((idx == 3) ? 0u : 255u);
      }
    }

    void decodeBC4Alpha(uint8_t dst[16], const uint8_t src[8]) {
      uint8_t a0 = src[0], a1 = src[1];
      uint64_t indices = uint64_t(src[2]) | (uint64_t(src[3]) << 8)
                       | (uint64_t(src[4]) << 16) | (uint64_t(src[5]) << 24)
                       | (uint64_t(src[6]) << 32) | (uint64_t(src[7]) << 40);
      for (int i = 0; i < 16; i++) {
        int idx = (indices >> (3 * i)) & 7;
        if (a0 > a1) {
          static const uint8_t bc4[8] = {0,1,2,3,4,5,6,7};
          dst[i] = uint8_t((a0 * (7 - bc4[idx]) + a1 * bc4[idx]) / 7);
        } else {
          if (idx == 0) dst[i] = a0;
          else if (idx == 1) dst[i] = a1;
          else if (idx <= 5) dst[i] = uint8_t(((6 - idx) * a0 + (idx - 1) * a1) / 5);
          else dst[i] = 0;
        }
      }
    }

    void decodeBC4(uint8_t dst[16], const uint8_t src[8]) {
      decodeBC4Alpha(dst, src);
    }

    void decodeBC3(uint8_t dst[16][4], const uint8_t src[16]) {
      decodeBC1(dst, src);
      uint8_t alpha[16];
      decodeBC4Alpha(alpha, src + 8);
      for (int i = 0; i < 16; i++)
        dst[i][3] = alpha[i];
    }

    void decodeBC5(uint8_t dst[16][4], const uint8_t src[16]) {
      uint8_t r[16], g[16];
      decodeBC4(r, src);
      decodeBC4(g, src + 8);
      for (int i = 0; i < 16; i++) {
        dst[i][0] = r[i]; dst[i][1] = g[i];
        dst[i][2] = 0;    dst[i][3] = 255;
      }
    }

    void decodeBC7(uint8_t dst[16][4], const uint8_t src[16]) {
      uint8_t mode = src[0];
      for (int i = 0; i < 16; i++) {
        dst[i][0] = 128; dst[i][1] = 128;
        dst[i][2] = 128; dst[i][3] = 255;
      }
      if ((mode & 0x80) == 0) return;

      uint16_t r0 = ((src[1] >> 1) & 0x7F) << 1 | (src[2] >> 7);
      uint16_t r1 = (src[2] & 0x7F) << 1 | (src[3] >> 7);
      uint16_t g0 = ((src[3] >> 1) & 0x7F) << 1 | (src[4] >> 7);
      uint16_t g1 = (src[4] & 0x7F) << 1 | (src[5] >> 7);
      uint16_t b0 = ((src[5] >> 1) & 0x7F) << 1 | (src[6] >> 7);
      uint16_t b1 = (src[6] & 0x7F) << 1 | (src[7] >> 7);
      uint8_t a0 = src[8], a1 = src[9];

      uint64_t indices = 0;
      for (int j = 0; j < 6; j++)
        indices |= uint64_t(src[10 + j]) << (8 * j);

      for (int i = 0; i < 16; i++) {
        int idx = (indices >> (4 * i)) & 0xF;
        dst[i][0] = uint8_t(((r0 * (64 - idx) + r1 * idx) * 255) / (63 * 64));
        dst[i][1] = uint8_t(((g0 * (64 - idx) + g1 * idx) * 255) / (63 * 64));
        dst[i][2] = uint8_t(((b0 * (64 - idx) + b1 * idx) * 255) / (63 * 64));
        dst[i][3] = uint8_t(((a0 * (64 - idx) + a1 * idx) * 255) / (63 * 64));
      }
    }

    // --- ASTC block encoder (simplified, 1 partition, LDR) ---

    void astcSetBits(uint8_t* block, uint32_t& bitPos, uint32_t count, uint32_t value) {
      for (uint32_t i = 0; i < count; i++) {
        uint32_t byteIdx = bitPos >> 3;
        uint32_t bitIdx = bitPos & 7;
        if (value & (1u << i))
          block[byteIdx] |= (1u << bitIdx);
        else
          block[byteIdx] &= ~(1u << bitIdx);
        bitPos++;
      }
    }

    uint32_t astcBlockMode(uint32_t w, uint32_t h, uint32_t bits) {
      if (w <= 11 && h <= 5 && bits >= 2 && bits <= 5) {
        uint32_t b = w - 4;
        uint32_t c = h - 2;
        uint32_t d = bits - 2;
        return (0 << 0) | (0 << 1) | ((b & 7) << 2) | ((c & 3) << 5) | ((d & 3) << 7);
      }
      if (w == h) {
        uint32_t D = w - 2;
        uint32_t wb = bits - 1;
        return (0 << 0) | (1 << 1) | (3 << 2) | (3 << 4) | ((D & 7) << 6) | ((wb & 3) << 9);
      }
      return (0 << 0) | (1 << 1) | (3 << 2) | (3 << 4) | (4 << 6) | (1 << 9);
    }

    bool astcBlockDims(VkFormat fmt, int& bw, int& bh) {
      switch (fmt) {
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:   bw = 4;  bh = 4;  return true;
        case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:   bw = 5;  bh = 4;  return true;
        case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:   bw = 5;  bh = 5;  return true;
        case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:   bw = 6;  bh = 5;  return true;
        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:   bw = 6;  bh = 6;  return true;
        case VK_FORMAT_ASTC_8x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x5_SRGB_BLOCK:   bw = 8;  bh = 5;  return true;
        case VK_FORMAT_ASTC_8x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x6_SRGB_BLOCK:   bw = 8;  bh = 6;  return true;
        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:   bw = 8;  bh = 8;  return true;
        case VK_FORMAT_ASTC_10x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x5_SRGB_BLOCK:  bw = 10; bh = 5;  return true;
        case VK_FORMAT_ASTC_10x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x6_SRGB_BLOCK:  bw = 10; bh = 6;  return true;
        case VK_FORMAT_ASTC_10x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x8_SRGB_BLOCK:  bw = 10; bh = 8;  return true;
        case VK_FORMAT_ASTC_10x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x10_SRGB_BLOCK: bw = 10; bh = 10; return true;
        case VK_FORMAT_ASTC_12x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x10_SRGB_BLOCK: bw = 12; bh = 10; return true;
        case VK_FORMAT_ASTC_12x12_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x12_SRGB_BLOCK: bw = 12; bh = 12; return true;
        default: bw = 0; bh = 0; return false;
      }
    }

    void encodeAstcBlock(uint8_t* dst, const uint8_t pixels[], int bw, int bh, bool hasAlpha) {
      memset(dst, 0, 16);

      int weightBits = (bw <= 4 && bh <= 4) ? 4 : (bw <= 5 && bh <= 5) ? 3 : 2;

      uint32_t bp = 0;

      uint32_t bm = astcBlockMode(bw, bh, weightBits);
      astcSetBits(dst, bp, 13, bm);

      astcSetBits(dst, bp, 2, 0);

      uint32_t cem = hasAlpha ? 8 : 6;
      astcSetBits(dst, bp, 4, cem);

      int pixelCount = bw * bh;
      uint8_t minR = 255, minG = 255, minB = 255, maxR = 0, maxG = 0, maxB = 0;
      uint8_t minA = 255, maxA = 0;

      for (int i = 0; i < pixelCount; i++) {
        uint8_t r = pixels[4*i+0], g = pixels[4*i+1], b = pixels[4*i+2], a = pixels[4*i+3];
        if (r < minR) minR = r; if (r > maxR) maxR = r;
        if (g < minG) minG = g; if (g > maxG) maxG = g;
        if (b < minB) minB = b; if (b > maxB) maxB = b;
        if (a < minA) minA = a; if (a > maxA) maxA = a;
      }

      int epBits = 5;
      if (!hasAlpha) {
        if (bw <= 4 && bh <= 4)       epBits = 6;
        else if (bw <= 5 && bh <= 5)  epBits = 5;
        else                           epBits = 4;
      } else {
        if (bw <= 4 && bh <= 4)       epBits = 5;
        else if (bw <= 5 && bh <= 5)  epBits = 4;
        else                           epBits = 4;
      }

      int epShift = 8 - epBits;
      uint32_t epR0 = minR >> epShift, epG0 = minG >> epShift, epB0 = minB >> epShift;
      uint32_t epR1 = maxR >> epShift, epG1 = maxG >> epShift, epB1 = maxB >> epShift;

      if (hasAlpha) {
        uint32_t epA0 = minA >> epShift, epA1 = maxA >> epShift;
        astcSetBits(dst, bp, epBits, epR0);
        astcSetBits(dst, bp, epBits, epG0);
        astcSetBits(dst, bp, epBits, epB0);
        astcSetBits(dst, bp, epBits, epR1);
        astcSetBits(dst, bp, epBits, epG1);
        astcSetBits(dst, bp, epBits, epB1);
        astcSetBits(dst, bp, epBits, epA0);
        astcSetBits(dst, bp, epBits, epA1);
      } else {
        astcSetBits(dst, bp, epBits, epR0);
        astcSetBits(dst, bp, epBits, epG0);
        astcSetBits(dst, bp, epBits, epB0);
        astcSetBits(dst, bp, epBits, epR1);
        astcSetBits(dst, bp, epBits, epG1);
        astcSetBits(dst, bp, epBits, epB1);
      }

      for (int ty = 0; ty < bh; ty++) {
        for (int tx = 0; tx < bw; tx++) {
          int morton = 0;
          for (int b = 0; b < 4; b++) {
            morton |= ((tx >> b) & 1) << (2 * b);
            morton |= ((ty >> b) & 1) << (2 * b + 1);
          }
          int i = morton;

          uint8_t r = pixels[4*i+0], g = pixels[4*i+1], b = pixels[4*i+2];
          uint8_t a = pixels[4*i+3];

          float w = 0.0f;
          if (maxR > minR) w = std::max(w, float(r - minR) / float(maxR - minR));
          if (maxG > minG) w = std::max(w, float(g - minG) / float(maxG - minG));
          if (maxB > minB) w = std::max(w, float(b - minB) / float(maxB - minB));

          if (hasAlpha && maxA > minA)
            w = std::max(w, float(a - minA) / float(maxA - minA));

          int weight = int(w * ((1 << weightBits) - 1) + 0.5f);
          if (weight < 0) weight = 0;
          if (weight >= (1 << weightBits)) weight = (1 << weightBits) - 1;

          astcSetBits(dst, bp, weightBits, weight);
        }
      }

      while (bp < 128)
        astcSetBits(dst, bp, 1, 0);
    }

  } // anonymous namespace

  void Vegas::transcodeImageData(
      void*                 dstData,
      const void*           srcData,
      VkFormat              srcFormat,
      VkFormat              dstFormat,
      uint32_t              width,
      uint32_t              height) {
    int srcBw = 4, srcBh = 4;
    int dstBw, dstBh;
    if (!astcBlockDims(dstFormat, dstBw, dstBh))
      return;

    int srcBlocksX = (int(width) + srcBw - 1) / srcBw;
    int srcBlocksY = (int(height) + srcBh - 1) / srcBh;
    int dstBlocksX = (int(width) + dstBw - 1) / dstBw;
    int dstBlocksY = (int(height) + dstBh - 1) / dstBh;

    int srcBlockSize = 16;
    if (srcFormat == VK_FORMAT_BC1_RGB_UNORM_BLOCK || srcFormat == VK_FORMAT_BC1_RGB_SRGB_BLOCK ||
        srcFormat == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || srcFormat == VK_FORMAT_BC1_RGBA_SRGB_BLOCK ||
        srcFormat == VK_FORMAT_BC4_UNORM_BLOCK || srcFormat == VK_FORMAT_BC4_SNORM_BLOCK) {
      srcBlockSize = 8;
    }

    int scratchTexels = srcBlocksX * srcBw * height;
    uint8_t* decodedPixels = new uint8_t[scratchTexels * 4];
    uint8_t blockPixels[16][4];
    bool hasAlpha = false;

    for (int by = 0; by < srcBlocksY; by++) {
      for (int bx = 0; bx < srcBlocksX; bx++) {
        const uint8_t* srcBlock = (const uint8_t*)srcData + (by * srcBlocksX + bx) * srcBlockSize;

        switch (srcFormat) {
          case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
          case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
          case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
          case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
            decodeBC1(blockPixels, srcBlock);
            break;
          case VK_FORMAT_BC3_UNORM_BLOCK:
          case VK_FORMAT_BC3_SRGB_BLOCK:
            decodeBC3(blockPixels, srcBlock);
            hasAlpha = true;
            break;
          case VK_FORMAT_BC4_UNORM_BLOCK:
          case VK_FORMAT_BC4_SNORM_BLOCK: {
            uint8_t* rp = (uint8_t*)blockPixels;
            memset(rp, 0, 16 * 4);
            decodeBC4(rp, srcBlock);
            break;
          }
          case VK_FORMAT_BC5_UNORM_BLOCK:
          case VK_FORMAT_BC5_SNORM_BLOCK:
            decodeBC5(blockPixels, srcBlock);
            break;
          case VK_FORMAT_BC7_UNORM_BLOCK:
          case VK_FORMAT_BC7_SRGB_BLOCK:
            decodeBC7(blockPixels, srcBlock);
            hasAlpha = true;
            break;
          default:
            delete[] decodedPixels;
            return;
        }

        for (uint32_t py = 0; py < srcBw && (by * srcBh + py) < height; py++) {
          for (uint32_t px = 0; px < srcBh && (bx * srcBw + px) < width; px++) {
            int gi = int((by * srcBh + py) * width + (bx * srcBw + px));
            int bi = int(py * srcBw + px);
            decodedPixels[gi*4+0] = blockPixels[bi][0];
            decodedPixels[gi*4+1] = blockPixels[bi][1];
            decodedPixels[gi*4+2] = blockPixels[bi][2];
            decodedPixels[gi*4+3] = blockPixels[bi][3];
          }
        }
      }
    }

    for (int by = 0; by < dstBlocksY; by++) {
      for (int bx = 0; bx < dstBlocksX; bx++) {
        uint8_t astcPixels[12 * 12 * 4];
        int idx = 0;
        for (int py = 0; py < dstBh; py++) {
          int sy = by * dstBh + py;
          if (sy >= int(height)) sy = int(height) - 1;
          for (int px = 0; px < dstBw; px++) {
            int sx = bx * dstBw + px;
            if (sx >= int(width)) sx = int(width) - 1;
            int si = sy * int(width) + sx;
            astcPixels[idx*4+0] = decodedPixels[si*4+0];
            astcPixels[idx*4+1] = decodedPixels[si*4+1];
            astcPixels[idx*4+2] = decodedPixels[si*4+2];
            astcPixels[idx*4+3] = decodedPixels[si*4+3];
            idx++;
          }
        }

        uint8_t astcBlock[16];
        encodeAstcBlock(astcBlock, astcPixels, dstBw, dstBh, hasAlpha);

        int dstIdx = by * dstBlocksX + bx;
        memcpy((uint8_t*)dstData + dstIdx * 16, astcBlock, 16);
      }
    }

    delete[] decodedPixels;
  }


  // ============================================================
  // Self-Aware Profile — auto-detect GPU, bake all thresholds
  // ============================================================

  void Vegas::initializeProfile(DxvkDevice* device) {
    if (s_initialized) return;

    if (device == nullptr || device->adapter() == nullptr) {
      s_initialized = true;
      return;
    }

    s_dxvkDevice = device;

    // Master switch: dxvk.enableStarProfile
    // Auto  → Adreno detection (current behavior)
    // True  → force-enable all Vegas features
    // False → force-disable all Vegas features (emergency escape)
    Tristate master = device->config().enableStarProfile;
    if (master == Tristate::False) {
      s_enabled        = false;
      s_bindSkipEnabled = false;
      s_tier           = 0;
      s_initialized    = true;
      return;
    }

    auto& props = device->adapter()->deviceProperties().core.properties;
#ifndef _WIN32
    bool isAdreno = device->adapter()->isAdreno();
#else
    bool isAdreno = false;
#endif

    // Local tier before config override
    uint32_t detectedTier = 0;

    if (master == Tristate::True) {
      // Force-enable: skip Adreno detection, classify from device name
      s_enabled        = true;
      s_bindSkipEnabled = true;
      detectedTier     = classifyAdrenoTier(props.deviceName);
    } else {
      // Auto: detect Adreno
      if (!isAdreno) {
        // Fallback: check device name
        std::string dname(props.deviceName);
        for (auto& c : dname) c = std::tolower(static_cast<unsigned char>(c));
        isAdreno = (dname.find("adreno") != std::string::npos);
      }

      if (isAdreno) {
        s_enabled        = true;
        s_bindSkipEnabled = true;
        detectedTier     = classifyAdrenoTier(props.deviceName);
      } else {
        s_enabled        = false;
        s_bindSkipEnabled = false;
        s_tier           = 0;
      }
    }

    // Apply detected tier, then allow config override
    if (s_enabled) {
      s_tier = detectedTier;
      if (s_tier < 1 || s_tier > 3) s_tier = 2;  // safety clamp

      int32_t overrideTier = device->config().vegasForceTier;
      if (overrideTier >= 1 && overrideTier <= 3)
        s_tier = static_cast<uint32_t>(overrideTier);
    }

    // Bake draw thresholds based on GPU tier (TBDR-aware, D3D11 base).
    // Adreno/Turnip is tile-based deferred renderer; thresholds must be
    // low enough that the GPU's tile buffer (~256KB-1MB depending on tier)
    // doesn't overflow within a single render pass.
    // Desktop values (600-2000) cause tile thrashing on all mobile GPUs.
    static constexpr uint32_t drawThresholdTable[] = { 100, 200, 350 };
    // HAAE thresholds: Tier 1 (low-end) needs MORE frequent pacing (lower
    // threshold) to prevent tile buffer overflow. Tier 3 (mid-end) can
    // batch slightly more but still TBDR-limited.
    static constexpr uint32_t haaeThresholdTable[] = { 30, 50, 80 };

    uint32_t idx = (s_tier >= 1 && s_tier <= 3) ? s_tier - 1 : 0;
    s_drawThreshold = drawThresholdTable[idx];
    s_haaeThreshold = haaeThresholdTable[idx];

    // Log tier, thresholds, and zero-init decision once (not per-shader)
    if (s_enabled) {
      Logger::debug(str::format(
          "Vegas: Tier ", s_tier,
          " drawThr=", s_drawThreshold,
          " haaeThr=", s_haaeThreshold,
          " zeroInit=", shouldZeroInit(s_tier)));
    }

    // Store Vulkan device/queue handles for FSR dispatch.
    // The VkDevice handle from device->handle() is an opaque pointer
    // valid for the lifetime of DxvkDevice.
    s_vkQueue         = reinterpret_cast<uint64_t>(device->queues().graphics.queueHandle);
    s_queueFamily     = device->queues().graphics.queueFamily;

    s_device          = reinterpret_cast<void*>(device->handle());
    s_physicalDevice  = reinterpret_cast<uint64_t>(device->adapter()->handle());

    // Store tier in shared DxvkDevice metrics for cross-DLL access
    if (device != nullptr) {
      device->m_vegasMetrics.tier        = s_tier;
      device->m_vegasMetrics.initialized = true;
    }

    s_initialized = true;
  }

  bool Vegas::isEnabled()           { return s_enabled; }
  bool Vegas::isBindSkipEnabled()   { return s_bindSkipEnabled; }
  uint32_t Vegas::getDrawThreshold() { return s_drawThreshold; }
  uint32_t Vegas::getHaaeThreshold() { return s_haaeThreshold; }
  uint32_t Vegas::getTier()         {
    // Prefer shared device metrics (cross-DLL safe)
    auto dev = s_dxvkDevice;
    if (dev != nullptr && dev->m_vegasMetrics.initialized
        && dev->m_vegasMetrics.tier != 0)
      return dev->m_vegasMetrics.tier;
    return s_tier;
  }

  // ============================================================
  // Decision Helpers — all feature logic lives here
  // ============================================================

  bool Vegas::shouldFlush(uint32_t drawCount) {
    return s_enabled && drawCount >= s_drawThreshold;
  }

  bool Vegas::shouldSkipBind() {
    return s_enabled && s_bindSkipEnabled;
  }

  bool Vegas::shouldSubmitHaae(uint32_t& counter, uint32_t drawCalls) {
    counter += drawCalls;
    if (counter >= s_haaeThreshold) {
      counter = 0;
      return true;
    }
    return false;
  }

  bool Vegas::shouldUpscale(Tristate upscalerState, VkExtent3D src, VkExtent3D dst) {
    if (upscalerState == Tristate::False)
      return false;
    if (upscalerState == Tristate::True)
      return true;
    // Auto: only upscale when source is smaller than destination
    return src.width < dst.width;
  }


  // ============================================================
  // FSR 1.0 EASU Dispatch — with all 4 safety steps
  // ============================================================

  // ---- Vulkan function pointer cache (loaded once via dlsym) ----

  namespace {

    // Loaded via dlopen+dlsym at first fsrUpscale call
    struct FsrVulkanFuncs {
      PFN_vkGetDeviceProcAddr      vkGetDeviceProcAddr      = nullptr;
      PFN_vkCreateShaderModule     vkCreateShaderModule     = nullptr;
      PFN_vkDestroyShaderModule    vkDestroyShaderModule    = nullptr;
      PFN_vkCreatePipelineLayout   vkCreatePipelineLayout   = nullptr;
      PFN_vkDestroyPipelineLayout  vkDestroyPipelineLayout  = nullptr;
      PFN_vkCreateComputePipelines vkCreateComputePipelines = nullptr;
      PFN_vkDestroyPipeline        vkDestroyPipeline        = nullptr;
      PFN_vkCreateDescriptorSetLayout   vkCreateDescriptorSetLayout   = nullptr;
      PFN_vkDestroyDescriptorSetLayout  vkDestroyDescriptorSetLayout  = nullptr;
      PFN_vkCreateDescriptorPool        vkCreateDescriptorPool        = nullptr;
      PFN_vkDestroyDescriptorPool       vkDestroyDescriptorPool       = nullptr;
      PFN_vkResetDescriptorPool         vkResetDescriptorPool         = nullptr;
      PFN_vkAllocateDescriptorSets      vkAllocateDescriptorSets      = nullptr;
      PFN_vkUpdateDescriptorSets        vkUpdateDescriptorSets        = nullptr;
      PFN_vkCreateImageView        vkCreateImageView        = nullptr;
      PFN_vkDestroyImageView       vkDestroyImageView       = nullptr;
      PFN_vkCreateCommandPool      vkCreateCommandPool      = nullptr;
      PFN_vkDestroyCommandPool     vkDestroyCommandPool     = nullptr;
      PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
      PFN_vkFreeCommandBuffers     vkFreeCommandBuffers     = nullptr;
      PFN_vkBeginCommandBuffer     vkBeginCommandBuffer     = nullptr;
      PFN_vkEndCommandBuffer       vkEndCommandBuffer       = nullptr;
      PFN_vkCmdPipelineBarrier     vkCmdPipelineBarrier     = nullptr;
      PFN_vkCmdBindPipeline        vkCmdBindPipeline        = nullptr;
      PFN_vkCmdBindDescriptorSets  vkCmdBindDescriptorSets  = nullptr;
      PFN_vkCmdPushConstants       vkCmdPushConstants       = nullptr;
      PFN_vkCmdDispatch            vkCmdDispatch            = nullptr;
      PFN_vkCmdCopyImage           vkCmdCopyImage           = nullptr;
      PFN_vkQueueSubmit            vkQueueSubmit            = nullptr;
      PFN_vkQueueWaitIdle          vkQueueWaitIdle          = nullptr;
      PFN_vkCreateFence            vkCreateFence            = nullptr;
      PFN_vkDestroyFence           vkDestroyFence           = nullptr;
      PFN_vkWaitForFences          vkWaitForFences          = nullptr;
      PFN_vkResetFences            vkResetFences            = nullptr;
      // Intermediate target + blit
      PFN_vkCreateImage               vkCreateImage               = nullptr;
      PFN_vkDestroyImage              vkDestroyImage              = nullptr;
      PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
      PFN_vkAllocateMemory            vkAllocateMemory            = nullptr;
      PFN_vkFreeMemory                vkFreeMemory                = nullptr;
      PFN_vkBindImageMemory           vkBindImageMemory           = nullptr;
      PFN_vkCmdBlitImage              vkCmdBlitImage              = nullptr;
      // Physical-device-level (loaded separately)
      PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
      // Buffer functions for LUT SSBOs (transcoder)
      PFN_vkCreateBuffer              vkCreateBuffer              = nullptr;
      PFN_vkDestroyBuffer             vkDestroyBuffer             = nullptr;
      PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
      PFN_vkBindBufferMemory          vkBindBufferMemory          = nullptr;
      PFN_vkMapMemory                 vkMapMemory                 = nullptr;
      PFN_vkUnmapMemory               vkUnmapMemory               = nullptr;
      bool                         loaded                   = false;
    };

    static FsrVulkanFuncs s_vk;

    /** Load all needed Vulkan device functions via dlsym + vkGetDeviceProcAddr. */
    static bool loadVulkanFuncs(VkDevice device) {
#ifndef _WIN32
      if (s_vk.loaded)
        return s_vk.vkCreateShaderModule != nullptr;
      void* lib = dlopen("libvulkan.so", RTLD_NOLOAD | RTLD_LOCAL);
      if (!lib) lib = dlopen("libvulkan.so.1", RTLD_NOLOAD | RTLD_LOCAL);
      // Fall back to RTLD_DEFAULT if libvulkan isn't accessible by path
      s_vk.vkGetDeviceProcAddr =
          lib ? (PFN_vkGetDeviceProcAddr)dlsym(lib, "vkGetDeviceProcAddr")
              : (PFN_vkGetDeviceProcAddr)dlsym(RTLD_DEFAULT, "vkGetDeviceProcAddr");
      if (!s_vk.vkGetDeviceProcAddr) {
        Logger::warn("Vegas FSR: vkGetDeviceProcAddr not found");
        s_vk.loaded = true;
        return false;
      }
      if (!s_vk.vkGetDeviceProcAddr) {
        Logger::warn("Vegas FSR: vkGetDeviceProcAddr not found");
        s_vk.loaded = true;
        return false;
      }
#     define VK_LOAD_DEV_FUNC(name) \
        s_vk.name = (PFN_##name)s_vk.vkGetDeviceProcAddr(device, #name); \
        if (!s_vk.name) { \
          Logger::warn("Vegas FSR: " #name " not found"); \
          s_vk.loaded = true; \
          return false; \
        }
      VK_LOAD_DEV_FUNC(vkCreateShaderModule)
      VK_LOAD_DEV_FUNC(vkDestroyShaderModule)
      VK_LOAD_DEV_FUNC(vkCreatePipelineLayout)
      VK_LOAD_DEV_FUNC(vkDestroyPipelineLayout)
      VK_LOAD_DEV_FUNC(vkCreateComputePipelines)
      VK_LOAD_DEV_FUNC(vkDestroyPipeline)
      VK_LOAD_DEV_FUNC(vkCreateDescriptorSetLayout)
      VK_LOAD_DEV_FUNC(vkDestroyDescriptorSetLayout)
      VK_LOAD_DEV_FUNC(vkCreateDescriptorPool)
      VK_LOAD_DEV_FUNC(vkDestroyDescriptorPool)
      VK_LOAD_DEV_FUNC(vkResetDescriptorPool)
      VK_LOAD_DEV_FUNC(vkAllocateDescriptorSets)
      VK_LOAD_DEV_FUNC(vkUpdateDescriptorSets)
      VK_LOAD_DEV_FUNC(vkCreateImageView)
      VK_LOAD_DEV_FUNC(vkDestroyImageView)
      VK_LOAD_DEV_FUNC(vkCreateCommandPool)
      VK_LOAD_DEV_FUNC(vkDestroyCommandPool)
      VK_LOAD_DEV_FUNC(vkAllocateCommandBuffers)
      VK_LOAD_DEV_FUNC(vkFreeCommandBuffers)
      VK_LOAD_DEV_FUNC(vkBeginCommandBuffer)
      VK_LOAD_DEV_FUNC(vkEndCommandBuffer)
      VK_LOAD_DEV_FUNC(vkCmdPipelineBarrier)
      VK_LOAD_DEV_FUNC(vkCmdBindPipeline)
      VK_LOAD_DEV_FUNC(vkCmdBindDescriptorSets)
      VK_LOAD_DEV_FUNC(vkCmdPushConstants)
      VK_LOAD_DEV_FUNC(vkCmdDispatch)
      VK_LOAD_DEV_FUNC(vkCmdCopyImage)
      VK_LOAD_DEV_FUNC(vkQueueSubmit)
      VK_LOAD_DEV_FUNC(vkQueueWaitIdle)
      VK_LOAD_DEV_FUNC(vkCreateFence)
      VK_LOAD_DEV_FUNC(vkDestroyFence)
      VK_LOAD_DEV_FUNC(vkWaitForFences)
      VK_LOAD_DEV_FUNC(vkResetFences)
      // Intermediate target + blit
      VK_LOAD_DEV_FUNC(vkCreateImage)
      VK_LOAD_DEV_FUNC(vkDestroyImage)
      VK_LOAD_DEV_FUNC(vkGetImageMemoryRequirements)
      VK_LOAD_DEV_FUNC(vkAllocateMemory)
      VK_LOAD_DEV_FUNC(vkFreeMemory)
      VK_LOAD_DEV_FUNC(vkBindImageMemory)
      VK_LOAD_DEV_FUNC(vkCmdBlitImage)
      // Buffer functions for LUT SSBOs (transcoder)
      VK_LOAD_DEV_FUNC(vkCreateBuffer)
      VK_LOAD_DEV_FUNC(vkDestroyBuffer)
      VK_LOAD_DEV_FUNC(vkGetBufferMemoryRequirements)
      VK_LOAD_DEV_FUNC(vkBindBufferMemory)
      VK_LOAD_DEV_FUNC(vkMapMemory)
      VK_LOAD_DEV_FUNC(vkUnmapMemory)
#     undef VK_LOAD_DEV_FUNC

      // Load physical-device-level functions via dlsym (not vkGetDeviceProcAddr)
      if (!s_vk.vkGetPhysicalDeviceMemoryProperties) {
        s_vk.vkGetPhysicalDeviceMemoryProperties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
                dlsym(RTLD_DEFAULT, "vkGetPhysicalDeviceMemoryProperties"));
        if (!s_vk.vkGetPhysicalDeviceMemoryProperties && lib) {
          s_vk.vkGetPhysicalDeviceMemoryProperties =
              reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
                  dlsym(lib, "vkGetPhysicalDeviceMemoryProperties"));
        }
        if (!s_vk.vkGetPhysicalDeviceMemoryProperties) {
          Logger::warn("Vegas FSR: vkGetPhysicalDeviceMemoryProperties not found");
          s_vk.loaded = true;
          return false;
        }
      }

      s_vk.loaded = true;
      return true;
#else
      return false;
#endif
    }

  } // anonymous namespace


  /** Helper: init FSR pipeline & descriptor resources. Returns true on success. */
  static bool initFsrPipeline(VkDevice device) {
    if (Vegas::s_fsrInitialized)
      return reinterpret_cast<VkPipeline>(Vegas::s_fsrPipeline) != VK_NULL_HANDLE;
    VkResult vr;

    // --- Shader module ---
    VkShaderModuleCreateInfo smCI = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smCI.codeSize = sizeof(dxvk_fsr_easu_code);
    smCI.pCode    = dxvk_fsr_easu_code;
    VkShaderModule sm = VK_NULL_HANDLE;
    vr = s_vk.vkCreateShaderModule(device, &smCI, nullptr, &sm);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR: vkCreateShaderModule failed (", vr, ")"));
      Vegas::s_fsrInitialized = true;
      return false;
    }

    // --- Descriptor set layout ---
    // Binding 0: sampled image (uInput)
    // Binding 1: storage image  (uOutput)
    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding            = 0;
    bindings[0].descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount    = 1;
    bindings[0].stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding            = 1;
    bindings[1].descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount    = 1;
    bindings[1].stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dslCI = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dslCI.bindingCount = 2;
    dslCI.pBindings    = bindings;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    vr = s_vk.vkCreateDescriptorSetLayout(device, &dslCI, nullptr, &dsl);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR: vkCreateDescriptorSetLayout failed (", vr, ")"));
      s_vk.vkDestroyShaderModule(device, sm, nullptr);
      Vegas::s_fsrInitialized = true;
      return false;
    }

    // --- Pipeline layout (push constants) ---
    VkPushConstantRange pcRange = {};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(VegasFsrConstants); // 16 bytes (vec4)

    VkPipelineLayoutCreateInfo plCI = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plCI.setLayoutCount         = 1;
    plCI.pSetLayouts            = &dsl;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges    = &pcRange;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    vr = s_vk.vkCreatePipelineLayout(device, &plCI, nullptr, &pl);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR: vkCreatePipelineLayout failed (", vr, ")"));
      s_vk.vkDestroyDescriptorSetLayout(device, dsl, nullptr);
      s_vk.vkDestroyShaderModule(device, sm, nullptr);
      Vegas::s_fsrInitialized = true;
      return false;
    }

    // --- Compute pipeline ---
    VkPipelineShaderStageCreateInfo ssCI = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    ssCI.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    ssCI.module = sm;
    ssCI.pName  = "main";

    VkComputePipelineCreateInfo cpCI = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpCI.stage  = ssCI;
    cpCI.layout = pl;

    VkPipeline pipeline = VK_NULL_HANDLE;
    vr = s_vk.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpCI, nullptr, &pipeline);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR: vkCreateComputePipelines failed (", vr, ")"));
      s_vk.vkDestroyPipelineLayout(device, pl, nullptr);
      s_vk.vkDestroyDescriptorSetLayout(device, dsl, nullptr);
      s_vk.vkDestroyShaderModule(device, sm, nullptr);
      Vegas::s_fsrInitialized = true;
      return false;
    }

    // --- Shader module no longer needed after pipeline creation ---
    s_vk.vkDestroyShaderModule(device, sm, nullptr);

    // --- Descriptor pool (small, reusable) ---
    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    poolSizes[0].descriptorCount = 1;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo dpCI = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpCI.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpCI.maxSets       = 1;
    dpCI.poolSizeCount = 2;
    dpCI.pPoolSizes    = poolSizes;
    VkDescriptorPool dp = VK_NULL_HANDLE;
    vr = s_vk.vkCreateDescriptorPool(device, &dpCI, nullptr, &dp);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR: vkCreateDescriptorPool failed (", vr, ")"));
      s_vk.vkDestroyPipeline(device, pipeline, nullptr);
      s_vk.vkDestroyPipelineLayout(device, pl, nullptr);
      s_vk.vkDestroyDescriptorSetLayout(device, dsl, nullptr);
      Vegas::s_fsrInitialized = true;
      return false;
    }

    // --- Done ---
    Vegas::s_fsrPipeline       = reinterpret_cast<uint64_t>(pipeline);
    Vegas::s_fsrPipelineLayout = reinterpret_cast<uint64_t>(pl);
    Vegas::s_fsrDescSetLayout  = reinterpret_cast<uint64_t>(dsl);
    Vegas::s_fsrDescPool       = reinterpret_cast<uint64_t>(dp);
    Vegas::s_fsrInitialized    = true;

    Logger::debug("Vegas FSR: compute pipeline created successfully");
    return true;
  }


  // ================================================================
  // GPU Transcoder — BCn→ASTC compute pipeline (Approach A)
  // ================================================================
  //
  // Two-pass compute pipeline:
  //   Pass 1: vegas_bcn_decode.comp — BCn compressed → RGBA8 u32 texels
  //   Pass 2: astc_enc_leegao.comp  — RGBA8 → ASTC 4×4 blocks (PCA quality)
  //
  // Both shaders dispatch at ceil(w/4) × ceil(h/4) workgroups, share the
  // same per-pixel buffer format (uint = R|G<<8|B<<16|A<<24), and use
  // 4×4 block size — so ASTC 4×4 maps 1:1 to BCn 4×4 with no alignment
  // issues.
  //
  // Architecture: Independent queue submit (FSR pattern — Approach A).
  // Creates transient command buffer, records both dispatches + barrier,
  // submits, waits. No DXVK internal state touched.
  // ================================================================

  /** Create a host-visible SSBO filled with static data (LUTs).
   *  \returns true on success; buffer and memory handles set to non-zero. */
  static bool createStaticSsbo(
      VkDevice             device,
      const void*          data,
      VkDeviceSize         size,
      VkBuffer&            buffer,
      VkDeviceMemory&      memory) {
    VkBufferCreateInfo bufCI = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufCI.size        = size;
    bufCI.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult vr = s_vk.vkCreateBuffer(device, &bufCI, nullptr, &buffer);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas TC: vkCreateBuffer (", size, " bytes) failed (", vr, ")"));
      buffer = VK_NULL_HANDLE;
      return false;
    }

    VkMemoryRequirements memReqs;
    s_vk.vkGetBufferMemoryRequirements(device, buffer, &memReqs);

    VkPhysicalDevice physDev = reinterpret_cast<VkPhysicalDevice>(Vegas::s_physicalDevice);
    VkPhysicalDeviceMemoryProperties physMemProps;
    s_vk.vkGetPhysicalDeviceMemoryProperties(physDev, &physMemProps);

    uint32_t memTypeIdx = UINT32_MAX;
    for (uint32_t i = 0; i < physMemProps.memoryTypeCount; ++i) {
      if ((memReqs.memoryTypeBits & (1u << i)) &&
          (physMemProps.memoryTypes[i].propertyFlags &
           (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
           == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        memTypeIdx = i;
        break;
      }
    }

    if (memTypeIdx == UINT32_MAX) {
      // Fallback: any compatible type
      for (uint32_t i = 0; i < physMemProps.memoryTypeCount; ++i) {
        if (memReqs.memoryTypeBits & (1u << i)) {
          memTypeIdx = i;
          break;
        }
      }
    }

    if (memTypeIdx == UINT32_MAX) {
      Logger::warn("Vegas TC: no compatible memory type for LUT SSBO");
      s_vk.vkDestroyBuffer(device, buffer, nullptr);
      buffer = VK_NULL_HANDLE;
      return false;
    }

    VkMemoryAllocateInfo allocCI = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocCI.allocationSize  = memReqs.size;
    allocCI.memoryTypeIndex = memTypeIdx;

    vr = s_vk.vkAllocateMemory(device, &allocCI, nullptr, &memory);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas TC: vkAllocateMemory (LUT) failed (", vr, ")"));
      s_vk.vkDestroyBuffer(device, buffer, nullptr);
      buffer = VK_NULL_HANDLE;
      memory = VK_NULL_HANDLE;
      return false;
    }

    vr = s_vk.vkBindBufferMemory(device, buffer, memory, 0);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas TC: vkBindBufferMemory (LUT) failed (", vr, ")"));
      s_vk.vkFreeMemory(device, memory, nullptr);
      s_vk.vkDestroyBuffer(device, buffer, nullptr);
      buffer = VK_NULL_HANDLE;
      memory = VK_NULL_HANDLE;
      return false;
    }

    // Map, copy, unmap
    void* mapped = nullptr;
    vr = s_vk.vkMapMemory(device, memory, 0, size, 0, &mapped);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas TC: vkMapMemory (LUT) failed (", vr, ")"));
      s_vk.vkFreeMemory(device, memory, nullptr);
      s_vk.vkDestroyBuffer(device, buffer, nullptr);
      buffer = VK_NULL_HANDLE;
      memory = VK_NULL_HANDLE;
      return false;
    }
    std::memcpy(mapped, data, static_cast<size_t>(size));
    s_vk.vkUnmapMemory(device, memory);

    Logger::debug(str::format("Vegas TC: LUT SSBO created (", size, " bytes)"));
    return true;
  }


  /** Initialize GPU transcoder pipelines + LUT SSBOs.
   *  Idempotent — safe to call multiple times. */
  static bool initTranscoderPipeline(VkDevice device) {
    if (Vegas::s_tcInitialized)
      return Vegas::s_tcDecodePipeline != 0;

    VkResult vr;

    // ================================================================
    // 1. Create LUT SSBOs (persistent, host-visible, read-only on GPU)
    // ================================================================
    VkBuffer lut2Buffer = VK_NULL_HANDLE;
    VkDeviceMemory lut2Memory = VK_NULL_HANDLE;
    if (!createStaticSsbo(device, vegas_astc_lut2_packed,
            sizeof(vegas_astc_lut2_packed), lut2Buffer, lut2Memory)) {
      Vegas::s_tcInitialized = true;
      return false;
    }

    VkBuffer lutS2Buffer = VK_NULL_HANDLE;
    VkDeviceMemory lutS2Memory = VK_NULL_HANDLE;
    if (!createStaticSsbo(device, vegas_astc_lut_2p_4x4_s2,
            sizeof(vegas_astc_lut_2p_4x4_s2), lutS2Buffer, lutS2Memory)) {
      s_vk.vkDestroyBuffer(device, lut2Buffer, nullptr);
      s_vk.vkFreeMemory(device, lut2Memory, nullptr);
      Vegas::s_tcInitialized = true;
      return false;
    }

    // ================================================================
    // 2. Decode shader module (vegas_bcn_decode.comp)
    // ================================================================
    VkShaderModuleCreateInfo smCI = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smCI.codeSize = sizeof(vegas_bcn_decode_code);
    smCI.pCode    = vegas_bcn_decode_code;

    VkShaderModule decodeSM = VK_NULL_HANDLE;
    vr = s_vk.vkCreateShaderModule(device, &smCI, nullptr, &decodeSM);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas TC: vkCreateShaderModule (decode) failed (", vr, ")"));
      s_vk.vkDestroyBuffer(device, lutS2Buffer, nullptr);
      s_vk.vkFreeMemory(device, lutS2Memory, nullptr);
      s_vk.vkDestroyBuffer(device, lut2Buffer, nullptr);
      s_vk.vkFreeMemory(device, lut2Memory, nullptr);
      Vegas::s_tcInitialized = true;
      return false;
    }

    // ================================================================
    // 3. Decode descriptor set layout
    //    Binding 0: SSBO read  (BCn src data)
    //    Binding 1: SSBO write (RGBA8 pixel output)
    // ================================================================
    VkDescriptorSetLayoutBinding decodeBindings[2] = {};
    decodeBindings[0].binding         = 0;
    decodeBindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    decodeBindings[0].descriptorCount = 1;
    decodeBindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    decodeBindings[1].binding         = 1;
    decodeBindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    decodeBindings[1].descriptorCount = 1;
    decodeBindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dslCI = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dslCI.bindingCount = 2;
    dslCI.pBindings    = decodeBindings;

    VkDescriptorSetLayout decodeDSL = VK_NULL_HANDLE;
    vr = s_vk.vkCreateDescriptorSetLayout(device, &dslCI, nullptr, &decodeDSL);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas TC: vkCreateDescriptorSetLayout (decode) failed (", vr, ")"));
      s_vk.vkDestroyShaderModule(device, decodeSM, nullptr);
      s_vk.vkDestroyBuffer(device, lutS2Buffer, nullptr);
      s_vk.vkFreeMemory(device, lutS2Memory, nullptr);
      s_vk.vkDestroyBuffer(device, lut2Buffer, nullptr);
      s_vk.vkFreeMemory(device, lut2Memory, nullptr);
      Vegas::s_tcInitialized = true;
      return false;
    }

    // ================================================================
    // 4. Decode pipeline layout (push constant: formatID + texSize)
    // ================================================================
    VkPushConstantRange decodePCRange = {};
    decodePCRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    decodePCRange.offset     = 0;
    decodePCRange.size       = 12;  // uint formatID + uint texWidth + uint texHeight

    VkPipelineLayoutCreateInfo plCI = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plCI.setLayoutCount         = 1;
    plCI.pSetLayouts            = &decodeDSL;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges    = &decodePCRange;

    VkPipelineLayout decodePL = VK_NULL_HANDLE;
    vr = s_vk.vkCreatePipelineLayout(device, &plCI, nullptr, &decodePL);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas TC: vkCreatePipelineLayout (decode) failed (", vr, ")"));
      s_vk.vkDestroyDescriptorSetLayout(device, decodeDSL, nullptr);
      s_vk.vkDestroyShaderModule(device, decodeSM, nullptr);
      s_vk.vkDestroyBuffer(device, lutS2Buffer, nullptr);
      s_vk.vkFreeMemory(device, lutS2Memory, nullptr);
      s_vk.vkDestroyBuffer(device, lut2Buffer, nullptr);
      s_vk.vkFreeMemory(device, lut2Memory, nullptr);
      Vegas::s_tcInitialized = true;
      return false;
    }

    // ================================================================
    // 5. Decode compute pipeline
    // ================================================================
    VkPipelineShaderStageCreateInfo ssCI = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    ssCI.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    ssCI.module = decodeSM;
    ssCI.pName  = "main";

    VkComputePipelineCreateInfo cpCI = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpCI.stage  = ssCI;
    cpCI.layout = decodePL;

    VkPipeline decodePipeline = VK_NULL_HANDLE;
    vr = s_vk.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpCI, nullptr, &decodePipeline);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas TC: vkCreateComputePipelines (decode) failed (", vr, ")"));
      s_vk.vkDestroyPipelineLayout(device, decodePL, nullptr);
      s_vk.vkDestroyDescriptorSetLayout(device, decodeDSL, nullptr);
      s_vk.vkDestroyShaderModule(device, decodeSM, nullptr);
      s_vk.vkDestroyBuffer(device, lutS2Buffer, nullptr);
      s_vk.vkFreeMemory(device, lutS2Memory, nullptr);
      s_vk.vkDestroyBuffer(device, lut2Buffer, nullptr);
      s_vk.vkFreeMemory(device, lut2Memory, nullptr);
      Vegas::s_tcInitialized = true;
      return false;
    }

    // Shader module no longer needed after pipeline creation
    s_vk.vkDestroyShaderModule(device, decodeSM, nullptr);
    decodeSM = VK_NULL_HANDLE;

    // ================================================================
    // 6. Encode shader module (astc_enc_leegao.comp)
    // ================================================================
    smCI.codeSize = sizeof(vegas_astc_enc_code);
    smCI.pCode    = vegas_astc_enc_code;

    VkShaderModule encodeSM = VK_NULL_HANDLE;
    vr = s_vk.vkCreateShaderModule(device, &smCI, nullptr, &encodeSM);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas TC: vkCreateShaderModule (encode) failed (", vr, ")"));
      s_vk.vkDestroyPipeline(device, decodePipeline, nullptr);
      s_vk.vkDestroyPipelineLayout(device, decodePL, nullptr);
      s_vk.vkDestroyDescriptorSetLayout(device, decodeDSL, nullptr);
      s_vk.vkDestroyBuffer(device, lutS2Buffer, nullptr);
      s_vk.vkFreeMemory(device, lutS2Memory, nullptr);
      s_vk.vkDestroyBuffer(device, lut2Buffer, nullptr);
      s_vk.vkFreeMemory(device, lut2Memory, nullptr);
      Vegas::s_tcInitialized = true;
      return false;
    }

    // ================================================================
    // 7. Encode descriptor set layout
    //    Binding 0: SSBO read  (RGBA8 pixel input)
    //    Binding 1: SSBO write (ASTC block output)
    //    Binding 2: SSBO read  (lut2_packed — partition pattern LUT)
    //    Binding 3: SSBO read  (astc_2p_4x4_lut_s2 — 2-plane seed LUT)
    // ================================================================
    VkDescriptorSetLayoutBinding encodeBindings[4] = {};
    for (int i = 0; i < 4; i++) {
      encodeBindings[i].binding         = static_cast<uint32_t>(i);
      encodeBindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      encodeBindings[i].descriptorCount = 1;
      encodeBindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    dslCI.bindingCount = 4;
    dslCI.pBindings    = encodeBindings;

    VkDescriptorSetLayout encodeDSL = VK_NULL_HANDLE;
    vr = s_vk.vkCreateDescriptorSetLayout(device, &dslCI, nullptr, &encodeDSL);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas TC: vkCreateDescriptorSetLayout (encode) failed (", vr, ")"));
      s_vk.vkDestroyShaderModule(device, encodeSM, nullptr);
      s_vk.vkDestroyPipeline(device, decodePipeline, nullptr);
      s_vk.vkDestroyPipelineLayout(device, decodePL, nullptr);
      s_vk.vkDestroyDescriptorSetLayout(device, decodeDSL, nullptr);
      s_vk.vkDestroyBuffer(device, lutS2Buffer, nullptr);
      s_vk.vkFreeMemory(device, lutS2Memory, nullptr);
      s_vk.vkDestroyBuffer(device, lut2Buffer, nullptr);
      s_vk.vkFreeMemory(device, lut2Memory, nullptr);
      Vegas::s_tcInitialized = true;
      return false;
    }

    // ================================================================
    // 8. Encode pipeline layout (push constant: texDim + flags)
    // ================================================================
    VkPushConstantRange encodePCRange = {};
    encodePCRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    encodePCRange.offset     = 0;
    encodePCRange.size       = 12;  // ivec2 texDim + uint flags

    plCI.setLayoutCount         = 1;
    plCI.pSetLayouts            = &encodeDSL;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges    = &encodePCRange;

    VkPipelineLayout encodePL = VK_NULL_HANDLE;
    vr = s_vk.vkCreatePipelineLayout(device, &plCI, nullptr, &encodePL);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas TC: vkCreatePipelineLayout (encode) failed (", vr, ")"));
      s_vk.vkDestroyDescriptorSetLayout(device, encodeDSL, nullptr);
      s_vk.vkDestroyShaderModule(device, encodeSM, nullptr);
      s_vk.vkDestroyPipeline(device, decodePipeline, nullptr);
      s_vk.vkDestroyPipelineLayout(device, decodePL, nullptr);
      s_vk.vkDestroyDescriptorSetLayout(device, decodeDSL, nullptr);
      s_vk.vkDestroyBuffer(device, lutS2Buffer, nullptr);
      s_vk.vkFreeMemory(device, lutS2Memory, nullptr);
      s_vk.vkDestroyBuffer(device, lut2Buffer, nullptr);
      s_vk.vkFreeMemory(device, lut2Memory, nullptr);
      Vegas::s_tcInitialized = true;
      return false;
    }

    // ================================================================
    // 9. Encode compute pipeline
    // ================================================================
    ssCI.module = encodeSM;
    ssCI.pName  = "main";
    cpCI.layout = encodePL;

    VkPipeline encodePipeline = VK_NULL_HANDLE;
    vr = s_vk.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpCI, nullptr, &encodePipeline);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas TC: vkCreateComputePipelines (encode) failed (", vr, ")"));
      s_vk.vkDestroyShaderModule(device, encodeSM, nullptr);
      s_vk.vkDestroyPipelineLayout(device, encodePL, nullptr);
      s_vk.vkDestroyDescriptorSetLayout(device, encodeDSL, nullptr);
      s_vk.vkDestroyPipeline(device, decodePipeline, nullptr);
      s_vk.vkDestroyPipelineLayout(device, decodePL, nullptr);
      s_vk.vkDestroyDescriptorSetLayout(device, decodeDSL, nullptr);
      s_vk.vkDestroyBuffer(device, lutS2Buffer, nullptr);
      s_vk.vkFreeMemory(device, lutS2Memory, nullptr);
      s_vk.vkDestroyBuffer(device, lut2Buffer, nullptr);
      s_vk.vkFreeMemory(device, lut2Memory, nullptr);
      Vegas::s_tcInitialized = true;
      return false;
    }

    // Shader module no longer needed after pipeline creation
    s_vk.vkDestroyShaderModule(device, encodeSM, nullptr);
    encodeSM = VK_NULL_HANDLE;

    // ================================================================
    // 10. Shared descriptor pool
    //     Max 2 sets (decode + encode), up to 6 storage buffers total
    // ================================================================
    VkDescriptorPoolSize tcPoolSize = {};
    tcPoolSize.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    tcPoolSize.descriptorCount = 6;  // 2 (decode) + 4 (encode)

    VkDescriptorPoolCreateInfo dpCI = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpCI.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpCI.maxSets       = 2;
    dpCI.poolSizeCount = 1;
    dpCI.pPoolSizes    = &tcPoolSize;

    VkDescriptorPool descPool = VK_NULL_HANDLE;
    vr = s_vk.vkCreateDescriptorPool(device, &dpCI, nullptr, &descPool);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas TC: vkCreateDescriptorPool failed (", vr, ")"));
      s_vk.vkDestroyPipeline(device, encodePipeline, nullptr);
      s_vk.vkDestroyPipelineLayout(device, encodePL, nullptr);
      s_vk.vkDestroyDescriptorSetLayout(device, encodeDSL, nullptr);
      s_vk.vkDestroyPipeline(device, decodePipeline, nullptr);
      s_vk.vkDestroyPipelineLayout(device, decodePL, nullptr);
      s_vk.vkDestroyDescriptorSetLayout(device, decodeDSL, nullptr);
      s_vk.vkDestroyBuffer(device, lutS2Buffer, nullptr);
      s_vk.vkFreeMemory(device, lutS2Memory, nullptr);
      s_vk.vkDestroyBuffer(device, lut2Buffer, nullptr);
      s_vk.vkFreeMemory(device, lut2Memory, nullptr);
      Vegas::s_tcInitialized = true;
      return false;
    }

    // ================================================================
    // 11. Store all handles
    // ================================================================
    Vegas::s_tcDecodePipeline       = reinterpret_cast<uint64_t>(decodePipeline);
    Vegas::s_tcEncodePipeline       = reinterpret_cast<uint64_t>(encodePipeline);
    Vegas::s_tcDecodePipelineLayout = reinterpret_cast<uint64_t>(decodePL);
    Vegas::s_tcEncodePipelineLayout = reinterpret_cast<uint64_t>(encodePL);
    Vegas::s_tcDecodeDescLayout     = reinterpret_cast<uint64_t>(decodeDSL);
    Vegas::s_tcEncodeDescLayout     = reinterpret_cast<uint64_t>(encodeDSL);
    Vegas::s_tcDescPool             = reinterpret_cast<uint64_t>(descPool);
    Vegas::s_tcLut2Buffer           = reinterpret_cast<uint64_t>(lut2Buffer);
    Vegas::s_tcLut2Memory           = reinterpret_cast<uint64_t>(lut2Memory);
    Vegas::s_tcLutS2Buffer          = reinterpret_cast<uint64_t>(lutS2Buffer);
    Vegas::s_tcLutS2Memory          = reinterpret_cast<uint64_t>(lutS2Memory);
    Vegas::s_tcInitialized          = true;

    Logger::debug("Vegas TC: GPU transcoder pipelines initialized");
    return true;
  }


  /** Ensure the scratch RGBA8 buffer exists at the given pixel dimensions.
   *  Destroys and recreates if dimensions changed.
   *  Buffer is device-local with STORAGE usage (optimal for compute R/W). */
  static bool ensureTcScratch(VkDevice device, uint32_t width, uint32_t height) {
    if (Vegas::s_tcScratchBuffer != 0
        && Vegas::s_tcScratchW == width
        && Vegas::s_tcScratchH == height) {
      return true;
    }

    // Destroy old scratch if any
    if (Vegas::s_tcScratchBuffer != 0) {
      s_vk.vkDestroyBuffer(device,
          reinterpret_cast<VkBuffer>(Vegas::s_tcScratchBuffer), nullptr);
      Vegas::s_tcScratchBuffer = 0;
    }
    if (Vegas::s_tcScratchMemory != 0) {
      s_vk.vkFreeMemory(device,
          reinterpret_cast<VkDeviceMemory>(Vegas::s_tcScratchMemory), nullptr);
      Vegas::s_tcScratchMemory = 0;
    }
    Vegas::s_tcScratchW = 0;
    Vegas::s_tcScratchH = 0;

    VkDeviceSize size = VkDeviceSize(width) * VkDeviceSize(height) * 4;

    VkBufferCreateInfo bufCI = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufCI.size        = size;
    bufCI.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer = VK_NULL_HANDLE;
    VkResult vr = s_vk.vkCreateBuffer(device, &bufCI, nullptr, &buffer);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas TC: vkCreateBuffer (scratch) failed (", vr, ")"));
      return false;
    }

    VkMemoryRequirements memReqs;
    s_vk.vkGetBufferMemoryRequirements(device, buffer, &memReqs);

    VkPhysicalDevice physDev = reinterpret_cast<VkPhysicalDevice>(Vegas::s_physicalDevice);
    VkPhysicalDeviceMemoryProperties physMemProps;
    s_vk.vkGetPhysicalDeviceMemoryProperties(physDev, &physMemProps);

    uint32_t memTypeIdx = UINT32_MAX;
    // Prefer DEVICE_LOCAL for best compute performance
    for (uint32_t i = 0; i < physMemProps.memoryTypeCount; ++i) {
      if ((memReqs.memoryTypeBits & (1u << i)) &&
          (physMemProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        memTypeIdx = i;
        break;
      }
    }
    if (memTypeIdx == UINT32_MAX) {
      // Fallback to any compatible type
      for (uint32_t i = 0; i < physMemProps.memoryTypeCount; ++i) {
        if (memReqs.memoryTypeBits & (1u << i)) {
          memTypeIdx = i;
          break;
        }
      }
    }
    if (memTypeIdx == UINT32_MAX) {
      Logger::warn("Vegas TC: no compatible memory for scratch buffer");
      s_vk.vkDestroyBuffer(device, buffer, nullptr);
      return false;
    }

    VkMemoryAllocateInfo allocCI = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocCI.allocationSize  = memReqs.size;
    allocCI.memoryTypeIndex = memTypeIdx;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    vr = s_vk.vkAllocateMemory(device, &allocCI, nullptr, &memory);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas TC: vkAllocateMemory (scratch) failed (", vr, ")"));
      s_vk.vkDestroyBuffer(device, buffer, nullptr);
      return false;
    }

    vr = s_vk.vkBindBufferMemory(device, buffer, memory, 0);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas TC: vkBindBufferMemory (scratch) failed (", vr, ")"));
      s_vk.vkFreeMemory(device, memory, nullptr);
      s_vk.vkDestroyBuffer(device, buffer, nullptr);
      return false;
    }

    Vegas::s_tcScratchBuffer = reinterpret_cast<uint64_t>(buffer);
    Vegas::s_tcScratchMemory = reinterpret_cast<uint64_t>(memory);
    Vegas::s_tcScratchW      = width;
    Vegas::s_tcScratchH      = height;

    Logger::debug(str::format("Vegas TC: scratch buffer created (",
                              width, "x", height, " = ", size, " bytes)"));
    return true;
  }


  // ---- GPU BCn→ASTC transcoder dispatch (Approach A — independent submit) ----

  /** Format-to-format-ID mapping for the decode shader's switch.
   *  Must match the constants in vegas_bcn_decode.comp. */
  static uint32_t bcnFormatToId(VkFormat fmt) {
    switch (fmt) {
      case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
      case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
      case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
      case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return 1;
      case VK_FORMAT_BC3_UNORM_BLOCK:
      case VK_FORMAT_BC3_SRGB_BLOCK:       return 3;
      case VK_FORMAT_BC4_UNORM_BLOCK:
      case VK_FORMAT_BC4_SNORM_BLOCK:      return 4;
      case VK_FORMAT_BC5_UNORM_BLOCK:
      case VK_FORMAT_BC5_SNORM_BLOCK:      return 5;
      case VK_FORMAT_BC7_UNORM_BLOCK:
      case VK_FORMAT_BC7_SRGB_BLOCK:       return 7;
      default:                             return 0;
    }
  }

  bool Vegas::gpuTranscodeImageData(
          VkBuffer             srcBuffer,
          uint32_t             srcOffset,
          VkBuffer             dstBuffer,
          uint32_t             dstOffset,
          VkFormat             srcFormat,
          uint32_t             width,
          uint32_t             height) {
    // ================================================================
    // Validate inputs
    // ================================================================
    uint32_t formatId = bcnFormatToId(srcFormat);
    if (formatId == 0) {
      Logger::warn(str::format("Vegas TC: unsupported source format ",
                               static_cast<int>(srcFormat)));
      return false;
    }

    if (width == 0 || height == 0)
      return false;

    VkDevice device = reinterpret_cast<VkDevice>(s_device);
    VkQueue  queue  = reinterpret_cast<VkQueue>(s_vkQueue);
    if (device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE) {
      Logger::debug("Vegas TC: no VkDevice/VkQueue");
      return false;
    }

    // ================================================================
    // Lazy init
    // ================================================================
    if (!loadVulkanFuncs(device)) {
      Logger::debug("Vegas TC: Vulkan functions not available");
      return false;
    }
    if (!initTranscoderPipeline(device)) {
      Logger::debug("Vegas TC: pipeline init failed");
      return false;
    }
    if (!ensureTcScratch(device, width, height)) {
      Logger::debug("Vegas TC: scratch buffer creation failed");
      return false;
    }

    VkPipeline           decodePipeline  = reinterpret_cast<VkPipeline>(s_tcDecodePipeline);
    VkPipelineLayout     decodeLayout    = reinterpret_cast<VkPipelineLayout>(s_tcDecodePipelineLayout);
    VkDescriptorSetLayout decodeDSL      = reinterpret_cast<VkDescriptorSetLayout>(s_tcDecodeDescLayout);
    VkPipeline           encodePipeline  = reinterpret_cast<VkPipeline>(s_tcEncodePipeline);
    VkPipelineLayout     encodeLayout    = reinterpret_cast<VkPipelineLayout>(s_tcEncodePipelineLayout);
    VkDescriptorSetLayout encodeDSL      = reinterpret_cast<VkDescriptorSetLayout>(s_tcEncodeDescLayout);
    VkDescriptorPool     descPool        = reinterpret_cast<VkDescriptorPool>(s_tcDescPool);
    VkBuffer             scratchBuffer   = reinterpret_cast<VkBuffer>(s_tcScratchBuffer);
    VkBuffer             lut2Buffer      = reinterpret_cast<VkBuffer>(s_tcLut2Buffer);
    VkBuffer             lutS2Buffer     = reinterpret_cast<VkBuffer>(s_tcLutS2Buffer);

    uint32_t blocksX = (width  + 3) / 4;
    uint32_t blocksY = (height + 3) / 4;
    uint32_t numBlocks = blocksX * blocksY;

    VkResult vr;

    // ================================================================
    // Transient command pool + buffer
    // ================================================================
    VkCommandPoolCreateInfo poolCI = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolCI.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolCI.queueFamilyIndex = s_queueFamily;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    vr = s_vk.vkCreateCommandPool(device, &poolCI, nullptr, &cmdPool);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas TC: vkCreateCommandPool failed (", vr, ")"));
      return false;
    }

    VkCommandBufferAllocateInfo allocCI = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocCI.commandPool        = cmdPool;
    allocCI.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocCI.commandBufferCount = 1;
    VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
    vr = s_vk.vkAllocateCommandBuffers(device, &allocCI, &cmdBuf);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas TC: vkAllocateCommandBuffers failed (", vr, ")"));
      s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);
      return false;
    }

    // ================================================================
    // Fence
    // ================================================================
    VkFenceCreateInfo fenceCI = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    vr = s_vk.vkCreateFence(device, &fenceCI, nullptr, &fence);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas TC: vkCreateFence failed (", vr, ")"));
      s_vk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
      s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);
      return false;
    }

    // ================================================================
    // Allocate + update descriptor sets
    // ================================================================
    s_vk.vkResetDescriptorPool(device, descPool, 0);

    // Decode set: {srcBuffer@0 read, scratchBuffer@1 write}
    VkDescriptorSetLayout decodeLayouts[1] = { decodeDSL };
    VkDescriptorSetAllocateInfo descAlloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    descAlloc.descriptorPool     = descPool;
    descAlloc.descriptorSetCount = 1;
    descAlloc.pSetLayouts        = decodeLayouts;
    VkDescriptorSet decodeSet = VK_NULL_HANDLE;
    vr = s_vk.vkAllocateDescriptorSets(device, &descAlloc, &decodeSet);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas TC: vkAllocateDescriptorSets (decode) failed (", vr, ")"));
      s_vk.vkDestroyFence(device, fence, nullptr);
      s_vk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
      s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);
      return false;
    }

    // Encode set: {scratchBuffer@0 read, dstBuffer@1 write, lut2@2 read, lutS2@3 read}
    VkDescriptorSetLayout encodeLayouts[1] = { encodeDSL };
    descAlloc.pSetLayouts = encodeLayouts;
    VkDescriptorSet encodeSet = VK_NULL_HANDLE;
    vr = s_vk.vkAllocateDescriptorSets(device, &descAlloc, &encodeSet);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas TC: vkAllocateDescriptorSets (encode) failed (", vr, ")"));
      s_vk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
      s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);
      s_vk.vkDestroyFence(device, fence, nullptr);
      return false;
    }

    // --- Update decode descriptors ---
    VkDescriptorBufferInfo srcBufInfo = {};
    srcBufInfo.buffer = srcBuffer;
    srcBufInfo.offset = srcOffset;
    srcBufInfo.range  = VK_WHOLE_SIZE;

    VkDescriptorBufferInfo scratchWriteInfo = {};
    scratchWriteInfo.buffer = scratchBuffer;
    scratchWriteInfo.offset = 0;
    scratchWriteInfo.range  = VK_WHOLE_SIZE;

    VkWriteDescriptorSet decodeWrites[2] = {};
    decodeWrites[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    decodeWrites[0].dstSet          = decodeSet;
    decodeWrites[0].dstBinding      = 0;
    decodeWrites[0].descriptorCount = 1;
    decodeWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    decodeWrites[0].pBufferInfo     = &srcBufInfo;

    decodeWrites[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    decodeWrites[1].dstSet          = decodeSet;
    decodeWrites[1].dstBinding      = 1;
    decodeWrites[1].descriptorCount = 1;
    decodeWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    decodeWrites[1].pBufferInfo     = &scratchWriteInfo;

    s_vk.vkUpdateDescriptorSets(device, 2, decodeWrites, 0, nullptr);

    // --- Update encode descriptors ---
    VkDescriptorBufferInfo scratchReadInfo = {};
    scratchReadInfo.buffer = scratchBuffer;
    scratchReadInfo.offset = 0;
    scratchReadInfo.range  = VK_WHOLE_SIZE;

    VkDescriptorBufferInfo dstBufInfo = {};
    dstBufInfo.buffer = dstBuffer;
    dstBufInfo.offset = dstOffset;
    dstBufInfo.range  = VK_WHOLE_SIZE;

    VkDescriptorBufferInfo lut2BufInfo = {};
    lut2BufInfo.buffer = lut2Buffer;
    lut2BufInfo.offset = 0;
    lut2BufInfo.range  = VK_WHOLE_SIZE;

    VkDescriptorBufferInfo lutS2BufInfo = {};
    lutS2BufInfo.buffer = lutS2Buffer;
    lutS2BufInfo.offset = 0;
    lutS2BufInfo.range  = VK_WHOLE_SIZE;

    VkWriteDescriptorSet encodeWrites[4] = {};
    for (int i = 0; i < 4; i++) {
      encodeWrites[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      encodeWrites[i].dstSet          = encodeSet;
      encodeWrites[i].dstBinding      = static_cast<uint32_t>(i);
      encodeWrites[i].descriptorCount = 1;
      encodeWrites[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    encodeWrites[0].pBufferInfo = &scratchReadInfo;
    encodeWrites[1].pBufferInfo = &dstBufInfo;
    encodeWrites[2].pBufferInfo = &lut2BufInfo;
    encodeWrites[3].pBufferInfo = &lutS2BufInfo;

    s_vk.vkUpdateDescriptorSets(device, 4, encodeWrites, 0, nullptr);

    // ================================================================
    // Begin command buffer
    // ================================================================
    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vr = s_vk.vkBeginCommandBuffer(cmdBuf, &beginInfo);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas TC: vkBeginCommandBuffer failed (", vr, ")"));
      s_vk.vkDestroyFence(device, fence, nullptr);
      s_vk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
      s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);
      return false;
    }

    // ================================================================
    // Pass 1: BCn decode — srcBuffer → scratchBuffer
    // ================================================================
    s_vk.vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, decodePipeline);
    s_vk.vkCmdBindDescriptorSets(cmdBuf,
        VK_PIPELINE_BIND_POINT_COMPUTE, decodeLayout,
        0, 1, &decodeSet, 0, nullptr);

    // Push constants: {formatId, width, height} = 3 × uint32_t = 12 bytes
    struct { uint32_t fmt; uint32_t w; uint32_t h; } decodePC = { formatId, width, height };
    s_vk.vkCmdPushConstants(cmdBuf, decodeLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(decodePC), &decodePC);

    s_vk.vkCmdDispatch(cmdBuf, blocksX, blocksY, 1);

    // ================================================================
    // Barrier: scratch buffer write → read (decode → encode)
    // ================================================================
    VkBufferMemoryBarrier scratchBarrier = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    scratchBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    scratchBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    scratchBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    scratchBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    scratchBarrier.buffer  = scratchBuffer;
    scratchBarrier.offset  = 0;
    scratchBarrier.size    = VK_WHOLE_SIZE;

    s_vk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 1, &scratchBarrier, 0, nullptr);

    // ================================================================
    // Pass 2: ASTC encode — scratchBuffer → dstBuffer
    // ================================================================
    s_vk.vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, encodePipeline);
    s_vk.vkCmdBindDescriptorSets(cmdBuf,
        VK_PIPELINE_BIND_POINT_COMPUTE, encodeLayout,
        0, 1, &encodeSet, 0, nullptr);

    // Push constants: {texWidth, texHeight, flags} = ivec2 + uint = 12 bytes
    // flags: bit 1 = try_2p (try 2-partition mode), bit 2 = only_2p (force 2P)
    struct { int32_t w; int32_t h; uint32_t flags; } encodePC;
    encodePC.w     = static_cast<int32_t>(width);
    encodePC.h     = static_cast<int32_t>(height);
    encodePC.flags = 2u;  // bit 1 set → try 2P, compare MSE with 1P

    s_vk.vkCmdPushConstants(cmdBuf, encodeLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(encodePC), &encodePC);

    s_vk.vkCmdDispatch(cmdBuf, blocksX, blocksY, 1);

    // ================================================================
    // End command buffer
    // ================================================================
    vr = s_vk.vkEndCommandBuffer(cmdBuf);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas TC: vkEndCommandBuffer failed (", vr, ")"));
      s_vk.vkDestroyFence(device, fence, nullptr);
      s_vk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
      s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);
      return false;
    }

    // ================================================================
    // Submit with fence and wait
    // ================================================================
    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &cmdBuf;

    vr = s_vk.vkQueueSubmit(queue, 1, &submitInfo, fence);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas TC: vkQueueSubmit failed (", vr, ")"));
      s_vk.vkDestroyFence(device, fence, nullptr);
      s_vk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
      s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);
      return false;
    }

    vr = s_vk.vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas TC: vkWaitForFences failed (", vr, ")"));
    }

    // Cleanup transient resources
    s_vk.vkDestroyFence(device, fence, nullptr);
    s_vk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
    s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);

    Logger::debug(str::format("Vegas TC: transcoded ", width, "x", height,
                              " (", numBlocks, " blocks) BCn->ASTC"));
    return true;
  }
   *  Creates a private VkImage with STORAGE_BIT + TRANSFER_SRC_BIT.
   *  Destroys and recreates if dimensions changed (swapchain resize). */
  static bool ensureFsrIntermediate(VkDevice device, VkExtent3D extent) {
    if (Vegas::s_fsrInterImage != 0 && Vegas::s_fsrInterW == extent.width && Vegas::s_fsrInterH == extent.height) {
      return true;  // already exists at correct size
    }

    // Drain any in-flight async FSR compute before destroying the
    // intermediate image (prevents GPU crash on resize).
    Vegas::fsrDrain();

    // Destroy old intermediate if any (size mismatch or first init)
    if (Vegas::s_fsrInterView != 0) {
      s_vk.vkDestroyImageView(device, reinterpret_cast<VkImageView>(Vegas::s_fsrInterView), nullptr);
      Vegas::s_fsrInterView = 0;
    }
    if (Vegas::s_fsrInterImage != 0) {
      s_vk.vkDestroyImage(device, reinterpret_cast<VkImage>(Vegas::s_fsrInterImage), nullptr);
      Vegas::s_fsrInterImage = 0;
    }
    if (Vegas::s_fsrInterMemory != 0) {
      s_vk.vkFreeMemory(device, reinterpret_cast<VkDeviceMemory>(Vegas::s_fsrInterMemory), nullptr);
      Vegas::s_fsrInterMemory = 0;
    }
    Vegas::s_fsrInterW = 0;
    Vegas::s_fsrInterH = 0;

    VkImageCreateInfo imgCI = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imgCI.imageType     = VK_IMAGE_TYPE_2D;
    imgCI.format        = VK_FORMAT_R8G8B8A8_UNORM;
    imgCI.extent.width  = extent.width;
    imgCI.extent.height = extent.height;
    imgCI.extent.depth  = 1;
    imgCI.mipLevels     = 1;
    imgCI.arrayLayers   = 1;
    imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;
    imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imgCI.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage interImage = VK_NULL_HANDLE;
    VkResult vr = s_vk.vkCreateImage(device, &imgCI, nullptr, &interImage);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR: vkCreateImage(intermediate) failed (", vr, ")"));
      return false;
    }

    VkMemoryRequirements memReqs;
    s_vk.vkGetImageMemoryRequirements(device, interImage, &memReqs);

    VkPhysicalDevice physDev = reinterpret_cast<VkPhysicalDevice>(Vegas::s_physicalDevice);
    VkPhysicalDeviceMemoryProperties physMemProps;
    s_vk.vkGetPhysicalDeviceMemoryProperties(physDev, &physMemProps);

    uint32_t memTypeIdx = UINT32_MAX;
    for (uint32_t i = 0; i < physMemProps.memoryTypeCount; ++i) {
      if ((memReqs.memoryTypeBits & (1u << i)) &&
          (physMemProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        memTypeIdx = i;
        break;
      }
    }
    if (memTypeIdx == UINT32_MAX) {
      // Fallback: any compatible type (may be HOST_VISIBLE on integrated GPUs)
      for (uint32_t i = 0; i < physMemProps.memoryTypeCount; ++i) {
        if (memReqs.memoryTypeBits & (1u << i)) {
          memTypeIdx = i;
          break;
        }
      }
    }
    if (memTypeIdx == UINT32_MAX) {
      Logger::warn("Vegas FSR: no compatible memory type for intermediate image");
      s_vk.vkDestroyImage(device, interImage, nullptr);
      return false;
    }

    VkMemoryAllocateInfo allocCI = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocCI.allocationSize  = memReqs.size;
    allocCI.memoryTypeIndex = memTypeIdx;

    VkDeviceMemory interMem = VK_NULL_HANDLE;
    vr = s_vk.vkAllocateMemory(device, &allocCI, nullptr, &interMem);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR: vkAllocateMemory(intermediate) failed (", vr, ")"));
      s_vk.vkDestroyImage(device, interImage, nullptr);
      return false;
    }

    vr = s_vk.vkBindImageMemory(device, interImage, interMem, 0);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR: vkBindImageMemory(intermediate) failed (", vr, ")"));
      s_vk.vkFreeMemory(device, interMem, nullptr);
      s_vk.vkDestroyImage(device, interImage, nullptr);
      return false;
    }

    Vegas::s_fsrInterImage  = reinterpret_cast<uint64_t>(interImage);
    Vegas::s_fsrInterMemory = reinterpret_cast<uint64_t>(interMem);
    Vegas::s_fsrInterW      = extent.width;
    Vegas::s_fsrInterH      = extent.height;

    // Create persistent intermediate image view (for async FSR descriptor set)
    VkImageViewCreateInfo viewCI = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewCI.image            = interImage;
    viewCI.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format           = VK_FORMAT_R8G8B8A8_UNORM;
    viewCI.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewCI.subresourceRange.baseMipLevel   = 0;
    viewCI.subresourceRange.levelCount     = 1;
    viewCI.subresourceRange.baseArrayLayer = 0;
    viewCI.subresourceRange.layerCount     = 1;

    VkImageView interView = VK_NULL_HANDLE;
    vr = s_vk.vkCreateImageView(device, &viewCI, nullptr, &interView);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR: vkCreateImageView(intermediate) failed (", vr, ")"));
      // Non-fatal: keep image, view will be created on demand
      Vegas::s_fsrInterView = 0;
    } else {
      Vegas::s_fsrInterView = reinterpret_cast<uint64_t>(interView);
    }

    Logger::debug(str::format("Vegas FSR: intermediate image created (",
                              extent.width, "x", extent.height, ")"));
    return true;
  }


  bool Vegas::fsrUpscale(
          VkImage              srcImage,
          VkImage              dstImage,
          VkExtent3D           srcExtent,
          VkExtent3D           dstExtent,
          VkFormat             swapchainFormat,
          VegasFsrConstants&   fsrConsts) {
    // ================================================================
    // Format guard — FSR only on UNORM swapchain formats
    // ================================================================
    if (swapchainFormat != VK_FORMAT_B8G8R8A8_UNORM &&
        swapchainFormat != VK_FORMAT_R8G8B8A8_UNORM) {
      Logger::debug(str::format(
          "Vegas FSR: skipped — unsupported swapchain format 0x",
          std::hex, static_cast<uint32_t>(swapchainFormat)));
      return false;
    }

    // ================================================================
    // Get device & queue handles
    // ================================================================
    VkDevice device = reinterpret_cast<VkDevice>(s_device);
    VkQueue  queue  = reinterpret_cast<VkQueue>(s_vkQueue);
    if (device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE) {
      Logger::debug("Vegas FSR: skipped — no VkDevice/VkQueue");
      return false;
    }

    // ================================================================
    // Load Vulkan functions (lazy, one-time)
    // ================================================================
    if (!loadVulkanFuncs(device)) {
      Logger::debug("Vegas FSR: skipped — Vulkan functions not available");
      return false;
    }

    // ================================================================
    // Init FSR pipeline (lazy, one-time)
    // ================================================================
    if (!initFsrPipeline(device)) {
      Logger::debug("Vegas FSR: skipped — pipeline init failed");
      return false;
    }

    // ================================================================
    // Ensure intermediate image exists at dstExtent
    // ================================================================
    if (!ensureFsrIntermediate(device, dstExtent)) {
      Logger::debug("Vegas FSR: skipped — intermediate image creation failed");
      return false;
    }

    VkPipeline              pipeline        = reinterpret_cast<VkPipeline>(s_fsrPipeline);
    VkPipelineLayout        pipelineLayout  = reinterpret_cast<VkPipelineLayout>(s_fsrPipelineLayout);
    VkDescriptorSetLayout   descSetLayout   = reinterpret_cast<VkDescriptorSetLayout>(s_fsrDescSetLayout);
    VkDescriptorPool        descPool        = reinterpret_cast<VkDescriptorPool>(s_fsrDescPool);
    VkImage                 interImage      = reinterpret_cast<VkImage>(s_fsrInterImage);

    VkResult vr;

    // --- Create temporary command pool ---
    VkCommandPoolCreateInfo poolCI = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolCI.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolCI.queueFamilyIndex = s_queueFamily;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    vr = s_vk.vkCreateCommandPool(device, &poolCI, nullptr, &cmdPool);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR: vkCreateCommandPool failed (", vr, ")"));
      return false;
    }

    // --- Allocate command buffer ---
    VkCommandBufferAllocateInfo allocCI = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocCI.commandPool        = cmdPool;
    allocCI.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocCI.commandBufferCount = 1;
    VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
    vr = s_vk.vkAllocateCommandBuffers(device, &allocCI, &cmdBuf);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR: vkAllocateCommandBuffers failed (", vr, ")"));
      s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);
      return false;
    }

    // --- Create fence ---
    VkFenceCreateInfo fenceCI = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    vr = s_vk.vkCreateFence(device, &fenceCI, nullptr, &fence);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR: vkCreateFence failed (", vr, ")"));
      s_vk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
      s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);
      return false;
    }

    // --- Create temporary image views ---
    VkImageViewCreateInfo viewCI = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewCI.viewType     = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format       = swapchainFormat;  // src matches swapchain format
    viewCI.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewCI.subresourceRange.baseMipLevel   = 0;
    viewCI.subresourceRange.levelCount     = 1;
    viewCI.subresourceRange.baseArrayLayer = 0;
    viewCI.subresourceRange.layerCount     = 1;

    viewCI.image = srcImage;
    VkImageView srcView = VK_NULL_HANDLE;
    vr = s_vk.vkCreateImageView(device, &viewCI, nullptr, &srcView);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR: vkCreateImageView(src) failed (", vr, ")"));
      s_vk.vkDestroyFence(device, fence, nullptr);
      s_vk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
      s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);
      return false;
    }

    // Intermediate view uses R8G8B8A8_UNORM (guaranteed storage support)
    viewCI.image  = interImage;
    viewCI.format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageView interView = VK_NULL_HANDLE;
    vr = s_vk.vkCreateImageView(device, &viewCI, nullptr, &interView);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR: vkCreateImageView(intermediate) failed (", vr, ")"));
      s_vk.vkDestroyImageView(device, srcView, nullptr);
      s_vk.vkDestroyFence(device, fence, nullptr);
      s_vk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
      s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);
      return false;
    }

    // --- Allocate + update descriptor set ---
    s_vk.vkResetDescriptorPool(device, descPool, 0);

    VkDescriptorSetAllocateInfo descAlloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    descAlloc.descriptorPool     = descPool;
    descAlloc.descriptorSetCount = 1;
    descAlloc.pSetLayouts        = &descSetLayout;
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    vr = s_vk.vkAllocateDescriptorSets(device, &descAlloc, &descSet);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR: vkAllocateDescriptorSets failed (", vr, ")"));
      s_vk.vkDestroyImageView(device, interView, nullptr);
      s_vk.vkDestroyImageView(device, srcView, nullptr);
      s_vk.vkDestroyFence(device, fence, nullptr);
      s_vk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
      s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);
      return false;
    }

    VkDescriptorImageInfo srcImgInfo = {};
    srcImgInfo.sampler     = VK_NULL_HANDLE;
    srcImgInfo.imageView   = srcView;
    srcImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo interImgInfo = {};
    interImgInfo.sampler     = VK_NULL_HANDLE;
    interImgInfo.imageView   = interView;
    interImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[2] = {};
    writes[0].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet           = descSet;
    writes[0].dstBinding       = 0;
    writes[0].descriptorCount  = 1;
    writes[0].descriptorType   = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].pImageInfo       = &srcImgInfo;

    writes[1].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet           = descSet;
    writes[1].dstBinding       = 1;
    writes[1].descriptorCount  = 1;
    writes[1].descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo       = &interImgInfo;

    s_vk.vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

    // ================================================================
    // Record command buffer — intermediate target + blit
    // ================================================================
    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vr = s_vk.vkBeginCommandBuffer(cmdBuf, &beginInfo);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR: vkBeginCommandBuffer failed (", vr, ")"));
      s_vk.vkDestroyImageView(device, interView, nullptr);
      s_vk.vkDestroyImageView(device, srcView, nullptr);
      s_vk.vkDestroyFence(device, fence, nullptr);
      s_vk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
      s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);
      return false;
    }

    // ----------------------------------------------------------------
    // Pre-dispatch barriers (split into two calls because srcImage
    // needs COLOR_ATTACHMENT_OUTPUT write visibility; inter + dst
    // have no producer to synchronize with).
    // ----------------------------------------------------------------
    // Barrier 1a: src PRESENT_SRC_KHR -> GENERAL (for shader read)
    //   srcStage/access must cover the COLOR_ATTACHMENT_OUTPUT writes
    //   from the previous render pass that produced srcImage content.
    VkImageMemoryBarrier srcBarrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    srcBarrier.srcAccessMask    = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    srcBarrier.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
    srcBarrier.oldLayout        = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    srcBarrier.newLayout        = VK_IMAGE_LAYOUT_GENERAL;
    srcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBarrier.image            = srcImage;
    srcBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    s_vk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &srcBarrier);

    // Barrier 1b: intermediate UNDEFINED -> GENERAL (for shader write)
    VkImageMemoryBarrier interBarrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    interBarrier.srcAccessMask    = 0;
    interBarrier.dstAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
    interBarrier.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
    interBarrier.newLayout        = VK_IMAGE_LAYOUT_GENERAL;
    interBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    interBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    interBarrier.image            = interImage;
    interBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // Barrier 1c: dst PRESENT_SRC_KHR -> TRANSFER_DST_OPTIMAL (for blit)
    VkImageMemoryBarrier dstBarrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    dstBarrier.srcAccessMask    = 0;
    dstBarrier.dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstBarrier.oldLayout        = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    dstBarrier.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstBarrier.image            = dstImage;
    dstBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkImageMemoryBarrier preBarriers[2] = { interBarrier, dstBarrier };
    s_vk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 2, preBarriers);

    // --- FSR compute dispatch: src -> intermediate ---
    s_vk.vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    s_vk.vkCmdBindDescriptorSets(cmdBuf,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        pipelineLayout, 0, 1, &descSet, 0, nullptr);
    s_vk.vkCmdPushConstants(cmdBuf, pipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0,
        sizeof(VegasFsrConstants), &fsrConsts);

    uint32_t gx = (dstExtent.width  + 15) / 16;
    uint32_t gy = (dstExtent.height + 15) / 16;
    s_vk.vkCmdDispatch(cmdBuf, gx, gy, 1);

    // Barrier 4: intermediate GENERAL -> TRANSFER_SRC_OPTIMAL (for blit read)
    VkImageMemoryBarrier interToBlit = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    interToBlit.srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
    interToBlit.dstAccessMask    = VK_ACCESS_TRANSFER_READ_BIT;
    interToBlit.oldLayout        = VK_IMAGE_LAYOUT_GENERAL;
    interToBlit.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    interToBlit.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    interToBlit.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    interToBlit.image            = interImage;
    interToBlit.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    s_vk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &interToBlit);

    // --- Blit intermediate -> dst (nearest filter, 1:1 scale) ---
    VkImageBlit blitRegion = {};
    blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blitRegion.srcSubresource.layerCount = 1;
    blitRegion.srcOffsets[0] = { 0, 0, 0 };
    blitRegion.srcOffsets[1] = { static_cast<int32_t>(dstExtent.width),
                                 static_cast<int32_t>(dstExtent.height), 1 };
    blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blitRegion.dstSubresource.layerCount = 1;
    blitRegion.dstOffsets[0] = { 0, 0, 0 };
    blitRegion.dstOffsets[1] = { static_cast<int32_t>(dstExtent.width),
                                 static_cast<int32_t>(dstExtent.height), 1 };

    s_vk.vkCmdBlitImage(cmdBuf,
        interImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dstImage,   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blitRegion, VK_FILTER_NEAREST);

    // Barrier 5a: src GENERAL -> PRESENT_SRC_KHR (restore for future acquire)
    VkImageMemoryBarrier srcBack = {};
    srcBack.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    srcBack.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    srcBack.dstAccessMask       = 0;  // no producer — just layout restore
    srcBack.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    srcBack.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    srcBack.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBack.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBack.image               = srcImage;
    srcBack.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // Barrier 5b: dst TRANSFER_DST_OPTIMAL -> PRESENT_SRC_KHR (for present)
    VkImageMemoryBarrier dstBack = {};
    dstBack.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    dstBack.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstBack.dstAccessMask       = 0;  // presentation reads via queue
    dstBack.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBack.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    dstBack.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstBack.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstBack.image               = dstImage;
    dstBack.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // Use ALL_COMMANDS_BIT as srcStage to cover both the compute
    // shader read (srcImage → GENERAL→PRESENT) and the blit write
    // (dstImage → TRANSFER_DST→PRESENT) without splitting the call.
    VkImageMemoryBarrier postBarriers[2] = { srcBack, dstBack };
    s_vk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 2, postBarriers);

    vr = s_vk.vkEndCommandBuffer(cmdBuf);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR: vkEndCommandBuffer failed (", vr, ")"));
      s_vk.vkDestroyImageView(device, interView, nullptr);
      s_vk.vkDestroyImageView(device, srcView, nullptr);
      s_vk.vkDestroyFence(device, fence, nullptr);
      s_vk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
      s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);
      return false;
    }

    // ================================================================
    // Submit with fence
    // ================================================================
    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &cmdBuf;

    vr = s_vk.vkQueueSubmit(queue, 1, &submitInfo, fence);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR: vkQueueSubmit failed (", vr, ")"));
      s_vk.vkDestroyImageView(device, interView, nullptr);
      s_vk.vkDestroyImageView(device, srcView, nullptr);
      s_vk.vkDestroyFence(device, fence, nullptr);
      s_vk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
      s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);
      return false;
    }

    vr = s_vk.vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR: vkWaitForFences failed (", vr, ")"));
    }

    // Cleanup temporary resources
    s_vk.vkDestroyImageView(device, interView, nullptr);
    s_vk.vkDestroyImageView(device, srcView, nullptr);
    s_vk.vkDestroyFence(device, fence, nullptr);
    s_vk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
    s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);

    Logger::debug(str::format("Vegas FSR: upscaled ", srcExtent.width, "x", srcExtent.height,
        " -> ", dstExtent.width, "x", dstExtent.height));
    return true;
  }


  // ================================================================
  // ====  Async FSR — DxvkFence (timeline semaphore)  ===============
  // ================================================================
  //
  // Splits the original synchronous fsrUpscale into two non-blocking
  // phases:
  //
  //   Phase 1 (fsrUpscaleAsync):
  //     Submit EASU compute to intermediate image with a timeline
  //     semaphore (DxvkFence) signal.  Returns immediately — CPU does
  //     NOT wait for GPU completion.
  //
  //   Phase 2 (fsrTryBlitResult):
  //     On the next frame, check the timeline semaphore non-blockingly
  //     (DxvkFence::getValue).  If the compute is done, submit a fast
  //     synchronous blit from intermediate → swapchain dst (≈0.1 ms).
  //
  // Result: zero CPU blocking for the expensive compute dispach;
  // only the cheap blit blocks, for ~0.1 ms per frame.
  //
  // Resource lifetime:
  //   - intermediate image: persistent, managed by ensureFsrIntermediate
  //   - intermediate view (interView): persistent, managed by ensureFsrIntermediate
  //   - src image view: created per frame in fsrUpscaleAsync, destroyed
  //     in fsrTryBlitResult after timeline semaphore signals.
  //
  // ================================================================


  bool Vegas::fsrUpscaleAsync(
          VkImage              srcImage,
          VkExtent3D           srcExtent,
          VkExtent3D           dstExtent,
          VkFormat             swapchainFormat,
          VegasFsrConstants&   fsrConsts) {
    // ================================================================
    // Guard: only one async FSR in flight at a time
    // ================================================================
    if (s_fsrInFlight) {
      Logger::debug("Vegas FSR async: skipped — previous compute still in flight");
      return false;
    }

    // ================================================================
    // Format guard — FSR only on UNORM swapchain formats
    // ================================================================
    if (swapchainFormat != VK_FORMAT_B8G8R8A8_UNORM &&
        swapchainFormat != VK_FORMAT_R8G8B8A8_UNORM) {
      Logger::debug(str::format(
          "Vegas FSR async: skipped — unsupported swapchain format 0x",
          std::hex, static_cast<uint32_t>(swapchainFormat)));
      return false;
    }

    // ================================================================
    // Get device & queue handles
    // ================================================================
    VkDevice device = reinterpret_cast<VkDevice>(s_device);
    VkQueue  queue  = reinterpret_cast<VkQueue>(s_vkQueue);
    if (device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE) {
      Logger::debug("Vegas FSR async: skipped — no VkDevice/VkQueue");
      return false;
    }

    // ================================================================
    // Load Vulkan functions (lazy, one-time)
    // ================================================================
    if (!loadVulkanFuncs(device)) {
      Logger::debug("Vegas FSR async: skipped — Vulkan functions not available");
      return false;
    }

    // ================================================================
    // Init FSR pipeline (lazy, one-time)
    // ================================================================
    if (!initFsrPipeline(device)) {
      Logger::debug("Vegas FSR async: skipped — pipeline init failed");
      return false;
    }

    // ================================================================
    // Ensure intermediate image exists at dstExtent
    // ================================================================
    if (!ensureFsrIntermediate(device, dstExtent)) {
      Logger::debug("Vegas FSR async: skipped — intermediate image creation failed");
      return false;
    }

    // ================================================================
    // Ensure DxvkFence (timeline semaphore) exists
    // ================================================================
    if (!s_fsrFence) {
      if (!s_dxvkDevice) {
        Logger::debug("Vegas FSR async: skipped — no DxvkDevice for fence creation");
        return false;
      }
      DxvkFenceCreateInfo fenceInfo = {};
      fenceInfo.initialValue = 0;
      s_fsrFence = reinterpret_cast<void*>(new DxvkFence(s_dxvkDevice, fenceInfo));
      s_fsrNextValue = 1;
      Logger::debug("Vegas FSR async: DxvkFence created");
    }

    VkPipeline            pipeline        = reinterpret_cast<VkPipeline>(s_fsrPipeline);
    VkPipelineLayout      pipelineLayout  = reinterpret_cast<VkPipelineLayout>(s_fsrPipelineLayout);
    VkDescriptorSetLayout descSetLayout   = reinterpret_cast<VkDescriptorSetLayout>(s_fsrDescSetLayout);
    VkDescriptorPool      descPool        = reinterpret_cast<VkDescriptorPool>(s_fsrDescPool);
    VkImage               interImage      = reinterpret_cast<VkImage>(s_fsrInterImage);
    VkImageView           interView       = reinterpret_cast<VkImageView>(s_fsrInterView);

    VkResult vr;

    // --- Create/lazy-init persistent async command pool + buffer ---
    // These live across frames to avoid destroying a pool while a
    // submitted command buffer is still pending on the GPU.
    if (s_fsrAsyncCmdPool == 0) {
      VkCommandPoolCreateInfo poolCI = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
      poolCI.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
      poolCI.queueFamilyIndex = s_queueFamily;
      VkCommandPool newPool = VK_NULL_HANDLE;
      vr = s_vk.vkCreateCommandPool(device, &poolCI, nullptr, &newPool);
      if (vr != VK_SUCCESS) {
        Logger::warn(str::format("Vegas FSR async: vkCreateCommandPool failed (", vr, ")"));
        return false;
      }
      s_fsrAsyncCmdPool = reinterpret_cast<uint64_t>(newPool);
    }
    if (s_fsrAsyncCmdBuf == 0) {
      VkCommandBufferAllocateInfo allocCI = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
      allocCI.commandPool        = reinterpret_cast<VkCommandPool>(s_fsrAsyncCmdPool);
      allocCI.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      allocCI.commandBufferCount = 1;
      VkCommandBuffer newBuf = VK_NULL_HANDLE;
      vr = s_vk.vkAllocateCommandBuffers(device, &allocCI, &newBuf);
      if (vr != VK_SUCCESS) {
        Logger::warn(str::format("Vegas FSR async: vkAllocateCommandBuffers failed (", vr, ")"));
        return false;
      }
      s_fsrAsyncCmdBuf = reinterpret_cast<uint64_t>(newBuf);
    }

    VkCommandBuffer cmdBuf  = reinterpret_cast<VkCommandBuffer>(s_fsrAsyncCmdBuf);

    // vkBeginCommandBuffer below implicitly resets the buffer.
    // No explicit vkResetCommandBuffer needed — the timeline semaphore
    // guarantees the previous submit completed, returning the buffer
    // to VK_COMMAND_BUFFER_STATE_INITIAL.

    // --- Create src view (per-frame; destroyed after timeline signals) ---
    VkImageViewCreateInfo viewCI = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewCI.viewType     = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format       = swapchainFormat;
    viewCI.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewCI.subresourceRange.baseMipLevel   = 0;
    viewCI.subresourceRange.levelCount     = 1;
    viewCI.subresourceRange.baseArrayLayer = 0;
    viewCI.subresourceRange.layerCount     = 1;

    viewCI.image = srcImage;
    VkImageView srcView = VK_NULL_HANDLE;
    vr = s_vk.vkCreateImageView(device, &viewCI, nullptr, &srcView);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR async: vkCreateImageView(src) failed (", vr, ")"));
      return false;
    }

    // --- Allocate + update descriptor set ---
    s_vk.vkResetDescriptorPool(device, descPool, 0);

    VkDescriptorSetAllocateInfo descAlloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    descAlloc.descriptorPool     = descPool;
    descAlloc.descriptorSetCount = 1;
    descAlloc.pSetLayouts        = &descSetLayout;
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    vr = s_vk.vkAllocateDescriptorSets(device, &descAlloc, &descSet);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR async: vkAllocateDescriptorSets failed (", vr, ")"));
      s_vk.vkDestroyImageView(device, srcView, nullptr);
      return false;
    }

    VkDescriptorImageInfo srcImgInfo = {};
    srcImgInfo.sampler     = VK_NULL_HANDLE;
    srcImgInfo.imageView   = srcView;
    srcImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo interImgInfo = {};
    interImgInfo.sampler     = VK_NULL_HANDLE;
    interImgInfo.imageView   = interView;
    interImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[2] = {};
    writes[0].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet           = descSet;
    writes[0].dstBinding       = 0;
    writes[0].descriptorCount  = 1;
    writes[0].descriptorType   = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].pImageInfo       = &srcImgInfo;

    writes[1].sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet           = descSet;
    writes[1].dstBinding       = 1;
    writes[1].descriptorCount  = 1;
    writes[1].descriptorType   = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo       = &interImgInfo;

    s_vk.vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

    // ================================================================
    // Record command buffer — compute only (no blit)
    // ================================================================
    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vr = s_vk.vkBeginCommandBuffer(cmdBuf, &beginInfo);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR async: vkBeginCommandBuffer failed (", vr, ")"));
      s_vk.vkDestroyImageView(device, srcView, nullptr);
      return false;
    }

    // Barrier: src PRESENT_SRC_KHR -> GENERAL (for shader read)
    VkImageMemoryBarrier srcBarrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    srcBarrier.srcAccessMask    = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    srcBarrier.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
    srcBarrier.oldLayout        = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    srcBarrier.newLayout        = VK_IMAGE_LAYOUT_GENERAL;
    srcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBarrier.image            = srcImage;
    srcBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    s_vk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &srcBarrier);

    // Barrier: intermediate UNDEFINED -> GENERAL (contents always overwritten)
    // Using UNDEFINED is safe: the compute shader writes every pixel.
    // This also gives the driver freedom to discard stale tile data.
    VkImageMemoryBarrier interBarrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    interBarrier.srcAccessMask    = 0;
    interBarrier.dstAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
    interBarrier.oldLayout        = VK_IMAGE_LAYOUT_UNDEFINED;
    interBarrier.newLayout        = VK_IMAGE_LAYOUT_GENERAL;
    interBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    interBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    interBarrier.image            = interImage;
    interBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    s_vk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &interBarrier);

    // --- FSR compute dispatch: src -> intermediate ---
    s_vk.vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    s_vk.vkCmdBindDescriptorSets(cmdBuf,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        pipelineLayout, 0, 1, &descSet, 0, nullptr);
    s_vk.vkCmdPushConstants(cmdBuf, pipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0,
        sizeof(VegasFsrConstants), &fsrConsts);

    uint32_t gx = (dstExtent.width  + 15) / 16;
    uint32_t gy = (dstExtent.height + 15) / 16;
    s_vk.vkCmdDispatch(cmdBuf, gx, gy, 1);

    // Barrier: src GENERAL -> PRESENT_SRC_KHR (restore for next acquire)
    VkImageMemoryBarrier srcBack = {};
    srcBack.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    srcBack.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    srcBack.dstAccessMask       = 0;
    srcBack.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    srcBack.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    srcBack.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBack.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    srcBack.image               = srcImage;
    srcBack.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    s_vk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &srcBack);

    // NOTE: inter remains in GENERAL — fsrTryBlitResult will transition
    // to TRANSFER_SRC_OPTIMAL for the blit and back to GENERAL afterwards.

    vr = s_vk.vkEndCommandBuffer(cmdBuf);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR async: vkEndCommandBuffer failed (", vr, ")"));
      s_vk.vkDestroyImageView(device, srcView, nullptr);
      return false;
    }

    // ================================================================
    // Submit with timeline semaphore signal — NO WAIT
    // ================================================================
    DxvkFence* fence = reinterpret_cast<DxvkFence*>(s_fsrFence);
    uint64_t signalValue = s_fsrNextValue;
    VkSemaphore timelineSema = fence->handle();

    VkTimelineSemaphoreSubmitInfo timelineInfo = {
      VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    timelineInfo.waitSemaphoreValueCount   = 0;
    timelineInfo.pWaitSemaphoreValues      = nullptr;
    timelineInfo.signalSemaphoreValueCount = 1;
    timelineInfo.pSignalSemaphoreValues    = &signalValue;

    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.pNext                  = &timelineInfo;
    submitInfo.commandBufferCount     = 1;
    submitInfo.pCommandBuffers        = &cmdBuf;
    submitInfo.signalSemaphoreCount   = 1;
    submitInfo.pSignalSemaphores      = &timelineSema;

    vr = s_vk.vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR async: vkQueueSubmit failed (", vr, ")"));
      s_vk.vkDestroyImageView(device, srcView, nullptr);
      return false;
    }

    // ================================================================
    // Post-submit bookkeeping
    // ================================================================

    // Destroy any leftover src view from a prior submit that was never
    // cleaned up (should not happen if s_fsrInFlight was correctly false,
    // but guard against fence gets eaten by an error path).
    if (s_fsrLastSrcView != 0) {
      s_vk.vkDestroyImageView(device,
          reinterpret_cast<VkImageView>(s_fsrLastSrcView), nullptr);
      s_fsrLastSrcView = 0;
    }

    // Store srcView for destruction after fence signals
    s_fsrLastSrcView = reinterpret_cast<uint64_t>(srcView);

    // Advance timeline value
    s_fsrNextValue++;

    // Mark in-flight
    s_fsrInFlight = true;

    Logger::debug(str::format("Vegas FSR async: submitted compute ",
        srcExtent.width, "x", srcExtent.height,
        " -> ", dstExtent.width, "x", dstExtent.height,
        " (timeline value ", signalValue, ")"));
    return true;
  }


  bool Vegas::fsrTryBlitResult(
          VkImage              dstImage,
          VkExtent3D           dstExtent) {
    // ================================================================
    // Nothing to blit if no async FSR is in flight
    // ================================================================
    if (!s_fsrInFlight) {
      return false;
    }

    if (!s_fsrFence) {
      return false;
    }

    // ================================================================
    // Non-blocking check: has the previous async compute completed?
    // ================================================================
    DxvkFence* fence = reinterpret_cast<DxvkFence*>(s_fsrFence);
    uint64_t completedValue = fence->getValue();
    uint64_t expectedValue  = s_fsrNextValue - 1; // last submitted value

    if (completedValue < expectedValue) {
      // FSR compute still running — skip blit this frame
      return false;
    }

    // ================================================================
    // Compute is done! Destroy the previous frame's src view.
    // ================================================================
    VkDevice device = reinterpret_cast<VkDevice>(s_device);
    VkQueue  queue  = reinterpret_cast<VkQueue>(s_vkQueue);
    if (device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE) {
      Logger::debug("Vegas FSR blit: skipped — no VkDevice/VkQueue");
      return false;
    }

    if (s_fsrLastSrcView != 0) {
      s_vk.vkDestroyImageView(device,
          reinterpret_cast<VkImageView>(s_fsrLastSrcView), nullptr);
      s_fsrLastSrcView = 0;
    }

    // ================================================================
    // Synchronous blit: inter (GENERAL) → dst (PRESENT → TRANSFER_DST)
    // ================================================================
    VkImage interImage = reinterpret_cast<VkImage>(s_fsrInterImage);
    if (interImage == VK_NULL_HANDLE) {
      s_fsrInFlight = false;
      return false;
    }

    // --- Create temp command pool + buffer + fence ---
    VkCommandPoolCreateInfo poolCI = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolCI.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolCI.queueFamilyIndex = s_queueFamily;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkResult vr = s_vk.vkCreateCommandPool(device, &poolCI, nullptr, &cmdPool);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR blit: vkCreateCommandPool failed (", vr, ")"));
      s_fsrInFlight = false;
      return false;
    }

    VkCommandBufferAllocateInfo allocCI = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocCI.commandPool        = cmdPool;
    allocCI.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocCI.commandBufferCount = 1;
    VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
    vr = s_vk.vkAllocateCommandBuffers(device, &allocCI, &cmdBuf);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR blit: vkAllocateCommandBuffers failed (", vr, ")"));
      s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);
      s_fsrInFlight = false;
      return false;
    }

    VkFenceCreateInfo fenceCI = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence blitFence = VK_NULL_HANDLE;
    vr = s_vk.vkCreateFence(device, &fenceCI, nullptr, &blitFence);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR blit: vkCreateFence failed (", vr, ")"));
      s_vk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
      s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);
      s_fsrInFlight = false;
      return false;
    }

    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vr = s_vk.vkBeginCommandBuffer(cmdBuf, &beginInfo);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR blit: vkBeginCommandBuffer failed (", vr, ")"));
      s_vk.vkDestroyFence(device, blitFence, nullptr);
      s_vk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
      s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);
      s_fsrInFlight = false;
      return false;
    }

    // Barrier: inter GENERAL -> TRANSFER_SRC_OPTIMAL
    // The timeline semaphore guarantees the compute shader has finished
    // writing. Use ALL_COMMANDS as srcStage to be safe.
    VkImageMemoryBarrier interToBlit = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    interToBlit.srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
    interToBlit.dstAccessMask    = VK_ACCESS_TRANSFER_READ_BIT;
    interToBlit.oldLayout        = VK_IMAGE_LAYOUT_GENERAL;
    interToBlit.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    interToBlit.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    interToBlit.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    interToBlit.image            = interImage;
    interToBlit.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    s_vk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &interToBlit);

    // Barrier: dst PRESENT_SRC_KHR -> TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier dstToBlit = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    dstToBlit.srcAccessMask    = 0;
    dstToBlit.dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstToBlit.oldLayout        = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    dstToBlit.newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstToBlit.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstToBlit.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstToBlit.image            = dstImage;
    dstToBlit.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    s_vk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &dstToBlit);

    // --- Blit inter -> dst (nearest filter, 1:1 scale) ---
    VkImageBlit blitRegion = {};
    blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blitRegion.srcSubresource.layerCount = 1;
    blitRegion.srcOffsets[0] = { 0, 0, 0 };
    blitRegion.srcOffsets[1] = { static_cast<int32_t>(dstExtent.width),
                                 static_cast<int32_t>(dstExtent.height), 1 };
    blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blitRegion.dstSubresource.layerCount = 1;
    blitRegion.dstOffsets[0] = { 0, 0, 0 };
    blitRegion.dstOffsets[1] = { static_cast<int32_t>(dstExtent.width),
                                 static_cast<int32_t>(dstExtent.height), 1 };

    s_vk.vkCmdBlitImage(cmdBuf,
        interImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dstImage,   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blitRegion, VK_FILTER_NEAREST);

    // Barrier: inter TRANSFER_SRC_OPTIMAL -> GENERAL (for next async compute)
    VkImageMemoryBarrier interBack = {};
    interBack.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    interBack.srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    interBack.dstAccessMask       = 0;
    interBack.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    interBack.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    interBack.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    interBack.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    interBack.image               = interImage;
    interBack.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // Barrier: dst TRANSFER_DST_OPTIMAL -> PRESENT_SRC_KHR
    VkImageMemoryBarrier dstBack = {};
    dstBack.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    dstBack.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    dstBack.dstAccessMask       = 0;
    dstBack.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dstBack.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    dstBack.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstBack.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dstBack.image               = dstImage;
    dstBack.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkImageMemoryBarrier postBarriers[2] = { interBack, dstBack };
    s_vk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 2, postBarriers);

    vr = s_vk.vkEndCommandBuffer(cmdBuf);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR blit: vkEndCommandBuffer failed (", vr, ")"));
      s_vk.vkDestroyFence(device, blitFence, nullptr);
      s_vk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
      s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);
      s_fsrInFlight = false;
      return false;
    }

    // --- Submit + WAIT (fast, <0.1 ms for blit) ---
    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &cmdBuf;

    vr = s_vk.vkQueueSubmit(queue, 1, &submitInfo, blitFence);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR blit: vkQueueSubmit failed (", vr, ")"));
      s_vk.vkDestroyFence(device, blitFence, nullptr);
      s_vk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
      s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);
      s_fsrInFlight = false;
      return false;
    }

    vr = s_vk.vkWaitForFences(device, 1, &blitFence, VK_TRUE, UINT64_MAX);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FSR blit: vkWaitForFences failed (", vr, ")"));
    }

    s_vk.vkDestroyFence(device, blitFence, nullptr);
    s_vk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
    s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);

    // ================================================================
    // Bookkeeping
    // ================================================================
    s_fsrInFlight     = false;
    s_fsrResultW      = dstExtent.width;
    s_fsrResultH      = dstExtent.height;

    Logger::debug(str::format("Vegas FSR blit: completed (",
                              dstExtent.width, "x", dstExtent.height, ")"));
    return true;
  }


  void Vegas::fsrDrain() {
    if (!s_fsrInFlight) {
      return;
    }

    if (!s_fsrFence) {
      s_fsrInFlight = false;
      return;
    }

    VkDevice device = reinterpret_cast<VkDevice>(s_device);
    if (device == VK_NULL_HANDLE) {
      return;
    }

    // Block until the async compute finishes
    DxvkFence* fence = reinterpret_cast<DxvkFence*>(s_fsrFence);
    uint64_t targetValue = s_fsrNextValue - 1;
    fence->wait(targetValue);

    // Destroy the src view from the last async submit
    if (s_fsrLastSrcView != 0) {
      s_vk.vkDestroyImageView(device,
          reinterpret_cast<VkImageView>(s_fsrLastSrcView), nullptr);
      s_fsrLastSrcView = 0;
    }

    s_fsrInFlight = false;
    Logger::debug("Vegas FSR: drained async compute");
  }


  // ================================================================
  // ====  Frame Generation (3-pass motion-compensated)  =============
  // ================================================================
  //
  // Binding convention (set=0, shared by all 3 shaders):
  //   0: texture2D uCurrent   (sampled image — current frame)
  //   1: texture2D uPrevious  (sampled image — previous frame)
  //   2: storage image         (motion write / median read)
  //   3: storage image         (median write / warp output)
  //
  // Pass 1 (FG_MOTION):  current, previous  → raw motion (R32G32_SFLOAT)
  // Pass 2 (FG_MEDIAN):  raw motion         → filtered motion
  // Pass 3 (FG_WARP):    current, previous, filtered motion → interpolated frame
  //
  // Push constants: float4(motionScale, blendMin, blendMax, blendStrength)
  // - motionScale : 1.0 / 16.0  (tile size normalization)
  // - blendMin    : minimum blend weight for edge stability
  // - blendMax    : maximum blend weight for motion-adaptive blending
  // - blendStrength: overall interpolation intensity
  //
  // ================================================================

  enum : uint32_t {
    FG_PASS_MOTION  = 0,
    FG_PASS_MEDIAN  = 1,
    FG_PASS_WARP    = 2,

    FG_BIND_CURRENT     = 0,  // sampled
    FG_BIND_PREVIOUS    = 1,  // sampled
    FG_BIND_MOTION      = 2,  // storage (raw / filtered input)
    FG_BIND_OUTPUT      = 3,  // storage (median output / warp output)

    FG_DESC_POOL_SIZE  = 3,   // one descriptor set per pass
    FG_TILE_SIZE       = 16,  // motion search tile
    FG_MEDIAN_TILE     = 8,   // median filter tile
    FG_WARP_TILE       = 8,   // warp tile
  };

  /** Helper: init framegen 3-pass pipeline. Call once. */
  static bool initFgPipeline(VkDevice device) {
    if (Vegas::s_fgInitialized)
      return reinterpret_cast<VkPipeline>(Vegas::s_fgPipeline[0]) != VK_NULL_HANDLE;

    VkResult vr;

    // ---- Shader modules ----
    const struct { const uint32_t* code; size_t size; } shaders[3] = {
      { dxvk_fg_motion_code,  sizeof(dxvk_fg_motion_code)  },
      { dxvk_fg_median_code,  sizeof(dxvk_fg_median_code)  },
      { dxvk_fg_warp_code,    sizeof(dxvk_fg_warp_code)    },
    };

    VkShaderModule modules[3] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    for (uint32_t i = 0; i < 3; i++) {
      VkShaderModuleCreateInfo smCI = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
      smCI.codeSize = shaders[i].size;
      smCI.pCode    = shaders[i].code;
      vr = s_vk.vkCreateShaderModule(device, &smCI, nullptr, &modules[i]);
      if (vr != VK_SUCCESS) {
        Logger::warn(str::format("Vegas FG: vkCreateShaderModule(pass ", i, ") failed (", vr, ")"));
        for (uint32_t j = 0; j < i; j++)
          s_vk.vkDestroyShaderModule(device, modules[j], nullptr);
        Vegas::s_fgInitialized = true;
        return false;
      }
    }

    // ---- Descriptor set layout (4 bindings, shared) ----
    VkDescriptorSetLayoutBinding bindings[4] = {};
    // Binding 0: uCurrent (sampled)
    bindings[0].binding            = FG_BIND_CURRENT;
    bindings[0].descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount    = 1;
    bindings[0].stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT;
    // Binding 1: uPrevious (sampled)
    bindings[1].binding            = FG_BIND_PREVIOUS;
    bindings[1].descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[1].descriptorCount    = 1;
    bindings[1].stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT;
    // Binding 2: motion/median (storage)
    bindings[2].binding            = FG_BIND_MOTION;
    bindings[2].descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[2].descriptorCount    = 1;
    bindings[2].stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT;
    // Binding 3: median output / warp output (storage)
    bindings[3].binding            = FG_BIND_OUTPUT;
    bindings[3].descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[3].descriptorCount    = 1;
    bindings[3].stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dslCI = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dslCI.bindingCount = 4;
    dslCI.pBindings    = bindings;

    VkDescriptorSetLayout dsLayout = VK_NULL_HANDLE;
    vr = s_vk.vkCreateDescriptorSetLayout(device, &dslCI, nullptr, &dsLayout);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkCreateDescriptorSetLayout failed (", vr, ")"));
      for (uint32_t i = 0; i < 3; i++)
        s_vk.vkDestroyShaderModule(device, modules[i], nullptr);
      Vegas::s_fgInitialized = true;
      return false;
    }

    // ---- Pipeline layout (push constants + DS) ----
    VkPushConstantRange pcRange = {};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.offset     = 0;
    pcRange.size       = sizeof(float) * 4;  // vec4

    VkPipelineLayoutCreateInfo plCI = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plCI.setLayoutCount         = 1;
    plCI.pSetLayouts            = &dsLayout;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges    = &pcRange;

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    vr = s_vk.vkCreatePipelineLayout(device, &plCI, nullptr, &pipelineLayout);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkCreatePipelineLayout failed (", vr, ")"));
      s_vk.vkDestroyDescriptorSetLayout(device, dsLayout, nullptr);
      for (uint32_t i = 0; i < 3; i++)
        s_vk.vkDestroyShaderModule(device, modules[i], nullptr);
      Vegas::s_fgInitialized = true;
      return false;
    }

    // ---- Compute pipelines ----
    VkComputePipelineCreateInfo cpCI[3] = {};
    for (uint32_t i = 0; i < 3; i++) {
      cpCI[i].sType              = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
      cpCI[i].stage.sType        = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      cpCI[i].stage.stage        = VK_SHADER_STAGE_COMPUTE_BIT;
      cpCI[i].stage.module       = modules[i];
      cpCI[i].stage.pName        = "main";
      cpCI[i].layout             = pipelineLayout;
    }

    VkPipeline pipelines[3] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    vr = s_vk.vkCreateComputePipelines(device, VK_NULL_HANDLE, 3, cpCI, nullptr, pipelines);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkCreateComputePipelines failed (", vr, ")"));
      s_vk.vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
      s_vk.vkDestroyDescriptorSetLayout(device, dsLayout, nullptr);
      for (uint32_t i = 0; i < 3; i++)
        s_vk.vkDestroyShaderModule(device, modules[i], nullptr);
      Vegas::s_fgInitialized = true;
      return false;
    }

    // ---- Destroy shader modules (no longer needed) ----
    for (uint32_t i = 0; i < 3; i++)
      s_vk.vkDestroyShaderModule(device, modules[i], nullptr);

    // ---- Descriptor pool ----
    VkDescriptorPoolSize poolSizes[2] = {};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    poolSizes[0].descriptorCount = FG_DESC_POOL_SIZE * 2;  // each pass uses up to 2 sampled
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = FG_DESC_POOL_SIZE * 2;  // each pass uses up to 2 storage

    VkDescriptorPoolCreateInfo dpCI = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpCI.maxSets       = FG_DESC_POOL_SIZE;
    dpCI.poolSizeCount = 2;
    dpCI.pPoolSizes    = poolSizes;

    VkDescriptorPool descPool = VK_NULL_HANDLE;
    vr = s_vk.vkCreateDescriptorPool(device, &dpCI, nullptr, &descPool);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkCreateDescriptorPool failed (", vr, ")"));
      s_vk.vkDestroyPipeline(device, pipelines[0], nullptr);
      s_vk.vkDestroyPipeline(device, pipelines[1], nullptr);
      s_vk.vkDestroyPipeline(device, pipelines[2], nullptr);
      s_vk.vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
      s_vk.vkDestroyDescriptorSetLayout(device, dsLayout, nullptr);
      Vegas::s_fgInitialized = true;
      return false;
    }

    // ---- Store as boxed uint64_t ----
    Vegas::s_fgPipeline[0]     = reinterpret_cast<uint64_t>(pipelines[0]);
    Vegas::s_fgPipeline[1]     = reinterpret_cast<uint64_t>(pipelines[1]);
    Vegas::s_fgPipeline[2]     = reinterpret_cast<uint64_t>(pipelines[2]);
    Vegas::s_fgPipelineLayout  = reinterpret_cast<uint64_t>(pipelineLayout);
    Vegas::s_fgDescSetLayout   = reinterpret_cast<uint64_t>(dsLayout);
    Vegas::s_fgDescPool        = reinterpret_cast<uint64_t>(descPool);

    Vegas::s_fgInitialized = true;

    Logger::debug("Vegas FG: 3-pass pipeline initialized");
    return true;
  }

  /** Helper: ensure framegen intermediate images exist at given resolution.
   *  Creates s_fgPrevImage, s_fgMotionImage, s_fgMotionFiltered, s_fgOutputImage
   *  if dimensions changed or images do not exist.
   */
  static bool ensureFgIntermediateImages(VkDevice device, uint32_t w, uint32_t h) {
    VkResult vr;

    // Motion buffer dimensions: one vector per 16×16 tile
    uint32_t mw = (w + FG_TILE_SIZE - 1) / FG_TILE_SIZE;
    uint32_t mh = (h + FG_TILE_SIZE - 1) / FG_TILE_SIZE;

    // If dimensions match and images exist, nothing to do
    if (Vegas::s_fgPrevImage && Vegas::s_fgMotionImage && Vegas::s_fgMotionFiltered && Vegas::s_fgOutputImage
        && Vegas::s_fgPrevW == w && Vegas::s_fgPrevH == h && Vegas::s_fgMotionW == mw && Vegas::s_fgMotionH == mh)
      return true;

    // ---- Destroy old images if any ----
    auto destroyImage = [&](uint64_t& img, uint64_t& mem) {
      if (img) {
        s_vk.vkDestroyImage(device, reinterpret_cast<VkImage>(img), nullptr);
        img = 0;
      }
      if (mem) {
        s_vk.vkFreeMemory(device, reinterpret_cast<VkDeviceMemory>(mem), nullptr);
        mem = 0;
      }
    };

    destroyImage(Vegas::s_fgPrevImage,       Vegas::s_fgPrevMemory);
    destroyImage(Vegas::s_fgMotionImage,     Vegas::s_fgMotionMemory);
    destroyImage(Vegas::s_fgMotionFiltered,  Vegas::s_fgMotionFMemory);
    destroyImage(Vegas::s_fgOutputImage,     Vegas::s_fgOutputMemory);

    Vegas::s_fgPrevValid = false;
    Vegas::s_fgPrevW = 0;
    Vegas::s_fgPrevH = 0;
    Vegas::s_fgMotionW = 0;
    Vegas::s_fgMotionH = 0;

    // Helper to create a storage/transfer image
    auto createImage = [&](uint32_t imgW, uint32_t imgH,
                           VkFormat fmt, VkImageUsageFlags usage,
                           uint64_t& outImg, uint64_t& outMem) -> bool {
      VkImageCreateInfo imgCI = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
      imgCI.imageType     = VK_IMAGE_TYPE_2D;
      imgCI.extent.width  = imgW;
      imgCI.extent.height = imgH;
      imgCI.extent.depth  = 1;
      imgCI.mipLevels     = 1;
      imgCI.arrayLayers   = 1;
      imgCI.format        = fmt;
      imgCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
      imgCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      imgCI.usage         = usage;
      imgCI.samples       = VK_SAMPLE_COUNT_1_BIT;

      VkImage img = VK_NULL_HANDLE;
      vr = s_vk.vkCreateImage(device, &imgCI, nullptr, &img);
      if (vr != VK_SUCCESS) {
        Logger::warn(str::format("Vegas FG: vkCreateImage (", imgW, "x", imgH, ") failed (", vr, ")"));
        return false;
      }

      VkMemoryRequirements memReq;
      s_vk.vkGetImageMemoryRequirements(device, img, &memReq);

      VkPhysicalDeviceMemoryProperties memProps;
      s_vk.vkGetPhysicalDeviceMemoryProperties(
          reinterpret_cast<VkPhysicalDevice>(Vegas::s_physicalDevice), &memProps);

      uint32_t memType = VK_MAX_MEMORY_TYPES;
      for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((memReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
          memType = i;
          break;
        }
      }
      if (memType == VK_MAX_MEMORY_TYPES) {
        Logger::warn("Vegas FG: no suitable memory type for intermediate image");
        s_vk.vkDestroyImage(device, img, nullptr);
        return false;
      }

      VkMemoryAllocateInfo allocAI = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
      allocAI.allocationSize  = memReq.size;
      allocAI.memoryTypeIndex = memType;

      VkDeviceMemory mem = VK_NULL_HANDLE;
      vr = s_vk.vkAllocateMemory(device, &allocAI, nullptr, &mem);
      if (vr != VK_SUCCESS) {
        Logger::warn(str::format("Vegas FG: vkAllocateMemory failed (", vr, ")"));
        s_vk.vkDestroyImage(device, img, nullptr);
        return false;
      }

      vr = s_vk.vkBindImageMemory(device, img, mem, 0);
      if (vr != VK_SUCCESS) {
        Logger::warn(str::format("Vegas FG: vkBindImageMemory failed (", vr, ")"));
        s_vk.vkFreeMemory(device, mem, nullptr);
        s_vk.vkDestroyImage(device, img, nullptr);
        return false;
      }

      outImg = reinterpret_cast<uint64_t>(img);
      outMem = reinterpret_cast<uint64_t>(mem);
      return true;
    };

    // ---- Create images ----
    // s_fgPrevImage: previous frame (UNORM, same as swapchain)
    if (!createImage(w, h, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        Vegas::s_fgPrevImage, Vegas::s_fgPrevMemory)) {
      return false;
    }

    // s_fgMotionImage: raw motion vectors (R32G32_SFLOAT, storage)
    if (!createImage(mw, mh, VK_FORMAT_R32G32_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        Vegas::s_fgMotionImage, Vegas::s_fgMotionMemory)) {
      return false;
    }

    // s_fgMotionFiltered: median-filtered motion (R32G32_SFLOAT, storage)
    if (!createImage(mw, mh, VK_FORMAT_R32G32_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        Vegas::s_fgMotionFiltered, Vegas::s_fgMotionFMemory)) {
      return false;
    }

    // s_fgOutputImage: interpolated frame output (UNORM)
    if (!createImage(w, h, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        Vegas::s_fgOutputImage, Vegas::s_fgOutputMemory)) {
      return false;
    }

    Vegas::s_fgPrevW   = w;
    Vegas::s_fgPrevH   = h;
    Vegas::s_fgMotionW = mw;
    Vegas::s_fgMotionH = mh;

    Logger::debug(str::format("Vegas FG: intermediate images created (",
                              w, "x", h, ", motion ", mw, "x", mh, ")"));
    return true;
  }

  /** Public getter for framegen output image (uint64_t → VkImage). */
  uint64_t Vegas::framegenOutputImage() {
    return s_fgOutputImage;
  }

  /** Returns true if the Frame Generator has a valid VkDevice/VkQueue. */
  bool Vegas::isFrameGenReady() {
    return s_device != nullptr;
  }

  /** Dispatch 3-pass motion-compensated framegen.
   *
   *  \param [in] curImage  Current rendered frame (VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
   *  \param [in] prevImage Previous frame (caller-provided; may be VK_NULL_HANDLE)
   *  \param [in] extent    Image dimensions
   *  \param [in] format    Image format (must be R8G8B8A8_UNORM)
   *  \returns true if dispatch succeeded and output is in s_fgOutputImage
   *
   *  On the first call (no previous frame), saves curImage internally and
   *  returns false.  Subsequent calls produce the interpolated frame.
   */
  bool Vegas::framegenDispatch(
          VkImage              curImage,
          VkImage              prevImage,
          VkExtent3D           extent,
          VkFormat             format) {
    // ================================================================
    // Guard: only UNORM supported
    // ================================================================
    if (format != VK_FORMAT_R8G8B8A8_UNORM &&
        format != VK_FORMAT_B8G8R8A8_UNORM) {
      Logger::debug("Vegas FG: skipped — unsupported format");
      return false;
    }

    // ================================================================
    // Get device & queue
    // ================================================================
    VkDevice device = reinterpret_cast<VkDevice>(s_device);
    VkQueue  queue  = reinterpret_cast<VkQueue>(s_vkQueue);
    if (device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE) {
      Logger::debug("Vegas FG: skipped — no VkDevice/VkQueue");
      return false;
    }

    if (!loadVulkanFuncs(device)) {
      Logger::debug("Vegas FG: skipped — Vulkan functions not available");
      return false;
    }

    if (!initFgPipeline(device)) {
      Logger::debug("Vegas FG: skipped — pipeline init failed");
      return false;
    }

    if (!ensureFgIntermediateImages(device, extent.width, extent.height)) {
      Logger::debug("Vegas FG: skipped — intermediate image creation failed");
      return false;
    }

    VkPipelineLayout    pipelineLayout  = reinterpret_cast<VkPipelineLayout>(s_fgPipelineLayout);
    VkDescriptorSetLayout descSetLayout = reinterpret_cast<VkDescriptorSetLayout>(s_fgDescSetLayout);
    VkDescriptorPool    descPool        = reinterpret_cast<VkDescriptorPool>(s_fgDescPool);

    // ================================================================
    // First frame? Just save current as previous, return false
    // ================================================================
    if (prevImage == VK_NULL_HANDLE && !s_fgPrevValid) {
      // Copy curImage → s_fgPrevImage for next frame
      // Use a simple command buffer for the copy
      VkCommandPoolCreateInfo poolCI = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
      poolCI.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
      poolCI.queueFamilyIndex = s_queueFamily;
      VkCommandPool cmdPool = VK_NULL_HANDLE;
      VkResult vr = s_vk.vkCreateCommandPool(device, &poolCI, nullptr, &cmdPool);
      if (vr != VK_SUCCESS) return false;

      VkCommandBufferAllocateInfo allocCI = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
      allocCI.commandPool        = cmdPool;
      allocCI.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      allocCI.commandBufferCount = 1;
      VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
      vr = s_vk.vkAllocateCommandBuffers(device, &allocCI, &cmdBuf);
      if (vr != VK_SUCCESS) {
        s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);
        return false;
      }

      VkFenceCreateInfo fenceCI = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
      VkFence fence = VK_NULL_HANDLE;
      vr = s_vk.vkCreateFence(device, &fenceCI, nullptr, &fence);
      if (vr != VK_SUCCESS) {
        s_vk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
        s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);
        return false;
      }

      VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
      beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      s_vk.vkBeginCommandBuffer(cmdBuf, &beginInfo);

      // Transition curImage PRESENT_SRC_KHR → TRANSFER_SRC_OPTIMAL
      VkImageMemoryBarrier curToSrc = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
      curToSrc.srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      curToSrc.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
      curToSrc.oldLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      curToSrc.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      curToSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      curToSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      curToSrc.image               = curImage;
      curToSrc.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

      s_vk.vkCmdPipelineBarrier(cmdBuf,
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT,
          0, 0, nullptr, 0, nullptr, 1, &curToSrc);

      // Transition s_fgPrevImage UNDEFINED → TRANSFER_DST_OPTIMAL
      VkImage prevDst = reinterpret_cast<VkImage>(s_fgPrevImage);
      VkImageMemoryBarrier prevToDst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
      prevToDst.srcAccessMask        = 0;
      prevToDst.dstAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT;
      prevToDst.oldLayout            = VK_IMAGE_LAYOUT_UNDEFINED;
      prevToDst.newLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      prevToDst.srcQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
      prevToDst.dstQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
      prevToDst.image                = prevDst;
      prevToDst.subresourceRange     = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

      s_vk.vkCmdPipelineBarrier(cmdBuf,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT,
          0, 0, nullptr, 0, nullptr, 1, &prevToDst);

      // Copy curImage → prevImage
      VkImageCopy copyRegion = {};
      copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      copyRegion.srcSubresource.layerCount = 1;
      copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      copyRegion.dstSubresource.layerCount = 1;
      copyRegion.extent = extent;

      s_vk.vkCmdCopyImage(cmdBuf,
          curImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          prevDst,  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          1, &copyRegion);

      // Restore curImage TRANSFER_SRC_OPTIMAL → PRESENT_SRC_KHR
      VkImageMemoryBarrier curBack = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
      curBack.srcAccessMask        = VK_ACCESS_TRANSFER_READ_BIT;
      curBack.dstAccessMask        = 0;
      curBack.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      curBack.newLayout            = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      curBack.srcQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
      curBack.dstQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
      curBack.image                = curImage;
      curBack.subresourceRange     = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

      // Leave prev in GENERAL for shader read next frame
      VkImageMemoryBarrier prevToGen = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
      prevToGen.srcAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT;
      prevToGen.dstAccessMask        = VK_ACCESS_SHADER_READ_BIT;
      prevToGen.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      prevToGen.newLayout            = VK_IMAGE_LAYOUT_GENERAL;
      prevToGen.srcQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
      prevToGen.dstQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
      prevToGen.image                = prevDst;
      prevToGen.subresourceRange     = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

      VkImageMemoryBarrier postBarriers[2] = { curBack, prevToGen };
      s_vk.vkCmdPipelineBarrier(cmdBuf,
          VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          0, 0, nullptr, 0, nullptr, 2, postBarriers);

      s_vk.vkEndCommandBuffer(cmdBuf);

      VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
      submitInfo.commandBufferCount = 1;
      submitInfo.pCommandBuffers    = &cmdBuf;
      s_vk.vkQueueSubmit(queue, 1, &submitInfo, fence);
      s_vk.vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);

      s_vk.vkDestroyFence(device, fence, nullptr);
      s_vk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
      s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);

      s_fgPrevValid = true;
      Logger::debug("Vegas FG: first frame captured as previous");
      return false;
    }

    // ================================================================
    // Use s_fgPrevImage as the actual previous frame if prevImage is null
    // ================================================================
    VkImage actualPrev = (prevImage != VK_NULL_HANDLE)
                         ? prevImage
                         : reinterpret_cast<VkImage>(s_fgPrevImage);
    VkImage actualCur  = curImage;

    // Unwrap intermediate images
    VkImage motionRaw      = reinterpret_cast<VkImage>(s_fgMotionImage);
    VkImage motionFiltered = reinterpret_cast<VkImage>(s_fgMotionFiltered);
    VkImage fgOutput       = reinterpret_cast<VkImage>(s_fgOutputImage);
    VkImage prevDst        = reinterpret_cast<VkImage>(s_fgPrevImage);

    // All 3 pipelines
    VkPipeline pipelineMotion  = reinterpret_cast<VkPipeline>(s_fgPipeline[FG_PASS_MOTION]);
    VkPipeline pipelineMedian  = reinterpret_cast<VkPipeline>(s_fgPipeline[FG_PASS_MEDIAN]);
    VkPipeline pipelineWarp    = reinterpret_cast<VkPipeline>(s_fgPipeline[FG_PASS_WARP]);

    VkImageView srcViewCur   = VK_NULL_HANDLE;
    VkImageView srcViewPrev  = VK_NULL_HANDLE;
    VkImageView motionView   = VK_NULL_HANDLE;
    VkImageView motionFilteredView = VK_NULL_HANDLE;
    VkImageView outputView   = VK_NULL_HANDLE;

    // ================================================================
    // Create temporary command pool + buffer + fence
    // ================================================================
    VkCommandPoolCreateInfo poolCI = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolCI.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolCI.queueFamilyIndex = s_queueFamily;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkResult vr = s_vk.vkCreateCommandPool(device, &poolCI, nullptr, &cmdPool);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkCreateCommandPool failed (", vr, ")"));
      return false;
    }

    VkCommandBufferAllocateInfo allocCI = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocCI.commandPool        = cmdPool;
    allocCI.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocCI.commandBufferCount = 1;
    VkCommandBuffer cmdBuf = VK_NULL_HANDLE;
    vr = s_vk.vkAllocateCommandBuffers(device, &allocCI, &cmdBuf);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkAllocateCommandBuffers failed (", vr, ")"));
      s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);
      return false;
    }

    VkFenceCreateInfo fenceCI = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    vr = s_vk.vkCreateFence(device, &fenceCI, nullptr, &fence);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkCreateFence failed (", vr, ")"));
      s_vk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
      s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);
      return false;
    }

    // Cleanup helper — call with stage number indicating what was allocated.
    // Stage: 0=none, 1=cmdPool, 2=cmdBuf, 3=fence,
    //        4=srcViewCur, 5=srcViewPrev, 6=motionView,
    //        7=motionFilteredView, 8=outputView
    auto fgCleanup = [&](int stage) {
      if (stage >= 8)
        s_vk.vkDestroyImageView(device, outputView, nullptr);
      if (stage >= 7)
        s_vk.vkDestroyImageView(device, motionFilteredView, nullptr);
      if (stage >= 6)
        s_vk.vkDestroyImageView(device, motionView, nullptr);
      if (stage >= 5)
        s_vk.vkDestroyImageView(device, srcViewPrev, nullptr);
      if (stage >= 4)
        s_vk.vkDestroyImageView(device, srcViewCur, nullptr);
      if (stage >= 3)
        s_vk.vkDestroyFence(device, fence, nullptr);
      if (stage >= 2)
        s_vk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
      if (stage >= 1)
        s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);
    };

    // ================================================================
    // Create image views
    // ================================================================
    VkImageViewCreateInfo viewCI = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewCI.viewType     = VK_IMAGE_VIEW_TYPE_2D;
    viewCI.format       = format;
    viewCI.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewCI.subresourceRange.baseMipLevel   = 0;
    viewCI.subresourceRange.levelCount     = 1;
    viewCI.subresourceRange.baseArrayLayer = 0;
    viewCI.subresourceRange.layerCount     = 1;

    // curImage view
    viewCI.image = actualCur;
    vr = s_vk.vkCreateImageView(device, &viewCI, nullptr, &srcViewCur);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkCreateImageView(cur) failed (", vr, ")"));
      fgCleanup(3); return false;
    }

    // prevImage view (same format)
    viewCI.image = actualPrev;
    viewCI.format = VK_FORMAT_R8G8B8A8_UNORM;  // prev is always UNORM
    vr = s_vk.vkCreateImageView(device, &viewCI, nullptr, &srcViewPrev);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkCreateImageView(prev) failed (", vr, ")"));
      fgCleanup(4); return false;
    }

    // Motion raw view (R32G32_SFLOAT)
    viewCI.image  = motionRaw;
    viewCI.format = VK_FORMAT_R32G32_SFLOAT;
    vr = s_vk.vkCreateImageView(device, &viewCI, nullptr, &motionView);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkCreateImageView(motion) failed (", vr, ")"));
      fgCleanup(5); return false;
    }

    // Motion filtered view (R32G32_SFLOAT)
    viewCI.image  = motionFiltered;
    viewCI.format = VK_FORMAT_R32G32_SFLOAT;
    vr = s_vk.vkCreateImageView(device, &viewCI, nullptr, &motionFilteredView);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkCreateImageView(motionFiltered) failed (", vr, ")"));
      fgCleanup(6); return false;
    }

    // Output view (R8G8B8A8_UNORM)
    viewCI.image  = fgOutput;
    viewCI.format = VK_FORMAT_R8G8B8A8_UNORM;
    vr = s_vk.vkCreateImageView(device, &viewCI, nullptr, &outputView);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkCreateImageView(output) failed (", vr, ")"));
      fgCleanup(7); return false;
    }

    // ================================================================
    // Record command buffer — 3-pass dispatch
    // ================================================================
    VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vr = s_vk.vkBeginCommandBuffer(cmdBuf, &beginInfo);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkBeginCommandBuffer failed (", vr, ")"));
      fgCleanup(8); return false;
    }

    // ----------------------------------------------------------------
    // Pre-dispatch barriers: bring all images to GENERAL layout
    // ----------------------------------------------------------------
    VkImageMemoryBarrier preBarriers[5] = {};

    // curImage: PRESENT_SRC_KHR → GENERAL (shader read)
    preBarriers[0].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    preBarriers[0].srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    preBarriers[0].dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    preBarriers[0].oldLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    preBarriers[0].newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    preBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preBarriers[0].image               = actualCur;
    preBarriers[0].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // actualPrev: GENERAL remains GENERAL (assumed already in GENERAL from previous frame)
    // Just ensure shader-read access is visible
    preBarriers[1].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    preBarriers[1].srcAccessMask       = 0;
    preBarriers[1].dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    preBarriers[1].oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    preBarriers[1].newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    preBarriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preBarriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preBarriers[1].image               = actualPrev;
    preBarriers[1].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // motionRaw: UNDEFINED → GENERAL (storage write)
    preBarriers[2].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    preBarriers[2].srcAccessMask       = 0;
    preBarriers[2].dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    preBarriers[2].oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    preBarriers[2].newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    preBarriers[2].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preBarriers[2].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preBarriers[2].image               = motionRaw;
    preBarriers[2].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // motionFiltered: UNDEFINED → GENERAL (storage write)
    preBarriers[3].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    preBarriers[3].srcAccessMask       = 0;
    preBarriers[3].dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    preBarriers[3].oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    preBarriers[3].newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    preBarriers[3].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preBarriers[3].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preBarriers[3].image               = motionFiltered;
    preBarriers[3].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // fgOutput: UNDEFINED → GENERAL (storage write)
    preBarriers[4].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    preBarriers[4].srcAccessMask       = 0;
    preBarriers[4].dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    preBarriers[4].oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    preBarriers[4].newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    preBarriers[4].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preBarriers[4].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preBarriers[4].image               = fgOutput;
    preBarriers[4].subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // Split into two calls: barrier 0 (cur) needs COLOR_ATTACHMENT_OUTPUT srcStage,
    // barriers 1-4 need TOP_OF_PIPE srcStage (no prior producer).
    // Barrier for curImage
    VkImageMemoryBarrier curBarrier = preBarriers[0];
    s_vk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &curBarrier);

    // Barriers for prev + intermediates
    s_vk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 4, &preBarriers[1]);

    // ----------------------------------------------------------------
    // Allocate 3 descriptor sets (one per pass)
    // ----------------------------------------------------------------
    s_vk.vkResetDescriptorPool(device, descPool, 0);

    VkDescriptorSetAllocateInfo descAlloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    descAlloc.descriptorPool     = descPool;
    descAlloc.descriptorSetCount = 3;

    VkDescriptorSetLayout layouts[3] = { descSetLayout, descSetLayout, descSetLayout };
    descAlloc.pSetLayouts = layouts;

    VkDescriptorSet descSets[3] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    vr = s_vk.vkAllocateDescriptorSets(device, &descAlloc, descSets);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkAllocateDescriptorSets failed (", vr, ")"));
      fgCleanup(8); return false;
    }

    // ---- Write descriptors for Pass 1 (MOTION) ----
    // cur + prev (sampled) → motionRaw (storage)
    VkDescriptorImageInfo curImgInfo  = {};
    curImgInfo.sampler     = VK_NULL_HANDLE;
    curImgInfo.imageView   = srcViewCur;
    curImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo prevImgInfo = {};
    prevImgInfo.sampler     = VK_NULL_HANDLE;
    prevImgInfo.imageView   = srcViewPrev;
    prevImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo motionImgInfo = {};
    motionImgInfo.sampler     = VK_NULL_HANDLE;
    motionImgInfo.imageView   = motionView;
    motionImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo mfiltImgInfo = {};
    mfiltImgInfo.sampler     = VK_NULL_HANDLE;
    mfiltImgInfo.imageView   = motionFilteredView;
    mfiltImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo outputImgInfo = {};
    outputImgInfo.sampler     = VK_NULL_HANDLE;
    outputImgInfo.imageView   = outputView;
    outputImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[4] = {};

    // Pass 1: bind 0=cur, 1=prev, 2=motion, 3=unused
    writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet          = descSets[0];
    writes[0].dstBinding      = FG_BIND_CURRENT;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[0].pImageInfo      = &curImgInfo;

    writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet          = descSets[0];
    writes[1].dstBinding      = FG_BIND_PREVIOUS;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[1].pImageInfo      = &prevImgInfo;

    writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet          = descSets[0];
    writes[2].dstBinding      = FG_BIND_MOTION;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo      = &motionImgInfo;

    s_vk.vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

    // Pass 2 (MEDIAN): bind 2=motion, 3=motionFiltered
    VkWriteDescriptorSet medianWrites[2] = {};
    medianWrites[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    medianWrites[0].dstSet          = descSets[1];
    medianWrites[0].dstBinding      = FG_BIND_MOTION;
    medianWrites[0].descriptorCount = 1;
    medianWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    medianWrites[0].pImageInfo      = &motionImgInfo;

    medianWrites[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    medianWrites[1].dstSet          = descSets[1];
    medianWrites[1].dstBinding      = FG_BIND_OUTPUT;
    medianWrites[1].descriptorCount = 1;
    medianWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    medianWrites[1].pImageInfo      = &mfiltImgInfo;

    s_vk.vkUpdateDescriptorSets(device, 2, medianWrites, 0, nullptr);

    // Pass 3 (WARP): bind 0=cur, 1=prev, 2=motionFiltered, 3=output
    VkWriteDescriptorSet warpWrites[4] = {};
    warpWrites[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    warpWrites[0].dstSet          = descSets[2];
    warpWrites[0].dstBinding      = FG_BIND_CURRENT;
    warpWrites[0].descriptorCount = 1;
    warpWrites[0].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    warpWrites[0].pImageInfo      = &curImgInfo;

    warpWrites[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    warpWrites[1].dstSet          = descSets[2];
    warpWrites[1].dstBinding      = FG_BIND_PREVIOUS;
    warpWrites[1].descriptorCount = 1;
    warpWrites[1].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    warpWrites[1].pImageInfo      = &prevImgInfo;

    warpWrites[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    warpWrites[2].dstSet          = descSets[2];
    warpWrites[2].dstBinding      = FG_BIND_MOTION;
    warpWrites[2].descriptorCount = 1;
    warpWrites[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    warpWrites[2].pImageInfo      = &mfiltImgInfo;

    warpWrites[3].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    warpWrites[3].dstSet          = descSets[2];
    warpWrites[3].dstBinding      = FG_BIND_OUTPUT;
    warpWrites[3].descriptorCount = 1;
    warpWrites[3].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    warpWrites[3].pImageInfo      = &outputImgInfo;

    s_vk.vkUpdateDescriptorSets(device, 4, warpWrites, 0, nullptr);

    // ----------------------------------------------------------------
    // Pass 1: Motion search
    // ----------------------------------------------------------------
    s_vk.vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineMotion);
    s_vk.vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
        pipelineLayout, 0, 1, &descSets[0], 0, nullptr);
    {
      float pcData[4] = { 1.0f / float(FG_TILE_SIZE), 0.0f, 0.0f, 0.0f };
      s_vk.vkCmdPushConstants(cmdBuf, pipelineLayout,
          VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcData), pcData);
    }

    uint32_t motionGX = (extent.width  + FG_TILE_SIZE - 1) / FG_TILE_SIZE;
    uint32_t motionGY = (extent.height + FG_TILE_SIZE - 1) / FG_TILE_SIZE;
    s_vk.vkCmdDispatch(cmdBuf, motionGX, motionGY, 1);

    // Barrier: motionRaw GENERAL (write→read for median)
    VkImageMemoryBarrier motionBarrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    motionBarrier.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    motionBarrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    motionBarrier.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    motionBarrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    motionBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    motionBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    motionBarrier.image               = motionRaw;
    motionBarrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    s_vk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &motionBarrier);

    // ----------------------------------------------------------------
    // Pass 2: Median filter
    // ----------------------------------------------------------------
    s_vk.vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineMedian);
    s_vk.vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
        pipelineLayout, 0, 1, &descSets[1], 0, nullptr);
    // Median push constants are unused but required by layout
    {
      float pcData[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
      s_vk.vkCmdPushConstants(cmdBuf, pipelineLayout,
          VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcData), pcData);
    }

    uint32_t medianGX = (motionGX + FG_MEDIAN_TILE - 1) / FG_MEDIAN_TILE;
    uint32_t medianGY = (motionGY + FG_MEDIAN_TILE - 1) / FG_MEDIAN_TILE;
    s_vk.vkCmdDispatch(cmdBuf, medianGX, medianGY, 1);

    // Barrier: motionFiltered GENERAL (write→read for warp)
    VkImageMemoryBarrier mfiltBarrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    mfiltBarrier.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    mfiltBarrier.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    mfiltBarrier.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    mfiltBarrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    mfiltBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mfiltBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mfiltBarrier.image               = motionFiltered;
    mfiltBarrier.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    s_vk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &mfiltBarrier);

    // ----------------------------------------------------------------
    // Pass 3: Warp + blend
    // ----------------------------------------------------------------
    s_vk.vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineWarp);
    s_vk.vkCmdBindDescriptorSets(cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
        pipelineLayout, 0, 1, &descSets[2], 0, nullptr);
    {
      float blendMin     = 0.05f;
      float blendMax     = 0.95f;
      float blendStrength = 0.5f;
      float motionScale   = 1.0f / float(FG_TILE_SIZE);
      float pcData[4]    = { blendMin, blendMax, blendStrength, motionScale };
      s_vk.vkCmdPushConstants(cmdBuf, pipelineLayout,
          VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcData), pcData);
    }

    uint32_t warpGX = (extent.width  + FG_WARP_TILE - 1) / FG_WARP_TILE;
    uint32_t warpGY = (extent.height + FG_WARP_TILE - 1) / FG_WARP_TILE;
    s_vk.vkCmdDispatch(cmdBuf, warpGX, warpGY, 1);

    // ----------------------------------------------------------------
    // Post-dispatch Step 1: save curImage → prevDst (for next frame)
    // ----------------------------------------------------------------
    // curImage GENERAL → TRANSFER_SRC_OPTIMAL
    VkImageMemoryBarrier curToSrc = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    curToSrc.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    curToSrc.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    curToSrc.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    curToSrc.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    curToSrc.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    curToSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    curToSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    curToSrc.image               = actualCur;
    curToSrc.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // prevDst GENERAL → TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier prevToDst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    prevToDst.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    prevToDst.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    prevToDst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    prevToDst.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    prevToDst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    prevToDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    prevToDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    prevToDst.image               = prevDst;
    prevToDst.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkImageMemoryBarrier copyPrep[2] = { curToSrc, prevToDst };
    s_vk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 2, copyPrep);

    // Copy curImage → prevDst
    VkImageCopy copyCur = {};
    copyCur.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyCur.srcSubresource.layerCount = 1;
    copyCur.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyCur.dstSubresource.layerCount = 1;
    copyCur.extent = extent;

    s_vk.vkCmdCopyImage(cmdBuf,
        actualCur, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        prevDst,   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &copyCur);

    // ----------------------------------------------------------------
    // Post-dispatch Step 2: blit fgOutput → curImage (for presentation)
    // ----------------------------------------------------------------
    // curImage TRANSFER_SRC_OPTIMAL → TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier curToDst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    curToDst.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    curToDst.srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    curToDst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    curToDst.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    curToDst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    curToDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    curToDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    curToDst.image               = actualCur;
    curToDst.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // fgOutput GENERAL → TRANSFER_SRC_OPTIMAL (after shader write completes)
    VkImageMemoryBarrier fgToSrc = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    fgToSrc.sType                = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    fgToSrc.srcAccessMask        = VK_ACCESS_SHADER_WRITE_BIT;
    fgToSrc.dstAccessMask        = VK_ACCESS_TRANSFER_READ_BIT;
    fgToSrc.oldLayout            = VK_IMAGE_LAYOUT_GENERAL;
    fgToSrc.newLayout            = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    fgToSrc.srcQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
    fgToSrc.dstQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
    fgToSrc.image                = fgOutput;
    fgToSrc.subresourceRange     = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // Also transition prevDst: TRANSFER_DST → GENERAL for next frame read
    VkImageMemoryBarrier prevToGen = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    prevToGen.sType                = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    prevToGen.srcAccessMask        = VK_ACCESS_TRANSFER_WRITE_BIT;
    prevToGen.dstAccessMask        = VK_ACCESS_SHADER_READ_BIT;
    prevToGen.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    prevToGen.newLayout            = VK_IMAGE_LAYOUT_GENERAL;
    prevToGen.srcQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
    prevToGen.dstQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
    prevToGen.image                = prevDst;
    prevToGen.subresourceRange     = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkImageMemoryBarrier blitPrep[3] = { curToDst, fgToSrc, prevToGen };
    s_vk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 3, blitPrep);

    // Blit fgOutput → curImage (nearest, 1:1)
    VkImageBlit blitFG = {};
    blitFG.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blitFG.srcSubresource.layerCount = 1;
    blitFG.srcOffsets[0] = { 0, 0, 0 };
    blitFG.srcOffsets[1] = { static_cast<int32_t>(extent.width),
                             static_cast<int32_t>(extent.height), 1 };
    blitFG.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blitFG.dstSubresource.layerCount = 1;
    blitFG.dstOffsets[0] = { 0, 0, 0 };
    blitFG.dstOffsets[1] = { static_cast<int32_t>(extent.width),
                             static_cast<int32_t>(extent.height), 1 };

    s_vk.vkCmdBlitImage(cmdBuf,
        fgOutput, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        actualCur, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blitFG, VK_FILTER_NEAREST);

    // ----------------------------------------------------------------
    // Final barrier: restore curImage to PRESENT_SRC_KHR
    // ----------------------------------------------------------------
    VkImageMemoryBarrier curFinal = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    curFinal.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    curFinal.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    curFinal.dstAccessMask       = 0;
    curFinal.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    curFinal.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    curFinal.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    curFinal.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    curFinal.image               = actualCur;
    curFinal.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    s_vk.vkCmdPipelineBarrier(cmdBuf,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, nullptr, 0, nullptr, 1, &curFinal);

    // ----------------------------------------------------------------
    // End & submit
    // ----------------------------------------------------------------
    vr = s_vk.vkEndCommandBuffer(cmdBuf);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkEndCommandBuffer failed (", vr, ")"));
      fgCleanup(8); return false;
    }

    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers    = &cmdBuf;

    vr = s_vk.vkQueueSubmit(queue, 1, &submitInfo, fence);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkQueueSubmit failed (", vr, ")"));
      fgCleanup(8); return false;
    }

    vr = s_vk.vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    if (vr != VK_SUCCESS) {
      Logger::warn(str::format("Vegas FG: vkWaitForFences failed (", vr, ")"));
    }

    // Cleanup views
    s_vk.vkDestroyImageView(device, outputView, nullptr);
    s_vk.vkDestroyImageView(device, motionFilteredView, nullptr);
    s_vk.vkDestroyImageView(device, motionView, nullptr);
    s_vk.vkDestroyImageView(device, srcViewPrev, nullptr);
    s_vk.vkDestroyImageView(device, srcViewCur, nullptr);

    s_vk.vkDestroyFence(device, fence, nullptr);
    s_vk.vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
    s_vk.vkDestroyCommandPool(device, cmdPool, nullptr);

    Logger::debug(str::format("Vegas FG: dispatch complete (", extent.width, "x", extent.height, ")"));
    return true;

  }

  // ============================================================
  // VegasHud metrics
  // ============================================================
  void Vegas::pushMetrics(
          float                gpuLoad,
          float                frameTime,
          VegasPerformanceState state,
          bool                 fsrActive,
          bool                 fgActive) {
    // Always write to static vars for backward compat
    s_lastGpuLoad     = gpuLoad;
    s_lastFrameTime   = frameTime;
    s_lastPerfState   = state;
    s_fsrActive       = fsrActive;
    s_fgActive        = fgActive;

    s_ftHistory[s_ftHead] = frameTime;
    s_ftHead = (s_ftHead + 1) % FT_HISTORY_SIZE;

    // Also write to shared DxvkDevice metrics (cross-DLL safe).
    // The device object is the same pointer in both d3d11.dll and
    // dxgi.dll, so metrics written by PresentBase (dxgi.dll) are
    // visible to VegasHud (d3d11.dll).
    auto dev = s_dxvkDevice;
    if (dev != nullptr && dev->m_vegasMetrics.initialized) {
      dev->m_vegasMetrics.gpuLoad    = gpuLoad;
      dev->m_vegasMetrics.frameTime  = frameTime;
      dev->m_vegasMetrics.perfState  = static_cast<uint32_t>(state);
      dev->m_vegasMetrics.fsrActive  = fsrActive;
      dev->m_vegasMetrics.fgActive   = fgActive;
      dev->m_vegasMetrics.ftHistory[dev->m_vegasMetrics.ftHead] = frameTime;
      dev->m_vegasMetrics.ftHead = (dev->m_vegasMetrics.ftHead + 1) % FT_HISTORY_SIZE;
    }
  }

  float Vegas::getHistoryFt(uint32_t idx) {
    if (idx >= FT_HISTORY_SIZE)
      return 0.0f;

    // Prefer shared device metrics (cross-DLL safe)
    auto dev = s_dxvkDevice;
    if (dev != nullptr && dev->m_vegasMetrics.initialized) {
      uint32_t pos = (dev->m_vegasMetrics.ftHead + FT_HISTORY_SIZE - 1 - idx) % FT_HISTORY_SIZE;
      return dev->m_vegasMetrics.ftHistory[pos];
    }

    // Fallback to per-DLL static
    uint32_t pos = (s_ftHead + FT_HISTORY_SIZE - 1 - idx) % FT_HISTORY_SIZE;
    return s_ftHistory[pos];
  }

  uint32_t Vegas::getHistoryFtCount() {
    return FT_HISTORY_SIZE;
  }

  float Vegas::getLastGpuLoad() {
    auto dev = s_dxvkDevice;
    if (dev != nullptr && dev->m_vegasMetrics.initialized)
      return dev->m_vegasMetrics.gpuLoad;
    return s_lastGpuLoad;
  }

  VegasPerformanceState Vegas::getLastPerfState() {
    auto dev = s_dxvkDevice;
    if (dev != nullptr && dev->m_vegasMetrics.initialized)
      return static_cast<VegasPerformanceState>(dev->m_vegasMetrics.perfState);
    return s_lastPerfState;
  }

  float Vegas::getLastFrameTime() {
    auto dev = s_dxvkDevice;
    if (dev != nullptr && dev->m_vegasMetrics.initialized)
      return dev->m_vegasMetrics.frameTime;
    return s_lastFrameTime;
  }

  bool Vegas::isFsrActive() {
    auto dev = s_dxvkDevice;
    if (dev != nullptr && dev->m_vegasMetrics.initialized)
      return dev->m_vegasMetrics.fsrActive;
    return s_fsrActive;
  }

  bool Vegas::isFgActive() {
    auto dev = s_dxvkDevice;
    if (dev != nullptr && dev->m_vegasMetrics.initialized)
      return dev->m_vegasMetrics.fgActive;
    return s_fgActive;
  }

} // namespace dxvk
