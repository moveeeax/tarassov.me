#include <gtest/gtest.h>

#include "database/Database.hpp"
#include "test_helpers.hpp"

static const std::string PG_CONN = TestHelpers::pg_conn_string();

// --- ConnectionPool tests ---

class ConnectionPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!TestHelpers::is_postgres_available()) {
            GTEST_SKIP() << "PostgreSQL not available";
        }
    }

    void TearDown() override { TestHelpers::reset_all_globals(); }
};

TEST_F(ConnectionPoolTest, Create) {
    Database::ConnectionPool pool(PG_CONN, 2);
    EXPECT_EQ(pool.size(), 2u);
    EXPECT_EQ(pool.active_count(), 0u);
    pool.shutdown();
}

TEST_F(ConnectionPoolTest, AcquireAndRelease) {
    Database::ConnectionPool pool(PG_CONN, 2);
    auto conn = pool.acquire();
    EXPECT_EQ(pool.active_count(), 1u);
    EXPECT_TRUE(conn->is_open());

    pool.release(std::move(conn));
    EXPECT_EQ(pool.active_count(), 0u);
    pool.shutdown();
}

// Regression: prior to lazy-fill, releasing a broken (null) connection would
// shrink the pool by one and never refill, eventually deadlocking acquire().
// With the fix, a fresh acquire() lazy-creates the replacement on demand.
TEST_F(ConnectionPoolTest, LazyFillAfterAllConnectionsBroken) {
    Database::ConnectionPool pool(PG_CONN, 2, std::chrono::seconds(2));

    auto c1 = pool.acquire();
    auto c2 = pool.acquire();
    EXPECT_EQ(pool.active_count(), 2u);

    // Simulate both connections broken in flight (dropped before release).
    pool.release(nullptr);
    pool.release(nullptr);
    EXPECT_EQ(pool.active_count(), 0u);

    // Old code: queue empty, predicate never satisfied → timeout.
    // New code: lazy-fill creates a fresh connection.
    auto c3 = pool.acquire();
    ASSERT_TRUE(c3 != nullptr);
    EXPECT_TRUE(c3->is_open());

    auto c4 = pool.acquire();
    ASSERT_TRUE(c4 != nullptr);
    EXPECT_TRUE(c4->is_open());

    pool.release(std::move(c3));
    pool.release(std::move(c4));
    pool.shutdown();
}

TEST_F(ConnectionPoolTest, PooledConnectionRAII) {
    Database::ConnectionPool pool(PG_CONN, 2);
    {
        Database::PooledConnection pc(pool);
        EXPECT_TRUE(pc.get() != nullptr);
        EXPECT_TRUE(pc->is_open());
        EXPECT_EQ(pool.active_count(), 1u);
    }
    // Connection returned after scope exit
    EXPECT_EQ(pool.active_count(), 0u);
    pool.shutdown();
}

// --- DatabaseManager tests ---

// Lifecycle tests drive initialize/shutdown themselves — no auto-init.
class DatabaseLifecycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!TestHelpers::is_postgres_available()) {
            GTEST_SKIP() << "PostgreSQL not available";
        }
    }

    void TearDown() override { TestHelpers::reset_all_globals(); }
};

// Operation tests get an initialized manager from the fixture.
class DatabaseManagerTest : public DatabaseLifecycleTest {
protected:
    void SetUp() override {
        DatabaseLifecycleTest::SetUp();
        if (::testing::Test::IsSkipped())
            return;
        Database::initialize(PG_CONN, {}, 2);
    }

    void TearDown() override {
        // Drop test table if it was created
        try {
            if (Database::is_initialized()) {
                Database::get().execute_write([](auto& txn) {
                    txn.exec("DROP TABLE IF EXISTS test_table");
                    return 0;
                });
            }
        } catch (...) {}
        TestHelpers::reset_all_globals();
    }
};

TEST_F(DatabaseLifecycleTest, InitializeAndShutdown) {
    Database::initialize(PG_CONN, {}, 2);
    EXPECT_TRUE(Database::is_initialized());

    Database::shutdown();
    EXPECT_FALSE(Database::is_initialized());
}

TEST_F(DatabaseLifecycleTest, DoubleInitThrows) {
    Database::initialize(PG_CONN, {}, 2);
    EXPECT_THROW(Database::initialize(PG_CONN, {}, 2), std::runtime_error);
}

TEST_F(DatabaseLifecycleTest, GetBeforeInitThrows) {
    EXPECT_THROW(Database::get(), std::runtime_error);
}

TEST_F(DatabaseManagerTest, HealthCheck) {
    EXPECT_TRUE(Database::get().health_check());
}

TEST_F(DatabaseManagerTest, ExecuteRead) {
    auto result = Database::get().execute_read([](auto& txn) { return txn.exec("SELECT 1 AS val"); });
    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0][0].template as<int>(), 1);
}

TEST_F(DatabaseManagerTest, ExecuteWrite) {
    Database::get().execute_write([](auto& txn) {
        txn.exec("CREATE TABLE IF NOT EXISTS test_table (id SERIAL PRIMARY KEY, name TEXT)");
        return 0;
    });

    Database::get().execute_write([](auto& txn) {
        txn.exec("INSERT INTO test_table (name) VALUES ('test_value')");
        return 0;
    });

    auto result = Database::get().execute_read(
        [](auto& txn) { return txn.exec("SELECT name FROM test_table WHERE name = 'test_value'"); });
    EXPECT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0][0].template as<std::string>(), "test_value");
}

TEST_F(DatabaseLifecycleTest, UnreachableReplicaDoesNotBlockBoot) {
    // Replica is an optional read optimization: a dead replica URL must not
    // fail initialization (it used to crash-loop the whole app). Reads then
    // serve from the primary.
    Database::initialize(PG_CONN, {"postgresql://postgres:postgres@no-such-replica-host:5432/appdb"}, 2);
    EXPECT_TRUE(Database::is_initialized());
    auto result = Database::get().execute_read([](auto& txn) { return txn.exec("SELECT 1 AS one"); });
    EXPECT_EQ(result[0][0].template as<int>(), 1);
}

TEST_F(DatabaseManagerTest, ReplicaFallbackToPrimary) {
    // Fixture initializes without replicas — get_replica() must fall back
    // to the primary pool.
    auto conn = Database::get().get_replica();
    EXPECT_TRUE(conn.get() != nullptr);
    EXPECT_TRUE(conn->is_open());
}

TEST_F(DatabaseLifecycleTest, ShutdownIdempotent) {
    Database::initialize(PG_CONN, {}, 2);
    EXPECT_NO_THROW(Database::shutdown());
    EXPECT_NO_THROW(Database::shutdown());
}
