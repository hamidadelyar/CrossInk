#include "AiRecapActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointState.h"
#include "HalClock.h"
#include "ReaderUtils.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/WifiUtils.h"
#include "util/InputReleaseGuard.h"

namespace {
StrId errorMessage(const AiRecapError error) {
  switch (error) {
    case AiRecapError::NotConfigured:
      return StrId::STR_AI_SETUP_REQUIRED;
    case AiRecapError::StorageFailure:
      return StrId::STR_AI_STORAGE_ERROR;
    case AiRecapError::TooLarge:
      return StrId::STR_AI_TOO_LARGE;
    case AiRecapError::Tls:
      return StrId::STR_AI_TLS_ERROR;
    case AiRecapError::Unauthorized:
      return StrId::STR_AI_KEY_ERROR;
    case AiRecapError::RateLimited:
      return StrId::STR_AI_RATE_LIMIT;
    case AiRecapError::Cancelled:
      return StrId::STR_AI_CANCELLED;
    case AiRecapError::OutOfMemory:
      return StrId::STR_MEMORY_ERROR;
    case AiRecapError::InvalidResponse:
      return StrId::STR_AI_INVALID_RESPONSE;
    case AiRecapError::Unsupported:
      return StrId::STR_AI_UNSUPPORTED;
    case AiRecapError::Provider:
      return StrId::STR_AI_PROVIDER_ERROR;
    default:
      return StrId::STR_AI_NETWORK_ERROR;
  }
}
}  // namespace

bool AiRecapActivity::preventAutoSleep() { return state != State::Result && state != State::Error; }

bool AiRecapActivity::blocksGlobalInput() const { return state == State::Working; }

void AiRecapActivity::onEnter() {
  Activity::onEnter();
  if (restartBeforeNetwork) {
    silentRestartToNetwork(NetworkBootTarget::AI_RECAP);
    return;
  }
  mappedInput.setReaderTouchscreenOverride(true);
  touchOverrideActive = true;
  initialRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  if (messageOnly) {
    showError(errorId);
    return;
  }
  if (!AiRecap::loadJob(job) || !AiRecap::validateSource(job)) {
    showError(StrId::STR_AI_SOURCE_ERROR);
    return;
  }
  if (job.readerOrientation < CrossPointSettings::ORIENTATION_COUNT) {
    ReaderUtils::applyOrientation(renderer, job.readerOrientation);
  }
  std::string cachedPath;
  if (AiRecap::findCachedRecap(job, cachedPath)) {
    cached = true;
    cacheSaved = true;
    // A damaged cache must not silently turn an offline view into a billed call.
    if (!showRecap(cachedPath.c_str()))
      showError(StrId::STR_AI_STORAGE_ERROR);
    else
      requestUpdate();
    return;
  }
  const auto provider = job.provider == "anthropic" ? AiRecapProvider::Anthropic : AiRecapProvider::OpenAI;
  if (!settings.loadProvider(provider)) {
    showError(StrId::STR_AI_SAVE_FAILED);
    return;
  }
  snprintf(settings.model, sizeof(settings.model), "%s", job.model.c_str());
  if (!settings.configured()) {
    showError(StrId::STR_AI_SETUP_REQUIRED);
    return;
  }
  {
    RenderLock lock(*this);
    const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    const int width = std::max(1, safe.width - UITheme::getInstance().getMetrics().contentSidePadding * 2);
    messageLines = renderer.wrappedText(UI_10_FONT_ID, I18N.get(StrId::STR_AI_GENERATING), width, 4);
  }
  state = State::ChooseWifi;
  requestUpdate();
}

void AiRecapActivity::stopWifi() {
  if (!wifiActivated) return;
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
}

void AiRecapActivity::onExit() {
  stopWifi();
  AiRecap::clearPendingJob();
  if (touchOverrideActive) mappedInput.setReaderTouchscreenOverride(false);
  memset(settings.apiKey, 0, sizeof(settings.apiKey));
  lines.clear();
  Activity::onExit();
  // Match existing reader network flows: reclaim TLS/Wi-Fi fragmentation before
  // reopening the EPUB. Offline cache views never need this second restart.
  if (wifiActivated) silentRestartToReader(true);
}

void AiRecapActivity::showError(const StrId message) {
  {
    RenderLock lock(*this);
    errorId = message;
    const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    const int width = std::max(1, safe.width - UITheme::getInstance().getMetrics().contentSidePadding * 2);
    messageLines = renderer.wrappedText(UI_10_FONT_ID, I18N.get(message), width, 4);
    state = State::Error;
  }
  requestUpdate();
}

void AiRecapActivity::startWifi() {
  sdFontSystem.releaseForNetwork(renderer);
  wifiActivated = true;
  if (hasActiveStationWifiConnection()) {
    state = State::Generate;
    return;
  }
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput, true, true);
  if (!wifi) {
    LOG_ERR("AI", "OOM: Wi-Fi selector");
    showError(StrId::STR_MEMORY_ERROR);
    return;
  }
  state = State::Working;
  startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
    if (result.isCancelled)
      showError(StrId::STR_AI_CANCELLED);
    else
      state = State::Generate;
    requestUpdate();
  });
}

bool AiRecapActivity::cancelled(void* context) {
  auto* self = static_cast<AiRecapActivity*>(context);
  self->mappedInput.update();
  return self->mappedInput.isPressed(MappedInputManager::Button::Back) ||
         TouchHeaderBackButton::wasTapped(self->mappedInput, self->renderer);
}

void AiRecapActivity::generate() {
  {
    RenderLock lock(*this);
    state = State::Working;
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    showError(StrId::STR_AI_NETWORK_ERROR);
    return;
  }
  WiFi.setSleep(false);
#ifndef SIMULATOR
  // Certificate verification requires a credible clock; TLS fails closed if
  // neither the RTC nor NTP supplies one.
  halClock.syncSystemTimeFromNTP();
#endif
  if (cancelled(this)) {
    stopWifi();
    showError(StrId::STR_AI_CANCELLED);
    return;
  }
  const auto result =
      AiRecapClient::generate(settings, AiRecap::SOURCE_PATH, job.options.detail == AiRecap::Detail::Detailed,
                              AiRecap::RESPONSE_PATH, &AiRecapActivity::cancelled, this);
  memset(settings.apiKey, 0, sizeof(settings.apiKey));
  stopWifi();
  if (result.error != AiRecapError::None) {
    showError(errorMessage(result.error));
    return;
  }
  std::string recapPath;
  cacheSaved = AiRecap::storeCachedRecap(job, AiRecap::RESPONSE_PATH, recapPath);
  const char* displayPath = cacheSaved ? recapPath.c_str() : AiRecap::RESPONSE_PATH;
  if (!showRecap(displayPath))
    showError(StrId::STR_AI_STORAGE_ERROR);
  else
    requestUpdate();
}

bool AiRecapActivity::showRecap(const char* path) {
  FsFile file;
  if (!Storage.openFileForRead("AI", path, file)) return false;
  const size_t size = file.size();
  if (size == 0 || size > AiRecap::MAX_RECAP_BYTES) {
    file.close();
    return false;
  }
  // At most 16 KiB, after the TLS connection has been destroyed. It cannot fit
  // the task stack and is released immediately after the one-time line layout.
  auto text = makeUniqueNoThrow<char[]>(size + 1);
  if (!text) {
    LOG_ERR("AI", "OOM: recap display (%u bytes)", static_cast<unsigned>(size + 1));
    file.close();
    return false;
  }
  const bool read = file.read(text.get(), size) == static_cast<int>(size);
  file.close();
  if (!read) return false;
  text[size] = '\0';
  {
    RenderLock lock(*this);
    const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int width = std::max(1, safe.width - metrics.contentSidePadding * 2);
    lines = renderer.wrappedText(UI_10_FONT_ID, text.get(), width, 512);
    resultNotice = renderer.truncatedText(SMALL_FONT_ID, I18N.get(StrId::STR_AI_RESULT_NOTICE), width);
    statusNotice = renderer.truncatedText(SMALL_FONT_ID,
                                          I18N.get(job.contextLimited ? StrId::STR_AI_LIMITED_CONTEXT
                                                   : cached           ? StrId::STR_AI_CACHED
                                                   : cacheSaved       ? StrId::STR_AI_SAVED
                                                                      : StrId::STR_AI_NOT_SAVED),
                                          width);
    const int headerHeight = metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput);
    const int notices = renderer.getLineHeight(SMALL_FONT_ID) * 3 + metrics.verticalSpacing * 2;
    linesPerPage = std::max(1, (safe.height - headerHeight - notices) / (renderer.getLineHeight(UI_10_FONT_ID) + 3));
    page = 0;
    state = State::Result;
  }
  return !lines.empty();
}

void AiRecapActivity::returnToReader() {
  if (messageOnly) {
    finishAfterBackPress();
    return;
  }
  const std::string path = job.bookPath.empty() ? APP_STATE.openEpubPath : job.bookPath;
  if (path.empty())
    onGoHome();
  else
    activityManager.goToReader(path, false, false, true);
}

void AiRecapActivity::turnPage(const int delta) {
  if (state != State::Result) return;
  const int pages = std::max(1, (static_cast<int>(lines.size()) + linesPerPage - 1) / linesPerPage);
  {
    RenderLock lock(*this);
    page = std::clamp(page + delta, 0, pages - 1);
  }
  requestUpdate();
}

void AiRecapActivity::loop() {
  if (state == State::ChooseWifi) {
    startWifi();
    return;
  }
  if (state == State::Generate) {
    generate();
    return;
  }
  if (InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Confirm, initialRelease))
    return;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    returnToReader();
    return;
  }
  navigator.onNext([this] { turnPage(1); });
  navigator.onPrevious([this] { turnPage(-1); });
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Up) turnPage(1);
  if (swipe == MappedInputManager::SwipeDir::Right || swipe == MappedInputManager::SwipeDir::Down) turnPage(-1);
}

void AiRecapActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect header{safe.x, safe.y + metrics.topPadding, safe.width,
                    TouchHeaderBackButton::height(metrics, mappedInput)};
  if (mappedInput.hasTouchHardware())
    TouchHeaderBackButton::draw(renderer, header, I18N.get(StrId::STR_AI_RECAP), true);
  else
    GUI.drawHeader(renderer, header, I18N.get(StrId::STR_AI_RECAP));
  const int x = safe.x + metrics.contentSidePadding;
  int y = header.y + header.height + metrics.verticalSpacing;
  if (state == State::Result) {
    renderer.drawText(SMALL_FONT_ID, x, y, resultNotice.c_str());
    y += renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;
    const int end = std::min(static_cast<int>(lines.size()), (page + 1) * linesPerPage);
    for (int i = page * linesPerPage; i < end; ++i) {
      renderer.drawText(UI_10_FONT_ID, x, y, lines[i].c_str());
      y += renderer.getLineHeight(UI_10_FONT_ID) + 3;
    }
    char label[32];
    const int totalPages = std::max(1, (static_cast<int>(lines.size()) + linesPerPage - 1) / linesPerPage);
    snprintf(label, sizeof(label), "%d/%d", page + 1, totalPages);
    const int bottom = safe.y + safe.height - renderer.getLineHeight(SMALL_FONT_ID) * 2;
    renderer.drawText(SMALL_FONT_ID, x, bottom, statusNotice.c_str());
    renderer.drawText(SMALL_FONT_ID,
                      safe.x + safe.width - metrics.contentSidePadding - renderer.getTextWidth(SMALL_FONT_ID, label),
                      bottom + renderer.getLineHeight(SMALL_FONT_ID), label);
  } else {
    for (const auto& line : messageLines) {
      renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
      y += renderer.getLineHeight(UI_10_FONT_ID) + 6;
    }
  }
  const auto labels = mappedInput.mapLabels(
      mappedInput.withBackArrow(I18N.get(state == State::Working ? StrId::STR_CANCEL : StrId::STR_BACK)), "",
      state == State::Result ? I18N.get(StrId::STR_DIR_UP) : "",
      state == State::Result ? I18N.get(StrId::STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  renderer.displayBuffer();
}
