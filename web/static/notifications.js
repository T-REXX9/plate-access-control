(() => {
  "use strict";

  const AUTO_DISMISS_MS = 4000;
  const FADE_DURATION_MS = 220;

  function dismiss(notification) {
    if (notification.classList.contains("is-dismissing")) return;
    notification.classList.add("is-dismissing");
    window.setTimeout(() => notification.remove(), FADE_DURATION_MS);
  }

  document.querySelectorAll(".flash").forEach((notification) => {
    const timer = window.setTimeout(() => dismiss(notification), AUTO_DISMISS_MS);
    notification.addEventListener("mouseenter", () => {
      window.clearTimeout(timer);
      dismiss(notification);
    }, { once: true });
  });
})();
