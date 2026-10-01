import { mkdtemp, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { describe, expect, it } from "vitest";

import { addonOk, apiOk, harness, type RecordedRequest } from "../helpers.js";

const G1 = "7E221F33-829B-4FBC-A670-E74DABCE6289";
const G2 = "20651FB7-60CE-D54C-8B66-E68287AC6791";
const E1 = "57E66B6A-CB0E-9244-B21B-D09850B94378";
const E2 = "619831BE-A41B-4943-AB98-7E166F901DFF";
const AREA = "AC5CCA52-F79B-4850-92A9-BED7CB7C3847";

/** Responder that answers the official built-in name lookups and echoes add-on calls. */
function officialAware(addonResponse: (req: RecordedRequest) => unknown = () => ({ ok: true })) {
  return (req: RecordedRequest) => {
    if (req.command === "API.GetPropertyIds") {
      const props = (req.parameters?.["properties"] as Array<{ nonLocalizedName: string }>) ?? [];
      return apiOk({
        properties: props.map((p) =>
          p.nonLocalizedName === "General_ElementID"
            ? { propertyId: { guid: G1 } }
            : p.nonLocalizedName === "General_Area"
              ? { propertyId: { guid: AREA } }
              : { error: { code: 4005, message: "Property definition not found" } },
        ),
      });
    }
    if (req.command === "API.GetAllPropertyNames") {
      return apiOk({
        properties: [
          { type: "BuiltIn", nonLocalizedName: "General_ElementID" },
          { type: "BuiltIn", nonLocalizedName: "General_Area" },
          { type: "UserDefined", localizedName: ["ЗОНЫ", "Предназначение"] },
        ],
      });
    }
    return addonOk(addonResponse(req));
  };
}

function addonCalls(h: { requests: RecordedRequest[] }) {
  return h.requests.filter((r) => r.command === "API.ExecuteAddOnCommand");
}

describe("tool registration", () => {
  it("registers every property-family tool with a valid schema", async () => {
    const h = await harness(() => addonOk({}));
    const names = (await h.listTools()).map((t) => t.name);
    for (const name of [
      "get_property_definitions",
      "create_property_groups",
      "modify_property_groups",
      "delete_property_groups",
      "create_property_definitions",
      "modify_property_definitions",
      "delete_property_definitions",
      "import_property_definitions_xml",
      "get_property_values",
      "set_property_values",
      "get_attribute_property_values",
      "set_attribute_property_values",
      "get_ifc_data",
      "set_ifc_properties",
      "create_classification_system",
      "create_classification_items",
      "modify_classification_items",
      "delete_classification_items",
      "modify_classification_system",
      "delete_classification_systems",
      "import_classifications_xml",
    ]) {
      expect(names).toContain(name);
    }
  });
});

describe("get_property_definitions", () => {
  it("forwards filters, converts element refs and annotates built-in names", async () => {
    const h = await harness(
      officialAware(() => ({
        definitions: [
          { guid: G1.toLowerCase(), name: "ID Элемента", group: "Общие Параметры", type: "string", kind: "BuiltIn" },
          { guid: G2, name: "Предназначение", group: "ЗОНЫ", type: "multiEnum", kind: "Custom" },
        ],
        total: 2,
        offset: 0,
        returned: 2,
        hasMore: false,
      })),
    );
    const res = await h.call("get_property_definitions", {
      search: "Площадь",
      kind: "All",
      elements: [E1, { guid: E2 }],
      elementMatch: "any",
      detail: "full",
      limit: 50,
    });
    expect(res.isError).toBe(false);
    const calls = addonCalls(h);
    expect(calls).toHaveLength(1);
    expect(calls[0]?.addOnCommand).toBe("GetPropertyDefinitions");
    expect(calls[0]?.addOnParameters).toEqual({ search: "Площадь", kind: "All", elements: [E1, E2], elementMatch: "any", detail: "full", limit: 50 });
    const json = res.json as { definitions: Array<Record<string, unknown>> };
    expect(json.definitions[0]?.["builtInName"]).toBe("General_ElementID");
    expect(json.definitions[1]?.["builtInName"]).toBeUndefined();
  });

  it("skips the built-in name lookup when disabled", async () => {
    const h = await harness(officialAware(() => ({ definitions: [{ guid: G1, kind: "BuiltIn" }] })));
    const res = await h.call("get_property_definitions", { includeBuiltInNames: false });
    expect(res.isError).toBe(false);
    expect(h.requests.map((r) => r.command)).toEqual(["API.ExecuteAddOnCommand"]);
    expect(h.requests[0]?.addOnParameters).toEqual({});
  });

  it("resolves {builtIn} references through API.GetPropertyIds", async () => {
    const h = await harness(officialAware(() => ({ definitions: [] })));
    const res = await h.call("get_property_definitions", {
      properties: [{ builtIn: "General_ElementID" }, "ЗОНЫ/Предназначение", { group: "ЗОНЫ", name: "Тип" }],
      includeBuiltInNames: false,
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.command).toBe("API.GetPropertyIds");
    expect(h.requests[0]?.parameters).toEqual({ properties: [{ type: "BuiltIn", nonLocalizedName: "General_ElementID" }] });
    expect(addonCalls(h)[0]?.addOnParameters).toEqual({
      properties: [{ guid: G1 }, "ЗОНЫ/Предназначение", { group: "ЗОНЫ", name: "Тип" }],
    });
  });

  it("reports unknown built-in names without calling the add-on", async () => {
    const h = await harness(officialAware());
    const res = await h.call("get_property_definitions", { properties: [{ builtIn: "General_Nope" }] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("General_Nope");
    expect(addonCalls(h)).toHaveLength(0);
  });

  it("surfaces add-on errors", async () => {
    const h = await harness(() => addonOk({ error: { code: -2130312312, message: "Unknown property 'X'. Similar: \"A/X1\"." } }));
    const res = await h.call("get_property_definitions", { properties: ["X"] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("Unknown property 'X'");
  });
});

describe("property groups", () => {
  it("create_property_groups forwards the groups", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G1, name: "CC-test-Group" }] }));
    const res = await h.call("create_property_groups", { groups: [{ name: "CC-test-Group", description: "d" }], undoName: "u" });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("CreatePropertyGroups");
    expect(h.requests[0]?.addOnParameters).toEqual({ groups: [{ name: "CC-test-Group", description: "d" }], undoName: "u" });
  });

  it("modify_property_groups rejects items without changes", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("modify_property_groups", { groups: [{ group: "CC-test-Group" }] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("give name and/or description");
    expect(h.requests).toHaveLength(0);
  });

  it("modify_property_groups forwards renames", async () => {
    const h = await harness(() => addonOk({ results: [] }));
    await h.call("modify_property_groups", { groups: [{ group: { guid: G1 }, name: "CC-test-Renamed" }] });
    expect(h.requests[0]?.addOnCommand).toBe("ModifyPropertyGroups");
    expect(h.requests[0]?.addOnParameters).toEqual({ groups: [{ group: { guid: G1 }, name: "CC-test-Renamed" }] });
  });

  it("delete_property_groups forwards deleteDefinitions", async () => {
    const h = await harness(() => addonOk({ results: [] }));
    await h.call("delete_property_groups", { groups: ["CC-test-Group", { guid: G1 }], deleteDefinitions: true });
    expect(h.requests[0]?.addOnCommand).toBe("DeletePropertyGroups");
    expect(h.requests[0]?.addOnParameters).toEqual({ groups: ["CC-test-Group", { guid: G1 }], deleteDefinitions: true });
  });
});

describe("create_property_definitions", () => {
  it("sends typed definitions and converts element refs", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G2, name: "Fire", group: "CC-test-Group", type: "singleEnum" }] }));
    const res = await h.call("create_property_definitions", {
      definitions: [
        {
          group: "CC-test-Group",
          name: "Fire",
          type: "singleEnum",
          enumValues: ["REI 30", "REI 60", { value: "REI 90", nonLocalizedValue: "R90" }],
          defaultValue: "REI 60",
          availability: "all",
        },
        { group: "CC-test-Group", name: "Len", type: "length", defaultValue: 2.5, availableForElements: [{ guid: E1 }, E2] },
        { group: "CC-test-Group", name: "Expr", type: "area", defaultExpressions: ["{Property:Общие Параметры/Площадь} * 2"], availability: [{ system: "Классификация Archicad" }, "Стена"] },
      ],
      createMissingGroups: false,
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("CreatePropertyDefinitions");
    expect(h.requests[0]?.addOnParameters).toEqual({
      definitions: [
        {
          group: "CC-test-Group",
          name: "Fire",
          type: "singleEnum",
          enumValues: ["REI 30", "REI 60", { value: "REI 90", nonLocalizedValue: "R90" }],
          defaultValue: "REI 60",
          availability: "all",
        },
        { group: "CC-test-Group", name: "Len", type: "length", defaultValue: 2.5, availableForElements: [E1, E2] },
        {
          group: "CC-test-Group",
          name: "Expr",
          type: "area",
          defaultExpressions: ["{Property:Общие Параметры/Площадь} * 2"],
          availability: [{ system: "Классификация Archicad" }, "Стена"],
        },
      ],
      createMissingGroups: false,
    });
  });

  it("rejects option sets without options and options on scalar types", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("create_property_definitions", {
      definitions: [
        { group: "G", name: "A", type: "multiEnum" },
        { group: "G", name: "B", type: "string", enumValues: ["x"] },
        { group: "G", name: "C", type: "integer", defaultValue: 1, defaultExpressions: ["1"] },
      ],
    });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("needs enumValues");
    expect(res.text).toContain("enumValues only apply");
    expect(res.text).toContain("defaultValue OR defaultExpressions");
    expect(h.requests).toHaveLength(0);
  });

  it("expands {builtIn:...} expression tokens to {ref:GUID}", async () => {
    const h = await harness(officialAware(() => ({ results: [] })));
    const res = await h.call("create_property_definitions", {
      definitions: [
        {
          group: "CC-test-Group",
          name: "Area110",
          type: "area",
          defaultExpressions: ["{builtIn:General_Area} * 1.1", "{ builtIn: General_Area } + {ref:CC-test-Group/Len}"],
        },
        { group: "CC-test-Group", name: "Plain", type: "string", defaultExpressions: ["CONCAT ( \"a\", \"b\" )"] },
      ],
    });
    expect(res.isError).toBe(false);
    const idCalls = h.requests.filter((r) => r.command === "API.GetPropertyIds");
    expect(idCalls).toHaveLength(1);
    expect(idCalls[0]?.parameters).toEqual({ properties: [{ type: "BuiltIn", nonLocalizedName: "General_Area" }] });
    const defs = addonCalls(h)[0]?.addOnParameters?.["definitions"] as Array<Record<string, unknown>>;
    expect(defs[0]?.["defaultExpressions"]).toEqual([`{ref:${AREA}} * 1.1`, `{ref:${AREA}} + {ref:CC-test-Group/Len}`]);
    expect(defs[1]?.["defaultExpressions"]).toEqual(['CONCAT ( "a", "b" )']);
  });

  it("fails on unknown {builtIn:...} expression tokens", async () => {
    const h = await harness(officialAware());
    const res = await h.call("create_property_definitions", {
      definitions: [{ group: "G", name: "X", type: "number", defaultExpressions: ["{builtIn:General_Nope} * 2"] }],
    });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("General_Nope");
    expect(addonCalls(h)).toHaveLength(0);
  });

  it("rejects unknown value types via the schema", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("create_property_definitions", { definitions: [{ group: "G", name: "A", type: "date" }] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("modify / delete property definitions", () => {
  it("modify_property_definitions forwards option edits and availability", async () => {
    const h = await harness(officialAware(() => ({ results: [] })));
    const res = await h.call("modify_property_definitions", {
      definitions: [
        {
          property: "CC-test-Group/Fire",
          addEnumValues: ["REI 120"],
          renameEnumValues: [{ from: "REI 30", to: "REI 45" }],
          removeAvailability: "all",
          availableForElements: [E1],
        },
      ],
    });
    expect(res.isError).toBe(false);
    expect(addonCalls(h)[0]?.addOnCommand).toBe("ModifyPropertyDefinitions");
    expect(addonCalls(h)[0]?.addOnParameters).toEqual({
      definitions: [
        {
          property: "CC-test-Group/Fire",
          addEnumValues: ["REI 120"],
          renameEnumValues: [{ from: "REI 30", to: "REI 45" }],
          removeAvailability: "all",
          availableForElements: [E1],
        },
      ],
    });
  });

  it("modify_property_definitions expands expression tokens and accepts {name} refs", async () => {
    const h = await harness(officialAware(() => ({ results: [] })));
    const res = await h.call("modify_property_definitions", {
      definitions: [{ property: { name: "Area110" }, defaultExpressions: ["{builtIn:General_Area} * 1.2"] }],
    });
    expect(res.isError).toBe(false);
    expect(addonCalls(h)[0]?.addOnParameters).toEqual({
      definitions: [{ property: { name: "Area110" }, defaultExpressions: [`{ref:${AREA}} * 1.2`] }],
    });
  });

  it("modify_property_definitions rejects empty and conflicting changes", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("modify_property_definitions", {
      definitions: [{ property: G1 }, { property: G2, defaultValue: 1, defaultUndefined: true }],
    });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("definitions[0]: nothing to change");
    expect(res.text).toContain("definitions[1]: give only one of");
    expect(h.requests).toHaveLength(0);
  });

  it("delete_property_definitions resolves built-in refs and forwards", async () => {
    const h = await harness(officialAware(() => ({ results: [{ error: { code: 1, message: "built-in" } }] })));
    const res = await h.call("delete_property_definitions", { properties: ["CC-test-Group/Fire", { builtIn: "General_Area" }] });
    expect(res.isError).toBe(false);
    expect(addonCalls(h)[0]?.addOnParameters).toEqual({ properties: ["CC-test-Group/Fire", { guid: AREA }] });
  });

  it("import_property_definitions_xml reads a file path", async () => {
    const dir = await mkdtemp(join(tmpdir(), "cc-props-"));
    const file = join(dir, "props.xml");
    await writeFile(file, "<BuildingInformation/>", "utf8");
    const h = await harness(() => addonOk({ created: [], groupsCreated: [], definitionCount: 1 }));
    const res = await h.call("import_property_definitions_xml", { filePath: file, conflictPolicy: "Replace" });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("ImportPropertyDefinitionsXml");
    expect(h.requests[0]?.addOnParameters).toEqual({ xml: "<BuildingInformation/>", conflictPolicy: "Replace" });
  });

  it("import_property_definitions_xml needs exactly one source", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("import_property_definitions_xml", {});
    expect(res.isError).toBe(true);
    expect(res.text).toContain("exactly one of xml or filePath");
  });
});

describe("property values", () => {
  it("get_property_values builds the request with resolved refs", async () => {
    const table = { properties: [{ guid: G1, name: "ID Элемента", group: "Общие Параметры", type: "string" }], results: [{ guid: E1, values: [{ value: "W-1" }] }] };
    const h = await harness(officialAware(() => table));
    const res = await h.call("get_property_values", {
      elements: [{ guid: E1 }],
      elementDefaults: ["Wall"],
      properties: [{ builtIn: "General_ElementID" }, "ЗОНЫ/Предназначение"],
      includeDisplay: false,
    });
    expect(res.isError).toBe(false);
    expect(addonCalls(h)[0]?.addOnCommand).toBe("GetPropertyValues");
    expect(addonCalls(h)[0]?.addOnParameters).toEqual({
      elements: [E1],
      elementDefaults: ["Wall"],
      properties: [{ guid: G1 }, "ЗОНЫ/Предназначение"],
      includeDisplay: false,
    });
    expect(res.json).toEqual(table);
  });

  it("get_property_values needs a target", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("get_property_values", { properties: [G1] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("set_property_values validates modes and forwards entries", async () => {
    const h = await harness(officialAware(() => ({ results: [], succeeded: 3, failed: 0 })));
    const res = await h.call("set_property_values", {
      values: [
        { elements: [E1, { guid: E2 }], property: { builtIn: "General_ElementID" }, value: "W-01" },
        { elements: [E1], property: "CC-test-Group/Fire", value: ["REI 60"] },
        { elementDefaults: ["Wall"], property: "CC-test-Group/Len", reset: true },
      ],
    });
    expect(res.isError).toBe(false);
    expect(addonCalls(h)[0]?.addOnCommand).toBe("SetPropertyValues");
    expect(addonCalls(h)[0]?.addOnParameters).toEqual({
      values: [
        { elements: [E1, E2], property: { guid: G1 }, value: "W-01" },
        { elements: [E1], property: "CC-test-Group/Fire", value: ["REI 60"] },
        { elementDefaults: ["Wall"], property: "CC-test-Group/Len", reset: true },
      ],
    });
  });

  it("set_property_values rejects entries without target or with several modes", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("set_property_values", {
      values: [
        { property: G1, value: 1 },
        { elements: [E1], property: G1, value: 1, reset: true },
        { elements: [E1], property: G1 },
      ],
    });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("values[0]: give elements and/or elementDefaults");
    expect(res.text).toContain("values[1]: give exactly one of");
    expect(res.text).toContain("values[2]: give exactly one of");
    expect(h.requests).toHaveLength(0);
  });

  it("attribute property values", async () => {
    const h = await harness(() => addonOk({ properties: [], results: [] }));
    await h.call("get_attribute_property_values", { attributes: [{ type: "BuildingMaterial", attribute: "Бетон" }, { attribute: 3 }] });
    expect(h.requests[0]?.addOnCommand).toBe("GetAttributePropertyValues");
    expect(h.requests[0]?.addOnParameters).toEqual({ attributes: [{ type: "BuildingMaterial", attribute: "Бетон" }, { attribute: 3 }] });

    await h.call("set_attribute_property_values", {
      values: [{ attributes: [{ attribute: { guid: G2 } }], property: "CC-test-Group/Len", value: 0.2 }],
    });
    expect(h.requests[1]?.addOnCommand).toBe("SetAttributePropertyValues");
    expect(h.requests[1]?.addOnParameters).toEqual({ values: [{ attributes: [{ attribute: { guid: G2 } }], property: "CC-test-Group/Len", value: 0.2 }] });
  });
});

describe("IFC", () => {
  it("get_ifc_data forwards elements and lookups", async () => {
    const h = await harness(() => addonOk({ elements: [] }));
    const res = await h.call("get_ifc_data", {
      elements: [{ guid: E1 }],
      ifcGlobalIds: ["2Hn3tQ$Pz0Hwv2k1cA7t2W"],
      include: ["identity", "properties"],
      propertySets: ["Pset_WallCommon"],
      storedOnly: true,
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("GetIfcData");
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [E1],
      ifcGlobalIds: ["2Hn3tQ$Pz0Hwv2k1cA7t2W"],
      include: ["identity", "properties"],
      propertySets: ["Pset_WallCommon"],
      storedOnly: true,
    });
  });

  it("get_ifc_data needs elements or ids", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("get_ifc_data", {})).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("set_ifc_properties forwards properties and attributes", async () => {
    const h = await harness(() => addonOk({ properties: [], attributes: [] }));
    const res = await h.call("set_ifc_properties", {
      properties: [
        { elements: [E1], propertySet: "CC_Pset", name: "Rating", value: "EI 60" },
        { elements: [{ guid: E2 }], propertySet: "CC_Pset", name: "Range", lower: { value: 1, valueType: "IfcLengthMeasure" }, upper: 2.5 },
        { elements: [E1], propertySet: "CC_Pset", name: "Old", remove: true },
      ],
      attributes: [{ elements: [E1], name: "ObjectType", value: "CC" }],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("SetIfcProperties");
    expect(h.requests[0]?.addOnParameters).toEqual({
      properties: [
        { elements: [E1], propertySet: "CC_Pset", name: "Rating", value: "EI 60" },
        { elements: [E2], propertySet: "CC_Pset", name: "Range", lower: { value: 1, valueType: "IfcLengthMeasure" }, upper: 2.5 },
        { elements: [E1], propertySet: "CC_Pset", name: "Old", remove: true },
      ],
      attributes: [{ elements: [E1], name: "ObjectType", value: "CC" }],
    });
  });

  it("set_ifc_properties forwards classification references", async () => {
    const h = await harness(() => addonOk({ classificationReferences: [] }));
    const res = await h.call("set_ifc_properties", {
      classificationReferences: [
        {
          elements: [{ guid: E1 }, E2],
          identification: "Ss_25_10_30",
          name: "Brick walling systems",
          source: { name: "Uniclass", edition: "2015" },
        },
        { elements: [E1], referenceName: "Old", remove: true },
      ],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({
      classificationReferences: [
        { elements: [E1, E2], identification: "Ss_25_10_30", name: "Brick walling systems", source: { name: "Uniclass", edition: "2015" } },
        { elements: [E1], referenceName: "Old", remove: true },
      ],
    });
  });

  it("set_ifc_properties validates classification references", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("set_ifc_properties", {
      classificationReferences: [{ elements: [E1], source: { name: "Uniclass" } }, { elements: [E1], identification: "X" }],
    });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("classificationReferences[0]: give referenceName, identification or name");
    expect(res.text).toContain("classificationReferences[1]: source {name, ...} is required");
    expect(h.requests).toHaveLength(0);
    expect((await h.call("set_ifc_properties", {})).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("set_ifc_properties validates value shapes", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("set_ifc_properties", {
      properties: [
        { elements: [E1], propertySet: "P", name: "A" },
        { elements: [E1], propertySet: "P", name: "B", type: "Table", definingValues: [1] },
      ],
      attributes: [{ elements: [E1], name: "Name" }],
    });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("properties[0]: a Single property needs value");
    expect(res.text).toContain("properties[1]: a Table property needs");
    expect(res.text).toContain("attributes[0]: give exactly one of value or clear");
    expect(h.requests).toHaveLength(0);
  });
});

describe("classification authoring", () => {
  it("create_classification_system sends the nested item tree", async () => {
    const h = await harness(() => addonOk({ system: { guid: G1 }, items: [], created: 0, failed: 0 }));
    const res = await h.call("create_classification_system", {
      name: "CC-test-System",
      editionVersion: "1.0",
      editionDate: "2026-09-30",
      items: [{ id: "A", name: "Alpha", children: [{ id: "A.1", children: [{ id: "A.1.1" }] }] }, { id: "B", parent: "A" }],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("CreateClassificationSystem");
    expect(h.requests[0]?.addOnParameters).toEqual({
      name: "CC-test-System",
      editionVersion: "1.0",
      editionDate: "2026-09-30",
      items: [{ id: "A", name: "Alpha", children: [{ id: "A.1", children: [{ id: "A.1.1" }] }] }, { id: "B", parent: "A" }],
    });
  });

  it("create_classification_system validates the date", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("create_classification_system", { name: "X", editionDate: "30.09.2026" });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("items: create / modify / delete and systems: modify / delete", async () => {
    const h = await harness(() => addonOk({ results: [] }));
    await h.call("create_classification_items", { system: { name: "CC-test-System", editionVersion: "1.0" }, parent: "A", items: [{ id: "A.2", before: "A.1" }] });
    await h.call("modify_classification_items", { system: "CC-test-System", items: [{ item: "A.2", name: "Two" }] });
    await h.call("delete_classification_items", { system: "CC-test-System", items: ["A.2", { guid: G2 }] });
    await h.call("modify_classification_system", { system: { guid: G1 }, description: "d" });
    await h.call("delete_classification_systems", { systems: ["CC-test-System"] });
    expect(h.requests.map((r) => r.addOnCommand)).toEqual([
      "CreateClassificationItems",
      "ModifyClassificationItems",
      "DeleteClassificationItems",
      "ModifyClassificationSystem",
      "DeleteClassificationSystems",
    ]);
    expect(h.requests[0]?.addOnParameters).toEqual({ system: { name: "CC-test-System", editionVersion: "1.0" }, parent: "A", items: [{ id: "A.2", before: "A.1" }] });
    expect(h.requests[3]?.addOnParameters).toEqual({ system: { guid: G1 }, description: "d" });
  });

  it("modify tools reject empty changes", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("modify_classification_system", { system: "X" })).isError).toBe(true);
    expect((await h.call("modify_classification_items", { items: [{ item: "A" }] })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("import_classifications_xml forwards xml and policies", async () => {
    const h = await harness(() => addonOk({ systems: [] }));
    await h.call("import_classifications_xml", { xml: "<BuildingInformation/>", systemConflictPolicy: "Skip", itemConflictPolicy: "Skip" });
    expect(h.requests[0]?.addOnCommand).toBe("ImportClassificationsXml");
    expect(h.requests[0]?.addOnParameters).toEqual({ xml: "<BuildingInformation/>", systemConflictPolicy: "Skip", itemConflictPolicy: "Skip" });
  });
});
