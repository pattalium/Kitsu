# K32 Kitsu public-site design system

This document is the source of truth for the public Kitsu site. It adapts two
references from the local Awesome DESIGN.md collection:

- Nintendo.com (2001): manufactured-console geometry, hardware faceplates,
  compact technical labels, and warm accents that mean action.
- Warp: a warm dark canvas, calm editorial typography, hairline elevation, and
  readable content bands instead of decorative card grids.

The result is Kitsu's own field-instrument language. It must not copy either
company's branding, artwork, logos, or text.

## Product character

Kitsu is a friendly companion inside serious radio hardware. The website should
feel tactile, capable, private, and a little playful. It is not a generic SaaS
landing page and it is not a fake monitoring dashboard.

## Visual principles

- Build the page from connected instrument panels and broad editorial bands.
- Use asymmetry: narrative copy on one side, a portrait Kitsu device on the
  other, with occasional elements crossing the grid.
- Use a single blue signal accent for radio, Bluetooth, links, and focus.
- Reserve amber for primary forward actions and voluntary support.
- Prefer sharp or lightly chamfered geometry. Pills are for tiny status marks,
  never for every button or container.
- Elevation comes from surface contrast, hairlines, inset highlights, and rare
  hard shadows. Avoid glassmorphism and soft floating-card stacks.
- Do not show statistics, decorative counters, numbered feature cards, download
  counts, or fake live telemetry.

## Color roles

Dark is the canonical theme. A light variant may reinterpret the same materials
without changing hierarchy.

| Role | Dark | Light | Use |
| --- | --- | --- | --- |
| Canvas | `#090b0d` | `#e9ecec` | Page background |
| Panel | `#111519` | `#f7f8f6` | Main instrument surface |
| Panel raised | `#171c21` | `#ffffff` | Device and download modules |
| Hairline | `#303840` | `#aab2b7` | Panel divisions |
| Ink | `#f2f0e9` | `#111518` | Primary text |
| Body | `#b7bdc2` | `#424b50` | Supporting text |
| Muted | `#7f8990` | `#667078` | Technical labels |
| Signal | `#49b9f2` | `#006eaa` | Radio, links, focus |
| Action | `#f4b52e` | `#9c6500` | Primary CTA and support |
| Danger | `#ff6b62` | `#b42318` | Verification failure only |

## Typography

- Display: `Arial Narrow`, `Aptos Display`, `Segoe UI`, sans-serif. Weight 700,
  tight tracking, sentence case.
- Narrative: `Segoe UI`, `Inter`, Arial, sans-serif. Weight 400/600.
- Technical: `Cascadia Mono`, `SFMono-Regular`, Consolas, monospace. Uppercase
  only for short labels; never uppercase paragraphs.
- Hero: `clamp(3.5rem, 8vw, 7.25rem)`, compact line height, no separate giant
  product-name billboard.
- Body: 1rem to 1.15rem with 1.65 line height and a readable measure.

## Spacing and layout

- Base spacing unit: 4px.
- Main steps: 8, 12, 16, 24, 32, 48, 72, 96px.
- Content width: 1440px maximum, with fluid 24 to 64px side padding.
- Hero: asymmetric two-column grid, approximately 55/45.
- Use 72 to 120px between major sections on desktop and 56 to 80px on mobile.
- Touch targets are at least 44px. Focus rings are visible and never clipped.
- At narrow widths, preserve reading order: copy, actions, device, signal rail.

## Component language

### Instrument panel

Near-black or off-white surface, 1px hairline, 2px radius, optional cut-corner
background detail, and a restrained inset highlight. Decorative screws may be
used only on the hero device and download module.

### Buttons

Rectangular, compact, minimum 44px high. Primary is amber with dark text;
secondary is transparent with a hairline. Hover is a two-pixel lift or color
change, not a glow. External links disclose their destination in accessible
text when necessary.

### Signal rail

A single connected horizontal band for Bluetooth, LoRa, and local ownership.
It is not a row of independent cards and contains no numeric labels.

### Download module

One prominent instrument panel. The signed-manifest status, release title,
description, action, and digest evidence form one hierarchy. Failure remains
visible and fail-closed.

### Resource links

Use editorial rows with a dividing rule and directional cue. Do not repeat the
same bordered card treatment for every destination.

## Motion

- One short hero entrance and restrained hover feedback.
- No continuous movement except the subtle radio pulse, which stops under
  `prefers-reduced-motion`.
- Motion duration stays near 160 to 420ms and never blocks interaction.

## Content rules

- Lead with the companion, then explain the transport and trust boundaries.
- Use plain language. Avoid inflated marketing claims.
- Do not advertise Play until a Play release exists.
- The Ko-fi action is a plain outbound link. Support remains voluntary and
  grants no feature, content, badge, or benefit.
- Preserve the clean-install warning between package/signing tracks.
- Use ordinary hyphens in visible copy; avoid decorative em dashes.

## Responsive and accessibility rules

- Navigation may wrap or reduce to essential links without a JavaScript-only
  menu.
- No horizontal page scrolling at 360px or wider.
- Body text maintains WCAG AA contrast; interactive focus uses the blue signal
  color plus an offset.
- Decorative hardware details are hidden from assistive technology.
- Headings remain in logical order and links describe their actual destination.
- Verification loading, success, and failure text is announced via a polite
  live region.
