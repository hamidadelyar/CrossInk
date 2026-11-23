#pragma once

#include <cstddef>
#include <cstdint>

enum class AiRecapProvider : uint8_t { OpenAI = 0, Anthropic = 1 };

// Loaded only while configuring or generating a recap. Credentials stay in
// device NVS, outside the SD-card files served by File Transfer.
struct AiRecapSettings {
  static constexpr size_t MAX_MODEL_LENGTH = 96;
  static constexpr size_t MAX_KEY_LENGTH = 512;

  AiRecapProvider provider = AiRecapProvider::OpenAI;
  char model[MAX_MODEL_LENGTH + 1] = {};
  char apiKey[MAX_KEY_LENGTH + 1] = {};

  ~AiRecapSettings();
  bool load();
  bool loadProvider(AiRecapProvider selected);
  bool save() const;
  bool clearKey();
  bool configured() const;
  static const char* defaultModel(AiRecapProvider selected);
  static bool validProvider(AiRecapProvider selected);
  static bool validModel(const char* value);
  static bool validKey(const char* value);
};
