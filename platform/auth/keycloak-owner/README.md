# Kitsu owner credential recovery

`recover-owner.mjs` is the explicit operator-only recovery path for the
username-only bootstrap owner. It exists because public registration and email
password reset are intentionally disabled.

The tool:

- accepts only the exact `k32-owner` confirmation phrase;
- uses the encrypted local Keycloak bootstrap-admin credential;
- refuses an absent, ambiguous, disabled, or administratively privileged
  owner;
- writes a mode-`0600` one-time handoff before invalidating the old password;
- assigns a strong random temporary password and forces `UPDATE_PASSWORD`;
- asks Keycloak to log out every existing owner session and records the
  successful logout before declaring recovery armed;
- resumes safely with the same handoff after an interrupted run;
- never prints the password or includes it in an error;
- removes a consumed handoff when Keycloak reports that the forced password
  change is complete.

Install the script in the protected service release. Run it only through a
manual oneshot/transient unit as the Keycloak service account, loading the
existing encrypted bootstrap-admin credential and passing a protected state
directory:

```text
recover-owner.mjs ADMIN_CREDENTIAL STATE_DIRECTORY --confirm-reset=k32-owner
```

Give the resulting private handoff to the owner through an authenticated
out-of-band channel. Do not copy it into logs, a ticket, a public web root, or
the source repository. Starting a new recovery invalidates the previous owner
password and terminates its existing server-side sessions.

Run its local integration test from the repository root:

```sh
node --test platform/auth/keycloak-owner/tests/recover-owner.test.mjs
```
