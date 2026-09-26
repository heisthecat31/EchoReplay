#include "audio/voipformat.h"

#include <string.h>

#include "audio/audio_facts.h"

namespace replay {
namespace audio {

VoipHeaderStub MakeHeaderStub(uint16_t seq) {
    VoipHeaderStub stub{};
    memset(stub.bytes, 0, sizeof stub.bytes);
    const uint16_t masked = static_cast<uint16_t>(seq & kSeqMask);
    memcpy(stub.bytes + audio_facts::kVoipRecvSeqOff, &masked, sizeof masked);
    return stub;
}

bool InjectVoip(const VoipInjector& injector, uint32_t handle, uint16_t seq, const void* payload,
                uint16_t length) {
    if (!injector.Ready() || length > kMaxPayload) {
        return false;
    }
    if (length != 0 && payload == nullptr) {
        return false;
    }
    const VoipHeaderStub stub = MakeHeaderStub(seq);
    injector.receiveVoip(injector.receiver, handle, stub.bytes, payload, length);
    return true;
}

}  // namespace audio
}  // namespace replay
