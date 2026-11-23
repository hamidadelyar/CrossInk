#pragma once

#include <string>
#include <utility>
#include <vector>

#include "Epub/Page.h"

struct FakeSection {
  std::vector<Page> pages;
  bool cached = true;
  bool partial = false;
  bool building = false;
  bool canBuild = true;
  int failingPage = -1;
  unsigned builds = 0;
};

class Epub {
 public:
  std::string path = "/book.epub";
  std::vector<FakeSection> sections;
  std::vector<std::pair<uint16_t, uint16_t>> readPages;
  const std::string& getPath() const { return path; }
  int getSpineItemsCount() const { return static_cast<int>(sections.size()); }
};
