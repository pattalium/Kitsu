import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFile } from "node:fs/promises";
import path from "node:path";
import test from "node:test";

const root = path.resolve(import.meta.dirname, "..");
const read = (relative) => readFile(path.join(root, relative), "utf8");

function relativeLuminance(hex) {
  const channels = hex.match(/[0-9a-f]{2}/giu).map((value) => Number.parseInt(value, 16) / 255);
  const linear = channels.map((value) =>
    value <= 0.04045 ? value / 12.92 : ((value + 0.055) / 1.055) ** 2.4,
  );
  return linear[0] * 0.2126 + linear[1] * 0.7152 + linear[2] * 0.0722;
}

function contrast(foreground, background) {
  const values = [relativeLuminance(foreground), relativeLuminance(background)]
    .sort((left, right) => right - left);
  return (values[0] + 0.05) / (values[1] + 0.05);
}

test("login form keeps Keycloak security fields and adds owner guidance", async () => {
  const source = await read("kitsu/login/login.ftl");
  assert.match(source, /action="\$\{url\.loginAction\}"/u);
  assert.match(source, /autocomplete="current-password"/u);
  assert.match(source, /label=msg\("kitsuOwnerUsername"\)/u);
  assert.match(source, /name="credentialId"/u);
  assert.match(source, /<@buttons\.loginButton \/>/u);
  assert.match(source, /kitsuBluetoothNoAccount/u);
  assert.match(source, /kitsuFirstLoginHelp/u);
  assert.match(source, /docs\.k32\.run\/android\/#owner-account/u);
  assert.doesNotMatch(source, /url\.registrationUrl|doRegister/u);
  const messages = await read("kitsu/login/messages/messages_en.properties");
  assert.match(messages, /^kitsuOwnerUsername=Owner username$/mu);
});

test("first-login password page preserves required-action validation", async () => {
  const source = await read("kitsu/login/login-update-password.ftl");
  assert.match(source, /name="password-new"/u);
  assert.match(source, /name="password-confirm"/u);
  assert.match(source, /autocomplete="new-password"/u);
  assert.match(source, /<@validator\.templates \/>/u);
  assert.match(source, /<@validator\.script field="password-new" \/>/u);
  assert.match(source, /kitsuChoosePasswordHelp/u);
});

test("theme fixes white-on-white labels and has a narrow-screen layout", async () => {
  const css = await read("kitsu/login/resources/css/kitsu-login.css");
  const action = css.match(/--kitsu-orange-action:\s*(#[0-9a-f]{6})/iu)?.[1];
  const actionHover = css.match(/--kitsu-orange-action-hover:\s*(#[0-9a-f]{6})/iu)?.[1];
  assert.equal(action, "#a63c13");
  assert.equal(actionHover, "#7d2b0d");
  assert.match(css, /\.pf-v5-c-login__main[\s\S]*background: var\(--kitsu-paper\)/u);
  assert.match(css, /\.pf-v5-c-form__label-text[\s\S]*color: var\(--kitsu-ink\) !important/u);
  assert.match(css, /@media \(max-width: 37\.5rem\)/u);
  assert.match(css, /min-height: 100dvh/u);
  assert.match(css, /:focus-visible/u);
  assert.match(css, /prefers-reduced-motion/u);
  assert.ok(contrast("#211713", "#fffaf4") >= 4.5, "form label contrast must be at least 4.5:1");
  assert.ok(contrast("#ffffff", action) >= 4.5, "primary button contrast must be at least 4.5:1");
  assert.ok(contrast("#ffffff", actionHover) >= 4.5, "hover button contrast must be at least 4.5:1");
});

test("theme uses the exact public mascot master", async () => {
  const themeMascot = await readFile(
    path.join(root, "kitsu/login/resources/img/kitsu-k32-mascot-bw-v2.png"),
  );
  const publicMascot = await readFile(
    path.resolve(root, "../../../assets/brand/kitsu-app-icon.png"),
  );
  assert.deepEqual(themeMascot, publicMascot);
  assert.equal(
    createHash("sha256").update(themeMascot).digest("hex"),
    "4f850b551e8fc242b0b31577ab76407cf1ade0e1a59bfaaf21edde3653b0ef42",
  );
});

test("reconciler activates only the named theme and keeps account creation closed", async () => {
  const source = await read("reconcile-keycloak-theme.mjs");
  assert.match(source, /loginTheme: "kitsu"/u);
  assert.match(source, /registrationAllowed !== false/u);
  assert.match(source, /resetPasswordAllowed !== false/u);
  assert.match(source, /Kitsu owner access/u);
  assert.match(source, /id=\\"username\\"/u);
  assert.doesNotMatch(source, /registrationAllowed:\s*true|resetPasswordAllowed:\s*true/u);
});
