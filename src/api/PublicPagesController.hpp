/**
 * @file PublicPagesController.hpp
 * @brief Server-rendered public entry points for SEO — a dynamic sitemap over
 *        published posts, clean per-post URLs (/blog/<slug>) with a real head
 *        (title, canonical, OG, JSON-LD BlogPosting) so crawlers and link
 *        previews get correct per-post metadata before JS runs, and a 301 from
 *        the old /blog-single.html?slug= form.
 *
 * The article BODY is still hydrated client-side by /js/blog.js (Google renders
 * JS); this controller only guarantees the head + the shell js/blog.js targets.
 * The nginx frontend proxies /sitemap.xml, /blog/ and /blog-single.html here.
 */

#pragma once

#include <string>

#include <drogon/HttpController.h>
#include <drogon/drogon.h>

#include <nlohmann/json.hpp>

#include "domain/Post.hpp"
#include "repositories/PostRepository.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class PublicPagesController : public HttpController<PublicPagesController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(PublicPagesController::sitemap, "/sitemap.xml", Get);
    ADD_METHOD_TO(PublicPagesController::blogPost, "/blog/{1}", Get);
    ADD_METHOD_TO(PublicPagesController::blogSingleRedirect, "/blog-single.html", Get);
    METHOD_LIST_END

    // GET /sitemap.xml — home + blog index + every published post (clean URL).
    void sitemap(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& cb) {
        Repositories::PostRepository repo;
        auto entries = repo.list_published_for_sitemap();
        const std::string base = origin(req);

        std::string xml;
        xml.reserve(entries.size() * 128 + 512);
        xml += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        xml += "<urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">\n";
        xml += "  <url><loc>" + esc(base) + "/</loc><changefreq>monthly</changefreq><priority>1.0</priority></url>\n";
        xml += "  <url><loc>" + esc(base) +
               "/blog.html</loc><changefreq>daily</changefreq><priority>0.8</priority></url>\n";
        for (const auto& e : entries) {
            xml += "  <url><loc>" + esc(base) + "/blog/" + esc(e.slug) + "</loc>";
            if (!e.lastmod.empty())
                xml += "<lastmod>" + e.lastmod + "</lastmod>";
            xml += "<changefreq>monthly</changefreq><priority>0.6</priority></url>\n";
        }
        xml += "</urlset>\n";

        auto resp = HttpResponse::newHttpResponse();
        resp->setBody(std::move(xml));
        resp->setContentTypeString("application/xml; charset=utf-8");
        cb(resp);
    }

    // GET /blog/{slug} — SSR head + the shell js/blog.js hydrates.
    void blogPost(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& cb,
                  const std::string& slug) {
        Repositories::PostRepository repo;
        auto post = repo.find_published_by_slug(slug);
        const std::string base = origin(req);

        if (!post) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setStatusCode(k404NotFound);
            resp->setContentTypeCode(CT_TEXT_HTML);
            resp->setBody(
                "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
                "<title>Not found — Michael Tarassov</title><meta name=\"robots\" content=\"noindex\">"
                "</head><body><p>Post not found. <a href=\"/blog.html\">All notes</a>.</p></body></html>");
            cb(resp);
            return;
        }

        const auto& p = *post;
        const std::string canonical = base + "/blog/" + p.slug;
        const std::string title = p.title + " — Michael Tarassov";
        const std::string desc =
            p.summary.empty() ? std::string("A field note from production by Michael Tarassov.") : p.summary;
        const std::string image = base + "/images/site/og.jpg";

        json ld = {
            {"@context", "https://schema.org"},
            {"@type", "BlogPosting"},
            {"headline", p.title},
            {"description", desc},
            {"url", canonical},
            {"mainEntityOfPage", canonical},
            {"image", image},
            {"author", {{"@type", "Person"}, {"name", "Michael Tarassov"}, {"url", base + "/"}}},
            {"publisher", {{"@type", "Person"}, {"name", "Michael Tarassov"}, {"url", base + "/"}}},
        };
        if (p.published_at)
            ld["datePublished"] = *p.published_at;
        ld["dateModified"] = p.updated_at;
        if (!p.topic.empty())
            ld["articleSection"] = p.topic;
        if (!p.tags.empty())
            ld["keywords"] = p.tags;
        // Guard against a literal </script> inside the JSON breaking the tag.
        std::string ldStr = ld.dump();
        for (std::size_t pos = 0; (pos = ldStr.find("</", pos)) != std::string::npos; pos += 3)
            ldStr.replace(pos, 2, "<\\/");

        std::string html;
        html.reserve(4096);
        html += "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n";
        html += "<meta charset=\"utf-8\">\n";
        html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
        html += "<title>" + esc(title) + "</title>\n";
        html += "<meta name=\"description\" content=\"" + esc(desc) + "\">\n";
        html += "<meta name=\"author\" content=\"Michael Tarassov\">\n";
        html += "<link rel=\"canonical\" href=\"" + esc(canonical) + "\">\n";
        html += "<meta property=\"og:type\" content=\"article\">\n";
        html += "<meta property=\"og:site_name\" content=\"tarassov.me\">\n";
        html += "<meta property=\"og:title\" content=\"" + esc(p.title) + "\">\n";
        html += "<meta property=\"og:description\" content=\"" + esc(desc) + "\">\n";
        html += "<meta property=\"og:url\" content=\"" + esc(canonical) + "\">\n";
        html += "<meta property=\"og:image\" content=\"" + esc(image) + "\">\n";
        html += "<meta name=\"twitter:card\" content=\"summary_large_image\">\n";
        html += "<meta name=\"twitter:title\" content=\"" + esc(p.title) + "\">\n";
        html += "<meta name=\"twitter:description\" content=\"" + esc(desc) + "\">\n";
        html += "<meta name=\"twitter:image\" content=\"" + esc(image) + "\">\n";
        html += "<script type=\"application/ld+json\">" + ldStr + "</script>\n";
        html += kHeadTail;  // icons + fonts + stylesheets (absolute paths)
        html += "</head>\n<body class=\"blog\">\n";
        html += "<div class=\"blog-progress\" id=\"blog-progress\"></div>\n";
        html +=
            "<div class=\"blog-frame\"><nav class=\"blog-nav\">"
            "<a class=\"wordmark\" href=\"/\">M. TARASSOV</a>"
            "<div class=\"links\"><a href=\"/\">HOME</a>"
            "<a class=\"active\" href=\"/blog.html\">BLOG</a></div></nav></div>\n";
        html += "<article class=\"blog-article\">\n";
        html += "<header class=\"blog-post-head\">\n";
        html += "<a class=\"blog-back\" href=\"/blog.html\">← ALL NOTES</a>\n";
        html +=
            "<div class=\"blog-post-topic\" id=\"post-topic\"" + hideIfEmpty(p.topic) + ">" + esc(p.topic) + "</div>\n";
        html += "<h1 class=\"blog-post-title\" id=\"post-title\">" + esc(p.title) + "</h1>\n";
        html += "<p class=\"blog-post-dek\" id=\"post-dek\"" + hideIfEmpty(p.summary) + ">" + esc(p.summary) + "</p>\n";
        html += "<div class=\"blog-post-meta\" id=\"post-meta\" hidden></div>\n";
        html += "</header>\n";
        html += "<div class=\"blog-body\" id=\"post-content\"><p class=\"blog-loading\">Loading…</p></div>\n";
        html += "<div class=\"blog-post-tags\" id=\"post-tags\" hidden></div>\n";
        html += kPostNav;
        html += "</article>\n";
        html +=
            "<div class=\"blog-frame\"><footer class=\"blog-footer\">"
            "<span>© 2026 MICHAEL TARASSOV</span>"
            "<a href=\"https://github.com/moveeeax\">GITHUB →</a></footer></div>\n";
        html += "<script src=\"/js/marked.min.js\"></script>\n<script src=\"/js/blog.js\"></script>\n";
        html += "</body>\n</html>\n";

        auto resp = HttpResponse::newHttpResponse();
        resp->setBody(std::move(html));
        resp->setContentTypeCode(CT_TEXT_HTML);
        cb(resp);
    }

    // GET /blog-single.html?slug=X — 301 to the clean URL (back-compat).
    void blogSingleRedirect(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& cb) {
        const std::string slug = req->getParameter("slug");
        const std::string target = slug.empty() ? "/blog.html" : "/blog/" + slug;
        cb(HttpResponse::newRedirectionResponse(target, k301MovedPermanently));
    }

private:
    // Public origin from the proxied request (self-adjusts across environments);
    // APP_BASE_URL is a placeholder in prod, so we don't use it here.
    static std::string origin(const HttpRequestPtr& req) {
        const std::string host = req->getHeader("host");
        if (host.empty())
            return "https://tarassov.me";
        std::string proto = req->getHeader("x-forwarded-proto");
        if (proto.empty())
            proto = "https";
        return proto + "://" + host;
    }

    // HTML/XML escape of the five significant characters (valid for both).
    static std::string esc(const std::string& s) {
        std::string o;
        o.reserve(s.size() + 16);
        for (char c : s) {
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

    static std::string hideIfEmpty(const std::string& s) { return s.empty() ? " hidden" : ""; }

    // ── Static markup fragments (keep in sync with blog-single.html) ─────────
    // Absolute asset paths so they resolve under /blog/<slug>, not /blog/.
    static constexpr const char* kHeadTail =
        R"HEAD(<link rel="icon" type="image/svg+xml" href="/images/ico/favicon.svg">
<link rel="icon" type="image/x-icon" href="/images/ico/favicon.ico">
<link rel="apple-touch-icon" href="/images/ico/apple-touch-icon.png">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;500;600&display=swap" rel="stylesheet">
<link rel="stylesheet" type="text/css" href="/css/normalize.css">
<link rel="stylesheet" type="text/css" href="/css/blog.css">
)HEAD";

    static constexpr const char* kPostNav =
        R"NAV(<nav class="blog-post-nav" id="post-pager" hidden aria-label="Adjacent posts">
<a id="pager-prev" class="prev" hidden><div class="label">← PREVIOUS</div><div class="title"></div></a>
<a id="pager-next" class="next" hidden><div class="label">NEXT →</div><div class="title"></div></a>
</nav>
)NAV";
};

}  // namespace Api
