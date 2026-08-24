(function () {
  "use strict";
  const key = "kitsu-theme";
  let stored = null;
  try { stored = localStorage.getItem(key); } catch { /* storage is optional */ }
  const theme = stored === "light" || stored === "dark"
    ? stored
    : matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
  document.documentElement.dataset.theme = theme;
  document.documentElement.style.colorScheme = theme;
  const meta = document.querySelector('meta[name="theme-color"]');
  if (meta) meta.content = theme === "dark" ? "#12110f" : "#f3efe5";
})();
