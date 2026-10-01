/**
 * Minimal live-test harness: calls MCP tools of this server in-process against the running Archicad.
 *   npx tsx test/live/<family>.live.ts
 * Prints one line per check (PASS/FAIL) and a summary; exit code 1 when anything failed.
 */

import { mkdirSync, writeFileSync } from "node:fs";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";

import { createServer } from "../../src/server.js";

export interface CallResult {
  isError: boolean;
  text: string;
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  json: any;
  images: string[];
}

export class LiveTest {
  private client!: Client;
  static imageCounter = 0;
  passed = 0;
  failed = 0;
  failures: string[] = [];

  constructor(readonly name: string) {}

  async start(): Promise<this> {
    const { server } = createServer({ port: Number(process.env["ARCHICAD_PORT"] ?? 19723) });
    const [a, b] = InMemoryTransport.createLinkedPair();
    await server.connect(b);
    this.client = new Client({ name: "live", version: "1" });
    await this.client.connect(a);
    return this;
  }

  /** Calls a tool; returns the parsed result (never throws). */
  async raw(tool: string, args: Record<string, unknown> = {}): Promise<CallResult> {
    try {
      const res = await this.client.callTool({ name: tool, arguments: args }, undefined, { timeout: 600_000 });
      const blocks = res.content as Array<{ type: string; text?: string; data?: string; mimeType?: string }>;
      const text = blocks.filter((b) => b.type === "text").map((b) => b.text).join("\n");
      const images: string[] = [];
      for (const b of blocks) {
        if (b.type === "image" && b.data) {
          mkdirSync("test/.tmp", { recursive: true });
          const file = `test/.tmp/${this.name}-${tool}-${++LiveTest.imageCounter}.${b.mimeType?.includes("jpeg") ? "jpg" : "png"}`;
          writeFileSync(file, Buffer.from(b.data, "base64"));
          images.push(file);
        }
      }
      let json: unknown = undefined;
      try {
        json = JSON.parse(text);
      } catch {
        /* not JSON */
      }
      return { isError: Boolean(res.isError), text, json, images };
    } catch (e) {
      return { isError: true, text: `MCP error: ${e instanceof Error ? e.message : String(e)}`, json: undefined, images: [] };
    }
  }

  /** Calls a tool and records PASS when it succeeds (and `check` returns true / nothing). */
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  async ok(label: string, tool: string, args: Record<string, unknown> = {}, check?: (json: any, r: CallResult) => boolean | string | void): Promise<any> {
    const r = await this.raw(tool, args);
    if (r.isError) {
      this.fail(label, `${tool} error: ${r.text.slice(0, 400)}`);
      return r.json;
    }
    const itemErrors = collectItemErrors(r.json);
    let verdict: boolean | string | void = true;
    try {
      verdict = check ? check(r.json, r) : itemErrors.length ? `item errors: ${itemErrors.join(" | ").slice(0, 400)}` : true;
    } catch (e) {
      verdict = `check threw: ${e instanceof Error ? e.message : String(e)}`;
    }
    if (verdict === false || typeof verdict === "string") this.fail(label, typeof verdict === "string" ? verdict : `unexpected result: ${r.text.slice(0, 400)}`);
    else this.pass(label);
    return r.json;
  }

  /** Expects the call to fail (tool error or per-item error) — for negative tests. */
  async fails(label: string, tool: string, args: Record<string, unknown>, mustContain?: RegExp): Promise<void> {
    const r = await this.raw(tool, args);
    const errs = r.isError ? [r.text] : collectItemErrors(r.json);
    if (errs.length === 0) return this.fail(label, `expected an error, got: ${r.text.slice(0, 300)}`);
    if (mustContain && !errs.some((e) => mustContain.test(e))) return this.fail(label, `error text mismatch: ${errs.join(" | ").slice(0, 300)}`);
    this.pass(label);
  }

  check(label: string, cond: boolean, detail = ""): void {
    if (cond) this.pass(label);
    else this.fail(label, detail);
  }

  pass(label: string): void {
    this.passed++;
    console.log(`  PASS ${label}`);
  }

  fail(label: string, detail: string): void {
    this.failed++;
    this.failures.push(`${label}: ${detail}`);
    console.log(`  FAIL ${label} — ${detail}`);
  }

  async finish(): Promise<void> {
    console.log(`== ${this.name}: ${this.passed} passed, ${this.failed} failed`);
    await this.client.close();
    process.exitCode = this.failed ? 1 : 0;
  }
}

/** Collects {error: {message}} items from typical batch results ({results:[...]}, {elements:[...]}, ...). */
export function collectItemErrors(json: unknown): string[] {
  const out: string[] = [];
  const visit = (v: unknown, depth: number) => {
    if (depth > 3 || v === null || typeof v !== "object") return;
    if (Array.isArray(v)) {
      for (const x of v) visit(x, depth + 1);
      return;
    }
    const o = v as Record<string, unknown>;
    if (o["error"] && typeof o["error"] === "object") {
      const e = o["error"] as { message?: string };
      out.push(e.message ?? JSON.stringify(e));
      return;
    }
    for (const k of ["results", "elements", "items", "created", "stories", "attributes"]) if (k in o) visit(o[k], depth + 1);
  };
  visit(json, 0);
  return out;
}

/** GUIDs from a {results:[{guid}]} batch result, in order (undefined for failed items). */
export function guids(json: { results?: Array<{ guid?: string }> } | undefined): string[] {
  return (json?.results ?? []).map((r) => r.guid as string);
}

export const near = (a: number, b: number, eps = 1e-3) => Math.abs(a - b) <= eps;
