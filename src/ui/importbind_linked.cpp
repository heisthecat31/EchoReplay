#include "ui/legacyimport.h"

namespace replay {
namespace ui {
namespace legacy {

ConvertFn LinkedConverter() { return &import::Convert; }

}  // namespace legacy
}  // namespace ui
}  // namespace replay
