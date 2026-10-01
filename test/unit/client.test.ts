import { describe, expect, it } from "vitest";

import { ArchicadClient, ArchicadConnectionError } from "../../src/archicad/client.js";

describe("ArchicadClient", () => {
  it("reports a blocking modal dialog when an add-on command times out", async () => {
    const fetchImpl = (async (_url: string, init?: { body?: string; signal?: AbortSignal }) => {
      const body = JSON.parse(init?.body ?? "{}") as { command: string };
      if (body.command === "API.IsAlive") {
        return new Response(JSON.stringify({ succeeded: false, error: { code: 4001, message: "Invalid program status (there is an open modal dialog: Предостережение!)" } }));
      }
      // add-on commands wait while the dialog is open
      return new Promise<Response>((_resolve, reject) => {
        init?.signal?.addEventListener("abort", () => reject(Object.assign(new Error("aborted"), { name: "TimeoutError" })));
      });
    }) as unknown as typeof fetch;
    const ac = new ArchicadClient({ port: 19723, timeoutMs: 200, fetchImpl });
    const err = await ac.addon("Ping").catch((e: unknown) => e);
    expect(err).toBeInstanceOf(ArchicadConnectionError);
    expect(String((err as Error).message)).toContain("Предостережение!");
    expect(await ac.modalDialog()).toBe("Предостережение!");
  });

  it("returns undefined when no dialog is open", async () => {
    const fetchImpl = (async () => new Response(JSON.stringify({ succeeded: true, result: { isAlive: true } }))) as unknown as typeof fetch;
    const ac = new ArchicadClient({ port: 19723, fetchImpl });
    expect(await ac.modalDialog()).toBeUndefined();
  });
});
