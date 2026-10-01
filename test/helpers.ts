/**
 * Test harness: an in-process MCP client connected to the real server, with the
 * Archicad HTTP endpoint replaced by a fake that records requests.
 *
 *   const h = await harness((req) => ({ succeeded: true, result: { addOnCommandResponse: { results: [] } } }));
 *   const res = await h.call("create_walls", { walls: [...] });
 *   expect(h.requests[0].parameters.addOnCommandParameters).toEqual(...)
 */

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";

import { createServer } from "../src/server.js";

export interface RecordedRequest {
  command: string;
  parameters?: Record<string, unknown>;
  /** For API.ExecuteAddOnCommand: the add-on command name */
  addOnCommand?: string;
  addOnParameters?: Record<string, unknown>;
}

export type Responder = (req: RecordedRequest) => unknown;

/** Wraps an add-on response object the way Archicad does. */
export function addonOk(response: unknown) {
  return { succeeded: true, result: { addOnCommandResponse: response } };
}

export function apiOk(result: unknown) {
  return { succeeded: true, result };
}

export async function harness(responder: Responder) {
  const requests: RecordedRequest[] = [];
  const fetchImpl = (async (_url: string, init?: { body?: string }) => {
    const body = JSON.parse(init?.body ?? "{}") as { command: string; parameters?: Record<string, unknown> };
    const req: RecordedRequest = { command: body.command, parameters: body.parameters };
    if (body.command === "API.ExecuteAddOnCommand") {
      const p = body.parameters as { addOnCommandId: { commandName: string }; addOnCommandParameters: Record<string, unknown> };
      req.addOnCommand = p.addOnCommandId.commandName;
      req.addOnParameters = p.addOnCommandParameters;
    }
    requests.push(req);
    const response = responder(req);
    return new Response(JSON.stringify(response), { status: 200, headers: { "Content-Type": "application/json" } });
  }) as unknown as typeof fetch;

  const { server } = createServer({ port: 19723, fetchImpl });
  const [clientT, serverT] = InMemoryTransport.createLinkedPair();
  await server.connect(serverT);
  const client = new Client({ name: "test", version: "1.0.0" });
  await client.connect(clientT);

  return {
    client,
    requests,
    async call(name: string, args: Record<string, unknown> = {}) {
      const result = await client.callTool({ name, arguments: args });
      const content = result.content as Array<{ type: string; text?: string }>;
      const text = content.find((c) => c.type === "text")?.text ?? "";
      let json: unknown = undefined;
      try {
        json = JSON.parse(text);
      } catch {
        /* not JSON */
      }
      return { isError: Boolean(result.isError), text, json, content };
    },
    async listTools() {
      return (await client.listTools()).tools;
    },
  };
}
