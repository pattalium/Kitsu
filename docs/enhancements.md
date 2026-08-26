# Kitsu enhancements

This document records the approved product direction and the constraints that
future encounter work must preserve. Firmware 0.17.1 and Android 2.2.1 deliver
the first complete production slice: MeshCore-triggered wild encounters, direct
nearby Kitsu presence and Pet actions, a 21-creature catalog across seven rarity
tiers, the device code journal, the encrypted Android code wallet, the website
unlock flow, and privately delivered downloadable creature packs. Broader
nearby actions remain planned work.

## Signal encounters and nearby Kitsu

Kitsu has two distinct radio-facing systems. They must remain separate:

1. **MeshCore activity** can trigger local creature-encounter rolls. Kitsu
   messages, adverts, peer discovery, and repeater discovery remain ordinary
   MeshCore operations.
2. **Nearby Kitsu interaction** extends the existing transport-independent
   Kitsu encounter protocol onto a dedicated nearby radio path. It is not a
   MeshCore message, must not be forwarded by MeshCore repeaters, and must not
   add Kitsu chatter to public meshes.

The two systems may both influence companion progression, but they do not
share an over-the-air packet format or transport path.

Firmware 0.17.1 keeps these paths separate in the live implementation: Listen
uses bounded direct Kitsu presence frames, while successful logical MeshCore
operations feed only the wild-encounter coordinator.

### MeshCore-triggered creature encounters

Every successfully completed logical MeshCore operation is eligible to
trigger a local creature encounter. Eligible operations include:

- receiving a valid direct or channel message;
- completing an actual outbound message transmission;
- completing an owner-requested advert;
- discovering a new MeshCore client, room, or sensor; and
- discovering a valid repeater.

A single logical operation gets at most one roll. Packet fragments, retries,
repeat observations, delivery-state changes, and duplicate adverts must not
create additional rolls for the same operation.

Discovering a valid MeshCore repeater is a **guaranteed creature encounter**.
Other MeshCore operations use configurable encounter probabilities. A
repeater guarantee must be based on the verified MeshCore advert role, not a
device-name heuristic.

MeshCore carries no Kitsu pet profile, pet action, portrait, animation, pack,
or unlock code. MeshCore activity is only the event source for the local
creature-encounter system.

### Listen means nearby Kitsu discovery

The owner-facing **Listen** action is for finding nearby Kitsu pets through
the dedicated Kitsu radio protocol. It is not the name for watching MeshCore
traffic.

When Listen receives a valid new Kitsu encounter:

- the neighbor's actual installed pet is shown every time;
- the neighbor pet is distinguished from a generic MeshCore identity;
- meeting that new owned pet also gets a lower-probability, separate creature
  encounter roll;
- an already-known pet produces a shorter familiar-return interaction; and
- no Kitsu packet is sent through, forwarded by, or acknowledged by MeshCore
  repeaters.

The existing Kitsu encounter fields for pack ID, appearance, evolution stage,
bond, mood, emote, peer identity, and nonce are the starting point for this
work. The nearby protocol must remain bounded, deduplicated, and rate-limited.
Its current CRC detects damage but is not identity authentication, so neighbor
actions remain harmless and capped and never issue rarity results or codes.

### Interacting with a neighbor's owned pet

Meeting another owner's Kitsu is not a wild-creature encounter. It is a
direct pet-to-pet interaction with the actual companion installed on the
neighboring Heltec.

The nearby interaction flow must support, at minimum, petting the neighbor's
pet. Additional positive actions may be added through a reviewed allowlist,
such as greeting or playing. Remote actions must:

- use only the dedicated nearby Kitsu protocol;
- require a fresh nearby encounter/session rather than an arbitrary stored ID;
- identify the target pet by its Kitsu peer identity and installed pack ID;
- carry a nonce or equivalent duplicate token;
- be acknowledged or rejected by the target Kitsu;
- be rate-limited per neighbor and action so they cannot be farmed or spammed;
- never permit destructive, negative, configuration, firmware, pack, or
  controller operations; and
- be disableable by the receiving owner.

On the receiving Heltec, the installed companion plays its real action
animation and applies the approved bounded effect. On the visiting Heltec,
the neighbor is represented by its static catalog portrait unless that same
pack is locally installed. The visitor never receives the neighbor's pack
bytes.

### Creature presentation

Kitsu's OLED is monochrome. Creature identity and rarity must not depend on
color or recoloring.

- Fox, Cat, and Dog remain the existing base companions.
- They must not be renamed, recolored, or multiplied into superficial
  variants for the encounter system. A genuinely evolved form is allowed
  when it has its own silhouette, portrait, animations, personality, and
  progression meaning rather than being a palette swap.
- New encounter rewards must be genuinely new creatures with distinct names,
  silhouettes, portraits, and animation packs.
- Kitsu remains a Tamagotchi-style pet product first. Common through
  Legendary creatures are literal companion animals, such as a normal frog,
  rather than humanoid characters wearing an animal theme.
- Mythical is the sole exception: it is reserved for highly evolved,
  anthropomorphic companion forms such as Cat Girl. These forms
  remain Kitsu companions and must retain the care, needs, bond, memory, game,
  sleep, and reaction systems rather than becoming unrelated character art.
- Private, owner-only companions are excluded from public encounter tables,
  rarity rolls, unlock codes, website catalogues, public downloads, Android
  catalogues, marketing, and the public emulator. They must never be offered
  as unlockable public packs. A nearby meeting may identify one only through
  whatever public presentation its owner explicitly permits; that does not
  grant or expose the pack.
- An encountered but uninstalled creature is represented by one static,
  monochrome portrait.
- A creature becomes fully animated only when its actual pack is installed on
  that Heltec.
- Meeting another owned Kitsu shows that pet's real static portrait and public
  presentation state, not a generic MeshCore icon or fabricated variant.

Every downloadable companion pack must also preserve one coherent visual
identity from source art through the installed animation:

- Start with one approved canonical identity image for the creature. Its
  species, silhouette, face, proportions, markings, and body structure are the
  reference for every later action.
- Generate each of the twelve actions as its own separate four-frame source
  asset: Idle, Blink, Pet, Sleep, Listen, Surprise, Play, Tired, Feed, Wake,
  Meet, and Evolve. Do not generate a complete contact sheet as one artwork and
  then treat unrelated cells as an animation pack.
- All four frames of an action must show the same creature and coherent motion.
  Actions may change pose, compression, or orientation, but must not change the
  creature's anatomy, style, markings, or identity.
- Each four-frame action must read as one short motion when played in order,
  not as four unrelated poses. Keep the creature on a stable floor and at a
  stable apparent scale unless the named action itself requires a small,
  continuous displacement.
- Use these action meanings consistently: Idle is breathing or a small
  species-specific twitch; Blink primarily changes the eyes; Pet is a pleased
  reaction to an unseen touch; Sleep lowers or curls into rest; Listen orients
  the creature's real ears, head, gills, or body; Surprise is a brief recoil;
  Play is species-appropriate locomotion; Tired visibly sags; Feed lowers,
  nibbles, or chews; Wake rises continuously from rest; Meet approaches,
  investigates, and settles; Evolve is a confident posture change. Do not use
  detached punctuation, floating food, effect marks, scenery, or a second
  character to make an otherwise unreadable action understandable.
- Meet animations for animal companions use species-appropriate behavior such
  as approaching, sniffing, attentive posture, ear or tail movement, a crouch,
  or a small body bounce. Animal companions do not wave like humans and do not
  grow human hands for an animation.
- Mythical companions use a deliberate monochrome anime-chibi kemonomimi
  aesthetic rather than the generic upright mascot style used in the rejected
  first pass. Their expressive face, hair silhouette, species ears, tail,
  antlers or other defining anatomy, body proportions, and one modest outfit
  remain identical across every action. They stay compact and legible on the
  64-by-64 Tamagotchi display: no realistic adult anatomy, sexualized pose or
  outfit, costume changes, oversized props, or style changes between actions.
  Their Meet animation uses a shy step, attentive lean, small bow, and
  ear-or-tail response with arms down; it is not a hand wave.
- Source actions must contain only the single companion: no second creature,
  text, scenery, detached decorative marks, gradients, anti-aliasing, or mixed
  visual styles.
- The build step mechanically rasterizes every frame to the exact 64-by-64,
  one-bit OLED contract and enforces safe bounds, a stable floor, coherent body
  axis, meaningful frame differences, and the existing role timing contract.
- Mechanical geometry, frame-count, hash, and similarity checks are necessary
  but are never visual acceptance. Before a pack becomes downloadable, every
  rasterized action must be reviewed at actual OLED scale and as an animation
  for species identity, action meaning, coherent frame order, silhouette,
  floor, clipping, and prohibited marks. A failed action rejects the pack until
  that action is regenerated and reviewed again.
- Canonical source art, action sources, contact sheets, serialized frames, and
  full pack bytes remain private release inputs. The public catalog may expose
  only the approved static 16-by-18 portrait and non-sensitive metadata before
  redemption.

Initial encounter copy should remain short and literal:

```text
SOMETHING
APPROACHES
```

The reveal then uses the creature's real approved name and rarity. Neighbor
meetings instead identify the actual nearby Kitsu pet. Placeholder species
names must not be exposed in firmware, Android, the website, or marketing.

### Rarity and code resolution

Each creature has an ordered rarity tier. The order is Common, Uncommon, Rare,
Very Rare, Epic, Legendary, and Mythical. Mythical supersedes Legendary and
must have a total encounter probability below one percent. The exact value and
the remaining tier weights are data-driven balancing decisions. Rarity must be
communicated with text, symbols, or monochrome framing rather than color.

Mythical rarity describes the highly evolved anthropomorphic forms. Every
lower rarity remains a literal pet creature. Rarity and evolution are related
product concepts but must remain explicit fields so an ordinary animal is not
silently presented as an anthropomorphic palette variant.

Encountering a creature and receiving its unlock code are separate outcomes:

1. The encounter selects and records a creature according to the applicable
   encounter table.
2. The device shows the creature's static portrait and rarity.
3. A resolution roll determines whether that encounter reveals the
   corresponding code.
4. A successful code inherits the creature's rarity and is stored before the
   reveal, so reset or power loss cannot reroll it.

Codes create a permanent unlock associated with the Kitsu hardware that earned
them. The same hardware may download the unlocked pack again; the code may not
be rebound to another device or pack. The firmware must retain pending codes
until Android has confirmed that it saved them.

### Android code wallet

The Android app must save codes received from an authenticated Kitsu. Each
record is associated with the saved Kitsu authorization and includes the
hardware identity, creature/pack ID, rarity, discovery source, creation time,
and redemption/install state.

Android must provide a local code list, allow the owner to copy a code, and
open the website's redemption page in the external browser. The app does not
need to download packs or gain an Internet permission. Receiving the same
device code again must update the existing record rather than create
duplicates. Codes belong in a separate Keystore-encrypted, multi-device vault,
not the bounded selected-device cache that is cleared during ordinary device
switching or Forget cleanup.

### Website unlock and ordinary `.k868` downloads

The website owns the unlock catalogue and downloadable pet packs:

1. The owner enters a saved code or arrives from Android with the code filled
   in.
2. The page validates the code with the connected Kitsu, then sends the bounded
   code/device proof to the K32 redemption API.
3. On first redemption, a valid code permanently binds the corresponding pack
   to that hardware record; the same hardware may download it again later.
4. The API returns the ordinary downloadable `.k868` file as a private,
   no-store response. There is no stable public pack URL.
5. The Web Serial flasher can install an unlocked pack by itself or alongside
   the core firmware in one session.

The `.k868` file format does **not** change for this enhancement. Firmware
continues to validate its existing structure and CRC. There is no encrypted,
signed, or device-bound `.k868` variant in this plan. Hardware binding applies
to website code redemption and download authorization, not to the bytes after
download; an ordinary downloaded `.k868` file remains copyable. The product
must state that boundary honestly.

The public site may expose approved static portraits and pack metadata, but an
unlocked full pack is delivered only through the website's code-gated download
flow. No full pack is transmitted between Kitsu devices over radio.

### Persistence and anti-duplication

The firmware must durably record:

- encountered creature IDs and rarity;
- neighbor Kitsu identities and their last observed public pet state;
- operation IDs already used for encounter rolls;
- pending and acknowledged codes;
- per-neighbor action cooldowns; and
- enough monotonic state to prevent reset-based rerolls and packet replay.

This state must remain bounded and preserve the existing owner-reflashable
security model. It must not block the BLE loop while radio events, encounter
scenes, or neighbor actions are processed.

### Emulator and acceptance

The public demo must exercise the production firmware implementation with
injected radio data:

- a verified repeater discovery demonstrates the guaranteed creature path;
- successful messages and adverts demonstrate probabilistic trigger paths;
- a dedicated Kitsu packet demonstrates the actual neighbor pet and a remote
  Pet action without entering MeshCore;
- the creature remains a static portrait before installation;
- installing its pack enables the real animations; and
- any displayed demo code is clearly non-redeemable.

Release acceptance must prove that Kitsu-nearby packets are not passed to the
MeshCore dispatcher or forwarding path, that each logical MeshCore operation
gets no more than one roll, that repeater discovery guarantees an encounter,
that remote pet actions are bounded and deduplicated, and that Android and the
website preserve the same code and hardware association.
