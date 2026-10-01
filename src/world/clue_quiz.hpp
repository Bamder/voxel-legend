#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace clue_quiz {

struct Question {
    const char* subject;
    const char* prompt;
    std::array<const char*, 4> options;
    uint8_t correct;
};

size_t count();
const Question& at(size_t index);

// Server-side only: the answer key is never included in a quiz network message.
uint32_t mix(uint32_t value);

} // namespace clue_quiz
