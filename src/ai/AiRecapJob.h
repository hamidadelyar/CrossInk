#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace AiRecap {

enum class Scope : uint8_t { PreviousChapter, LastPages, SoFar };
enum class Detail : uint8_t { Short, Detailed };

struct Options {
  Scope scope = Scope::LastPages;
  uint16_t pages = 10;
  Detail detail = Detail::Short;
};

struct Point {
  uint16_t spine = 0;
  uint16_t page = 0;
};

constexpr bool before(const Point a, const Point b) {
  return a.spine < b.spine || (a.spine == b.spine && a.page < b.page);
}
constexpr bool samePoint(const Point a, const Point b) { return a.spine == b.spine && a.page == b.page; }

constexpr uint32_t PROMPT_VERSION = 1;
constexpr size_t MAX_TARGET_BYTES = 64U * 1024U;
constexpr size_t MAX_CONTEXT_BYTES = 8U * 1024U;
constexpr size_t MAX_SOURCE_BYTES = MAX_TARGET_BYTES + MAX_CONTEXT_BYTES + 256U;
constexpr size_t MAX_RECAP_BYTES = 16U * 1024U;
constexpr char SOURCE_PATH[] = "/.crosspoint/ai/recap-source.txt";
constexpr char RESPONSE_PATH[] = "/.crosspoint/ai/recap-response.txt";

// No credentials or book title/author are written into this record or the source
// payload. The model and provider are frozen with the request across reboot.
struct Job {
  std::string bookPath;
  std::string provider;
  std::string model;
  Options options;
  Point cutoff;
  Point start;
  Point end;
  uint32_t layoutSignature = 0;
  uint32_t targetBytes = 0;
  uint32_t contextBytes = 0;
  uint32_t sourceBytes = 0;
  bool contextLimited = false;
  uint8_t readerOrientation = UINT8_MAX;
  char contentKey[17] = {};
};

bool validJob(const Job& job);
bool saveJob(const Job& job);
bool loadJob(Job& job);
bool validateSource(const Job& job);
// Used by capture after the complete source file is published, before saveJob.
bool sealSource(Job& job);
void clearPendingJob();

// Bucket names use a small rolling fingerprint, but a cache hit additionally
// compares every source byte and the full request metadata. A hash collision
// therefore cannot reuse a recap from a different reading cutoff or model.
bool findCachedRecap(const Job& job, std::string& recapPath);
bool storeCachedRecap(const Job& job, const char* responsePath, std::string& recapPath);

}  // namespace AiRecap
