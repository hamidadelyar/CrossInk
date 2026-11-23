#pragma once

#include "AiRecapJob.h"

class Epub;
class Section;
class GfxRenderer;
struct ReaderRenderSpec;

namespace AiRecap {

enum class CaptureError : uint8_t {
  None,
  InvalidRange,
  NoPreviousChapter,
  SourceUnavailable,
  TooLarge,
  EmptyText,
  StorageFailure,
  Cancelled,
  OutOfMemory,
};

// Call with the reader's RenderLock held and reject footnote previews first.
// currentPage is inclusive. The current Section is never rebuilt or read beyond
// that frozen page; prior sections use the identical layout and finish indexing
// before they may be used as completed chapters. currentChapterStart and
// previousChapterStart are spine indices from the reader's chapter grouping.
//
// Set job.provider/model/readerOrientation before calling. A successful capture
// fills the remaining fields and atomically publishes the source plus pending
// job. Failures invalidate the pending job and never publish partial text.
CaptureError capture(Epub& epub, Section& currentSection, GfxRenderer& renderer, const ReaderRenderSpec& spec,
                     uint16_t currentSpine, uint16_t currentPage, int currentChapterStart, int previousChapterStart,
                     const Options& options, Job& job, bool (*shouldCancel)(void*) = nullptr,
                     void* cancelContext = nullptr);

}  // namespace AiRecap
