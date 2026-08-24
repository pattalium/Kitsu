const storageKey = "kitsu-theme";
const root = document.documentElement;
const systemTheme = window.matchMedia("(prefers-color-scheme: dark)");

function savedTheme() {
  try {
    const value = window.localStorage.getItem(storageKey);
    return value === "light" || value === "dark" ? value : null;
  } catch {
    return null;
  }
}

function preferredTheme() {
  return savedTheme() || (systemTheme.matches ? "dark" : "light");
}

function updateControls(theme) {
  const nextTheme = theme === "dark" ? "light" : "dark";
  for (const control of document.querySelectorAll("[data-theme-toggle]")) {
    control.setAttribute("aria-label", `Switch to ${nextTheme} theme`);
    control.setAttribute("title", `Switch to ${nextTheme} theme`);
    control.setAttribute("aria-pressed", String(theme === "dark"));
    const label = control.querySelector("[data-theme-label]");
    if (label) label.textContent = theme === "dark" ? "Dark" : "Light";
  }
}

function applyTheme(theme, persist) {
  root.dataset.theme = theme;
  root.style.colorScheme = theme;
  const themeColor = document.querySelector('meta[name="theme-color"]');
  if (themeColor) themeColor.content = theme === "dark" ? "#12110f" : "#f3efe5";
  if (persist) {
    try {
      window.localStorage.setItem(storageKey, theme);
    } catch {
      // The selected theme still applies when storage is unavailable.
    }
  }
  updateControls(theme);
}

function bindControls() {
  updateControls(root.dataset.theme);
  for (const control of document.querySelectorAll("[data-theme-toggle]")) {
    control.addEventListener("click", () => {
      applyTheme(root.dataset.theme === "dark" ? "light" : "dark", true);
    });
  }
}

applyTheme(preferredTheme(), false);

if (document.readyState === "loading") {
  document.addEventListener("DOMContentLoaded", bindControls, { once: true });
} else {
  bindControls();
}

systemTheme.addEventListener("change", () => {
  if (!savedTheme()) applyTheme(preferredTheme(), false);
});
