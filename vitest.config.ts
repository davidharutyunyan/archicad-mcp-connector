import { defineConfig } from "vitest/config";

export default defineConfig({
  test: {
    include: ["test/**/*.test.ts"],
    exclude: process.env["ARCHICAD_LIVE"] ? [] : ["test/live/**"],
    testTimeout: 120_000,
  },
});
