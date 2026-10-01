/**
 * Calls MCP tools of this server in-process (same validation + handlers Claude uses).
 *   npx tsx scripts/mcp-call.ts --list                       list tool names
 *   npx tsx scripts/mcp-call.ts --describe <tool>            show a tool's input schema
 *   npx tsx scripts/mcp-call.ts <tool> '<json-args>'         call a tool
 *   npx tsx scripts/mcp-call.ts <tool> @args.json            args from a file
 * Images returned by tools are written to test/.tmp/<tool>-<n>.png and their path is printed.
 */

import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";

import { createServer } from "../src/server.js";

const [, , tool, rawArgs] = process.argv;
if (!tool) {
  console.error("usage: mcp-call.ts --list | --describe <tool> | <tool> '<json>'");
  process.exit(2);
}

const { server } = createServer({
  port: process.env["ARCHICAD_PORT"] ? Number(process.env["ARCHICAD_PORT"]) : undefined,
});
const [clientT, serverT] = InMemoryTransport.createLinkedPair();
await server.connect(serverT);
const client = new Client({ name: "mcp-call", version: "1.0.0" });
await client.connect(clientT);

if (tool === "--list" || tool === "--describe") {
  const { tools } = await client.listTools();
  if (tool === "--list") {
    for (const t of tools) console.log(t.name);
    console.error(`${tools.length} tools`);
  } else {
    const t = tools.find((x) => x.name === rawArgs);
    console.log(JSON.stringify(t, null, 2));
  }
  process.exit(0);
}

let args: Record<string, unknown> = {};
if (rawArgs) args = JSON.parse(rawArgs.startsWith("@") ? readFileSync(rawArgs.slice(1), "utf8") : rawArgs);
const result = await client.callTool({ name: tool, arguments: args });
let n = 0;
for (const block of result.content as Array<Record<string, unknown>>) {
  if (block["type"] === "text") console.log(block["text"]);
  else if (block["type"] === "image") {
    mkdirSync("test/.tmp", { recursive: true });
    const ext = String(block["mimeType"]).includes("jpeg") ? "jpg" : "png";
    const file = `test/.tmp/${tool}-${n++}.${ext}`;
    writeFileSync(file, Buffer.from(String(block["data"]), "base64"));
    console.log(`[image ${block["mimeType"]} -> ${file}]`);
  } else console.log(JSON.stringify(block));
}
if (result.isError) process.exitCode = 1;
await client.close();
