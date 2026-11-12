/**
 * @file AtomFeed.hpp
 * @brief Atom 1.0 (RFC 4287) rendering for GET /feed.xml, plus the Redis
 *        cache-generation helpers the post write paths use to invalidate it.
 *
 * render_atom() is pure (no DB, no config) so the XML shape is unit-tested
 * directly (tests/unit/test_atom_feed.cpp); PublicPagesController::feed does
 * the query, the caching and the HTTP response.
 */

#pragma once

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "cache/Cache.hpp"
#include "domain/Post.hpp"

namespace Pages {

/// Feed-level fields. URLs are absolute and unescaped; render_atom escapes.
struct AtomFeedInfo {
    std::string id;        // atom:id — the feed's own URL (incl. ?topic=)
    std::string self_url;  // rel="self"
    std::string html_url;  // rel="alternate" — the blog index
    std::string base;      // site origin, no trailing slash
    std::string title;
    std::string subtitle;
    std::string author;
    std::string empty_updated;  // feed atom:updated when there are no entries
};

/// XML text/attribute escape. Also drops the C0 control characters XML 1.0
/// forbids (everything below 0x20 except TAB, LF, CR): one stray \x0b pasted
/// into a post summary would otherwise make the whole feed unparseable for
/// every reader. Bytes >= 0x80 (UTF-8 sequences) pass through unchanged.
inline std::string xml_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 16);
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 && u != '\t' && u != '\n' && u != '\r')
            continue;
        switch (c) {
            case '&':
                o += "&amp;";
                break;
            case '<':
                o += "&lt;";
                break;
            case '>':
                o += "&gt;";
                break;
            case '"':
                o += "&quot;";
                break;
            case '\'':
                o += "&#39;";
                break;
            default:
                o += c;
        }
    }
    return o;
}

/// RFC 3986 percent-encoding for one query value (unreserved chars kept).
inline std::string url_encode_component(const std::string& s) {
    std::string o;
    o.reserve(s.size() * 3);
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= '0' && u <= '9') || c == '-' || c == '.' ||
            c == '_' || c == '~') {
            o += c;
        } else {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%%%02X", u);
            o += buf;
        }
    }
    return o;
}

/// Render the feed. Entries are expected newest first (the repository order).
/// atom:id of an entry is urn:uuid:<post id>, not the URL: Atom ids must never
/// change, and a renamed slug would otherwise show up as a new post in every
/// reader. The feed's atom:updated is the newest entry's updated_at (an edit
/// to an older post moves it too); timestamps are UTC ISO 8601, so string
/// order is time order.
inline std::string render_atom(const AtomFeedInfo& info, const std::vector<Domain::PostFeedItem>& entries) {
    std::string updated;
    for (const auto& e : entries)
        updated = std::max(updated, e.updated_at);
    if (updated.empty())
        updated = info.empty_updated;

    std::string x;
    x.reserve(entries.size() * 768 + 1024);
    x += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    x += "<feed xmlns=\"http://www.w3.org/2005/Atom\">\n";
    x += "  <id>" + xml_escape(info.id) + "</id>\n";
    x += "  <title>" + xml_escape(info.title) + "</title>\n";
    if (!info.subtitle.empty())
        x += "  <subtitle>" + xml_escape(info.subtitle) + "</subtitle>\n";
    x += "  <updated>" + xml_escape(updated) + "</updated>\n";
    x += "  <link rel=\"self\" type=\"application/atom+xml\" href=\"" + xml_escape(info.self_url) + "\"/>\n";
    x += "  <link rel=\"alternate\" type=\"text/html\" href=\"" + xml_escape(info.html_url) + "\"/>\n";
    x += "  <author><name>" + xml_escape(info.author) + "</name><uri>" + xml_escape(info.base) + "/</uri></author>\n";
    x += "  <icon>" + xml_escape(info.base) + "/images/ico/favicon.ico</icon>\n";

    for (const auto& e : entries) {
        const std::string url = info.base + "/blog/" + e.slug;
        x += "  <entry>\n";
        x += "    <id>urn:uuid:" + xml_escape(e.id) + "</id>\n";
        x += "    <title type=\"text\">" + xml_escape(e.title) + "</title>\n";
        x += "    <link rel=\"alternate\" type=\"text/html\" href=\"" + xml_escape(url) + "\"/>\n";
        if (e.published_at)
            x += "    <published>" + xml_escape(*e.published_at) + "</published>\n";
        x += "    <updated>" + xml_escape(e.updated_at) + "</updated>\n";
        if (!e.summary.empty())
            x += "    <summary type=\"text\">" + xml_escape(e.summary) + "</summary>\n";
        // Topic first, then tags; a tag repeating the topic is not listed twice.
        std::vector<std::string> cats;
        if (!e.topic.empty())
            cats.push_back(e.topic);
        for (const auto& t : e.tags)
            if (std::find(cats.begin(), cats.end(), t) == cats.end())
                cats.push_back(t);
        for (const auto& c : cats)
            x += "    <category term=\"" + xml_escape(c) + "\"/>\n";
        x += "  </entry>\n";
    }
    x += "</feed>\n";
    return x;
}

// ── Cache generation ────────────────────────────────────────────────────────
// Rendered feeds are cached under feed:atom:<generation>:<topic>. A post
// create/update/delete bumps the generation, so every cached variant (the
// main feed and each per-topic one) goes stale at once without having to
// enumerate keys. Old generations simply age out on their TTL. Fail-open like
// the rest of Cache: with Redis down the feed is rendered on every request.
inline constexpr const char* kFeedGenerationKey = "feed:atom:gen";

inline std::string feed_cache_key(const std::string& topic) {
    std::string gen = "0";
    if (Cache::is_initialized()) {
        try {
            gen = Cache::get().get(kFeedGenerationKey).value_or("0");
        } catch (const std::exception& e) {
            spdlog::debug("feed: generation read failed ({})", e.what());
        }
    }
    return "feed:atom:" + gen + ":" + topic;
}

inline void invalidate_feed_cache() {
    if (!Cache::is_initialized())
        return;
    try {
        Cache::get().incr(kFeedGenerationKey);
    } catch (const std::exception& e) {
        // incr() rethrows by contract; a missed bump only means the cached
        // feed lives out its (short) TTL — never fail the post write for it.
        spdlog::warn("feed: cache invalidation failed ({})", e.what());
    }
}

}  // namespace Pages
