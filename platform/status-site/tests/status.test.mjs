import assert from "node:assert/strict";
import { access, readFile } from "node:fs/promises";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

import { validateHealthResponse } from "../status-response.js";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

test("status page checks only the four public release surfaces", async () => {
  const [html, script] = await Promise.all([
    readFile(path.join(root, "index.html"), "utf8"),
    readFile(path.join(root, "status.js"), "utf8"),
  ]);
  assert.deepEqual(
    [...html.matchAll(/data-url="([^"]+)"/g)].map((match) => match[1]),
    ["/checks/public", "/checks/flash", "/checks/docs", "/checks/updates"],
  );
  assert.match(html, /Nearby Kitsu control is local Bluetooth/i);
  assert.match(html, /<link rel="icon" href="data:,"/);
  assert.match(html, /class="summary" aria-live="polite" aria-atomic="true"/);
  assert.match(html, /does not claim that a new Bluetooth package passed physical acceptance/i);
  assert.doesNotMatch(`${html}${script}`, /checks\/(?:app|api|auth|gateway)|data-kind|OIDC|owner API|device gateway/i);
  assert.doesNotMatch(script, /gated|Not promoted|counted/);
});

test("health validation fails closed for every non-200 or unexpected body", async () => {
  await assert.doesNotReject(validateHealthResponse({ ok: true, status: 200, text: async () => "ok\n" }));
  await assert.rejects(
    validateHealthResponse({ ok: false, status: 503, text: async () => "not promoted" }),
    /HTTP 503/,
  );
  await assert.rejects(
    validateHealthResponse({ ok: true, status: 200, text: async () => "ready" }),
    /unexpected health response/,
  );
});

test("every local status asset exists and dead state styling is absent", async () => {
  await Promise.all(["index.html", "styles.css", "status.js", "status-response.js"].map((file) => access(path.join(root, file))));
  const styles = await readFile(path.join(root, "styles.css"), "utf8");
  assert.doesNotMatch(styles, /\.dot\.(?:gated|private)/);
});

test("status text assets are valid UTF-8 without mojibake", async () => {
  for (const file of ["index.html", "styles.css", "status.js", "status-response.js"]) {
    const text = await readFile(path.join(root, file), "utf8");
    assert.doesNotMatch(text, /(?:Ã‚|Ã¢â‚¬|Ãƒ|ï¿½)/, file);
  }
});
