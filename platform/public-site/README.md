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
but `site.js` exposes only the exact Android 2.2.7 / version-code 28 production
contract. Install the accepted release directly from the browser on a supported
Android device.

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

The browser accepts only a same-origin content-addressed APK path, the
stable/release package `ptl.kitsu.app`, exact version 2.2.7 / code 28, bounded
numeric fields, canonical lowercase SHA-256, the pinned direct-download APK
certificate, and a valid manifest timestamp. Repository tests additionally
verify the detached signature and hash the exact APK bytes.

The legacy `app.kitsu.mobile` package and the direct-download/Play signing
tracks do not cross-update. Before switching, connect to the Kitsu and use
Forget authorization, uninstall the old app, install the chosen track, and pair
again. The signed Play app is not distributed from this static site.

When the fixed `latest.json` authority advances, exact signed historical
manifests remain available under immutable names. The 2.0.0 authority is
`downloads/android-stable-2.0.0-20260822t123928z.json`; the 2.2.0 authority is
`downloads/android-stable-2.2.0-20260825t170821z.json`; the 2.2.1 authority is
`downloads/android-stable-2.2.1-20260826t002057z.json`; the 2.2.3 authority is
`downloads/android-stable-2.2.3-20260826t132716z.json`; the 2.2.4 authority is
`downloads/android-stable-2.2.4-20260828t092529z.json`; the 2.2.5 authority is
`downloads/android-stable-2.2.5-20260828t143256z.json`; and the immediately
prior 2.2.6 authority is
`downloads/android-stable-2.2.6-20260829t015805z.json`. Each retains its raw
detached signature. Historical APK and testing-preview bytes remain available
for audit and rollback but are not advertised as the current download.

The support card is a plain outbound HTTPS link to `https://ko-fi.com/pattalium`.
Support is voluntary and grants no app feature, content, badge, or other
benefit. The site does not embed payment code, collect payment details, or add
tracking parameters to that destination.
