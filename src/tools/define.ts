/**
 * Tool definition helper shared by every tool family.
 *
 *   defineTool(server, ctx, {
 *     name: "create_walls",
 *     title: "Create walls",
 *     description: "...",
 *     input: { walls: z.array(WallSpec).min(1) },
 *     annotations: { destructiveHint: false },
 *     handler: async ({ walls }, ctx) => ctx.ac.addon("CreateElements", { elements: ... }),
 *   });
 *
 * The handler returns plain data (serialized as compact JSON with rounded numbers), an
 * `ImageResult` (returned as MCP image content), or a `ToolContent` for full control.
 * Thrown errors become `isError` results with an actionable message.
 */

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import type { CallToolResult, ToolAnnotations } from "@modelcontextprotocol/sdk/types.js";
import { z, type ZodRawShape } from "zod";

import { ArchicadCommandError, ArchicadConnectionError, type ArchicadClient } from "../archicad/client.js";

export interface ToolContext {
  ac: ArchicadClient;
}

export interface ImageResult {
  kind: "image";
  /** base64-encoded image bytes */
  data: string;
  mimeType: string;
  /** optional JSON-able metadata returned as a text block next to the image */
  meta?: unknown;
}

export interface ToolContent {
  kind: "content";
  result: CallToolResult;
}

export function imageResult(data: string, mimeType: string, meta?: unknown): ImageResult {
  return { kind: "image", data, mimeType, meta };
}

type HandlerResult = unknown | ImageResult | ToolContent;

export interface ToolDefinition<S extends ZodRawShape> {
  name: string;
  title: string;
  description: string;
  input: S;
  annotations?: ToolAnnotations;
  handler: (args: z.objectOutputType<S, z.ZodTypeAny>, ctx: ToolContext) => Promise<HandlerResult>;
}

/** Maximum characters of JSON returned in one tool result before truncation. */
export const MAX_RESULT_CHARS = 180_000;

/** Rounds floating point noise (3.0000000000004 -> 3) to keep results readable and small. */
export function roundNumbers(value: unknown, digits = 6): unknown {
  if (typeof value === "number") {
    if (!Number.isFinite(value) || Number.isInteger(value)) return value;
    const f = 10 ** digits;
    return Math.round(value * f) / f;
  }
  if (Array.isArray(value)) return value.map((v) => roundNumbers(v, digits));
  if (value && typeof value === "object") {
    const out: Record<string, unknown> = {};
    for (const [k, v] of Object.entries(value)) out[k] = roundNumbers(v, digits);
    return out;
  }
  return value;
}

export function formatJson(data: unknown): string {
  const text = JSON.stringify(roundNumbers(data));
  if (text === undefined) return "null";
  if (text.length <= MAX_RESULT_CHARS) return text;
  return (
    text.slice(0, MAX_RESULT_CHARS) +
    `\n…[truncated: result was ${text.length} characters. Narrow the request (filters, fewer elements, limit/offset, fields).]`
  );
}

function isImage(x: unknown): x is ImageResult {
  return typeof x === "object" && x !== null && (x as { kind?: unknown }).kind === "image";
}

function isContent(x: unknown): x is ToolContent {
  return typeof x === "object" && x !== null && (x as { kind?: unknown }).kind === "content";
}

export function errorResult(err: unknown): CallToolResult {
  let message: string;
  if (err instanceof ArchicadConnectionError) {
    message = `Archicad connection problem: ${err.message}`;
  } else if (err instanceof ArchicadCommandError) {
    message = err.message;
  } else if (err instanceof z.ZodError) {
    message = `Invalid input: ${err.issues.map((i) => `${i.path.join(".") || "(root)"}: ${i.message}`).join("; ")}`;
  } else if (err instanceof Error) {
    message = err.message;
  } else {
    message = String(err);
  }
  return { isError: true, content: [{ type: "text", text: message }] };
}

/**
 * Makes every object schema reachable from `schema` reject unknown keys (zod "strict"), in place,
 * except objects explicitly marked .passthrough(). Misspelled fields then produce an error that
 * names the key instead of being silently dropped (which would make the tool run with defaults).
 */
export function makeStrict(schema: z.ZodTypeAny, seen = new Set<z.ZodTypeAny>()): void {
  if (!schema || seen.has(schema)) return;
  seen.add(schema);
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  const d = (schema as any)._def;
  if (!d) return;
  if (schema instanceof z.ZodObject) {
    if (d.unknownKeys === "strip") d.unknownKeys = "strict";
    for (const v of Object.values(schema.shape as Record<string, z.ZodTypeAny>)) makeStrict(v, seen);
    return;
  }
  const children: unknown[] = [d.innerType, d.schema, d.type, d.valueType, d.left, d.right, d.in, d.out, ...(d.options ?? []), ...(d.items ?? [])];
  if (d.options instanceof Map) children.push(...d.options.values());
  for (const c of children) if (c instanceof z.ZodType) makeStrict(c, seen);
}

export function defineTool<S extends ZodRawShape>(server: McpServer, ctx: ToolContext, def: ToolDefinition<S>): void {
  for (const v of Object.values(def.input)) makeStrict(v as z.ZodTypeAny);
  server.registerTool(
    def.name,
    {
      title: def.title,
      description: def.description,
      inputSchema: z.object(def.input).strict(),
      annotations: { title: def.title, openWorldHint: false, ...def.annotations },
    },
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    (async (args: any): Promise<CallToolResult> => {
      try {
        const result = await def.handler(args, ctx);
        if (isContent(result)) return result.result;
        if (isImage(result)) {
          const content: CallToolResult["content"] = [{ type: "image", data: result.data, mimeType: result.mimeType }];
          if (result.meta !== undefined) content.push({ type: "text", text: formatJson(result.meta) });
          return { content };
        }
        return { content: [{ type: "text", text: formatJson(result ?? { ok: true }) }] };
      } catch (err) {
        return errorResult(err);
      }
    }) as never,
  );
}

/** Annotation presets. */
export const READ_ONLY: ToolAnnotations = { readOnlyHint: true, destructiveHint: false, idempotentHint: true };
export const CREATES: ToolAnnotations = { readOnlyHint: false, destructiveHint: false, idempotentHint: false };
export const MODIFIES: ToolAnnotations = { readOnlyHint: false, destructiveHint: true, idempotentHint: true };
export const DESTRUCTIVE: ToolAnnotations = { readOnlyHint: false, destructiveHint: true, idempotentHint: false };
