/**
 * @file Templates.hpp
 * @brief Render email templates from disk via inja.
 *
 * flask-base parity: app/email.py renders Jinja2 templates with the
 * Flask app context. We use inja (a Jinja-subset engine for C++) and
 * pass a plain nlohmann::json as context.
 *
 * Convention: every template ships as a .txt + .html pair under
 * templates/email/<name>.{txt,html}. Mailer expects both bodies, so
 * render_pair() returns them together.
 *
 * Templates are not cached — files are small and Mailer::send is the
 * dominant cost (network roundtrip). When this becomes a hotspot,
 * cache by mtime in inja::Environment.
 */

#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <spdlog/spdlog.h>

#include <inja/inja.hpp>
#include <nlohmann/json.hpp>

#include "utils/Config.hpp"

namespace Email::Templates {

using json = nlohmann::json;

/**
 * @brief Configurable directory root. Defaults to "templates/email" relative
 *        to the working directory; override via mail.templates_dir /
 *        MAIL_TEMPLATES_DIR for non-standard layouts.
 */
inline std::string templates_dir() {
    if (Config::is_initialized()) {
        return Config::get().get<std::string>("mail.templates_dir", "MAIL_TEMPLATES_DIR", "templates/email");
    }
    // Env override must work without Config too (unit tests, tooling) —
    // mirrors the layered lookup where env always wins anyway.
    if (const char* env = std::getenv("MAIL_TEMPLATES_DIR"))
        return env;
    return "templates/email";
}

inline std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.good())
        throw std::runtime_error("template not found: " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/**
 * @brief Render one variant (txt or html) of @p name with @p ctx.
 * @throws std::runtime_error if the file is missing — caller decides
 *         whether that's fatal.
 */
inline std::string render(const std::string& name, const std::string& ext, const json& ctx) {
    const auto path = std::filesystem::path(templates_dir()) / (name + "." + ext);
    const std::string tpl = read_file(path.string());
    inja::Environment env;
    return env.render(tpl, ctx);
}

struct Pair {
    std::string text;
    std::string html;
};

/**
 * @brief Render both .txt and .html variants in one go. The two share
 *        the same context — same variables resolve identically.
 */
inline Pair render_pair(const std::string& name, const json& ctx) {
    Pair p;
    p.text = render(name, "txt", ctx);
    p.html = render(name, "html", ctx);
    return p;
}

/**
 * @brief Common context fields injected into every template render.
 *        flask-base used Flask's `current_app.config['APP_NAME']` etc.;
 *        we pass them explicitly so a unit test can render templates
 *        without booting Config.
 */
inline json default_context() {
    json ctx = json::object();
    if (Config::is_initialized()) {
        ctx["app_name"] = Config::get().get<std::string>("app.name", "APP_NAME", "App");
        ctx["base_url"] = Config::get().get<std::string>("app.base_url", "APP_BASE_URL", "http://localhost:8080");
    } else {
        ctx["app_name"] = "App";
        ctx["base_url"] = "http://localhost:8080";
    }
    return ctx;
}

}  // namespace Email::Templates
