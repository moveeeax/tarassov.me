/**
 * @file Pg.hpp
 * @brief PostgreSQL connection-string helpers.
 */

#pragma once

#include <string>

namespace Utils::Pg {

/// Quote+escape a single libpq conninfo value. libpq key=value parsing treats a
/// backslash as an escape and a single quote as a value delimiter, so both must
/// be backslash-escaped; we always wrap in single quotes so spaces / empties are
/// handled too. Keeps a password with special chars (spaces, quotes, backslashes)
/// intact without URL percent-encoding pitfalls.
inline std::string quote_value(const std::string& v) {
    std::string out = "'";
    for (char c : v) {
        if (c == '\\' || c == '\'')
            out += '\\';
        out += c;
    }
    out += '\'';
    return out;
}

/// Build a libpq key=value conninfo string from discrete parts. Assembling the
/// DSN here (instead of materializing a postgresql://user:pass@host URL in an env
/// var) keeps the password out of DATABASE_PRIMARY_URL — so it isn't duplicated
/// into the process environment / `kubectl exec -- env` / crash dumps; it lives
/// only in DATABASE_PASSWORD. An empty password component is omitted (peer/cert
/// auth).
inline std::string make_conninfo(const std::string& host,
                                 int port,
                                 const std::string& user,
                                 const std::string& dbname,
                                 const std::string& password,
                                 int connect_timeout_sec = 5) {
    std::string dsn = "host=" + quote_value(host) + " port=" + std::to_string(port) + " user=" + quote_value(user) +
                      " dbname=" + quote_value(dbname);
    if (!password.empty())
        dsn += " password=" + quote_value(password);
    // Bound the TCP connect: libpq's default is unbounded, so a blackholed DB
    // endpoint (dead node, dropped packets) would park a reconnecting request
    // thread for the OS SYN-retry window (~2 min) — far past acquire_timeout,
    // which only bounds the wait for a free pool slot. Fail fast instead.
    if (connect_timeout_sec > 0)
        dsn += " connect_timeout=" + std::to_string(connect_timeout_sec);
    return dsn;
}

/// True if a libpq DSN (key=value form OR postgresql:// URL) already pins a
/// connect_timeout — so we don't clobber an operator-supplied one.
inline bool has_connect_timeout(const std::string& dsn) {
    return dsn.find("connect_timeout") != std::string::npos;
}

/// Append a bounded connect_timeout to an operator-supplied DSN (URL or
/// key=value form) unless it already pins one. Same rationale as make_conninfo:
/// libpq's default connect is unbounded. Applied to the primary AND every read
/// replica so a blackholed replica can't hang read traffic either.
inline std::string with_connect_timeout(std::string dsn, int connect_timeout_sec = 5) {
    if (connect_timeout_sec <= 0 || has_connect_timeout(dsn))
        return dsn;
    const bool url_form = dsn.rfind("postgres", 0) == 0 && dsn.find("://") != std::string::npos;
    if (url_form)
        dsn += (dsn.find('?') == std::string::npos ? "?connect_timeout=" : "&connect_timeout=") +
               std::to_string(connect_timeout_sec);
    else
        dsn += " connect_timeout=" + std::to_string(connect_timeout_sec);
    return dsn;
}

}  // namespace Utils::Pg
