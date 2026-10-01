import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";

import { ArchicadClient, type ClientOptions } from "./archicad/client.js";
import { SERVER_INSTRUCTIONS } from "./guide.js";
import type { ToolContext } from "./tools/define.js";
import { registerAllTools } from "./tools/index.js";

export const SERVER_NAME = "archicad-connector";
export const SERVER_VERSION = "1.0.0";

export function createServer(options: ClientOptions = {}): { server: McpServer; ctx: ToolContext } {
  const server = new McpServer({ name: SERVER_NAME, version: SERVER_VERSION }, { instructions: SERVER_INSTRUCTIONS });
  const ctx: ToolContext = { ac: new ArchicadClient(options) };
  registerAllTools(server, ctx);
  return { server, ctx };
}
