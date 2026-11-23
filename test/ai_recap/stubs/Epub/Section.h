#pragma once

#include <Epub.h>
#include <Epub/ReaderRenderSpec.h>

#include <cstdint>
#include <functional>
#include <memory>

class GfxRenderer;

struct SectionBuildOptions {
  bool (*shouldCancel)(void*) = nullptr;
  void* cancelContext = nullptr;
  bool* cancellationObserved = nullptr;
};

class Section {
 public:
  uint16_t pageCount;
  int currentPage = 0;
  Section(Epub& epub, const int spine, GfxRenderer&)
      : pageCount(static_cast<uint16_t>(epub.sections[spine].pages.size())), epub(epub), spine(spine) {}
  bool loadSectionFile(const ReaderRenderSpec&) { return data().cached; }
  bool isPartial() const { return epub.sections[spine].partial; }
  bool isBuilding() const { return epub.sections[spine].building; }
  bool createSectionFile(const ReaderRenderSpec&, const std::function<void()>&, bool*, bool*,
                         SectionBuildOptions options) {
    ++data().builds;
    if (options.shouldCancel && options.shouldCancel(options.cancelContext)) {
      if (options.cancellationObserved) *options.cancellationObserved = true;
      return false;
    }
    if (!data().canBuild) return false;
    data().partial = false;
    data().cached = true;
    return true;
  }
  std::unique_ptr<Page> loadPage(const int page) {
    epub.readPages.emplace_back(spine, page);
    if (page < 0 || page >= pageCount || page == data().failingPage) return {};
    return std::make_unique<Page>(data().pages[page]);
  }

 private:
  Epub& epub;
  int spine;
  FakeSection& data() { return epub.sections[spine]; }
};
