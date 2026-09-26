// A stand-in for rlay.dll: the other plugin this one shares plugins\ with. It does nothing at all, which
// is the point - SC9 asks that we work with it present and absent, in either load order, and "present"
// only needs GetModuleHandleA("rlay.dll") to answer.
#include <windows.h>

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) {
    return TRUE;
}
