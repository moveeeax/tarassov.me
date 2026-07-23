/*
 * Public blog rendering for tarassov.me.
 *
 * Pulls posts from the backend's unauthenticated read API and renders the
 * "Field notes" design (tag cloud + filter + pagination on the index; progress
 * bar + prev/next on the article page). The static HTML stays static; content
 * is DB-backed (admin-authored).
 *
 *   blog.html          -> GET /api/v1/public/posts          (list)
 *   blog-single.html   -> GET /api/v1/public/posts/{slug}   (one, by ?slug=)
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

    // ── Index (blog.html): tag cloud + OR-filter + client-side pagination ───
    function renderList() {
        var listEl = document.getElementById("blog-posts");
        if (!listEl) return;
        var filterEl = document.getElementById("blog-filter");
        var cloudEl = document.getElementById("blog-cloud");
        var resultEl = document.getElementById("blog-result");
        var clearEl = document.getElementById("blog-clear");
        var pagerEl = document.getElementById("blog-pager");

        var state = { posts: [], active: [], page: 0 };

        fetch(API + "?limit=100")
            .then(function (r) {
                return r.json();
            })
            .then(function (res) {
                state.posts = ((res && res.data) || []).slice().sort(function (x, y) {
                    return (y.published_at || "").localeCompare(x.published_at || "");
                });
                draw();
            })
            .catch(function () {
                listEl.innerHTML = '<p class="blog-loading">Failed to load posts.</p>';
            });

        function toggle(name) {
            var i = state.active.indexOf(name);
            if (i === -1) state.active.push(name);
            else state.active.splice(i, 1);
            state.page = 0;
            draw();
        }

        function matches(p) {
            if (!state.active.length) return true;
            return postTags(p).some(function (t) {
                return state.active.indexOf(t) !== -1;
            });
        }

        function drawCloud() {
            if (!cloudEl) return;
            var counts = {};
            state.posts.forEach(function (p) {
                postTags(p).forEach(function (t) {
                    counts[t] = (counts[t] || 0) + 1;
                });
            });
            var names = Object.keys(counts).sort(function (a, b) {
                return counts[b] - counts[a] || a.localeCompare(b);
            });
            if (!names.length) {
                if (filterEl) filterEl.setAttribute("hidden", "");
                return;
            }
            var cmin = Infinity, cmax = -Infinity;
            names.forEach(function (n) {
                if (counts[n] < cmin) cmin = counts[n];
                if (counts[n] > cmax) cmax = counts[n];
            });
            cloudEl.innerHTML = "";
            names.forEach(function (name) {
                var on = state.active.indexOf(name) !== -1;
                // 12–20px: rarer tags smaller, most-used largest.
                var size = 12 + Math.round(((counts[name] - cmin) / Math.max(1, cmax - cmin)) * 8);
                var b = document.createElement("button");
                b.type = "button";
                b.className = "blog-tag" + (on ? " is-active" : "");
                b.style.fontSize = size + "px";
                b.textContent = name + " " + counts[name];
                b.addEventListener("click", function () {
                    toggle(name);
                });
                cloudEl.appendChild(b);
            });
            if (filterEl) filterEl.removeAttribute("hidden");
        }

        function drawResult() {
            var total = state.posts.length;
            var matched = state.posts.filter(matches).length;
            if (resultEl) {
                resultEl.textContent = state.active.length
                    ? matched + " OF " + total + " · " + state.active.join(" / ")
                    : total + " ARTICLES";
            }
            if (clearEl) {
                if (state.active.length) clearEl.removeAttribute("hidden");
                else clearEl.setAttribute("hidden", "");
            }
        }

        function draw() {
            drawCloud();
            drawResult();

            var posts = state.posts.filter(matches);
            var totalPages = Math.max(1, Math.ceil(posts.length / PER_PAGE));
            if (state.page > totalPages - 1) state.page = totalPages - 1;
            if (state.page < 0) state.page = 0;
            var page = state.page;
            var slice = posts.slice(page * PER_PAGE, page * PER_PAGE + PER_PAGE);

            if (!posts.length) {
                listEl.innerHTML =
                    '<p class="blog-empty">' +
                    (state.active.length ? "No articles match this filter." : "No posts yet.") +
                    "</p>";
            } else {
                listEl.innerHTML = slice
                    .map(function (p) {
                        var chips = postTags(p)
                            .map(function (t) {
                                var on = state.active.indexOf(t) !== -1;
                                return '<span class="blog-chip' + (on ? " is-active" : "") + '">' + esc(t) + "</span>";
                            })
                            .join("");
                        return (
                            '<a class="blog-row" href="blog-single.html?slug=' +
                            encodeURIComponent(p.slug) +
                            '">' +
                            '<div class="blog-row-meta">' +
                            "<div>" + esc(fmtDate(p.published_at)) + "</div>" +
                            '<div class="blog-row-read">' + readMins(p.body) + " MIN</div>" +
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

            drawPager(posts.length, totalPages, page);
        }

        function drawPager(count, totalPages, page) {
            if (!pagerEl) return;
            if (totalPages <= 1) {
                pagerEl.innerHTML = "";
                pagerEl.setAttribute("hidden", "");
                return;
            }
            var from = page * PER_PAGE + 1;
            var to = Math.min(count, page * PER_PAGE + PER_PAGE);

            var row = document.createElement("div");
            row.className = "blog-pager";

            var prev = document.createElement("button");
            prev.type = "button";
            prev.className = "blog-arrow";
            prev.textContent = "← PREV";
            prev.disabled = page <= 0;
            prev.addEventListener("click", function () {
                goTo(page - 1);
            });
            row.appendChild(prev);

            var nums = document.createElement("div");
            nums.className = "blog-pager-nums";
            for (var n = 0; n < totalPages; n++) {
                (function (n) {
                    var btn = document.createElement("button");
                    btn.type = "button";
                    btn.className = "blog-num" + (n === page ? " is-active" : "");
                    btn.textContent = String(n + 1);
                    btn.addEventListener("click", function () {
                        goTo(n);
                    });
                    nums.appendChild(btn);
                })(n);
            }
            row.appendChild(nums);

            var next = document.createElement("button");
            next.type = "button";
            next.className = "blog-arrow";
            next.textContent = "NEXT →";
            next.disabled = page >= totalPages - 1;
            next.addEventListener("click", function () {
                goTo(page + 1);
            });
            row.appendChild(next);

            var range = document.createElement("div");
            range.className = "blog-range";
            range.textContent = from + "–" + to + " OF " + count;

            pagerEl.innerHTML = "";
            pagerEl.appendChild(row);
            pagerEl.appendChild(range);
            pagerEl.removeAttribute("hidden");
        }

        function goTo(p) {
            state.page = p;
            draw();
            window.scrollTo(0, 0);
        }

        if (clearEl)
            clearEl.addEventListener("click", function () {
                state.active = [];
                state.page = 0;
                draw();
            });
    }

    // ── Article (blog-single.html): header, body, tags, prev/next, progress ─
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
                // Wide tables scroll inside their own wrapper, not the 760px column.
                contentEl.querySelectorAll("table").forEach(function (tbl) {
                    var wrap = document.createElement("div");
                    wrap.className = "blog-table-scroll";
                    tbl.parentNode.insertBefore(wrap, tbl);
                    wrap.appendChild(tbl);
                });

                bindProgress(); // re-measure after content lands
                renderPager(slug);
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
                var posts = ((res && res.data) || []).slice().sort(function (x, y) {
                    return (y.published_at || "").localeCompare(x.published_at || "");
                });
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
                    a.querySelector(".title").textContent = post.title;
                    a.removeAttribute("hidden");
                    return true;
                }
                var hasOlder = side("pager-prev", older);
                var hasNewer = side("pager-next", newer);
                if (hasOlder || hasNewer) pager.removeAttribute("hidden");
                bindProgress(); // neighbours change page height
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

                // Desktop plaque line (4A): the whole strip links to the latest post.
                var line = document.getElementById("cover-blog-line");
                var headline = document.getElementById("cover-blog-headline");
                if (line && headline) {
                    line.href = "blog-single.html?slug=" + encodeURIComponent(posts[0].slug);
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
        if (location.pathname.indexOf("blog-single") !== -1) renderSingle();
        else if (document.getElementById("blog-posts")) renderList();
    });
})();
