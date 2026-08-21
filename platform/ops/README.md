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
