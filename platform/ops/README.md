# Private deployment boundary

Kitsu's public repository intentionally excludes machine-specific deployment
inventory: operating-system account names, private addresses, certificate
labels, credential locations, applied tunnel state, backup destinations, and
service-unit overrides.

The public source under `platform/` is deployable through normal environment
and file-based configuration. A real installation must keep its inventory in
a private operations repository or protected configuration store, validate
service health before an atomic release switch, and never commit secrets or
local network identities here.

Public documentation is in `platform/docs-site/`. Product deployment details
must not be added to that user manual.

## Unlock-page browser policy

The hardware verification page is the only public-site route that needs Web
Serial and a cross-origin request to the public API. Install
`k32-unlock-policy-map.conf` in nginx's `http` context. In the `k32.run`
server, replace the fixed `Content-Security-Policy` and `Permissions-Policy`
values with these two directives while leaving the other security headers
unchanged:

```nginx
add_header Content-Security-Policy $kitsu_public_site_csp always;
add_header Permissions-Policy $kitsu_public_site_permissions_policy always;
```

Do not leave both the fixed and mapped directives active: duplicate CSP or
Permissions-Policy response fields produce a different, usually stricter,
effective policy. Validate with `nginx -t`, reload nginx, and confirm that
`/unlock/` has `connect-src ... https://api.k32.run` and `serial=(self)` while
`/` still has no API `connect-src` grant and keeps `serial=()`.
