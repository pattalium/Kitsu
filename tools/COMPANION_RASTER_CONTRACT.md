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

Release artwork is forty-eight independent native PNGs, one identity PNG, and
one independently authored portrait PNG. There are no multi-action sheets and
no 2x2 release sheets.

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

The separately locked ImageGen import path uses the same directory layout, but
`identity.png` and the forty-eight action PNGs are RGB/RGBA files sharing the
exact identity source canvas. The exact 16x18 portrait is still authored
separately. The two paths have different lock schemas and cannot be silently
reinterpreted as one another.

## Image-generation workflow

ImageGen is used one identity or one named action at a time. Cat, Dog, and Fox
may be supplied only as immutable style and line-economy references; a real
animal reference should supply anatomy. The approved identity is then pinned
and reused as the identity reference for all twelve actions.

Direct 64x80 art remains preferred. A larger ImageGen result may enter the
separate importer only through one quality-preserving transform approved from
the identity and reused byte-for-byte for all forty-eight frames:

1. require the exact locked source canvas for identity and every action;
2. composite RGB/RGBA over opaque white;
3. use an exact centered 4:5 viewport that removes at most a two-pixel proven-
   white source border—never a subject bounding-box crop;
4. downsample black coverage with Pillow `BOX` area sampling to 64x80;
5. apply one identity-stage black-coverage threshold, recorded per mille;
6. apply one integer output offset derived from the identity only, to place its
   body near `x=32` and its floor at `y=77`;
7. validate the exact resulting mask without morphology, cleanup, component
   deletion, sharpening, per-frame translation, or per-frame threshold tuning.

The source crop, output offset, and coverage threshold may be compared only at
the identity-approval stage. Once selected, their canonical JSON SHA-256 is an
immutable input to every action. If that fixed transform clips or mis-scales an
action, reject the action; the importer has no auto-fit fallback. BOX coverage
is used instead of source-threshold plus nearest-neighbor reduction so a clean
one-logical-pixel contour survives without nearest-sample phase loss.

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

Generated RGB/RGBA source uses a distinct lock. This is the exact transform
used for the selected Ferret E identity; other identities may use their own
identity-approved near-full-canvas centered 4:5 viewport, but all their actions
must reuse it exactly.

```json
{
  "schema": "kitsu-wild-imagegen-import-lock-v1",
  "identities": [
    {
      "approved": true,
      "identity_key": "ferret",
      "identity_source_sha256": "d3ddf4e7651c8f1e8310feb6b5047e47cb3a550de3264ab567799b8682f88261",
      "identity_frame_sha256": "7dfe950a53a8560ceafa9888328dd34271fe70f64fb4b8a805840122074a4e64",
      "transform": {
        "alpha_background": [255, 255, 255],
        "black_coverage_threshold_per_mille": 180,
        "crop_rect": [1, 1, 1121, 1401],
        "luminance_mode": "pillow-rgb-luma-over-white",
        "output_canvas": [64, 80],
        "output_offset": [-1, 27],
        "resample_mode": "box-area",
        "source_canvas": [1122, 1402]
      },
      "transform_sha256": "22ab825dffc03758d28d96b9536f29ab276b8f015617325a0fbf7d37d95b5893"
    }
  ]
}
```

The manifest repeats the complete transform, its hash, the raw identity hash,
the imported 640-byte identity hash, every raw action hash, every final frame
hash, and `fixed_action_scale=64/1120`. A one-pixel source-canvas, crop, or
offset change breaks the lock. The importer rejects more than 55% mid-tone
source ink, or a final raster where more than 5% of ink pixels change under a
`+/-20`-per-mille coverage-stability probe; these are rejection gates, not
alternate thresholds.

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
- the portrait is not exact mode `1` or is not exactly 16x18;
- the source tree has missing or unexpected identities, actions, or phases;
- any subject pixel leaves `[2, 2, 61, 77]` or enters rows 78..79;
- the subject does not land on floor `y=77` or leaves the centered stage;
- Cat, Dog, or Fox enters the new-art pipeline;
- a v1 identity lock, changed identity hash, changed import-transform hash, or
  unapproved identity is used;
- a generated source contains ink in the removed border, the BOX/coverage
  result is threshold-unstable, or the fixed output offset clips an action;
- the frame contains excessive components, a second subject, or detached
  debris outside the primary subject;
- any phase is byte-identical to another phase, an adjacent pair changes fewer
  than four native pixels, or the complete action changes fewer than sixteen;
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
forty-eight phases), and the exact pack SHA-256. Every role must be explicitly
accepted; passing metrics alone never authorizes integration or publication.
