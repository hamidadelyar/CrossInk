#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

class GfxRenderer;

class TextBlock {
 public:
  std::vector<std::string> words;
  std::vector<bool> hyphens;
  bool validBlock = true;
  explicit TextBlock(const std::string& text) {
    std::istringstream input(text);
    for (std::string word; input >> word;) words.push_back(std::move(word));
    hyphens.resize(words.size());
  }
  bool valid() const { return validBlock; }
  uint16_t wordCount() const { return static_cast<uint16_t>(words.size()); }
  const char* wordText(const uint16_t i) const { return words[i].c_str(); }
  uint16_t wordTextLen(const uint16_t i) const { return static_cast<uint16_t>(words[i].size()); }
  bool wordHasSpaceBefore(uint16_t i) const { return i > 0; }
  bool wordEndsWithInsertedHyphen(const uint16_t i) const { return hyphens[i]; }
  int wordXpos(const uint16_t i) const { return i == 0 ? 0 : static_cast<int>(i * 10); }
  int resolvedFontId(const GfxRenderer&, const int fontId) const { return fontId; }
  int wordStyle(uint16_t) const { return 0; }
};
