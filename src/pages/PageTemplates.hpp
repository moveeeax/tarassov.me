/**
 * @file PageTemplates.hpp
 * @brief File-based HTML page templates with {{KEY}} substitution. Mirrors the
 *        email template loader (src/email/Templates.hpp): files live in
 *        templates/pages/, are read once and cached for the process lifetime.
 *        Values must be pre-escaped by the caller — this layer substitutes
 *        only, it never escapes.
 */

#pragma once

#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "utils/Config.hpp"

namespace Pages {

inline std::string templates_dir() {
    return Config::get().get<std::string>("site.pages_templates_dir", "SITE_PAGES_TEMPLATES_DIR", "templates/pages");
}

inline const std::string& load(const std::string& name) {
    static std::unordered_map<std::string, std::string> cache;
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    auto it = cache.find(name);
    if (it != cache.end())
        return it->second;
    std::ifstream f(templates_dir() + "/" + name + ".html");
    if (!f.good())
        throw std::runtime_error("page template not found: " + templates_dir() + "/" + name + ".html");
    std::stringstream ss;
    ss << f.rdbuf();
    return cache.emplace(name, ss.str()).first->second;
}

/// Replace every {{KEY}} with its (pre-escaped) value. Single pass over the
/// TEMPLATE — substituted values are never rescanned, so post content that
/// happens to contain a literal "{{TITLE}}" stays literal instead of being
/// expanded. Unknown placeholders are left as-is.
inline std::string render(const std::string& name, const std::map<std::string, std::string>& vars) {
    const std::string& tpl = load(name);
    std::string out;
    out.reserve(tpl.size());
    std::size_t pos = 0;
    while (true) {
        const auto open = tpl.find("{{", pos);
        if (open == std::string::npos) {
            out.append(tpl, pos, std::string::npos);
            break;
        }
        const auto close = tpl.find("}}", open + 2);
        if (close == std::string::npos) {
            out.append(tpl, pos, std::string::npos);
            break;
        }
        out.append(tpl, pos, open - pos);
        const std::string key = tpl.substr(open + 2, close - open - 2);
        const auto it = vars.find(key);
        if (it != vars.end())
            out += it->second;
        else
            out.append(tpl, open, close + 2 - open);
        pos = close + 2;
    }
    return out;
}

}  // namespace Pages
