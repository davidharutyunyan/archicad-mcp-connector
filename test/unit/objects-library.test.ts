import { describe, expect, it } from "vitest";

import { addonOk, harness } from "../helpers.js";

const G1 = "92F5E7F6-2460-FF4F-A499-0B07286D2B5F";
const G2 = "1A2B3C4D-0000-1111-2222-333344445555";

const TOOL_NAMES = [
  "create_objects",
  "create_lamps",
  "search_library_parts",
  "get_library_part_subtypes",
  "get_library_part_details",
  "get_library_part_scripts",
  "create_library_part",
  "get_libraries",
  "add_libraries",
  "remove_libraries",
  "reload_libraries",
  "get_gdl_parameters",
  "set_gdl_parameters",
  "change_library_part",
];

describe("objects-library tool registration", () => {
  it("registers every tool of the family with a description", async () => {
    const h = await harness(() => addonOk({}));
    const tools = await h.listTools();
    for (const name of TOOL_NAMES) {
      const tool = tools.find((t) => t.name === name);
      expect(tool, name).toBeDefined();
      expect((tool?.description ?? "").length).toBeGreaterThan(80);
    }
  });

  it("documents GDL in create_library_part", async () => {
    const h = await harness(() => addonOk({}));
    const tool = (await h.listTools()).find((t) => t.name === "create_library_part");
    expect(tool?.description).toContain("PROJECT2 3, 270, 2");
    expect(tool?.description).toContain("PRISM_");
  });
});

describe("create_objects / create_lamps", () => {
  it("sends typed object specs to CreateElements and normalizes 2-D array params", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G1, type: "Object" }] }));
    const res = await h.call("create_objects", {
      objects: [
        {
          libraryPart: "Стол 01 26",
          position: { x: 1, y: 2 },
          elevation: 0.1,
          angle: 90,
          mirrored: true,
          sizeA: 1.6,
          sizeB: 0.8,
          height: 0.75,
          params: { gs_cont_pen: 3, matrix: [[1, 2], [3, 4]], list: [1, 2, 3], mat: "Дерево - Дуб" },
          overrideSurface: false,
          showOnStories: "AllRelevant",
          layer: "Мебель",
        },
      ],
    });
    expect(res.isError).toBe(false);
    expect(h.requests).toHaveLength(1);
    expect(h.requests[0]?.addOnCommand).toBe("CreateElements");
    expect(h.requests[0]?.addOnParameters).toEqual({
      undoName: "Create objects (Claude)",
      elements: [
        {
          type: "Object",
          libraryPart: "Стол 01 26",
          position: { x: 1, y: 2 },
          elevation: 0.1,
          angle: 90,
          mirrored: true,
          sizeA: 1.6,
          sizeB: 0.8,
          height: 0.75,
          params: { gs_cont_pen: 3, matrix: [{ values: [1, 2] }, { values: [3, 4] }], list: [1, 2, 3], mat: "Дерево - Дуб" },
          overrideSurface: false,
          showOnStories: "AllRelevant",
          layer: "Мебель",
        },
      ],
    });
    expect(res.json).toEqual({ results: [{ guid: G1, type: "Object" }] });
  });

  it("accepts {guid} library part references and custom story visibility", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G1, type: "Object" }] }));
    const res = await h.call("create_objects", {
      objects: [{ libraryPart: { guid: "{AAAA}-{BBBB}" }, position: { x: 0, y: 0 }, showOnStories: { home: true, above: 1 } }],
      undoName: "Place tables",
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toMatchObject({
      undoName: "Place tables",
      elements: [{ type: "Object", libraryPart: { guid: "{AAAA}-{BBBB}" }, showOnStories: { home: true, above: 1 } }],
    });
  });

  it("rejects objects without position or library part before calling Archicad", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("create_objects", { objects: [{ libraryPart: "X" }] })).isError).toBe(true);
    expect((await h.call("create_objects", { objects: [{ position: { x: 0, y: 0 } }] })).isError).toBe(true);
    expect((await h.call("create_objects", { objects: [{ libraryPart: "X", position: { x: 0, y: 0 }, sizeA: -1 }] })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("creates lamps with light fields", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G2, type: "Lamp" }] }));
    const res = await h.call("create_lamps", {
      lamps: [
        {
          libraryPart: "Светильник 01",
          position: { x: 3, y: 3 },
          elevation: 2.6,
          lightOn: true,
          lightColor: { red: 1, green: 0.9, blue: 0.8 },
          lightIntensity: 80,
        },
      ],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("CreateElements");
    expect(h.requests[0]?.addOnParameters).toMatchObject({
      undoName: "Create lamps (Claude)",
      elements: [
        {
          type: "Lamp",
          libraryPart: "Светильник 01",
          elevation: 2.6,
          lightOn: true,
          lightColor: { red: 1, green: 0.9, blue: 0.8 },
          lightIntensity: 80,
        },
      ],
    });
  });

  it("rejects out-of-range light colors", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("create_lamps", {
      lamps: [{ libraryPart: "L", position: { x: 0, y: 0 }, lightColor: { red: 2, green: 0, blue: 0 } }],
    });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("library part discovery", () => {
  it("search_library_parts forwards filters", async () => {
    const found = { libraryParts: [{ index: 12, name: "Стул 01 26", guid: "{A}-{B}", type: "Object" }], total: 1, offset: 0, hasMore: false };
    const h = await harness(() => addonOk(found));
    const res = await h.call("search_library_parts", {
      query: "стул",
      type: ["Object", "Lamp"],
      subtypeOf: "Furnishing",
      placeableOnly: true,
      limit: 20,
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("SearchLibraryParts");
    expect(h.requests[0]?.addOnParameters).toEqual({ query: "стул", type: ["Object", "Lamp"], subtypeOf: "Furnishing", placeableOnly: true, limit: 20 });
    expect(res.json).toEqual(found);
  });

  it("search_library_parts rejects unknown types", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("search_library_parts", { type: "Chair" });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("get_library_part_subtypes filters the query case-insensitively on the full list", async () => {
    const h = await harness(() =>
      addonOk({
        subtypes: [
          { name: "Мебель", path: "Общий Объект GDL > Элемент Модели > Мебель" },
          { name: "Стол", path: "Общий Объект GDL > Элемент Модели > Мебель > Стол" },
          { name: "Растение", path: "Общий Объект GDL > Элемент Модели > Растение" },
        ],
        total: 3,
      }),
    );
    const res = await h.call("get_library_part_subtypes", { query: "мебель", limit: 1, type: "Object" });
    expect(h.requests[0]?.addOnCommand).toBe("GetLibraryPartSubtypes");
    expect(h.requests[0]?.addOnParameters).toEqual({ type: "Object", limit: 2000 });
    expect(res.json).toEqual({ subtypes: [{ name: "Мебель", path: "Общий Объект GDL > Элемент Модели > Мебель" }], total: 2, offset: 0, hasMore: true });
  });

  it("get_library_part_subtypes without a query paginates in the add-on", async () => {
    const h = await harness(() => addonOk({ subtypes: [], total: 0 }));
    await h.call("get_library_part_subtypes", { offset: 10, limit: 50 });
    expect(h.requests[0]?.addOnParameters).toEqual({ offset: 10, limit: 50 });
  });

  it("get_library_part_details normalizes references to objects", async () => {
    const h = await harness(() => addonOk({ libraryParts: [] }));
    const res = await h.call("get_library_part_details", {
      libraryParts: ["Стул 01 26", 17, { guid: "{A}-{B}" }],
      includeValueLists: true,
      parameterNames: ["gs_cont_pen"],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("GetLibraryPartDetails");
    expect(h.requests[0]?.addOnParameters).toEqual({
      libraryParts: [{ name: "Стул 01 26" }, { index: 17 }, { guid: "{A}-{B}" }],
      includeValueLists: true,
      parameterNames: ["gs_cont_pen"],
    });
  });

  it("get_library_part_scripts forwards the script selection", async () => {
    const h = await harness(() => addonOk({ scripts: { script3D: "BLOCK A, B, ZZYZX" } }));
    const res = await h.call("get_library_part_scripts", { libraryPart: { index: 5 }, scripts: ["script3D", "comment"], maxLength: 1000 });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("GetLibraryPartScripts");
    expect(h.requests[0]?.addOnParameters).toEqual({ libraryPart: { index: 5 }, scripts: ["script3D", "comment"], maxLength: 1000 });
  });

  it("get_library_part_scripts rejects unknown script keys", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("get_library_part_scripts", { libraryPart: "X", scripts: ["3d"] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("create_library_part", () => {
  it("sends scripts and parameter definitions, converting 2-D array defaults to rows", async () => {
    const created = { libraryPart: { index: 9001, name: "Claude Table", guid: "{A}-{B}", type: "Object" }, overwritten: false };
    const h = await harness(() => addonOk(created));
    const res = await h.call("create_library_part", {
      name: "Claude Table",
      subtype: "Furnishing",
      scripts: { script3D: "BLOCK A, B, ZZYZX", parameterScript: 'VALUES "topThk" RANGE [0.02, 0.1]' },
      parameters: [
        { name: "topThk", type: "Length", value: 0.04, description: "Толщина столешницы" },
        { name: "mat", type: "Surface", value: "Дерево - Дуб" },
        { name: "grid", type: "RealNum", value: [[1, 0], [0, 1]] },
        { type: "Separator" },
      ],
      a: 1.6,
      b: 0.8,
      height: 0.75,
      overwrite: true,
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("CreateLibraryPart");
    expect(h.requests[0]?.addOnParameters).toEqual({
      name: "Claude Table",
      subtype: "Furnishing",
      scripts: { script3D: "BLOCK A, B, ZZYZX", parameterScript: 'VALUES "topThk" RANGE [0.02, 0.1]' },
      parameters: [
        { name: "topThk", type: "Length", value: 0.04, description: "Толщина столешницы" },
        { name: "mat", type: "Surface", value: "Дерево - Дуб" },
        { name: "grid", type: "RealNum", value: [{ values: [1, 0] }, { values: [0, 1] }] },
        { type: "Separator" },
      ],
      a: 1.6,
      b: 0.8,
      height: 0.75,
      overwrite: true,
    });
    expect(res.json).toEqual(created);
  });

  it("validates GDL parameter names and types locally", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("create_library_part", { name: "X", parameters: [{ name: "1bad", type: "Length" }] })).isError).toBe(true);
    expect((await h.call("create_library_part", { name: "X", parameters: [{ name: "ok", type: "Float" }] })).isError).toBe(true);
    expect((await h.call("create_library_part", { name: "X", scripts: { script3d: "BLOCK 1,1,1" }, type: "Chair" })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("surfaces add-on errors (e.g. name already used)", async () => {
    const h = await harness(() =>
      addonOk({ error: { code: -2130313030, message: "A library part named 'X' already exists. Pass overwrite: true to replace it" } }),
    );
    const res = await h.call("create_library_part", { name: "X", scripts: { script3D: "BLOCK 1, 1, 1" } });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("already exists");
  });
});

describe("libraries", () => {
  it("maps the library tools to their add-on commands", async () => {
    const h = await harness(() => addonOk({ libraries: [], libraryPartCount: 0 }));
    await h.call("get_libraries");
    await h.call("add_libraries", { paths: ["~/Documents/My Objects"] });
    await h.call("remove_libraries", { libraries: ["My Objects"] });
    await h.call("reload_libraries");
    expect(h.requests.map((r) => r.addOnCommand)).toEqual(["GetLibraries", "AddLibraries", "RemoveLibraries", "ReloadLibraries"]);
    expect(h.requests[1]?.addOnParameters).toEqual({ paths: ["~/Documents/My Objects"] });
    expect(h.requests[2]?.addOnParameters).toEqual({ libraries: ["My Objects"] });
  });

  it("requires at least one path", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("add_libraries", { paths: [] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("GDL parameters of placed elements", () => {
  it("get_gdl_parameters converts element refs to GUID strings", async () => {
    const h = await harness(() => addonOk({ elements: [] }));
    const res = await h.call("get_gdl_parameters", {
      elements: [G1, { guid: G2 }],
      search: "pen",
      includeValueLists: true,
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("GetGdlParameters");
    // 'search' is applied client-side (the add-on build of AC26 crashed on empty descriptions)
    expect(h.requests[0]?.addOnParameters).toEqual({ elements: [G1, G2], includeValueLists: true });
  });

  it("get_gdl_parameters filters by name or description, case-insensitively", async () => {
    const h = await harness(() =>
      addonOk({
        elements: [
          {
            guid: G1,
            parameters: [
              { name: "gs_cont_pen", description: "Перо Контура", value: 1 },
              { name: "A", description: "", value: 1 },
              { name: "matTop", description: "Покрытие Столешницы", value: 3 },
            ],
            parameterCount: 3,
          },
          { error: { code: 1, message: "not found" } },
        ],
      }),
    );
    const res = await h.call("get_gdl_parameters", { elements: [G1, G2], search: "ПОКРЫТИЕ" });
    expect(res.json).toEqual({
      elements: [
        { guid: G1, parameters: [{ name: "matTop", description: "Покрытие Столешницы", value: 3 }], parameterCount: 1 },
        { error: { code: 1, message: "not found" } },
      ],
    });
  });

  it("rejects empty GDL parameter names", async () => {
    const h = await harness(() => addonOk({ results: [] }));
    const res = await h.call("set_gdl_parameters", { elements: [{ guid: G1, params: { "": 1 } }] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("set_gdl_parameters merges common params under per-element params", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G1 }, { guid: G2 }] }));
    const res = await h.call("set_gdl_parameters", {
      elements: [{ guid: G1, params: { A: 1.2 } }, { guid: G2 }],
      params: { A: 0.9, gs_cont_pen: 5, grid: [[1, 2]] },
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("SetGdlParameters");
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [
        { guid: G1, params: { A: 1.2, gs_cont_pen: 5, grid: [{ values: [1, 2] }] } },
        { guid: G2, params: { A: 0.9, gs_cont_pen: 5, grid: [{ values: [1, 2] }] } },
      ],
    });
  });

  it("set_gdl_parameters fails without any params", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("set_gdl_parameters", { elements: [{ guid: G1 }] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("No parameters");
    expect(h.requests).toHaveLength(0);
  });

  it("set_gdl_parameters reports per-element add-on errors in the result", async () => {
    const h = await harness(() =>
      addonOk({ results: [{ error: { code: -2130313111, message: "GDL parameter 'foo' does not exist in this library part." } }] }),
    );
    const res = await h.call("set_gdl_parameters", { elements: [{ guid: G1, params: { foo: 1 } }] });
    expect(res.isError).toBe(false);
    expect(res.text).toContain("does not exist");
  });

  it("change_library_part applies top-level defaults", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G1 }, { guid: G2 }] }));
    const res = await h.call("change_library_part", {
      elements: [{ guid: G1 }, { guid: G2, libraryPart: { guid: "{C}-{D}" }, keepSize: false, params: { m: [[1]] } }],
      libraryPart: "Дверь 01",
      keepParameters: false,
      keepSize: true,
      undoName: "Swap",
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("ChangeLibraryPart");
    expect(h.requests[0]?.addOnParameters).toEqual({
      undoName: "Swap",
      elements: [
        { guid: G1, libraryPart: "Дверь 01", keepParameters: false, keepSize: true },
        { guid: G2, libraryPart: { guid: "{C}-{D}" }, keepParameters: false, keepSize: false, params: { m: [{ values: [1] }] } },
      ],
    });
  });

  it("change_library_part requires a library part", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("change_library_part", { elements: [{ guid: G1 }] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("No libraryPart");
    expect(h.requests).toHaveLength(0);
  });

  it("surfaces whole-command add-on errors", async () => {
    const h = await harness(() => addonOk({ error: { code: -2130313112, message: "Missing required array field 'elements'." } }));
    const res = await h.call("get_gdl_parameters", { elements: [G1] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("Missing required array field");
  });
});
