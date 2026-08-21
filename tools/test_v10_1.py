"""Receive-only hardware QA entry point for Kitsu868 0.10.1.

The complete, audited serial allow-list and MeshCore checks remain in
``test_v09`` so patch releases exercise the same passive test surface.  This
entry point changes only the expected firmware version.  It does not add an
unlock, transmit, introduction, or message-send command.
"""

from __future__ import annotations

import test_v09


test_v09.FIRMWARE_VERSION = "0.10.1"


if __name__ == "__main__":
    raise SystemExit(test_v09.main())
