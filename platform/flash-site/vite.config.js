import { defineConfig } from "vite";

export default defineConfig({
  build: {
    rollupOptions: {
      output: {
        assetFileNames(assetInfo) {
          const sourceName = assetInfo.originalFileNames?.[0]
            ?? assetInfo.names?.[0]
            ?? assetInfo.name
            ?? "";
          if (sourceName.endsWith(".k868")) return "assets/[name]-[hash].pet";
          return "assets/[name]-[hash][extname]";
        },
      },
    },
  },
});
