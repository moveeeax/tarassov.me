/**
 * @file test_base64.cpp
 * @brief Unit tests for Utils::Base64 url-encode/decode (pure, no services).
 *
 * The decode-side validation is security-relevant: it rejects non-canonical
 * encodings that would otherwise let a *different* token string decode to the
 * same signature bytes, slipping past one-shot / JTI replay guards (token
 * malleability). See Base64.hpp.
 */

#include <string>

#include <gtest/gtest.h>

#include "utils/Base64.hpp"

namespace {

using Utils::Base64::url_decode;
using Utils::Base64::url_encode;

TEST(Base64Test, RoundTrips) {
    for (const std::string& s : {std::string(""),
                                 std::string("a"),
                                 std::string("ab"),
                                 std::string("abc"),
                                 std::string("hello world"),
                                 std::string("\x00\x01\x02\xff\xfe", 5)}) {
        EXPECT_EQ(url_decode(url_encode(s)), s) << "roundtrip failed for size " << s.size();
    }
}

TEST(Base64Test, NoPaddingCharsInOutput) {
    // base64url (RFC 4648 §5, no padding) — never emits '='.
    EXPECT_EQ(url_encode("any").find('='), std::string::npos);
    EXPECT_EQ(url_encode("ab").find('='), std::string::npos);
}

TEST(Base64Test, RejectsInvalidCharacter) {
    EXPECT_THROW(url_decode("abc$"), std::runtime_error);  // '$' not in alphabet
    EXPECT_THROW(url_decode("a b"), std::runtime_error);   // space not in alphabet
}

TEST(Base64Test, RejectsStructurallyImpossibleLength) {
    // A single trailing 6-bit group (length ≡ 1 mod 4) carries no full byte —
    // structurally impossible for any real encoding.
    EXPECT_THROW(url_decode("AAAAA"), std::runtime_error);  // 5 chars
    EXPECT_THROW(url_decode("A"), std::runtime_error);      // 1 char
}

TEST(Base64Test, RejectsNonZeroPaddingBits) {
    // "QQ" decodes 'A' (0x41) from 12 bits; the low 4 leftover bits must be 0.
    // The canonical encoding of {0x41} is "QQ". A variant whose final char sets
    // those leftover bits decodes to the same byte but is non-canonical → must
    // be rejected so it can't be a malleated duplicate of a valid token.
    EXPECT_EQ(url_decode("QQ"), std::string("A"));  // canonical OK
    EXPECT_THROW(url_decode("QR"), std::runtime_error) << "non-zero trailing bits must be rejected";
}

TEST(Base64Test, MalleabilityResistance) {
    // Two distinct strings must never decode to identical bytes. Encode 32
    // random-ish bytes (HMAC-sized), then append a char: the result either
    // throws or decodes differently — it must NOT silently equal the original.
    const std::string sig(32, '\x5a');
    const std::string enc = url_encode(sig);         // 43 chars for 32 bytes
    const std::string decoded_ok = url_decode(enc);  // canonical
    EXPECT_EQ(decoded_ok, sig);
    // enc has length 43 (≡ 3 mod 4). Appending one char → 44 (≡ 0 mod 4) but the
    // appended 6 bits would be dropped as a partial byte with non-zero padding,
    // or change the last byte — either way it must not equal the original sig.
    bool rejected_or_different = false;
    try {
        rejected_or_different = (url_decode(enc + "B") != sig);
    } catch (const std::exception&) {
        rejected_or_different = true;
    }
    EXPECT_TRUE(rejected_or_different);
}

}  // namespace
