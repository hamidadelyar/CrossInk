#pragma once

#include <cstdint>

#include "AiRecapSettings.h"

enum class AiRecapError : uint8_t {
  None,
  NotConfigured,
  StorageFailure,
  TooLarge,
  Network,
  Tls,
  Unauthorized,
  RateLimited,
  Provider,
  InvalidResponse,
  Cancelled,
  OutOfMemory,
  Unsupported,
};

struct AiRecapClientResult {
  AiRecapError error = AiRecapError::None;
  int httpStatus = 0;
  uint32_t outputBytes = 0;
};

namespace AiRecapClient {
// The caller owns Wi-Fi/time setup and supplies a frozen source file. Only a
// complete, validated recap is written to outputPath. There are no retries.
AiRecapClientResult generate(const AiRecapSettings& settings, const char* sourcePath, bool detailed,
                             const char* outputPath, bool (*cancel)(void*) = nullptr, void* context = nullptr);
}  // namespace AiRecapClient
