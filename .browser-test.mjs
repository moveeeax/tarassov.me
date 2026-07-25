import { chromium } from "playwright";

const browser = await chromium.launch();

async function testPage(url) {
  console.log("\n===== " + url + " =====");
  const page = await browser.newPage();
  const logs = [];
  page.on("console", (m) => { if (m.type() !== "log") logs.push(m.type() + ": " + m.text().slice(0, 300)); });
  page.on("pageerror", (e) => logs.push("PAGEERROR: " + String(e).slice(0, 300)));
  page.on("requestfailed", (r) => logs.push("REQFAIL: " + r.url().slice(0, 120) + " :: " + (r.failure()?.errorText || "")));
  await page.goto(url, { waitUntil: "networkidle", timeout: 30000 });
  await page.waitForTimeout(2500);
  const state = await page.evaluate(() => {
    const g = (id) => document.getElementById(id);
    return {
      content: g("post-content") ? g("post-content").textContent.trim().slice(0, 90) : null,
      metaHidden: g("post-meta") ? g("post-meta").hasAttribute("hidden") : null,
      pagerHidden: g("post-pager") ? g("post-pager").hasAttribute("hidden") : null,
      listChildren: g("blog-posts") ? g("blog-posts").children.length : null,
      topicChips: g("blog-topics") ? g("blog-topics").children.length : null,
    };
  });
  const js = await page.evaluate(async () => {
    const r = await fetch("/js/blog.js", { cache: "no-store" });
    const t = await r.text();
    return { status: r.status, cfCache: r.headers.get("cf-cache-status"), hasFacets: t.includes("include=facets"), hasLimit1000: t.includes("limit=1000") };
  });
  console.log("state:", JSON.stringify(state));
  console.log("blog.js fetched fresh:", JSON.stringify(js));
  console.log("errors:", logs.length ? logs.slice(0, 12) : "none");
  await page.close();
}

await testPage("https://tarassov.me/blog/7-reverse-integer");
await testPage("https://tarassov.me/blog.html");
await browser.close();
