#pragma once

#include <stdint.h>

#include <vector>

#include "foundation/demofile/demotypes.h"
#include "foundation/demofile/format.h"

namespace replay {

// A Manifest plus the storage its pointers borrow from. Appendix B.0 says every Blob and pointer member
// borrows from one arena inside the reader; this is that arena.
class ManifestStore {
public:
    void CopyFrom(const Manifest& src);
    void FillFromHeader(const FileHeaderDisk& h);
    void SetRounds(const RoundMarker* rounds, size_t count);
    // ARCH-21's round table, which is only known at close. `source` is recorded even when there are no
    // rows: "the writer chose and found nothing" and "nobody ever asked" are different files.
    bool SetRoundTable(const RoundRow* rows, size_t count, MarkerSource source);
    void Rebind();  // re-points every borrowed pointer after the backing vectors changed

    Manifest& View() { return view_; }
    const Manifest& View() const { return view_; }

private:
    friend bool DeserializeManifest(const uint8_t*, uint32_t, ManifestStore&, const char**);

    Manifest view_{};
    std::vector<PlayerRecord> players_;
    std::vector<RoundMarker> rounds_;
    std::vector<RoundRow> roundRows_;
    std::vector<SchemaDef> schemas_;
    std::vector<FieldDef> fields_;
    std::vector<uint32_t> fieldOffsets_;  // per schema, into fields_
    // The reason the round table was refused, empty when it was not. Owned here so `View()` can hand
    // out a pointer that outlives the parse and survives Rebind.
    char roundsRefusedBuf_[160] = {};
    std::vector<uint8_t> arena_;
    std::vector<uint32_t> blobOffsets_;  // entrant/unlocks per player, then the four manifest blobs
    std::vector<uint32_t> blobLengths_;
};

// Everything the FileHeader already carries is deliberately absent: one field, one place. The reader
// fills those from the 128 bytes at offset 0.
void SerializeManifest(const Manifest& m, std::vector<uint8_t>& out);
bool DeserializeManifest(const uint8_t* p, uint32_t n, ManifestStore& store, const char** error);

}  // namespace replay
