#pragma once

#include "overlay_metrics.hpp"

#include <string>

struct NVGcontext;

namespace ounce
{

class StreamOverlay
{
  public:
    void update(uint64_t now, uint64_t received, bool real, bool generated, uint64_t submitted, uint64_t decoded);
    void draw(NVGcontext* vg, float x, float y, int font);

  private:
    OverlayMetrics metrics;
    float panelWidth = 332;
    bool measureText = true;
    std::string rates = "Measuring frame rates…";
    std::string breakdown, latency;
};

}
