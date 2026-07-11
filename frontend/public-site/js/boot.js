/**
 * Runs in <head> before first paint.
 *
 * 1. Applies `.safe-mod` early (same rules as main.js) so mobile never
 *    flashes the desktop 3D book layout for a frame, then reflows.
 * 2. Schedules a failsafe `.is-ready` so a JS error can't leave the page blank.
 */
(function () {
  var html = document.documentElement;

  function needsSafeMod() {
    if (html.getAttribute('data-safeMod') === 'true') return true;
    // main.js uses $(window).width() < 960 — match that threshold here.
    if (window.innerWidth < 960) return true;
    var ua = navigator.userAgent;
    if (/MSIE |Trident\/7\./.test(ua)) return true;
    if (/Android/i.test(ua)) return true;
    // Book layout needs 3D transforms.
    try {
      if (window.CSS && CSS.supports) {
        var ok =
          CSS.supports('perspective', '1px') ||
          CSS.supports('-webkit-perspective', '1px') ||
          CSS.supports('transform-style', 'preserve-3d') ||
          CSS.supports('-webkit-transform-style', 'preserve-3d');
        if (!ok) return true;
      }
    } catch (e) {
      return true;
    }
    return false;
  }

  if (needsSafeMod()) {
    html.classList.add('safe-mod');
  }

  // If main.js never finishes, still reveal content after a short wait.
  setTimeout(function () {
    html.classList.add('is-ready');
  }, 2500);
})();
