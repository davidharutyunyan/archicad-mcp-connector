import { describe, expect, it } from "vitest";

import { addonOk, apiOk, harness, type RecordedRequest } from "../helpers.js";

const G1 = "92F5E7F6-2460-FF4F-A499-0B07286D2B5F";
const G2 = "0A1B2C3D-4E5F-4061-8293-A4B5C6D7E8F9";
const G3 = "11111111-2222-4333-8444-555555555555";

const square = [
  { x: 0, y: 0 },
  { x: 8, y: 0 },
  { x: 8, y: 6 },
  { x: 0, y: 6 },
];

function created(type: string, n = 1) {
  return addonOk({ results: Array.from({ length: n }, (_, i) => ({ guid: [G1, G2, G3][i], type })) });
}

describe("slabs-roofs tool registration", () => {
  it("registers the typed create/modify tools with described schemas", async () => {
    const h = await harness(() => addonOk({}));
    const tools = await h.listTools();
    const names = tools.map((t) => t.name);
    for (const name of [
      "create_slabs",
      "create_roofs",
      "create_shells",
      "create_meshes",
      "modify_slabs",
      "modify_roofs",
      "modify_shells",
      "modify_meshes",
    ]) {
      expect(names).toContain(name);
    }
    const roofs = tools.find((t) => t.name === "create_roofs")!;
    const schemaText = JSON.stringify(roofs.inputSchema);
    expect(schemaText).toContain("slopeAngle");
    expect(schemaText).toContain("pivotEdges");
    expect(schemaText).toContain("degrees");
    const meshes = tools.find((t) => t.name === "create_meshes")!;
    expect(JSON.stringify(meshes.inputSchema)).toContain("levelLines");
  });
});

describe("create_slabs", () => {
  it("sends typed slab specs (polygon with holes/arcs, structure, edges) to CreateElements", async () => {
    const h = await harness(() => created("Slab"));
    const slab = {
      polygon: { points: square, arcs: [{ index: 1, angle: 90 }], holes: [{ points: [{ x: 2, y: 2 }, { x: 3, y: 2 }, { x: 3, y: 3 }] }] },
      thickness: 0.25,
      level: 3,
      referencePlane: "Top",
      composite: "Перекрытие 250",
      topSurface: "Бетон",
      sideSurface: false,
      edgeTrim: "CustomAngle",
      edgeAngle: 60,
      edges: [{ index: 0, trim: "Vertical" }, { contour: 1, index: 2, surface: 3 }],
      cutFillPen: false,
      storyVisibility: { onHomeStory: true, storiesAbove: 1 },
      storyIndex: 1,
      elementId: "SL-01",
    };
    const res = await h.call("create_slabs", { slabs: [slab] });
    expect(res.isError).toBe(false);
    expect(h.requests).toHaveLength(1);
    expect(h.requests[0]?.addOnCommand).toBe("CreateElements");
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [{ type: "Slab", ...slab }],
      undoName: "Create slabs (Claude)",
    });
    expect(res.json).toEqual({ results: [{ guid: G1, type: "Slab" }] });
  });

  it("accepts a plain point array and a custom undo name", async () => {
    const h = await harness(() => created("Slab", 2));
    const res = await h.call("create_slabs", {
      slabs: [{ polygon: square }, { polygon: square, level: 3 }],
      undoName: "Floors",
    });
    expect(res.isError).toBe(false);
    const params = h.requests[0]?.addOnParameters as { elements: unknown[]; undoName: string };
    expect(params.elements).toHaveLength(2);
    expect(params.undoName).toBe("Floors");
  });

  it("rejects invalid input before calling Archicad", async () => {
    const h = await harness(() => addonOk({}));
    for (const bad of [
      { slabs: [{ thickness: 0.2 }] }, // no polygon
      { slabs: [{ polygon: [{ x: 0, y: 0 }, { x: 1, y: 0 }] }] }, // < 3 points
      { slabs: [{ polygon: square, thickness: -1 }] },
      { slabs: [{ polygon: square, edgeAngle: 180 }] },
      { slabs: [{ polygon: square, referencePlane: "Middle" }] },
      // Archicad resets Horizontal / AlignWithCut slab edges to Vertical: rejected up front
      { slabs: [{ polygon: square, edgeTrim: "Horizontal" }] },
      { slabs: [{ polygon: square, edges: [{ index: 0, trim: "AlignWithCut" }] }] },
      { slabs: [] },
    ]) {
      const res = await h.call("create_slabs", bad);
      expect(res.isError).toBe(true);
    }
    expect(h.requests).toHaveLength(0);
  });

  it("surfaces per-item add-on errors unchanged", async () => {
    const h = await harness(() =>
      addonOk({ results: [{ guid: G1, type: "Slab" }, { error: { code: -2130313112, message: "Cannot create slab: APIERR_IRREGULARPOLY" } }] }),
    );
    const res = await h.call("create_slabs", { slabs: [{ polygon: square }, { polygon: square }] });
    expect(res.isError).toBe(false);
    expect(res.text).toContain("APIERR_IRREGULARPOLY");
  });
});

describe("create_roofs", () => {
  it("builds a single-plane roof request", async () => {
    const h = await harness(() => created("Roof"));
    const roof = {
      polygon: square,
      pivotLine: { begin: { x: 0, y: 0 }, end: { x: 8, y: 0 } },
      slopeAngle: 35,
      level: 3,
      thickness: 0.3,
      buildingMaterial: 12,
      edges: [{ index: 2, edgeType: "Ridge" }, { index: 0, trim: "Horizontal" }], // roofs keep all edge trims
    };
    const res = await h.call("create_roofs", { roofs: [roof] });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("CreateElements");
    expect(h.requests[0]?.addOnParameters).toMatchObject({ elements: [{ type: "Roof", ...roof }] });
  });

  it("builds a multi-plane roof request (class inferred from pivotPolygon)", async () => {
    const h = await harness(() => created("Roof"));
    const roof = {
      pivotPolygon: square,
      level: 3,
      levels: [{ angle: 60, height: 2.2 }, { angle: 20 }],
      eavesOverhang: 0.6,
      pivotEdges: [{ index: 1, gable: true }, { index: 3, angle: 45, eavesOverhang: 0.2 }],
      composite: "Кровля",
      connectionBody: "PivotLinesDown",
    };
    const res = await h.call("create_roofs", { roofs: [roof] });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toMatchObject({ elements: [{ type: "Roof", ...roof }] });
  });

  it("rejects fields of the other roof class and missing geometry", async () => {
    const h = await harness(() => addonOk({}));
    const cases = [
      { roofs: [{ polygon: square }] }, // single-plane without pivotLine
      { roofs: [{ pivotLine: { begin: { x: 0, y: 0 }, end: { x: 1, y: 0 } } }] }, // no polygon
      { roofs: [{ pivotPolygon: square, pivotLine: { begin: { x: 0, y: 0 }, end: { x: 1, y: 0 } } }] }, // multi + single field
      { roofs: [{ roofClass: "MultiPlane", polygon: square }] }, // multi without pivotPolygon, with polygon
      { roofs: [{ polygon: square, pivotLine: { begin: { x: 0, y: 0 }, end: { x: 1, y: 0 } }, levels: [{ angle: 30 }] }] },
      { roofs: [{ pivotPolygon: square, slopeAngle: 95 }] },
      { roofs: [{ pivotPolygon: square, levels: [] }] },
      { roofs: [{ pivotPolygon: square, levels: [{ angle: 60 }, { angle: 20, height: 2 }] }] }, // lower level without height
      { roofs: [{ pivotPolygon: square, levels: [{ angle: 60, height: 3 }, { angle: 40, height: 2 }, { angle: 20 }] }] }, // decreasing
    ];
    for (const bad of cases) {
      const res = await h.call("create_roofs", bad);
      expect(res.isError, JSON.stringify(bad)).toBe(true);
    }
    expect(h.requests).toHaveLength(0);
  });
});

describe("create_shells", () => {
  it("builds extruded, revolved and ruled shell requests", async () => {
    const h = await harness(() => created("Shell", 3));
    const shells = [
      {
        profile: { points: [{ x: -3, y: 0 }, { x: 3, y: 0 }], arcs: [{ index: 0, angle: -180 }] },
        begin: { x: 0, y: 0, z: 3 },
        extrusion: { x: 0, y: 12, z: 0 },
        thickness: 0.15,
      },
      {
        shellClass: "Revolved",
        profile: [{ x: 5, y: 0 }, { x: 5, y: 3 }, { x: 0, y: 6 }],
        axisOrigin: { x: 20, y: 20 },
        revolutionAngle: 360,
        circleSegments: 48,
      },
      {
        shellClass: "Ruled",
        profile: [{ x: 0, y: 0 }, { x: 4, y: 0 }],
        profile2: [{ x: 0, y: 1 }, { x: 4, y: 2 }],
        plane1: { origin: { x: 0, y: 0, z: 0 }, xAxis: { x: 1, y: 0, z: 0 }, yAxis: { x: 0, y: 0, z: 1 } },
        plane2: { matrix: [1, 0, 0, 0, 0, 0, -1, 5, 0, 1, 0, 0] },
        morphingRule: "Smooth",
      },
    ];
    const res = await h.call("create_shells", { shells });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toMatchObject({
      elements: shells.map((s) => ({ type: "Shell", ...s })),
      undoName: "Create shells (Claude)",
    });
  });

  it("requires the class-specific geometry", async () => {
    const h = await harness(() => addonOk({}));
    const cases = [
      { shells: [{ profile: [{ x: 0, y: 0 }, { x: 1, y: 1 }] }] }, // extruded without extrusion
      { shells: [{ shellClass: "Ruled", profile: [{ x: 0, y: 0 }, { x: 1, y: 1 }] }] }, // ruled without profile2/planes
      { shells: [{ shellClass: "Revolved" }] }, // no profile
      { shells: [{ shellClass: "Revolved", profile: [{ x: 0, y: 0 }, { x: 1, y: 1 }], revolutionAngle: 400 }] },
      { shells: [{ shellClass: "Ruled", profile: [{ x: 0, y: 0 }, { x: 1, y: 1 }], profile2: [{ x: 0, y: 0 }, { x: 1, y: 1 }], plane1: { matrix: [1, 2] }, plane2: {} }] },
    ];
    for (const bad of cases) {
      const res = await h.call("create_shells", bad);
      expect(res.isError, JSON.stringify(bad)).toBe(true);
    }
    expect(h.requests).toHaveLength(0);
  });
});

describe("create_meshes", () => {
  it("sends points with z and level lines", async () => {
    const h = await harness(() => created("Mesh"));
    const mesh = {
      polygon: [
        { x: 0, y: 0, z: 0 },
        { x: 30, y: 0, z: 1 },
        { x: 30, y: 20, z: 2.5 },
        { x: 0, y: 20, z: 0.5 },
      ],
      levelLines: [{ points: [{ x: 5, y: 5, z: 1.2 }, { x: 25, y: 15, z: 1.8 }] }],
      level: -0.1,
      skirt: "SolidBody",
      skirtLevel: 1,
      ridges: "AllSmooth",
      buildingMaterial: "Грунт",
    };
    const res = await h.call("create_meshes", { meshes: [mesh] });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toMatchObject({ elements: [{ type: "Mesh", ...mesh }] });
  });

  it("rejects bad meshes", async () => {
    const h = await harness(() => addonOk({}));
    const cases = [
      { meshes: [{ polygon: square, levelLines: [{ points: [{ x: 1, y: 1, z: 0 }] }] }] }, // level line with one point
      { meshes: [{ polygon: square, levelLines: [{ points: [{ x: 1, y: 1 }, { x: 2, y: 2 }] }] }] }, // level line without z
      { meshes: [{ polygon: square, skirt: "Hollow" }] },
      { meshes: [{ polygon: square, composite: "X" }] }, // meshes have no composite (unknown key is stripped -> still valid?)
    ];
    const results = [];
    for (const bad of cases) results.push((await h.call("create_meshes", bad)).isError);
    expect(results.slice(0, 3)).toEqual([true, true, true]);
    // Unknown keys are stripped by zod, so a composite never reaches the add-on:
    const sent = h.requests.map((r) => JSON.stringify(r.addOnParameters));
    expect(sent.join("")).not.toContain("composite");
  });
});

describe("modify_* tools", () => {
  function typesResponder(types: Record<string, string | null>, addon: (req: RecordedRequest) => unknown) {
    return (req: RecordedRequest) => {
      if (req.command === "API.GetTypesOfElements") {
        const elements = (req.parameters as { elements: Array<{ elementId: { guid: string } }> }).elements;
        return apiOk({
          typesOfElements: elements.map((e) => {
            const t = types[e.elementId.guid];
            return t
              ? { typeOfElement: { elementId: { guid: e.elementId.guid }, elementType: t } }
              : { error: { code: 7204, message: "Element not found" } };
          }),
        });
      }
      return addon(req);
    };
  }

  it("modify_slabs only sends slabs and merges per-item errors in input order", async () => {
    const h = await harness(
      typesResponder({ [G1]: "Slab", [G2]: "Wall", [G3]: null }, (req) => {
        const sent = (req.addOnParameters as { elements: Array<{ guid: string }> }).elements;
        return addonOk({ results: sent.map((e) => ({ guid: e.guid })) });
      }),
    );
    const res = await h.call("modify_slabs", {
      slabs: [
        { guid: G1, thickness: 0.3, polygon: square, edges: [{ index: 1, angle: 45 }] },
        { guid: G2, thickness: 0.3 },
        { guid: G3, level: 1 },
      ],
    });
    expect(res.isError).toBe(false);
    expect(h.requests.map((r) => r.command)).toEqual(["API.GetTypesOfElements", "API.ExecuteAddOnCommand"]);
    expect(h.requests[1]?.addOnCommand).toBe("ModifyElements");
    expect(h.requests[1]?.addOnParameters).toEqual({
      elements: [{ guid: G1, thickness: 0.3, polygon: square, edges: [{ index: 1, angle: 45 }] }],
      undoName: "Modify slabs (Claude)",
    });
    const results = (res.json as { results: Array<{ guid: string; error?: { message: string } }> }).results;
    expect(results).toHaveLength(3);
    expect(results[0]).toEqual({ guid: G1 });
    expect(results[1]?.error?.message).toContain("is a Wall, not a Slab");
    expect(results[1]?.error?.message).toContain("modify_elements");
    expect(results[2]?.error?.message).toContain("not found");
  });

  it("points to the right family tool on a type mismatch", async () => {
    const h = await harness(typesResponder({ [G1]: "Mesh" }, () => addonOk({ results: [] })));
    const res = await h.call("modify_roofs", { roofs: [{ guid: G1, slopeAngle: 30 }] });
    const results = (res.json as { results: Array<{ error?: { message: string } }> }).results;
    expect(results[0]?.error?.message).toContain("modify_meshes");
    // nothing left to send -> no ModifyElements call
    expect(h.requests.map((r) => r.addOnCommand ?? r.command)).toEqual(["API.GetTypesOfElements"]);
  });

  it("still modifies when the type check is unavailable", async () => {
    const h = await harness((req) => {
      if (req.command === "API.GetTypesOfElements") return { succeeded: false, error: { code: 2002, message: "not found" } };
      return addonOk({ results: [{ guid: G1 }] });
    });
    const res = await h.call("modify_meshes", { meshes: [{ guid: G1, levelLines: [] }], undoName: "Flatten" });
    expect(res.isError).toBe(false);
    expect(h.requests[1]?.addOnParameters).toEqual({ elements: [{ guid: G1, levelLines: [] }], undoName: "Flatten" });
  });

  it("modify_shells and modify_roofs pass class-specific fields through", async () => {
    const h = await harness(
      typesResponder({ [G1]: "Shell", [G2]: "Roof" }, (req) => {
        const sent = (req.addOnParameters as { elements: Array<{ guid: string }> }).elements;
        return addonOk({ results: sent.map((e) => ({ guid: e.guid })) });
      }),
    );
    const shell = await h.call("modify_shells", { shells: [{ guid: G1, extrusion: { x: 0, y: 8 }, closedProfile: true }] });
    expect(shell.isError).toBe(false);
    const roof = await h.call("modify_roofs", { roofs: [{ guid: G2, pivotEdges: [{ index: 0, gable: true }], levels: [{ angle: 30 }] }] });
    expect(roof.isError).toBe(false);
    const modifyCalls = h.requests.filter((r) => r.addOnCommand === "ModifyElements");
    expect(modifyCalls[0]?.addOnParameters).toMatchObject({ elements: [{ guid: G1, extrusion: { x: 0, y: 8 }, closedProfile: true }] });
    expect(modifyCalls[1]?.addOnParameters).toMatchObject({ elements: [{ guid: G2, pivotEdges: [{ index: 0, gable: true }], levels: [{ angle: 30 }] }] });
  });

  it("surfaces a whole-command add-on error as a tool error", async () => {
    const h = await harness(
      typesResponder({ [G1]: "Slab" }, () => addonOk({ error: { code: -2130313112, message: "Missing required array field 'elements'." } })),
    );
    const res = await h.call("modify_slabs", { slabs: [{ guid: G1, level: 0 }] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("Missing required array field");
  });

  it("rejects invalid GUIDs before calling Archicad", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("modify_slabs", { slabs: [{ guid: "not-a-guid", level: 0 }] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});
