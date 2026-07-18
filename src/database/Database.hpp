/**
 * @file Database.hpp
 * @brief Database module for PostgreSQL integration
 * @details Provides async database operations using libpqxx with connection pooling
 *          and support for read replicas
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <pqxx/pqxx>
#include <queue>
#include <stdexcept>
#include <string>
#include <vector>

#include <opentelemetry/trace/provider.h>
#include <spdlog/spdlog.h>

#include "utils/Retry.hpp"

namespace Database {

namespace detail {

// RAII helper: opens a `db.<op>` child span if tracing is up. The parent
// comes from the active OTel RuntimeContext: the HTTP tracing middleware
// (Api.hpp) and the worker's job span both attach their span there, so
// db.* spans nest under the request / job instead of floating as orphan
// single-span traces. `pool` says which pool actually served the call —
// the answer to "are reads really hitting the replica?" lives right on
// the span.
struct AutoSpan {
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span_;
    AutoSpan(const char* name, const char* pool) {
        // Read the GLOBAL provider (Observability::initialize installs it via
        // Provider::SetTracerProvider) instead of including Observability.hpp —
        // that header drags the whole OTel SDK + exporters + prometheus into
        // every TU that touches the database. Before initialization this
        // yields the no-op tracer, so spans cost nothing.
        auto tracer = opentelemetry::trace::Provider::GetTracerProvider()->GetTracer("db");
        span_ = tracer->StartSpan(name, {{"db.system", "postgresql"}, {"db.pool", pool}});
    }
    ~AutoSpan() {
        if (span_)
            span_->End();
    }
};

/**
 * @brief Transparent transaction proxy that records every executed
 *        statement onto the surrounding db.* span as `db.statement`.
 *
 * SAFE BY CONSTRUCTION: repositories use exec_params everywhere, so the
 * recorded text is the QUERY TEMPLATE with $1/$2 placeholders — parameter
 * values (emails, hashes, tokens) never reach the trace. This matches the
 * OTel semantic convention for db.query.text.
 *
 * Repositories receive this proxy as `auto& txn` — only exec / exec_params
 * are part of the contract; reach the underlying transaction via raw() if
 * you genuinely need more.
 */
template <typename TxnT>
class TracingTxn {
public:
    TracingTxn(TxnT& txn, opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span)
        : txn_(txn), span_(std::move(span)) {}

    ~TracingTxn() {
        if (span_ && count_ > 0) {
            span_->SetAttribute("db.statement", truncated_());
            span_->SetAttribute("db.statement_count", static_cast<int64_t>(count_));
        }
    }

    TracingTxn(const TracingTxn&) = delete;
    TracingTxn& operator=(const TracingTxn&) = delete;

    template <typename... Args>
    auto exec_params(const std::string& query, Args&&... args) {
        record_(query);
        // libpqxx deprecated exec_params(query, args...) in favour of
        // exec(query, params). Keep this wrapper's name/signature (callers are
        // unchanged) and translate the pack into a pqxx::params here.
        return txn_.exec(query, pqxx::params{std::forward<Args>(args)...});
    }

    auto exec(const std::string& query) {
        record_(query);
        return txn_.exec(query);
    }

    /// Escape hatch to the real pqxx transaction.
    TxnT& raw() { return txn_; }

private:
    void record_(const std::string& q) {
        ++count_;
        if (!statements_.empty())
            statements_ += "; ";
        statements_ += q;
    }

    std::string truncated_() const {
        constexpr size_t kMaxAttrLen = 2048;
        if (statements_.size() <= kMaxAttrLen)
            return statements_;
        return statements_.substr(0, kMaxAttrLen) + "…";
    }

    TxnT& txn_;
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span_;
    std::string statements_;
    size_t count_ = 0;
};

}  // namespace detail

// Pluggable query-counter sink, mirroring Retry::bind_metrics: Core wires a
// Prometheus counter here (db_queries_total{op, pool}) without this header
// depending on prometheus directly. The pool label is the live answer to
// "are reads actually hitting the replica?" on dashboards.
using QueryMetricFn = void (*)(const char* op, const char* pool);
// Bound on the main thread, read from every IO/worker thread — atomic to
// publish the pointer without a data race.
inline std::atomic<QueryMetricFn> query_metric_sink_{nullptr};

inline void bind_query_metrics(QueryMetricFn fn) {
    query_metric_sink_.store(fn, std::memory_order_relaxed);
}

// Unbind before Observability teardown — same dangling-family hazard as Retry.
inline void reset_query_metrics() {
    query_metric_sink_.store(nullptr, std::memory_order_relaxed);
}

inline void inc_query_metric_(const char* op, const char* pool) {
    if (auto fn = query_metric_sink_.load(std::memory_order_relaxed)) {
        try {
            fn(op, pool);
        } catch (...) {}
    }
}

/**
 * @brief PostgreSQL transaction isolation level
 * @details Maps to standard SQL isolation levels. Choose the weakest level
 *          that satisfies your invariants:
 *          - ReadCommitted: default. Each statement sees a snapshot at its start.
 *          - RepeatableRead: snapshot at transaction start. Use for read-modify-write
 *            on the same row.
 *          - Serializable: full serializability. Use for cross-row invariants;
 *            be ready to retry on serialization_failure (40001).
 */
enum class IsolationLevel {
    ReadCommitted,
    RepeatableRead,
    Serializable,
};

inline const char* isolation_sql(IsolationLevel level) {
    switch (level) {
        case IsolationLevel::ReadCommitted:
            return "SET TRANSACTION ISOLATION LEVEL READ COMMITTED";
        case IsolationLevel::RepeatableRead:
            return "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ";
        case IsolationLevel::Serializable:
            return "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE";
    }
    return "SET TRANSACTION ISOLATION LEVEL READ COMMITTED";
}

/**
 * @brief Connection pool for database connections
 */
class ConnectionPool {
private:
    std::string connection_string_;
    std::queue<std::unique_ptr<pqxx::connection>> available_connections_;
    std::mutex pool_mutex_;
    std::condition_variable cv_;
    size_t pool_size_;
    std::atomic<size_t> active_connections_{0};
    std::chrono::milliseconds acquire_timeout_;
    std::chrono::milliseconds statement_timeout_;

    // Apply session-level guards to a freshly opened connection.
    // statement_timeout is the only guard we set today, but this is the
    // hook for adding lock_timeout, idle_in_transaction_session_timeout, etc.
    void apply_session_guards(pqxx::connection& c) {
        if (statement_timeout_.count() <= 0)
            return;
        try {
            pqxx::nontransaction n(c);
            n.exec("SET statement_timeout = " + std::to_string(statement_timeout_.count()));
        } catch (const std::exception& e) {
            // Non-fatal: a connection without statement_timeout is still
            // usable; we just lose the per-query DoS guardrail.
            spdlog::warn("Failed to apply statement_timeout: {}", e.what());
        }
    }

public:
    /// Re-apply the per-connection session guards (statement_timeout). Call after
    /// code that mutated session state — e.g. a no-transaction migration that did
    /// `SET statement_timeout = 0` — so the next borrower of this pooled
    /// connection doesn't inherit the cleared timeout.
    void reapply_session_guards(pqxx::connection& c) { apply_session_guards(c); }

    /**
     * @brief Construct connection pool
     * @param conn_str PostgreSQL connection string
     * @param size Number of connections in pool
     * @param timeout Max time to wait for a free connection
     * @param stmt_timeout Per-connection PostgreSQL statement_timeout. 0 disables.
     */
    ConnectionPool(const std::string& conn_str,
                   size_t size = 10,
                   std::chrono::milliseconds timeout = std::chrono::seconds(5),
                   std::chrono::milliseconds stmt_timeout = std::chrono::milliseconds(0))
        : connection_string_(conn_str), pool_size_(size), acquire_timeout_(timeout), statement_timeout_(stmt_timeout) {
        try {
            for (size_t i = 0; i < pool_size_; ++i) {
                auto conn = std::make_unique<pqxx::connection>(connection_string_);
                apply_session_guards(*conn);
                available_connections_.push(std::move(conn));
            }
            spdlog::info(
                "Database connection pool initialized with {} connections "
                "(acquire_timeout={}ms, statement_timeout={}ms)",
                pool_size_,
                acquire_timeout_.count(),
                statement_timeout_.count());
        } catch (const std::exception& e) {
            spdlog::error("Failed to initialize connection pool: {}", e.what());
            throw;
        }
    }

    /**
     * @brief Acquire a connection from the pool.
     * @details Three-way wait: take a queued connection, or — if total
     *          live connections (queued + in-use) is still below pool_size —
     *          create a fresh one on demand. The on-demand path lets the
     *          pool self-heal after release() drops broken connections,
     *          so a network blip can't permanently shrink the pool.
     * @return Unique pointer to a usable connection
     * @throws std::runtime_error if no connection is available within the timeout
     */
    std::unique_ptr<pqxx::connection> acquire() {
        std::unique_lock<std::mutex> lock(pool_mutex_);

        const auto can_proceed = [this] {
            return !available_connections_.empty() ||
                   (available_connections_.size() + active_connections_) < pool_size_;
        };

        if (!cv_.wait_for(lock, acquire_timeout_, can_proceed)) {
            throw std::runtime_error(
                "Timed out waiting for database connection from pool (size=" + std::to_string(pool_size_) +
                ", active=" + std::to_string(active_connections_.load()) + ")");
        }

        if (!available_connections_.empty()) {
            auto conn = std::move(available_connections_.front());
            available_connections_.pop();
            ++active_connections_;
            return conn;
        }

        // Lazy-fill path: reserve the slot under the lock, then connect
        // outside it so a slow handshake can't block other waiters.
        ++active_connections_;
        lock.unlock();
        try {
            auto conn = std::make_unique<pqxx::connection>(connection_string_);
            apply_session_guards(*conn);
            return conn;
        } catch (...) {
            std::lock_guard<std::mutex> relock(pool_mutex_);
            --active_connections_;
            cv_.notify_one();
            throw;
        }
    }

    /**
     * @brief Release connection back to pool.
     * @details Healthy connections go back on the queue. Broken ones are
     *          dropped — the next acquire() will lazy-fill a replacement.
     *          We deliberately do NOT reconnect synchronously here: a
     *          slow handshake under the mutex blocked every other caller,
     *          and a throwing reconnect used to permanently shrink the
     *          pool until it deadlocked.
     * @param conn Connection to release (may be null or closed)
     */
    void release(std::unique_ptr<pqxx::connection> conn) {
        std::lock_guard<std::mutex> lock(pool_mutex_);

        if (conn && conn->is_open()) {
            available_connections_.push(std::move(conn));
        }
        // else: drop the bad connection silently. pool_size is the cap;
        // acquire() will create a replacement on the next request.

        --active_connections_;
        cv_.notify_one();
    }

    size_t size() const { return pool_size_; }
    size_t active_count() const { return active_connections_.load(); }

    void shutdown() {
        std::unique_lock<std::mutex> lock(pool_mutex_);
        // Drain in-use connections before tearing down. A PooledConnection still
        // alive on another thread calls pool->release() in its dtor; once
        // DatabaseManager resets the pool's unique_ptr that would be a
        // use-after-free. In the correct shutdown order (Drogon loop stopped,
        // handlers finished) active is already 0 — the bounded wait is a safety
        // net, not the happy path.
        if (active_connections_.load() > 0) {
            cv_.wait_for(lock, std::chrono::seconds(5), [this] { return active_connections_.load() == 0; });
            if (active_connections_.load() > 0)
                spdlog::warn("Database pool shutdown with {} connection(s) still in use — potential leak",
                             active_connections_.load());
        }
        while (!available_connections_.empty()) {
            available_connections_.pop();
        }
        spdlog::info("Database connection pool shut down");
    }
};

/**
 * @brief RAII wrapper for connection pool
 */
class PooledConnection {
private:
    std::unique_ptr<pqxx::connection> conn;
    ConnectionPool* pool;

public:
    explicit PooledConnection(ConnectionPool& p) : pool(&p) { conn = pool->acquire(); }

    ~PooledConnection() {
        // Both guards: a moved-from instance has conn==null AND pool==null;
        // checking both keeps the dtor correct even if the move invariant
        // ever changes (release on a null pool would be a use-after-free).
        if (conn && pool) {
            pool->release(std::move(conn));
        }
    }

    PooledConnection(const PooledConnection&) = delete;
    PooledConnection& operator=(const PooledConnection&) = delete;

    PooledConnection(PooledConnection&& other) noexcept : conn(std::move(other.conn)), pool(other.pool) {
        other.pool = nullptr;
    }

    pqxx::connection& operator*() { return *conn; }
    pqxx::connection* operator->() { return conn.get(); }
    pqxx::connection* get() { return conn.get(); }
};

/**
 * @brief Database manager with primary and replica support
 */
class DatabaseManager {
private:
    std::unique_ptr<ConnectionPool> primary_pool_;
    std::vector<std::unique_ptr<ConnectionPool>> replica_pools_;
    std::atomic<size_t> current_replica_{0};
    bool initialized_ = false;
    Retry::Policy retry_policy_;

public:
    void set_retry_policy(const Retry::Policy& p) { retry_policy_ = p; }
    const Retry::Policy& retry_policy() const { return retry_policy_; }

    void initialize(const std::string& primary_conn,
                    const std::vector<std::string>& replica_conns = {},
                    size_t pool_size = 10,
                    std::chrono::milliseconds acquire_timeout = std::chrono::seconds(5),
                    std::chrono::milliseconds statement_timeout = std::chrono::milliseconds(0)) {
        if (initialized_) {
            throw std::runtime_error("Database already initialized");
        }

        primary_pool_ = std::make_unique<ConnectionPool>(primary_conn, pool_size, acquire_timeout, statement_timeout);
        spdlog::info("Primary database pool initialized");

        // Replicas are an OPTIONAL read optimization — a replica that is
        // down/lagging at boot must not take the whole service with it
        // (verified the hard way: a stopped replica container crash-looped
        // the app even though the primary was healthy). Warn and continue;
        // execute_read falls back to the primary.
        for (const auto& replica_conn : replica_conns) {
            try {
                replica_pools_.push_back(
                    std::make_unique<ConnectionPool>(replica_conn, pool_size, acquire_timeout, statement_timeout));
            } catch (const std::exception& e) {
                spdlog::warn("Replica pool unavailable — continuing without it (reads go to primary): {}", e.what());
            }
        }

        if (!replica_pools_.empty()) {
            spdlog::info("Initialized {} replica pool(s)", replica_pools_.size());
        }

        initialized_ = true;
    }

    PooledConnection get_primary() {
        if (!initialized_ || !primary_pool_) {
            throw std::runtime_error("Database not initialized");
        }
        return PooledConnection(*primary_pool_);
    }

    PooledConnection get_replica() {
        if (!initialized_) {
            throw std::runtime_error("Database not initialized");
        }
        return PooledConnection(pick_replica_pool_());
    }

    /**
     * @brief Internal core: open a transaction of TxnT on the given pool and run @p func.
     *        @p classifier controls retry semantics; @p span_name labels the OTel span.
     */
    template <typename TxnT, typename Func, typename Pred>
    auto execute_with_(ConnectionPool& pool,
                       Pred classifier,
                       const char* span_name,
                       const char* pool_label,
                       Func&& func,
                       IsolationLevel level = IsolationLevel::ReadCommitted)
        -> decltype(func(std::declval<detail::TracingTxn<TxnT>&>())) {
        detail::AutoSpan _span(span_name, pool_label);
        // Strip the "db." prefix for the metric op label, but only if it's
        // actually there — don't blindly skip 3 chars (UB on a shorter name).
        const char* op_label =
            (span_name[0] == 'd' && span_name[1] == 'b' && span_name[2] == '.') ? span_name + 3 : span_name;
        inc_query_metric_(op_label, pool_label);
        return Retry::run(
            [&] {
                PooledConnection conn(pool);
                TxnT txn(*conn);
                if constexpr (std::is_same_v<TxnT, pqxx::work>) {
                    if (level != IsolationLevel::ReadCommitted) {
                        txn.exec(isolation_sql(level));
                    }
                }
                try {
                    // The proxy records every statement onto the db.* span
                    // (placeholders only — exec_params keeps values out).
                    detail::TracingTxn<TxnT> traced(txn, _span.span_);
                    auto result = func(traced);
                    txn.commit();
                    return result;
                } catch (const std::exception& e) {
                    if (!classifier(e)) {
                        spdlog::error("[{}] transaction failed: {}", span_name, e.what());
                    }
                    throw;
                }
            },
            classifier,
            retry_policy_,
            span_name);
    }

    /**
     * @brief Conservative write: only 40001/40P01 retried (PG-confirmed
     *        rollbacks). Connection-class errors NOT retried — a commit in
     *        flight could have landed on the server, retry would double-insert.
     */
    template <typename Func>
    auto execute_write(Func&& func) -> decltype(func(std::declval<detail::TracingTxn<pqxx::work>&>())) {
        return execute_with_<pqxx::work>(*primary_pool_or_throw_(),
                                         &Retry::is_transient_pqxx_write,
                                         "db.write",
                                         "primary",
                                         std::forward<Func>(func));
    }

    /**
     * @brief Like execute_write but uses the liberal read-style classifier.
     *        Safe ONLY for idempotent writes (UPSERT, DELETE by PK, etc.).
     */
    template <typename Func>
    auto execute_write_idempotent(Func&& func) -> decltype(func(std::declval<detail::TracingTxn<pqxx::work>&>())) {
        return execute_with_<pqxx::work>(*primary_pool_or_throw_(),
                                         &Retry::is_transient_pqxx_read,
                                         "db.write.idempotent",
                                         "primary",
                                         std::forward<Func>(func));
    }

    /**
     * @brief Run multiple statements in a single transaction at @p level.
     */
    template <typename Func>
    auto execute_transaction(IsolationLevel level, Func&& func)
        -> decltype(func(std::declval<detail::TracingTxn<pqxx::work>&>())) {
        return execute_with_<pqxx::work>(*primary_pool_or_throw_(),
                                         &Retry::is_transient_pqxx_write,
                                         "db.transaction",
                                         "primary",
                                         std::forward<Func>(func),
                                         level);
    }

    /// Convenience overload — defaults to ReadCommitted.
    template <typename Func>
    auto execute_transaction(Func&& func) -> decltype(func(std::declval<detail::TracingTxn<pqxx::work>&>())) {
        return execute_transaction(IsolationLevel::ReadCommitted, std::forward<Func>(func));
    }

    /// Borrow a PRIMARY connection and hand the RAW pqxx::connection to @p func —
    /// no transaction wrapper, no retry, no tracing. For the rare statement that
    /// must run in autocommit (CREATE INDEX CONCURRENTLY in a no-transaction
    /// migration). The pool's session guards (statement_timeout) are re-applied
    /// when func returns, so a `SET statement_timeout = 0` can't leak to the next
    /// borrower. Most callers want execute_write/execute_read instead.
    template <typename Func>
    auto with_primary_connection(Func&& func) -> decltype(func(std::declval<pqxx::connection&>())) {
        auto& pool = *primary_pool_or_throw_();
        PooledConnection conn(pool);
        struct GuardRestore {
            ConnectionPool& p;
            pqxx::connection& c;
            ~GuardRestore() {
                try {
                    p.reapply_session_guards(c);
                } catch (...) {}
            }
        } restore{pool, *conn};
        return func(*conn);
    }

    /**
     * @brief Read that MUST observe the latest committed write — always runs on
     *        the primary, never a (possibly lagging) replica. Use right after a
     *        write when the same request/job re-reads the row it just wrote
     *        (read-after-write); on an HA cluster a replica can otherwise return
     *        a stale row or a not-yet-replicated "not found".
     */
    template <typename Func>
    auto execute_read_primary(Func&& func)
        -> decltype(func(std::declval<detail::TracingTxn<pqxx::read_transaction>&>())) {
        return execute_with_<pqxx::read_transaction>(
            *primary_pool_or_throw_(), &Retry::is_transient_pqxx_read, "db.read", "primary", std::forward<Func>(func));
    }

    template <typename Func>
    auto execute_read(Func&& func) -> decltype(func(std::declval<detail::TracingTxn<pqxx::read_transaction>&>())) {
        if (replica_pools_.empty()) {
            return execute_with_<pqxx::read_transaction>(*primary_pool_or_throw_(),
                                                         &Retry::is_transient_pqxx_read,
                                                         "db.read",
                                                         "primary",
                                                         std::forward<Func>(func));
        }
        // Replica path with a one-shot primary fallback: if the replica
        // fails past the retry budget mid-flight, a read must degrade to
        // the primary, not surface as a 500. Reads are idempotent, so the
        // duplicate attempt is safe. (func is invoked at most once per
        // path — no forward on the first call so it stays reusable.)
        try {
            return execute_with_<pqxx::read_transaction>(
                replica_pool_or_throw_(), &Retry::is_transient_pqxx_read, "db.read", "replica", func);
        } catch (const std::exception& e) {
            spdlog::warn("execute_read via replica failed ({}); falling back to primary", e.what());
            return execute_with_<pqxx::read_transaction>(*primary_pool_or_throw_(),
                                                         &Retry::is_transient_pqxx_read,
                                                         "db.read",
                                                         "primary",
                                                         std::forward<Func>(func));
        }
    }

private:
    ConnectionPool* primary_pool_or_throw_() {
        if (!initialized_ || !primary_pool_)
            throw std::runtime_error("Database not initialized");
        return primary_pool_.get();
    }

    ConnectionPool& replica_pool_or_throw_() {
        if (!initialized_)
            throw std::runtime_error("Database not initialized");
        return pick_replica_pool_();
    }

    // Single source of truth for replica selection: round-robin across the
    // replica pools, falling back to the primary when none are configured.
    ConnectionPool& pick_replica_pool_() {
        if (replica_pools_.empty())
            return *primary_pool_;
        size_t idx = current_replica_.fetch_add(1, std::memory_order_relaxed) % replica_pools_.size();
        return *replica_pools_[idx];
    }

public:
    bool health_check() {
        try {
            auto conn = get_primary();
            pqxx::nontransaction ntxn(*conn);
            ntxn.exec("SELECT 1");
            return true;
        } catch (const std::exception& e) {
            spdlog::error("Database health check failed: {}", e.what());
            return false;
        }
    }

    void shutdown() {
        if (initialized_) {
            spdlog::info("Shutting down database manager");

            for (auto& replica : replica_pools_) {
                replica->shutdown();
            }
            replica_pools_.clear();

            if (primary_pool_) {
                primary_pool_->shutdown();
                primary_pool_.reset();
            }

            initialized_ = false;
        }
    }

    bool is_initialized() const { return initialized_; }

    // ── Pool saturation introspection ─────────────────────────────────────
    // The connection pool already tracks active_count()/size(); these surface
    // them so Core can export db_pool_* gauges. Saturation (active/size → 1.0)
    // is the leading indicator that acquire() is about to start timing out —
    // the cause the HighP99Latency alert tells operators to check first.
    size_t primary_pool_active() const { return primary_pool_ ? primary_pool_->active_count() : 0; }
    size_t primary_pool_size() const { return primary_pool_ ? primary_pool_->size() : 0; }
    size_t replica_count() const { return replica_pools_.size(); }
    size_t replica_pool_active() const {
        size_t n = 0;
        for (const auto& r : replica_pools_)
            n += r->active_count();
        return n;
    }
    size_t replica_pool_size() const {
        size_t n = 0;
        for (const auto& r : replica_pools_)
            n += r->size();
        return n;
    }
};

/**
 * @brief Global database instance
 */
inline std::unique_ptr<DatabaseManager> global_db = nullptr;

inline void initialize(const std::string& primary_conn,
                       const std::vector<std::string>& replica_conns = {},
                       size_t pool_size = 10,
                       std::chrono::milliseconds acquire_timeout = std::chrono::seconds(5),
                       std::chrono::milliseconds statement_timeout = std::chrono::milliseconds(0)) {
    if (global_db != nullptr) {
        throw std::runtime_error("Database already initialized");
    }
    global_db = std::make_unique<DatabaseManager>();
    global_db->initialize(primary_conn, replica_conns, pool_size, acquire_timeout, statement_timeout);
}

inline DatabaseManager& get() {
    if (global_db == nullptr) {
        throw std::runtime_error("Database not initialized");
    }
    return *global_db;
}

inline bool is_initialized() {
    return global_db != nullptr && global_db->is_initialized();
}

inline void shutdown() {
    if (global_db) {
        global_db->shutdown();
        global_db.reset();
    }
}

}  // namespace Database
