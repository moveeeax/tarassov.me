#include <filesystem>

#include <gtest/gtest.h>

#include "observability/Observability.hpp"
#include "test_helpers.hpp"

// --- Logger tests ---

class LoggerTest : public ::testing::Test {
protected:
    void TearDown() override {
        spdlog::shutdown();
        TestHelpers::restore_default_spdlog();
    }
};

TEST_F(LoggerTest, InitializeConsoleOnly) {
    Observability::Logger logger;
    EXPECT_NO_THROW(logger.initialize("test_console"));
    EXPECT_NE(logger.get(), nullptr);
}

TEST_F(LoggerTest, InitializeWithFile) {
    Observability::Logger logger;
    EXPECT_NO_THROW(logger.initialize("test_file", "logs/test_logger.log"));
    EXPECT_NE(logger.get(), nullptr);
}

TEST_F(LoggerTest, SetAllLevels) {
    Observability::Logger logger;
    logger.initialize("test_levels");

    EXPECT_NO_THROW(logger.set_level("trace"));
    EXPECT_NO_THROW(logger.set_level("debug"));
    EXPECT_NO_THROW(logger.set_level("info"));
    EXPECT_NO_THROW(logger.set_level("warn"));
    EXPECT_NO_THROW(logger.set_level("error"));
    EXPECT_NO_THROW(logger.set_level("critical"));
}

TEST_F(LoggerTest, FlushNoThrow) {
    Observability::Logger logger;
    logger.initialize("test_flush");
    EXPECT_NO_THROW(logger.flush());
}

TEST_F(LoggerTest, Shutdown) {
    Observability::Logger logger;
    logger.initialize("test_shutdown");
    EXPECT_NO_THROW(logger.shutdown());
}

// --- Metrics tests ---

// All fixtures draw ports from the single process-wide allocator — local
// per-fixture counters with hand-picked bases collided with each other and
// with TestHelpers::minimal_config() in a full-binary run.
class MetricsTest : public ::testing::Test {
protected:
    static std::string get_metrics_addr() { return "0.0.0.0:" + std::to_string(TestHelpers::next_metrics_port()); }
};

TEST_F(MetricsTest, Initialize) {
    Observability::Metrics metrics;
    EXPECT_NO_THROW(metrics.initialize(get_metrics_addr()));
    EXPECT_NE(metrics.get_registry(), nullptr);
    metrics.shutdown();
}

TEST_F(MetricsTest, CreateCounter) {
    Observability::Metrics metrics;
    metrics.initialize(get_metrics_addr());

    auto& counter_family = metrics.create_counter("test_counter", "A test counter");
    auto& counter = counter_family.Add({{"label", "value"}});
    counter.Increment();
    EXPECT_DOUBLE_EQ(counter.Value(), 1.0);
    metrics.shutdown();
}

TEST_F(MetricsTest, CreateGauge) {
    Observability::Metrics metrics;
    metrics.initialize(get_metrics_addr());

    auto& gauge_family = metrics.create_gauge("test_gauge", "A test gauge");
    auto& gauge = gauge_family.Add({{"label", "value"}});
    gauge.Set(42.0);
    EXPECT_DOUBLE_EQ(gauge.Value(), 42.0);
    metrics.shutdown();
}

TEST_F(MetricsTest, CreateHistogram) {
    Observability::Metrics metrics;
    metrics.initialize(get_metrics_addr());

    auto& hist_family = metrics.create_histogram("test_histogram", "A test histogram");
    auto& hist = hist_family.Add({{"label", "value"}}, prometheus::Histogram::BucketBoundaries{0.1, 1.0, 10.0});
    hist.Observe(0.5);
    // No crash is the success criterion
    metrics.shutdown();
}

// --- Tracer tests ---

class TracerTest : public ::testing::Test {};

TEST_F(TracerTest, Initialize) {
    Observability::Tracer tracer;
    EXPECT_NO_THROW(tracer.initialize("test_service"));
    tracer.shutdown();
}

TEST_F(TracerTest, GetTracer) {
    Observability::Tracer tracer;
    tracer.initialize("test_service");
    auto t = tracer.get_tracer("test_tracer");
    EXPECT_TRUE(t != nullptr);
    tracer.shutdown();
}

TEST_F(TracerTest, CreateSpan) {
    Observability::Tracer tracer;
    tracer.initialize("test_service");
    auto t = tracer.get_tracer("test_tracer");
    auto span = t->StartSpan("test_span");
    EXPECT_TRUE(span != nullptr);
    span->End();
    tracer.shutdown();
}

// --- ObservabilitySystem tests ---

class ObservabilitySystemTest : public ::testing::Test {
protected:
    static std::string get_metrics_addr() { return "0.0.0.0:" + std::to_string(TestHelpers::next_metrics_port()); }

    void TearDown() override { TestHelpers::reset_all_globals(); }
};

TEST_F(ObservabilitySystemTest, Initialize) {
    Observability::ObservabilitySystem sys;
    EXPECT_NO_THROW(sys.initialize("obs_test", "logs/obs_test.log", get_metrics_addr(), "obs_test_svc"));
    EXPECT_TRUE(sys.is_initialized());
    sys.shutdown();
    EXPECT_FALSE(sys.is_initialized());
}

TEST_F(ObservabilitySystemTest, DoubleInitThrows) {
    Observability::ObservabilitySystem sys;
    sys.initialize("obs_test2", "logs/obs_test2.log", get_metrics_addr(), "obs_test_svc2");
    EXPECT_THROW(sys.initialize("obs_test2", "", get_metrics_addr(), ""), std::runtime_error);
    sys.shutdown();
}

TEST_F(ObservabilitySystemTest, AccessSubsystems) {
    Observability::ObservabilitySystem sys;
    sys.initialize("obs_test3", "logs/obs_test3.log", get_metrics_addr(), "obs_test_svc3");

    EXPECT_NE(sys.logger().get(), nullptr);
    EXPECT_NE(sys.metrics().get_registry(), nullptr);

    sys.shutdown();
}

// --- Global singleton tests ---

class ObservabilityGlobalTest : public ::testing::Test {
protected:
    static std::string get_metrics_addr() { return "0.0.0.0:" + std::to_string(TestHelpers::next_metrics_port()); }

    void TearDown() override { TestHelpers::reset_all_globals(); }
};

TEST_F(ObservabilityGlobalTest, GlobalInitAndShutdown) {
    EXPECT_FALSE(Observability::is_initialized());
    Observability::initialize("global_test", "logs/global_test.log", get_metrics_addr(), "global_svc");
    EXPECT_TRUE(Observability::is_initialized());

    Observability::shutdown();
    EXPECT_FALSE(Observability::is_initialized());
}

TEST_F(ObservabilityGlobalTest, GlobalDoubleInitThrows) {
    Observability::initialize("global_test2", "logs/global_test2.log", get_metrics_addr(), "global_svc2");
    EXPECT_THROW(Observability::initialize("global_test2b", "", get_metrics_addr(), ""), std::runtime_error);
}

TEST_F(ObservabilityGlobalTest, GlobalGetBeforeInitThrows) {
    EXPECT_THROW(Observability::get(), std::runtime_error);
}

TEST_F(ObservabilityGlobalTest, GlobalShutdownIdempotent) {
    Observability::initialize("global_test3", "logs/global_test3.log", get_metrics_addr(), "global_svc3");
    EXPECT_NO_THROW(Observability::shutdown());
    EXPECT_NO_THROW(Observability::shutdown());
}
