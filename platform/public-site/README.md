# k32.run public site

This dependency-free static site is served by local nginx on the Kitsu host and
published through Cloudflare Tunnel. The authenticated browser companion stays
on `app.k32.run`. Android remains native; iOS is not part of the current release
scope.

`config.json` points to the public source repository at
`https://github.com/pattalium/Kitsu`.

## Android release trust

The APK on the website is the signed Android release. Install it directly from
the browser on a supported Android device.

The Android download has two independent integrity boundaries:

1. Android verifies the APK's dedicated Kitsu release certificate. Its SHA-256
   certificate digest is pinned in both `downloads/latest.json` and `site.js`.
2. The site verifies the exact bytes of `downloads/latest.json` with the pinned
   Ed25519 update public key before it exposes the download link.

The private Android keystore and Ed25519 update key live only in protected
release configuration. They must never be copied into this
directory or committed. `downloads/update-ed25519-public.pem` is public by
design. `downloads/latest.json.sig` is a raw 64-byte Ed25519 signature over the
exact `downloads/latest.json` bytes.

The browser accepts only a same-origin versioned APK path, the stable/release
package `app.kitsu.mobile`, bounded numeric fields, canonical lowercase SHA-256,
the pinned APK certificate, and a valid manifest timestamp. Tests also verify
the detached signature and hash the exact APK bytes.
