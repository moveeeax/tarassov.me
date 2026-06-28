/**
 * @file Config.hpp
 * @brief Configuration management module for parsing JSON files and environment variables
 * @details Provides utilities to load application configuration from config files
 *          with environment variable overrides following 12-factor app principles
 */

#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace Config {

using json = nlohmann::json;

/**
 * @brief Main configuration class
 * @details Manages application configuration with support for JSON files
 *          and environment variable overrides
 */
class AppConfig {
private:
    json config_data;
    std::filesystem::path config_path;

public:
    /**
     * @brief Construct configuration from a JSON file
     * @param config_file Path to the configuration file
     * @throws std::runtime_error if file cannot be loaded
     */
    explicit AppConfig(const std::string& config_file) {
        config_path = config_file;
        load_from_file(config_file);
    }

    /**
     * @brief Load configuration from a JSON file
     * @param file_path Path to configuration file
     * @throws std::runtime_error if file cannot be opened or parsed
     * @details After parsing, all string values are recursively scanned for
     *          ${VAR} and ${VAR:-default} placeholders and substituted from
     *          process environment. Keeps secrets out of committed JSON.
     */
    void load_from_file(const std::string& file_path) {
        std::ifstream file(file_path);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open config file: " + file_path);
        }

        try {
            file >> config_data;
        } catch (const json::parse_error& e) {
            throw std::runtime_error("Failed to parse config file: " + std::string(e.what()));
        }

        substitute_env_placeholders(config_data);
    }

    /**
     * @brief Get a configuration value with environment variable override
     * @tparam T The type of the value to retrieve
     * @param key JSON key path (e.g., "database.host")
     * @param env_var Environment variable name to check for override
     * @param default_value Default value if key not found
     * @return Configuration value
     */
    template <typename T>
    T get(const std::string& key, const std::string& env_var = "", const T& default_value = T{}) const {
        // Check environment variable first
        if (!env_var.empty()) {
            const char* env_value = std::getenv(env_var.c_str());
            if (env_value != nullptr) {
                return parse_env_value<T>(env_value);
            }
        }

        // Fall back to config file
        try {
            return get_nested_value<T>(key);
        } catch (...) {
            return default_value;
        }
    }

    /**
     * @brief Like get() but throws if neither env var nor config value is present.
     * @details Use for values that must be explicitly set in any deployment —
     *          e.g. JWT secret, database URL in production.
     */
    template <typename T>
    T require(const std::string& key, const std::string& env_var = "") const {
        if (!env_var.empty()) {
            const char* env_value = std::getenv(env_var.c_str());
            if (env_value != nullptr) {
                return parse_env_value<T>(env_value);
            }
        }
        try {
            return get_nested_value<T>(key);
        } catch (...) {
            throw std::runtime_error("Required configuration missing: key='" + key + "' env='" + env_var + "'");
        }
    }

    /**
     * @brief Get a configuration value without environment override
     * @tparam T The type of the value to retrieve
     * @param key JSON key path
     * @return Optional value
     */
    template <typename T>
    std::optional<T> get_optional(const std::string& key) const {
        try {
            return get_nested_value<T>(key);
        } catch (...) {
            return std::nullopt;
        }
    }

    /**
     * @brief Get raw JSON object
     * @return Reference to underlying JSON data
     */
    const json& get_json() const { return config_data; }

    /**
     * @brief Reload configuration from file
     */
    void reload() { load_from_file(config_path.string()); }

private:
    /**
     * @brief Parse environment variable value to specified type
     * @tparam T Target type
     * @param value String value from environment
     * @return Parsed value
     */
    template <typename T>
    T parse_env_value(const char* value) const {
        if constexpr (std::is_same_v<T, std::string>) {
            return std::string(value);
        } else if constexpr (std::is_same_v<T, int>) {
            return std::stoi(value);
        } else if constexpr (std::is_same_v<T, long>) {
            return std::stol(value);
        } else if constexpr (std::is_same_v<T, double>) {
            return std::stod(value);
        } else if constexpr (std::is_same_v<T, bool>) {
            std::string str_value(value);
            return str_value == "true" || str_value == "1" || str_value == "yes";
        } else {
            return T{};
        }
    }

    /**
     * @brief Expand ${VAR} and ${VAR:-default} placeholders in a single string.
     * @details Simple POSIX-shell-style substitution. Unmatched placeholders
     *          are replaced with empty string (or their default clause).
     */
    static std::string expand_string(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        size_t i = 0;
        while (i < s.size()) {
            if (i + 1 < s.size() && s[i] == '$' && s[i + 1] == '{') {
                size_t end = s.find('}', i + 2);
                if (end == std::string::npos) {
                    out.append(s, i, std::string::npos);
                    break;
                }
                std::string expr = s.substr(i + 2, end - i - 2);
                std::string var_name;
                std::string default_value;
                bool has_default = false;
                auto sep = expr.find(":-");
                if (sep != std::string::npos) {
                    var_name = expr.substr(0, sep);
                    default_value = expr.substr(sep + 2);
                    has_default = true;
                } else {
                    var_name = expr;
                }
                const char* env_value = var_name.empty() ? nullptr : std::getenv(var_name.c_str());
                if (env_value != nullptr) {
                    out.append(env_value);
                } else if (has_default) {
                    out.append(default_value);
                }
                // else: leave the placeholder unexpanded? No — drop it silently
                // (matches POSIX shell behavior for unset-without-default).
                i = end + 1;
            } else {
                out.push_back(s[i++]);
            }
        }
        return out;
    }

    /**
     * @brief Recursively walk JSON and expand placeholders in every string value.
     */
    static void substitute_env_placeholders(json& node) {
        if (node.is_string()) {
            const auto& raw = node.get_ref<const std::string&>();
            if (raw.find("${") != std::string::npos) {
                node = expand_string(raw);
            }
        } else if (node.is_object()) {
            for (auto it = node.begin(); it != node.end(); ++it) {
                substitute_env_placeholders(it.value());
            }
        } else if (node.is_array()) {
            for (auto& v : node)
                substitute_env_placeholders(v);
        }
    }

    /**
     * @brief Get value from nested JSON path (e.g., "database.host")
     * @tparam T Target type
     * @param key Dot-separated path
     * @return Value at path
     */
    template <typename T>
    T get_nested_value(const std::string& key) const {
        // Walk by pointer — get() is called dozens of times at boot, so
        // copying the whole config document per segment (the old behaviour)
        // was pure waste.
        const json* current = &config_data;
        size_t start = 0;
        size_t pos;
        while ((pos = key.find('.', start)) != std::string::npos) {
            current = &current->at(key.substr(start, pos - start));
            start = pos + 1;
        }
        return current->at(key.substr(start)).get<T>();
    }
};

/**
 * @brief Global configuration instance
 */
inline std::unique_ptr<AppConfig> global_config = nullptr;

/**
 * @brief Initialize global configuration
 * @param config_file Path to configuration file
 * @throws std::runtime_error if already initialized
 */
inline void initialize(const std::string& config_file) {
    if (global_config != nullptr) {
        throw std::runtime_error("Configuration already initialized");
    }
    global_config = std::make_unique<AppConfig>(config_file);
}

/**
 * @brief Get global configuration instance
 * @return Reference to global config
 * @throws std::runtime_error if not initialized
 */
inline AppConfig& get() {
    if (global_config == nullptr) {
        throw std::runtime_error("Configuration not initialized");
    }
    return *global_config;
}

/**
 * @brief Check if configuration is initialized
 * @return true if initialized
 */
inline bool is_initialized() {
    return global_config != nullptr;
}

/**
 * @brief Shutdown and cleanup configuration
 */
inline void shutdown() {
    global_config.reset();
}

}  // namespace Config
