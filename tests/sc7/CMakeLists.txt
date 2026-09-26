# P12 / C8. The SC7 in-process reader's controls: a planted undeclared difference is named with its RVA,
# a declared one is attributed to its ledger row, a relocated field is delta-checked rather than masked,
# and an empty ledger is FAIL or COULD-NOT-ASK but never PASS.
replay_add_test(NAME sc7
    SOURCES test_sc7.cpp
    LIBS replay_bootstrap)

# The same document, taken to a real parser. test_sc7 asserts the escaped bytes are present; a string
# search cannot say the file PARSES, and L5 is what that difference cost. check.ps1 also carries the
# retroactive arm against the broken artifact this directory records.
replay_add_script_check(NAME sc7)
