#include <gtest/gtest.h>

#include "observability/Trace.hpp"

namespace T = Observability::Trace;

TEST(TraceTest, ParsesValidTraceparent) {
    auto tc = T::parse_traceparent("00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
    ASSERT_TRUE(tc.has_value());
    EXPECT_EQ(tc->trace_id, "0af7651916cd43dd8448eb211c80319c");
    EXPECT_EQ(tc->parent_id, "b7ad6b7169203331");
    EXPECT_EQ(tc->flags, "01");
}

TEST(TraceTest, NormalizesMixedCase) {
    auto tc = T::parse_traceparent("00-0AF7651916CD43DD8448EB211C80319C-B7AD6B7169203331-01");
    ASSERT_TRUE(tc.has_value());
    EXPECT_EQ(tc->trace_id, "0af7651916cd43dd8448eb211c80319c");
}

TEST(TraceTest, RejectsUnknownVersion) {
    auto tc = T::parse_traceparent("ff-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01");
    EXPECT_FALSE(tc.has_value());
}

TEST(TraceTest, RejectsAllZeroTraceId) {
    auto tc = T::parse_traceparent("00-00000000000000000000000000000000-b7ad6b7169203331-01");
    EXPECT_FALSE(tc.has_value());
}

TEST(TraceTest, RejectsAllZeroSpanId) {
    auto tc = T::parse_traceparent("00-0af7651916cd43dd8448eb211c80319c-0000000000000000-01");
    EXPECT_FALSE(tc.has_value());
}

TEST(TraceTest, RejectsBadHex) {
    auto tc = T::parse_traceparent("00-zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz-b7ad6b7169203331-01");
    EXPECT_FALSE(tc.has_value());
}

TEST(TraceTest, RejectsWrongLength) {
    auto tc = T::parse_traceparent("00-abc-def-01");
    EXPECT_FALSE(tc.has_value());
}

TEST(TraceTest, GeneratesValidContext) {
    auto ctx = T::generate_context();
    EXPECT_EQ(ctx.trace_id.size(), 32u);
    EXPECT_EQ(ctx.parent_id.size(), 16u);
    EXPECT_EQ(ctx.flags, "01");
    // Round-trip: format → parse must succeed.
    auto s = T::format_traceparent(ctx);
    auto round = T::parse_traceparent(s);
    ASSERT_TRUE(round.has_value());
    EXPECT_EQ(round->trace_id, ctx.trace_id);
}

TEST(TraceTest, GeneratedContextsAreUnique) {
    auto a = T::generate_context();
    auto b = T::generate_context();
    EXPECT_NE(a.trace_id, b.trace_id);
    EXPECT_NE(a.parent_id, b.parent_id);
}

TEST(TraceTest, ExtractOrGenerateFallsBack) {
    auto ctx = T::extract_or_generate("");
    EXPECT_EQ(ctx.trace_id.size(), 32u);
    auto ctx2 = T::extract_or_generate("not-valid");
    EXPECT_EQ(ctx2.trace_id.size(), 32u);
}

// Ambient traceparent: the bridge that lets Jobs::submit (OTel-decoupled) carry
// the request's trace context to the worker. Set by the HTTP advice, read by
// submit, cleared in post-advice.
TEST(TraceTest, AmbientTraceparentDefaultsEmpty) {
    T::clear_current_traceparent();
    EXPECT_TRUE(T::current_traceparent().empty());
}

TEST(TraceTest, AmbientTraceparentRoundTrips) {
    T::TraceContext ctx{"0af7651916cd43dd8448eb211c80319c", "b7ad6b7169203331", "01"};
    T::set_current_traceparent(ctx);
    EXPECT_EQ(T::current_traceparent(), T::format_traceparent(ctx));
    // A parse of what submit would stamp must reconstruct the same context.
    auto parsed = T::parse_traceparent(T::current_traceparent());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->trace_id, ctx.trace_id);
    EXPECT_EQ(parsed->parent_id, ctx.parent_id);
    T::clear_current_traceparent();
    EXPECT_TRUE(T::current_traceparent().empty());
}
