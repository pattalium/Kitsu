#pragma once

// MeshCore headers mention Arduino's Stream only by reference in the code
// exercised by native Dispatcher tests.  A complete Arduino Print/Stream
// implementation is deliberately unnecessary here.
class Stream {};
