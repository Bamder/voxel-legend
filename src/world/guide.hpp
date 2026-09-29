#pragma once

namespace guide {

inline constexpr int kPageCount = 8;
inline constexpr int kMaxLines = 7;

const char* pageTitle(int page);
int pageLineCount(int page);
const char* pageLine(int page, int line);

} // namespace guide
