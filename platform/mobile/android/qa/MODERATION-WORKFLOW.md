# Kitsu mesh moderation workflow

This runbook is the developer-side workflow for exported `kitsu.mesh-moderation-report.v1` files. The Android app does not claim to submit a report and has no network permission.

## Intake

1. The user selects **Report message** or, for an inbound direct message with a stable peer identifier, **Report sender**, chooses a reason, optionally adds context, and chooses an Android document destination.
2. The app exports one UTF-8 JSON file. The user sends that file to the published Kitsu support contact using a communication method outside the app.
3. Support records the intake timestamp, handler, report SHA-256, and a case identifier in the access-controlled moderation register. Do not duplicate the message body into tickets or chat unless review requires it.

## Validation

Reject or quarantine a file unless all of these hold:

- `schema` is exactly `kitsu.mesh-moderation-report.v1`.
- `policy_version` is a supported positive version and `policy_version_label` matches its published policy.
- `app_id` is `ptl.kitsu.app` (a `.debug` suffix is acceptable only for internal QA evidence).
- reason is one of `spam_or_scam`, `harassment_or_hate`, `illegal_or_exploitative`, `privacy_violation`, or `other`.
- `report_type` is `message` or `sender`; sender reports require a stable direct-message `peer_id`.
- required message identifiers and content fields are present; optional context is at most 512 UTF-8 bytes.

Compute and retain the original file's SHA-256 before review. Never edit the submitted evidence file in place.

## Review and disposition

1. Triage credible imminent-harm or child-safety material immediately under the developer's escalation policy and applicable law.
2. For other reports, review the reported content, stated reason, and available authenticated identity/channel context. A single exported report is evidence, not proof of identity ownership or intent.
3. Record one disposition: `actioned`, `no_violation`, `duplicate`, `insufficient_evidence`, or `referred`.
4. Where a direct peer should be blocked locally, instruct the reporter to use **Block sender** from that inbound message's overflow menu. The current firmware has no remote/global block operation; do not promise a mesh-wide ban. Channel traffic without a stable sender identifier cannot be sender-blocked by this app.
5. If a recurring abuse pattern requires a product response, ship a reviewed policy/UI/filter update through the normal signed Android release process. Do not create an undocumented server or gateway moderation dependency.

## Retention and deletion

Keep the minimum evidence required by the published privacy/retention policy. Restrict access to trained reviewers. When retention ends, delete the report and any derived content copies while retaining only the case identifier, original SHA-256, timestamps, and non-content disposition where legally appropriate.
