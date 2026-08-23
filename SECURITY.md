# Security policy

## Supported releases

Security fixes are targeted at the latest accepted stable Kitsu firmware and
Android release exposed through the signed public release surfaces. Preview,
candidate, development, and historical builds do not have a guaranteed
maintenance window.

## Reporting a vulnerability

Use [GitHub private vulnerability reporting](https://github.com/pattalium/Kitsu/security/advisories/new).
Include the affected component and release, the practical impact, concise
reproduction steps, and a safe way to contact you. Please do not publish an
unfixed issue until a fix is available or a disclosure date has been agreed.

Never include credentials, controller roots, signing keys, private keys,
complete device flash dumps, or third-party radio traffic in a report. Test
only hardware, accounts, and radio systems you own or have explicit permission
to assess.

Kitsu does not promise a bug bounty or a fixed response deadline. The project
will acknowledge and investigate good-faith reports as capacity permits.

## Physical-access boundary

Kitsu is deliberately owner-reflashable. It does not enable Secure Boot or
Flash Encryption, burn security eFuses, disable the ESP32-S3 ROM downloader,
or claim that physical possession is a tamper-resistant boundary. Reports
should distinguish remote or proximity attacks from behavior that requires
unrestricted physical access to the board.
