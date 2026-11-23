#include "AiRecapSource.h"

#include <BufferedFile.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "Epub/Page.h"
#include "Epub/ReaderRenderSpec.h"
#include "Epub/Section.h"
#include "Epub/blocks/TextBlock.h"

namespace AiRecap {
namespace {
constexpr char DIRECTORY[] = "/.crosspoint/ai";
constexpr char TARGET_TEMP[] = "/.crosspoint/ai/recap-target.tmp";
constexpr char SOURCE_TEMP[] = "/.crosspoint/ai/recap-source.tmp";
constexpr char CONTEXT_HEADING[] = "EARLIER CONTEXT\n";
constexpr char LIMITED_CONTEXT[] = "(Earlier context is limited; do not assume complete coverage.)\n";
constexpr char NO_CONTEXT[] = "(No earlier context included.)\n";
constexpr char TARGET_HEADING[] = "\nTARGET PASSAGES\n";
constexpr size_t MAX_CAPTURE_PAGES = 2000;
constexpr size_t MAX_CONTEXT_PAGES = 128;

struct Cancellation {
  bool (*callback)(void*) = nullptr;
  void* context = nullptr;
  bool requested() const { return callback && callback(context); }
};

// A single existing page model is loaded at a time. Previous sections are
// replaced one at a time rather than retaining a chapter or book in memory.
class SectionAccess {
 public:
  SectionAccess(Epub& epub, Section& current, GfxRenderer& renderer, const ReaderRenderSpec& spec,
                const uint16_t currentSpine, const Cancellation cancellation)
      : epub(epub),
        current(current),
        renderer(renderer),
        spec(spec),
        currentSpine(currentSpine),
        cancellation(cancellation) {}

  Section* get(const uint16_t spine) {
    if (cancellation.requested()) {
      error = CaptureError::Cancelled;
      return nullptr;
    }
    if (spine > currentSpine) {
      error = CaptureError::InvalidRange;
      return nullptr;
    }
    if (spine == currentSpine) return &current;
    if (previous && previousSpine == spine) return previous.get();
    previous.reset();
    // One Section controller, not source text, lives on the heap because it
    // owns parser/cache state and outlives individual page reads.
    previous = makeUniqueNoThrow<Section>(epub, spine, renderer);
    if (!previous) {
      LOG_ERR("AI", "Could not allocate prior-section controller (%u bytes)", static_cast<unsigned>(sizeof(Section)));
      error = CaptureError::OutOfMemory;
      return nullptr;
    }
    previousSpine = spine;
    if (!previous->loadSectionFile(spec) || previous->isPartial() || previous->isBuilding()) {
      // Both parsers would share Epub's mutable CSS cache. Rebuilding an older
      // section while the live reader is still indexing could clear that cache
      // underneath the reader. Retry once indexing finishes instead.
      if (current.isBuilding()) {
        error = CaptureError::SourceUnavailable;
        LOG_ERR("AI", "Prior-page indexing must wait for the current section");
        return nullptr;
      }
      bool cancelled = false;
      SectionBuildOptions build;
      build.shouldCancel = cancellation.callback;
      build.cancelContext = cancellation.context;
      build.cancellationObserved = &cancelled;
      if (!previous->createSectionFile(spec, nullptr, nullptr, nullptr, build)) {
        error = cancelled || cancellation.requested() ? CaptureError::Cancelled : CaptureError::SourceUnavailable;
        LOG_ERR("AI", "Could not prepare previous pages for recap");
        return nullptr;
      }
    }
    return previous.get();
  }

  bool previousPoint(Point& point, bool& exists) {
    exists = false;
    if (point.page > 0) {
      --point.page;
      exists = true;
      return true;
    }
    while (point.spine > 0) {
      --point.spine;
      const Section* section = get(point.spine);
      if (!section) return false;
      if (section->pageCount == 0) continue;
      point.page = section->pageCount - 1;
      exists = true;
      return true;
    }
    return true;
  }

  CaptureError error = CaptureError::None;

 private:
  Epub& epub;
  Section& current;
  GfxRenderer& renderer;
  const ReaderRenderSpec& spec;
  uint16_t currentSpine;
  Cancellation cancellation;
  uint16_t previousSpine = 0;
  std::unique_ptr<Section> previous;
};

struct TextSink {
  serialization::BufferedFileWriter* file;
  GfxRenderer& renderer;
  int fontId;
  size_t limit;
  Cancellation cancellation;
  size_t count = 0;
  bool hasText = false;
  bool joinHyphen = false;
  CaptureError error = CaptureError::None;

  bool write(const char* text, const size_t length) {
    if (length > limit - count) {
      error = CaptureError::TooLarge;
      return false;
    }
    if (file) file->write(text, length);
    count += length;
    return true;
  }

  bool line(const PageTextLine& line) {
    if (cancellation.requested()) {
      error = CaptureError::Cancelled;
      return false;
    }
    if (!line.block || !line.block->valid()) {
      error = CaptureError::SourceUnavailable;
      return false;
    }
    const TextBlock& block = *line.block;
    if (line.isTableText && (line.yPos < line.clipY || line.yPos + line.lineHeight > line.clipY + line.clipHeight)) {
      // A serialized table cell can contain text clipped out by the renderer.
      // Text that the reader cannot see is never eligible source material.
      joinHyphen = false;
      return true;
    }
    bool firstVisibleWord = true;
    for (uint16_t i = 0; i < block.wordCount(); ++i) {
      const char* text = block.wordText(i);
      size_t length = block.wordTextLen(i);
      if (length == 0) continue;
      if (line.isTableText) {
        const int x = line.xPos + block.wordXpos(i);
        const int font = block.resolvedFontId(renderer, fontId);
        const int width = renderer.getTextAdvanceX(font, text, block.wordStyle(i));
        if (x < line.clipX || x + width > line.clipX + line.clipWidth) {
          joinHyphen = false;
          continue;
        }
      }
      const bool insertedHyphen = block.wordEndsWithInsertedHyphen(i) && text[length - 1] == '-';
      if (insertedHyphen) --length;
      if (length == 0) continue;
      if (hasText && !joinHyphen) {
        if (firstVisibleWord) {
          if (!write("\n", 1)) return false;
        } else if (block.wordHasSpaceBefore(i) && !write(" ", 1)) {
          return false;
        }
      }
      if (!write(text, length)) return false;
      hasText = true;
      joinHyphen = insertedHyphen;
      firstVisibleWord = false;
    }
    return true;
  }
};

bool visitLine(const PageTextLine& line, void* context) { return static_cast<TextSink*>(context)->line(line); }

CaptureError appendPage(SectionAccess& access, const Point point, TextSink& sink) {
  Section* section = access.get(point.spine);
  if (!section) return access.error;
  if (point.page >= section->pageCount) return CaptureError::SourceUnavailable;
  // The layout engine owns the one-page allocation and checks failure. It is
  // discarded before the next page, so request length adds no retained RAM.
  const auto page = section->loadPage(point.page);
  if (!page) return CaptureError::SourceUnavailable;
  if (!page->forEachTextLine(visitLine, &sink)) {
    return sink.error == CaptureError::None ? CaptureError::SourceUnavailable : sink.error;
  }
  return CaptureError::None;
}

CaptureError appendRange(SectionAccess& access, const Point start, const Point end, TextSink& sink) {
  if (before(end, start)) return CaptureError::InvalidRange;
  size_t visitedPages = 0;
  for (uint32_t spine = start.spine; spine <= end.spine; ++spine) {
    Section* section = access.get(static_cast<uint16_t>(spine));
    if (!section) return access.error;
    const uint32_t begin = spine == start.spine ? start.page : 0;
    const uint32_t stop = spine == end.spine ? static_cast<uint32_t>(end.page) + 1 : section->pageCount;
    if (begin > stop || stop > section->pageCount) return CaptureError::SourceUnavailable;
    for (uint32_t page = begin; page < stop; ++page) {
      if (++visitedPages > MAX_CAPTURE_PAGES) return CaptureError::TooLarge;
      const CaptureError error = appendPage(access, {static_cast<uint16_t>(spine), static_cast<uint16_t>(page)}, sink);
      if (error != CaptureError::None) return error;
    }
  }
  return CaptureError::None;
}

CaptureError selectTarget(SectionAccess& access, const Point cutoff, const int currentChapterStart,
                          const int previousChapterStart, const Options& options, Point& start, Point& end) {
  start = cutoff;
  end = cutoff;
  if (options.scope == Scope::PreviousChapter) {
    if (currentChapterStart <= 0 || previousChapterStart < 0 || previousChapterStart >= currentChapterStart ||
        currentChapterStart > cutoff.spine) {
      return CaptureError::NoPreviousChapter;
    }
    start = {static_cast<uint16_t>(previousChapterStart), 0};
    end = {static_cast<uint16_t>(currentChapterStart), 0};
    bool exists = false;
    if (!access.previousPoint(end, exists)) return access.error;
    if (!exists || before(end, start)) return CaptureError::EmptyText;
  } else if (options.scope == Scope::LastPages) {
    if (options.pages == 0 || options.pages > 1000) return CaptureError::InvalidRange;
    for (uint16_t count = 1; count < options.pages; ++count) {
      Point previous = start;
      bool exists = false;
      if (!access.previousPoint(previous, exists)) return access.error;
      if (!exists) break;
      start = previous;
    }
  } else if (options.scope == Scope::SoFar) {
    start = {0, 0};
  } else {
    return CaptureError::InvalidRange;
  }
  return before(cutoff, end) ? CaptureError::InvalidRange : CaptureError::None;
}

CaptureError selectContext(SectionAccess& access, const Point targetStart, GfxRenderer& renderer, const int fontId,
                           const Cancellation cancellation, Point& start, Point& end, bool& included, bool& limited) {
  end = targetStart;
  bool exists = false;
  if (!access.previousPoint(end, exists)) return access.error;
  included = false;
  limited = false;
  if (!exists) return CaptureError::None;
  Point candidate = end;
  size_t bytes = 0;
  for (size_t pages = 0; pages < MAX_CONTEXT_PAGES; ++pages) {
    // Count a complete page before including it. The two-byte allowance per
    // page covers joins/newlines when pages are emitted in reading order.
    TextSink counter{nullptr, renderer, fontId, MAX_CONTEXT_BYTES, cancellation};
    const CaptureError error = appendPage(access, candidate, counter);
    if (error != CaptureError::None && error != CaptureError::TooLarge) return error;
    if (error == CaptureError::TooLarge || counter.count + 2 > MAX_CONTEXT_BYTES - bytes) {
      limited = true;
      return CaptureError::None;
    }
    bytes += counter.count + 2;
    start = candidate;
    included = true;
    if (!access.previousPoint(candidate, exists)) return access.error;
    if (!exists) return CaptureError::None;
  }
  limited = true;
  return CaptureError::None;
}

bool appendFile(FsFile& output, const char* path, const size_t expected) {
  FsFile input;
  if (!Storage.openFileForRead("AI", path, input)) return false;
  if (input.size() != expected) {
    input.close();
    return false;
  }
  uint8_t buffer[128];
  bool success = true;
  for (size_t position = 0; position < expected;) {
    const size_t count = std::min(sizeof(buffer), expected - position);
    if (input.read(buffer, count) != static_cast<int>(count) || output.write(buffer, count) != count) {
      success = false;
      break;
    }
    position += count;
  }
  input.close();
  return success;
}

bool literal(FsFile& output, const char* text) {
  const size_t length = std::strlen(text);
  return output.write(text, length) == length;
}

CaptureError fail(const CaptureError error) {
  LOG_ERR("AI", "Recap capture failed (%u)", static_cast<unsigned>(error));
  clearPendingJob();
  return error;
}
}  // namespace

CaptureError capture(Epub& epub, Section& currentSection, GfxRenderer& renderer, const ReaderRenderSpec& spec,
                     const uint16_t currentSpine, const uint16_t currentPage, const int currentChapterStart,
                     const int previousChapterStart, const Options& options, Job& job, bool (*shouldCancel)(void*),
                     void* cancelContext) {
  clearPendingJob();
  if (currentSpine >= epub.getSpineItemsCount() || currentPage >= currentSection.pageCount ||
      currentSection.currentPage != currentPage || spec.viewportWidth == 0 || spec.viewportHeight == 0 ||
      static_cast<uint8_t>(options.detail) > static_cast<uint8_t>(Detail::Detailed)) {
    return fail(CaptureError::InvalidRange);
  }
  if (!Storage.ensureDirectoryExists(DIRECTORY)) return fail(CaptureError::StorageFailure);
  const Cancellation cancellation{shouldCancel, cancelContext};
  if (cancellation.requested()) return fail(CaptureError::Cancelled);
  SectionAccess access(epub, currentSection, renderer, spec, currentSpine, cancellation);
  job.bookPath = epub.getPath();
  job.options = options;
  job.cutoff = {currentSpine, currentPage};
  job.layoutSignature = readerRenderSpecSignature(spec);
  job.targetBytes = 0;
  job.contextBytes = 0;
  job.sourceBytes = 0;
  job.contextLimited = false;
  job.contentKey[0] = '\0';
  CaptureError error =
      selectTarget(access, job.cutoff, currentChapterStart, previousChapterStart, options, job.start, job.end);
  if (error != CaptureError::None) return fail(error);

  FsFile target;
  if (!Storage.openFileForWrite("AI", TARGET_TEMP, target)) return fail(CaptureError::StorageFailure);
  bool targetHasText = false;
  bool targetWritten = false;
  {
    // One 1 KiB buffer batches the interleaved page reads and word writes. The
    // existing wrapper falls back to correct unbuffered I/O if allocation fails.
    serialization::BufferedFileWriter writer(target, 1024);
    TextSink targetSink{&writer, renderer, spec.fontId, MAX_TARGET_BYTES, cancellation};
    error = appendRange(access, job.start, job.end, targetSink);
    targetWritten = writer.flush();
    targetHasText = targetSink.hasText;
    job.targetBytes = static_cast<uint32_t>(targetSink.count);
  }
  const bool targetSynced = error == CaptureError::None && targetWritten && target.sync();
  const bool targetClosed = target.close();
  if (error != CaptureError::None) return fail(error);
  if (!targetSynced || !targetClosed) return fail(CaptureError::StorageFailure);
  if (!targetHasText) return fail(CaptureError::EmptyText);

  Point contextStart;
  Point contextEnd;
  bool contextIncluded = false;
  error = selectContext(access, job.start, renderer, spec.fontId, cancellation, contextStart, contextEnd,
                        contextIncluded, job.contextLimited);
  if (error != CaptureError::None) return fail(error);

  FsFile source;
  if (!Storage.openFileForWrite("AI", SOURCE_TEMP, source)) return fail(CaptureError::StorageFailure);
  bool sourceWritten = literal(source, CONTEXT_HEADING);
  if (sourceWritten && job.contextLimited) sourceWritten = literal(source, LIMITED_CONTEXT);
  if (sourceWritten && contextIncluded) {
    // Reuse the same bounded batching strategy; the target buffer is freed.
    serialization::BufferedFileWriter writer(source, 1024);
    TextSink contextSink{&writer, renderer, spec.fontId, MAX_CONTEXT_BYTES, cancellation};
    error = appendRange(access, contextStart, contextEnd, contextSink);
    sourceWritten = error == CaptureError::None && writer.flush();
    job.contextBytes = static_cast<uint32_t>(contextSink.count);
  } else if (sourceWritten) {
    sourceWritten = literal(source, NO_CONTEXT);
  }
  sourceWritten = sourceWritten && literal(source, TARGET_HEADING) && appendFile(source, TARGET_TEMP, job.targetBytes);
  const bool sourceSynced = sourceWritten && source.sync();
  const bool sourceClosed = source.close();
  if (error != CaptureError::None) return fail(error);
  if (!sourceSynced || !sourceClosed || !Storage.rename(SOURCE_TEMP, SOURCE_PATH))
    return fail(CaptureError::StorageFailure);
  Storage.remove(TARGET_TEMP);
  if (cancellation.requested()) return fail(CaptureError::Cancelled);
  if (!sealSource(job) || !saveJob(job)) return fail(CaptureError::StorageFailure);
  return CaptureError::None;
}

}  // namespace AiRecap
