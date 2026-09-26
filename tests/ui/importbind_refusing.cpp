#include "ui/legacyimport.h"

#include <stdio.h>

namespace replay {
namespace ui {
namespace legacy {
namespace {

// The panel suite never converts for real (each case installs a fake), so it links this instead of the
// converter library. Result is filled by hand because Refuse() lives in that library.
import::Result RefuseUnlinked(const wchar_t*, const wchar_t*, const import::ConvertOptions&,
                              import::ConvertReport&, import::Progress*) {
    import::Result r{};
    r.ok = false;
    r.code = import::ResultCode::NotImplemented;
    snprintf(r.message, sizeof r.message,
             "the panel test build does not link the legacy-replay converter");
    return r;
}

}  // namespace

ConvertFn LinkedConverter() { return &RefuseUnlinked; }

}  // namespace legacy
}  // namespace ui
}  // namespace replay
