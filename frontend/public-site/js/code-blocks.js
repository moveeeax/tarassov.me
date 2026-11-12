/*
 * Code blocks on blog articles: syntax highlighting, a language label and a
 * Copy button for every <pre><code> that marked rendered.
 *
 * Highlighting is the vendored Prism bundle (js/prism.min.js, loaded with
 * data-manual so it never auto-runs before the body exists). The language
 * comes from the fence info string: marked turns ```hcl into
 * <code class="language-hcl">. Names Prism doesn't know under that spelling
 * go through ALIASES; anything still unknown stays plain text but keeps the
 * label and the Copy button.
 *
 * Exposes window.BlogCode = { enhance, languageOf, prismLanguage, ALIASES };
 * js/blog.js calls enhance(contentEl) after rendering the Markdown.
 * frontend/src/lib/publicSiteCode.test.ts runs this file and the Prism
 * bundle in a VM to check the alias table against the bundled grammars.
 */
(function (root) {
    "use strict";

    // Fence names used in posts -> Prism grammar ids in js/prism.min.js.
    var ALIASES = {
        terraform: "hcl",
        tf: "hcl",
        tfvars: "hcl",
        hcl: "hcl",
        console: "shell-session",
        shellsession: "shell-session",
        zsh: "bash",
        golang: "go",
        "c++": "cpp",
        cc: "cpp",
        cxx: "cpp",
        hpp: "cpp",
        h: "c",
        rs: "rust",
        jsonc: "json",
        json5: "json",
        patch: "diff",
        dotenv: "ini",
        conf: "ini",
        cfg: "ini",
        make: "makefile",
        mk: "makefile",
        tsx: "typescript",
        jsx: "javascript",
        psql: "sql",
        postgres: "sql",
        postgresql: "sql",
        text: "plain",
        txt: "plain",
    };

    var COPIED_MS = 1600;

    // "language-hcl foo" -> "hcl"; "" when there is no language class.
    function languageOf(className) {
        var m = /(?:^|\s)lang(?:uage)?-(\S+)/i.exec(className || "");
        return m ? m[1].toLowerCase() : "";
    }

    // Prism grammar id for a fence language, or "" when none is bundled.
    function prismLanguage(lang, languages) {
        if (!lang || !languages) return "";
        var id = Object.prototype.hasOwnProperty.call(ALIASES, lang) ? ALIASES[lang] : lang;
        return id !== "plain" && languages[id] && typeof languages[id] === "object" ? id : "";
    }

    // Clipboard API first (needs a secure context, which the site is); the
    // hidden-textarea fallback covers older browsers and plain-http dev.
    function copyText(text) {
        if (root.navigator && root.navigator.clipboard && root.navigator.clipboard.writeText) {
            return root.navigator.clipboard.writeText(text);
        }
        return new Promise(function (resolve, reject) {
            var doc = root.document;
            var ta = doc.createElement("textarea");
            ta.value = text;
            ta.setAttribute("readonly", "");
            ta.style.position = "fixed";
            ta.style.top = "-1000px";
            doc.body.appendChild(ta);
            ta.select();
            var ok = false;
            try {
                ok = doc.execCommand("copy");
            } catch (e) {
                ok = false;
            }
            doc.body.removeChild(ta);
            if (ok) resolve();
            else reject(new Error("copy failed"));
        });
    }

    function copyButton(code) {
        var btn = root.document.createElement("button");
        btn.type = "button";
        btn.className = "blog-code-copy";
        btn.textContent = "COPY";
        btn.setAttribute("aria-label", "Copy code to clipboard");
        var timer = null;
        btn.addEventListener("click", function () {
            // textContent of the <code>: Prism only wraps tokens in spans,
            // so this is exactly the source text, line breaks included.
            copyText(code.textContent).then(
                function () { flash("COPIED"); },
                function () { flash("FAILED"); }
            );
        });
        function flash(label) {
            btn.textContent = label;
            if (timer) clearTimeout(timer);
            timer = setTimeout(function () { btn.textContent = "COPY"; }, COPIED_MS);
        }
        return btn;
    }

    // Wrap each top-level <pre><code> in .blog-code with a label + Copy bar,
    // then highlight it. Idempotent: an already wrapped block is skipped.
    function enhance(container) {
        if (!container || !container.querySelectorAll) return;
        var prism = root.Prism;
        var blocks = container.querySelectorAll("pre > code");
        Array.prototype.forEach.call(blocks, function (code) {
            var pre = code.parentNode;
            if (!pre || !pre.parentNode || pre.parentNode.classList.contains("blog-code")) return;

            var lang = languageOf(code.className);
            var grammar = prism ? prismLanguage(lang, prism.languages) : "";

            var wrap = root.document.createElement("div");
            wrap.className = "blog-code";
            var bar = root.document.createElement("div");
            bar.className = "blog-code-bar";
            var label = root.document.createElement("span");
            label.className = "blog-code-lang";
            label.textContent = lang ? lang.toUpperCase() : "CODE";
            bar.appendChild(label);
            bar.appendChild(copyButton(code));

            pre.parentNode.insertBefore(wrap, pre);
            wrap.appendChild(bar);
            wrap.appendChild(pre);

            if (grammar) {
                // Prism reads the grammar from the class; normalise aliases
                // (language-terraform -> language-hcl) before highlighting.
                code.className = code.className.replace(/(?:^|\s)lang(?:uage)?-\S+/gi, "").trim();
                code.classList.add("language-" + grammar);
                pre.classList.add("language-" + grammar);
                try {
                    prism.highlightElement(code);
                } catch (e) {
                    // A grammar bug must not take the article down; the block
                    // stays readable as plain text.
                }
            }
        });
    }

    root.BlogCode = { enhance: enhance, languageOf: languageOf, prismLanguage: prismLanguage, ALIASES: ALIASES };
})(typeof window !== "undefined" ? window : this);
