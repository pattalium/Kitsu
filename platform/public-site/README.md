# k32.run public site

This dependency-free static site distributes the signed native Android app,
links the signed firmware installer and documentation, and points to the public
source repository. The Kitsu app is local-first: it talks to the device through
authenticated Bluetooth and has no product account, gateway, or runtime-server
dependency. iOS is not part of the current release scope.

Android and firmware have independent release gates. The Android download may
be available while the firmware installer safely reports that no physically
accepted firmware release is available.

`config.json` points to the public source repository at
`https://github.com/pattalium/Kitsu`.

## Android release trust

The download card exposes only a signed local-first Android release. A valid
historical manifest or APK may remain in `downloads/` for audit and rollback,
but `site.js` deliberately refuses to link any build older than Android 2.0.0 /
version code 13. Install an eligible release directly from the browser on a
supported Android device.

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
the pinned APK certificate, a valid manifest timestamp, and the minimum
local-first version gate. Repository tests additionally verify the detached
signature and hash the exact APK bytes.
