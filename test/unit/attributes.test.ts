import { describe, expect, it } from "vitest";

import { addonOk, harness } from "../helpers.js";

const GUID = "92F5E7F6-2460-FF4F-A499-0B07286D2B5F";
const ok = () => addonOk({ results: [{ index: 12, name: "CC-test-attributes-x", guid: GUID, created: true }] });

const ATTRIBUTE_TOOLS = [
  "get_attributes",
  "create_layers",
  "create_layer_combinations",
  "create_composites",
  "create_building_materials",
  "create_surfaces",
  "create_fills",
  "create_line_types",
  "create_zone_categories",
  "create_profiles",
  "duplicate_attributes",
  "modify_attributes",
  "delete_attributes",
  "modify_pens",
  "apply_layer_combination",
  "set_layer_states",
];

describe("attribute tool registration", () => {
  it("registers every attribute tool with a description and an input schema", async () => {
    const h = await harness(() => addonOk({}));
    const tools = await h.listTools();
    for (const name of ATTRIBUTE_TOOLS) {
      const tool = tools.find((t) => t.name === name);
      expect(tool, name).toBeDefined();
      expect(tool!.description!.length, name).toBeGreaterThan(80);
      expect(tool!.inputSchema.type).toBe("object");
    }
    const modify = tools.find((t) => t.name === "modify_attributes")!;
    expect(JSON.stringify(modify.inputSchema)).toContain("BuildingMaterial");
    expect(tools.find((t) => t.name === "get_attributes")!.annotations?.readOnlyHint).toBe(true);
    expect(tools.find((t) => t.name === "delete_attributes")!.annotations?.destructiveHint).toBe(true);
  });
});

describe("get_attributes", () => {
  it("sends type, filter and paging and normalizes attribute refs to objects", async () => {
    const h = await harness(() => addonOk({ type: "Layer", total: 1, attributes: [] }));
    const res = await h.call("get_attributes", {
      type: "Layer",
      nameFilter: "Стены*",
      detailed: true,
      offset: 10,
      limit: 50,
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.command).toBe("API.ExecuteAddOnCommand");
    expect(h.requests[0]?.addOnCommand).toBe("GetAttributes");
    expect(h.requests[0]?.addOnParameters).toEqual({ type: "Layer", nameFilter: "Стены*", detailed: true, offset: 10, limit: 50 });

    await h.call("get_attributes", { type: "Composite", attributes: [3, "Кирпич 250", { guid: GUID }] });
    await h.call("get_attributes", { type: "Surface" });
    expect(h.requests[2]?.addOnParameters).toEqual({ type: "Surface", limit: 300 });
    expect(h.requests[1]?.addOnParameters).toEqual({
      type: "Composite",
      attributes: [{ index: 3 }, { name: "Кирпич 250" }, { guid: GUID }],
    });
  });

  it("asks for per-type counts when no type is given", async () => {
    const h = await harness(() => addonOk({ types: [{ type: "Layer", count: 40 }] }));
    const res = await h.call("get_attributes", {});
    expect(h.requests[0]?.addOnParameters).toEqual({});
    expect(res.json).toEqual({ types: [{ type: "Layer", count: 40 }] });
  });

  it("rejects unknown attribute types and filters without a type before calling Archicad", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("get_attributes", { type: "Material3D" });
    expect(res.isError).toBe(true);
    const noType = await h.call("get_attributes", { nameFilter: "Бетон" });
    expect(noType.isError).toBe(true);
    expect(noType.text).toContain("Pass 'type'");
    expect(h.requests).toHaveLength(0);
  });
});

describe("create tools", () => {
  it("create_layers sends CreateAttributes with type Layer", async () => {
    const h = await harness(ok);
    const res = await h.call("create_layers", {
      layers: [
        { name: "CC-test-attributes-Layer A", locked: true, intersectionGroup: 3, folder: ["CC-test-attributes"] },
        { name: "CC-test-attributes-Layer B", visible: false },
      ],
      ifExists: "skip",
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("CreateAttributes");
    expect(h.requests[0]?.addOnParameters).toEqual({
      type: "Layer",
      ifExists: "skip",
      undoName: "Create layers (Claude)",
      attributes: [
        { name: "CC-test-attributes-Layer A", locked: true, intersectionGroup: 3, folder: ["CC-test-attributes"] },
        { name: "CC-test-attributes-Layer B", visible: false },
      ],
    });
    expect(res.json).toEqual({ results: [{ index: 12, name: "CC-test-attributes-x", guid: GUID, created: true }] });
  });

  it("create_layer_combinations passes base and ordered layer states", async () => {
    const h = await harness(ok);
    await h.call("create_layer_combinations", {
      layerCombinations: [
        {
          name: "CC-test-attributes-Combo",
          base: "allHidden",
          layers: [{ match: "Конструктив*", visible: true }, { layer: 5, locked: true }],
        },
      ],
    });
    expect(h.requests[0]?.addOnParameters).toMatchObject({
      type: "LayerCombination",
      attributes: [
        { name: "CC-test-attributes-Combo", base: "allHidden", layers: [{ match: "Конструктив*", visible: true }, { layer: 5, locked: true }] },
      ],
    });
  });

  it("rejects a layer state without layer or match", async () => {
    const h = await harness(ok);
    const res = await h.call("create_layer_combinations", {
      layerCombinations: [{ name: "CC-test-attributes-Combo", layers: [{ hidden: true }] }],
    });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("create_composites sends skins, skin lines and usage", async () => {
    const h = await harness(ok);
    const composite = {
      name: "CC-test-attributes-Wall",
      skins: [
        { thickness: 0.02, buildingMaterial: "Штукатурка", finish: true },
        { thickness: 0.25, buildingMaterial: 7, core: true, endLinePen: 3 },
      ],
      skinLines: [{ lineType: 1, pen: 1 }, { pen: 2 }, { lineType: "Сплошная линия", pen: 1 }],
      usage: { walls: true, slabs: false },
    };
    await h.call("create_composites", { composites: [composite] });
    expect(h.requests[0]?.addOnParameters).toMatchObject({ type: "Composite", attributes: [composite] });
  });

  it("rejects non-positive skin thickness", async () => {
    const h = await harness(ok);
    const res = await h.call("create_composites", {
      composites: [{ name: "CC-test-attributes-Bad", skins: [{ thickness: 0, buildingMaterial: 1 }] }],
    });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("create_building_materials passes physical and graphic fields", async () => {
    const h = await harness(ok);
    const bm = {
      name: "CC-test-attributes-BM",
      basedOn: "Бетон",
      cutFill: "50 %",
      cutFillPen: 3,
      cutFillBackgroundPen: 0,
      surface: { index: 4 },
      cutFillOrientation: "FitToSkin",
      priority: 800,
      thermalConductivity: 1.7,
      density: 2400,
      heatCapacity: 880,
      participatesInCollisionDetection: true,
    };
    await h.call("create_building_materials", { buildingMaterials: [bm], ifExists: "update" });
    expect(h.requests[0]?.addOnParameters).toMatchObject({ type: "BuildingMaterial", ifExists: "update", attributes: [bm] });
  });

  it("create_surfaces validates colors", async () => {
    const h = await harness(ok);
    const res = await h.call("create_surfaces", {
      surfaces: [{ name: "CC-test-attributes-Red", color: "#B22222", transparency: 20, texture: false, fill: false }],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toMatchObject({
      type: "Surface",
      attributes: [{ name: "CC-test-attributes-Red", color: "#B22222", transparency: 20, texture: false, fill: false }],
    });
    const bad = await h.call("create_surfaces", { surfaces: [{ name: "CC-test-attributes-Bad", color: "red" }] });
    expect(bad.isError).toBe(true);
    expect(h.requests).toHaveLength(1);
  });

  it("create_fills passes vector hatch lines and bitmap patterns", async () => {
    const h = await harness(ok);
    const fills = [
      { name: "CC-test-attributes-Diag", fillType: "Vector", lines: [{ angle: 45, spacing: 2, dashes: [3, 1] }], usage: { cut: true } },
      { name: "CC-test-attributes-50", fillType: "Solid", bitmapPattern: "55AA55AA55AA55AA" },
    ];
    await h.call("create_fills", { fills });
    expect(h.requests[0]?.addOnParameters).toMatchObject({ type: "Fill", attributes: fills });
    const bad = await h.call("create_fills", { fills: [{ name: "CC-test-attributes-Bad", bitmapPattern: "XYZ" }] });
    expect(bad.isError).toBe(true);
  });

  it("create_line_types sends dashes", async () => {
    const h = await harness(ok);
    await h.call("create_line_types", {
      lineTypes: [{ name: "CC-test-attributes-Dash", dashes: [{ dash: 3, gap: 1.5 }], scaleWithPlan: false }],
    });
    expect(h.requests[0]?.addOnParameters).toMatchObject({
      type: "Line",
      attributes: [{ name: "CC-test-attributes-Dash", dashes: [{ dash: 3, gap: 1.5 }], scaleWithPlan: false }],
    });
  });

  it("create_zone_categories sends code, color and stamp", async () => {
    const h = await harness(ok);
    await h.call("create_zone_categories", {
      zoneCategories: [{ name: "CC-test-attributes-Zone", code: "CC", color: { red: 1, green: 0.5, blue: 0 }, stamp: "Штамп Зоны 26" }],
    });
    expect(h.requests[0]?.addOnParameters).toMatchObject({
      type: "ZoneCategory",
      attributes: [{ name: "CC-test-attributes-Zone", code: "CC", color: { red: 1, green: 0.5, blue: 0 }, stamp: "Штамп Зоны 26" }],
    });
  });

  it("create_profiles sends shapes with polygons", async () => {
    const h = await harness(ok);
    const profile = {
      name: "CC-test-attributes-Profile",
      usage: { beams: true, columns: true },
      shapes: [
        {
          polygon: [
            { x: -0.15, y: 0 },
            { x: 0.15, y: 0 },
            { x: 0.15, y: 0.5 },
            { x: -0.15, y: 0.5 },
          ],
          buildingMaterial: "Бетон",
          core: true,
        },
      ],
    };
    await h.call("create_profiles", { profiles: [profile] });
    expect(h.requests[0]?.addOnParameters).toMatchObject({ type: "Profile", attributes: [profile] });
  });
});

describe("duplicate_attributes", () => {
  it("maps source to basedOn and forwards type-specific fields", async () => {
    const h = await harness(ok);
    await h.call("duplicate_attributes", {
      type: "PenTable",
      attributes: [{ source: "Архитектурный", name: "CC-test-attributes-Pens", pens: [{ index: 1, color: "#FF0000" }] }],
    });
    expect(h.requests[0]?.addOnParameters).toEqual({
      type: "PenTable",
      undoName: "Duplicate PenTable attributes (Claude)",
      attributes: [{ basedOn: "Архитектурный", name: "CC-test-attributes-Pens", pens: [{ index: 1, color: "#FF0000" }] }],
    });
  });

  it("refuses pens and fonts", async () => {
    const h = await harness(ok);
    const res = await h.call("duplicate_attributes", { type: "Pen", attributes: [{ source: 1, name: "x" }] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("modify_attributes", () => {
  it("sends typed patches for several attribute types", async () => {
    const h = await harness(() => addonOk({ results: [{ index: 1 }, { index: 2 }, { index: 12 }] }));
    const items = [
      { type: "Layer", attribute: "CC-test-attributes-Layer A", locked: false, name: "CC-test-attributes-Layer C" },
      { type: "Surface", attribute: { guid: GUID }, color: "#AA0000", folder: "CC/test" },
      { type: "Pen", attribute: 12, color: "#FF0000", width: 0.5 },
      { type: "LayerCombination", attribute: 3, base: "current" },
    ];
    const res = await h.call("modify_attributes", { attributes: items });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("ModifyAttributes");
    expect(h.requests[0]?.addOnParameters).toEqual({ attributes: items });
  });

  it("rejects fields of another type and unknown types", async () => {
    const h = await harness(ok);
    const wrong = await h.call("modify_attributes", { attributes: [{ type: "Font", attribute: 1 }] });
    expect(wrong.isError).toBe(true);
    const badPen = await h.call("modify_attributes", { attributes: [{ type: "Pen", attribute: 300, width: 0.2 }] });
    expect(badPen.isError).toBe(true);
    const foreignField = await h.call("modify_attributes", { attributes: [{ type: "Layer", attribute: "A", color: "#FF0000" }] });
    expect(foreignField.isError).toBe(true);
    expect(foreignField.text).toContain("unknown field(s) for Layer: color");
    const badValue = await h.call("modify_attributes", { attributes: [{ type: "Surface", attribute: "A", color: "red" }] });
    expect(badValue.isError).toBe(true);
    const penName = await h.call("modify_attributes", { attributes: [{ type: "Pen", attribute: 3, name: "x" }] });
    expect(penName.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("keeps the tool definition compact", async () => {
    const h = await harness(ok);
    const tool = (await h.listTools()).find((t) => t.name === "modify_attributes")!;
    expect(JSON.stringify(tool.inputSchema).length).toBeLessThan(8000);
    expect(JSON.stringify(tool.inputSchema)).toContain("Composite: skins, skinLines, usage");
  });
});

describe("delete_attributes", () => {
  it("normalizes refs and forwards force", async () => {
    const h = await harness(() => addonOk({ results: [{ index: 12, deleted: true }] }));
    await h.call("delete_attributes", { type: "Layer", attributes: ["CC-test-attributes-Layer A", 12], force: true });
    expect(h.requests[0]?.addOnCommand).toBe("DeleteAttributes");
    expect(h.requests[0]?.addOnParameters).toEqual({
      type: "Layer",
      attributes: [{ name: "CC-test-attributes-Layer A" }, { index: 12 }],
      force: true,
    });
  });

  it("refuses to delete pens", async () => {
    const h = await harness(ok);
    const res = await h.call("delete_attributes", { type: "Pen", attributes: [1] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("surfaces the add-on error message", async () => {
    const h = await harness(() =>
      addonOk({ error: { code: -2130313111, message: "Layer 'Walls' holds 3 element(s) that would be DELETED with it." } }),
    );
    const res = await h.call("delete_attributes", { type: "Layer", attributes: ["Walls"] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("would be DELETED");
  });
});

describe("pens and layer states", () => {
  it("modify_pens targets a pen table when given", async () => {
    const h = await harness(() => addonOk({ results: [{ index: 5, color: "#00FF00", width: 0.35 }] }));
    await h.call("modify_pens", { pens: [{ index: 5, color: "#00FF00", width: 0.35 }], penTable: "CC-test-attributes-Pens" });
    expect(h.requests[0]?.addOnCommand).toBe("ModifyPens");
    expect(h.requests[0]?.addOnParameters).toEqual({
      pens: [{ index: 5, color: "#00FF00", width: 0.35 }],
      penTable: "CC-test-attributes-Pens",
    });
    const bad = await h.call("modify_pens", { pens: [{ index: 0, width: 0.1 }] });
    expect(bad.isError).toBe(true);
  });

  it("apply_layer_combination sends the combination reference", async () => {
    const h = await harness(() => addonOk({ ok: true }));
    await h.call("apply_layer_combination", { layerCombination: "CC-test-attributes-Combo" });
    expect(h.requests[0]?.addOnCommand).toBe("ApplyLayerCombination");
    expect(h.requests[0]?.addOnParameters).toEqual({ layerCombination: "CC-test-attributes-Combo" });
  });

  it("set_layer_states sends ordered layer items", async () => {
    const h = await harness(() => addonOk({ results: [] }));
    const layers = [
      { match: "*", hidden: true },
      { layer: "CC-test-attributes-Layer A", visible: true, wireframe: false },
    ];
    await h.call("set_layer_states", { layers });
    expect(h.requests[0]?.addOnCommand).toBe("SetLayerStates");
    expect(h.requests[0]?.addOnParameters).toEqual({ layers });
    const bad = await h.call("set_layer_states", { layers: [{ locked: true }] });
    expect(bad.isError).toBe(true);
    expect(h.requests).toHaveLength(1);
  });
});
