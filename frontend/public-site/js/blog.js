/*
 * Public blog rendering for tarassov.me.
 *
 * Renders the "Field notes" design on top of the hybrid API contract
 * (spec 2026-07-25): the SERVER filters, pages and computes facets; this
 * script draws and asks for the next page/filter — one request per
 * interaction, never the whole feed.
 *
 *   blog.html      -> GET /api/v1/public/posts?page&topic|tag&include=facets
 *   /blog/<slug>   -> hydrates from the SSR #post-data island (adjacent via
 *                     GET /api/v1/public/posts/{slug}?include=adjacent)
 *
 * Same-origin: nginx proxies /api/* to the backend, so no CORS.
 */
(function () {
    "use strict";

    var API = "/api/v1/public/posts";
    var PER_PAGE = 10;
    var MONTHS = ["JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"];

    // textContent-based escape: never inject post fields as raw HTML.
    function esc(s) {
        var d = document.createElement("div");
        d.textContent = s == null ? "" : String(s);
        return d.innerHTML;
    }

    function readMins(body) {
        var t = (body || "").trim();
        var words = t ? t.split(/\s+/).length : 0;
        return Math.max(1, Math.ceil(words / 200));
    }

    // "2025-06-20T..." | "2025-06-20" -> "20 JUN 2025" (uppercase). Falls back
    // to the raw slice if the shape is unexpected.
    function fmtDate(iso) {
        var s = (iso || "").slice(0, 10);
        var m = /^(\d{4})-(\d{2})-(\d{2})$/.exec(s);
        if (!m) return s.toUpperCase();
        return m[3] + " " + MONTHS[+m[2] - 1] + " " + m[1];
    }

    function postTags(p) {
        return Array.isArray(p.tags) ? p.tags : [];
    }

    // ── Index (blog.html): two-row filter bar (TOPIC / KEYWORD) + one-line
    //    pager. Constant height regardless of dataset size — the topic row is
    //    the real sections, the keyword row is the top 14 tags of the result. ─
    function renderList() {
        var listEl = document.getElementById("blog-posts");
        if (!listEl) return;
        var filterEl = document.getElementById("blog-filter");
        var topicsEl = document.getElementById("blog-topics");
        var keywordsEl = document.getElementById("blog-keywords");
        var keywordRowEl = document.getElementById("blog-keyword-row");
        var resultEl = document.getElementById("blog-result");
        var pagerEl = document.getElementById("blog-pager");

        var PER_PAGE = 10;
        // active: {kind:"topic"|"tag", name} | null. Single-select — the API
        // filters server-side (?topic= / ?tag=); one request per interaction.
        // grandTotal remembers the unfiltered count for the "n / total" line,
        // and topics remembers the unfiltered topics facet (see draw()).
        var state = { active: null, page: 1, grandTotal: null, topics: null };

        function load() {
            var qs = "?page=" + state.page + "&limit=" + PER_PAGE + "&include=facets";
            if (state.active) {
                qs += "&" + state.active.kind + "=" + encodeURIComponent(state.active.name);
            }
            fetch(API + qs)
                .then(function (r) { return r.json(); })
                .then(function (res) {
                    if (state.grandTotal === null && !state.active) state.grandTotal = res.total;
                    draw(res);
                })
                .catch(function () {
                    listEl.innerHTML = '<p class="blog-loading">Failed to load posts.</p>';
                });
        }

        function setFilter(kind, name) {
            var same = state.active && state.active.kind === kind && state.active.name === name;
            state.active = same ? null : { kind: kind, name: name };
            state.page = 1;
            load();
        }

        // One shared chip <button>; keyword (tag) chips render dimmer (is-dim).
        function chip(name, kind) {
            var b = document.createElement("button");
            b.type = "button";
            var on = state.active && state.active.kind === kind && state.active.name === name;
            b.className = "blog-chip-btn" + (kind === "tag" ? " is-dim" : "") + (on ? " is-active" : "");
            b.textContent = name;
            b.addEventListener("click", function () { setFilter(kind, name); });
            return b;
        }

        // The rail is the fixed set of sections, so it is drawn from the
        // remembered UNFILTERED topics facet — not from the current response.
        function drawTopics(topics) {
            if (!topicsEl) return;
            topicsEl.innerHTML = "";
            // "All" clears the filter; active when nothing is selected.
            var all = document.createElement("button");
            all.type = "button";
            all.className = "blog-chip-btn" + (state.active ? "" : " is-active");
            all.textContent = "All";
            all.addEventListener("click", function () {
                state.active = null;
                state.page = 1;
                load();
            });
            topicsEl.appendChild(all);
            (topics || []).forEach(function (t) {
                topicsEl.appendChild(chip(t.name, "topic"));
            });
        }

        // Top 14 tags of the current result (server orders by count desc);
        // row hidden when ≤1 keyword.
        function drawKeywords(facets) {
            if (!keywordsEl || !keywordRowEl) return;
            var names = (facets.tags || []).slice(0, 14);
            if (names.length <= 1) {
                keywordRowEl.setAttribute("hidden", "");
                return;
            }
            keywordsEl.innerHTML = "";
            names.forEach(function (t) { keywordsEl.appendChild(chip(t.name, "tag")); });
            keywordRowEl.removeAttribute("hidden");
        }

        function pad2(n) { return String(n).padStart(2, "0"); }

        function draw(res) {
            if (filterEl) filterEl.removeAttribute("hidden");
            var facets = res.facets || { topics: [], tags: [] };
            // Facets are scoped to the active filter (server contract), so a
            // ?topic= response carries exactly one topic. Cache the rail from
            // the first unfiltered load and keep drawing it, or selecting a
            // topic would erase every other section from the bar.
            if (!state.active && facets.topics) state.topics = facets.topics;
            drawTopics(state.topics || facets.topics);
            drawKeywords(facets);

            if (resultEl) {
                resultEl.textContent = state.active
                    ? res.total + " / " + (state.grandTotal === null ? res.total : state.grandTotal)
                    : res.total + " ARTICLES";
            }

            var items = res.items || [];
            if (!items.length) {
                listEl.innerHTML =
                    '<p class="blog-empty">' +
                    (state.active ? "No articles match this filter." : "No posts yet.") +
                    "</p>";
            } else {
                listEl.innerHTML = items
                    .map(function (p) {
                        var chips = postTags(p)
                            .map(function (t) {
                                var on = state.active && state.active.kind === "tag" && state.active.name === t;
                                return '<span class="blog-chip' + (on ? " is-active" : "") + '">' + esc(t) + "</span>";
                            })
                            .join("");
                        return (
                            '<a class="blog-row" href="/blog/' +
                            encodeURIComponent(p.slug) +
                            '">' +
                            '<div class="blog-row-meta">' +
                            "<div>" + esc(fmtDate(p.published_at)) + "</div>" +
                            '<div class="blog-row-read">' + (p.read_mins || 1) + " MIN</div>" +
                            "</div>" +
                            "<div>" +
                            (p.topic ? '<div class="blog-row-topic">' + esc(p.topic) + "</div>" : "") +
                            '<h3 class="blog-row-title">' + esc(p.title) + "</h3>" +
                            (chips ? '<div class="blog-chips">' + chips + "</div>" : "") +
                            "</div>" +
                            "</a>"
                        );
                    })
                    .join("");
            }

            drawPager(res.total, Math.max(1, Math.ceil(res.total / PER_PAGE)), state.page - 1);
        }

        // One quiet line: "<from>–<to> OF <total>" left, "← 01 / 73 →" right.
        function drawPager(count, totalPages, page) {
            if (!pagerEl) return;
            if (totalPages <= 1) {
                pagerEl.innerHTML = "";
                pagerEl.removeAttribute("class");
                pagerEl.setAttribute("hidden", "");
                return;
            }
            var from = page * PER_PAGE + 1;
            var to = Math.min(count, page * PER_PAGE + PER_PAGE);

            pagerEl.className = "blog-pager";
            pagerEl.innerHTML = "";

            var range = document.createElement("div");
            range.className = "blog-range";
            range.textContent = count ? from + "–" + to + " OF " + count : "NOTHING HERE";
            pagerEl.appendChild(range);

            var nav = document.createElement("div");
            nav.className = "blog-pager-nav";

            var prev = document.createElement("button");
            prev.type = "button";
            prev.className = "blog-arrow";
            prev.textContent = "←";
            prev.disabled = page <= 0;
            prev.addEventListener("click", function () { goTo(page); });
            nav.appendChild(prev);

            var counter = document.createElement("span");
            counter.className = "blog-counter";
            counter.textContent = pad2(page + 1) + " / " + pad2(totalPages);
            nav.appendChild(counter);

            var next = document.createElement("button");
            next.type = "button";
            next.className = "blog-arrow";
            next.textContent = "→";
            next.disabled = page >= totalPages - 1;
            next.addEventListener("click", function () { goTo(page + 2); });
            nav.appendChild(next);

            pagerEl.appendChild(nav);
            pagerEl.removeAttribute("hidden");
        }

        // goTo takes a 1-based page; no scroll-to-top on page change (per spec).
        function goTo(p) {
            state.page = p;
            load();
        }

        load();
    }

    // ── Article (/blog/<slug>): header, body, tags, prev/next, progress ─────
    function renderSingle() {
        var topicEl = document.getElementById("post-topic");
        var titleEl = document.getElementById("post-title");
        var dekEl = document.getElementById("post-dek");
        var metaEl = document.getElementById("post-meta");
        var tagsEl = document.getElementById("post-tags");
        var contentEl = document.getElementById("post-content");
        if (!contentEl) return;

        bindProgress();

        function fail(msg) {
            if (titleEl) {
                titleEl.textContent = "Post not found";
                titleEl.removeAttribute("hidden");
            }
            contentEl.innerHTML = '<p class="blog-loading">' + esc(msg) + "</p>";
        }

        // Clean URL only: /blog/<slug> (the legacy ?slug= shell is gone).
        var pathMatch = location.pathname.match(/^\/blog\/(.+)$/);
        var slug = pathMatch ? decodeURIComponent(pathMatch[1]) : null;
        if (!slug) {
            fail("No post specified.");
            return;
        }

        // SSR island first — the server embeds the post JSON so no second
        // request is needed; fall back to the API if the island is missing.
        // Adjacent posts always come from the API (the island carries none).
        var island = document.getElementById("post-data");
        var islandPost = null;
        if (island) {
            try {
                islandPost = JSON.parse(island.textContent);
            } catch (e) {
                islandPost = null;
            }
        }
        var preview = new URLSearchParams(location.search).get("preview");
        var apiUrl =
            API + "/" + encodeURIComponent(slug) +
            "?include=adjacent" + (preview ? "&preview=" + encodeURIComponent(preview) : "");

        (islandPost
            ? Promise.resolve({ data: islandPost })
            : fetch(apiUrl).then(function (r) {
                  if (!r.ok) throw new Error("not found");
                  return r.json();
              })
        )
            .then(function (res) {
                var post = res && res.data;
                if (!post) throw new Error("not found");
                document.title = post.title + " — Michael Tarassov";

                var body = post.body || "";

                if (topicEl && post.topic) {
                    topicEl.textContent = post.topic;
                    topicEl.removeAttribute("hidden");
                }
                if (titleEl) {
                    titleEl.textContent = post.title;
                    titleEl.removeAttribute("hidden");
                }
                if (dekEl && post.summary) {
                    dekEl.textContent = post.summary;
                    dekEl.removeAttribute("hidden");
                }
                if (metaEl) {
                    metaEl.innerHTML =
                        "<span>MICHAEL TARASSOV</span>" +
                        '<span class="sep">/</span>' +
                        "<span>" + esc(fmtDate(post.published_at)) + "</span>" +
                        '<span class="sep">/</span>' +
                        "<span>" + readMins(body) + " MIN READ</span>";
                    metaEl.removeAttribute("hidden");
                }

                if (tagsEl && Array.isArray(post.tags) && post.tags.length) {
                    // Absolute /blog.html — a relative "blog.html" resolves
                    // against /blog/<slug> to the dead /blog/blog.html.
                    tagsEl.innerHTML = post.tags
                        .map(function (t) {
                            return '<a href="/blog.html">' + esc(t) + "</a>";
                        })
                        .join("");
                    tagsEl.removeAttribute("hidden");
                }

                // Body is Markdown; marked sanitizes structure but not raw HTML,
                // so authoring is trusted (admin-only). Good enough for a personal blog.
                contentEl.innerHTML = window.marked ? window.marked.parse(body) : esc(body);
                // Wide tables scroll inside their own wrapper, not the 760px column.
                contentEl.querySelectorAll("table").forEach(function (tbl) {
                    var wrap = document.createElement("div");
                    wrap.className = "blog-table-scroll";
                    tbl.parentNode.insertBefore(wrap, tbl);
                    wrap.appendChild(tbl);
                });

                bindProgress(); // re-measure after content lands
                renderPager(slug, post.adjacent, preview);
            })
            .catch(function () {
                fail("This post does not exist or has not been published yet.");
            });
    }

    // Scroll progress bar: width = scrollTop / (scrollHeight − clientHeight).
    // Idempotent — safe to call before and after the body renders.
    function bindProgress() {
        var bar = document.getElementById("blog-progress");
        if (!bar) return;
        if (!bindProgress._bound) {
            var onScroll = function () {
                var doc = document.documentElement;
                var max = doc.scrollHeight - doc.clientHeight;
                var pct = max > 0 ? Math.min(100, (doc.scrollTop / max) * 100) : 0;
                bar.style.width = pct + "%";
            };
            window.addEventListener("scroll", onScroll, { passive: true });
            window.addEventListener("resize", onScroll, { passive: true });
            bindProgress._onScroll = onScroll;
            bindProgress._bound = true;
        }
        bindProgress._onScroll();
    }

    // Prev (older) / next (newer) from the by-slug include=adjacent payload —
    // or a one-shot fetch when we hydrated from the SSR island (which carries
    // no adjacent block). A side with no neighbour stays hidden; with no
    // neighbours at all the bar never shows. Previewed drafts have no pager.
    function renderPager(slug, adjacent, preview) {
        var pager = document.getElementById("post-pager");
        if (!pager) return;
        if (preview) return; // a draft has no position in the published feed
        function apply(adj) {
            if (!adj) return;
            function side(id, ref) {
                var a = document.getElementById(id);
                if (!a || !ref) return false;
                a.href = "/blog/" + encodeURIComponent(ref.slug);
                a.querySelector(".title").textContent = ref.title;
                a.removeAttribute("hidden");
                return true;
            }
            var hasOlder = side("pager-prev", adj.prev);
            var hasNewer = side("pager-next", adj.next);
            if (hasOlder || hasNewer) pager.removeAttribute("hidden");
            bindProgress(); // neighbours change page height
        }
        if (adjacent) {
            apply(adjacent);
            return;
        }
        fetch(API + "/" + encodeURIComponent(slug) + "?include=adjacent")
            .then(function (r) {
                return r.json();
            })
            .then(function (res) {
                apply(res && res.data && res.data.adjacent);
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
                var posts = (res && res.items) || [];
                if (!posts.length) return;
                el.innerHTML = posts
                    .map(function (p) {
                        return (
                            '<h2><a href="/blog/' +
                            encodeURIComponent(p.slug) +
                            '">' +
                            esc(p.title) +
                            "</a></h2>"
                        );
                    })
                    .join("");
                if (widget) widget.removeAttribute("hidden");

                // Desktop plaque line (4A): the whole strip links to the latest post.
                var line = document.getElementById("cover-blog-line");
                var headline = document.getElementById("cover-blog-headline");
                if (line && headline) {
                    line.href = "/blog/" + encodeURIComponent(posts[0].slug);
                    headline.textContent = posts[0].title;
                    line.removeAttribute("hidden");
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
        if (document.getElementById("latest-posts")) renderLatest();
        bindContact();
        // Element-based dispatch: the article shell (#post-content) is served
        // by the SSR /blog/<slug> page.
        if (document.getElementById("post-content")) renderSingle();
        else if (document.getElementById("blog-posts")) renderList();
    });
})();
