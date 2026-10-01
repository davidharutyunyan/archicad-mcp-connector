#!/usr/bin/env node
/**
 * archicad-connector — MCP server (stdio).
 * Environment:
 *   ARCHICAD_PORT        fixed JSON API port (default: auto-discover 19723-19744)
 *   ARCHICAD_HOST        default 127.0.0.1
 *   ARCHICAD_TIMEOUT_MS  per-request timeout (default 120000)
 */

import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";

import { createServer } from "./server.js";

const port = process.env["ARCHICAD_PORT"] ? Number(process.env["ARCHICAD_PORT"]) : undefined;
const timeoutMs = process.env["ARCHICAD_TIMEOUT_MS"] ? Number(process.env["ARCHICAD_TIMEOUT_MS"]) : undefined;
const host = process.env["ARCHICAD_HOST"];

const { server } = createServer({ port, timeoutMs, host });
await server.connect(new StdioServerTransport());
