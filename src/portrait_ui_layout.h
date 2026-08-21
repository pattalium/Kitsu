#pragma once

#include <stddef.h>
#include <stdint.h>

// Display-independent layout primitives for Kitsu's physically rotated
// 128x64 OLED.  The application renders into this logical 64x128 portrait
// canvas.  Keeping the measurements here (rather than scattered through
// main.cpp) makes every text and geometry decision host-testable.
namespace kitsu868 {
namespace portrait {

constexpr int16_t kCanvasWidth = 64;
constexpr int16_t kCanvasHeight = 128;
constexpr int16_t kContentInset = 2;
constexpr int16_t kContentWidth = kCanvasWidth - kContentInset * 2;
constexpr uint8_t kGlyphWidth = 5;
constexpr uint8_t kGlyphHeight = 7;
constexpr uint8_t kMaximumScale = 2;
constexpr uint8_t kEllipsisCharacters = 2;

struct TextPlan {
  uint8_t scale = 1;
  uint8_t advance = 6;
  size_t sourceCharacters = 0;
  size_t renderedCharacters = 0;
  int16_t width = 0;
  bool compact = false;
  bool ellipsized = false;
  bool valid = false;
};

struct DotPlan {
  uint8_t size = 0;
  uint8_t gap = 0;
  int16_t width = 0;
  bool valid = false;
};

constexpr int16_t glyphWidth(uint8_t scale) {
  return static_cast<int16_t>(kGlyphWidth * scale);
}

constexpr int16_t glyphHeight(uint8_t scale) {
  return static_cast<int16_t>(kGlyphHeight * scale);
}

constexpr uint8_t regularAdvance(uint8_t scale) {
  return static_cast<uint8_t>((kGlyphWidth + 1U) * scale);
}

constexpr uint8_t compactAdvance(uint8_t scale) {
  return static_cast<uint8_t>(kGlyphWidth * scale);
}

inline int16_t textWidth(size_t characters, uint8_t scale,
                         uint8_t advance) {
  if (characters == 0U || scale == 0U || advance == 0U) return 0;
  const size_t glyph = static_cast<size_t>(glyphWidth(scale));
  if (characters - 1U > (SIZE_MAX - glyph) / advance) return INT16_MAX;
  const size_t width = (characters - 1U) * advance + glyph;
  return width > static_cast<size_t>(INT16_MAX)
             ? INT16_MAX
             : static_cast<int16_t>(width);
}

inline size_t lineCapacity(int16_t maximumWidth, uint8_t scale,
                           uint8_t advance) {
  const int16_t glyph = glyphWidth(scale);
  if (maximumWidth < glyph || scale == 0U || advance == 0U) return 0U;
  return 1U + static_cast<size_t>(maximumWidth - glyph) / advance;
}

inline TextPlan makePlan(size_t sourceCharacters, size_t renderedCharacters,
                         uint8_t scale, uint8_t advance, bool compact,
                         bool ellipsized) {
  TextPlan plan;
  plan.scale = scale;
  plan.advance = advance;
  plan.sourceCharacters = sourceCharacters;
  plan.renderedCharacters = renderedCharacters;
  plan.width = textWidth(renderedCharacters, scale, advance);
  plan.compact = compact;
  plan.ellipsized = ellipsized;
  plan.valid = true;
  return plan;
}

// Prefer readable tracking at the requested scale.  If that cannot fit, use
// edge-to-edge glyph tracking at the same scale, then step down.  Only when the
// complete label cannot fit at scale 1 do we reserve two cells for "..".  The
// returned width therefore never exceeds maximumWidth.
inline TextPlan planText(size_t characters, int16_t maximumWidth,
                         uint8_t preferredScale = 1U) {
  TextPlan invalid;
  if (maximumWidth <= 0) return invalid;
  if (preferredScale == 0U) preferredScale = 1U;
  if (preferredScale > kMaximumScale) preferredScale = kMaximumScale;

  if (characters == 0U) {
    return makePlan(0U, 0U, preferredScale,
                    regularAdvance(preferredScale), false, false);
  }

  for (uint8_t scale = preferredScale; scale >= 1U; --scale) {
    const uint8_t regular = regularAdvance(scale);
    if (textWidth(characters, scale, regular) <= maximumWidth) {
      return makePlan(characters, characters, scale, regular, false, false);
    }
    const uint8_t compact = compactAdvance(scale);
    if (textWidth(characters, scale, compact) <= maximumWidth) {
      return makePlan(characters, characters, scale, compact, true, false);
    }
    if (scale == 1U) break;
  }

  const uint8_t scale = 1U;
  const uint8_t advance = compactAdvance(scale);
  const size_t capacity = lineCapacity(maximumWidth, scale, advance);
  if (capacity == 0U) return invalid;
  if (capacity <= kEllipsisCharacters) {
    return makePlan(0U, capacity, scale, advance, true, true);
  }
  return makePlan(capacity - kEllipsisCharacters, capacity, scale, advance,
                  true, true);
}

inline DotPlan planDots(uint8_t count, int16_t maximumWidth,
                        uint8_t preferredSize = 3U,
                        uint8_t preferredGap = 5U) {
  DotPlan plan;
  if (count == 0U || maximumWidth <= 0) return plan;
  uint8_t size = preferredSize == 0U ? 1U : preferredSize;
  while (size > 1U && static_cast<int16_t>(count * size) > maximumWidth) {
    --size;
  }
  if (static_cast<int16_t>(count * size) > maximumWidth) return plan;
  uint8_t gap = 0U;
  if (count > 1U) {
    const int16_t remaining = maximumWidth - count * size;
    const uint8_t available = static_cast<uint8_t>(remaining / (count - 1U));
    gap = available < preferredGap ? available : preferredGap;
  }
  plan.size = size;
  plan.gap = gap;
  plan.width = static_cast<int16_t>(count * size + (count - 1U) * gap);
  plan.valid = plan.width <= maximumWidth;
  return plan;
}

inline bool rectangleFits(int16_t x, int16_t y, int16_t width,
                          int16_t height) {
  return x >= 0 && y >= 0 && width >= 0 && height >= 0 &&
         x + width <= kCanvasWidth && y + height <= kCanvasHeight;
}

inline bool centeredTextFitsVertically(int16_t y, const TextPlan& plan) {
  return plan.valid && rectangleFits(
      static_cast<int16_t>((kCanvasWidth - plan.width) / 2), y, plan.width,
      glyphHeight(plan.scale));
}

}  // namespace portrait
}  // namespace kitsu868
