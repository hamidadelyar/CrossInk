#include "AiRecapSettings.h"

#include <Logging.h>

#include <cstring>

#ifndef SIMULATOR
#include <nvs.h>
#endif

namespace {
#ifndef SIMULATOR
constexpr char NVS_NAMESPACE[] = "ai_recap";
#endif

void eraseSecret(char* value, const size_t size) {
  volatile char* cursor = value;
  for (size_t i = 0; i < size; ++i) cursor[i] = 0;
}

#ifdef SIMULATOR
// Simulator settings are deliberately process-local; API keys never become
// files in a simulated SD card or a checkout.
struct SimProfile {
  char model[AiRecapSettings::MAX_MODEL_LENGTH + 1] = {};
  char key[AiRecapSettings::MAX_KEY_LENGTH + 1] = {};
};
SimProfile simProfiles[2];
AiRecapProvider simProvider = AiRecapProvider::OpenAI;
#else
const char* modelKey(const AiRecapProvider provider) {
  return provider == AiRecapProvider::OpenAI ? "openai_model" : "anth_model";
}
const char* credentialKey(const AiRecapProvider provider) {
  return provider == AiRecapProvider::OpenAI ? "openai_key" : "anth_key";
}

bool readString(const nvs_handle_t handle, const char* key, char* destination, const size_t capacity) {
  size_t length = capacity;
  const esp_err_t result = nvs_get_str(handle, key, destination, &length);
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK || length == 0 || length > capacity) {
    LOG_ERR("AI", "Cannot read stored setting");
    return false;
  }
  return true;
}
#endif
}  // namespace

AiRecapSettings::~AiRecapSettings() { eraseSecret(apiKey, sizeof(apiKey)); }

const char* AiRecapSettings::defaultModel(const AiRecapProvider selected) {
  return selected == AiRecapProvider::Anthropic ? "claude-haiku-4-5-20251001" : "gpt-4.1-mini";
}

bool AiRecapSettings::validProvider(const AiRecapProvider selected) {
  return selected == AiRecapProvider::OpenAI || selected == AiRecapProvider::Anthropic;
}

bool AiRecapSettings::validModel(const char* value) {
  if (!value || !value[0]) return false;
  for (size_t i = 0; i <= MAX_MODEL_LENGTH; ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (!c) return true;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
          c == '.' || c == ':')) {
      return false;
    }
  }
  return false;
}

bool AiRecapSettings::validKey(const char* value) {
  if (!value || !value[0]) return false;
  for (size_t i = 0; i <= MAX_KEY_LENGTH; ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (!c) return true;
    // Header credentials cannot contain whitespace, newlines or control bytes.
    if (c <= 0x20 || c >= 0x7f) return false;
  }
  return false;
}

bool AiRecapSettings::configured() const { return validProvider(provider) && validModel(model) && validKey(apiKey); }

bool AiRecapSettings::load() {
#ifdef SIMULATOR
  return loadProvider(simProvider);
#else
  nvs_handle_t handle;
  const esp_err_t opened = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) return loadProvider(AiRecapProvider::OpenAI);
  if (opened != ESP_OK) {
    LOG_ERR("AI", "Cannot open stored settings");
    eraseSecret(apiKey, sizeof(apiKey));
    return false;
  }
  uint8_t selected = 0;
  const esp_err_t result = nvs_get_u8(handle, "provider", &selected);
  nvs_close(handle);
  if ((result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) || !validProvider(static_cast<AiRecapProvider>(selected))) {
    LOG_ERR("AI", "Invalid stored provider");
    eraseSecret(apiKey, sizeof(apiKey));
    return false;
  }
  return loadProvider(static_cast<AiRecapProvider>(selected));
#endif
}

bool AiRecapSettings::loadProvider(const AiRecapProvider selected) {
  eraseSecret(apiKey, sizeof(apiKey));
  if (!validProvider(selected)) return false;
  provider = selected;
  std::strncpy(model, defaultModel(provider), sizeof(model));
#ifdef SIMULATOR
  const auto& profile = simProfiles[static_cast<uint8_t>(provider)];
  if (profile.model[0]) std::memcpy(model, profile.model, sizeof(model));
  std::memcpy(apiKey, profile.key, sizeof(apiKey));
#else
  nvs_handle_t handle;
  const esp_err_t opened = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND) return true;
  if (opened != ESP_OK) {
    LOG_ERR("AI", "Cannot open provider settings");
    return false;
  }
  const bool ok = readString(handle, modelKey(provider), model, sizeof(model)) &&
                  readString(handle, credentialKey(provider), apiKey, sizeof(apiKey));
  nvs_close(handle);
  if (!ok) {
    eraseSecret(apiKey, sizeof(apiKey));
    return false;
  }
#endif
  if (!validModel(model) || (apiKey[0] && !validKey(apiKey))) {
    LOG_ERR("AI", "Invalid provider settings");
    eraseSecret(apiKey, sizeof(apiKey));
    return false;
  }
  return true;
}

bool AiRecapSettings::save() const {
  if (!validProvider(provider) || !validModel(model) || (apiKey[0] && !validKey(apiKey))) {
    LOG_ERR("AI", "Refusing invalid provider settings");
    return false;
  }
#ifdef SIMULATOR
  auto& profile = simProfiles[static_cast<uint8_t>(provider)];
  std::memcpy(profile.model, model, sizeof(model));
  std::memcpy(profile.key, apiKey, sizeof(apiKey));
  simProvider = provider;
  return true;
#else
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
    LOG_ERR("AI", "Cannot open settings for save");
    return false;
  }
  const bool ok = nvs_set_str(handle, modelKey(provider), model) == ESP_OK &&
                  nvs_set_str(handle, credentialKey(provider), apiKey) == ESP_OK &&
                  nvs_set_u8(handle, "provider", static_cast<uint8_t>(provider)) == ESP_OK &&
                  nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  if (!ok) LOG_ERR("AI", "Cannot save provider settings");
  return ok;
#endif
}

bool AiRecapSettings::clearKey() {
  eraseSecret(apiKey, sizeof(apiKey));
  return save();
}
