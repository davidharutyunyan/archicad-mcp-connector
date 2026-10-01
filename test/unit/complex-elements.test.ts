import { describe, expect, it } from "vitest";

import { addonOk, apiOk, harness, type RecordedRequest } from "../helpers.js";

const G1 = "92F5E7F6-2460-FF4F-A499-0B07286D2B5F";
const G2 = "0A1B2C3D-4E5F-6071-8293-A4B5C6D7E8F9";
const G3 = "11111111-2222-3333-4444-555555555555";

/** Responder that answers API.GetTypesOfElements from a guid->type map and echoes ModifyElements results. */
function typedResponder(types: Record<string, string>, extra?: (req: RecordedRequest) => unknown) {
  return (req: RecordedRequest) => {
    if (req.command === "API.GetTypesOfElements") {
      const elements = (req.parameters?.["elements"] as Array<{ elementId: { guid: string } }>) ?? [];
      return apiOk({
        typesOfElements: elements.map((e) =>
          types[e.elementId.guid]
            ? { typeOfElement: { elementType: types[e.elementId.guid] } }
            : { error: { code: -2130313112, message: "not found" } },
        ),
      });
    }
    if (req.addOnCommand === "ModifyElements") {
      const elements = (req.addOnParameters?.["elements"] as Array<{ guid: string }>) ?? [];
      return addonOk({ results: elements.map((e) => ({ guid: e.guid })) });
    }
    return extra ? extra(req) : addonOk({});
  };
}

describe("complex-elements tool registration", () => {
  it("registers every tool of the family", async () => {
    const h = await harness(() => addonOk({}));
    const names = (await h.listTools()).map((t) => t.name);
    for (const n of [
      "create_morphs",
      "modify_morphs",
      "get_morph_geometry",
      "create_curtain_walls",
      "modify_curtain_walls",
      "modify_curtain_wall_parts",
      "create_stairs",
      "modify_stairs",
      "create_railings",
      "modify_railings",
    ]) {
      expect(names).toContain(n);
    }
  });
});

describe("create_morphs", () => {
  it("sends a box morph to CreateElements", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G1, type: "Morph" }] }));
    const res = await h.call("create_morphs", {
      morphs: [{ box: { origin: { x: 1, y: 2, z: 0 }, size: { x: 3, y: 2, z: 1 }, rotation: 30, topSurface: "Glass" }, buildingMaterial: 5 }],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("CreateElements");
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [
        {
          type: "Morph",
          box: { origin: { x: 1, y: 2, z: 0 }, size: { x: 3, y: 2, z: 1 }, rotation: 30, topSurface: "Glass" },
          buildingMaterial: 5,
        },
      ],
      undoName: "Create morphs (Claude)",
    });
    expect(res.json).toEqual({ results: [{ guid: G1, type: "Morph" }] });
  });

  it("normalizes mesh vertices and faces to the object form", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G1, type: "Morph" }] }));
    const res = await h.call("create_morphs", {
      morphs: [
        {
          mesh: {
            vertices: [[0, 0, 0], { x: 1, y: 0, z: 0 }, [0, 1, 0], [0, 0, 1]],
            faces: [[0, 2, 1], { vertices: [0, 1, 3], surface: "Brick" }, [1, 2, 3], { vertices: [0, 3, 2], holes: [[0, 1, 2]] }],
          },
          bodyType: "Solid",
        },
      ],
    });
    expect(res.isError).toBe(false);
    const el = (h.requests[0]?.addOnParameters?.["elements"] as Array<Record<string, unknown>>)[0]!;
    expect(el["type"]).toBe("Morph");
    expect(el["mesh"]).toEqual({
      vertices: [
        { x: 0, y: 0, z: 0 },
        { x: 1, y: 0, z: 0 },
        { x: 0, y: 1, z: 0 },
        { x: 0, y: 0, z: 1 },
      ],
      faces: [
        { vertices: [0, 2, 1] },
        { vertices: [0, 1, 3], surface: "Brick" },
        { vertices: [1, 2, 3] },
        { vertices: [0, 3, 2], holes: [{ vertices: [0, 1, 2] }] },
      ],
    });
  });

  it("sends an extrusion with holes and arcs unchanged", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G1, type: "Morph" }] }));
    const polygon = {
      points: [{ x: 0, y: 0 }, { x: 4, y: 0 }, { x: 4, y: 3 }, { x: 0, y: 3 }],
      arcs: [{ index: 1, angle: 90 }],
      holes: [{ points: [{ x: 1, y: 1 }, { x: 2, y: 1 }, { x: 2, y: 2 }] }],
    };
    const res = await h.call("create_morphs", { morphs: [{ extrusion: { polygon, zBottom: 0, zTop: 2.5 }, storyIndex: 1 }] });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toMatchObject({
      elements: [{ type: "Morph", extrusion: { polygon, zBottom: 0, zTop: 2.5 }, storyIndex: 1 }],
    });
  });

  it("rejects items without exactly one geometry before calling Archicad", async () => {
    const h = await harness(() => addonOk({}));
    const none = await h.call("create_morphs", { morphs: [{ bodyType: "Solid" }] });
    expect(none.isError).toBe(true);
    const two = await h.call("create_morphs", {
      morphs: [{ box: { origin: { x: 0, y: 0, z: 0 }, size: { x: 1, y: 1, z: 1 } }, mesh: { vertices: [[0, 0, 0], [1, 0, 0], [0, 1, 0]], faces: [[0, 1, 2]] } }],
    });
    expect(two.isError).toBe(true);
    const badSize = await h.call("create_morphs", { morphs: [{ box: { origin: { x: 0, y: 0, z: 0 }, size: { x: 0, y: 1, z: 1 } } }] });
    expect(badSize.isError).toBe(true);
    const shortFace = await h.call("create_morphs", { morphs: [{ mesh: { vertices: [[0, 0, 0], [1, 0, 0], [0, 1, 0]], faces: [[0, 1]] } }] });
    expect(shortFace.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("surfaces per-item add-on errors in the results", async () => {
    const h = await harness(() =>
      addonOk({ results: [{ error: { code: -2130313112, message: "bodyType 'Solid' needs a closed mesh" } }] }),
    );
    const res = await h.call("create_morphs", {
      morphs: [{ mesh: { vertices: [[0, 0, 0], [1, 0, 0], [0, 1, 0]], faces: [[0, 1, 2]] }, bodyType: "Solid" }],
    });
    expect(res.isError).toBe(false);
    expect(res.text).toContain("needs a closed mesh");
  });
});

describe("modify_morphs", () => {
  it("checks the types and modifies only morphs", async () => {
    const h = await harness(typedResponder({ [G1]: "Morph", [G2]: "Wall" }));
    const res = await h.call("modify_morphs", {
      morphs: [
        { guid: G1, offset: { x: 1, y: 0, z: 0.5 }, faceSurface: false },
        { guid: G2, level: 1 },
        { guid: G3, castShadow: false },
      ],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.command).toBe("API.GetTypesOfElements");
    const modify = h.requests.find((r) => r.addOnCommand === "ModifyElements");
    expect(modify?.addOnParameters).toEqual({
      elements: [{ guid: G1, offset: { x: 1, y: 0, z: 0.5 }, faceSurface: false }],
      undoName: "Modify morphs (Claude)",
    });
    const results = (res.json as { results: Array<{ guid: string; error?: { message: string } }> }).results;
    expect(results[0]).toEqual({ guid: G1 });
    expect(results[1]?.error?.message).toContain("is a Wall, not a Morph");
    expect(results[2]?.error?.message).toContain("was not found");
  });

  it("normalizes a replacement mesh and rejects geometry + level", async () => {
    const h = await harness(typedResponder({ [G1]: "Morph" }));
    const ok = await h.call("modify_morphs", {
      morphs: [{ guid: G1, mesh: { vertices: [[0, 0, 0], [1, 0, 0], [0, 1, 0]], faces: [[0, 1, 2]] } }],
    });
    expect(ok.isError).toBe(false);
    const modify = h.requests.find((r) => r.addOnCommand === "ModifyElements");
    expect((modify?.addOnParameters?.["elements"] as Array<Record<string, unknown>>)[0]!["mesh"]).toEqual({
      vertices: [
        { x: 0, y: 0, z: 0 },
        { x: 1, y: 0, z: 0 },
        { x: 0, y: 1, z: 0 },
      ],
      faces: [{ vertices: [0, 1, 2] }],
    });

    const count = h.requests.length;
    const bad = await h.call("modify_morphs", {
      morphs: [{ guid: G1, level: 2, box: { origin: { x: 0, y: 0, z: 0 }, size: { x: 1, y: 1, z: 1 } } }],
    });
    expect(bad.isError).toBe(true);
    expect(h.requests).toHaveLength(count);
  });

  it("does not call ModifyElements when every item fails the type check", async () => {
    const h = await harness(typedResponder({ [G1]: "Slab" }));
    const res = await h.call("modify_morphs", { morphs: [{ guid: G1, castShadow: true }] });
    expect(res.isError).toBe(false);
    expect(h.requests.some((r) => r.addOnCommand === "ModifyElements")).toBe(false);
  });
});

describe("get_morph_geometry", () => {
  it("sends plain GUIDs and maxVertices", async () => {
    const h = await harness(() => addonOk({ elements: [{ guid: G1, vertexCount: 8, faceCount: 6, vertices: [], faces: [] }] }));
    const res = await h.call("get_morph_geometry", { elements: [G1, { guid: G2 }], maxVertices: 500 });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("GetMorphGeometry");
    expect(h.requests[0]?.addOnParameters).toEqual({ elements: [G1, G2], maxVertices: 500 });
  });

  it("returns add-on errors as tool errors", async () => {
    const h = await harness(() => addonOk({ error: { code: -2130313112, message: "Missing required array field 'elements'." } }));
    const res = await h.call("get_morph_geometry", { elements: [G1] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("elements");
  });
});

describe("curtain walls", () => {
  it("create_curtain_walls sends begin/end, grids and panel settings", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G1, type: "CurtainWall" }] }));
    const res = await h.call("create_curtain_walls", {
      curtainWalls: [
        {
          begin: { x: 0, y: 0 },
          end: { x: 6, y: 0 },
          height: 3,
          primarySpacing: 1.5,
          secondaryGrid: { logic: "NumberOfDivisions", divisions: 3 },
          panelSurface: "Стекло",
          panelThickness: 0.03,
        },
      ],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [
        {
          type: "CurtainWall",
          begin: { x: 0, y: 0 },
          end: { x: 6, y: 0 },
          height: 3,
          primarySpacing: 1.5,
          secondaryGrid: { logic: "NumberOfDivisions", divisions: 3 },
          panelSurface: "Стекло",
          panelThickness: 0.03,
        },
      ],
      undoName: "Create curtain walls (Claude)",
    });
  });

  it("create_curtain_walls accepts a curved path and requires a base line", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G1, type: "CurtainWall" }] }));
    const ok = await h.call("create_curtain_walls", {
      curtainWalls: [{ path: { points: [{ x: 0, y: 0 }, { x: 5, y: 0 }, { x: 5, y: 5 }], arcs: [{ index: 1, angle: 45 }] }, height: 4 }],
    });
    expect(ok.isError).toBe(false);
    expect(h.requests).toHaveLength(1);
    const bad = await h.call("create_curtain_walls", { curtainWalls: [{ height: 3 }] });
    expect(bad.isError).toBe(true);
    expect(bad.text).toContain("begin");
    expect(h.requests).toHaveLength(1);
  });

  it("modify_curtain_walls only sends curtain walls", async () => {
    const h = await harness(typedResponder({ [G1]: "CurtainWall", [G2]: "Wall" }));
    const res = await h.call("modify_curtain_walls", {
      curtainWalls: [
        { guid: G1, height: 4.2, primaryGrid: { sizes: [1.2, 0.6], flexible: [1] } },
        { guid: G2, height: 3 },
      ],
    });
    expect(res.isError).toBe(false);
    const modify = h.requests.find((r) => r.addOnCommand === "ModifyElements");
    expect(modify?.addOnParameters?.["elements"]).toEqual([{ guid: G1, height: 4.2, primaryGrid: { sizes: [1.2, 0.6], flexible: [1] } }]);
    expect(res.text).toContain("is a Wall, not a CurtainWall");
  });

  it("modify_curtain_wall_parts validates fields per part type", async () => {
    const h = await harness(typedResponder({ [G1]: "CurtainWallPanel", [G2]: "CurtainWallFrame", [G3]: "CurtainWallFrame" }));
    const res = await h.call("modify_curtain_wall_parts", {
      parts: [
        { guid: G1, outerSurface: "Glass", thickness: 0.05 },
        { guid: G2, surface: 12, buildingMaterial: "Aluminium" },
        { guid: G3, thickness: 0.1 },
      ],
    });
    expect(res.isError).toBe(false);
    const modify = h.requests.find((r) => r.addOnCommand === "ModifyElements");
    expect(modify?.addOnParameters?.["elements"]).toEqual([
      { guid: G1, outerSurface: "Glass", thickness: 0.05 },
      { guid: G2, surface: 12, buildingMaterial: "Aluminium" },
    ]);
    const results = (res.json as { results: Array<{ error?: { message: string } }> }).results;
    expect(results[2]?.error?.message).toContain("do not apply to a CurtainWallFrame");
  });
});

describe("stairs", () => {
  it("create_stairs sends a straight stair", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G1, type: "Stair" }] }));
    const res = await h.call("create_stairs", {
      stairs: [{ begin: { x: 0, y: 0 }, end: { x: 4.5, y: 0 }, height: 3, width: 1.2, riserCount: 17, ignoreRules: true }],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [{ type: "Stair", begin: { x: 0, y: 0 }, end: { x: 4.5, y: 0 }, height: 3, width: 1.2, riserCount: 17, ignoreRules: true }],
      undoName: "Create stairs (Claude)",
    });
  });

  it("create_stairs validates input", async () => {
    const h = await harness(() => addonOk({}));
    const noBaseline = await h.call("create_stairs", { stairs: [{ height: 3 }] });
    expect(noBaseline.isError).toBe(true);
    const badEnum = await h.call("create_stairs", { stairs: [{ baseline: [{ x: 0, y: 0 }, { x: 3, y: 0 }], baselinePosition: "Middle" }] });
    expect(badEnum.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("rejects an inconsistent height / riserCount / riserHeight triple but accepts any two", async () => {
    const h = await harness(typedResponder({ [G1]: "Stair" }, () => addonOk({ results: [{ guid: G1, type: "Stair" }] })));
    const line = { begin: { x: 0, y: 0 }, end: { x: 4, y: 0 } };
    const bad = await h.call("create_stairs", { stairs: [{ ...line, height: 3, riserCount: 17, riserHeight: 0.2 }] });
    expect(bad.isError).toBe(true);
    expect(bad.text).toContain("riserCount × riserHeight");
    const badPatch = await h.call("modify_stairs", { stairs: [{ guid: G1, height: 2.8, riserCount: 16, riserHeight: 0.2 }] });
    expect(badPatch.isError).toBe(true);
    expect(h.requests).toHaveLength(0);

    const exact = await h.call("create_stairs", { stairs: [{ ...line, height: 3.4, riserCount: 20, riserHeight: 0.17 }] });
    expect(exact.isError).toBe(false);
    const two = await h.call("create_stairs", { stairs: [{ ...line, riserCount: 18, riserHeight: 0.17 }] });
    expect(two.isError).toBe(false);
    expect(h.requests.filter((r) => r.addOnCommand === "CreateElements")).toHaveLength(2);
  });

  it("modify_stairs uses the typed modify path", async () => {
    const h = await harness(typedResponder({ [G1]: "Stair" }));
    const res = await h.call("modify_stairs", { stairs: [{ guid: G1, width: 1.4, treadDepth: 0.28 }], undoName: "Wider stair" });
    expect(res.isError).toBe(false);
    const modify = h.requests.find((r) => r.addOnCommand === "ModifyElements");
    expect(modify?.addOnParameters).toEqual({ elements: [{ guid: G1, width: 1.4, treadDepth: 0.28 }], undoName: "Wider stair" });
  });
});

describe("railings", () => {
  it("create_railings sends a sloped polyline path", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G1, type: "Railing" }] }));
    const path = [{ x: 0, y: 0, z: 0 }, { x: 4, y: 0, z: 1.5 }, { x: 4, y: 2, z: 1.5 }];
    const res = await h.call("create_railings", { railings: [{ path, height: 1.1, referenceLine: "Center" }] });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [{ type: "Railing", path, height: 1.1, referenceLine: "Center" }],
      undoName: "Create railings (Claude)",
    });
  });

  it("modify_railings rejects non-railings per item", async () => {
    const h = await harness(typedResponder({ [G1]: "Railing", [G2]: "Stair" }));
    const res = await h.call("modify_railings", { railings: [{ guid: G1, height: 0.9 }, { guid: G2, height: 1 }] });
    expect(res.isError).toBe(false);
    const results = (res.json as { results: Array<{ guid: string; error?: { message: string } }> }).results;
    expect(results[0]).toEqual({ guid: G1 });
    expect(results[1]?.error?.message).toContain("is a Stair, not a Railing");
  });

  it("falls back to the add-on when the type check is unavailable", async () => {
    const h = await harness((req) => {
      if (req.command === "API.GetTypesOfElements") return { succeeded: false, error: { code: 1, message: "unavailable" } };
      return addonOk({ results: [{ guid: G1 }] });
    });
    const res = await h.call("modify_railings", { railings: [{ guid: G1, bottomOffset: 0.2 }] });
    expect(res.isError).toBe(false);
    expect(h.requests.some((r) => r.addOnCommand === "ModifyElements")).toBe(true);
    expect(res.json).toEqual({ results: [{ guid: G1 }] });
  });
});
