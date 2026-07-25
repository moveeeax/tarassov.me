/**
 * @file PublicPagesController.hpp
 * @brief Server-rendered public entry points for SEO — a dynamic sitemap over
 *        published posts and clean per-post URLs (/blog/<slug>) with a real
 *        head (title, canonical, OG, JSON-LD BlogPosting) so crawlers and link
 *        previews get correct per-post metadata before JS runs.
 *
 * The article BODY is still hydrated client-side by /js/blog.js (from the
 * embedded #post-data island); this controller guarantees the head + shell.
 * The nginx frontend proxies /sitemap.xml and /blog/ here.
 */

#pragma once

#include <string>

#include <drogon/HttpController.h>
#include <drogon/drogon.h>

#include <nlohmann/json.hpp>

#include "api/PostsController.hpp"
#include "domain/Post.hpp"
#include "pages/PageTemplates.hpp"
#include "repositories/PostRepository.hpp"
#include "utils/Config.hpp"

namespace Api {

using namespace drogon;
using json = nlohmann::json;

class PublicPagesController : public HttpController<PublicPagesController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(PublicPagesController::sitemap, "/sitemap.xml", Get);
    ADD_METHOD_TO(PublicPagesController::blogPost, "/blog/{1}", Get);
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
        // Cheap SQL, but crawlers poll: an hour of caching is plenty fresh.
        resp->addHeader("Cache-Control", "public, max-age=3600");
        cb(resp);
    }

    // GET /blog/{slug} — SSR head from a file template; the embedded
    // #post-data island lets js/blog.js hydrate without a second fetch.
    // ?preview=<token> renders a draft (noindex) — see PostsController.
    void blogPost(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& cb,
                  const std::string& slug) {
        auto post = PostsController::resolve_post(slug, req->getParameter("preview"));
        const std::string base = origin(req);

        if (!post) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setStatusCode(k404NotFound);
            resp->setContentTypeCode(CT_TEXT_HTML);
            resp->setBody(Pages::render("blog_post_404", {}));
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

        auto resp = HttpResponse::newHttpResponse();
        resp->setBody(
            Pages::render("blog_post",
                          {
                              {"TITLE", esc(title)},
                              {"DESC", esc(desc)},
                              {"CANONICAL", esc(canonical)},
                              {"OG_TITLE", esc(p.title)},
                              {"OG_IMAGE", esc(image)},
                              {"JSON_LD", script_safe(ld.dump())},
                              // Previewed drafts must never be indexed.
                              {"ROBOTS", p.status == "published" ? "" : "<meta name=\"robots\" content=\"noindex\">\n"},
                              {"TOPIC", esc(p.topic)},
                              {"TOPIC_HIDDEN", hideIfEmpty(p.topic)},
                              {"H1", esc(p.title)},
                              {"DEK", esc(p.summary)},
                              {"DEK_HIDDEN", hideIfEmpty(p.summary)},
                              // Hydration island: same shape as the public by-slug `data`.
                              {"POST_JSON", script_safe(json(p).dump())},
                          }));
        resp->setContentTypeCode(CT_TEXT_HTML);
        cb(resp);
    }

private:
    // Canonical origin: site.base_url when configured (prod — validated at
    // boot), header-derived only as a dev fallback with no configured base.
    // Header derivation produced http:// URLs behind the TLS-terminating
    // ingress; config is authoritative.
    static std::string origin(const HttpRequestPtr& req) {
        const std::string cfg_base = Config::get().get<std::string>("site.base_url", "SITE_BASE_URL", "");
        if (!cfg_base.empty())
            return cfg_base.back() == '/' ? cfg_base.substr(0, cfg_base.size() - 1) : cfg_base;
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

    // Guard against a literal </script> inside embedded JSON breaking the tag.
    static std::string script_safe(std::string s) {
        for (std::size_t pos = 0; (pos = s.find("</", pos)) != std::string::npos; pos += 3)
            s.replace(pos, 2, "<\\/");
        return s;
    }
};

}  // namespace Api
