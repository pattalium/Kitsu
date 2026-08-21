# Default companion source sheets

These nine source sheets are the public artwork inputs for the default Kitsu
companion packs: Cat, Fox, and Dog. Each species has three 4-by-4 sheets:

- `core`: Idle, Blink, Pet, Play
- `life`: Feed, Sleep, Wake, Listen
- `social`: Surprise, Meet, Evolve, Tired

Rows are roles in the order above; columns are four chronological animation
frames. `tools/build_default_packs.py` crops the equal cells, removes isolated
artifacts, applies one scale per four-frame role, anchors the perceived body axis at
`x=32`, anchors the lowest body pixel at `y=61`, and serializes the result as a
K868 pack.

The sheets use one shared visual system and one shared four-phase choreography
for all three species. Within a role, identity, proportions, line weight,
camera, body center, and floor baseline stay fixed unless the action itself
requires a pose change. The artwork is pure black and white with no labels,
grid lines, symbols, motion marks, props, or detached artifacts.

Species identities:

- Cat: normal domestic kitten, triangular ears, striped forehead, curled tail.
- Dog: normal floppy-eared puppy, round muzzle, collar tag, wagging tail.
- Fox: normal four-legged fox, pointed ears, tapered muzzle, dark lower legs,
  and a large bushy tail with a white tip indicated by negative space; never
  bipedal or human-shaped.

Sheet rows, top to bottom:

- `core`: idle breathing/tail sway; open-close-open blink; happy pet reaction
  without a visible hand; playful pounce or bow returning to center.
- `life`: sniff-lick-chew-satisfied feed reaction without food; settle into
  sleep; wake through a stretch to alert; listen with ear direction changes.
- `social`: surprise and recover; friendly bow/paw greeting; taller proud
  evolve posture; tired transition into a low resting pose.

The builder records the exact source SHA-256 digests in
`assets/packs/default-packs-manifest.json` and produces the Cat, Fox, and Dog
default K868 payloads from these nine files only.
