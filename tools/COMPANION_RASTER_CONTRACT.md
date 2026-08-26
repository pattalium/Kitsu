# Companion raster contract

This is the private wild-companion build contract. Cat, Dog, and Fox are
protected starters and must never be passed to this pipeline.

## Candidate source layout

Keep candidates outside the public checkout. Each identity has one approved
identity master and twelve separate action files; never combine different
actions into one generated sheet.

```text
<private-source>/rabbit/
  identity.png
  idle.png
  blink.png
  pet.png
  sleep.png
  listen.png
  surprise.png
  play.png
  tired.png
  feed.png
  wake.png
  meet.png
  evolve.png
```

Each action PNG is an even square 2x2 image containing phases 0, 1, 2, and 3
in reading order. Its full canvas must exactly match `identity.png`. Subjects
must have whitespace on every cell edge. The four phases must all be present
and remain distinct after 64x64 conversion.

The builder does not clean, crop, repair, clamp, or automatically shrink
ImageGen output. It resizes the complete cell with nearest-neighbour sampling
at one scale derived from the approved identity master, then applies rigid
translation to body-axis `x=32` and floor `y=61`. A frame that does not fit is
rejected and must be regenerated.

## Identity lock

Create a private lock only after the identity master is visually approved.
The lock must cover exactly the identities selected on the command line.

```json
{
  "schema": "kitsu-wild-identity-lock-v1",
  "identities": [
    {
      "approved": true,
      "identity_key": "rabbit",
      "identity_sha256": "<lowercase SHA-256 of rabbit/identity.png>",
      "source_canvas": [1254, 1254],
      "target_long_axis_pixels": 48
    }
  ]
}
```

`target_long_axis_pixels` is an explicit visual decision in the range 40..52;
it is not inferred from the largest action frame.

## Private candidate build

The destination must not exist. The builder uses a sibling staging directory,
validates the complete selected build, snapshots every input PNG byte-for-byte,
and performs one final rename. An existing destination is never overwritten.

```powershell
python tools/build_wild_packs.py `
  --source-dir C:\private\kitsu-candidates `
  --identity-lock C:\private\kitsu-identity-lock-v1.json `
  --private-output C:\private\kitsu-rabbit-review-20260827 `
  --species rabbit
```

Review all twelve private GIFs and the 48-frame contact sheet. After visual
acceptance is recorded against the exact pack and contact-sheet SHA-256 values,
rerun deterministically to a second new destination with:

```powershell
  --visual-acceptance C:\private\kitsu-wild-visual-acceptance-v1.json
```

The resulting manifest is `kitsu-wild-pack-private-release-v3`. Public portrait
synchronization accepts only a complete v3 roster with the exact no-crop,
no-shrink, no-cleanup contract and byte-exact source-snapshot provenance.

## Mechanical rejection gates

- subject ink touching a quadrant edge (source clipping);
- empty, duplicate, or downscale-collapsed frames;
- more or fewer than phases 0..3;
- detached debris, excessive components, or one-pixel output fragments;
- per-frame apparent scale outside the role envelope;
- failure to fit the fixed safe canvas at the locked scale;
- identity Jaccard below the role-specific floor;
- any 64x64 or 16x18 1-bit packing round-trip mismatch;
- a changed identity SHA-256 or unapproved identity lock;
- Cat, Dog, or Fox entering the wild builder;
- any attempt to reuse an existing output directory.
