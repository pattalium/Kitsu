# Companion raster contract

This contract governs new private wild-companion artwork. Cat, Dog, and Fox
remain byte-exact legacy format-v1 packs. They are style references only and
must never enter the wild builder, be regenerated, be padded to the new canvas,
or be modified to make them resemble new companions.

## Native format-v2 display geometry

The Heltec OLED is physically 128x64 pixels and Kitsu exposes it as a rotated
64x128 logical framebuffer. A format-v2 companion frame is exactly **64x80**
pixels and is rendered **1:1**. There is no renderer scaling.

- frame cell: `64x80`, 1 bit per pixel;
- frame bytes: `640`;
- Pet and Sleep render origin: `(0, 16)`;
- Listen render origin: `(0, 17)`;
- subject safe stage inside the frame: `[2, 2, 61, 77]`, inclusive;
- identity body axis target: `x=32`;
- subject floor: `y=77` inside the frame;
- rows `78..79`: required blank guard rows.

At the Pet/Sleep origin, the subject floor lands on screen row 93. Rows 94..98
remain clear before the next label at row 99. Listen lands the floor on row 94
and starts its counter at row 101. Ink outside the safe stage is a build error,
not something the builder may crop, translate, clamp, erase, or shrink.

The usable vertical subject stage is 76 pixels (`y=2..77`) instead of the
legacy 60 pixels (`y=2..61`). The approved final frame already contains every
one of those native pixels; the extra area is not blank padding around a legacy
64x64 raster.

Forty-eight frames occupy 30,720 bytes. Including the 64-byte header, twelve
clip records, and forty-eight step records, a normal pack is 31,120 bytes. This
is well below the 1,310,720-byte companion partition, so storage is not a reason
to reduce art quality.

## Exact direct-at-target source layout

Release artwork is forty-eight independent native frames, one identity, and
one independently authored portrait. There are no multi-action release sheets
and no 2x2 sheets in the serialized pack. The preferred direct-target source
tree stores each release frame as its own PNG:

```text
<private-source>/ferret/
  identity.png                 # exact 64x80, mode 1
  portrait.png                 # exact 16x18, mode 1
  idle/00.png                  # exact 64x80, mode 1
  idle/01.png
  idle/02.png
  idle/03.png
  blink/00.png
  blink/01.png
  blink/02.png
  blink/03.png
  ...
  evolve/00.png
  evolve/01.png
  evolve/02.png
  evolve/03.png
```

The twelve required action directories, in pack order, are `idle`, `blink`,
`pet`, `sleep`, `listen`, `surprise`, `play`, `tired`, `feed`, `wake`, `meet`,
and `evolve`. Every directory contains exactly `00.png` through `03.png` and
nothing else. This prevents a missing quadrant, duplicated cell, accidental
crop, or unrelated frame from being hidden inside a sheet.

The preferred direct-target path requires every canonical PNG to already be
Pillow mode `1` at the exact dimensions. That path does not threshold
grayscale, flatten alpha, quantize a palette, resize, crop, pad, translate,
clean, sharpen, or delete components. Its fixed production raster scale is
exactly `1.0` for every frame of every action.

The separately locked ImageGen import path uses one independent full-canvas
RGB/RGBA file per phase. It also requires a separately frozen JSON
preauthorization for every role and phase:

```text
<private-source>/ferret/
  identity.png                 # exact locked 1122x1402 RGB/RGBA identity
  portrait.png                 # exact 16x18 mode-1 portrait
  idle/00.png ... 03.png       # independent full-canvas ImageGen sources
  ...
  evolve/00.png ... 03.png
  preauthorization/
    idle/00.json ... 03.json   # frozen before each generation call
    ...
    evolve/00.json ... 03.json
```

Each preauthorization uses
`kitsu-wild-generated-phase-preauthorization-v2` and pins the frozen
storyboard SHA-256, edit-target kind, and exact native 64x80 allowed-change
mask. The file must declare `frozen_before_generation=true` and
`mask_authoring_basis=frozen-storyboard-native-region-before-generation`.
Unexpected fields are forbidden so a generated candidate cannot be used to
derive, expand, or explain its own permission mask.

There is one fail-closed migration exception for a role phase 0 that was
already generated under a hash-pinned pre-call freeze which declared an exact
inclusive native rectangle but did not serialize the 640-byte mask. Its v4
wrapper uses
`mask_authoring_basis=hash-pinned-pre-generation-native-rectangle-migration`
and an exact `rectangle_migration` record with:

- `freeze_record_relative_path`, which must be
  `preauthorization/_frozen-source/<freeze SHA-256>.json`;
- the same `freeze_record_sha256`, the frozen record's schema, role `phase=0`,
  and the fixed source field
  `roles[].preauthorized_role_pose_region`.

A hash-pinned addendum that listed additional P0 roles before their calls may
chain to that original rectangle freeze. Its migration record additionally
pins `base_freeze_record_relative_path`, `base_freeze_record_sha256`, and
`base_freeze_record_schema`, and uses the fixed field
`base_freeze.roles[].preauthorized_role_pose_region`. The addendum must name
the same base hash, identity, storyboard, identity edit target, exact bounded
registration policy, and selected role with zero raw sources at freeze time.
The base record must authenticate one identical safe rectangle across all of
its listed roles. Both original JSON files are content-addressed, copied, and
hash-listed; neither an unpinned inheritance claim nor a role-specific
rectangle invented after generation is accepted.

The loader authenticates those original bytes, requires their identity-source
and storyboard hashes to match the v4 lock, requires
`status=frozen-before-p0-generation`, identity as the edit target, and
`generated_source_count_at_freeze=0`, and then mechanically materializes the
declared inclusive rectangle. The result must equal both the wrapper's exact
native mask and the semantic lock byte-for-byte. The rectangle must stay in
the 64x80 safe stage and keep rows 78..79 forbidden. This exception is not
available to identity-anchored roles or phases 1..3; those phases still require
their ordinary exact masks frozen before their own calls. Every referenced
legacy freeze is copied into the source snapshot and hash-listed, while
missing, drifted, extra, or orphan freeze records fail the build. A legacy
freeze that already embeds an exact native mask uses the ordinary path, not
this rectangle-only migration.

ImageGen import v4 explicitly rejects one-action sheets and all other
multi-phase generated assets. A sheet cannot prove that phases 1, 2, and 3
were independently edited from the same immutable phase-0 action base; a
visually coherent sheet is still invalid provenance. The old sheet parsing
helpers remain diagnostic/legacy utilities only and have no v4 builder path.

## Image-generation workflow

ImageGen is used for one identity or one phase at a time. Cat, Dog, and Fox may
be supplied only as immutable style and line-economy references; a real animal
reference should supply anatomy. Every generated phase receives two explicit
references: Image 1 is the immutable approved species identity/anatomy source,
and Image 2 is the immutable edit target. For every role, phase 0 targets the
identity. After phase 0 is accepted and hash-frozen, phases 1, 2, and 3 each
independently target that same accepted phase 0, including Idle, Blink, and
Listen. Phase 1 is never an input to phase 2, and no phase may target phase 2
or phase 3.

A phase 0 that is intentionally the unchanged identity needs no ImageGen call,
including identity-anchored Idle, Blink, or Listen. It is represented
explicitly as
`generated_asset.layout=immutable-identity-baseline-copy`: the role's `00.png`
must be byte-identical to `identity.png`, its imported, registered, and final
64x80 hashes must equal the identity frame hash, and its one role registration
must be exactly zero. The ordinary exact P0 preauthorization and bounded
composition records remain required. Only role P0 can use this provenance;
phases 1..3 cannot use it. Later role phases still receive Image 1 as the
immutable identity and Image 2 as `<role>/00.png`, the byte-identical immutable
P0 star base. This avoids asking ImageGen to redraw a base pose that is meant to
remain unchanged while preserving the same two-reference lineage.

For that exact-copy layout only, identity-to-role-P0 landmark evidence compares
the identity landmark region against the same coordinates in P0. The verifier
may take this path only after proving that the source hash, imported frame,
registered frame, final frame, and zero registration are all exactly the
approved identity. A generated or altered P0 continues to compare its distinct
identity and role-pose regions and receives no such exemption. This prevents a
pre-call hypothetical pose-region mapping from falsely rejecting a byte-exact
identity copy without weakening topology checks for generated art.

Direct 64x80 art remains preferred. A larger ImageGen result may enter the
separate importer only through one quality-preserving transform approved from
the identity and reused byte-for-byte for all forty-eight frames:

1. authenticate the phase's pre-generation storyboard and native allowed-
   change mask before importing its generated candidate;
2. require the exact locked source canvas for identity and every independent
   full-canvas phase;
3. composite RGB/RGBA over opaque white;
4. use an exact centered 4:5 viewport that removes at most a two-pixel proven-
   white source border—never a subject bounding-box crop;
5. downsample black coverage with Pillow `BOX` area sampling to 64x80;
6. apply the one identity-stage black-coverage threshold and identity transform;
7. for identity-anchored roles, retain exact zero role registration; for a
   role-base action, apply its one hash-pinned role-level output offset to all
   four candidates. `dy` is exactly `77 - unregistered_P0_floor`; `dx` is
   explicitly approved and root-alignment checked; both are bounded to
   `[-4, 4]`, and no phase can override either value;
8. choose the immutable composition baseline: identity for every role phase 0,
   and the accepted composited role P0 for every phase 1, 2, and 3;
9. build the release frame exactly as `(baseline - allowed_mask) |
   (registered_candidate & allowed_mask)`;
10. prove byte-exact baseline pixels outside the mask and candidate pixels
    inside the mask, then validate the complete composited frame globally.

The source crop, identity offset, and coverage threshold are included in the
canonical transform JSON SHA-256. Role registration is a separate hash-pinned
record because only role P0 may justify it, and that one value is reused for
P0/P1/P2/P3. If the fixed transform or role registration clips a candidate,
reject it; the importer has no auto-fit fallback. BOX coverage is used instead
of source-threshold plus nearest-neighbor reduction so a clean one-logical-
pixel contour survives without nearest-sample phase loss.

Generated candidates are provenance inputs, not release frames. Exact canvas,
source-edge safety, and registration clipping are checked globally. Mid-tone
and coverage-threshold ambiguity is checked only inside the inverse-registered
preauthorized output region, because candidate pixels outside that region
cannot enter the release result. Those discarded pixels remain represented in
the raw-source and imported/registered candidate hashes. The deterministic
composite then receives all safe-stage, guard-row, debris, scale, identity,
topology, motion, and continuity checks globally. This is bounded import, not
post-generation cleanup or manual paint.

Generation and review rules:

- preserve the same species anatomy, face, markings, silhouette, proportions,
  line weight, camera scale, and stage position in all actions;
- use one fixed identity scale across all forty-eight frames;
- keep ears, horns, gills, wings, feet, and tails inside the safe stage with
  real whitespace, including in surprise, evolve, play, and wake;
- make all four phases distinct and readable as one continuous action;
- make `meet` a species-appropriate creature greeting such as a bow, sniff,
  chirp, nuzzle, ear perk, tail gesture, or paw placement—not a human hand wave;
- do not add visible hands, labels, borders, grids, unrelated props, scenery,
  motion-symbol debris, or a second subject;
- mythical Cat Girl, Rabbit Girl, and Deer Girl are anime-anthro Tamagotchi
  characters with animal structure first, not ordinary human girls wearing
  accessories;
- inspect the exact canonical pixels at both 1x device size and nearest-neighbor
  8x review size. A high-resolution concept preview is not acceptance evidence.

## Format-v2 identity lock

Approve an identity only after inspecting its exact 64x80 canonical PNG. The
direct-target lock schema cannot be reused from the legacy source-scaling
pipeline because direct format v2 has no `source_canvas` or
`target_long_axis_pixels` transform.

```json
{
  "schema": "kitsu-wild-identity-lock-v2",
  "identities": [
    {
      "approved": true,
      "identity_key": "ferret",
      "identity_sha256": "<lowercase SHA-256 of exact ferret/identity.png>",
      "frame_canvas": [64, 80]
    }
  ]
}
```

The lock must cover exactly the identities selected for the build. Cat, Dog,
and Fox are forbidden. A changed identity byte invalidates all dependent action
approval and requires a new visual review.

## ImageGen import lock

Generated RGB/RGBA source uses a distinct hard-break v4 lock. Version 4 adds
pre-generation mask provenance, two-reference edit-target lineage, imported
and registered candidate hashes, deterministic bounded-composition baselines
and results, plus one optional-nonzero role-level registration record. Versions
1, 2, and 3 are invalid release inputs: none can prove that global ImageGen
redraw outside a local semantic permission was excluded byte-exactly, and v3
does not represent an action-specific role-P0 edit target. Mechanical approval
is not acceptance of the artwork, action GIFs, pack, publication, or device
installation.

```json
{
  "schema": "kitsu-wild-imagegen-import-lock-v4",
  "identities": [
    {
      "approved": true,
      "action_semantic_contract": {
        "schema": "kitsu-wild-generated-action-semantic-locality-v3",
        "roles": ["<twelve complete canonical role records>"]
      },
      "action_semantic_contract_sha256": "<lowercase SHA-256 of canonical action_semantic_contract JSON>",
      "identity_key": "ferret",
      "identity_source_sha256": "d3ddf4e7651c8f1e8310feb6b5047e47cb3a550de3264ab567799b8682f88261",
      "identity_frame_sha256": "70cde51efa3500a8d94e6fb6e4379e001e5b8c2a0771e43169dbe1ffde79feeb",
      "transform": {
        "action_output_offset": [-1, 27],
        "alpha_background": [255, 255, 255],
        "black_coverage_threshold_per_mille": 120,
        "crop_rect": [1, 1, 1121, 1401],
        "luminance_mode": "pillow-rgb-luma-over-white",
        "output_canvas": [64, 80],
        "output_offset": [-1, 27],
        "resample_mode": "box-area",
        "source_canvas": [1122, 1402]
      },
      "transform_sha256": "<SHA-256 of the exact canonical transform JSON>"
    }
  ]
}
```

The transform's `action_output_offset` is retained as an exact legacy-reserved
field so old transform records cannot be ambiguously reinterpreted; v4
independent-frame import does not consume it. New v4 records set it equal to
the identity `output_offset`. Registration is represented only by each role's
separate hash-pinned `role_registration` record.

Every role record contains one `role_registration`, its SHA-256, and four phase
records in canonical order. Every phase pins both references supplied to
ImageGen: the immutable `identity.png` reference and the immutable edit target.
For every role phase 0, the edit target is identity. For every role phase 1..3,
including Idle, Blink, and Listen, it is exactly `<role>/00.png`, with the raw
P0 source hash, registered-candidate packed hash, and accepted-composited P0
packed hash. All three must share the same values. F1 can never become F2's
input, and no phase can reference P2 or P3.

Idle, Blink, and Listen are `identity-anchored`: phase 0 composes against the
approved standing identity; phases 1..3 compose against that accepted P0; all
four remain validated against the identity's protected landmarks and planted
contacts and require `identity-anchored-zero-offset=[0,0]`. All other roles are
`immutable-role-phase-0`: phase 0 is independently generated from identity,
composed against identity through its pre-frozen role-pose mask, then accepted
and hash-frozen. Phases 1..3 are independent edits of that P0 and compose
against that exact accepted P0. Phase 0 first passes ordinary identity Jaccard
and scale envelopes plus a bounded whole-subject component/topology delta and
hash-pinned identity-region to role-pose-region landmark gates. This permits a
real Sleep pose without permitting head, marking, or paw shimmer between
Sleep phases.

Each non-identity role may lock one
`role-p0-fixed-dx-explicit-dy-floor-derived` registration. Its P0 unregistered
floor is pinned, `dy` must equal `77 - floor`, and `dx` must preserve identity
root alignment. Both components are at most four native pixels in magnitude.
The raw imported and post-registration packed hashes are recorded for every
phase, and recomputing the same one role offset must reproduce all four
registered hashes. A per-phase registration field is malformed schema; a
different registered result is treated as a per-phase override and rejected.

Every role lock explicitly names its per-species contact policy; no policy is
inferred from a default. The defaults guide storyboarding, while the allowed
capability set is the hard schema ceiling:

```text
role      baseline                 default                         allowed capabilities
idle      identity-anchored        planted-identity                planted-identity
blink     identity-anchored        planted-identity                planted-identity
pet       immutable-role-phase-0   planted-role-base               planted-role-base | bounded-approved-gait-lift
sleep     immutable-role-phase-0   planted-role-base               planted-role-base | bounded-approved-pose-change
listen    identity-anchored        planted-identity                planted-identity
surprise  immutable-role-phase-0   planted-role-base               planted-role-base | bounded-approved-pose-change
play      immutable-role-phase-0   bounded-approved-gait-lift      planted-role-base | bounded-approved-gait-lift
tired     immutable-role-phase-0   planted-role-base               planted-role-base | bounded-approved-pose-change
feed      immutable-role-phase-0   planted-role-base               planted-role-base | bounded-approved-gait-lift
wake      immutable-role-phase-0   bounded-approved-pose-change    planted-role-base | bounded-approved-pose-change
meet      immutable-role-phase-0   bounded-approved-gait-lift      planted-role-base | bounded-approved-gait-lift
evolve    immutable-role-phase-0   bounded-approved-pose-change    planted-role-base | bounded-approved-pose-change
```

Thus a turtle Surprise may explicitly approve a bounded foot retraction, a
pangolin Sleep may explicitly approve a bounded curl/contact transition, and a
Pet or Feed may explicitly approve a bounded paw lift. An Idle lock can never
request any of those capabilities. Gait/lift policies pin a maximum of at most
16 changed floor pixels per phase; pose-change policies pin at most 32. Every
actual floor change must also be inside that phase's exact allowed-change mask,
and the zero-tolerance frozen mask must contain every baseline floor contact
outside that exact per-phase permission. Phase 0 freezes all contacts in the
immutable role-pose baseline.

Each phase pins all of the following:

- generated raw path and source SHA-256;
- imported-candidate and registered-candidate 640-byte packed SHA-256 values;
- immutable identity and edit-target source/packed hashes;
- preauthorization path, file SHA-256, storyboard SHA-256, edit-target kind,
  and exact allowed-change-mask SHA-256;
- composition mode, baseline packed SHA-256, and final composited packed
  SHA-256;
- both required zero-tolerance frozen-region kinds: `planted-contact` and
  `protected-identity-landmark`.

The final raster is recomputed from those exact values. Baseline pixels outside
the mask and candidate pixels inside the mask must both compare byte-exactly.
Production out-of-region budget remains exactly zero; global candidate redraw
does not consume a budget because it is never copied into the final raster.

Regions use `kitsu-native-region-mask-64x80-v1`: exactly 640 row-major,
least-significant-bit-first bytes represented as lowercase hexadecimal, plus
the SHA-256 of those bytes. Set bits select coordinates where a policy applies;
they do not mean that the artwork pixel must be black. Missing bytes, a changed
bit with the old hash, a changed hash with the old enclosing contract hash, an
alternate canvas/encoding, or a region entering guard rows fails closed.

Every role has at least one named motion-landmark region and a changed-pixel
minimum of at least four native pixels. Each minimum must be met inside that
landmark, all four frames must have distinct states inside the union of the
role's landmarks, and every adjacent phase must change at least four pixels
inside that union. Role-base motion is measured from phase 0, so the one-time
standing-to-Sleep pose change cannot masquerade as four meaningful Sleep
phases. Four packed-frame hashes that differ only through scattered head, tail,
paw, or background noise—and off-role noise hiding a one-pixel landmark
shimmer—do not constitute an animation.

The private release manifest is
`kitsu-wild-pack-private-release-v6`. It repeats the complete transform and
hash, semantic-v3 contract and hash, preauthorization-v2 and registration-v1
schemas, raw identity and imported identity hashes, every preauthorization and
raw action source hash, every imported/registered/baseline/final packed hash,
role registrations, and fixed scales. ImageGen imports record
`identity_raster_scale=action_cell_raster_scale=64/1120`; the only permitted
generated source layout is one independent full-canvas file per phase. The
portrait synchronizer revalidates the star lineage and all of these hashes
before it will consume even the separately authored catalog portraits.

The importer rejects more than 55% mid-tone source ink or excessive coverage
sensitivity within pixels that can enter the preauthorized region. Candidate
ambiguity outside that region is recorded in raw/candidate hashes and
discarded by construction. The composited final raster is always validated
globally. These are rejection and composition rules, never alternate per-frame
thresholds.

## Frame and portrait packing

Each frame is packed row-major using eight bytes per row. Pixel `(x, y)` maps
to byte `y * 8 + x // 8` and bit `1 << (x & 7)`. A set bit is OLED ink. This is
XBM least-significant-bit-first packing and must round-trip all 640 bytes to the
exact source mask.

The catalog portrait remains `16x18`, 36 bytes, two bytes per row using the same
LSB-first rule. It is **not derived by resizing the 64x80 identity**. It is an
independently authored `portrait.png`, visually matched to the locked identity,
and is packed without transformation. This keeps catalog constraints from
reintroducing the destructive downscale the native animation path removes.

## Mechanical rejection gates

The format-v2 validator rejects the complete build when any of these occur:

- a direct-target frame is not exact mode `1` or is not exactly 64x80;
- a generated frame is not RGB/RGBA on the exact locked source canvas;
- the selected source layout does not match its exact source tree;
- a one-action sheet or any other multi-phase generated source is selected;
- the portrait is not exact mode `1` or is not exactly 16x18;
- the source tree has missing or unexpected identities, actions, phases, or
  preauthorization records;
- any subject pixel leaves `[2, 2, 61, 77]` or enters rows 78..79;
- the subject does not land on floor `y=77` or leaves the centered stage;
- Cat, Dog, or Fox enters the new-art pipeline;
- a legacy identity lock, ImageGen import lock v1/v2/v3, changed identity hash,
  changed import-transform hash, missing or drifted semantic/preauthorization/
  registration hash, or unapproved identity is used;
- a generated source contains ink in the removed border, the BOX/coverage
  result is threshold-unstable inside its allowed output region, or the fixed
  transform/role registration clips its input;
- role registration exceeds four pixels, uses non-floor-derived `dy`, drifts
  identity root alignment, or differs between phases;
- the frame contains excessive components, a second subject, or detached
  debris outside the primary subject;
- any phase is byte-identical to another phase, an adjacent pair changes fewer
  than four native pixels, or the complete action changes fewer than sixteen;
- a phase omits the immutable identity reference; a role P0 does not target
  identity; any role phase 1..3 does not target the same accepted P0; or
  P1/P2/P3 forms a chain;
- a preauthorization was not frozen before generation, contains dynamic fields,
  changes its storyboard/target/mask hash, or differs from the semantic lock;
- a deterministic composite differs from baseline outside its mask or from the
  registered candidate inside it, changes a planted contact/protected landmark,
  requests a contact capability outside the role's allowed set or bound, drifts
  a role-pose landmark/topology gate, or proves uniqueness only with pixels
  outside role motion landmarks;
- adjacent phase differences indicate a discontinuous identity/pose jump;
- apparent scale leaves the role-specific identity envelope or pops within an
  action;
- a 640-byte frame or 36-byte portrait packing round-trip changes one pixel;
- a direct-target builder attempts any transform, or an ImageGen importer
  attempts a transform not exactly equal to its identity lock;
- a builder attempts to clean, morph, delete components, auto-fit, tune one
  frame, or overwrite an existing accepted output.

Mechanical gates cannot prove that a ferret looks like a ferret or that `meet`
reads as a greeting. Final acceptance must bind a human-reviewed 1x/8x contact
sheet, every action GIF, all fifty source PNG hashes (identity, portrait, and
forty-eight phases), all forty-eight preauthorization JSON hashes, and the exact
pack SHA-256. Every role must be explicitly accepted; passing metrics alone
never authorizes integration or publication.
