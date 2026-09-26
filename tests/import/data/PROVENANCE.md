# Test data provenance

| file | source | licence | sha256 |
|---|---|---|---|
| `sample.echoreplay` | EchoTools/tape `testdata/sample.echoreplay` @ `32b7ba9d94cbaa749ea4e861bd672dfaba40ea4a` | MIT, (c) 2024 thesprockee | `de975bae5fe0f235af6ed55609f8f119d4a47d2695a41ad505683d5c7fe537c4` |
| `sample.tape` | EchoTools/tape `testdata/sample.tape.golden` @ the same commit, renamed | MIT, (c) 2024 thesprockee | `a4ba97682a96a53eb4b517b40f37e28a822f2811485fc6fef5735a230ed72f46` |

tape made `sample.tape` from `sample.echoreplay`, so the two describe one match (63.3 s, 15 Hz, 2 players);
`test_readers.cpp` relies on that for its cross-format comparison. Both files are binary (zip, zstd) and
are compared byte-for-byte against the hashes above by nothing but this table: re-copy them from the
commit named if they are ever in doubt.
