#include "AiRecapJob.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace AiRecap {
namespace {
constexpr char DIRECTORY[] = "/.crosspoint/ai";
constexpr char CACHE_DIRECTORY[] = "/.crosspoint/ai/cache";
constexpr char JOB_PATH[] = "/.crosspoint/ai/recap-job.bin";
constexpr char JOB_TEMP_PATH[] = "/.crosspoint/ai/recap-job.tmp";
constexpr uint32_t JOB_MAGIC = 0x31524941;  // AIR1, owned format; not an EPUB cache.
constexpr uint64_t HASH_INITIAL = UINT64_C(14695981039346656037);
constexpr uint64_t HASH_PRIME = UINT64_C(1099511628211);
constexpr size_t MAX_PATH_BYTES = 1024;
constexpr size_t MAX_MODEL_BYTES = 128;

void hashBytes(uint64_t& hash, const void* data, const size_t length) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < length; ++i) {
    hash ^= bytes[i];
    hash *= HASH_PRIME;
  }
}

void hashString(uint64_t& hash, const std::string& value) {
  const uint32_t length = static_cast<uint32_t>(value.size());
  hashBytes(hash, &length, sizeof(length));
  hashBytes(hash, value.data(), value.size());
}

void hashToText(const uint64_t hash, char (&output)[17]) {
  snprintf(output, sizeof(output), "%08lx%08lx", static_cast<unsigned long>(hash >> 32),
           static_cast<unsigned long>(hash & UINT32_MAX));
}

bool validKey(const char (&key)[17]) {
  if (key[16] != '\0') return false;
  for (size_t i = 0; i < 16; ++i) {
    if (!((key[i] >= '0' && key[i] <= '9') || (key[i] >= 'a' && key[i] <= 'f'))) return false;
  }
  return true;
}

bool validString(const std::string& value, const size_t maximum) {
  return !value.empty() && value.size() <= maximum && value.find('\0') == std::string::npos;
}

bool readBoundedString(FsFile& file, std::string& value, const size_t maximum) {
  uint32_t length = 0;
  if (!serialization::tryReadPod(file, length) || length == 0 || length > maximum) return false;
  // Cold-path metadata only: each field has an explicit cap, never book text.
  value.resize(length);
  return file.read(value.data(), length) == static_cast<int>(length) && validString(value, maximum);
}

bool writeJobFile(const char* path, const Job& job) {
  FsFile file;
  if (!Storage.openFileForWrite("AI", path, file)) return false;
  const uint8_t scope = static_cast<uint8_t>(job.options.scope);
  const uint8_t detail = static_cast<uint8_t>(job.options.detail);
  const uint8_t limited = job.contextLimited ? 1 : 0;
  const bool written =
      serialization::tryWritePod(file, JOB_MAGIC) && serialization::tryWritePod(file, PROMPT_VERSION) &&
      serialization::tryWriteString(file, job.bookPath) && serialization::tryWriteString(file, job.provider) &&
      serialization::tryWriteString(file, job.model) && serialization::tryWritePod(file, scope) &&
      serialization::tryWritePod(file, detail) && serialization::tryWritePod(file, job.options.pages) &&
      serialization::tryWritePod(file, job.cutoff.spine) && serialization::tryWritePod(file, job.cutoff.page) &&
      serialization::tryWritePod(file, job.start.spine) && serialization::tryWritePod(file, job.start.page) &&
      serialization::tryWritePod(file, job.end.spine) && serialization::tryWritePod(file, job.end.page) &&
      serialization::tryWritePod(file, job.layoutSignature) && serialization::tryWritePod(file, job.targetBytes) &&
      serialization::tryWritePod(file, job.contextBytes) && serialization::tryWritePod(file, job.sourceBytes) &&
      serialization::tryWritePod(file, limited) && serialization::tryWritePod(file, job.readerOrientation) &&
      file.write(job.contentKey, sizeof(job.contentKey)) == sizeof(job.contentKey);
  const bool synced = written && file.sync();
  const bool closed = file.close();
  if (!synced || !closed) LOG_ERR("AI", "Could not persist recap metadata");
  return synced && closed;
}

bool readJobFile(const char* path, Job& job) {
  FsFile file;
  if (!Storage.openFileForRead("AI", path, file)) return false;
  uint32_t magic = 0;
  uint32_t version = 0;
  uint8_t scope = 0;
  uint8_t detail = 0;
  uint8_t limited = 0;
  const bool read =
      serialization::tryReadPod(file, magic) && magic == JOB_MAGIC && serialization::tryReadPod(file, version) &&
      version == PROMPT_VERSION && readBoundedString(file, job.bookPath, MAX_PATH_BYTES) &&
      readBoundedString(file, job.provider, 16) && readBoundedString(file, job.model, MAX_MODEL_BYTES) &&
      serialization::tryReadPod(file, scope) && serialization::tryReadPod(file, detail) &&
      serialization::tryReadPod(file, job.options.pages) && serialization::tryReadPod(file, job.cutoff.spine) &&
      serialization::tryReadPod(file, job.cutoff.page) && serialization::tryReadPod(file, job.start.spine) &&
      serialization::tryReadPod(file, job.start.page) && serialization::tryReadPod(file, job.end.spine) &&
      serialization::tryReadPod(file, job.end.page) && serialization::tryReadPod(file, job.layoutSignature) &&
      serialization::tryReadPod(file, job.targetBytes) && serialization::tryReadPod(file, job.contextBytes) &&
      serialization::tryReadPod(file, job.sourceBytes) && serialization::tryReadPod(file, limited) &&
      serialization::tryReadPod(file, job.readerOrientation) &&
      file.read(job.contentKey, sizeof(job.contentKey)) == static_cast<int>(sizeof(job.contentKey)) &&
      file.position() == file.size();
  file.close();
  job.options.scope = static_cast<Scope>(scope);
  job.options.detail = static_cast<Detail>(detail);
  job.contextLimited = limited == 1;
  if (!read || limited > 1 || !validJob(job)) {
    LOG_ERR("AI", "Invalid recap metadata");
    return false;
  }
  return true;
}

bool replaceFile(const char* temporary, const char* destination) {
  if (Storage.exists(destination) && !Storage.remove(destination)) return false;
  if (Storage.rename(temporary, destination)) return true;
  LOG_ERR("AI", "Could not publish recap file");
  return false;
}

bool sourceFingerprint(char (&key)[17], uint32_t& length) {
  FsFile file;
  if (!Storage.openFileForRead("AI", SOURCE_PATH, file)) return false;
  const size_t size = file.size();
  if (size == 0 || size > MAX_SOURCE_BYTES) {
    file.close();
    return false;
  }
  uint64_t hash = HASH_INITIAL;
  uint8_t buffer[128];
  size_t consumed = 0;
  while (consumed < size) {
    const size_t wanted = std::min(sizeof(buffer), size - consumed);
    const int count = file.read(buffer, wanted);
    if (count != static_cast<int>(wanted)) {
      file.close();
      return false;
    }
    hashBytes(hash, buffer, wanted);
    consumed += wanted;
  }
  file.close();
  length = static_cast<uint32_t>(size);
  hashToText(hash, key);
  return true;
}

std::string cacheStem(const Job& job) {
  uint64_t hash = HASH_INITIAL;
  hashBytes(hash, &PROMPT_VERSION, sizeof(PROMPT_VERSION));
  hashString(hash, job.bookPath);
  hashString(hash, job.provider);
  hashString(hash, job.model);
  hashBytes(hash, job.contentKey, 16);
  const uint8_t detail = static_cast<uint8_t>(job.options.detail);
  const uint8_t scope = static_cast<uint8_t>(job.options.scope);
  hashBytes(hash, &detail, sizeof(detail));
  hashBytes(hash, &scope, sizeof(scope));
  hashBytes(hash, &job.options.pages, sizeof(job.options.pages));
  hashBytes(hash, &job.cutoff.spine, sizeof(job.cutoff.spine));
  hashBytes(hash, &job.cutoff.page, sizeof(job.cutoff.page));
  hashBytes(hash, &job.layoutSignature, sizeof(job.layoutSignature));
  char key[17];
  hashToText(hash, key);
  std::string stem;
  stem.reserve(sizeof(CACHE_DIRECTORY) + sizeof(key));
  stem.assign(CACHE_DIRECTORY);
  stem += '/';
  stem += key;
  return stem;
}

bool sameRequest(const Job& a, const Job& b) {
  return a.bookPath == b.bookPath && a.provider == b.provider && a.model == b.model &&
         a.options.scope == b.options.scope && a.options.detail == b.options.detail &&
         a.options.pages == b.options.pages && samePoint(a.cutoff, b.cutoff) && samePoint(a.start, b.start) &&
         samePoint(a.end, b.end) && a.layoutSignature == b.layoutSignature && a.targetBytes == b.targetBytes &&
         a.contextBytes == b.contextBytes && a.sourceBytes == b.sourceBytes && a.contextLimited == b.contextLimited &&
         std::strcmp(a.contentKey, b.contentKey) == 0;
}

bool filesEqual(const char* a, const char* b, const size_t length) {
  FsFile left;
  FsFile right;
  if (!Storage.openFileForRead("AI", a, left)) return false;
  if (!Storage.openFileForRead("AI", b, right)) {
    left.close();
    return false;
  }
  bool equal = left.size() == length && right.size() == length;
  uint8_t leftBytes[96];
  uint8_t rightBytes[96];
  for (size_t position = 0; equal && position < length;) {
    const size_t wanted = std::min(sizeof(leftBytes), length - position);
    equal = left.read(leftBytes, wanted) == static_cast<int>(wanted) &&
            right.read(rightBytes, wanted) == static_cast<int>(wanted) &&
            std::memcmp(leftBytes, rightBytes, wanted) == 0;
    position += wanted;
  }
  left.close();
  right.close();
  return equal;
}

bool copyFile(const char* source, const char* destination, const size_t maximum, const size_t expected = 0) {
  FsFile input;
  FsFile output;
  if (!Storage.openFileForRead("AI", source, input)) return false;
  const size_t size = input.size();
  if (size == 0 || size > maximum || (expected != 0 && size != expected) ||
      !Storage.openFileForWrite("AI", destination, output)) {
    input.close();
    return false;
  }
  uint8_t buffer[128];
  bool success = true;
  for (size_t position = 0; position < size;) {
    const size_t wanted = std::min(sizeof(buffer), size - position);
    if (input.read(buffer, wanted) != static_cast<int>(wanted) || output.write(buffer, wanted) != wanted) {
      success = false;
      break;
    }
    position += wanted;
  }
  input.close();
  const bool synced = success && output.sync();
  const bool closed = output.close();
  return synced && closed;
}
}  // namespace

bool validJob(const Job& job) {
  return validString(job.bookPath, MAX_PATH_BYTES) && job.bookPath.front() == '/' &&
         (job.provider == "openai" || job.provider == "anthropic") && validString(job.model, MAX_MODEL_BYTES) &&
         static_cast<uint8_t>(job.options.scope) <= static_cast<uint8_t>(Scope::SoFar) &&
         static_cast<uint8_t>(job.options.detail) <= static_cast<uint8_t>(Detail::Detailed) &&
         (job.options.scope != Scope::LastPages || (job.options.pages > 0 && job.options.pages <= 1000)) &&
         !before(job.end, job.start) && !before(job.cutoff, job.end) && job.layoutSignature != 0 &&
         job.targetBytes > 0 && job.targetBytes <= MAX_TARGET_BYTES && job.contextBytes <= MAX_CONTEXT_BYTES &&
         job.sourceBytes >= job.targetBytes + job.contextBytes && job.sourceBytes <= MAX_SOURCE_BYTES &&
         validKey(job.contentKey);
}

bool sealSource(Job& job) {
  if (!sourceFingerprint(job.contentKey, job.sourceBytes)) {
    LOG_ERR("AI", "Could not fingerprint recap source");
    return false;
  }
  return validJob(job);
}

bool validateSource(const Job& job) {
  char key[17];
  uint32_t length = 0;
  const bool valid = validJob(job) && sourceFingerprint(key, length) && length == job.sourceBytes &&
                     std::strcmp(key, job.contentKey) == 0;
  if (!valid) LOG_ERR("AI", "Recap source does not match the saved request");
  return valid;
}

bool saveJob(const Job& job) {
  if (!validJob(job) || !Storage.ensureDirectoryExists(DIRECTORY) || !writeJobFile(JOB_TEMP_PATH, job) ||
      !replaceFile(JOB_TEMP_PATH, JOB_PATH)) {
    LOG_ERR("AI", "Could not save recap request");
    return false;
  }
  return true;
}

bool loadJob(Job& job) { return readJobFile(JOB_PATH, job) && validateSource(job); }

void clearPendingJob() {
  // Removing the metadata first makes interrupted cleanup fail closed.
  const char* paths[] = {JOB_PATH,
                         JOB_TEMP_PATH,
                         SOURCE_PATH,
                         RESPONSE_PATH,
                         "/.crosspoint/ai/recap-source.tmp",
                         "/.crosspoint/ai/recap-target.tmp"};
  for (const char* path : paths) {
    if (Storage.exists(path) && !Storage.remove(path)) LOG_ERR("AI", "Could not clear pending recap file");
  }
}

bool findCachedRecap(const Job& job, std::string& recapPath) {
  recapPath.clear();
  if (!validateSource(job)) return false;
  const std::string stem = cacheStem(job);
  Job cached;
  if (!Storage.exists((stem + ".job").c_str()) || !readJobFile((stem + ".job").c_str(), cached) ||
      !sameRequest(job, cached) || !filesEqual(SOURCE_PATH, (stem + ".source").c_str(), job.sourceBytes)) {
    return false;
  }
  FsFile response;
  if (!Storage.openFileForRead("AI", (stem + ".txt").c_str(), response)) return false;
  const size_t bytes = response.size();
  response.close();
  if (bytes == 0 || bytes > MAX_RECAP_BYTES) return false;
  recapPath = stem + ".txt";
  return true;
}

bool storeCachedRecap(const Job& job, const char* responsePath, std::string& recapPath) {
  recapPath.clear();
  if (!responsePath || !validateSource(job) || !Storage.ensureDirectoryExists(CACHE_DIRECTORY)) return false;
  const std::string stem = cacheStem(job);
  // Publish metadata last. An interrupted replacement cannot make an old
  // response eligible under the new source's metadata.
  const std::string metadataPath = stem + ".job";
  if (Storage.exists(metadataPath.c_str()) && !Storage.remove(metadataPath.c_str())) return false;
  bool success = copyFile(SOURCE_PATH, (stem + ".source.tmp").c_str(), MAX_SOURCE_BYTES, job.sourceBytes) &&
                 copyFile(responsePath, (stem + ".txt.tmp").c_str(), MAX_RECAP_BYTES) &&
                 writeJobFile((stem + ".job.tmp").c_str(), job) &&
                 replaceFile((stem + ".source.tmp").c_str(), (stem + ".source").c_str()) &&
                 replaceFile((stem + ".txt.tmp").c_str(), (stem + ".txt").c_str()) &&
                 replaceFile((stem + ".job.tmp").c_str(), metadataPath.c_str());
  if (!success) {
    LOG_ERR("AI", "Could not cache completed recap");
    return false;
  }
  recapPath = stem + ".txt";
  return true;
}

}  // namespace AiRecap
