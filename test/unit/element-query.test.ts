import { describe, expect, it } from "vitest";

import { addonOk, harness } from "../helpers.js";

const G1 = "92F5E7F6-2460-FF4F-A499-0B07286D2B5F";
const G2 = "0A1B2C3D-4E5F-6071-8293-A4B5C6D7E8F9";

const TOOLS = [
  "find_elements",
  "get_element_counts",
  "get_element_quantities",
  "get_connected_elements",
  "get_element_relations",
  "get_subelements",
  "get_selection",
  "set_selection",
  "get_element_2d_geometry",
  "get_element_3d_geometry",
];

describe("element-query tool registration", () => {
  it("registers every tool of the family with a description", async () => {
    const h = await harness(() => addonOk({}));
    const tools = await h.listTools();
    for (const name of TOOLS) {
      const tool = tools.find((t) => t.name === name);
      expect(tool, name).toBeDefined();
      expect(tool!.description!.length).toBeGreaterThan(80);
    }
    const find = tools.find((t) => t.name === "find_elements")!;
    expect(find.annotations?.readOnlyHint).toBe(true);
    const set = tools.find((t) => t.name === "set_selection")!;
    expect(set.annotations?.readOnlyHint).toBe(false);
  });
});

describe("find_elements", () => {
  it("sends all filters to FindElements and normalizes element references", async () => {
    const h = await harness(() => addonOk({ total: 1, offset: 0, returned: 1, hasMore: false, elements: [{ guid: G1, type: "Wall" }] }));
    const res = await h.call("find_elements", {
      types: ["Wall", "Column"],
      excludeTypes: ["Door"],
      storyIndex: 0,
      stories: [1, "2. Этаж", { name: "Кровля" }],
      layers: ["Конструктив - Стены Несущие", 3],
      renovationStatus: ["New"],
      filters: ["OnVisibleLayer", "Editable"],
      withinElements: [G1, { guid: G2 }],
      elementId: "W-*",
      libraryPart: "Стул",
      grouped: false,
      locked: false,
      region: { xMin: 0, yMin: 0, xMax: 10, yMax: 8, mode: "Inside" },
      includeBoundingBox: true,
      offset: 10,
      limit: 20,
    });
    expect(res.isError).toBe(false);
    expect(h.requests).toHaveLength(1);
    expect(h.requests[0]?.addOnCommand).toBe("FindElements");
    expect(h.requests[0]?.addOnParameters).toEqual({
      types: ["Wall", "Column"],
      excludeTypes: ["Door"],
      storyIndex: 0,
      stories: [1, "2. Этаж", { name: "Кровля" }],
      layers: ["Конструктив - Стены Несущие", 3],
      renovationStatus: ["New"],
      filters: ["OnVisibleLayer", "Editable"],
      withinElements: [G1, G2],
      elementId: "W-*",
      libraryPart: "Стул",
      grouped: false,
      locked: false,
      region: { xMin: 0, yMin: 0, xMax: 10, yMax: 8, mode: "Inside" },
      includeBoundingBox: true,
      offset: 10,
      limit: 20,
    });
    expect(res.json).toMatchObject({ total: 1, elements: [{ guid: G1, type: "Wall" }] });
  });

  it("sends an empty parameter object when no filter is given", async () => {
    const h = await harness(() => addonOk({ total: 0, elements: [] }));
    const res = await h.call("find_elements", {});
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({});
  });

  it("rejects unknown element types and filters before calling Archicad", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("find_elements", { types: ["Walls"] })).isError).toBe(true);
    expect((await h.call("find_elements", { filters: ["Visible"] })).isError).toBe(true);
    expect((await h.call("find_elements", { region: { xMin: 0, mode: "Touching" } })).isError).toBe(true);
    expect((await h.call("find_elements", { limit: 0 })).isError).toBe(true);
    expect((await h.call("find_elements", { withinElements: ["not-a-guid"] })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("surfaces add-on errors as tool errors", async () => {
    const h = await harness(() => addonOk({ error: { code: -2130313112, message: "Story named 'X' not found." } }));
    const res = await h.call("find_elements", { storyIndex: "X" });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("Story named 'X' not found");
  });
});

describe("get_element_counts", () => {
  it("passes filters and groupBy", async () => {
    const h = await harness(() => addonOk({ total: 3, byType: { Wall: 2, Slab: 1 } }));
    const res = await h.call("get_element_counts", { groupBy: ["story", "layer"], filters: ["In3D"], withinElements: [{ guid: G1 }] });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("GetElementCounts");
    expect(h.requests[0]?.addOnParameters).toEqual({ groupBy: ["story", "layer"], filters: ["In3D"], withinElements: [G1] });
    expect(res.json).toEqual({ total: 3, byType: { Wall: 2, Slab: 1 } });
  });

  it("rejects an unknown groupBy value", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("get_element_counts", { groupBy: ["color"] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("get_element_quantities", () => {
  it("sends GUID strings and options", async () => {
    const h = await harness(() =>
      addonOk({ elements: [{ guid: G1, type: "Wall", quantities: { volume: 1.2345678912 } }], totals: { Wall: { count: 1, volume: 1.2345678912 } } }),
    );
    const res = await h.call("get_element_quantities", {
      elements: [{ guid: G1 }, G2],
      includeParts: true,
      minOpeningSize: 0.5,
      includeExposedSurfaces: true,
      coverElements: [{ guid: G2 }],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("GetElementQuantities");
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [G1, G2],
      coverElements: [G2],
      includeParts: true,
      minOpeningSize: 0.5,
      includeExposedSurfaces: true,
    });
    // numbers are rounded for readability
    expect(res.json).toMatchObject({ elements: [{ quantities: { volume: 1.234568 } }] });
  });

  it("requires at least one element", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("get_element_quantities", { elements: [] })).isError).toBe(true);
    expect((await h.call("get_element_quantities", {})).isError).toBe(true);
    expect((await h.call("get_element_quantities", { elements: [G1], minOpeningSize: -1 })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("passes per-element errors through unchanged", async () => {
    const h = await harness(() => addonOk({ elements: [{ guid: G1, error: { code: -1, message: "Quantities are not available for this Line" } }] }));
    const res = await h.call("get_element_quantities", { elements: [G1] });
    expect(res.isError).toBe(false);
    expect(res.text).toContain("Quantities are not available");
  });
});

describe("get_connected_elements / get_element_relations / get_subelements", () => {
  it("get_connected_elements builds the request", async () => {
    const h = await harness(() => addonOk({ elements: [{ guid: G1, type: "Wall", connected: { Door: [G2] } }] }));
    const res = await h.call("get_connected_elements", {
      elements: [{ guid: G1 }],
      types: ["Door", "Window"],
      includeSolidOperations: true,
      includeTypes: true,
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("GetConnectedElements");
    expect(h.requests[0]?.addOnParameters).toEqual({ elements: [G1], types: ["Door", "Window"], includeSolidOperations: true, includeTypes: true });
  });

  it("get_element_relations builds the request", async () => {
    const h = await harness(() => addonOk({ elements: [{ guid: G1, type: "Zone", relationKind: "Zone" }] }));
    const res = await h.call("get_element_relations", { elements: [G1, G2], includePolygons: false });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("GetElementRelations");
    expect(h.requests[0]?.addOnParameters).toEqual({ elements: [G1, G2], includePolygons: false });
  });

  it("get_subelements only accepts sub-element types in 'types'", async () => {
    const h = await harness(() => addonOk({ elements: [] }));
    expect((await h.call("get_subelements", { elements: [G1], types: ["Wall"] })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
    const res = await h.call("get_subelements", { elements: [G1], types: ["CurtainWallPanel", "Tread"], maxPerType: 50 });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("GetSubelements");
    expect(h.requests[0]?.addOnParameters).toEqual({ elements: [G1], types: ["CurtainWallPanel", "Tread"], maxPerType: 50 });
  });
});

describe("selection tools", () => {
  it("get_selection passes options", async () => {
    const h = await harness(() => addonOk({ selectionType: "Elements", total: 1, elements: [{ guid: G1 }] }));
    const res = await h.call("get_selection", { onlyEditable: true, marqueeRelation: "InsideEntirely", limit: 10 });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("GetSelection");
    expect(h.requests[0]?.addOnParameters).toEqual({ onlyEditable: true, marqueeRelation: "InsideEntirely", limit: 10 });
  });

  it("set_selection defaults to mode 'set' and sends GUID strings", async () => {
    const h = await harness(() => addonOk({ mode: "set", requested: 2, applied: 2, failed: [], selectionCount: 2 }));
    const res = await h.call("set_selection", { elements: [G1, { guid: G2 }] });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("SetSelection");
    expect(h.requests[0]?.addOnParameters).toEqual({ mode: "set", elements: [G1, G2] });
  });

  it("set_selection clear sends no elements", async () => {
    const h = await harness(() => addonOk({ mode: "clear", requested: 0, applied: 0, failed: [], selectionCount: 0 }));
    const res = await h.call("set_selection", { mode: "clear", elements: [G1] });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({ mode: "clear" });
  });

  it("set_selection add/remove without elements fails before calling Archicad", async () => {
    const h = await harness(() => addonOk({}));
    const add = await h.call("set_selection", { mode: "add" });
    expect(add.isError).toBe(true);
    expect(add.text).toContain("needs 'elements'");
    expect((await h.call("set_selection", { mode: "remove", elements: [] })).isError).toBe(true);
    expect((await h.call("set_selection", { mode: "toggle", elements: [G1] })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("geometry tools", () => {
  it("get_element_2d_geometry builds the request", async () => {
    const h = await harness(() => addonOk({ elements: [{ guid: G1, type: "Door", total: 5, counts: { line: 4, arc: 1 } }] }));
    const res = await h.call("get_element_2d_geometry", { elements: [{ guid: G1 }], kinds: ["line", "arc"], maxPrimitives: 50, includeHotspots: true });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("GetElement2DGeometry");
    expect(h.requests[0]?.addOnParameters).toEqual({ elements: [G1], kinds: ["line", "arc"], maxPrimitives: 50, includeHotspots: true });
  });

  it("get_element_2d_geometry rejects unknown primitive kinds", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("get_element_2d_geometry", { elements: [G1], kinds: ["spline"] })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("get_element_3d_geometry builds the request and validates mode", async () => {
    const h = await harness(() => addonOk({ elements: [{ guid: G1, type: "Wall", bodyCount: 1 }] }));
    const res = await h.call("get_element_3d_geometry", { elements: [G1], mode: "mesh", maxVertices: 100, includeNormals: true });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("GetElement3DGeometry");
    expect(h.requests[0]?.addOnParameters).toEqual({ elements: [G1], mode: "mesh", maxVertices: 100, includeNormals: true });
    expect((await h.call("get_element_3d_geometry", { elements: [G1], mode: "full" })).isError).toBe(true);
    expect(h.requests).toHaveLength(1);
  });
});
