# Google Play store listing — en-US

This file freezes the public listing metadata used for `ptl.kitsu.app`. It does
not record release-track state, tester addresses, private bundle paths, or Play
credentials.

## Organization and contact

- Application type: App
- Category: Tools
- Public support email: `hello@rgg.me`
- Public website: `https://k32.run`
- Privacy policy: `https://k32.run/privacy/`
- Ads: No
- Data safety: No user data collected; no user data shared with companies or
  organizations

## Default listing

### App name

Kitsu Companion App

### Short description

Care for your Kitsu, meet nearby companions, and use MeshCore over Bluetooth.

### Full description

Your Kitsu, directly connected.

Kitsu Companion App pairs with a nearby K32 Kitsu device over authenticated
Bluetooth. No account, cloud service, analytics, advertising, or internet
permission is required.

Use the app to:

- See energy, curiosity, affection, bond progress, memories, and device status
- Pet, feed, and play with your companion
- Listen for nearby Kitsu companions through Kitsu's separate direct-radio
  protocol
- See a neighbor's actual installed creature and send a bounded Pet interaction
- Discover wild creatures from verified radio activity and keep earned unlock
  codes encrypted on your phone
- Configure the local MeshCore radio
- Send direct messages and use configured channels
- Review message history and delivery state
- Install compatible Kitsu firmware updates
- Repair Bluetooth pairing while reusing an existing controller authorization
- Forget a paired controller and remove its local credentials

Nearby Kitsu meetings and MeshCore remain separate: companion-presence traffic
is not sent through MeshCore repeaters. User messages leave the phone only when
you choose to transmit them over the configured radio.

Your companion state remains on Kitsu. The app keeps a bounded encrypted local
cache, an encrypted encounter-code vault, and Android Keystore-backed controller
credentials on your phone.

Requires compatible K32 Kitsu hardware running supported firmware. Bluetooth
and radio availability depend on the device, regional radio configuration, and
surroundings.

Learn more, read the manual, or try the interactive firmware demo at
https://k32.run/

## Required visual assets

- 512×512 fully opaque Play icon
- 1024×500 RGB feature graphic
- Four genuine 1080×1920 RGB phone screenshots

The upload artifacts and their hashes belong in the private protected-release
stage. They must not be copied into the public source repository merely to fill
the Play listing.
