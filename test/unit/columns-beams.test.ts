import { describe, expect, it } from "vitest";

import { addonOk, apiOk, harness, type RecordedRequest } from "../helpers.js";

const G1 = "92F5E7F6-2460-FF4F-A499-0B07286D2B5F";
const G2 = "1A2B3C4D-1111-2222-3333-444455556666";
const G3 = "0F0E0D0C-AAAA-BBBB-CCCC-DDDDEEEEFFFF";

describe("create_columns", () => {
  it("sends typed column specs (with segments and cuts) to CreateElements", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G1, type: "Column" }] }));
    const column = {
      origin: { x: 1, y: 2 },
      height: 3,
      width: 0.4,
      depth: 0.3,
      buildingMaterial: "Бетон",
      anchor: "Center",
      rotationAngle: 45,
      showOnStories: "Auto",
      segments: [{ length: 1 }, { lengthProportion: 1, diameter: 0.3 }],
      cuts: [{}, { type: "Custom", angle: 30 }, {}],
      storyIndex: 1,
    };
    const res = await h.call("create_columns", { columns: [column] });
    expect(res.isError).toBe(false);
    expect(h.requests).toHaveLength(1);
    expect(h.requests[0]?.addOnCommand).toBe("CreateElements");
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [{ type: "Column", ...column }],
      undoName: "Create columns (Claude)",
    });
    expect(res.json).toEqual({ results: [{ guid: G1, type: "Column" }] });
  });

  it("passes a custom undo name and accepts numeric anchors and circular columns", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G1, type: "Column" }, { guid: G2, type: "Column" }] }));
    const res = await h.call("create_columns", {
      columns: [
        { origin: { x: 0, y: 0 }, diameter: 0.5, topLinkedStory: 1, topOffset: -0.25, anchor: 4 },
        { origin: { x: 5, y: 0 }, slantAngle: 60, slantDirection: 90, veneerThickness: 0.02, veneerType: "Finish" },
      ],
      undoName: "Grid columns",
    });
    expect(res.isError).toBe(false);
    const params = h.requests[0]?.addOnParameters as { elements: Array<Record<string, unknown>>; undoName: string };
    expect(params.undoName).toBe("Grid columns");
    expect(params.elements[0]).toMatchObject({ type: "Column", diameter: 0.5, topLinkedStory: 1, anchor: 4 });
    expect(params.elements[1]).toMatchObject({ type: "Column", slantAngle: 60, veneerType: "Finish" });
  });

  it.each([
    ["missing origin", { height: 3 }],
    ["both buildingMaterial and profile", { origin: { x: 0, y: 0 }, buildingMaterial: "A", profile: "B" }],
    ["slant angle out of range", { origin: { x: 0, y: 0 }, slantAngle: 200 }],
    ["diameter with rectangular shape", { origin: { x: 0, y: 0 }, shape: "Rectangular", diameter: 0.3 }],
    ["segment with length and proportion", { origin: { x: 0, y: 0 }, segments: [{ length: 1, lengthProportion: 0.5 }] }],
    ["negative height", { origin: { x: 0, y: 0 }, height: -1 }],
    ["invalid anchor", { origin: { x: 0, y: 0 }, anchor: 9 }],
    ["invalid veneer type", { origin: { x: 0, y: 0 }, veneerType: "Skin" }],
  ])("rejects invalid input before calling Archicad: %s", async (_name, column) => {
    const h = await harness(() => addonOk({ results: [] }));
    const res = await h.call("create_columns", { columns: [column] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("surfaces add-on errors as tool errors", async () => {
    const h = await harness(() => addonOk({ error: { code: -2130313112, message: "Column requires 'origin' {x, y}" } }));
    const res = await h.call("create_columns", { columns: [{ origin: { x: 0, y: 0 } }] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("Column requires 'origin'");
  });

  it("returns per-item errors unchanged", async () => {
    const h = await harness(() =>
      addonOk({ results: [{ guid: G1, type: "Column" }, { error: { code: -2130313112, message: "segments[1]: 'width' must be greater than 0" } }] }),
    );
    const res = await h.call("create_columns", { columns: [{ origin: { x: 0, y: 0 } }, { origin: { x: 1, y: 0 } }] });
    expect(res.isError).toBe(false);
    expect(res.json).toEqual({
      results: [{ guid: G1, type: "Column" }, { error: { code: -2130313112, message: "segments[1]: 'width' must be greater than 0" } }],
    });
  });
});

describe("create_beams", () => {
  it("sends typed beam specs (holes, segments, curve) to CreateElements", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G1, type: "Beam" }] }));
    const beam = {
      begin: { x: 0, y: 0 },
      end: { x: 6, y: 0 },
      level: 3,
      width: 0.3,
      height: 0.5,
      arcAngle: 30,
      profileRotationAngle: 10,
      topSurface: "Штукатурка",
      endsSurface: false,
      holes: [{ distanceFromBegin: 1.5, diameter: 0.1 }, { distanceFromBegin: 3, shape: "Rectangular", width: 0.2, height: 0.1, depthBelowTop: 0.25 }],
      segments: [{}, { tapered: true, endHeight: 0.3 }],
    };
    const res = await h.call("create_beams", { beams: [beam] });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("CreateElements");
    expect(h.requests[0]?.addOnParameters).toEqual({ elements: [{ type: "Beam", ...beam }], undoName: "Create beams (Claude)" });
  });

  it.each([
    ["missing end", { begin: { x: 0, y: 0 } }],
    ["both curve kinds", { begin: { x: 0, y: 0 }, end: { x: 1, y: 0 }, arcAngle: 20, verticalCurveHeight: 0.5 }],
    ["hole without position", { begin: { x: 0, y: 0 }, end: { x: 1, y: 0 }, holes: [{ width: 0.1 }] }],
    ["slant out of range", { begin: { x: 0, y: 0 }, end: { x: 1, y: 0 }, slantAngle: 95 }],
    ["sequence out of range", { begin: { x: 0, y: 0 }, end: { x: 1, y: 0 }, sequence: 1000 }],
    ["cuts with one item", { begin: { x: 0, y: 0 }, end: { x: 1, y: 0 }, cuts: [{}] }],
  ])("rejects invalid input before calling Archicad: %s", async (_name, beam) => {
    const h = await harness(() => addonOk({ results: [] }));
    const res = await h.call("create_beams", { beams: [beam] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("rejects an invalid pen in the line settings", async () => {
    const h = await harness(() => addonOk({ results: [] }));
    const res = await h.call("create_beams", { beams: [{ begin: { x: 0, y: 0 }, end: { x: 1, y: 0 }, lines: { reference: { pen: 300 } } }] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("allows a straight beam with arcAngle 0 and verticalCurveHeight 0", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G1, type: "Beam" }] }));
    const res = await h.call("create_beams", { beams: [{ begin: { x: 0, y: 0 }, end: { x: 1, y: 0 }, arcAngle: 0, verticalCurveHeight: 0 }] });
    expect(res.isError).toBe(false);
    expect(h.requests).toHaveLength(1);
  });
});

function typesResponder(types: Record<string, string>, modifyResults: (req: RecordedRequest) => unknown) {
  return (req: RecordedRequest) => {
    if (req.command === "API.GetTypesOfElements") {
      const elements = (req.parameters?.["elements"] ?? []) as Array<{ elementId: { guid: string } }>;
      return apiOk({
        typesOfElements: elements.map((e) =>
          types[e.elementId.guid]
            ? { typeOfElement: { elementId: e.elementId, elementType: types[e.elementId.guid] } }
            : { error: { code: -2130313111, message: "The referenced element does not exist." } },
        ),
      });
    }
    if (req.addOnCommand === "ModifyElements") return addonOk(modifyResults(req));
    return { succeeded: false, error: { code: 1, message: `unexpected ${req.command}` } };
  };
}

describe("modify_columns", () => {
  it("checks element types, modifies only columns and keeps the input order", async () => {
    const h = await harness(
      typesResponder({ [G1]: "Column", [G2]: "Wall" }, (req) => ({
        results: ((req.addOnParameters?.["elements"] ?? []) as Array<{ guid: string }>).map((e) => ({ guid: e.guid })),
      })),
    );
    const res = await h.call("modify_columns", {
      columns: [
        { guid: G2, height: 2.5 },
        { guid: G1, height: 2.5, segments: [{}, { width: 0.5 }] },
        { guid: G3, height: 2.5 },
      ],
    });
    expect(res.isError).toBe(false);
    expect(h.requests.map((r) => r.command)).toEqual(["API.GetTypesOfElements", "API.ExecuteAddOnCommand"]);
    expect(h.requests[0]?.parameters).toEqual({
      elements: [{ elementId: { guid: G2 } }, { elementId: { guid: G1 } }, { elementId: { guid: G3 } }],
    });
    expect(h.requests[1]?.addOnParameters).toEqual({
      elements: [{ guid: G1, height: 2.5, segments: [{}, { width: 0.5 }] }],
      undoName: "Modify columns (Claude)",
    });
    const results = (res.json as { results: Array<Record<string, unknown>> }).results;
    expect(results).toHaveLength(3);
    expect(results[0]).toMatchObject({ guid: G2 });
    expect(JSON.stringify(results[0])).toContain("is a Wall, not a Column");
    expect(results[1]).toEqual({ guid: G1 });
    expect(JSON.stringify(results[2])).toContain("not found");
  });

  it("does not call ModifyElements when no item is a column", async () => {
    const h = await harness(typesResponder({ [G1]: "Beam" }, () => ({ results: [] })));
    const res = await h.call("modify_columns", { columns: [{ guid: G1, width: 0.3 }] });
    expect(res.isError).toBe(false);
    expect(h.requests).toHaveLength(1);
    expect(JSON.stringify(res.json)).toContain("is a Beam, not a Column");
  });

  it("validates patch fields like create_columns before calling Archicad", async () => {
    const h = await harness(typesResponder({ [G1]: "Column" }, () => ({ results: [{ guid: G1 }] })));
    const res = await h.call("modify_columns", { columns: [{ guid: G1, slantAngle: 270 }] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("slantAngle");
    expect(h.requests).toHaveLength(0);
  });

  it("passes floor plan attributes (lines, cover fill, pen overrides)", async () => {
    const h = await harness(typesResponder({ [G1]: "Column" }, () => ({ results: [{ guid: G1 }] })));
    const patch = {
      guid: G1,
      lines: { contour: { pen: 3, lineType: "Сплошная линия" }, symbol: { pen: 5 } },
      coverFill: { fill: 12, pen: 2 },
      cutFillPen: false,
      cutFillBackgroundPen: 19,
      showOnStories: { homeStory: true, storiesAbove: 1 },
    };
    const res = await h.call("modify_columns", { columns: [patch] });
    expect(res.isError).toBe(false);
    expect(h.requests[1]?.addOnParameters).toEqual({ elements: [patch], undoName: "Modify columns (Claude)" });
  });

  it("rejects a patch without guid", async () => {
    const h = await harness(() => addonOk({ results: [] }));
    const res = await h.call("modify_columns", { columns: [{ height: 3 }] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("modify_beams", () => {
  it("sends hole edits for beams", async () => {
    const h = await harness(typesResponder({ [G1]: "Beam" }, () => ({ results: [{ guid: G1 }] })));
    const res = await h.call("modify_beams", {
      beams: [{ guid: G1, addHoles: [{ distanceFromBegin: 2, diameter: 0.15 }], removeHoles: [1] }],
      undoName: "Holes",
    });
    expect(res.isError).toBe(false);
    expect(h.requests[1]?.addOnCommand).toBe("ModifyElements");
    expect(h.requests[1]?.addOnParameters).toEqual({
      elements: [{ guid: G1, addHoles: [{ distanceFromBegin: 2, diameter: 0.15 }], removeHoles: [1] }],
      undoName: "Holes",
    });
    expect(res.json).toEqual({ results: [{ guid: G1 }] });
  });

  it("falls back to the add-on validation when the type check is unavailable", async () => {
    const h = await harness((req) => {
      if (req.command === "API.GetTypesOfElements") return { succeeded: false, error: { code: 4011, message: "Unknown command" } };
      return addonOk({ results: [{ guid: G1 }, { guid: G2 }] });
    });
    const res = await h.call("modify_beams", { beams: [{ guid: G1, level: 2.7 }, { guid: G2, sequence: 5 }] });
    expect(res.isError).toBe(false);
    expect(h.requests[1]?.addOnParameters).toMatchObject({ elements: [{ guid: G1, level: 2.7 }, { guid: G2, sequence: 5 }] });
    expect(res.json).toEqual({ results: [{ guid: G1 }, { guid: G2 }] });
  });

  it("surfaces a failing ModifyElements call as a tool error", async () => {
    const h = await harness((req) => {
      if (req.command === "API.GetTypesOfElements") return apiOk({ typesOfElements: [{ typeOfElement: { elementType: "Beam" } }] });
      return addonOk({ error: { code: -2130313112, message: "Undo scope failed" } });
    });
    const res = await h.call("modify_beams", { beams: [{ guid: G1, level: 2.7 }] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("Undo scope failed");
  });
});

describe("tool listing", () => {
  it("registers the column/beam tools with documented schemas", async () => {
    const h = await harness(() => addonOk({}));
    const tools = await h.listTools();
    const byName = new Map(tools.map((t) => [t.name, t]));
    for (const name of ["create_columns", "create_beams", "modify_columns", "modify_beams"]) {
      expect(byName.has(name)).toBe(true);
      expect((byName.get(name)?.description ?? "").length).toBeGreaterThan(100);
    }
    const columnItem = JSON.stringify(byName.get("create_columns")?.inputSchema);
    for (const field of ["origin", "height", "topLinkedStory", "segments", "cuts", "veneerThickness", "anchor", "slantAngle"]) {
      expect(columnItem).toContain(`"${field}"`);
    }
    const beamItem = JSON.stringify(byName.get("create_beams")?.inputSchema);
    for (const field of ["begin", "end", "level", "holes", "arcAngle", "verticalCurveHeight", "profileRotationAngle", "leftSurface"]) {
      expect(beamItem).toContain(`"${field}"`);
    }
  });
});
