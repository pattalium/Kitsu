(function () {
    "use strict";

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
        for (const control of document.querySelectorAll("[data-kitsu-theme-toggle]")) {
            control.setAttribute("aria-label", `Switch to ${nextTheme} theme`);
            control.setAttribute("title", `Switch to ${nextTheme} theme`);
            control.setAttribute("aria-pressed", String(theme === "dark"));
            const label = control.querySelector("[data-kitsu-theme-label]");
            if (label) label.textContent = theme === "dark" ? "Dark" : "Light";
        }
    }

    function applyTheme(theme, persist) {
        root.setAttribute("data-kitsu-theme", theme);
        root.style.colorScheme = theme;
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
        updateControls(root.getAttribute("data-kitsu-theme"));
        for (const control of document.querySelectorAll("[data-kitsu-theme-toggle]")) {
            control.addEventListener("click", () => {
                applyTheme(root.getAttribute("data-kitsu-theme") === "dark" ? "light" : "dark", true);
            });
        }
    }

    applyTheme(preferredTheme(), false);

    if (document.readyState === "loading") {
        document.addEventListener("DOMContentLoaded", bindControls, { once: true });
    } else {
        bindControls();
    }

    const followSystemTheme = () => {
        if (!savedTheme()) applyTheme(preferredTheme(), false);
    };
    if (typeof systemTheme.addEventListener === "function") {
        systemTheme.addEventListener("change", followSystemTheme);
    } else if (typeof systemTheme.addListener === "function") {
        systemTheme.addListener(followSystemTheme);
    }
}());
