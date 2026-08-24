# K32 Kitsu visual system

This is the source of truth for K32's public web surfaces. It refines the warm,
quiet identity of the original K32 homepage. It must not be replaced with a
generic product launch, dashboard, editorial publication, or fake hardware
presentation.

## Character

Kitsu is a small, local-first radio companion. The interface should feel warm,
capable, open, and personal without pretending that Kitsu is a mass-market
product. Lead with the companion and its real behaviors. Explain the radio and
trust boundaries plainly.

## Non-negotiable rules

- Do not invent a device enclosure, phone mockup, hardware render, or product
  photograph. The fox mark and restrained radio-line graphics are enough.
- Do not use decorative counters, statistics, numbered cards, or fake live
  telemetry.
- Do not turn the site into a news, blog, magazine, or SaaS landing page.
- Avoid nested card grids. Use open sections, dividers, and a small number of
  purposeful surfaces.
- Preserve all release-verification, privacy, and fail-closed behavior.
- Light and dark themes have equal status. The user's explicit choice persists.

## Visual language

The light theme uses warm cream paper, near-black ink, fine taupe rules, and one
muted burnt-orange accent. Dark mode translates the same hierarchy to warm
charcoal rather than blue-black. Surfaces remain quiet; depth comes from
hairlines and rare soft shadows.

| Role | Light | Dark |
| --- | --- | --- |
| Canvas | `#f3efe5` | `#12110f` |
| Surface | `#fbf9f3` | `#1a1916` |
| Strong surface | `#fffdf8` | `#211f1b` |
| Ink | `#181713` | `#f4efe4` |
| Body | `#646158` | `#c3bcaf` |
| Rule | `#d6cfc1` | `#39362f` |
| Accent | `#c95b36` | `#ef7a50` |

Display typography is Georgia or Times New Roman: humane, compact, and
recognizably continuous with the original site. Interface and body text use
Aptos or Segoe UI. Monospace is reserved for short technical labels, never for
paragraphs.

## Layout and components

- Maximum content width is 1380px with fluid 20-64px gutters.
- The hero is copy plus an unboxed, low-opacity fox/radio trace composition.
- Major sections use generous vertical rhythm and rounded corners no larger
  than 24px. Links and feature descriptions usually sit on divider rows.
- One dark band explains Bluetooth and LoRa; it is a deliberate contrast beat,
  not the default appearance of the whole site.
- Buttons have at least a 44px target, visible focus, clear labels, and modest
  12-14px radii. Pills are not the default component shape.
- The verified Android release is one coherent module. Historical testing
  builds are preserved as files but are not advertised.
- Resource destinations are open list rows, not repeated promotional cards.

## Motion, responsiveness, and access

Hover motion is limited to a two-pixel lift or directional cue. There is no
continuous animation. All motion is suppressed for reduced-motion users.
Reading order remains copy-first on narrow screens, no horizontal page scroll
is allowed at 360px or wider, focus rings remain visible, and status changes
use the existing polite live region.

## Cross-site use

The public site, manual, flasher, status page, connected-app rollback surface,
and custom login theme share these tokens and character. Each surface keeps its
own information architecture and business logic. Machine-only API, gateway,
and update origins do not receive decorative landing pages.
