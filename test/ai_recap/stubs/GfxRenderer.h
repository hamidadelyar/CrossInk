#pragma once
#include <cstring>

class GfxRenderer {
 public:
  int getTextAdvanceX(int, const char* text, int, unsigned = 0) const { return static_cast<int>(std::strlen(text)); }
};
