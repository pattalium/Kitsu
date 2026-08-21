# Kitsu Keycloak login theme

This directory contains the production login theme for the `kitsu` realm. It
keeps account creation and email reset disabled while making the supported
owner bootstrap flow understandable:

- nearby Bluetooth pairing does not need an owner account;
- an owner account enables remote access through a configured Wi-Fi gateway;
- the initial username and temporary password arrive in the private bootstrap
  handoff created during service provisioning;
- the first login replaces the temporary password;
- lost credentials use the operator recovery procedure, not public
  self-registration or an email reset.

The `kitsu/` directory is installed as a complete Keycloak theme. Its checked
app icon must remain byte-identical to `assets/brand/kitsu-app-icon.png`.

## Origin deployment contract

1. Install `kitsu/` at `<KEYCLOAK_HOME>/themes/kitsu` without following
   symlinks. Files are world-readable but never writable by the Keycloak
   service account.
2. Restart Keycloak so its production theme cache cannot retain the previous
   resource set.
3. Run `reconcile-keycloak-theme.mjs` as the Keycloak service account through
   a oneshot unit that provides the encrypted bootstrap-admin credential.
4. Require the script's authorization-page probe to pass before declaring the
   deployment healthy. It verifies the custom stylesheet, labels, app icon,
   explanatory copy, login form, and preserved registration/reset policy.
5. Probe both the native and browser authorization clients, then verify the
   issuer discovery document still reports the exact configured issuer.

The included unit is a reference for step 3. No Cloudflare, tunnel, or DNS
change is part of a theme deployment.

Run the source checks from the repository root:

```sh
node --test platform/auth/keycloak-theme/tests/theme.test.mjs
```
