import assert from "node:assert/strict";
import { mkdtemp, readFile, stat, writeFile } from "node:fs/promises";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import path from "node:path";
import { spawn } from "node:child_process";
import test from "node:test";

const script = path.resolve(import.meta.dirname, "../recover-owner.mjs");

function run(args, environment) {
  return new Promise((resolve, reject) => {
    const child = spawn(process.execPath, [script, ...args], {
      env: { ...process.env, ...environment },
      stdio: ["ignore", "pipe", "pipe"],
    });
    const stdout = [];
    const stderr = [];
    child.stdout.on("data", (chunk) => stdout.push(chunk));
    child.stderr.on("data", (chunk) => stderr.push(chunk));
    child.on("error", reject);
    child.on("close", (code) => resolve({
      code,
      stdout: Buffer.concat(stdout).toString("utf8"),
      stderr: Buffer.concat(stderr).toString("utf8"),
    }));
  });
}

test("recovery writes a private resumable handoff before forcing password change", async (t) => {
  const temporaryRoot = await mkdtemp(path.join(tmpdir(), "kitsu-owner-recovery-"));
  const credentialPath = path.join(temporaryRoot, "admin-password");
  const stateDirectory = path.join(temporaryRoot, "state");
  const pendingPath = path.join(stateDirectory, "owner-recovery.pending.json");
  await writeFile(credentialPath, `${"A".repeat(40)}\n`, { mode: 0o600 });

  let owner = {
    id: "owner-id",
    username: "k32-owner",
    enabled: true,
    requiredActions: [],
    attributes: { "kitsu.bootstrap.state": ["ready"] },
  };
  let resetCount = 0;
  let logoutCount = 0;
  let resetPassword;
  let handoffExistedAtReset = false;
  const server = createServer(async (request, response) => {
    const chunks = [];
    for await (const chunk of request) chunks.push(chunk);
    const body = Buffer.concat(chunks).toString("utf8");
    if (request.url === "/realms/master/protocol/openid-connect/token") {
      response.setHeader("content-type", "application/json");
      response.end(JSON.stringify({ access_token: "T".repeat(64) }));
    } else if (request.url?.startsWith("/admin/realms/kitsu/users?")) {
      response.setHeader("content-type", "application/json");
      response.end(JSON.stringify([owner]));
    } else if (request.url === "/admin/realms/kitsu/users/owner-id/role-mappings") {
      response.setHeader("content-type", "application/json");
      response.end(JSON.stringify({ realmMappings: [], clientMappings: {} }));
    } else if (
      request.url === "/admin/realms/kitsu/users/owner-id/reset-password" &&
      request.method === "PUT"
    ) {
      handoffExistedAtReset = await readFile(pendingPath, "utf8").then(() => true, () => false);
      const payload = JSON.parse(body);
      resetPassword = payload.value;
      resetCount += 1;
      response.statusCode = 204;
      response.end();
    } else if (
      request.url === "/admin/realms/kitsu/users/owner-id" &&
      request.method === "PUT"
    ) {
      owner = JSON.parse(body);
      response.statusCode = 204;
      response.end();
    } else if (
      request.url === "/admin/realms/kitsu/users/owner-id/logout" &&
      request.method === "POST"
    ) {
      logoutCount += 1;
      response.statusCode = 204;
      response.end();
    } else {
      response.statusCode = 404;
      response.end();
    }
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  t.after(() => new Promise((resolve) => server.close(resolve)));
  const { port } = server.address();
  const environment = { KITSU_KEYCLOAK_LOCAL_PORT: String(port) };
  const args = [credentialPath, stateDirectory, "--confirm-reset=k32-owner"];

  const first = await run(args, environment);
  assert.equal(first.code, 0, first.stderr);
  assert.match(first.stdout, /previous password is invalid/u);
  assert.equal(first.stdout.includes(resetPassword), false);
  const handoff = JSON.parse(await readFile(pendingPath, "utf8"));
  assert.equal(handoff.temporaryPassword, resetPassword);
  assert.equal(handoffExistedAtReset, true);
  assert.equal(resetCount, 1);
  assert.equal(logoutCount, 1);
  assert.equal(owner.requiredActions.includes("UPDATE_PASSWORD"), true);
  assert.equal(owner.attributes["kitsu.recovery.id"][0], handoff.recoveryId);
  assert.equal(owner.attributes["kitsu.recovery.sessions-revoked"][0], "true");
  if (process.platform !== "win32") {
    assert.equal((await stat(pendingPath)).mode & 0o777, 0o600);
  }

  const resumed = await run(args, environment);
  assert.equal(resumed.code, 0, resumed.stderr);
  assert.match(resumed.stdout, /already armed/u);
  assert.equal(resetCount, 1);
  assert.equal(logoutCount, 1);

  owner = {
    ...owner,
    attributes: { ...owner.attributes, "kitsu.recovery.sessions-revoked": ["false"] },
  };
  const interruptedLogout = await run(args, environment);
  assert.equal(interruptedLogout.code, 0, interruptedLogout.stderr);
  assert.match(interruptedLogout.stdout, /already armed/u);
  assert.equal(resetCount, 1);
  assert.equal(logoutCount, 2);
  assert.equal(owner.attributes["kitsu.recovery.sessions-revoked"][0], "true");

  owner = { ...owner, requiredActions: [] };
  const consumed = await run(args, environment);
  assert.equal(consumed.code, 0, consumed.stderr);
  assert.match(consumed.stdout, /consumed one-time handoff was removed/u);
  await assert.rejects(readFile(pendingPath), { code: "ENOENT" });
  assert.equal(resetCount, 1);
  assert.equal(logoutCount, 2);
});

test("recovery requires the exact explicit owner confirmation", async () => {
  const result = await run([], {});
  assert.notEqual(result.code, 0);
  assert.match(result.stderr, /--confirm-reset=k32-owner/u);
});

test("recovery refuses absent, ambiguous, disabled, and administrative owners", async (t) => {
  const cases = [
    { name: "absent", owners: [], mappings: {}, error: /missing or ambiguous/u },
    {
      name: "ambiguous",
      owners: [
        { id: "one", username: "k32-owner", enabled: true },
        { id: "two", username: "k32-owner", enabled: true },
      ],
      mappings: {},
      error: /missing or ambiguous/u,
    },
    {
      name: "disabled",
      owners: [{ id: "owner-id", username: "k32-owner", enabled: false }],
      mappings: {},
      error: /disabled/u,
    },
    {
      name: "administrative",
      owners: [{ id: "owner-id", username: "k32-owner", enabled: true }],
      mappings: { realmMappings: [{ name: "realm-admin" }], clientMappings: {} },
      error: /administratively privileged/u,
    },
  ];

  for (const scenario of cases) {
    await t.test(scenario.name, async (subtest) => {
      const temporaryRoot = await mkdtemp(path.join(tmpdir(), `kitsu-owner-${scenario.name}-`));
      const credentialPath = path.join(temporaryRoot, "admin-password");
      const stateDirectory = path.join(temporaryRoot, "state");
      await writeFile(credentialPath, `${"A".repeat(40)}\n`, { mode: 0o600 });
      let mutationCount = 0;
      const server = createServer(async (request, response) => {
        for await (const _chunk of request) {
          // Drain the request before responding.
        }
        response.setHeader("content-type", "application/json");
        if (request.url === "/realms/master/protocol/openid-connect/token") {
          response.end(JSON.stringify({ access_token: "T".repeat(64) }));
        } else if (request.url?.startsWith("/admin/realms/kitsu/users?")) {
          response.end(JSON.stringify(scenario.owners));
        } else if (request.url?.endsWith("/role-mappings")) {
          response.end(JSON.stringify({
            realmMappings: scenario.mappings.realmMappings ?? [],
            clientMappings: scenario.mappings.clientMappings ?? {},
          }));
        } else {
          mutationCount += 1;
          response.statusCode = 204;
          response.end();
        }
      });
      await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
      subtest.after(() => new Promise((resolve) => server.close(resolve)));
      const { port } = server.address();
      const result = await run(
        [credentialPath, stateDirectory, "--confirm-reset=k32-owner"],
        { KITSU_KEYCLOAK_LOCAL_PORT: String(port) },
      );
      assert.notEqual(result.code, 0);
      assert.match(result.stderr, scenario.error);
      assert.equal(mutationCount, 0);
    });
  }
});
