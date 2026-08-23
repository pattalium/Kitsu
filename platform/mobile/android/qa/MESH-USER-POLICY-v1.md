# Kitsu mesh messaging terms and user policy

Policy version: `2026-08-22.1` (application policy version `1`)

Mesh messaging is for lawful, respectful communication. Users must not send threats, harassment, hate, spam, scams, illegal content, sexual exploitation, or content that exposes another person's private information. A user is responsible for messages they deliberately transmit.

The Android composer is unavailable until the user reviews and accepts this exact policy version. A future policy-version change invalidates the prior acceptance and gates sending again.

The app has no `INTERNET` permission. Messages move through the selected Kitsu over authenticated Bluetooth and its local mesh. Device authorization, cached mesh activity, policy acceptance, and blocked peer identifiers stay on the phone. A moderation report is created only after the user selects **Report message** or **Report sender** and is written only to a user-chosen document. It is not automatically submitted.

Blocking is available only for an inbound direct message with a stable peer identifier and is persistent on the Android app installation. **Block sender** hides that peer's messages and removes the peer from recipient suggestions on that phone. Inbound channel messages without a stable sender identifier can be reported as content but cannot offer sender report/block controls. Because the firmware contract has no remote block-list operation, the selected Kitsu may still receive radio traffic from a blocked peer; the UI states this limitation at the block confirmation and in Settings.
