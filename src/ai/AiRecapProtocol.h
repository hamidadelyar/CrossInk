#pragma once

#include <cstddef>

#include "AiRecapClient.h"

namespace AiRecapProtocol {
constexpr size_t MAX_SOURCE_BYTES = 64 * 1024 + 8 * 1024 + 256;
constexpr size_t MAX_RESPONSE_BYTES = 24 * 1024;
constexpr size_t MAX_OUTPUT_BYTES = 8 * 1024;
constexpr size_t MAX_JSON_HEAP_BYTES = 24 * 1024;
// Negative read denotes I/O failure; zero is EOF. A failed write aborts.
using Read = int (*)(void*, char*, size_t);
using Write = bool (*)(void*, const char*, size_t);
AiRecapError writeRequest(AiRecapProvider provider, const char* model, bool detailed, Read read, void* readContext,
                          Write write, void* writeContext);
// The JSON allocator is bounded separately from the response buffer. No text
// is accepted until the entire document and completion status are validated.
AiRecapClientResult parseResponse(AiRecapProvider provider, char* json, size_t length, char* output, size_t capacity);
}  // namespace AiRecapProtocol
