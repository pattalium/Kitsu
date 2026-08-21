import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";

// The production target is a static bundle served by the owner's local nginx.
// Cloudflare Tunnel terminates the public route to that origin.
export default defineConfig({
  plugins: [react()],
});
