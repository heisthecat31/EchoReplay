// AUD-L5 §8: the L5 #3 egress record said "16/16 hooks live, 60 refused" and named no destination, so
// "the blocker did its job" could not be checked -- the auditor had to infer the peer set from the
// engine's own log. These checks are over the rendering that ends that, and the one that matters most is
// the CONTROL: the emitted set is greppable for the raw public host and the raw hostname, and they must
// be absent. Skip the redaction call in egress.cpp and that control is the check that reds.

#include <cstring>
#include <string>

#include <doctest.h>
#include <windows.h>

#include "bootstrap/bootstrap.h"
#include "bootstrap/egress.h"
#include "bootstrap/sha256.h"

namespace {

std::string Sha256Hex(const std::string& s) {
    replay::Sha256 sha;
    sha.Update(s.data(), s.size());
    uint8_t digest[replay::Sha256::kDigestBytes];
    sha.Final(digest);
    char hex[replay::kSha256HexChars];
    replay::Sha256::ToHex(digest, hex);
    return std::string(hex);
}

std::string Redact(const char* text) {
    char out[128] = {0};
    const bool ok = replay::egress::RedactDestination(text, replay::egress::RedactionSalt(), out,
                                                      sizeof out);
    return ok ? std::string(out) : std::string("<REFUSED>");
}

// Everything the record would carry about destinations, as one blob to grep.
std::string RecordText() {
    const char* refused = replay::egress::RefusedDestinationsJson();
    const char* allowed = replay::egress::AllowedDestinationsJson();
    REQUIRE(refused != nullptr);
    REQUIRE(allowed != nullptr);
    return std::string("refusedDestinations: [") + refused + "], allowedDestinations: [" + allowed + "]";
}

bool LooksLikeToken(const std::string& s) {
    if (s.compare(0, 9, "redacted-") != 0 || s.size() < 17) {
        return false;
    }
    for (size_t i = 9; i < 17; ++i) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

}  // namespace

// A hash we ported and never checked would produce a plausible fictional digest, and every destination
// token would be wrong in a way nothing else in this suite could see. FIPS 180-4 B.1-B.3.
TEST_CASE("sha256 matches the published vectors") {
    CHECK(Sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(Sha256Hex(std::string(1000000, 'a')) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    // The block boundary either side of the length field, where a padding bug hides.
    CHECK(Sha256Hex(std::string(55, 'a')) ==
          "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(Sha256Hex(std::string(56, 'a')) ==
          "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(Sha256Hex(std::string(64, 'a')) ==
          "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}

TEST_CASE("addresses that cannot be secrets stay verbatim") {
    REQUIRE(replay::egress::RedactionSalt() != nullptr);
    CHECK(Redact("127.0.0.1:80") == "127.0.0.1:80");
    CHECK(Redact("127.0.0.1:6719") == "127.0.0.1:6719");
    CHECK(Redact("[::1]:443") == "[::1]:443");
    CHECK(Redact("0.0.0.0:0") == "0.0.0.0:0");
    CHECK(Redact("10.0.0.5:7777") == "10.0.0.5:7777");
    CHECK(Redact("172.16.4.9:8080") == "172.16.4.9:8080");
    CHECK(Redact("192.168.1.20:27015") == "192.168.1.20:27015");
    CHECK(Redact("100.64.100.100:6894") == "100.64.100.100:6894");   // CGNAT, the L5 #3 inbound join
    CHECK(Redact("localhost") == "localhost");
    CHECK(Redact("family=1") == "family=1");
    CHECK(Redact("(null)") == "(null)");
    // 172.15 and 172.32 are OUTSIDE RFC1918, and a /16-shaped test would not tell the two apart.
    CHECK(LooksLikeToken(Redact("172.15.0.1:80")));
    CHECK(LooksLikeToken(Redact("172.32.0.1:80")));
    CHECK(LooksLikeToken(Redact("100.63.0.1:80")));   // just below CGNAT
    CHECK(LooksLikeToken(Redact("100.128.0.1:80")));  // just above CGNAT
}

TEST_CASE("everything else becomes a stable token that keeps only the port and the TLD") {
    REQUIRE(replay::egress::RedactionSalt() != nullptr);

    const std::string ip = Redact("203.0.113.9:443");
    CHECK(LooksLikeToken(ip));
    CHECK(ip.find(":443") != std::string::npos);
    CHECK(ip.find("203.0.113.9") == std::string::npos);
    CHECK(ip.find('.') == std::string::npos);   // an IP literal has no TLD to keep

    const std::string name = Redact("login.echovr.example:6789");
    CHECK(LooksLikeToken(name));
    CHECK(name.find(".example:6789") != std::string::npos);
    CHECK(name.find("login") == std::string::npos);
    CHECK(name.find("echovr") == std::string::npos);

    // Stable within a build, and case does not make a second identity.
    CHECK(Redact("login.echovr.example:6789") == name);
    CHECK(Redact("LOGIN.EchoVR.Example:6789") == name);
    // The port is not part of the identity, so the same host on two ports shares one token.
    CHECK(Redact("login.echovr.example:443").substr(0, 17) == name.substr(0, 17));
    // Different hosts do not share one.
    CHECK(Redact("api.echovr.example:6789").substr(0, 17) != name.substr(0, 17));
}

TEST_CASE("no salt means refuse, never the raw text") {
    char out[128];
    std::memset(out, 'x', sizeof out);
    CHECK_FALSE(replay::egress::RedactDestination("login.echovr.example:6789", nullptr, out,
                                                  sizeof out));
    CHECK(out[0] == '\0');
    std::memset(out, 'x', sizeof out);
    CHECK_FALSE(replay::egress::RedactDestination("login.echovr.example:6789", "", out, sizeof out));
    CHECK(out[0] == '\0');
    CHECK(std::string(out).find("echovr") == std::string::npos);
}

TEST_CASE("a synthetic refusal list renders exactly as expected, and the raw hosts are absent") {
    replay::egress::ResetForTest();
    replay::egress::RecordDestinationForTest("127.0.0.1:80", "connect", true);
    replay::egress::RecordDestinationForTest("192.168.1.20:27015", "sendto", true);
    replay::egress::RecordDestinationForTest("203.0.113.9:443", "connect", false);
    replay::egress::RecordDestinationForTest("login.echovr.example", "InternetConnectW", false);

    const std::string refused = replay::egress::RefusedDestinationsJson();
    const std::string allowed = replay::egress::AllowedDestinationsJson();
    CHECK(replay::egress::RedactionRefusalReason() == nullptr);

    CHECK(allowed == "{\"dest\": \"127.0.0.1:80\", \"via\": \"connect\", \"count\": 1}, "
                     "{\"dest\": \"192.168.1.20:27015\", \"via\": \"sendto\", \"count\": 1}");

    const std::string expected =
        "{\"dest\": \"" + Redact("203.0.113.9:443") + "\", \"via\": \"connect\", \"count\": 1}, " +
        "{\"dest\": \"" + Redact("login.echovr.example") + "\", \"via\": \"InternetConnectW\", "
        "\"count\": 1}";
    CHECK(refused == expected);

    // THE CONTROL. Grep the emitted record for what must never reach it.
    const std::string record = RecordText();
    CHECK(record.find("203.0.113.9") == std::string::npos);
    CHECK(record.find("login.echovr.example") == std::string::npos);
    CHECK(record.find("echovr") == std::string::npos);
    CHECK(record.find("login") == std::string::npos);
    // And the control is not vacuous: it is looking at a record that DOES carry these rows.
    CHECK(record.find("redacted-") != std::string::npos);
    CHECK(record.find(":443") != std::string::npos);
    CHECK(record.find("InternetConnectW") != std::string::npos);
    CHECK(record.find("127.0.0.1:80") != std::string::npos);

    replay::egress::ResetForTest();
}

// The retroactive case. AUD-L5 §8: the L5 #3 record carried 60 refusals and no hosts; the auditor
// reconstructed the peer set out of the engine log
// (launch run L5-8abcbbf 20260902-165645Z, its gamelogs), which held exactly
// 127.0.0.1:80 x204, 127.0.0.1:6719 x2 and 100.64.100.100:6894 x4 -- the last being an inbound join to
// this host over CGNAT. This is what the record would have had to show for that inference to be
// unnecessary, and every one of those addresses is unredactable, so the reader sees them exactly.
TEST_CASE("the L5 #3 destination set renders as the reading the auditor had to infer") {
    replay::egress::ResetForTest();
    for (int i = 0; i < 204; ++i) {
        replay::egress::RecordDestinationForTest("127.0.0.1:80", "connect", true);
    }
    for (int i = 0; i < 2; ++i) {
        replay::egress::RecordDestinationForTest("127.0.0.1:6719", "sendto", true);
    }
    for (int i = 0; i < 4; ++i) {
        replay::egress::RecordDestinationForTest("100.64.100.100:6894", "WSASendTo", false);
    }

    CHECK(std::string(replay::egress::AllowedDestinationsJson()) ==
          "{\"dest\": \"127.0.0.1:80\", \"via\": \"connect\", \"count\": 204}, "
          "{\"dest\": \"127.0.0.1:6719\", \"via\": \"sendto\", \"count\": 2}");
    CHECK(std::string(replay::egress::RefusedDestinationsJson()) ==
          "{\"dest\": \"100.64.100.100:6894\", \"via\": \"WSASendTo\", \"count\": 4}");
    // The conclusion this rendering makes checkable rather than inferable: nothing outside loopback
    // and CGNAT appears at all, so no token is present.
    CHECK(RecordText().find("redacted-") == std::string::npos);

    replay::egress::ResetForTest();
}

TEST_CASE("an empty set is an answer, not a refusal") {
    replay::egress::ResetForTest();
    const char* refused = replay::egress::RefusedDestinationsJson();
    REQUIRE(refused != nullptr);
    CHECK(std::string(refused).empty());
    CHECK(replay::egress::RedactionRefusalReason() == nullptr);
}

// The salt is PER LAUNCH. It used to be bootstrap::BuildId(), which the plugin log prints, so a
// reader holding the log could hash a candidate host list and invert every token. These two cases are
// the ones that red if the salt goes back to being anything a reader can obtain.
TEST_CASE("egress redaction: two inits token the same host differently, one init tokens it once") {
    replay::egress::ResetForTest();
    const char* raw1 = replay::egress::RedactionSalt();
    REQUIRE(raw1 != nullptr);
    const std::string salt1(raw1);
    const std::string first = Redact("login.example.net");

    replay::egress::ResetForTest();
    const char* raw2 = replay::egress::RedactionSalt();
    REQUIRE(raw2 != nullptr);
    const std::string salt2(raw2);
    const std::string second = Redact("login.example.net");

    REQUIRE(LooksLikeToken(first));
    REQUIRE(LooksLikeToken(second));
    CHECK(salt1.size() == 32);
    CHECK(salt2.size() == 32);
    CHECK(salt1 != salt2);
    CHECK(first != second);
    // Intra-record correlation is the property being KEPT: inside one init the host is one token.
    CHECK(Redact("login.example.net") == second);
    CHECK(Redact("LOGIN.EXAMPLE.NET") == second);
    // And the salt is not a value the log hands out. Named, because that is the exact mutant.
    CHECK(salt2 != std::string(replay::bootstrap::BuildId()));
    replay::egress::ResetForTest();
}

TEST_CASE("egress redaction: the salt appears in nothing the record emits") {
    replay::egress::ResetForTest();
    const char* raw = replay::egress::RedactionSalt();
    REQUIRE(raw != nullptr);
    const std::string salt(raw);
    const std::string host = "login.example.net";
    const std::string input = host + ":443";
    replay::egress::RecordDestinationForTest(input.c_str(), "connect", false);
    replay::egress::RecordDestinationForTest("127.0.0.1:6721", "connect", true);

    const std::string record = RecordText();
    // NON-VACUITY, both directions. The host IS in the input the tokens were built from, and the
    // record IS the real rendering and not an empty string: the loopback row goes through verbatim
    // and the remote row is a token. Without these three, the two absence checks below pass over
    // nothing.
    CHECK(input.find(host) != std::string::npos);
    CHECK(record.find("127.0.0.1:6721") != std::string::npos);
    CHECK(record.find("redacted-") != std::string::npos);

    CHECK(record.find(salt) == std::string::npos);
    CHECK(record.find(host) == std::string::npos);
    // The token is derived from the salt but must not carry it: a prefix of the salt in the hex
    // would be the same leak in a shorter form.
    CHECK(record.find(salt.substr(0, 8)) == std::string::npos);
    replay::egress::ResetForTest();
}
