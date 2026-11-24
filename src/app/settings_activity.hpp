#pragma once

#include <borealis.hpp>

namespace ounce
{

// Resolution, frame rate and bitrate for every stream. Changes are saved at once.
class SettingsActivity : public brls::Activity
{
  public:
    brls::View* createContentView() override;

  private:
    void refreshBitrate();
    void refreshUpscaling();
    std::vector<std::string> bitrateChoices() const;

    brls::SelectorCell* bitrate      = nullptr;
    brls::SelectorCell* frameGenMode = nullptr;
    brls::BooleanCell* upscaling     = nullptr;
    brls::SelectorCell* sharpening   = nullptr;
};

}
