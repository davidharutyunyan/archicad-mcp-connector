import { describe, expect, it } from "vitest";

import { addonOk, harness, type RecordedRequest } from "../helpers.js";

const WALL = "92F5E7F6-2460-FF4F-A499-0B07286D2B5F";
const WALL2 = "0C7A6E2B-1D3F-4E5A-9B8C-7D6E5F4A3B2C";
const DIM = "A1B2C3D4-E5F6-4A1B-8C2D-3E4F5A6B7C8D";
const DIM2 = "B2C3D4E5-F6A1-4B2C-9D3E-4F5A6B7C8D9E";

/** Responder: CreateElements → given results, GetElementDetails → given details. */
function responder(createResults: unknown[], details: unknown[] = []) {
  return (req: RecordedRequest) => {
    if (req.addOnCommand === "CreateElements" || req.addOnCommand === "ModifyElements") return addonOk({ results: createResults });
    if (req.addOnCommand === "GetElementDetails") return addonOk({ elements: details });
    return addonOk({});
  };
}

describe("dimension tools registration", () => {
  it("registers every dimension tool", async () => {
    const h = await harness(() => addonOk({}));
    const names = (await h.listTools()).map((t) => t.name);
    for (const n of [
      "create_dimensions",
      "create_level_dimensions",
      "create_radial_dimensions",
      "create_angle_dimensions",
      "dimension_walls",
      "modify_dimensions",
      "get_dimension_anchors",
    ]) {
      expect(names).toContain(n);
    }
  });
});

describe("create_dimensions", () => {
  it("sends typed Dimension specs (element refs normalized) and merges the measured summary", async () => {
    const h = await harness(
      responder(
        [{ guid: DIM, type: "Dimension" }],
        [
          {
            guid: DIM,
            type: "Dimension",
            details: { pointCount: 3, associativePoints: 2, segments: [5, 2.5], total: 7.5, linePoint: { x: 0, y: 1 }, directionAngle: 0, points: [] },
          },
        ],
      ),
    );
    const res = await h.call("create_dimensions", {
      dimensions: [
        {
          points: [{ element: { guid: WALL }, at: "begin" }, { element: WALL, at: "end" }, { x: 7.5, y: 0, text: "EQ" }],
          direction: "Horizontal",
          offset: 1.2,
          markerType: "SlashLine45",
          textSize: 2.5,
          layer: "Размеры",
        },
      ],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("CreateElements");
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [
        {
          type: "Dimension",
          points: [{ element: WALL, at: "begin" }, { element: WALL, at: "end" }, { x: 7.5, y: 0, text: "EQ" }],
          direction: "Horizontal",
          offset: 1.2,
          markerType: "SlashLine45",
          textSize: 2.5,
          layer: "Размеры",
        },
      ],
      undoName: "Create dimensions (Claude)",
    });
    expect(h.requests[1]?.addOnCommand).toBe("GetElementDetails");
    expect(h.requests[1]?.addOnParameters).toEqual({ elements: [DIM] });
    expect(res.json).toEqual({
      results: [{ guid: DIM, type: "Dimension", pointCount: 3, associativePoints: 2, segments: [5, 2.5], total: 7.5, linePoint: { x: 0, y: 1 }, directionAngle: 0 }],
    });
  });

  it("warns when element-referencing points ended up static", async () => {
    const h = await harness(
      responder([{ guid: DIM, type: "Dimension" }], [{ guid: DIM, type: "Dimension", details: { pointCount: 2, associativePoints: 0 } }]),
    );
    const res = await h.call("create_dimensions", {
      dimensions: [{ points: [{ element: WALL, x: 0, y: 0 }, { element: WALL, x: 5, y: 0 }] }],
    });
    expect(res.isError).toBe(false);
    const item = (res.json as { results: Array<{ warning?: string }> }).results[0];
    expect(item?.warning).toContain("2 of 2");
  });

  it("keeps per-item errors and only asks details for created dimensions", async () => {
    const h = await harness(
      responder(
        [{ guid: DIM, type: "Dimension" }, { error: { code: -2130313112, message: "points[0]: give 'x','y'" } }],
        [{ guid: DIM, type: "Dimension", details: { pointCount: 2, associativePoints: 0, total: 3 } }],
      ),
    );
    const res = await h.call("create_dimensions", {
      dimensions: [{ points: [{ x: 0, y: 0 }, { x: 3, y: 0 }] }, { points: [{ x: 0, y: 0 }, { x: 0, y: 0 }], direction: 90 }],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[1]?.addOnParameters).toEqual({ elements: [DIM] });
    const results = (res.json as { results: unknown[] }).results;
    expect(results[0]).toMatchObject({ guid: DIM, total: 3 });
    expect(results[1]).toEqual({ error: { code: -2130313112, message: "points[0]: give 'x','y'" } });
  });

  it("returns the create results when the details call fails", async () => {
    const h = await harness((req) =>
      req.addOnCommand === "GetElementDetails"
        ? addonOk({ error: { code: 1, message: "boom" } })
        : addonOk({ results: [{ guid: DIM, type: "Dimension" }] }),
    );
    const res = await h.call("create_dimensions", { dimensions: [{ points: [{ x: 0, y: 0 }, { x: 3, y: 0 }] }] });
    expect(res.isError).toBe(false);
    expect(res.json).toEqual({ results: [{ guid: DIM, type: "Dimension" }] });
  });

  it("rejects invalid points before calling Archicad", async () => {
    const h = await harness(() => addonOk({}));
    for (const points of [
      [{ x: 0, y: 0 }], // fewer than 2 points
      [{ x: 0 }, { x: 1, y: 1 }], // x without y
      [{ at: "begin" }, { x: 1, y: 1 }], // neither x,y nor element
      [{ element: WALL, inIndex: 1 }, { x: 1, y: 1 }], // raw reference without x,y
      [{ element: WALL }, { x: 1, y: 1 }], // element point without 'at' or x,y
      [{ at: "end", x: 0, y: 0 }, { x: 1, y: 1 }], // 'at' without element
    ]) {
      const res = await h.call("create_dimensions", { dimensions: [{ points }] });
      expect(res.isError).toBe(true);
    }
    const bad = await h.call("create_dimensions", { dimensions: [{ points: [{ x: 0, y: 0 }, { x: 1, y: 0 }], markerType: "Arrow" }] });
    expect(bad.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("surfaces add-on command errors as tool errors", async () => {
    const h = await harness(() => addonOk({ error: { code: -2130313112, message: "Cannot create the dimension: APIERR_BADPARS" } }));
    const res = await h.call("create_dimensions", { dimensions: [{ points: [{ x: 0, y: 0 }, { x: 1, y: 0 }] }] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("Cannot create the dimension");
  });
});

describe("create_level_dimensions", () => {
  it("sends LevelDimension specs", async () => {
    const h = await harness(
      responder(
        [{ guid: DIM, type: "LevelDimension" }],
        [{ guid: DIM, type: "LevelDimension", details: { position: { x: 1, y: 2 }, level: 3, static: false, element: WALL, elementType: "Slab", markerStyle: 2 } }],
      ),
    );
    const res = await h.call("create_level_dimensions", {
      levelDimensions: [{ position: { x: 1, y: 2 }, element: { guid: WALL }, elevationReference: "SeaLevel", markerStyle: 2, showPlusSign: true }],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [{ type: "LevelDimension", position: { x: 1, y: 2 }, element: WALL, elevationReference: "SeaLevel", markerStyle: 2, showPlusSign: true }],
      undoName: "Create level dimensions (Claude)",
    });
    expect((res.json as { results: unknown[] }).results[0]).toEqual({
      guid: DIM,
      type: "LevelDimension",
      position: { x: 1, y: 2 },
      level: 3,
      static: false,
      element: WALL,
      elementType: "Slab",
    });
  });

  it("requires a position and a valid marker style", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("create_level_dimensions", { levelDimensions: [{ level: 3 }] })).isError).toBe(true);
    expect((await h.call("create_level_dimensions", { levelDimensions: [{ position: { x: 0, y: 0 }, markerStyle: 12 }] })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("create_radial_dimensions", () => {
  it("sends RadialDimension specs with the element GUID", async () => {
    const h = await harness(
      responder([{ guid: DIM, type: "RadialDimension" }], [{ guid: DIM, type: "RadialDimension", details: { radius: 2.5, associative: true, element: WALL } }]),
    );
    const res = await h.call("create_radial_dimensions", {
      radialDimensions: [{ element: { guid: WALL }, at: { x: 3, y: 3 }, prefix: "R", textDirection: "Radial" }],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [{ type: "RadialDimension", element: WALL, at: { x: 3, y: 3 }, prefix: "R", textDirection: "Radial" }],
      undoName: "Create radial dimensions (Claude)",
    });
    expect((res.json as { results: unknown[] }).results[0]).toMatchObject({ radius: 2.5, associative: true });
  });

  it("requires the element", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("create_radial_dimensions", { radialDimensions: [{ at: { x: 1, y: 1 } }] })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("create_angle_dimensions", () => {
  it("sends static line pairs", async () => {
    const h = await harness(responder([{ guid: DIM, type: "AngleDimension" }], [{ guid: DIM, type: "AngleDimension", details: { angle: 45, radius: 1 } }]));
    const res = await h.call("create_angle_dimensions", {
      angleDimensions: [{ line1: { begin: { x: 0, y: 0 }, end: { x: 5, y: 0 } }, line2: { begin: { x: 0, y: 0 }, end: { x: 5, y: 5 } }, radius: 2 }],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [{ type: "AngleDimension", line1: { begin: { x: 0, y: 0 }, end: { x: 5, y: 0 } }, line2: { begin: { x: 0, y: 0 }, end: { x: 5, y: 5 } }, radius: 2 }],
      undoName: "Create angle dimensions (Claude)",
    });
    expect((res.json as { results: unknown[] }).results[0]).toMatchObject({ angle: 45 });
  });

  it("normalizes element references", async () => {
    const h = await harness(responder([{ guid: DIM, type: "AngleDimension" }]));
    await h.call("create_angle_dimensions", { angleDimensions: [{ elements: [{ guid: WALL }, WALL2] }] });
    expect(h.requests[0]?.addOnParameters).toMatchObject({ elements: [{ type: "AngleDimension", elements: [WALL, WALL2] }] });
  });

  it("needs two elements or both lines", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("create_angle_dimensions", { angleDimensions: [{ line1: { begin: { x: 0, y: 0 }, end: { x: 1, y: 0 } } }] })).isError).toBe(true);
    expect((await h.call("create_angle_dimensions", { angleDimensions: [{ elements: [WALL] }] })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("dimension_walls", () => {
  it("calls DimensionWalls with GUID strings and options", async () => {
    const h = await harness(() =>
      addonOk({ results: [{ guid: DIM, role: "chain", walls: [WALL, WALL2], pointCount: 6, segments: [1, 2], total: 3 }, { guid: DIM2, role: "overall" }] }),
    );
    const res = await h.call("dimension_walls", {
      walls: [{ guid: WALL }, WALL2],
      mode: "Chain",
      side: "Left",
      distance: 1.5,
      overall: true,
      openingPoints: "Centers",
      markerType: "SlashLine45",
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("DimensionWalls");
    expect(h.requests[0]?.addOnParameters).toEqual({
      walls: [WALL, WALL2],
      mode: "Chain",
      side: "Left",
      distance: 1.5,
      overall: true,
      openingPoints: "Centers",
      markerType: "SlashLine45",
    });
    expect((res.json as { results: unknown[] }).results).toHaveLength(2);
  });

  it("validates the options", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("dimension_walls", { walls: [] })).isError).toBe(true);
    expect((await h.call("dimension_walls", { walls: [WALL], mode: "Perimeter" })).isError).toBe(true);
    expect((await h.call("dimension_walls", { walls: [WALL], distance: -1 })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("sends thickness mode with its position and per-wall results", async () => {
    const h = await harness(() =>
      addonOk({
        results: [
          { guid: DIM, role: "thickness", walls: [WALL], pointCount: 2, associativePoints: 2, segments: [0.3], total: 0.3 },
          { error: { code: -2130313112, message: "Wall X is curved; mode \"Thickness\" works with straight walls only" }, wall: WALL2 },
        ],
      }),
    );
    const res = await h.call("dimension_walls", { walls: [WALL, { guid: WALL2 }], mode: "Thickness", thicknessPosition: 0.5, textSize: 2 });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({ walls: [WALL, WALL2], mode: "Thickness", thicknessPosition: 0.5, textSize: 2 });
    const results = (res.json as { results: Array<Record<string, unknown>> }).results;
    expect(results[0]).toMatchObject({ role: "thickness", total: 0.3 });
    expect(results[1]).toMatchObject({ wall: WALL2 });
  });

  it("rejects a direction in thickness mode before calling Archicad", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("dimension_walls", { walls: [WALL], mode: "Thickness", direction: "Horizontal" });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("does not apply to mode 'Thickness'");
    expect(h.requests).toHaveLength(0);
  });

  it("surfaces add-on errors", async () => {
    const h = await harness(() => addonOk({ error: { code: -2130313112, message: "Element X is a Slab, not a Wall." } }));
    const res = await h.call("dimension_walls", { walls: [WALL] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("not a Wall");
  });
});

describe("modify_dimensions", () => {
  it("sends patches to ModifyElements and returns summaries", async () => {
    const h = await harness(
      responder([{ guid: DIM }], [{ guid: DIM, type: "Dimension", details: { pointCount: 2, associativePoints: 2, linePoint: { x: 0, y: 3 }, total: 5 } }]),
    );
    const res = await h.call("modify_dimensions", {
      dimensions: [
        { guid: DIM, linePoint: { x: 0, y: 3 }, textPen: 3 },
        { guid: DIM2, element: false, level: 1.2 },
      ],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("ModifyElements");
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [
        { guid: DIM, linePoint: { x: 0, y: 3 }, textPen: 3 },
        { guid: DIM2, element: false, level: 1.2 },
      ],
      undoName: "Modify dimensions (Claude)",
    });
    expect((res.json as { results: unknown[] }).results[0]).toEqual({ guid: DIM, pointCount: 2, associativePoints: 2, linePoint: { x: 0, y: 3 }, total: 5 });
  });

  it("rejects patches without a valid guid", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("modify_dimensions", { dimensions: [{ linePoint: { x: 0, y: 3 } }] })).isError).toBe(true);
    expect((await h.call("modify_dimensions", { dimensions: [{ guid: "nope" }] })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("passes pointTexts, the level elevation reference and custom texts through", async () => {
    const h = await harness(responder([{ guid: DIM }, { guid: DIM2 }]));
    const res = await h.call("modify_dimensions", {
      dimensions: [
        { guid: DIM, pointTexts: [{ index: 1, text: "EQ" }, { index: 2, text: false }] },
        { guid: DIM2, elevationReference: "ReferenceLevel1", text: "" },
      ],
      undoName: "Texts",
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [
        { guid: DIM, pointTexts: [{ index: 1, text: "EQ" }, { index: 2, text: false }] },
        { guid: DIM2, elevationReference: "ReferenceLevel1", text: "" },
      ],
      undoName: "Texts",
    });
  });

  it("validates pointTexts", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("modify_dimensions", { dimensions: [{ guid: DIM, pointTexts: [] }] })).isError).toBe(true);
    expect((await h.call("modify_dimensions", { dimensions: [{ guid: DIM, pointTexts: [{ index: -1, text: "A" }] }] })).isError).toBe(true);
    expect((await h.call("modify_dimensions", { dimensions: [{ guid: DIM, pointTexts: [{ index: 1, text: true }] }] })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("get_dimension_anchors", () => {
  it("calls GetDimensionAnchors", async () => {
    const anchors = { elements: [{ guid: WALL, type: "Wall", anchorCount: 1, anchors: [{ x: 0, y: 0, z: 0, neig: "Wall", inIndex: 1, usableAsPoint: true }] }] };
    const h = await harness(() => addonOk(anchors));
    const res = await h.call("get_dimension_anchors", { elements: [{ guid: WALL }], near: { x: 0, y: 0 }, radius: 0.5, limit: 10 });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("GetDimensionAnchors");
    expect(h.requests[0]?.addOnParameters).toEqual({ elements: [WALL], near: { x: 0, y: 0 }, radius: 0.5, limit: 10 });
    expect(res.json).toEqual(anchors);
  });
});
