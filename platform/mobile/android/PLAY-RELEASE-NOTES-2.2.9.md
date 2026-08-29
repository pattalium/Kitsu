# Kitsu 2.2.9

- Recovers cleanly when Android closes the first post-bond GATT connection, with one bounded status-22 retry.
- Serializes controller credential changes and allows local Forget only after the selected authorization is authoritatively proven absent.
