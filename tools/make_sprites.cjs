#!/usr/bin/env node
// Compatibility entry point for the public default-pack builder.
// The actual image normalization and K868 serialization live in Python so the
// same Pillow-based pipeline can render and verify release evidence.

const path = require('path');
const fs = require('fs');
const { spawnSync } = require('child_process');

const PACK_PARTITION_BYTES = 0x140000;
const project = path.resolve(__dirname, '..');
const builder = path.join(project, 'tools', 'build_default_packs.py');
const python = process.env.KITSU_PYTHON || (process.platform === 'win32' ? 'python' : 'python3');
const result = spawnSync(python, [builder, ...process.argv.slice(2)], {
  cwd: project,
  stdio: 'inherit',
});

if (result.error) {
  console.error(`PACK_BUILD_FAIL ${result.error.message}`);
  process.exit(1);
}
if (result.status === 0) {
  const packDirectory = path.join(project, 'assets', 'packs');
  for (const filename of fs.readdirSync(packDirectory)) {
    if (filename.endsWith('.k868')) {
      const bytes = fs.statSync(path.join(packDirectory, filename)).size;
      if (bytes > PACK_PARTITION_BYTES) {
        console.error(`PACK_BUILD_FAIL ${filename} exceeds ${PACK_PARTITION_BYTES} bytes`);
        process.exit(1);
      }
    }
  }
}
process.exit(result.status ?? 1);
