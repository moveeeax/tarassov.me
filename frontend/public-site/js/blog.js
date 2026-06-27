/*
 * Public blog rendering for the BookCard static site.
 *
 * Pulls posts from the backend's unauthenticated read API and injects them
 * into the template's existing markup, so the static template stays static
 * while the content is DB-backed (admin-authored).
 *
 *   blog.html          -> GET /api/v1/public/posts          (list)
 *   blog-single.html   -> GET /api/v1/public/posts/{slug}   (one, by ?slug=)
 *
 * Same-origin: nginx proxies /api/* to the backend, so no CORS.
 */
(function () {
    "use strict";

    var API = "/api/v1/public/posts";

    // textContent-based escape: never inject post fields as raw HTML.
    function esc(s) {
        var d = document.createElement("div");
        d.textContent = s == null ? "" : String(s);
        return d.innerHTML;
    }

    function fmtDate(iso) {
        if (!iso) return "";
        try {
            return new Date(iso).toLocaleDateString("ru-RU", { year: "numeric", month: "long", day: "numeric" });
        } catch (e) {
            return iso;
        }
    }

    function notice(container, text) {
        var p = document.createElement("p");
        p.className = "blog-notice";
        p.textContent = text;
        container.appendChild(p);
    }

    function renderList() {
        var container = document.querySelector("#content .blog-regular") || document.querySelector("#content");
        if (!container) return;
        // Drop the template's demo articles + static pagination.
        container.querySelectorAll(".hentry, .navigation, .pagination").forEach(function (n) {
            n.remove();
        });

        fetch(API)
            .then(function (r) {
                return r.json();
            })
            .then(function (res) {
                var posts = (res && res.data) || [];
                if (!posts.length) {
                    notice(container, "Пока нет записей.");
                    return;
                }
                posts.forEach(function (post) {
                    var href = "blog-single.html?slug=" + encodeURIComponent(post.slug);
                    var art = document.createElement("article");
                    art.className = "hentry post";
                    art.innerHTML =
                        '<header class="entry-header">' +
                        '<h2 class="entry-title"><a href="' + href + '">' + esc(post.title) + "</a></h2>" +
                        '<div class="entry-meta"><span class="entry-date"><time class="entry-date" datetime="' +
                        esc(post.published_at || "") + '">' + esc(fmtDate(post.published_at)) + "</time></span></div>" +
                        "</header>" +
                        '<div class="entry-summary"><p>' + esc(post.summary) + "</p></div>" +
                        '<footer class="entry-footer"><a class="more-link" href="' + href + '">Читать →</a></footer>';
                    container.appendChild(art);
                });
            })
            .catch(function () {
                notice(container, "Не удалось загрузить записи.");
            });
    }

    function renderSingle() {
        var article = document.querySelector("#content .hentry") || document.querySelector("#content");
        if (!article) return;
        var titleEl = article.querySelector(".entry-title");
        var metaEl = article.querySelector(".entry-meta");
        var contentEl = article.querySelector(".entry-content");

        var slug = new URLSearchParams(location.search).get("slug");
        if (!slug) {
            if (titleEl) titleEl.textContent = "Запись не найдена";
            if (contentEl) contentEl.innerHTML = "<p>Не указан адрес записи.</p>";
            return;
        }

        fetch(API + "/" + encodeURIComponent(slug))
            .then(function (r) {
                if (!r.ok) throw new Error("not found");
                return r.json();
            })
            .then(function (res) {
                var post = res && res.data;
                if (!post) throw new Error("not found");
                document.title = post.title + " — Михаил Тарасов";
                if (titleEl) titleEl.textContent = post.title;
                if (metaEl) {
                    metaEl.innerHTML =
                        '<span class="entry-date"><time class="entry-date" datetime="' +
                        esc(post.published_at || "") + '">' + esc(fmtDate(post.published_at)) + "</time></span>";
                }
                if (contentEl) {
                    // Body is Markdown; marked sanitizes structure but not raw HTML,
                    // so authoring is trusted (admin-only). Good enough for a personal blog.
                    contentEl.innerHTML = window.marked ? window.marked.parse(post.body || "") : esc(post.body);
                }
            })
            .catch(function () {
                if (titleEl) titleEl.textContent = "Запись не найдена";
                if (contentEl) contentEl.innerHTML = "<p>Эта запись не существует или ещё не опубликована.</p>";
            });
    }

    document.addEventListener("DOMContentLoaded", function () {
        if (location.pathname.indexOf("blog-single") !== -1) renderSingle();
        else renderList();
    });
})();
