// Is the Appendix B namespace split fixed yet? Asked the way the COMPILER answers it.
//
// services.h forward-declares IEntity and IAsSeen inside namespace replay; engine/entity.h and
// engine/asseen.h define them at global scope. Those are two types with one spelling, so
// ReplayServices::entity names a class that is declared and never defined, and W2-A cannot gather
// S_BODY_SAMPLED or S_ASSEEN through it.
//
// ★ THIS EXISTS BECAUSE READING THE HEADER IS NOT THE ANSWER. The architect namespaced twelve of
// fifteen APPENDIX SECTIONS in the design document; whether the two HEADERS on disk moved is a
// different question, and the only authority on it is the compiler. Run this instead of grepping.
//
// It prints rather than asserts on purpose: it is a status probe, not a gate. When both replay:: lines
// read 1, capture.cpp starts compiling the gathers in with no change to capture.cpp at all, and the
// load-time warning about empty streams stops being emitted.
#include <cstdio>
#include <type_traits>

#include "bootstrap/services.h"   // forward-declares replay::IEntity / replay::IAsSeen
#include "engine/entity.h"        // defines ::IEntity  (or replay::IEntity, if it moved)
#include "engine/asseen.h"

template <class T, class = void> struct Usable : std::false_type {};
template <class T> struct Usable<T, std::void_t<decltype(sizeof(T))>> : std::true_type {};

int main() {
    printf("replay::IEntity complete = %d\n", int(Usable<replay::IEntity>::value));
    printf("replay::IAsSeen complete = %d\n", int(Usable<replay::IAsSeen>::value));
    printf("::IEntity      complete = %d\n", int(Usable<::IEntity>::value));
    printf("::IAsSeen      complete = %d\n", int(Usable<::IAsSeen>::value));
    return 0;
}
