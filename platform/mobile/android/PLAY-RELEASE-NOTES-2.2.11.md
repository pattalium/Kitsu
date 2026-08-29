# Kitsu 2.2.11

- Adds offline import of signed `.kitsu-fw` packages and authenticated Bluetooth A/B updates for firmware 0.20.3 on the current 8 MiB dual-OTA Heltec layout.
- Verifies the package signature, application image, firmware identity, version, layout, and geometry before any update is staged.
- Distinguishes temporary recent-action capacity from durable storage failure, with safe retry guidance when no action has run.
