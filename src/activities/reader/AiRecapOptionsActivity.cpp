#include "AiRecapOptionsActivity.h"

#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#include "activities/util/KeyboardEntryActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/InputReleaseGuard.h"

namespace fui = freeink::ui;
namespace {
constexpr fui::ActionId ACTION_ROW = 1;
constexpr uint16_t PAGE_CHOICES[] = {5, 10, 20, 50, 100};
const StrId LABELS[] = {StrId::STR_AI_SCOPE,      StrId::STR_AI_PAGE_COUNT, StrId::STR_AI_DETAIL,
                        StrId::STR_AI_PROVIDER,   StrId::STR_AI_MODEL,      StrId::STR_AI_API_KEY,
                        StrId::STR_AI_FORGET_KEY, StrId::STR_AI_GENERATE};
}  // namespace

AiRecapOptionsActivity::AiRecapOptionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("AiRecapOptions", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void AiRecapOptionsActivity::onEnter() {
  Activity::onEnter();
  saveFailed = !settings.load();
  {
    RenderLock lock(*this);
    const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    const int width = std::max(1, safe.width - UITheme::getInstance().getMetrics().contentSidePadding * 2);
    constexpr StrId ids[] = {StrId::STR_AI_SEND_NOTICE, StrId::STR_AI_CUTOFF_NOTICE, StrId::STR_AI_CONTEXT_NOTICE,
                             StrId::STR_AI_SAVE_FAILED};
    for (size_t i = 0; i < notices.size(); ++i) {
      notices[i] = renderer.truncatedText(SMALL_FONT_ID, I18N.get(ids[i]), width);
    }
  }
  initialRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &AiRecapOptionsActivity::onRow, this);
  app.setScreen(&AiRecapOptionsActivity::listScreen, this);
  requestUpdate();
}

void AiRecapOptionsActivity::onExit() {
  // Keys remain in device storage, never in a reader result or an SD job.
  memset(settings.apiKey, 0, sizeof(settings.apiKey));
  Activity::onExit();
}

void AiRecapOptionsActivity::persist() {
  saveFailed = !settings.save();
  requestUpdate();
}

void AiRecapOptionsActivity::select() {
  if (selected == 0) {
    options.scope = static_cast<AiRecap::Scope>((static_cast<unsigned>(options.scope) + 1) % 3);
  } else if (selected == 1) {
    size_t next = 0;
    for (size_t i = 0; i < std::size(PAGE_CHOICES); ++i) {
      if (PAGE_CHOICES[i] == options.pages) next = (i + 1) % std::size(PAGE_CHOICES);
    }
    options.pages = PAGE_CHOICES[next];
    options.scope = AiRecap::Scope::LastPages;
  } else if (selected == 2) {
    options.detail = options.detail == AiRecap::Detail::Short ? AiRecap::Detail::Detailed : AiRecap::Detail::Short;
  } else if (selected == 3) {
    const auto provider =
        settings.provider == AiRecapProvider::OpenAI ? AiRecapProvider::Anthropic : AiRecapProvider::OpenAI;
    if (settings.loadProvider(provider))
      persist();
    else
      saveFailed = true;
  } else if (selected == 4 || selected == 5) {
    const bool key = selected == 5;
    // One short-lived keyboard activity; its text is bounded to the provider limits.
    auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(
        renderer, mappedInput, I18N.get(key ? StrId::STR_AI_API_KEY : StrId::STR_AI_MODEL), key ? "" : settings.model,
        key ? AiRecapSettings::MAX_KEY_LENGTH : AiRecapSettings::MAX_MODEL_LENGTH,
        key ? InputType::Password : InputType::Text, 1);
    if (!keyboard) {
      LOG_ERR("AI", "OOM: recap setup keyboard");
      saveFailed = true;
      requestUpdate();
      return;
    }
    startActivityForResult(std::move(keyboard), [this, key](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& text = std::get<KeyboardResult>(result.data).text;
        if (key)
          snprintf(settings.apiKey, sizeof(settings.apiKey), "%s", text.c_str());
        else
          snprintf(settings.model, sizeof(settings.model), "%s", text.c_str());
        persist();
      }
    });
    return;
  } else if (selected == 6) {
    saveFailed = !settings.clearKey();
  } else if (selected == 7) {
    // A matching saved recap remains readable after the user deletes the key.
    // The network activity checks credentials only after trying the exact cache.
    if (AiRecapSettings::validModel(settings.model)) {
      setResult(AiRecapOptionsResult{options, settings.provider, settings.model});
      finish();
      return;
    } else {
      saveFailed = true;
    }
  }
  requestUpdate();
}

void AiRecapOptionsActivity::onRow(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<AiRecapOptionsActivity*>(user);
  if (event.value < 0 || event.value >= ROW_COUNT) return;
  self->selected = event.value;
  self->app.clearTapFlash();
  self->select();
}

void AiRecapOptionsActivity::loop() {
  if (InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Confirm, initialRelease))
    return;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finishAfterBackPress();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    select();
    return;
  }
  if (uiReady) {
    const auto snapshot = touchSnapshotFrom(mappedInput);
    if (snapshot.touchPressed || snapshot.touchReleased) {
      const auto event = app.route(snapshot);
      if (app.invalidated()) requestUpdate();
      if (event) return;
    }
  }
  navigator.onNext([this] {
    selected = (selected + 1) % ROW_COUNT;
    topIndex = followListSelection(selected, topIndex, visibleRows, ROW_COUNT);
    requestUpdate();
  });
  navigator.onPrevious([this] {
    selected = (selected + ROW_COUNT - 1) % ROW_COUNT;
    topIndex = followListSelection(selected, topIndex, visibleRows, ROW_COUNT);
    requestUpdate();
  });
}

void AiRecapOptionsActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<AiRecapOptionsActivity*>(user)->buildList(screen);
}

void AiRecapOptionsActivity::buildList(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int noticeHeight = renderer.getLineHeight(SMALL_FONT_ID) * 3 + metrics.verticalSpacing;
  screen.setContentMargin(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput) +
                           noticeHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - safe.x - safe.width),
      static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height), static_cast<int16_t>(safe.x)});
  char pages[8];
  snprintf(pages, sizeof(pages), "%u", options.pages);
  items.fill({});
  for (size_t i = 0; i < items.size(); ++i) {
    items[i].label = I18N.get(LABELS[i]);
    items[i].actionValue = static_cast<int16_t>(i);
  }
  items[0].value = options.scope == AiRecap::Scope::PreviousChapter ? I18N.get(StrId::STR_AI_PREVIOUS_CHAPTER)
                   : options.scope == AiRecap::Scope::SoFar         ? I18N.get(StrId::STR_AI_SO_FAR)
                                                                    : I18N.get(StrId::STR_AI_LAST_PAGES);
  items[1].value = pages;
  items[2].value =
      options.detail == AiRecap::Detail::Short ? I18N.get(StrId::STR_AI_SHORT) : I18N.get(StrId::STR_AI_DETAILED);
  items[3].value = settings.provider == AiRecapProvider::OpenAI ? "OpenAI" : "Claude";
  items[4].value = settings.model;
  items[5].value = settings.apiKey[0] ? I18N.get(StrId::STR_AI_KEY_SAVED) : I18N.get(StrId::STR_NOT_SET);
  fui::ListProps props;
  props.items = items.data();
  props.count = ROW_COUNT;
  props.selectedIndex = selected;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, ROW_COUNT);
  props.topIndex = topIndex;
  screen.list(props);
}

void AiRecapOptionsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect header{safe.x, safe.y + metrics.topPadding, safe.width,
                    TouchHeaderBackButton::height(metrics, mappedInput)};
  if (mappedInput.hasTouchHardware())
    TouchHeaderBackButton::draw(renderer, uiTarget, header, I18N.get(StrId::STR_AI_RECAP), false);
  else
    GUI.drawHeader(renderer, header, I18N.get(StrId::STR_AI_RECAP));
  const int x = safe.x + metrics.contentSidePadding;
  const int y = header.y + header.height;
  renderer.drawText(SMALL_FONT_ID, x, y, notices[saveFailed ? 3 : 0].c_str());
  renderer.drawText(SMALL_FONT_ID, x, y + renderer.getLineHeight(SMALL_FONT_ID), notices[1].c_str());
  renderer.drawText(SMALL_FONT_ID, x, y + renderer.getLineHeight(SMALL_FONT_ID) * 2, notices[2].c_str());
  uiReady = false;
  app.render();
  uiReady = true;
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(I18N.get(StrId::STR_BACK)), I18N.get(StrId::STR_SELECT),
                            I18N.get(StrId::STR_DIR_UP), I18N.get(StrId::STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
