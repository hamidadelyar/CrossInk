#pragma once

#include <I18n.h>

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "ai/AiRecapClient.h"
#include "ai/AiRecapJob.h"
#include "ai/AiRecapSettings.h"
#include "util/ButtonNavigator.h"

// A lightweight handoff replaces the EPUB reader before rebooting into a clean
// network heap. The same activity on network boot owns Wi-Fi and the result.
class AiRecapActivity final : public Activity {
 public:
  AiRecapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool restartBeforeNetwork = false)
      : Activity("AiRecap", renderer, mappedInput), restartBeforeNetwork(restartBeforeNetwork) {}
  AiRecapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, StrId message)
      : Activity("AiRecap", renderer, mappedInput), restartBeforeNetwork(false), messageOnly(true), errorId(message) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
  bool allowGlobalHomeGesture() const override { return false; }
  bool blocksGlobalInput() const override;

 private:
  enum class State : uint8_t { Loading, ChooseWifi, Generate, Working, Result, Error };
  State state = State::Loading;
  bool restartBeforeNetwork;
  bool messageOnly = false;
  bool wifiActivated = false;
  bool initialRelease = false;
  bool cached = false;
  bool cacheSaved = false;
  bool touchOverrideActive = false;
  AiRecap::Job job;
  AiRecapSettings settings;
  ButtonNavigator navigator;
  // Allocated only after TLS is finished; capped by MAX_RECAP_BYTES and 512 lines.
  std::vector<std::string> lines;
  std::vector<std::string> messageLines;
  std::string resultNotice;
  std::string statusNotice;
  int page = 0;
  int linesPerPage = 1;
  StrId errorId = StrId::STR_AI_SOURCE_ERROR;
  static bool cancelled(void* context);
  void showError(StrId message);
  void startWifi();
  void generate();
  bool showRecap(const char* path);
  void returnToReader();
  void turnPage(int delta);
  void stopWifi();
};
