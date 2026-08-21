#!/usr/bin/env node

import { createHash } from "node:crypto";
import { lstat, readFile } from "node:fs/promises";
import { request } from "node:http";
import path from "node:path";
import process from "node:process";

const [adminCredentialPath, themeRootPath] = process.argv.slice(2);
if (!adminCredentialPath || !themeRootPath) {
  throw new Error("usage: reconcile-keycloak-theme ADMIN_CREDENTIAL THEME_ROOT");
}

const requiredThemeFiles = [
  "login/theme.properties",
  "login/login.ftl",
  "login/login-update-password.ftl",
  "login/messages/messages_en.properties",
  "login/resources/css/kitsu-login.css",
  "login/resources/img/kitsu-k32-mascot-bw-v2.png",
];

for (const relative of requiredThemeFiles) {
  const fullPath = path.join(themeRootPath, ...relative.split("/"));
  const metadata = await lstat(fullPath);
  if (!metadata.isFile() || metadata.isSymbolicLink()) {
    throw new Error(`unsafe or missing theme file: ${relative}`);
  }
}

const mascot = await readFile(
  path.join(themeRootPath, "login/resources/img/kitsu-k32-mascot-bw-v2.png"),
);
const mascotSha256 = createHash("sha256").update(mascot).digest("hex");
if (mascotSha256 !== "4f850b551e8fc242b0b31577ab76407cf1ade0e1a59bfaaf21edde3653b0ef42") {
  throw new Error("theme mascot does not match the authoritative Kitsu asset");
}

function localRequest(requestPath, { method = "GET", headers = {}, body } = {}) {
  return new Promise((resolve, reject) => {
    const requestBody = body === undefined ? undefined : Buffer.from(body);
    const outgoing = request({
      hostname: "127.0.0.1",
      port: 8789,
      path: requestPath,
      method,
      headers: {
        ...headers,
        ...(requestBody === undefined ? {} : { "content-length": requestBody.length }),
      },
    }, (response) => {
      const chunks = [];
      let length = 0;
      response.on("data", (chunk) => {
        length += chunk.length;
        if (length > 1024 * 1024) {
          outgoing.destroy(new Error("Keycloak response exceeded 1 MiB"));
          return;
        }
        chunks.push(chunk);
      });
      response.on("end", () => resolve({
        status: response.statusCode ?? 0,
        headers: response.headers,
        body: Buffer.concat(chunks),
      }));
    });
    outgoing.setTimeout(5_000, () => outgoing.destroy(new Error("Keycloak request timed out")));
    outgoing.on("error", reject);
    if (requestBody !== undefined) outgoing.write(requestBody);
    outgoing.end();
  });
}

function requireSuccess(response, label) {
  if (response.status < 200 || response.status >= 300) {
    throw new Error(`${label} failed: HTTP ${response.status}`);
  }
}

function parseJson(response, label) {
  try {
    return JSON.parse(response.body.toString("utf8"));
  } catch {
    throw new Error(`${label} returned invalid JSON`);
  }
}

const rawAdminPassword = await readFile(adminCredentialPath, "utf8");
const adminPassword = rawAdminPassword.replace(/[\r\n]+$/u, "");
if (adminPassword.length < 32) throw new Error("invalid Keycloak admin credential");

const tokenBody = new URLSearchParams({
  grant_type: "password",
  client_id: "admin-cli",
  username: "kitsu-bootstrap-admin",
  password: adminPassword,
});
const tokenResponse = await localRequest("/realms/master/protocol/openid-connect/token", {
  method: "POST",
  headers: { "content-type": "application/x-www-form-urlencoded" },
  body: tokenBody.toString(),
});
requireSuccess(tokenResponse, "Keycloak bootstrap authentication");
const accessToken = parseJson(tokenResponse, "Keycloak bootstrap authentication").access_token;
if (typeof accessToken !== "string" || accessToken.length < 32) {
  throw new Error("Keycloak returned no bootstrap access token");
}
const adminHeaders = { authorization: `Bearer ${accessToken}` };

const realmResponse = await localRequest("/admin/realms/kitsu", { headers: adminHeaders });
requireSuccess(realmResponse, "read Kitsu realm");
const realm = parseJson(realmResponse, "Kitsu realm");
if (realm.registrationAllowed !== false || realm.resetPasswordAllowed !== false) {
  throw new Error("refusing to theme a realm with public registration or email reset enabled");
}
if (realm.loginTheme !== "kitsu") {
  const update = await localRequest("/admin/realms/kitsu", {
    method: "PUT",
    headers: { ...adminHeaders, "content-type": "application/json" },
    body: JSON.stringify({ ...realm, loginTheme: "kitsu" }),
  });
  requireSuccess(update, "activate Kitsu login theme");
}

const verifiedRealmResponse = await localRequest("/admin/realms/kitsu", { headers: adminHeaders });
requireSuccess(verifiedRealmResponse, "verify Kitsu realm theme");
const verifiedRealm = parseJson(verifiedRealmResponse, "verified Kitsu realm");
if (
  verifiedRealm.loginTheme !== "kitsu" ||
  verifiedRealm.registrationAllowed !== false ||
  verifiedRealm.resetPasswordAllowed !== false
) {
  throw new Error("Kitsu realm theme or account-creation policy did not reconcile");
}

const query = new URLSearchParams({
  client_id: "kitsu-native",
  redirect_uri: "app.kitsu.mobile:/oauth2redirect",
  response_type: "code",
  scope: "openid profile offline_access kitsu.owner",
  state: "theme-probe",
  nonce: "theme-probe",
  code_challenge: "A".repeat(43),
  code_challenge_method: "S256",
});
const loginResponse = await localRequest(
  `/realms/kitsu/protocol/openid-connect/auth?${query}`,
);
requireSuccess(loginResponse, "probe themed Kitsu authorization page");
const loginHtml = loginResponse.body.toString("utf8");
for (const expected of [
  "/login/kitsu/css/kitsu-login.css",
  "Kitsu owner access",
  "Using a nearby Kitsu over Bluetooth does not require an owner account.",
  "Kitsu",
  "id=\"username\"",
  "id=\"password\"",
  "id=\"kc-login\"",
]) {
  if (!loginHtml.includes(expected)) {
    throw new Error(`themed authorization page is missing ${expected}`);
  }
}

process.stdout.write(
  "Kitsu login theme active; fields, mascot, owner guidance, and closed registration policy verified.\n",
);
