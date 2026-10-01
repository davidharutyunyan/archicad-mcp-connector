/** Prints compact tool signatures: npx tsx scripts/sig.ts <tool-or-prefix>... */
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { ArchicadClient } from "../src/archicad/client.js";
import { registerAllTools } from "../src/tools/index.js";

type S = { type?: string | string[]; properties?: Record<string, S>; required?: string[]; items?: S; enum?: unknown[]; anyOf?: S[]; description?: string };
const short = (s: S | undefined, d = 0): string => {
  if (!s) return "?";
  if (s.enum) return s.enum.join("|");
  if (s.anyOf) return s.anyOf.map((x) => short(x, d)).join(" / ");
  if (s.type === "array") return `[${short(s.items, d)}]`;
  if (s.type === "object" && s.properties && d < 2)
    return `{${Object.entries(s.properties).map(([k, v]) => `${k}${s.required?.includes(k) ? "*" : ""}:${short(v, d + 1)}`).join(", ")}}`;
  return String(s.type ?? "any");
};
const server = new McpServer({ name: "x", version: "1" });
registerAllTools(server, { ac: new ArchicadClient({ port: 19723 }) }, "all");
const [a, b] = InMemoryTransport.createLinkedPair();
await server.connect(b);
const c = new Client({ name: "x", version: "1" });
await c.connect(a);
const { tools } = await c.listTools();
const wanted = process.argv.slice(2);
for (const t of tools) {
  if (wanted.length && !wanted.some((w) => t.name === w || t.name.startsWith(w))) continue;
  console.log(`${t.name}: ${short(t.inputSchema as S)}`);
}
await c.close();
