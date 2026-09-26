#include "import/result.h"

#include <stdarg.h>
#include <stdio.h>

#include "import/convert.h"

namespace replay::import {

Result Success() {
    Result r{};
    r.ok = true;
    r.code = ResultCode::Ok;
    return r;
}

Result Refuse(ResultCode code, const char* format, ...) {
    Result r{};
    r.ok = false;
    r.code = code;
    va_list args;
    va_start(args, format);
    vsnprintf(r.message, sizeof(r.message), format, args);
    va_end(args);
    return r;
}

const char* ResultCodeName(ResultCode code) {
    switch (code) {
    case ResultCode::Ok: return "OK";
    case ResultCode::Container: return "E_CONTAINER";
    case ResultCode::Corrupt: return "E_CORRUPT";
    case ResultCode::TapeVersion: return "E_TAPE_VERSION";
    case ResultCode::UnsupportedMap: return "E_UNSUPPORTED_MAP";
    case ResultCode::TooShort: return "E_TOO_SHORT";
    case ResultCode::Basis: return "E_BASIS";
    case ResultCode::Roster: return "E_ROSTER";
    case ResultCode::Io: return "E_IO";
    case ResultCode::Writer: return "E_WRITER";
    case ResultCode::Cancelled: return "E_CANCELLED";
    case ResultCode::BadArguments: return "E_BAD_ARGUMENTS";
    case ResultCode::NotImplemented: return "E_NOT_IMPLEMENTED";
    case ResultCode::TooLarge: return "E_TOO_LARGE";
    }
    return "E_UNKNOWN";
}

void Progress::Report(uint64_t done, uint64_t total) {
    if (total == 0) return;
    const uint64_t within = done >= total ? stageSpan : done * stageSpan / total;
    permille.store(static_cast<uint32_t>(stageBase + within), std::memory_order_relaxed);
}

int ExitCodeFor(const Result& result) {
    if (result.ok) return 0;
    return result.code == ResultCode::Io ? 3 : 2;
}

}  // namespace replay::import
