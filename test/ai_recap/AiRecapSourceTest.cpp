#include <Epub.h>
#include <Epub/ReaderRenderSpec.h>
#include <Epub/Section.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "ai/AiRecapJob.h"
#include "ai/AiRecapSource.h"

namespace {
using namespace AiRecap;
constexpr char JOB_PATH[] = "/.crosspoint/ai/recap-job.bin";

FakeSection chapter(std::initializer_list<const char*> pages) {
  FakeSection result;
  for (const char* page : pages) result.pages.emplace_back(page);
  return result;
}

std::string largeText(const size_t words) {
  std::string result;
  for (size_t i = 0; i < words; ++i) result += "abcdefghij ";
  return result;
}

class RecapSourceTest : public testing::Test {
 protected:
  Epub epub;
  GfxRenderer renderer;
  ReaderRenderSpec spec;
  Job job;

  void SetUp() override {
    Storage.fs = {};
    spec.viewportWidth = 100;
    spec.viewportHeight = 100;
    spec.fontId = 1;
    job.provider = "openai";
    job.model = "test-model";
  }

  CaptureError run(const Options options, const uint16_t spine, const uint16_t page, const int currentChapterStart = -1,
                   const int previousChapterStart = -1, bool (*cancel)(void*) = nullptr,
                   void* cancelContext = nullptr) {
    Section current(epub, spine, renderer);
    current.currentPage = page;
    return capture(epub, current, renderer, spec, spine, page, currentChapterStart, previousChapterStart, options, job,
                   cancel, cancelContext);
  }

  std::string source() const { return Storage.fs.files.at(SOURCE_PATH); }
  std::string target() const {
    const std::string text = source();
    const std::string marker = "TARGET PASSAGES\n";
    return text.substr(text.find(marker) + marker.size());
  }
  std::string cache() {
    Storage.fs.files[RESPONSE_PATH] = "A factual recap.";
    std::string path;
    EXPECT_TRUE(storeCachedRecap(job, RESPONSE_PATH, path));
    return path;
  }
};

TEST_F(RecapSourceTest, IncludesCurrentPageAndNeverReadsFollowingPage) {
  epub.sections = {chapter({"Earlier context."}), chapter({"First target.", "Current target.", "FUTURE TWIST."})};
  ASSERT_EQ(run({Scope::LastPages, 2, Detail::Short}, 1, 1), CaptureError::None);
  EXPECT_EQ(target(), "First target.\nCurrent target.");
  EXPECT_NE(source().find("Earlier context."), std::string::npos);
  EXPECT_EQ(source().find("FUTURE"), std::string::npos);
  EXPECT_TRUE(std::all_of(epub.readPages.begin(), epub.readPages.end(),
                          [](const auto& position) { return position.first < 1 || position.second <= 1; }));
  EXPECT_TRUE(samePoint(job.cutoff, {1, 1}));
  Job restored;
  ASSERT_TRUE(loadJob(restored));
  EXPECT_TRUE(samePoint(restored.cutoff, job.cutoff));
}

TEST_F(RecapSourceTest, LastPagesCrossesSectionsAndClampsAtStartOfBook) {
  epub.sections = {chapter({"A", "B"}), chapter({}), chapter({"C", "D", "UNREAD"})};
  ASSERT_EQ(run({Scope::LastPages, 3, Detail::Short}, 2, 1), CaptureError::None);
  EXPECT_EQ(target(), "B\nC\nD");
  EXPECT_TRUE(samePoint(job.start, {0, 1}));
  ASSERT_EQ(run({Scope::LastPages, 100, Detail::Short}, 2, 1), CaptureError::None);
  EXPECT_EQ(target(), "A\nB\nC\nD");
  EXPECT_TRUE(samePoint(job.start, {0, 0}));
}

TEST_F(RecapSourceTest, PreviousChapterIncludesGroupedSpinesButNoCurrentChapterContext) {
  epub.sections = {chapter({"Chapter one part A."}), chapter({"Chapter one part B."}),
                   chapter({"CURRENT CHAPTER START"}), chapter({"CURRENT PAGE", "FUTURE"})};
  ASSERT_EQ(run({Scope::PreviousChapter, 10, Detail::Short}, 3, 0, 2, 0), CaptureError::None);
  EXPECT_EQ(target(), "Chapter one part A.\nChapter one part B.");
  EXPECT_EQ(source().find("CURRENT"), std::string::npos);
  EXPECT_EQ(source().find("FUTURE"), std::string::npos);
  EXPECT_TRUE(samePoint(job.end, {1, 0}));
}

TEST_F(RecapSourceTest, PreviousChapterAtStartFailsAndRemovesOldPendingJob) {
  epub.sections = {chapter({"Opening page."})};
  ASSERT_EQ(run({Scope::LastPages, 1, Detail::Short}, 0, 0), CaptureError::None);
  ASSERT_TRUE(Storage.exists(JOB_PATH));
  EXPECT_EQ(run({Scope::PreviousChapter, 1, Detail::Short}, 0, 0, 0, -1), CaptureError::NoPreviousChapter);
  EXPECT_FALSE(Storage.exists(JOB_PATH));
  EXPECT_FALSE(Storage.exists(SOURCE_PATH));
}

TEST_F(RecapSourceTest, MovedReadingPositionFailsClosed) {
  epub.sections = {chapter({"Read", "Unread"})};
  Section current(epub, 0, renderer);
  current.currentPage = 1;
  EXPECT_EQ(capture(epub, current, renderer, spec, 0, 0, 0, -1, {}, job), CaptureError::InvalidRange);
  EXPECT_FALSE(Storage.exists(SOURCE_PATH));
  EXPECT_TRUE(epub.readPages.empty());
}

TEST_F(RecapSourceTest, CurrentPartialSectionIsNotRebuiltOrExtended) {
  epub.sections = {chapter({"First", "Current", "Unread"})};
  epub.sections[0].partial = true;
  ASSERT_EQ(run({Scope::SoFar, 10, Detail::Short}, 0, 1), CaptureError::None);
  EXPECT_EQ(target(), "First\nCurrent");
  EXPECT_EQ(epub.sections[0].builds, 0U);
  EXPECT_EQ(epub.readPages.size(), 2U);
}

TEST_F(RecapSourceTest, PriorPartialSectionMustFinishBeforeUse) {
  epub.sections = {chapter({"Earlier first", "Earlier last"}), chapter({"Current"})};
  epub.sections[0].partial = true;
  ASSERT_EQ(run({Scope::LastPages, 2, Detail::Short}, 1, 0), CaptureError::None);
  EXPECT_EQ(target(), "Earlier last\nCurrent");
  EXPECT_EQ(epub.sections[0].builds, 1U);
  epub.sections[0].partial = true;
  epub.sections[0].canBuild = false;
  EXPECT_EQ(run({Scope::LastPages, 2, Detail::Short}, 1, 0), CaptureError::SourceUnavailable);
  EXPECT_FALSE(Storage.exists(SOURCE_PATH));
}

TEST_F(RecapSourceTest, PriorRebuildCannotMutateActiveCurrentParserCss) {
  epub.sections = {chapter({"Earlier"}), chapter({"Current", "Unread"})};
  epub.sections[0].cached = false;
  epub.sections[1].building = true;
  EXPECT_EQ(run({Scope::LastPages, 2, Detail::Short}, 1, 0), CaptureError::SourceUnavailable);
  EXPECT_EQ(epub.sections[0].builds, 0U);
  EXPECT_EQ(epub.sections[1].builds, 0U);
  EXPECT_FALSE(Storage.exists(JOB_PATH));
  epub.sections[0].cached = true;
  ASSERT_EQ(run({Scope::LastPages, 2, Detail::Short}, 1, 0), CaptureError::None);
  EXPECT_EQ(target(), "Earlier\nCurrent");
}

TEST_F(RecapSourceTest, MissingPageDoesNotPublishPartialRequest) {
  epub.sections = {chapter({"Earlier", "Current"})};
  epub.sections[0].failingPage = 0;
  EXPECT_EQ(run({Scope::SoFar, 10, Detail::Short}, 0, 1), CaptureError::SourceUnavailable);
  EXPECT_FALSE(Storage.exists(JOB_PATH));
  EXPECT_FALSE(Storage.exists(SOURCE_PATH));
}

TEST_F(RecapSourceTest, OversizedTargetFailsRatherThanSilentlyTruncating) {
  epub.sections = {chapter({"Beginning"})};
  epub.sections[0].pages.emplace_back(largeText(7000));
  EXPECT_EQ(run({Scope::SoFar, 10, Detail::Short}, 0, 1), CaptureError::TooLarge);
  EXPECT_FALSE(Storage.exists(JOB_PATH));
  EXPECT_FALSE(Storage.exists(SOURCE_PATH));
}

TEST_F(RecapSourceTest, ContextIsBoundedAndExplicitlyMarkedAsLimited) {
  epub.sections = {chapter({"ANCIENT CONTEXT"})};
  epub.sections[0].pages.emplace_back(largeText(400));
  epub.sections[0].pages.emplace_back(largeText(400));
  epub.sections[0].pages.emplace_back("Current target.");
  ASSERT_EQ(run({Scope::LastPages, 1, Detail::Short}, 0, 3), CaptureError::None);
  EXPECT_TRUE(job.contextLimited);
  EXPECT_LE(job.contextBytes, MAX_CONTEXT_BYTES);
  EXPECT_EQ(source().find("ANCIENT"), std::string::npos);
  EXPECT_NE(source().find("Earlier context is limited"), std::string::npos);
  EXPECT_EQ(target(), "Current target.");
}

TEST_F(RecapSourceTest, CancellationNeverPublishesPendingRequest) {
  epub.sections = {chapter({"Earlier", "Current"})};
  const auto cancel = [](void*) { return true; };
  EXPECT_EQ(run({Scope::LastPages, 1, Detail::Short}, 0, 1, -1, -1, cancel), CaptureError::Cancelled);
  EXPECT_FALSE(Storage.exists(JOB_PATH));
  EXPECT_FALSE(Storage.exists(SOURCE_PATH));
}

TEST_F(RecapSourceTest, InsertedHyphenJoinsAcrossPageBoundary) {
  epub.sections = {chapter({"A multi-", "line word"})};
  epub.sections[0].pages[0].blocks[0].hyphens[1] = true;
  ASSERT_EQ(run({Scope::LastPages, 2, Detail::Short}, 0, 1), CaptureError::None);
  EXPECT_EQ(target(), "A multiline word");
}

TEST_F(RecapSourceTest, TableTextOutsideVisibleClipIsExcluded) {
  epub.sections = {chapter({"Visible"})};
  Page& page = epub.sections[0].pages[0];
  page.table = true;
  page.clipHeight = 1;
  page.blocks.emplace_back("HIDDEN SPOILER");
  ASSERT_EQ(run({Scope::LastPages, 1, Detail::Short}, 0, 0), CaptureError::None);
  EXPECT_EQ(target(), "Visible");
  EXPECT_EQ(source().find("HIDDEN"), std::string::npos);
}

TEST_F(RecapSourceTest, StorageFailureLeavesNoPublishableRequest) {
  epub.sections = {chapter({"Read page"})};
  Storage.fs.failWrite = "/.crosspoint/ai/recap-target.tmp";
  EXPECT_EQ(run({Scope::LastPages, 1, Detail::Short}, 0, 0), CaptureError::StorageFailure);
  EXPECT_FALSE(Storage.exists(JOB_PATH));
  Storage.fs.failWrite.clear();
  Storage.fs.failRename = "/.crosspoint/ai/recap-source.tmp";
  EXPECT_EQ(run({Scope::LastPages, 1, Detail::Short}, 0, 0), CaptureError::StorageFailure);
  EXPECT_FALSE(Storage.exists(JOB_PATH));
}

TEST_F(RecapSourceTest, CacheReusesOnlyExactSourceModelAndDetail) {
  epub.sections = {chapter({"First page", "Next page"})};
  ASSERT_EQ(run({Scope::LastPages, 1, Detail::Short}, 0, 0), CaptureError::None);
  const std::string cachedPath = cache();
  std::string found;
  ASSERT_TRUE(findCachedRecap(job, found));
  EXPECT_EQ(found, cachedPath);
  job.model = "different-model";
  EXPECT_FALSE(findCachedRecap(job, found));
  job.model = "test-model";
  job.options.detail = Detail::Detailed;
  EXPECT_FALSE(findCachedRecap(job, found));
  job.options.detail = Detail::Short;
  job.provider = "anthropic";
  EXPECT_FALSE(findCachedRecap(job, found));
}

TEST_F(RecapSourceTest, BacktrackingNeverLoadsRecapFromLaterPosition) {
  epub.sections = {chapter({"First page", "LATER REVEAL"})};
  ASSERT_EQ(run({Scope::SoFar, 10, Detail::Short}, 0, 1), CaptureError::None);
  cache();
  ASSERT_EQ(run({Scope::SoFar, 10, Detail::Short}, 0, 0), CaptureError::None);
  std::string found;
  EXPECT_FALSE(findCachedRecap(job, found));
  EXPECT_EQ(source().find("LATER"), std::string::npos);
}

TEST_F(RecapSourceTest, ReplacedBookAndLayoutChangesInvalidateCache) {
  epub.sections = {chapter({"Original text"})};
  ASSERT_EQ(run({Scope::LastPages, 1, Detail::Short}, 0, 0), CaptureError::None);
  cache();
  ++spec.viewportWidth;
  ASSERT_EQ(run({Scope::LastPages, 1, Detail::Short}, 0, 0), CaptureError::None);
  std::string found;
  EXPECT_FALSE(findCachedRecap(job, found));
  --spec.viewportWidth;
  epub.sections[0].pages[0] = Page("Different text");
  ASSERT_EQ(run({Scope::LastPages, 1, Detail::Short}, 0, 0), CaptureError::None);
  EXPECT_FALSE(findCachedRecap(job, found));
}

TEST_F(RecapSourceTest, CacheBucketAndMetadataMatchStillRequiresEverySourceByte) {
  epub.sections = {chapter({"First page"})};
  ASSERT_EQ(run({Scope::LastPages, 1, Detail::Short}, 0, 0), CaptureError::None);
  const std::string cached = cache();
  const std::string cachedSource = cached.substr(0, cached.size() - 4) + ".source";
  ASSERT_TRUE(Storage.fs.files.contains(cachedSource));
  Storage.fs.files[cachedSource].back() = '!';
  std::string found;
  EXPECT_FALSE(findCachedRecap(job, found));
}

TEST_F(RecapSourceTest, CorruptSourceOrUnboundedMetadataFailsClosed) {
  epub.sections = {chapter({"First page"})};
  ASSERT_EQ(run({Scope::LastPages, 1, Detail::Short}, 0, 0), CaptureError::None);
  Storage.fs.files[SOURCE_PATH] += " FUTURE";
  Job restored;
  EXPECT_FALSE(loadJob(restored));
  ASSERT_EQ(run({Scope::LastPages, 1, Detail::Short}, 0, 0), CaptureError::None);
  auto& metadata = Storage.fs.files[JOB_PATH];
  const uint32_t invalidLength = UINT32_MAX;
  std::memcpy(metadata.data() + sizeof(uint32_t) * 2, &invalidLength, sizeof(invalidLength));
  EXPECT_FALSE(loadJob(restored));
}

TEST_F(RecapSourceTest, InterruptedCachePublishHasNoEligibleMetadata) {
  epub.sections = {chapter({"First page"})};
  ASSERT_EQ(run({Scope::LastPages, 1, Detail::Short}, 0, 0), CaptureError::None);
  const std::string path = cache();
  Storage.fs.failRename = path + ".tmp";
  std::string found;
  EXPECT_FALSE(storeCachedRecap(job, RESPONSE_PATH, found));
  EXPECT_FALSE(findCachedRecap(job, found));
}

}  // namespace
