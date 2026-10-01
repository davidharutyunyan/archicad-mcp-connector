import { describe, expect, it } from "vitest";

import { addonOk, harness } from "../helpers.js";

const G1 = "92F5E7F6-2460-FF4F-A499-0B07286D2B5F";
const G2 = "0A1B2C3D-4E5F-6071-8293-A4B5C6D7E8F9";

describe("create_zones", () => {
  it("sends a manual zone (polygon) to CreateElements with type Zone", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G1, type: "Zone" }] }));
    const res = await h.call("create_zones", {
      zones: [
        {
          polygon: [
            { x: 0, y: 0 },
            { x: 5, y: 0 },
            { x: 5, y: 4 },
            { x: 0, y: 4 },
          ],
          name: "Кухня",
          number: 101,
          category: "Жилая зона",
          height: 2.8,
          stampPosition: { x: 2.5, y: 2 },
          stampParameters: { gs_text_size: 2.5 },
          storyIndex: 1,
        },
      ],
    });
    expect(res.isError).toBe(false);
    expect(h.requests).toHaveLength(1);
    expect(h.requests[0]?.addOnCommand).toBe("CreateElements");
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [
        {
          type: "Zone",
          polygon: [
            { x: 0, y: 0 },
            { x: 5, y: 0 },
            { x: 5, y: 4 },
            { x: 0, y: 4 },
          ],
          name: "Кухня",
          number: "101",
          category: "Жилая зона",
          height: 2.8,
          stampPosition: { x: 2.5, y: 2 },
          stampParameters: { gs_text_size: 2.5 },
          storyIndex: 1,
        },
      ],
      undoName: "Create zones (Claude)",
    });
    expect(res.json).toEqual({ results: [{ guid: G1, type: "Zone" }] });
  });

  it("sends an automatic zone (referencePoint + boundary method)", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G1, type: "Zone" }] }));
    const res = await h.call("create_zones", {
      zones: [{ referencePoint: { x: 1, y: 1 }, boundary: "ReferenceLine", name: "Hall", number: "1.02", stamp: { name: "Штамп зоны 26" } }],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toMatchObject({
      elements: [{ type: "Zone", referencePoint: { x: 1, y: 1 }, boundary: "ReferenceLine", name: "Hall", number: "1.02", stamp: { name: "Штамп зоны 26" } }],
    });
  });

  it("accepts a polygon with holes and arcs", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G1, type: "Zone" }] }));
    const polygon = {
      points: [
        { x: 0, y: 0 },
        { x: 10, y: 0 },
        { x: 10, y: 8 },
        { x: 0, y: 8 },
      ],
      arcs: [{ index: 1, angle: 45 }],
      holes: [
        {
          points: [
            { x: 4, y: 3 },
            { x: 6, y: 3 },
            { x: 6, y: 5 },
          ],
        },
      ],
    };
    const res = await h.call("create_zones", { zones: [{ polygon }] });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toMatchObject({ elements: [{ type: "Zone", polygon }] });
  });

  it("requires exactly one of polygon / referencePoint", async () => {
    const h = await harness(() => addonOk({}));
    const neither = await h.call("create_zones", { zones: [{ name: "No geometry" }] });
    expect(neither.isError).toBe(true);
    expect(neither.text).toMatch(/polygon|referencePoint/);
    const both = await h.call("create_zones", {
      zones: [
        {
          name: "Both",
          referencePoint: { x: 1, y: 1 },
          polygon: [
            { x: 0, y: 0 },
            { x: 1, y: 0 },
            { x: 1, y: 1 },
          ],
        },
      ],
    });
    expect(both.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("validates ranges and enums before calling Archicad", async () => {
    const h = await harness(() => addonOk({}));
    const bad = [
      { referencePoint: { x: 0, y: 0 }, areaReduction: 150 },
      { referencePoint: { x: 0, y: 0 }, height: -1 },
      { referencePoint: { x: 0, y: 0 }, boundary: "Outside" },
      { referencePoint: { x: 0, y: 0 }, stampPen: 0 },
      { referencePoint: { x: 0, y: 0 }, number: "x".repeat(40) },
    ];
    for (const zone of bad) {
      const res = await h.call("create_zones", { zones: [zone] });
      expect(res.isError, JSON.stringify(zone)).toBe(true);
    }
    expect(h.requests).toHaveLength(0);
  });

  it("surfaces per-item add-on errors (e.g. no closed boundary around the reference point)", async () => {
    const message = "Cannot create automatic zone (APIERR_GENERAL). Archicad found no closed boundary around the reference point (1.000, 1.000) on story 0.";
    const h = await harness(() => addonOk({ results: [{ error: { code: -2130313215, message } }] }));
    const res = await h.call("create_zones", { zones: [{ referencePoint: { x: 1, y: 1 } }] });
    expect(res.isError).toBe(false);
    expect(res.json).toEqual({ results: [{ error: { code: -2130313215, message } }] });
  });

  it("publishes a JSON schema that documents both geometry modes", async () => {
    const h = await harness(() => addonOk({}));
    const tools = await h.listTools();
    const names = tools.map((t) => t.name);
    for (const n of ["create_zones", "modify_zones", "update_zones", "get_zones"]) expect(names).toContain(n);
    const create = tools.find((t) => t.name === "create_zones")!;
    const schemaText = JSON.stringify(create.inputSchema);
    expect(schemaText).toContain("referencePoint");
    expect(schemaText).toContain("polygon");
    expect(schemaText).toContain("stampParameters");
    expect(create.annotations?.readOnlyHint).toBe(false);
  });
});

describe("modify_zones", () => {
  it("sends patches to ModifyElements and relocates zones whose referencePoint changes", async () => {
    const G3 = "33333333-3333-3333-3333-333333333333";
    const h = await harness((req) =>
      req.addOnCommand === "RelocateZones"
        ? addonOk({ results: [{ guid: G2, newGuid: G3, status: "recreated", relocated: true }] })
        : addonOk({ results: [{ guid: G1 }, { guid: G3 }] }),
    );
    const res = await h.call("modify_zones", {
      zones: [
        { guid: G1, name: "Спальня", number: 7, height: 3, category: { index: 2 } },
        { guid: G2, referencePoint: { x: 3, y: 4 }, stamp: "Zone Stamp 26", stampParameters: { showArea: true } },
      ],
    });
    expect(res.isError).toBe(false);
    const relocate = h.requests.find((r) => r.addOnCommand === "RelocateZones");
    expect(relocate?.addOnParameters).toEqual({ zones: [{ guid: G2, referencePoint: { x: 3, y: 4 } }] });
    const modify = h.requests.find((r) => r.addOnCommand === "ModifyElements");
    expect(modify?.addOnParameters).toEqual({
      elements: [
        { guid: G1, name: "Спальня", number: "7", height: 3, category: { index: 2 } },
        { guid: G3, stamp: "Zone Stamp 26", stampParameters: { showArea: true } },
      ],
      undoName: "Modify zones (Claude)",
    });
    expect((res.json as any).results[1]).toMatchObject({ guid: G2, newGuid: G3, relocated: true });
  });

  it("supports freezing an automatic zone", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G1 }] }));
    const res = await h.call("modify_zones", { zones: [{ guid: G1, automatic: false }] });
    expect(res.isError).toBe(false);
    expect(h.requests.find((r) => r.addOnCommand === "ModifyElements")?.addOnParameters).toMatchObject({ elements: [{ guid: G1, automatic: false }] });
  });

  it("rejects polygon + referencePoint together and invalid guids", async () => {
    const h = await harness(() => addonOk({}));
    const both = await h.call("modify_zones", {
      zones: [
        {
          guid: G1,
          referencePoint: { x: 1, y: 1 },
          polygon: [
            { x: 0, y: 0 },
            { x: 1, y: 0 },
            { x: 1, y: 1 },
          ],
        },
      ],
    });
    expect(both.isError).toBe(true);
    const badGuid = await h.call("modify_zones", { zones: [{ guid: "not-a-guid", name: "x" }] });
    expect(badGuid.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("update_zones", () => {
  it("updates every automatic zone by default", async () => {
    const response = { results: [], summary: { processed: 0 }, dryRun: false, method: "copyBoundary" };
    const h = await harness(() => addonOk(response));
    const res = await h.call("update_zones", {});
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("UpdateZones");
    expect(h.requests[0]?.addOnParameters).toEqual({});
    expect(res.json).toEqual(response);
  });

  it("passes zones (normalized to GUID strings), stories, dryRun and method", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G1, status: "outdated" }], summary: { outdated: 1 } }));
    const res = await h.call("update_zones", { zones: [G1, { guid: G2 }], stories: [0, 1], dryRun: true, method: "recreate" });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({ zones: [G1, G2], stories: [0, 1], dryRun: true, method: "recreate" });
  });

  it("rejects an unknown method", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("update_zones", { method: "magic" });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("surfaces add-on errors (e.g. old add-on build without the command)", async () => {
    const h = await harness(() => addonOk({ error: { code: -2130313112, message: "Unknown command UpdateZones" } }));
    const res = await h.call("update_zones", {});
    expect(res.isError).toBe(true);
    expect(res.text).toContain("UpdateZones");
  });
});

describe("get_zones", () => {
  it("sends filters to GetZones", async () => {
    const response = {
      zones: [{ guid: G1, name: "Кухня", number: "101", area: 20, netArea: 19.5, calculatedArea: 19.5, volume: 56 }],
      total: 1,
      offset: 0,
      hasMore: false,
      totals: { count: 1, area: 20, netArea: 19.5, calculatedArea: 19.5, volume: 56 },
    };
    const h = await harness(() => addonOk(response));
    const res = await h.call("get_zones", {
      zones: [{ guid: G1 }],
      stories: [0],
      category: "Жилая зона",
      search: "кух",
      includePolygon: true,
      includeRelations: true,
      includeReductions: true,
      includeQuantities: true,
      offset: 0,
      limit: 50,
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("GetZones");
    expect(h.requests[0]?.addOnParameters).toEqual({
      zones: [G1],
      stories: [0],
      category: "Жилая зона",
      search: "кух",
      includePolygon: true,
      includeRelations: true,
      includeReductions: true,
      includeQuantities: true,
      offset: 0,
      limit: 50,
    });
    expect(res.json).toEqual(response);
  });

  it("works without arguments and is read-only", async () => {
    const h = await harness(() => addonOk({ zones: [], total: 0, offset: 0, hasMore: false, totals: { count: 0 } }));
    const res = await h.call("get_zones", {});
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({});
    const tool = (await h.listTools()).find((t) => t.name === "get_zones");
    expect(tool?.annotations?.readOnlyHint).toBe(true);
  });

  it("rejects an invalid limit", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("get_zones", { limit: 0 });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});
