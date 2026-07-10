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

    // blog-single.html (layout 1A): meta line with reading time, optional
    // tags, Markdown body, prev/next neighbours from the public posts index.
    function renderSingle() {
        var meta = document.getElementById("post-meta");
        var titleEl = document.getElementById("post-title");
        var tagsEl = document.getElementById("post-tags");
        var contentEl = document.getElementById("post-content");
        if (!contentEl) return;

        function fail(msg) {
            if (titleEl) {
                titleEl.textContent = "Post not found";
                titleEl.removeAttribute("hidden");
            }
            contentEl.innerHTML = '<p class="post-loading">' + esc(msg) + "</p>";
        }

        var slug = new URLSearchParams(location.search).get("slug");
        if (!slug) {
            fail("No post specified.");
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

                var body = post.body || "";
                var words = body.trim() ? body.trim().split(/\s+/).length : 0;
                var minutes = Math.max(1, Math.ceil(words / 200));
                if (meta) {
                    meta.textContent = (post.published_at || "").slice(0, 10) + " · " + minutes + " min read";
                    meta.removeAttribute("hidden");
                }
                if (titleEl) {
                    titleEl.textContent = post.title;
                    titleEl.removeAttribute("hidden");
                }

                // The backend has no tags today — render the row only if the
                // API ever starts returning them (no empty frames otherwise).
                if (tagsEl && Array.isArray(post.tags) && post.tags.length) {
                    tagsEl.innerHTML = post.tags
                        .map(function (t) {
                            return '<a href="blog.html">' + esc(t) + "</a>";
                        })
                        .join("");
                    tagsEl.removeAttribute("hidden");
                }

                // Body is Markdown; marked sanitizes structure but not raw HTML,
                // so authoring is trusted (admin-only). Good enough for a personal blog.
                contentEl.innerHTML = window.marked ? window.marked.parse(body) : esc(body);
                // Wide tables must scroll inside their own wrapper, not break
                // the 620px column.
                contentEl.querySelectorAll("table").forEach(function (tbl) {
                    var wrap = document.createElement("div");
                    wrap.className = "post-table-scroll";
                    tbl.parentNode.insertBefore(wrap, tbl);
                    wrap.appendChild(tbl);
                });

                renderPager(slug);
            })
            .catch(function () {
                fail("This post does not exist or has not been published yet.");
            });
    }

    // Prev (older) / next (newer): the public index is newest-first
    // (ORDER BY published_at DESC). A side with no neighbour stays hidden;
    // with no neighbours at all the bar never shows.
    function renderPager(slug) {
        var pager = document.getElementById("post-pager");
        if (!pager) return;
        fetch(API + "?limit=100")
            .then(function (r) {
                return r.json();
            })
            .then(function (res) {
                var posts = (res && res.data) || [];
                var i = posts.findIndex(function (p) {
                    return p.slug === slug;
                });
                if (i === -1) return;
                var newer = i > 0 ? posts[i - 1] : null;
                var older = i + 1 < posts.length ? posts[i + 1] : null;
                function side(id, post) {
                    var a = document.getElementById(id);
                    if (!a || !post) return false;
                    a.href = "blog-single.html?slug=" + encodeURIComponent(post.slug);
                    a.querySelector(".post-pager-title").textContent = post.title;
                    a.removeAttribute("hidden");
                    return true;
                }
                var hasOlder = side("pager-prev", older);
                var hasNewer = side("pager-next", newer);
                if (hasOlder || hasNewer) pager.removeAttribute("hidden");
                if (hasOlder !== hasNewer) pager.className += " post-pager-single";
            })
            .catch(function () {});
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
