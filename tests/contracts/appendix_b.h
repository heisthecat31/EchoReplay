// Appendix B, as the preprocessor sees it. B.13: "A contract is frozen when it COMPILES ... An
// undefined type is a build failure, not a Wave-2 discovery."
//
// Wave 1 runs thirteen packets in parallel, so most of these headers do not exist yet. Each is pulled
// in through __has_include and the row records whether it was there, so a contract that has not landed
// is REPORTED rather than either breaking twelve other packets' builds or passing silently. Nothing
// here needs editing when one arrives: the row starts being satisfied on its own.
//
// Header paths come from the design's component table and S13's Owns. lines. A row whose path is
// wrong shows up as a contract that never lands, which is the failure mode to watch for.
#pragma once

#include "bootstrap/services.h"

#if __has_include(<foundation/bitio/bitio.h>)
#include <foundation/bitio/bitio.h>
#define REPLAY_HAS_B1 1
#else
#define REPLAY_HAS_B1 0
#endif

#if __has_include(<foundation/demofile/demofile.h>)
#include <foundation/demofile/demofile.h>
#define REPLAY_HAS_B2 1
#else
#define REPLAY_HAS_B2 0
#endif

#if __has_include(<foundation/demofile/demotypes.h>)
#include <foundation/demofile/demotypes.h>
#define REPLAY_HAS_B0 1
#else
#define REPLAY_HAS_B0 0
#endif

#if __has_include(<engine/tick.h>)
#include <engine/tick.h>
#define REPLAY_HAS_B3 1
#else
#define REPLAY_HAS_B3 0
#endif

#if __has_include(<engine/entity.h>)
#include <engine/entity.h>
#define REPLAY_HAS_B4 1
#else
#define REPLAY_HAS_B4 0
#endif

#if __has_include(<engine/asseen.h>)
#include <engine/asseen.h>
#define REPLAY_HAS_B111 1
#else
#define REPLAY_HAS_B111 0
#endif

#if __has_include(<engine/session.h>)
#include <engine/session.h>
#define REPLAY_HAS_B6 1
#else
#define REPLAY_HAS_B6 0
#endif

#if __has_include(<engine/camera.h>)
#include <engine/camera.h>
#define REPLAY_HAS_B7 1
#else
#define REPLAY_HAS_B7 0
#endif

#if __has_include(<engine/clock.h>)
#include <engine/clock.h>
#define REPLAY_HAS_B5 1
#else
#define REPLAY_HAS_B5 0
#endif

#if __has_include(<engine/netvar.h>)
#include <engine/netvar.h>
#define REPLAY_HAS_B112 1
#else
#define REPLAY_HAS_B112 0
#endif

#if __has_include(<engine/identity_access.h>)
#include <engine/identity_access.h>
#define REPLAY_HAS_B113 1
#else
#define REPLAY_HAS_B113 0
#endif

#if __has_include(<engine/audiostate.h>)
#include <engine/audiostate.h>
#define REPLAY_HAS_B114 1
#else
#define REPLAY_HAS_B114 0
#endif

#if __has_include(<engine/wiretap.h>)
#include <engine/wiretap.h>
#define REPLAY_HAS_B14 1
#else
#define REPLAY_HAS_B14 0
#endif

#if __has_include(<engine/audiorate.h>)
#include <engine/audiorate.h>
#define REPLAY_HAS_B9 1
#else
#define REPLAY_HAS_B9 0
#endif

#if __has_include(<playback/transport.h>)
#include <playback/transport.h>
#define REPLAY_HAS_B8 1
#else
#define REPLAY_HAS_B8 0
#endif

// X(appendix, name, header, owner, present)
#define REPLAY_CONTRACT_ROWS(X)                                                                   \
    X("B.12", "ReplayServices/ILog/IConfig/ReplayModule", "bootstrap/services.h", "W1-C", 1)             \
    X("B.1", "IFieldCodec", "foundation/bitio/bitio.h", "W1-A", REPLAY_HAS_B1)                           \
    X("B.2", "IDemoWriter/IDemoReader", "foundation/demofile/demofile.h", "W1-B", REPLAY_HAS_B2)         \
    X("B.0", "Manifest/DecodedChapter/IDemoIndex", "foundation/demofile/demotypes.h", "W1-B", REPLAY_HAS_B0)\
    X("B.3", "ITick", "engine/tick.h", "W1-D", REPLAY_HAS_B3)                                            \
    X("B.4", "IEntity/IPosePublisher", "engine/entity.h", "W1-E", REPLAY_HAS_B4)                         \
    X("B.11.1", "IAsSeen", "engine/asseen.h", "W1-E", REPLAY_HAS_B111)                                   \
    X("B.6", "ISessionEntry", "engine/session.h", "W1-F", REPLAY_HAS_B6)                                 \
    X("B.7", "IReplayCamera", "engine/camera.h", "W1-H", REPLAY_HAS_B7)                                  \
    X("B.5", "IReplayClock", "engine/clock.h", "W2-D", REPLAY_HAS_B5)                                    \
    X("B.11.2", "INetVar", "engine/netvar.h", "W2-A", REPLAY_HAS_B112)                                   \
    X("B.11.3", "IIdentity", "engine/identity_access.h", "W2-C", REPLAY_HAS_B113)                        \
    X("B.11.4", "IAudioState", "engine/audiostate.h", "W2-H", REPLAY_HAS_B114)                           \
    X("B.9", "IAudioRate", "engine/audiorate.h", "W2-H", REPLAY_HAS_B9)                                  \
    X("B.14", "IWiretap", "engine/wiretap.h", "W1-L", REPLAY_HAS_B14)                                    \
    X("B.8", "IReplayTransport", "playback/transport.h", "W2-B", REPLAY_HAS_B8)                          
