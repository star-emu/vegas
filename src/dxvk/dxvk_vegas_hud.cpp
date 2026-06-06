#include "dxvk_vegas_hud.h"

#include "../util/config/config.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace dxvk {

  VegasHud::VegasHud(
    const Rc<DxvkDevice>& device)
  : m_renderer(device) {
    // Check config: vegas.enableHud = Auto/True -> enabled if Vegas active
    Config config;
    Tristate opt = config.getOption<Tristate>("vegas.enableHud", Tristate::Auto);
    m_enabled = (opt != Tristate::False) && Vegas::isEnabled();
    m_options.scale = 1.0f;
    m_options.opacity = 1.0f;
  }


  VegasHud::~VegasHud() {

  }


  VkExtent3D VegasHud::getExtent(
    const Rc<DxvkImageView>& dstView) const {
    return dstView->mipLevelExtent(0u);
  }


  void VegasHud::render(
    const Rc<DxvkCommandList>& ctx,
    const Rc<DxvkImageView>&   dstView) {
    if (!m_enabled)
      return;

    VkExtent3D extent = getExtent(dstView);
    int32_t w = static_cast<int32_t>(extent.width);
    int32_t h = static_cast<int32_t>(extent.height);

    // Bail early if surface is too small for overlay
    if (w < 200 || h < 60)
      return;

    // Begin HUD frame (sets viewport/scissor, uploads font if needed)
    m_renderer.beginFrame(ctx, dstView, m_options);

    constexpr int32_t fontSz  = 14;
    constexpr int32_t lineH   = 17;  // line height in pixels
    constexpr int32_t margin  = 8;   // right margin from edge
    int32_t x = w - margin;          // right-aligned column
    int32_t y = 8;                    // start from top

    // ----------------------------------------------------------------
    // Line 0: Header "VEGAS v1.0"
    // ----------------------------------------------------------------
    {
      std::string header = "VEGAS v1.0";
      x -= static_cast<int32_t>(header.size()) * 8; // approx char width
      m_renderer.drawText(fontSz, { x, y }, COLOR_HEADER, header);
    }

    // ----------------------------------------------------------------
    // Line 1: "T3  72%  16.7ms"
    // ----------------------------------------------------------------
    y += lineH;
    {
      uint32_t tier    = Vegas::getTier();
      float    load    = Vegas::getLastGpuLoad();
      float    ft      = Vegas::getLastFrameTime();

      std::string tierStr = "T" + std::to_string(tier);
      std::string loadStr = std::to_string(static_cast<int>(load * 100.0f)) + "%";

      char ftBuf[16];
      std::snprintf(ftBuf, sizeof(ftBuf), "%.1fms", static_cast<double>(ft));

      std::string line = tierStr + "  " + loadStr + "  " + ftBuf;
      x = w - margin - static_cast<int32_t>(line.size()) * 8;

      // Pick color based on performance state
      uint32_t color = COLOR_WHITE;
      switch (Vegas::getLastPerfState()) {
        case VegasPerformanceState::Lagging:    color = COLOR_WARN; break;
        case VegasPerformanceState::Stuttering: color = COLOR_WARN; break;
        case VegasPerformanceState::Overheating:color = COLOR_CRIT; break;
        default: break; /* keep white for Normal */
      }

      m_renderer.drawText(fontSz, { x, y }, color, line);
    }

    // ----------------------------------------------------------------
    // Line 2: Performance state + feature flags
    // ----------------------------------------------------------------
    y += lineH;
    {
      const char* stateStr  = Vegas::getStatusString(Vegas::getLastPerfState());
      bool        fsr       = Vegas::isFsrActive();
      bool        fg        = Vegas::isFgActive();

      std::string line = stateStr;
      if (fsr) line += "  FSR";
      if (fg)  line += "  FG";

      uint32_t color = COLOR_DIM;
      switch (Vegas::getLastPerfState()) {
        case VegasPerformanceState::Lagging:    color = COLOR_WARN; break;
        case VegasPerformanceState::Stuttering: color = COLOR_WARN; break;
        case VegasPerformanceState::Overheating:color = COLOR_CRIT; break;
        default: color = COLOR_DIM; break;
      }

      x = w - margin - static_cast<int32_t>(line.size()) * 8;
      m_renderer.drawText(fontSz, { x, y }, color, line);
    }

    // ----------------------------------------------------------------
    // Line 3: Numeric frame-time history (last 6 frames)
    // ----------------------------------------------------------------
    y += lineH + 2;
    {
      // Build a compact string showing recent frame times
      std::string ftLine = "FT:";
      for (uint32_t i = 0; i < FT_DISPLAY_COUNT; i++) {
        float ft = Vegas::getHistoryFt(i);
        if (ft <= 0.0f || ft > 500.0f)
          ft = 0.0f;

        char buf[16];
        std::snprintf(buf, sizeof(buf), " %.1f", static_cast<double>(ft));
        ftLine += buf;
      }

      x = w - margin - static_cast<int32_t>(ftLine.size()) * 8;
      if (ftLine.size() > 3) // "FT:" minimum
        m_renderer.drawText(fontSz, { x, y }, COLOR_HIST, ftLine);
    }

    // Flush all queued text draws
    m_renderer.flushDraws(ctx, dstView, m_options);

    // End HUD debug label
    m_renderer.endFrame(ctx);
  }

} // namespace dxvk
