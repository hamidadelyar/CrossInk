#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <array>
#include <atomic>

#include "activities/Activity.h"
#include "ai/AiRecapJob.h"
#include "ai/AiRecapSettings.h"
#include "util/ButtonNavigator.h"

class AiRecapOptionsActivity final : public Activity {
 public:
  AiRecapOptionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  using UiApp = freeink::ui::FreeInkApp<12, 4>;
  static constexpr int ROW_COUNT = 8;
  AiRecap::Options options;
  AiRecapSettings settings;
  ButtonNavigator navigator;
  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
  // Reused row descriptors (several hundred bytes) live with the activity,
  // outside the small render task stack.
  std::array<freeink::ui::ListItem, ROW_COUNT> items{};
  std::array<std::string, 4> notices;
  std::atomic<bool> uiReady{false};
  int selected = 0;
  int topIndex = 0;
  int visibleRows = 1;
  bool initialRelease = false;
  bool saveFailed = false;
  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRow(const freeink::ui::ActionEvent& event, void* user);
  void buildList(UiApp::ScreenType& screen);
  void select();
  void persist();
};
