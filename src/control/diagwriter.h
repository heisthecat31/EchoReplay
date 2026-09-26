#pragma once

// W2-G. A bounded, always-terminated text cursor.
//
// * WHY THIS EXISTS RATHER THAN ONE BIG snprintf. `diag` prints eighteen values, and an eighteen
// argument format string is a construct where one inserted line silently shifts every argument
// after it -- reading an int as a char* and printing whatever the stack held. MSVC's format checker
// caught exactly that here during the build. One value per call cannot shift, cannot miscount, and
// each line is independently readable.
//
// Truncation is a fact the reader is told, not a place the text happens to stop: once the buffer is
// full the cursor keeps counting what it could not write, and Truncated() says so.

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

namespace replay {
namespace control {

class TextOut {
public:
    TextOut(char* buf, size_t cap) : buf_(buf), cap_(cap), len_(0) {
        if (cap_ != 0) {
            buf_[0] = 0;
        }
    }

    void Line(const char* fmt, ...) {
        if (cap_ == 0 || len_ + 1 >= cap_) {
            overflowed_ = true;
            return;
        }
        va_list args;
        va_start(args, fmt);
        const int n = vsnprintf(buf_ + len_, cap_ - len_, fmt, args);
        va_end(args);
        if (n < 0) {
            overflowed_ = true;
            return;
        }
        if (static_cast<size_t>(n) >= cap_ - len_) {
            len_ = cap_ - 1;
            overflowed_ = true;
        } else {
            len_ += static_cast<size_t>(n);
        }
        buf_[len_] = 0;
    }

    size_t Length() const { return len_; }
    bool   Truncated() const { return overflowed_; }

private:
    char*  buf_;
    size_t cap_;
    size_t len_;
    bool   overflowed_ = false;
};

}  // namespace control
}  // namespace replay
