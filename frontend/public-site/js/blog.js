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

    function readMins(body) {
        var words = (body || "").trim() ? (body || "").trim().split(/\s+/).length : 0;
        return Math.max(1, Math.ceil(words / 200));
    }

    function postTags(p) {
        return Array.isArray(p.tags) ? p.tags : [];
    }

    // blog.html (layout 2A): tag filter + post list + client-side pagination.
    // The tag row stays hidden until some post actually carries tags.
    function renderList() {
        var listEl = document.getElementById("blog-posts");
        if (!listEl) return;
        var tagsEl = document.getElementById("blog-tags");
        var pager = {
            bar: document.getElementById("blog-pager"),
            newer: document.getElementById("blog-pager-newer"),
            older: document.getElementById("blog-pager-older"),
            page: document.getElementById("blog-pager-page"),
        };
        var PAGE_SIZE = 10;
        var state = { posts: [], tag: null, page: 1 };

        fetch(API + "?limit=100")
            .then(function (r) {
                return r.json();
            })
            .then(function (res) {
                state.posts = (res && res.data) || [];
                renderTagFilter();
                draw();
            })
            .catch(function () {
                listEl.innerHTML = '<p class="post-loading">Failed to load posts.</p>';
            });

        function renderTagFilter() {
            if (!tagsEl) return;
            var tags = [];
            state.posts.forEach(function (p) {
                postTags(p).forEach(function (t) {
                    if (tags.indexOf(t) === -1) tags.push(t);
                });
            });
            if (!tags.length) return;
            tagsEl.innerHTML = ['<a href="#" class="active" data-tag="">all</a>']
                .concat(
                    tags.map(function (t) {
                        return '<a href="#" data-tag="' + esc(t) + '">' + esc(t) + "</a>";
                    })
                )
                .join("");
            tagsEl.addEventListener("click", function (e) {
                var a = e.target.closest("a[data-tag]");
                if (!a) return;
                e.preventDefault();
                state.tag = a.getAttribute("data-tag") || null;
                state.page = 1;
                tagsEl.querySelectorAll("a").forEach(function (el) {
                    el.classList.toggle("active", el === a);
                });
                draw();
            });
            tagsEl.removeAttribute("hidden");
        }

        function filtered() {
            if (!state.tag) return state.posts;
            return state.posts.filter(function (p) {
                return postTags(p).indexOf(state.tag) !== -1;
            });
        }

        function draw() {
            var posts = filtered();
            if (!posts.length) {
                listEl.innerHTML =
                    '<p class="post-loading">' +
                    (state.tag ? "no posts tagged #" + esc(state.tag) + " yet" : "No posts yet.") +
                    "</p>";
                pager.bar.setAttribute("hidden", "");
                return;
            }
            var pages = Math.ceil(posts.length / PAGE_SIZE);
            if (state.page > pages) state.page = pages;
            var slice = posts.slice((state.page - 1) * PAGE_SIZE, state.page * PAGE_SIZE);

            listEl.innerHTML = slice
                .map(function (p) {
                    var tags = postTags(p)
                        .map(function (t) {
                            return "<span>#" + esc(t) + "</span>";
                        })
                        .join("");
                    return (
                        '<a class="post-item" href="blog-single.html?slug=' + encodeURIComponent(p.slug) + '">' +
                        '<span class="post-item-meta">' +
                        esc((p.published_at || "").slice(0, 10)) + " · " + readMins(p.body) + " min read" +
                        "</span>" +
                        '<span class="post-item-title">' + esc(p.title) + "</span>" +
                        (p.summary ? '<span class="post-item-excerpt">' + esc(p.summary) + "</span>" : "") +
                        (tags ? '<span class="post-item-tags">' + tags + "</span>" : "") +
                        "</a>"
                    );
                })
                .join("");
            drawPager(pages);
        }

        function drawPager(pages) {
            if (!pager.bar) return;
            if (pages <= 1) {
                pager.bar.setAttribute("hidden", "");
                return;
            }
            pager.page.textContent = "page " + state.page + " / " + pages;
            pager.newer.classList.toggle("disabled", state.page <= 1);
            pager.older.classList.toggle("disabled", state.page >= pages);
            pager.bar.removeAttribute("hidden");
        }

        function step(delta) {
            var pages = Math.ceil(filtered().length / PAGE_SIZE);
            var next = state.page + delta;
            if (next < 1 || next > pages) return;
            state.page = next;
            draw();
            window.scrollTo(0, 0);
        }
        if (pager.newer)
            pager.newer.addEventListener("click", function (e) {
                e.preventDefault();
                step(-1);
            });
        if (pager.older)
            pager.older.addEventListener("click", function (e) {
                e.preventDefault();
                step(1);
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
                if (meta) {
                    meta.textContent = (post.published_at || "").slice(0, 10) + " · " + readMins(body) + " min read";
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

    // vCard home page blog spots: the desktop plaque line (4A) and the mobile
    // "from the blog." teaser (5B). Both stay hidden while the blog is empty.
    function renderLatest() {
        var line = document.getElementById("cover-blog-line");
        var teaser = document.getElementById("teaser-post");
        if (!line && !teaser) return;
        fetch(API + "?limit=1")
            .then(function (r) {
                return r.json();
            })
            .then(function (res) {
                var posts = (res && res.data) || [];
                if (!posts.length) return;
                var post = posts[0];
                var href = "blog-single.html?slug=" + encodeURIComponent(post.slug);
                if (line) {
                    line.href = href;
                    var headline = document.getElementById("cover-blog-headline");
                    if (headline) headline.textContent = post.title;
                    line.removeAttribute("hidden");
                }
                if (teaser) {
                    var meta = document.getElementById("teaser-post-meta");
                    var title = document.getElementById("teaser-post-title");
                    if (meta) meta.textContent = (post.published_at || "").slice(0, 10) + " · " + readMins(post.body) + " min read";
                    if (title) {
                        title.textContent = post.title;
                        title.href = href;
                    }
                    teaser.removeAttribute("hidden");
                }
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
        if (document.getElementById("cover-blog-line") || document.getElementById("teaser-post")) renderLatest();
        bindContact();
        if (location.pathname.indexOf("blog-single") !== -1) renderSingle();
        else if (document.getElementById("blog-posts")) renderList();
    });
})();
