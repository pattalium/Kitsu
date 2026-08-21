#!/usr/bin/env node

import { randomBytes, randomUUID } from "node:crypto";
import { chmod, mkdir, readFile, unlink, writeFile } from "node:fs/promises";
import { request } from "node:http";
import path from "node:path";
import process from "node:process";

const [adminCredentialPath, stateDirectory, confirmation] = process.argv.slice(2);
if (
  !adminCredentialPath ||
  !stateDirectory ||
  confirmation !== "--confirm-reset=k32-owner"
) {
  throw new Error(
    "usage: recover-owner ADMIN_CREDENTIAL STATE_DIRECTORY --confirm-reset=k32-owner",
  );
}

const ownerUsername = "k32-owner";
const pendingPath = path.join(stateDirectory, "owner-recovery.pending.json");
const localPort = Number.parseInt(process.env.KITSU_KEYCLOAK_LOCAL_PORT ?? "8789", 10);
if (!Number.isInteger(localPort) || localPort < 1 || localPort > 65535) {
  throw new Error("invalid local Keycloak port");
}

function localRequest(requestPath, { method = "GET", headers = {}, body } = {}) {
  return new Promise((resolve, reject) => {
    const requestBody = body === undefined ? undefined : Buffer.from(body);
    const outgoing = request({
      hostname: "127.0.0.1",
      port: localPort,
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

function makePassword() {
  const alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789!@#$%^&*_-+=";
  for (;;) {
    const random = randomBytes(96);
    let password = "";
    for (const byte of random) {
      if (byte >= Math.floor(256 / alphabet.length) * alphabet.length) continue;
      password += alphabet[byte % alphabet.length];
      if (password.length === 32) break;
    }
    if (
      password.length === 32 &&
      /[A-Z]/u.test(password) &&
      /[a-z]/u.test(password) &&
      /[0-9]/u.test(password) &&
      /[^A-Za-z0-9]/u.test(password)
    ) return password;
  }
}

async function readPending() {
  try {
    const pending = JSON.parse(await readFile(pendingPath, "utf8"));
    if (
      pending.schema !== "kitsu.owner-recovery.v1" ||
      pending.username !== ownerUsername ||
      typeof pending.recoveryId !== "string" ||
      !/^[0-9a-f-]{36}$/u.test(pending.recoveryId) ||
      typeof pending.temporaryPassword !== "string" ||
      pending.temporaryPassword.length !== 32 ||
      pending.requiredAction !== "UPDATE_PASSWORD"
    ) {
      throw new Error("invalid pending owner recovery handoff");
    }
    return pending;
  } catch (error) {
    if (error?.code === "ENOENT") return undefined;
    throw error;
  }
}

await mkdir(stateDirectory, { recursive: true, mode: 0o700 });
await chmod(stateDirectory, 0o700);
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

async function findOwner() {
  const response = await localRequest(
    `/admin/realms/kitsu/users?username=${ownerUsername}&exact=true`,
    { headers: adminHeaders },
  );
  requireSuccess(response, "find Kitsu owner");
  const matches = parseJson(response, "Kitsu owner search")
    .filter(({ username }) => username === ownerUsername);
  if (matches.length !== 1) throw new Error("Kitsu owner is missing or ambiguous");
  if (matches[0].enabled !== true) throw new Error("Kitsu owner is disabled");
  return matches[0];
}

async function requireNonAdministrativeOwner(ownerId) {
  const response = await localRequest(
    `/admin/realms/kitsu/users/${ownerId}/role-mappings`,
    { headers: adminHeaders },
  );
  requireSuccess(response, "read Kitsu owner role mappings");
  const mappings = parseJson(response, "Kitsu owner role mappings");
  const forbiddenRealmRole = /^(?:admin|realm-admin|create-realm)$/u;
  const administrativeClientRole = Object.values(mappings.clientMappings ?? {}).some(
    ({ client, mappings: roles }) =>
      ["realm-management", "master-realm"].includes(client) && (roles ?? []).length > 0,
  );
  if (
    (mappings.realmMappings ?? []).some(({ name }) => forbiddenRealmRole.test(name)) ||
    administrativeClientRole
  ) {
    throw new Error("refusing to recover an administratively privileged owner");
  }
}

async function revokeOwnerSessionsAndMark(currentOwner, recovery) {
  const logout = await localRequest(
    `/admin/realms/kitsu/users/${currentOwner.id}/logout`,
    { method: "POST", headers: adminHeaders },
  );
  requireSuccess(logout, "revoke existing Kitsu owner sessions");
  const marked = await localRequest(`/admin/realms/kitsu/users/${currentOwner.id}`, {
    method: "PUT",
    headers: { ...adminHeaders, "content-type": "application/json" },
    body: JSON.stringify({
      ...currentOwner,
      attributes: {
        ...(currentOwner.attributes ?? {}),
        "kitsu.recovery.id": [recovery.recoveryId],
        "kitsu.recovery.started-at": [recovery.createdAt],
        "kitsu.recovery.sessions-revoked": ["true"],
      },
    }),
  });
  requireSuccess(marked, "record Kitsu owner session revocation");
}

let owner = await findOwner();
await requireNonAdministrativeOwner(owner.id);
let pending = await readPending();
const activeRecoveryId = owner.attributes?.["kitsu.recovery.id"]?.[0];
const requiredActions = new Set(owner.requiredActions ?? []);

if (
  pending &&
  typeof activeRecoveryId === "string" &&
  activeRecoveryId !== pending.recoveryId &&
  requiredActions.has("UPDATE_PASSWORD")
) {
  throw new Error("a different owner recovery is already awaiting password change");
}

if (pending && activeRecoveryId === pending.recoveryId) {
  if (requiredActions.has("UPDATE_PASSWORD")) {
    if (owner.attributes?.["kitsu.recovery.sessions-revoked"]?.[0] !== "true") {
      await revokeOwnerSessionsAndMark(owner, pending);
    }
    process.stdout.write(
      "Owner recovery is already armed; use the existing private one-time handoff.\n",
    );
    process.exit(0);
  }
  await unlink(pendingPath);
  process.stdout.write(
    "The recovered owner changed its password; the consumed one-time handoff was removed. " +
      "Run the command again only to start a new recovery.\n",
  );
  process.exit(0);
}

if (!pending) {
  pending = {
    schema: "kitsu.owner-recovery.v1",
    recoveryId: randomUUID(),
    username: ownerUsername,
    temporaryPassword: makePassword(),
    requiredAction: "UPDATE_PASSWORD",
    createdAt: new Date().toISOString(),
    issuer: "https://auth.k32.run/realms/kitsu",
  };
  await writeFile(pendingPath, `${JSON.stringify(pending, null, 2)}\n`, {
    encoding: "utf8",
    mode: 0o600,
    flag: "wx",
  });
}

// The recoverable 0600 handoff is durable before the old password is invalidated.
const reset = await localRequest(`/admin/realms/kitsu/users/${owner.id}/reset-password`, {
  method: "PUT",
  headers: { ...adminHeaders, "content-type": "application/json" },
  body: JSON.stringify({
    type: "password",
    value: pending.temporaryPassword,
    temporary: true,
  }),
});
requireSuccess(reset, "reset Kitsu owner password");

const updated = await localRequest(`/admin/realms/kitsu/users/${owner.id}`, {
  method: "PUT",
  headers: { ...adminHeaders, "content-type": "application/json" },
  body: JSON.stringify({
    ...owner,
    enabled: true,
    requiredActions: [...new Set([...(owner.requiredActions ?? []), "UPDATE_PASSWORD"])],
    attributes: {
      ...(owner.attributes ?? {}),
      "kitsu.recovery.id": [pending.recoveryId],
      "kitsu.recovery.started-at": [pending.createdAt],
      "kitsu.recovery.sessions-revoked": ["false"],
    },
  }),
});
requireSuccess(updated, "arm Kitsu owner recovery");

owner = await findOwner();
await requireNonAdministrativeOwner(owner.id);
await revokeOwnerSessionsAndMark(owner, pending);
owner = await findOwner();
if (
  owner.attributes?.["kitsu.recovery.id"]?.[0] !== pending.recoveryId ||
  owner.attributes?.["kitsu.recovery.sessions-revoked"]?.[0] !== "true" ||
  !(owner.requiredActions ?? []).includes("UPDATE_PASSWORD")
) {
  throw new Error("Kitsu owner recovery did not reach the forced-password-change state");
}

process.stdout.write(
  "Owner recovery armed. Deliver the protected one-time handoff; the previous password is invalid.\n",
);
