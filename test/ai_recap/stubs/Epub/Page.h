#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "blocks/TextBlock.h"

struct PageTextLine {
  const TextBlock* block = nullptr;
  int xPos = 0;
  int yPos = 0;
  int clipX = 0;
  int clipY = 0;
  int clipWidth = 0;
  int clipHeight = 0;
  int lineHeight = 1;
  bool isTableText = false;
};
using PageTextLineVisitor = bool (*)(const PageTextLine&, void*);

class Page {
 public:
  explicit Page(const std::string& text) { blocks.emplace_back(text); }
  std::vector<TextBlock> blocks;
  bool table = false;
  int clipWidth = 100;
  int clipHeight = 100;
  bool forEachTextLine(PageTextLineVisitor visitor, void* context) const {
    int y = 0;
    for (const TextBlock& block : blocks) {
      if (!visitor({&block, 0, y++, 0, 0, clipWidth, clipHeight, 1, table}, context)) return false;
    }
    return true;
  }
};
