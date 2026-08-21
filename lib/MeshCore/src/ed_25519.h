#pragma once

// The upstream MeshCore repository keeps this dependency in lib/ed25519.
// Kitsu's vendored PlatformIO library places it below src so its C sources
// are compiled as part of this deliberately small library.
#include "ed25519/ed_25519.h"
