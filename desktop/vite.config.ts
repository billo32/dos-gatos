import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import tailwindcss from "@tailwindcss/vite";

// The window UI lives in web/; `npm run build:web` → dist-web/ (Tauri's frontendDist).
export default defineConfig({
  root: "web",
  plugins: [react(), tailwindcss()],
  clearScreen: false,
  server: { port: 5173, strictPort: true },
  build: { outDir: "../dist-web", emptyOutDir: true, target: "safari15" },
  test: { root: ".", include: ["web/src/**/*.test.ts"] },
});
