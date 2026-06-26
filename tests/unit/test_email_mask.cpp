/**
 * @file test_email_mask.cpp
 * @brief Unit tests for Utils::Strings::mask_email — keep logs free of raw PII.
 */

#include <gtest/gtest.h>

#include "utils/Strings.hpp"

TEST(EmailMaskTest, KeepsFirstLocalCharAndDomain) {
    EXPECT_EQ(Utils::Strings::mask_email("john.doe@example.com"), "j***@example.com");
}

TEST(EmailMaskTest, FullyMasksSingleCharLocalPart) {
    // A 1-char local part must not be recoverable.
    EXPECT_EQ(Utils::Strings::mask_email("a@example.com"), "***@example.com");
}

TEST(EmailMaskTest, EmptyStays) {
    EXPECT_EQ(Utils::Strings::mask_email(""), "");
}

TEST(EmailMaskTest, NoAtSignIsTreatedAsLocalPart) {
    EXPECT_EQ(Utils::Strings::mask_email("notanemail"), "n***");
}
