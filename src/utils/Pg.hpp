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
                                 const std::string& password) {
    std::string dsn = "host=" + quote_value(host) + " port=" + std::to_string(port) + " user=" + quote_value(user) +
                      " dbname=" + quote_value(dbname);
    if (!password.empty())
        dsn += " password=" + quote_value(password);
    return dsn;
}

}  // namespace Utils::Pg
