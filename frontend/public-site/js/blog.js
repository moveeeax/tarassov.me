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
            return new Date(iso).toLocaleDateString("en-US", { year: "numeric", month: "long", day: "numeric" });
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
                    notice(container, "No posts yet.");
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
                        '<footer class="entry-footer"><a class="more-link" href="' + href + '">Read →</a></footer>';
                    container.appendChild(art);
                });
            })
            .catch(function () {
                notice(container, "Failed to load posts.");
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
            if (titleEl) titleEl.textContent = "Post not found";
            if (contentEl) contentEl.innerHTML = "<p>No post specified.</p>";
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
                document.title = post.title + " — Michael Tarassov";
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
                if (titleEl) titleEl.textContent = "Post not found";
                if (contentEl) contentEl.innerHTML = "<p>This post does not exist or has not been published yet.</p>";
            });
    }

    // The vCard's "latest from the blog" widget (index.html #latest-posts).
    // The whole widget ships hidden and is only revealed when the API returns
    // posts — an empty blog (or a failed fetch) must not leave an unreadable
    // "No posts yet" notice sitting on the cover photo.
    function renderLatest() {
        var el = document.getElementById("latest-posts");
        if (!el) return;
        var widget = el.closest ? el.closest(".latest-from-blog") : null;
        // 2 posts max: the widget sits on the cover photo and must stay compact.
        fetch(API + "?limit=2")
            .then(function (r) {
                return r.json();
            })
            .then(function (res) {
                var posts = (res && res.data) || [];
                if (!posts.length) return;
                el.innerHTML = posts
                    .map(function (p) {
                        return (
                            '<h2><a href="blog-single.html?slug=' +
                            encodeURIComponent(p.slug) +
                            '">' +
                            esc(p.title) +
                            "</a></h2>"
                        );
                    })
                    .join("");
                if (widget) widget.removeAttribute("hidden");
            })
            .catch(function () {});
    }

    // Contact form (index.html #contact-form) -> POST /api/v1/public/contact.
    function bindContact() {
        var form = document.getElementById("contact-form");
        if (!form) return;
        var status = document.getElementById("cf-status");
        form.addEventListener("submit", function (e) {
            e.preventDefault();
            var btn = form.querySelector('button[type="submit"]');
            // form.elements[...] (not form.name — that resolves the form's own name attr).
            var payload = {
                name: form.elements["name"].value.trim(),
                email: form.elements["email"].value.trim(),
                subject: form.elements["subject"] ? form.elements["subject"].value.trim() : "",
                message: form.elements["message"].value.trim(),
            };
            if (status) status.textContent = "Sending…";
            if (btn) btn.disabled = true;
            fetch("/api/v1/public/contact", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify(payload),
            })
                .then(function (r) {
                    return r.json().then(function (b) {
                        return { ok: r.ok, body: b };
                    });
                })
                .then(function (res) {
                    if (res.ok) {
                        if (status) status.textContent = "Thanks! Your message has been sent.";
                        form.reset();
                    } else if (status) {
                        status.textContent = (res.body && (res.body.message || res.body.error)) || "Failed to send.";
                    }
                })
                .catch(function () {
                    if (status) status.textContent = "Network error.";
                })
                .finally(function () {
                    if (btn) btn.disabled = false;
                });
        });
    }

    document.addEventListener("DOMContentLoaded", function () {
        if (document.getElementById("latest-posts")) renderLatest();
        bindContact();
        if (location.pathname.indexOf("blog-single") !== -1) renderSingle();
        else if (document.querySelector("#content .blog-regular")) renderList();
    });
})();
