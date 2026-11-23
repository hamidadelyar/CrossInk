#include "AiRecapClient.h"

#include "AiRecapProtocol.h"

#ifndef SIMULATOR
#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "network/WifiPowerSaveGuard.h"

namespace {
constexpr int CONNECT_TIMEOUT_MS = 15000;
constexpr int READ_TIMEOUT_MS = 1000;
constexpr uint32_t RESPONSE_TIMEOUT_MS = 90000;

struct RequestInput {
  FsFile* file;
  bool (*cancel)(void*);
  void* context;
  size_t expectedBytes;
  size_t readBytes = 0;
  bool cancelled = false;
};

int readSource(void* context, char* buffer, const size_t size) {
  auto* input = static_cast<RequestInput*>(context);
  if (input->cancel && input->cancel(input->context)) {
    input->cancelled = true;
    return -1;
  }
  const int count = input->file->read(buffer, size);
  if (count < 0 || static_cast<size_t>(count) > input->expectedBytes - input->readBytes) return -1;
  input->readBytes += static_cast<size_t>(count);
  if (!count && input->readBytes != input->expectedBytes) return -1;
  return count;
}

bool countBytes(void* context, const char*, const size_t size) {
  *static_cast<size_t*>(context) += size;
  return true;
}

// One 1 KiB transmit buffer amortizes JSON escaping writes. It is heap-owned
// only for this request; putting this plus credentials on a 4 KiB task stack
// would leave too little room for the HTTP/TLS call chain.
struct RequestBuffers {
  char bytes[1024];
  char authorization[AiRecapSettings::MAX_KEY_LENGTH + 8];
  size_t used = 0;
  esp_http_client_handle_t client = nullptr;
  RequestInput* input = nullptr;

  ~RequestBuffers() {
    volatile char* value = authorization;
    for (size_t i = 0; i < sizeof(authorization); ++i) value[i] = 0;
  }

  bool flush() {
    size_t sent = 0;
    while (sent < used) {
      if (input->cancel && input->cancel(input->context)) {
        input->cancelled = true;
        return false;
      }
      const int result = esp_http_client_write(client, bytes + sent, static_cast<int>(used - sent));
      if (result <= 0) return false;
      sent += static_cast<size_t>(result);
      delay(0);
    }
    used = 0;
    return true;
  }
};

bool sendBytes(void* context, const char* bytes, size_t size) {
  auto* output = static_cast<RequestBuffers*>(context);
  while (size) {
    const size_t count = std::min(size, sizeof(output->bytes) - output->used);
    std::memcpy(output->bytes + output->used, bytes, count);
    output->used += count;
    bytes += count;
    size -= count;
    if (output->used == sizeof(output->bytes) && !output->flush()) return false;
  }
  return true;
}

struct ClientOwner {
  esp_http_client_handle_t handle = nullptr;
  ~ClientOwner() { close(); }
  void close() {
    if (handle) esp_http_client_cleanup(handle);
    handle = nullptr;
  }
};

// ESP-IDF logs the complete outgoing HTTP headers at DEBUG level, including
// Authorization. Suppress that SDK trace for this request even in debug builds.
class PrivateHttpLogGuard {
  esp_log_level_t previous_ = esp_log_level_get("HTTP_CLIENT");

 public:
  PrivateHttpLogGuard() {
    if (previous_ > ESP_LOG_WARN) esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);
  }
  ~PrivateHttpLogGuard() { esp_log_level_set("HTTP_CLIENT", previous_); }
};

AiRecapClientResult failure(const AiRecapError error, const int status = 0) {
  // Do not log provider response bodies, request text, URLs with query strings,
  // or credentials. This is the only request-failure log.
  LOG_ERR("AI", "Recap request failed (error %u, HTTP %d)", static_cast<unsigned>(error), status);
  return {error, status, 0};
}
}  // namespace
#endif

AiRecapClientResult AiRecapClient::generate(const AiRecapSettings& settings, const char* sourcePath,
                                            const bool detailed, const char* outputPath, bool (*cancel)(void*),
                                            void* context) {
#ifdef SIMULATOR
  (void)settings;
  (void)sourcePath;
  (void)detailed;
  (void)outputPath;
  (void)cancel;
  (void)context;
  return {AiRecapError::Unsupported};
#else
  if (!settings.configured()) return failure(AiRecapError::NotConfigured);
  if (!sourcePath || !outputPath || !sourcePath[0] || !outputPath[0] || !std::strcmp(sourcePath, outputPath)) {
    return failure(AiRecapError::StorageFailure);
  }
  if (cancel && cancel(context)) return failure(AiRecapError::Cancelled);
  // TLS certificate validity must be evaluated against real time; fail closed
  // if the caller could not establish it. There is no insecure fallback.
  if (std::time(nullptr) < 1700000000) return failure(AiRecapError::Tls);
  if (Storage.exists(outputPath) && !Storage.remove(outputPath)) return failure(AiRecapError::StorageFailure);
  FsFile source;
  if (!Storage.openFileForRead("AI", sourcePath, source)) return failure(AiRecapError::StorageFailure);
  const size_t sourceSize = source.size();
  if (!sourceSize || sourceSize > AiRecapProtocol::MAX_SOURCE_BYTES) {
    source.close();
    return failure(sourceSize ? AiRecapError::TooLarge : AiRecapError::StorageFailure);
  }
  RequestInput input{&source, cancel, context, sourceSize};
  size_t payloadBytes = 0;
  auto error = AiRecapProtocol::writeRequest(settings.provider, settings.model, detailed, readSource, &input,
                                             countBytes, &payloadBytes);
  if (error != AiRecapError::None || !source.seek(0)) {
    source.close();
    return failure(input.cancelled               ? AiRecapError::Cancelled
                   : error == AiRecapError::None ? AiRecapError::StorageFailure
                                                 : error);
  }
  input.readBytes = 0;
  auto buffers = makeUniqueNoThrow<RequestBuffers>();
  if (!buffers) {
    source.close();
    return failure(AiRecapError::OutOfMemory);
  }
  const bool openai = settings.provider == AiRecapProvider::OpenAI;
  // The IDF configuration is bounded stack state required for client init;
  // payload and response storage stay off the small network task stack.
  esp_http_client_config_t config = {};
  config.url = openai ? "https://api.openai.com/v1/chat/completions" : "https://api.anthropic.com/v1/messages";
  config.method = HTTP_METHOD_POST;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.skip_cert_common_name_check = false;
  config.disable_auto_redirect = true;
  config.keep_alive_enable = false;
  config.timeout_ms = CONNECT_TIMEOUT_MS;
  config.buffer_size = 1024;
  config.buffer_size_tx = 1024;
  PrivateHttpLogGuard privateLogs;
  ClientOwner client{esp_http_client_init(&config)};
  if (!client.handle) {
    source.close();
    return failure(AiRecapError::OutOfMemory);
  }
  bool headersOk = esp_http_client_set_header(client.handle, "Content-Type", "application/json") == ESP_OK &&
                   esp_http_client_set_header(client.handle, "Accept", "application/json") == ESP_OK;
  if (openai) {
    snprintf(buffers->authorization, sizeof(buffers->authorization), "Bearer %s", settings.apiKey);
    headersOk =
        headersOk && esp_http_client_set_header(client.handle, "Authorization", buffers->authorization) == ESP_OK;
  } else {
    headersOk = headersOk && esp_http_client_set_header(client.handle, "x-api-key", settings.apiKey) == ESP_OK &&
                esp_http_client_set_header(client.handle, "anthropic-version", "2023-06-01") == ESP_OK;
  }
  if (!headersOk) {
    source.close();
    return failure(AiRecapError::OutOfMemory);
  }
  WifiPowerSaveGuard powerSave;
  if (cancel && cancel(context)) {
    source.close();
    return failure(AiRecapError::Cancelled);
  }
  if (esp_http_client_open(client.handle, static_cast<int>(payloadBytes)) != ESP_OK) {
    int tlsError = 0;
    int tlsFlags = 0;
    esp_http_client_get_and_clear_last_tls_error(client.handle, &tlsError, &tlsFlags);
    source.close();
    return failure(tlsError || tlsFlags ? AiRecapError::Tls : AiRecapError::Network);
  }
  buffers->client = client.handle;
  buffers->input = &input;
  error = AiRecapProtocol::writeRequest(settings.provider, settings.model, detailed, readSource, &input, sendBytes,
                                        buffers.get());
  const bool sent = error == AiRecapError::None && buffers->flush();
  source.close();
  if (!sent) return failure(input.cancelled ? AiRecapError::Cancelled : AiRecapError::Network);
  buffers.reset();
  if (cancel && cancel(context)) return failure(AiRecapError::Cancelled);
  if (esp_http_client_set_timeout_ms(client.handle, READ_TIMEOUT_MS) != ESP_OK) {
    return failure(AiRecapError::Network);
  }
  const uint32_t started = millis();
  int64_t contentLength;
  for (;;) {
    if (cancel && cancel(context)) return failure(AiRecapError::Cancelled);
    if (millis() - started >= RESPONSE_TIMEOUT_MS) return failure(AiRecapError::Network);
    contentLength = esp_http_client_fetch_headers(client.handle);
#ifdef ESP_ERR_HTTP_EAGAIN
    // ESP-IDF preserves the HTTP parser across a timed-out header read and
    // assigns status at on_headers_complete. Poll the same response; never
    // reopen the connection or resend a potentially billed request.
    if (contentLength == -ESP_ERR_HTTP_EAGAIN) {
      delay(1);
      continue;
    }
#endif
    break;
  }
  const int status = esp_http_client_get_status_code(client.handle);
  if (status != 200) {
    if (status == 401 || status == 403) return failure(AiRecapError::Unauthorized, status);
    if (status == 429) return failure(AiRecapError::RateLimited, status);
    return failure(status > 0 ? AiRecapError::Provider : AiRecapError::Network, status);
  }
  if (contentLength < 0) return failure(AiRecapError::Network, status);
  if (contentLength > static_cast<int64_t>(AiRecapProtocol::MAX_RESPONSE_BYTES)) {
    return failure(AiRecapError::TooLarge, status);
  }
  // A single bounded 24 KiB response is parsed only after TLS is released.
  // Its size never scales with chapter/book length.
  auto response = makeUniqueNoThrow<char[]>(AiRecapProtocol::MAX_RESPONSE_BYTES + 1);
  if (!response) return failure(AiRecapError::OutOfMemory, status);
  char* const responseData = response.get();
  size_t received = 0;
  for (;;) {
    if (cancel && cancel(context)) return failure(AiRecapError::Cancelled, status);
    if (millis() - started >= RESPONSE_TIMEOUT_MS) return failure(AiRecapError::Network, status);
    // Read through EOF even if the parser is complete: fetch_headers can
    // already have buffered the complete body. One sentinel byte detects an
    // over-limit response without accepting its first MAX_RESPONSE_BYTES.
    const size_t remaining = AiRecapProtocol::MAX_RESPONSE_BYTES + 1 - received;
    const int count = esp_http_client_read(client.handle, responseData + received,
                                           static_cast<int>(std::min<size_t>(remaining, 1024)));
#ifdef ESP_ERR_HTTP_EAGAIN
    if (count == -ESP_ERR_HTTP_EAGAIN) {
      delay(1);
      continue;
    }
#endif
    if (count < 0 || (!count && !esp_http_client_is_complete_data_received(client.handle))) {
      return failure(AiRecapError::Network, status);
    }
    if (!count) break;
    received += static_cast<size_t>(count);
    if (received > AiRecapProtocol::MAX_RESPONSE_BYTES) return failure(AiRecapError::TooLarge, status);
    delay(0);
  }
  client.close();
  response[received] = '\0';
  // The output buffer is allocated only after TLS teardown, capped at 8 KiB;
  // it holds the validated text while the input JSON is still referenced.
  auto output = makeUniqueNoThrow<char[]>(AiRecapProtocol::MAX_OUTPUT_BYTES + 1);
  if (!output) return failure(AiRecapError::OutOfMemory, status);
  auto result = AiRecapProtocol::parseResponse(settings.provider, response.get(), received, output.get(),
                                               AiRecapProtocol::MAX_OUTPUT_BYTES + 1);
  response.reset();
  if (result.error != AiRecapError::None) return failure(result.error, status);
  if (cancel && cancel(context)) return failure(AiRecapError::Cancelled, status);
  FsFile target;
  if (!Storage.openFileForWrite("AI", outputPath, target)) return failure(AiRecapError::StorageFailure, status);
  const bool written = target.write(output.get(), result.outputBytes) == result.outputBytes && target.sync();
  const bool closed = target.close();
  if (!written || !closed) {
    Storage.remove(outputPath);
    return failure(AiRecapError::StorageFailure, status);
  }
  result.httpStatus = status;
  return result;
#endif
}
