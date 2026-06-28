/**
 * @file Csrf.hpp
 * @brief Double-submit-cookie CSRF check (pure decision function).
 *
 * The session cookies are SameSite=Lax + same-origin, which already stops a
 * cross-site POST from carrying them in modern browsers. This is defense in
 * depth for older browsers / misconfigured SameSite: on a state-changing,
 * COOKIE-authenticated request we require a non-HttpOnly CSRF cookie to be
 * echoed in a request header (set by the SPA from JS). An attacker on another
 * origin can neither read the victim's CSRF cookie (same-origin policy) nor set
 * the custom header cross-site without a CORS grant, so the two can't match.
 *
 * Requests authenticated by the Authorization header instead of a cookie are
 * not CSRF-able (the attacker can't set that header either), and the public
 * auth endpoints (login/register/refresh) carry no access cookie yet — both are
 * skipped by the access_cookie-empty short-circuit, so no path allowlist is
 * needed.
 */

#pragma once

#include <string_view>

#include "utils/Crypto.hpp"

namespace Security::Csrf {

/**
 * @brief Decide whether a request passes the CSRF check.
 * @param unsafe_method  true for POST/PUT/PATCH/DELETE; false for GET/HEAD/OPTIONS.
 * @param access_cookie  the session access cookie value (empty → not cookie-auth).
 * @param token_cookie   the double-submit CSRF cookie value.
 * @param token_header   the value echoed back in the request header.
 * @return true if the request is allowed (check passes or does not apply).
 */
inline bool passes(bool unsafe_method,
                   std::string_view access_cookie,
                   std::string_view token_cookie,
                   std::string_view token_header) {
    if (!unsafe_method)
        return true;  // safe methods never mutate state
    if (access_cookie.empty())
        return true;  // header/Bearer auth or unauthenticated — not CSRF-able
    if (token_cookie.empty() || token_header.empty())
        return false;  // cookie-auth mutation with no token on one side → reject
    return Utils::Crypto::constant_time_equals(token_cookie, token_header);
}

}  // namespace Security::Csrf
