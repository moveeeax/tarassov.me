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

/// Replace every {{KEY}} with its (pre-escaped) value.
inline std::string render(const std::string& name, const std::map<std::string, std::string>& vars) {
    std::string out = load(name);
    for (const auto& [key, value] : vars) {
        const std::string needle = "{{" + key + "}}";
        for (std::size_t pos = 0; (pos = out.find(needle, pos)) != std::string::npos; pos += value.size())
            out.replace(pos, needle.size(), value);
    }
    return out;
}

}  // namespace Pages
