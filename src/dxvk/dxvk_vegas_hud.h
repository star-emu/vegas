#pragma once

#include "dxvk_device.h"
#include "hud/dxvk_hud_renderer.h"
#include "dxvk_vegas.h"

namespace dxvk {

  /**
   * \brief VegasHud — standalone dynamic overlay for Adreno metrics
   *
   * Completely independent of the DXVK_HUD system. Uses its own
   * HudRenderer instance to render text at the top-right of the
   * swapchain image. Reads metrics from Vegas static state.
   *
   * Controlled by the `vegas.enableHud` config option (Auto = enabled
   * on Adreno). Always constructed; render() is a no-op when disabled.
   */
  class VegasHud {

  public:

    VegasHud(
      const Rc<DxvkDevice>& device);

    ~VegasHud();

    VegasHud(const VegasHud&) = delete;
    VegasHud& operator=(const VegasHud&) = delete;

    /**
     * \brief Render the HUD overlay into an active render pass
     *
     * Must be called between cmdBeginRendering / cmdEndRendering.
     * dstView can be the swapchain image (non-composited path) or
     * the HUD composition image (composited path).
     */
    void render(
      const Rc<DxvkCommandList>& ctx,
      const Rc<DxvkImageView>&   dstView);

    /** \brief True if the HUD will produce visible output */
    bool isEnabled() const { return m_enabled; }

  private:

    hud::HudRenderer m_renderer;
    hud::HudOptions  m_options;
    bool             m_enabled = false;

    /** Number of recent frame times to show in numeric history */
    static constexpr uint32_t FT_DISPLAY_COUNT = 6;

    /** Colors (ARGB) */
    static constexpr uint32_t COLOR_HEADER  = 0xFF00E5FF; /* cyan */
    static constexpr uint32_t COLOR_NORMAL  = 0xFF00FF00; /* green */
    static constexpr uint32_t COLOR_WARN    = 0xFFFFFF00; /* yellow */
    static constexpr uint32_t COLOR_CRIT    = 0xFFFF4500; /* orange-red */
    static constexpr uint32_t COLOR_WHITE   = 0xFFFFFFFF;
    static constexpr uint32_t COLOR_DIM     = 0xFFAAAAAA;
    static constexpr uint32_t COLOR_HIST  = 0xFF88FFFF; /* cyan-tinted history */

    VkExtent3D getExtent(
      const Rc<DxvkImageView>& dstView) const;
  };

} // namespace dxvk
