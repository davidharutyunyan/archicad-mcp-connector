import { describe, expect, it } from "vitest";

import { apiOk, harness, type RecordedRequest } from "../helpers.js";

const W1 = "85999C0D-1731-ED48-90EB-5E6220F506A8";
const W2 = "58BE19AE-17FA-3E4B-A9D6-8C2FAEB3DC3B";
const S1 = "57E66B6A-CB0E-9244-B21B-D09850B94378";
const Z1 = "0A1B2C3D-4E5F-6071-8293-A4B5C6D7E8F9";
const C1 = "B3B85900-6A9E-A610-633B-5C81389348A5";
const SYS = "9E077996-8432-304B-8F5F-D7D3C32812A1";
const SYS2 = "11111111-2222-3333-4444-555555555555";
const I_ROOT = "7FE29BE3-EB3F-4045-9977-BCD7DB0DC6F8";
const I_WALL = "2FE7D575-A2F2-B54D-8D9E-F1DEF9F70F9F";
const I_SOLID = "F5AD387C-60C5-0742-984A-D9FF0614B0FB";
const P_THICK = "88FF0334-FAB9-4B10-9361-33D4B6903B91";
const P_BMAT = "A936C5CB-5126-4135-BD87-D2A46AEF5A07";
const P_ANGLE = "59D1CBE9-6C29-4B97-BA57-7EC9011B8D67";
const P_UD = "19865BAA-3F0B-B24B-B0C3-DD07D4C9C20E";
const NAV_ROOT = "D0C2316C-DC71-44E2-B572-691D95E6AB3D";
const LB_ROOT = "52B690D8-CCAC-4537-A775-10EB470D505B";
const SUBSET = "A8962201-4BAD-3246-A535-4846372F09EA";
const LAYOUT = "B167A39F-7537-4CE1-893C-ED75701103CF";
const MASTER = "87419F3C-2947-42B4-92CF-C87A2E080AD2";
const MASTER2 = "53D58681-8230-4608-9FFE-E6B6A93D6BEA";
const VM_ROOT = "38185F87-EF79-4E75-851D-61DE8ED16E94";
const VM_TOP = "3C4A2145-A752-401C-AD75-902D0235F1D7";
const STORY = "52BD03F2-B8EC-4BB6-BEB2-E7374D208705";
const PROFILE = "6016ED76-72A6-4166-84E4-538D0B0417AC";
const PT1 = "6D00F0C8-8F31-4E97-9BC6-905F90F5883A";
const PT2 = "0803C4C9-8439-4622-B89E-09FF4CAFF1E2";
const LAYER1 = "8C809610-4541-4CE5-A039-B12FD8C5C0E0";
const LAYER2 = "F2526710-72BC-48BE-BD5B-33C17806A910";
const FOLDER = "DAFFBDBA-6B06-4BDD-91BC-99C2B29E7346";

const TOOLS = [
  "list_elements",
  "get_element_types",
  "get_bounding_boxes",
  "get_elements_related_to_zones",
  "get_element_components",
  "get_component_property_values",
  "get_classification_systems",
  "get_classification_tree",
  "get_classification_item_details",
  "get_element_classifications",
  "set_element_classifications",
  "get_elements_by_classification",
  "get_classification_availability",
  "get_navigator_tree",
  "get_navigator_items",
  "rename_navigator_item",
  "move_navigator_item",
  "delete_navigator_items",
  "clone_project_map_item_to_view_map",
  "create_view_map_folder",
  "create_layout",
  "create_layout_subset",
  "get_layout_settings",
  "set_layout_settings",
  "get_attribute_folders",
  "create_attribute_folders",
  "delete_attribute_folders",
  "move_attributes_to_folder",
  "rename_attribute_folder",
  "get_publisher_sets",
  "get_profile_preview",
  "get_active_pen_tables",
  "get_property_ids_by_name",
];

type Handler = (p: Record<string, any>) => unknown;

/** Fake official JSON API: dispatches on the command name; unknown commands fail like Archicad does. */
function fakeApi(handlers: Record<string, Handler>) {
  return (req: RecordedRequest) => {
    const name = req.command.replace(/^API\./, "");
    const h = handlers[name];
    if (!h) return { succeeded: false, error: { code: 4001, message: `Unexpected command ${req.command}` } };
    const out = h((req.parameters ?? {}) as Record<string, any>);
    if (out && typeof out === "object" && "succeeded" in (out as object)) return out;
    return apiOk(out);
  };
}

const guids = (p: any, key: string, inner: string) => (p[key] as any[]).map((x) => x[inner].guid);
const notFound = (g: string) => ({ error: { code: 7204, message: `Element not found (element guid: "${g}")` } });

const TYPES: Record<string, string> = { [W1]: "Wall", [W2]: "Wall", [S1]: "Slab", [Z1]: "Zone" };
const typesHandler: Handler = (p) => ({
  typesOfElements: guids(p, "elements", "elementId").map((g: string) =>
    TYPES[g] ? { typeOfElement: { elementId: { guid: g }, elementType: TYPES[g] } } : notFound(g),
  ),
});

const classTree = {
  classificationItems: [
    {
      classificationItem: {
        classificationItemId: { guid: I_ROOT },
        id: "Элементы",
        name: "",
        description: "",
        children: [
          {
            classificationItem: {
              classificationItemId: { guid: I_WALL },
              id: "Стена",
              name: "",
              description: "Walls",
              children: [{ classificationItem: { classificationItemId: { guid: I_SOLID }, id: "Сплошная Стена", name: "", description: "" } }],
            },
          },
        ],
      },
    },
  ],
};

const systemsHandler: Handler = () => ({
  classificationSystems: [
    { classificationSystemId: { guid: SYS }, name: "Классификация Archicad", description: "", source: "www.graphisoft.com", version: "v 2.0", date: "2019-08-01" },
  ],
});

const layoutBookTree = {
  navigatorTree: {
    rootItem: {
      navigatorItemId: { guid: NAV_ROOT },
      prefix: "",
      name: "",
      type: "UndefinedItem",
      children: [
        {
          navigatorItem: {
            navigatorItemId: { guid: LB_ROOT },
            prefix: "Без имени",
            name: "Без имени",
            type: "LayoutBookRootItem",
            children: [
              {
                navigatorItem: {
                  navigatorItemId: { guid: SUBSET },
                  prefix: "АР",
                  name: "Планы",
                  type: "SubsetItem",
                  children: [
                    {
                      navigatorItem: {
                        navigatorItemId: { guid: LAYOUT },
                        prefix: "03",
                        name: "План 1",
                        type: "LayoutItem",
                        sourceNavigatorItemId: { guid: "7D728684-AE3D-4741-B233-62733C98B8ED" },
                      },
                    },
                  ],
                },
              },
              {
                navigatorItem: {
                  navigatorItemId: { guid: "AA8FCD72-0610-43B2-A565-335CAD0E8C7E" },
                  prefix: "Основные",
                  name: "Основные",
                  type: "MasterFolderItem",
                  children: [
                    { navigatorItem: { navigatorItemId: { guid: MASTER2 }, prefix: "Обложка", name: "Обложка", type: "MasterLayoutItem" } },
                    { navigatorItem: { navigatorItemId: { guid: MASTER }, prefix: "А3 - А - Ф3", name: "А3 - А - Ф3", type: "MasterLayoutItem" } },
                  ],
                },
              },
            ],
          },
        },
      ],
    },
  },
};

const projectMapTree = {
  navigatorTree: {
    rootItem: {
      navigatorItemId: { guid: "641C65C4-5E29-44E6-A9FC-E816563B1553" },
      prefix: "",
      name: "",
      type: "FolderItem",
      children: [
        {
          navigatorItem: {
            navigatorItemId: { guid: "C3B3EB83-1A09-4E21-AB1C-F5A8F4A38DAB" },
            prefix: "",
            name: "Проект",
            type: "ProjectMapRootItem",
            children: [
              {
                navigatorItem: {
                  navigatorItemId: { guid: "8A3BBC57-86D6-407E-8B3C-3A862E32DF3A" },
                  prefix: "",
                  name: "Этажи",
                  type: "UndefinedItem",
                  children: [{ navigatorItem: { navigatorItemId: { guid: STORY }, prefix: "3.", name: "3-й этаж", type: "StoryItem" } }],
                },
              },
            ],
          },
        },
      ],
    },
  },
};

const viewMapTree = {
  navigatorTree: {
    rootItem: {
      navigatorItemId: { guid: VM_ROOT },
      prefix: "",
      name: "",
      type: "FolderItem",
      children: [{ navigatorItem: { navigatorItemId: { guid: VM_TOP }, prefix: "", name: "Проект", type: "FolderItem" } }],
    },
  },
};

const layoutParams = (w: number, h: number, extra: Record<string, unknown> = {}) => ({
  layoutParameters: {
    horizontalSize: w,
    verticalSize: h,
    leftMargin: 20,
    topMargin: 5,
    rightMargin: 5,
    bottomMargin: 5,
    customLayoutNumber: "",
    customLayoutNumbering: false,
    doNotIncludeInNumbering: false,
    displayMasterLayoutBelow: true,
    layoutPageNumber: 0,
    actPageIndex: 0,
    currentRevisionId: "",
    currentFinalRevisionId: "",
    hasIssuedRevision: false,
    hasActualRevision: false,
    ...extra,
  },
});

function navTreeHandler(p: any) {
  const t = p.navigatorTreeId.type;
  if (t === "LayoutBook") return layoutBookTree;
  if (t === "ProjectMap") return projectMapTree;
  if (t === "ViewMap") return viewMapTree;
  if (t === "PublisherSets") return viewMapTree;
  return { succeeded: false, error: { code: 6005, message: "Teamwork project required" } };
}

describe("official tool registration", () => {
  it("registers every tool of the family with a useful description and correct hints", async () => {
    const h = await harness(fakeApi({}));
    const tools = await h.listTools();
    for (const name of TOOLS) {
      const tool = tools.find((t) => t.name === name);
      expect(tool, name).toBeDefined();
      expect(tool!.description!.length, name).toBeGreaterThan(60);
    }
    const ro = ["list_elements", "get_navigator_tree", "get_classification_tree", "get_profile_preview", "get_property_ids_by_name"];
    for (const n of ro) expect(tools.find((t) => t.name === n)!.annotations?.readOnlyHint, n).toBe(true);
    for (const n of ["set_element_classifications", "delete_navigator_items", "delete_attribute_folders", "create_layout"]) {
      expect(tools.find((t) => t.name === n)!.annotations?.readOnlyHint, n).toBe(false);
    }
    expect(tools.find((t) => t.name === "delete_attribute_folders")!.annotations?.destructiveHint).toBe(true);
  });
});

describe("elements", () => {
  it("list_elements lists all elements with types and counts, paginated", async () => {
    const h = await harness(
      fakeApi({
        GetAllElements: () => ({ elements: [W1, W2, S1].map((g) => ({ elementId: { guid: g } })) }),
        GetTypesOfElements: typesHandler,
      }),
    );
    const res = await h.call("list_elements", { limit: 2 });
    expect(res.isError).toBe(false);
    expect(h.requests.map((r) => r.command)).toEqual(["API.GetAllElements", "API.GetTypesOfElements"]);
    expect(res.json).toEqual({
      total: 3,
      offset: 0,
      returned: 2,
      hasMore: true,
      countsByType: { Wall: 2, Slab: 1 },
      elements: [
        { guid: W1, type: "Wall" },
        { guid: W2, type: "Wall" },
      ],
    });
  });

  it("list_elements with types queries GetElementsByType per type; includeTypes false returns plain GUIDs", async () => {
    const h = await harness(
      fakeApi({
        GetElementsByType: (p) => ({ elements: (p.elementType === "Wall" ? [W1, W2] : [S1]).map((g) => ({ elementId: { guid: g } })) }),
      }),
    );
    const res = await h.call("list_elements", { types: ["Wall", "Slab"] });
    expect(h.requests.map((r) => r.parameters)).toEqual([{ elementType: "Wall" }, { elementType: "Slab" }]);
    expect((res.json as any).countsByType).toEqual({ Wall: 2, Slab: 1 });
    const res2 = await h.call("list_elements", { types: ["Wall"], includeTypes: false });
    expect((res2.json as any).elements).toEqual([W1, W2]);
  });

  it("list_elements selectedOnly passes onlyEditable and filters the selection by type", async () => {
    const h = await harness(
      fakeApi({
        GetSelectedElements: () => ({ elements: [W1, S1].map((g) => ({ elementId: { guid: g } })) }),
        GetTypesOfElements: typesHandler,
      }),
    );
    const res = await h.call("list_elements", { selectedOnly: true, onlyEditable: true, types: ["Slab"] });
    expect(h.requests[0]).toMatchObject({ command: "API.GetSelectedElements", parameters: { onlyEditable: true } });
    expect((res.json as any).elements).toEqual([{ guid: S1, type: "Slab" }]);
  });

  it("get_element_types normalizes GUIDs ({braces}, lower case) and reports per-item errors", async () => {
    const h = await harness(fakeApi({ GetTypesOfElements: typesHandler }));
    const missing = "00000000-0000-0000-0000-000000000001";
    const res = await h.call("get_element_types", { elements: [`{${W1.toLowerCase()}}`, { guid: S1 }, missing] });
    expect(h.requests[0]?.parameters).toEqual({ elements: [W1, S1, missing].map((g) => ({ elementId: { guid: g } })) });
    expect((res.json as any).elements[0]).toEqual({ guid: W1, type: "Wall" });
    expect((res.json as any).elements[2].error).toContain("Element not found");
    expect((res.json as any).elements[2].error).toContain("code 7204");
  });

  it("get_bounding_boxes returns 2D and 3D boxes and their union", async () => {
    const h = await harness(
      fakeApi({
        Get3DBoundingBoxes: () => ({
          boundingBoxes3D: [
            { boundingBox3D: { xMin: 0, yMin: 0, zMin: 0, xMax: 4, yMax: 0.3, zMax: 3 } },
            { boundingBox3D: { xMin: 3.7, yMin: 0, zMin: -1, xMax: 4, yMax: 3, zMax: 3 } },
          ],
        }),
        Get2DBoundingBoxes: () => ({
          boundingBoxes2D: [{ boundingBox2D: { xMin: 0, yMin: 0, xMax: 4, yMax: 0.3 } }, notFound(W2)],
        }),
      }),
    );
    const res = await h.call("get_bounding_boxes", { elements: [W1, W2], kind: "both" });
    const j = res.json as any;
    expect(j.overall3D).toEqual({ xMin: 0, yMin: 0, zMin: -1, xMax: 4, yMax: 3, zMax: 3 });
    expect(j.overall2D).toEqual({ xMin: 0, yMin: 0, xMax: 4, yMax: 0.3 });
    expect(j.elements[1].box3D).toBeDefined();
    expect(j.elements[1].error).toContain("Element not found");
  });

  it("get_elements_related_to_zones passes the type filter and groups by type", async () => {
    const h = await harness(
      fakeApi({
        GetElementsRelatedToZones: () => ({
          elementsRelatedToZones: [
            { elements: [W1, W2, S1].map((g) => ({ elementId: { guid: g } })) },
            { error: { code: 7200, message: "The type of the element is not the expected" } },
          ],
        }),
        GetTypesOfElements: typesHandler,
      }),
    );
    const res = await h.call("get_elements_related_to_zones", { zones: [Z1, W1], elementTypes: ["Wall", "Slab"] });
    expect(h.requests[0]?.parameters).toEqual({ zones: [Z1, W1].map((g) => ({ elementId: { guid: g } })), elementTypes: ["Wall", "Slab"] });
    const j = res.json as any;
    expect(j.zones[0]).toEqual({ zone: Z1, total: 3, elements: { Wall: [W1, W2], Slab: [S1] } });
    expect(j.zones[1].error).toContain("not a zone");
  });

  it("get_element_components reads the default summary and converts nothing but angles", async () => {
    const h = await harness(
      fakeApi({
        GetComponentsOfElements: () => ({
          componentsOfElements: [{ elementComponents: [{ elementComponentId: { elementId: { guid: W1 }, componentId: { guid: C1 } } }] }, notFound(S1)],
        }),
        GetPropertyIds: (p) => ({ properties: p.properties.map((_: unknown, i: number) => ({ propertyId: { guid: `00000000-0000-0000-0000-00000000000${i}` } })) }),
        GetPropertyValuesOfElementComponents: (p) => ({
          propertyValuesForElementComponents: [
            {
              propertyValues: p.properties.map((_: unknown, i: number) =>
                i === 0 ? { propertyValue: { type: "string", status: "normal", value: "Кирпич" } } : { propertyValue: { type: "length", status: "notAvailable" } },
              ),
            },
          ],
        }),
      }),
    );
    const res = await h.call("get_element_components", { elements: [W1, S1] });
    expect(res.isError).toBe(false);
    const valuesReq = h.requests.find((r) => r.command === "API.GetPropertyValuesOfElementComponents")!;
    expect(valuesReq.parameters?.["elementComponents"]).toEqual([{ elementComponentId: { elementId: { guid: W1 }, componentId: { guid: C1 } } }]);
    const idsReq = h.requests.find((r) => r.command === "API.GetPropertyIds")!;
    expect((idsReq.parameters?.["properties"] as any[])[0]).toEqual({ type: "BuiltIn", nonLocalizedName: "BuildingMaterial_Name" });
    const j = res.json as any;
    expect(j.elements[0].components[0].values).toEqual({ BuildingMaterial_Name: "Кирпич" });
    expect(j.elements[0].components[0].unavailable.Component_Thickness).toBe("notAvailable");
    expect(j.elements[1].error).toContain("Element not found");
  });

  it("get_component_property_values resolves names (with localized fallback), converts angles to degrees and reports unknown properties", async () => {
    const h = await harness(
      fakeApi({
        GetPropertyIds: (p) => ({
          properties: p.properties.map((q: any) =>
            q.nonLocalizedName === "Component_Thickness"
              ? { propertyId: { guid: P_THICK } }
              : q.nonLocalizedName === "General_SlantAngle"
                ? { propertyId: { guid: P_ANGLE } }
                : { error: { code: 4005, message: "Property definition not found" } },
          ),
        }),
        GetAllPropertyIds: () => ({ propertyIds: [{ propertyId: { guid: P_UD } }] }),
        GetDetailsOfProperties: (p) => ({
          propertyDefinitions: p.properties.map((x: any) => ({
            propertyDefinition:
              x.propertyId.guid === P_UD
                ? { propertyId: { guid: P_UD }, group: { name: "ОСНОВНЫЕ КОНСТРУКЦИИ" }, name: "Технология", isEditable: true, type: "singleEnum" }
                : { propertyId: x.propertyId, group: { name: "Компоненты" }, name: "Толщина Компонента", isEditable: false, type: "length" },
          })),
        }),
        GetAllPropertyNames: () => ({ properties: [{ type: "BuiltIn", nonLocalizedName: "Component_Thickness" }] }),
        GetPropertyValuesOfElementComponents: () => ({
          propertyValuesForElementComponents: [
            {
              propertyValues: [
                { propertyValue: { type: "length", status: "normal", value: 0.3 } },
                { propertyValue: { type: "angle", status: "normal", value: Math.PI / 2 } },
                { propertyValue: { type: "singleEnum", status: "normal", value: { type: "displayValue", displayValue: "Монолит" } } },
              ],
            },
          ],
        }),
      }),
    );
    const res = await h.call("get_component_property_values", {
      components: [{ element: W1, component: C1 }],
      properties: ["Component_Thickness", "General_SlantAngle", "ОСНОВНЫЕ КОНСТРУКЦИИ/Технология", "Nope_Nope"],
    });
    expect(res.isError).toBe(false);
    const j = res.json as any;
    expect(j.components[0].values).toEqual({ Component_Thickness: 0.3, General_SlantAngle: 90, "ОСНОВНЫЕ КОНСТРУКЦИИ/Технология": "Монолит" });
    expect(j.propertyErrors.Nope_Nope).toContain("not found");
  });

  it("get_component_property_values needs components or elements", async () => {
    const h = await harness(fakeApi({}));
    const res = await h.call("get_component_property_values", { properties: ["Component_Thickness"] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("components");
    expect(h.requests).toHaveLength(0);
  });

  it("get_property_ids_by_name resolves names and marks built-in / user-defined properties", async () => {
    const h = await harness(
      fakeApi({
        GetPropertyIds: (p) => ({
          properties: p.properties.map((q: any) =>
            q.type === "BuiltIn" ? { propertyId: { guid: P_THICK } } : { propertyId: { guid: P_UD } },
          ),
        }),
        GetDetailsOfProperties: (p) => ({
          propertyDefinitions: p.properties.map((x: any) => ({
            propertyDefinition: {
              propertyId: x.propertyId,
              group: { name: x.propertyId.guid === P_UD ? "ОСНОВНЫЕ КОНСТРУКЦИИ" : "Компоненты" },
              name: x.propertyId.guid === P_UD ? "Технология" : "Толщина Компонента",
              isEditable: x.propertyId.guid === P_UD,
              type: x.propertyId.guid === P_UD ? "singleEnum" : "length",
              ...(x.propertyId.guid === P_UD
                ? {
                    possibleEnumValues: [
                      { enumValue: { enumValueId: { type: "nonLocalizedValue", nonLocalizedValue: "M" }, displayValue: "Монолит", nonLocalizedValue: "M" } },
                    ],
                  }
                : {}),
            },
          })),
        }),
        GetAllPropertyIds: () => ({ propertyIds: [{ propertyId: { guid: P_UD } }] }),
      }),
    );
    const res = await h.call("get_property_ids_by_name", { properties: ["Component_Thickness", { group: "ОСНОВНЫЕ КОНСТРУКЦИИ", name: "Технология" }] });
    const j = res.json as any;
    expect(j.properties[0]).toMatchObject({ guid: P_THICK, builtInName: "Component_Thickness", kind: "BuiltIn", type: "length" });
    expect(j.properties[1]).toMatchObject({ guid: P_UD, kind: "UserDefined", enumValues: [{ displayValue: "Монолит", nonLocalizedValue: "M" }] });
  });

  it("get_property_ids_by_name search browses built-in and user-defined catalogs", async () => {
    const h = await harness(
      fakeApi({
        GetAllPropertyIds: () => ({ propertyIds: [{ propertyId: { guid: P_UD } }] }),
        GetAllPropertyNames: () => ({ properties: [{ type: "BuiltIn", nonLocalizedName: "Component_Thickness" }, { type: "BuiltIn", nonLocalizedName: "BuildingMaterial_Name" }] }),
        GetPropertyIds: (p) => ({
          properties: p.properties.map((q: any) => ({ propertyId: { guid: q.nonLocalizedName === "Component_Thickness" ? P_THICK : P_BMAT } })),
        }),
        GetDetailsOfProperties: (p) => ({
          propertyDefinitions: p.properties.map((x: any) => ({
            propertyDefinition: {
              propertyId: x.propertyId,
              group: { name: x.propertyId.guid === P_UD ? "ОСНОВНЫЕ КОНСТРУКЦИИ" : "Компоненты" },
              name: x.propertyId.guid === P_UD ? "Технология" : x.propertyId.guid === P_THICK ? "Толщина Компонента" : "Имя",
              isEditable: false,
              type: "string",
            },
          })),
        }),
      }),
    );
    const res = await h.call("get_property_ids_by_name", { search: "толщина" });
    const j = res.json as any;
    expect(j.total).toBe(1);
    expect(j.properties[0]).toMatchObject({ guid: P_THICK, builtInName: "Component_Thickness", kind: "BuiltIn", name: "Толщина Компонента" });
  });

  it("get_property_ids_by_name filters the catalog by the properties available for elements", async () => {
    const h = await harness(
      fakeApi({
        GetAllPropertyIds: () => ({ propertyIds: [{ propertyId: { guid: P_UD } }] }),
        GetAllPropertyNames: () => ({ properties: [{ type: "BuiltIn", nonLocalizedName: "Component_Thickness" }, { type: "BuiltIn", nonLocalizedName: "BuildingMaterial_Name" }] }),
        GetPropertyIds: (p) => ({
          properties: p.properties.map((q: any) => ({ propertyId: { guid: q.nonLocalizedName === "Component_Thickness" ? P_THICK : P_BMAT } })),
        }),
        GetDetailsOfProperties: (p) => ({
          propertyDefinitions: p.properties.map((x: any) => ({
            propertyDefinition: { propertyId: x.propertyId, group: { name: "G" }, name: x.propertyId.guid, isEditable: false, type: "string" },
          })),
        }),
        GetAllPropertyIdsOfElements: (p) => ({
          propertyIdsOfElements: guids(p, "elements", "elementId").map((g: string) =>
            g === W1
              ? { propertyIdsOfElement: { elementId: { guid: g }, propertyIds: [{ propertyId: { guid: P_UD } }, { propertyId: { guid: P_THICK } }] } }
              : g === W2
                ? { propertyIdsOfElement: { elementId: { guid: g }, propertyIds: [{ propertyId: { guid: P_THICK } }] } }
                : notFound(g),
          ),
        }),
      }),
    );
    const any = (await h.call("get_property_ids_by_name", { elements: [W1, W2, S1] })).json as any;
    expect(h.requests.find((r) => r.command === "API.GetAllPropertyIdsOfElements")?.parameters).toEqual({
      elements: [W1, W2, S1].map((g) => ({ elementId: { guid: g } })),
    });
    expect(any.properties.map((p: any) => p.guid).sort()).toEqual([P_THICK, P_UD].sort());
    expect(any.elementErrors[0]).toMatchObject({ guid: S1 });
    const all = (await h.call("get_property_ids_by_name", { elements: [W1, W2], elementMatch: "all", propertyType: "BuiltIn" })).json as any;
    expect(h.requests.filter((r) => r.command === "API.GetAllPropertyIdsOfElements").at(-1)?.parameters?.["propertyType"]).toBe("BuiltIn");
    expect(all.properties).toEqual([{ guid: P_THICK, builtInName: "Component_Thickness", kind: "BuiltIn", group: "G", name: P_THICK, type: "string", editable: false }]);
    expect((await h.call("get_property_ids_by_name", { elementMatch: "all" })).isError).toBe(true);
  });
});

describe("classifications", () => {
  const base = {
    GetAllClassificationSystems: systemsHandler,
    GetAllClassificationsInSystem: () => classTree,
  };

  it("get_classification_systems adds item counts", async () => {
    const h = await harness(fakeApi(base));
    const res = await h.call("get_classification_systems", {});
    expect((res.json as any).systems[0]).toMatchObject({ guid: SYS, name: "Классификация Archicad", itemCount: 3 });
  });

  it("get_classification_tree returns a nested tree limited by maxDepth, or flat search results with paths", async () => {
    const h = await harness(fakeApi(base));
    const tree = (await h.call("get_classification_tree", { maxDepth: 2 })).json as any;
    expect(tree.tree[0]).toEqual({ guid: I_ROOT, id: "Элементы", children: [{ guid: I_WALL, id: "Стена", childCount: 1 }] });
    const flat = (await h.call("get_classification_tree", { search: "сплошная" })).json as any;
    expect(flat.items).toEqual([{ guid: I_SOLID, id: "Сплошная Стена", path: "Элементы > Стена > Сплошная Стена", depth: 2 }]);
    const branch = (await h.call("get_classification_tree", { root: "Стена" })).json as any;
    expect(branch.root.guid).toBe(I_WALL);
    expect(branch.tree).toEqual([{ guid: I_SOLID, id: "Сплошная Стена" }]);
  });

  it("set_element_classifications resolves ids/paths, supports unclassify (null) and reports unknown items", async () => {
    const h = await harness(
      fakeApi({
        ...base,
        SetClassificationsOfElements: (p) => ({ executionResults: p.elementClassifications.map(() => ({ success: true })) }),
      }),
    );
    const res = await h.call("set_element_classifications", {
      assignments: [
        { elements: [W1], item: "Сплошная Стена" },
        { elements: [W2], item: "Элементы > Стена" },
        { elements: [S1], item: null },
        { elements: [Z1], item: "Нет такого" },
      ],
    });
    expect(res.isError).toBe(false);
    const setReq = h.requests.find((r) => r.command === "API.SetClassificationsOfElements")!;
    expect(setReq.parameters).toEqual({
      elementClassifications: [
        { elementId: { guid: W1 }, classificationId: { classificationSystemId: { guid: SYS }, classificationItemId: { guid: I_SOLID } } },
        { elementId: { guid: W2 }, classificationId: { classificationSystemId: { guid: SYS }, classificationItemId: { guid: I_WALL } } },
        { elementId: { guid: S1 }, classificationId: { classificationSystemId: { guid: SYS } } },
      ],
    });
    const j = res.json as any;
    expect(j.results[0]).toMatchObject({ guid: W1, ok: true, item: "Элементы > Стена > Сплошная Стена" });
    expect(j.results[2]).toMatchObject({ guid: S1, ok: true, item: null });
    expect(j.results[3].error).toContain("not found");
  });

  it("asks for the system when several exist and an item is unclassified", async () => {
    const h = await harness(
      fakeApi({
        GetAllClassificationSystems: () => ({
          classificationSystems: [
            { classificationSystemId: { guid: SYS }, name: "A" },
            { classificationSystemId: { guid: SYS2 }, name: "B" },
          ],
        }),
      }),
    );
    const res = await h.call("set_element_classifications", { assignments: [{ elements: [W1], item: null }] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("Several classification systems");
  });

  it("get_element_classifications maps items to ids and paths", async () => {
    const h = await harness(
      fakeApi({
        ...base,
        GetClassificationsOfElements: () => ({
          elementClassifications: [
            { classificationIds: [{ classificationId: { classificationSystemId: { guid: SYS }, classificationItemId: { guid: I_WALL } } }] },
            { classificationIds: [{ classificationId: { classificationSystemId: { guid: SYS } } }] },
            notFound(Z1),
          ],
        }),
      }),
    );
    const res = await h.call("get_element_classifications", { elements: [W1, W2, Z1] });
    const j = res.json as any;
    expect(j.elements[0].classifications[0]).toEqual({
      system: "Классификация Archicad",
      systemGuid: SYS,
      item: { guid: I_WALL, id: "Стена", path: "Элементы > Стена" },
    });
    expect(j.elements[1].classifications[0].item).toBeNull();
    expect(j.elements[2].error).toContain("Element not found");
  });

  it("get_elements_by_classification includes sub-items when asked", async () => {
    const h = await harness(
      fakeApi({
        ...base,
        GetElementsByClassification: (p) => ({
          elements: p.classificationItemId.guid === I_SOLID ? [{ elementId: { guid: W1 } }] : p.classificationItemId.guid === I_WALL ? [{ elementId: { guid: W2 } }] : [],
        }),
        GetTypesOfElements: typesHandler,
      }),
    );
    const res = await h.call("get_elements_by_classification", { item: "Стена", includeSubItems: true });
    const j = res.json as any;
    expect(j.total).toBe(2);
    expect(j.countsByType).toEqual({ Wall: 2 });
    expect(j.elements).toContainEqual({ guid: W1, type: "Wall", item: "Элементы > Стена > Сплошная Стена" });
  });

  it("get_elements_by_classification scans element classifications for large branches", async () => {
    const kid = (i: number) => `00000000-0000-0000-0000-${String(i).padStart(12, "0")}`;
    const bigTree = {
      classificationItems: [
        {
          classificationItem: {
            classificationItemId: { guid: I_ROOT },
            id: "Элементы",
            name: "",
            description: "",
            children: Array.from({ length: 14 }, (_, i) => ({
              classificationItem: { classificationItemId: { guid: kid(i + 1) }, id: `Item ${i + 1}`, name: "", description: "" },
            })),
          },
        },
      ],
    };
    const h = await harness(
      fakeApi({
        GetAllClassificationSystems: systemsHandler,
        GetAllClassificationsInSystem: () => bigTree,
        GetAllElements: () => ({ elements: [W1, W2, S1].map((g) => ({ elementId: { guid: g } })) }),
        GetClassificationsOfElements: (p) => ({
          elementClassifications: guids(p, "elements", "elementId").map((g: string) => ({
            classificationIds: [
              {
                classificationId:
                  g === W1
                    ? { classificationSystemId: { guid: SYS }, classificationItemId: { guid: kid(13) } }
                    : g === W2
                      ? { classificationSystemId: { guid: SYS }, classificationItemId: { guid: "99999999-0000-0000-0000-000000000000" } }
                      : { classificationSystemId: { guid: SYS } },
              },
            ],
          })),
        }),
        GetTypesOfElements: typesHandler,
      }),
    );
    const res = (await h.call("get_elements_by_classification", { item: "Элементы", includeSubItems: true })).json as any;
    expect(h.requests.some((r) => r.command === "API.GetElementsByClassification")).toBe(false);
    expect(h.requests.find((r) => r.command === "API.GetClassificationsOfElements")?.parameters?.["classificationSystemIds"]).toEqual([
      { classificationSystemId: { guid: SYS } },
    ]);
    expect(res).toMatchObject({ total: 1, countsByType: { Wall: 1 }, elements: [{ guid: W1, type: "Wall", item: "Элементы > Item 13" }] });
  });

  it("get_classification_availability works in both directions", async () => {
    const h = await harness(
      fakeApi({
        ...base,
        GetClassificationItemAvailability: () => ({
          classificationItemAvailabilityList: [{ classificationItemAvailability: { classificationItemId: { guid: I_WALL }, availableProperties: [{ propertyId: { guid: P_UD } }] } }],
        }),
        GetPropertyDefinitionAvailability: () => ({
          propertyDefinitionAvailabilityList: [{ propertyDefinitionAvailability: { propertyId: { guid: P_UD }, availableClassifications: [{ classificationItemId: { guid: I_SOLID } }] } }],
        }),
        GetDetailsOfProperties: (p) => ({
          propertyDefinitions: p.properties.map((x: any) => ({
            propertyDefinition: { propertyId: x.propertyId, group: { name: "ОСНОВНЫЕ КОНСТРУКЦИИ" }, name: "Технология", isEditable: true, type: "singleEnum" },
          })),
        }),
      }),
    );
    const res = await h.call("get_classification_availability", { items: ["Стена"], properties: [P_UD] });
    const j = res.json as any;
    expect(j.items[0].properties).toEqual([{ guid: P_UD, group: "ОСНОВНЫЕ КОНСТРУКЦИИ", name: "Технология", type: "singleEnum" }]);
    expect(j.properties[0]).toMatchObject({ availableForItems: 1, items: [{ guid: I_SOLID, id: "Сплошная Стена", path: "Элементы > Стена > Сплошная Стена" }] });
  });
});

describe("navigator", () => {
  it("get_navigator_tree returns a compact tree or a filtered flat list", async () => {
    const h = await harness(fakeApi({ GetNavigatorItemTree: navTreeHandler }));
    const tree = (await h.call("get_navigator_tree", { tree: "LayoutBook", maxDepth: 1 })).json as any;
    expect(h.requests[0]?.parameters).toEqual({ navigatorTreeId: { type: "LayoutBook" } });
    expect(tree.root.children[0]).toEqual({ id: LB_ROOT, type: "LayoutBookRootItem", prefix: "Без имени", name: "Без имени", childCount: 2 });
    const flat = (await h.call("get_navigator_tree", { tree: "LayoutBook", types: ["LayoutItem"] })).json as any;
    expect(flat.items).toEqual([
      {
        id: LAYOUT,
        type: "LayoutItem",
        prefix: "03",
        name: "План 1",
        path: "Без имени / АР Планы / 03 План 1",
        depth: 3,
        childCount: 0,
        parentId: SUBSET,
        sourceId: "7D728684-AE3D-4741-B233-62733C98B8ED",
      },
    ]);
  });

  it("get_navigator_tree: publisher sets need a name; MyViewMap errors explain Teamwork", async () => {
    const h = await harness(fakeApi({ GetNavigatorItemTree: navTreeHandler }));
    const r1 = await h.call("get_navigator_tree", { tree: "PublisherSet" });
    expect(r1.isError).toBe(true);
    expect(r1.text).toContain("publisherSet");
    const r2 = await h.call("get_navigator_tree", { publisherSet: "1 - Виды" });
    expect(h.requests.at(-1)?.parameters).toEqual({ navigatorTreeId: { type: "PublisherSets", name: "1 - Виды" } });
    expect(r2.isError).toBe(false);
    const r3 = await h.call("get_navigator_tree", { tree: "MyViewMap" });
    expect(r3.isError).toBe(true);
    expect(r3.text).toContain("Teamwork");
  });

  it("get_navigator_items dispatches type-specific detail commands and locates items", async () => {
    const h = await harness(
      fakeApi({
        GetNavigatorItemsType: (p) => ({
          navigatorItemIdAndTypeList: guids(p, "navigatorItemIds", "navigatorItemId").map((g: string) =>
            g === STORY
              ? { navigatorItemIdAndType: { navigatorItemId: { guid: g }, navigatorItemType: "StoryItem" } }
              : g === LAYOUT
                ? { navigatorItemIdAndType: { navigatorItemId: { guid: g }, navigatorItemType: "LayoutItem" } }
                : { error: { code: 7400, message: "Navigator item not found" } },
          ),
        }),
        GetStoryNavigatorItems: () => ({
          navigatorItems: [{ storyNavigatorItem: { navigatorItemId: { guid: STORY }, prefix: "3.", name: "3-й этаж", floorNumber: 2, floorLevel: 6 } }],
        }),
        GetLayoutSettings: () => layoutParams(594, 420),
        GetNavigatorItemTree: navTreeHandler,
      }),
    );
    const res = await h.call("get_navigator_items", { ids: [STORY, LAYOUT, "00000000-0000-0000-0000-000000000000"] });
    const j = res.json as any;
    expect(j.items[0]).toMatchObject({ id: STORY, type: "StoryItem", storyIndex: 2, elevation: 6, tree: "ProjectMap", path: "Проект / Этажи / 3. 3-й этаж" });
    expect(j.items[1]).toMatchObject({ id: LAYOUT, type: "LayoutItem", tree: "LayoutBook", name: "План 1" });
    expect(j.items[1].layoutSettings.horizontalSize).toBe(594);
    expect(j.items[2].error).toContain("not found");
  });

  it("rename / move / delete / clone / create folder build the official parameters", async () => {
    const h = await harness(
      fakeApi({
        RenameNavigatorItem: () => ({}),
        MoveNavigatorItem: (p) =>
          p.parentNavigatorItemId.guid === STORY
            ? { succeeded: false, error: { code: 7400, message: `Navigator item not found (navigator item guid: "${STORY}")` } }
            : {},
        DeleteNavigatorItems: (p) => ({ executionResults: p.navigatorItemIds.map((_: unknown, i: number) => (i === 0 ? { success: true } : { success: false, error: { code: 7400, message: "Navigator item not found" } })) }),
        CloneProjectMapItemToViewMap: () => ({ createdNavigatorItemId: { guid: W1 } }),
        CreateViewMapFolder: () => ({ createdFolderNavigatorItemId: { guid: W2 } }),
        GetNavigatorItemTree: navTreeHandler,
      }),
    );
    expect((await h.call("rename_navigator_item", { item: LAYOUT })).isError).toBe(true);
    // the fake tree does not change -> the read-back reports what Archicad kept
    const renamed = (await h.call("rename_navigator_item", { item: LAYOUT, newName: "Новый", newId: "A-1" })).json as any;
    expect(h.requests.find((r) => r.command === "API.RenameNavigatorItem")?.parameters).toEqual({ navigatorItemId: { guid: LAYOUT }, newName: "Новый", newId: "A-1" });
    expect(renamed).toMatchObject({ ok: true, id: LAYOUT, tree: "LayoutBook", type: "LayoutItem", prefix: "03", name: "План 1" });
    expect(renamed.warning).toContain("customLayoutNumber");
    const same = (await h.call("rename_navigator_item", { item: LAYOUT, newName: "План 1" })).json as any;
    expect(same.warning).toBeUndefined();
    expect(h.requests.filter((r) => r.command === "API.RenameNavigatorItem").at(-1)?.parameters).toEqual({ navigatorItemId: { guid: LAYOUT }, newName: "План 1" });
    // a subset ID reads back fine but is recomputed later by Archicad: always point at the lasting alternative
    const subset = (await h.call("rename_navigator_item", { item: SUBSET, newId: "АР" })).json as any;
    expect(subset).toMatchObject({ ok: true, type: "SubsetItem", prefix: "АР" });
    expect(subset.warning).toContain("create_layout_subset {customNumbering: true");

    const moved = (await h.call("move_navigator_item", { item: LAYOUT, parent: SUBSET, after: MASTER })).json as any;
    expect(h.requests.find((r) => r.command === "API.MoveNavigatorItem")?.parameters).toEqual({
      navigatorItemIdToMove: { guid: LAYOUT },
      parentNavigatorItemId: { guid: SUBSET },
      previousNavigatorItemId: { guid: MASTER },
    });
    expect(moved).toMatchObject({ ok: true, tree: "LayoutBook", parent: SUBSET, path: "Без имени / АР Планы / 03 План 1" });
    expect(moved.warning).toBeUndefined();
    const cross = await h.call("move_navigator_item", { item: LAYOUT, parent: STORY });
    expect(cross.isError).toBe(true);
    expect(cross.text).toContain("SAME tree");
    const del = (await h.call("delete_navigator_items", { items: [LAYOUT, SUBSET] })).json as any;
    expect(del.results).toEqual([{ id: LAYOUT, ok: true }, { id: SUBSET, error: "Navigator item not found (code 7400)" }]);
    const clone = (await h.call("clone_project_map_item_to_view_map", { item: STORY })).json as any;
    expect(h.requests.at(-1)?.parameters).toEqual({ projectMapNavigatorItemId: { guid: STORY }, parentNavigatorItemId: { guid: VM_TOP } });
    expect(clone).toEqual({ viewId: W1, parent: VM_TOP });
    const folder = (await h.call("create_view_map_folder", { name: "Мои виды", parent: VM_TOP })).json as any;
    expect(h.requests.at(-1)?.parameters).toEqual({ folderParameters: { name: "Мои виды" }, parentNavigatorItemId: { guid: VM_TOP } });
    expect(folder.folderId).toBe(W2);
  });
});

describe("layouts", () => {
  it("create_layout resolves master/subset by name and passes the master's settings unchanged", async () => {
    let created: any;
    const h = await harness(
      fakeApi({
        GetNavigatorItemTree: navTreeHandler,
        GetLayoutSettings: (p) => (p.layoutNavigatorItemId.guid === MASTER ? layoutParams(420, 297) : layoutParams(420, 297, { displayMasterLayoutBelow: false, layoutPageNumber: 1, actPageIndex: 1 })),
        CreateLayout: (p) => {
          created = p;
          return { createdNavigatorItemId: { guid: W1 } };
        },
      }),
    );
    const res = await h.call("create_layout", { name: "Лист 5", master: "А3 - А - Ф3", parent: "Планы", customLayoutNumbering: true, customLayoutNumber: "A-5" });
    expect(res.isError).toBe(false);
    expect(created.layoutName).toBe("Лист 5");
    expect(created.masterNavigatorItemId).toEqual({ guid: MASTER });
    expect(created.parentNavigatorItemId).toEqual({ guid: SUBSET });
    expect(created.layoutParameters).toMatchObject({ horizontalSize: 420, verticalSize: 297, leftMargin: 20, customLayoutNumbering: true, customLayoutNumber: "A-5", layoutPageNumber: 1 });
    expect((res.json as any).layoutId).toBe(W1);
  });

  it("create_layout defaults to the first master and the Layout Book root; unknown masters list the available ones", async () => {
    let created: any;
    const h = await harness(
      fakeApi({
        GetNavigatorItemTree: navTreeHandler,
        GetLayoutSettings: () => layoutParams(210, 297),
        CreateLayout: (p) => {
          created = p;
          return { createdNavigatorItemId: { guid: W1 } };
        },
      }),
    );
    await h.call("create_layout", { name: "X" });
    expect(created.masterNavigatorItemId).toEqual({ guid: MASTER2 });
    expect(created.parentNavigatorItemId).toEqual({ guid: LB_ROOT });
    const bad = await h.call("create_layout", { name: "Y", master: "А9" });
    expect(bad.isError).toBe(true);
    expect(bad.text).toContain("'Обложка'");
  });

  it("set_layout_settings merges with the current settings and guards master-owned paper size", async () => {
    let set: any;
    const h = await harness(
      fakeApi({
        GetNavigatorItemTree: navTreeHandler,
        GetLayoutSettings: () => layoutParams(594, 420, { displayMasterLayoutBelow: false }),
        GetNavigatorItemsType: (p) => ({
          navigatorItemIdAndTypeList: guids(p, "navigatorItemIds", "navigatorItemId").map((g: string) => ({
            navigatorItemIdAndType: { navigatorItemId: { guid: g }, navigatorItemType: g === MASTER ? "MasterLayoutItem" : "LayoutItem" },
          })),
        }),
        SetLayoutSettings: (p) => {
          set = p;
          return {};
        },
      }),
    );
    const guarded = (await h.call("set_layout_settings", { layouts: [{ layout: LAYOUT, horizontalSize: 420 }] })).json as any;
    expect(guarded.results[0].error).toContain("master layout");
    expect(set).toBeUndefined();
    const ok = (await h.call("set_layout_settings", { layouts: [{ layout: "03 План 1", customLayoutNumbering: true, customLayoutNumber: "Z-1" }] })).json as any;
    expect(ok.results[0].layout).toBe(LAYOUT);
    expect(set.layoutNavigatorItemId).toEqual({ guid: LAYOUT });
    expect(set.layoutParameters).toMatchObject({ horizontalSize: 594, customLayoutNumbering: true, customLayoutNumber: "Z-1", displayMasterLayoutBelow: false });
    await h.call("set_layout_settings", { layouts: [{ layout: MASTER, horizontalSize: 420, verticalSize: 297 }] });
    expect(set.layoutNavigatorItemId).toEqual({ guid: MASTER });
    expect(set.layoutParameters.horizontalSize).toBe(420);
  });

  it("set_layout_settings reports fields Archicad ignored and keeps displayMasterLayoutBelow for masters", async () => {
    const sets: any[] = [];
    const h = await harness(
      fakeApi({
        GetNavigatorItemTree: navTreeHandler,
        // Archicad applies the custom number but keeps doNotIncludeInNumbering false
        GetLayoutSettings: (p) =>
          layoutParams(420, 297, p.layoutNavigatorItemId.guid === MASTER ? {} : { customLayoutNumbering: true, customLayoutNumber: "Z-2", displayMasterLayoutBelow: false }),
        GetNavigatorItemsType: (p) => ({
          navigatorItemIdAndTypeList: guids(p, "navigatorItemIds", "navigatorItemId").map((g: string) => ({
            navigatorItemIdAndType: { navigatorItemId: { guid: g }, navigatorItemType: g === MASTER ? "MasterLayoutItem" : "LayoutItem" },
          })),
        }),
        SetLayoutSettings: (p) => {
          sets.push(p);
          return {};
        },
      }),
    );
    const res = (
      await h.call("set_layout_settings", {
        layouts: [
          { layout: LAYOUT, customLayoutNumbering: true, customLayoutNumber: "Z-2", doNotIncludeInNumbering: true },
          { layout: LAYOUT, displayMasterLayoutBelow: true },
          { layout: MASTER, displayMasterLayoutBelow: true },
        ],
      })
    ).json as any;
    expect(res.results[0].ignored).toEqual(["doNotIncludeInNumbering"]);
    expect(res.results[1].error).toContain("master layout setting");
    expect(res.results[2].type).toBe("MasterLayoutItem");
    expect(res.results[2].ignored).toBeUndefined();
    expect(sets).toHaveLength(2);
    expect(sets[1].layoutParameters.displayMasterLayoutBelow).toBe(true);
    // page count / index are output-only: strict input validation names the unknown key
    const bad = await h.call("set_layout_settings", { layouts: [{ layout: LAYOUT, layoutPageNumber: 3 }] });
    expect(bad.isError).toBe(true);
    expect(bad.text).toContain("layoutPageNumber");
  });

  it("create_layout reports requested fields that Archicad did not apply", async () => {
    const h = await harness(
      fakeApi({
        GetNavigatorItemTree: navTreeHandler,
        GetLayoutSettings: () => layoutParams(420, 297, { displayMasterLayoutBelow: false }),
        CreateLayout: () => ({ createdNavigatorItemId: { guid: W1 } }),
      }),
    );
    const res = (await h.call("create_layout", { name: "Z", master: MASTER, doNotIncludeInNumbering: true })).json as any;
    expect(res.layoutId).toBe(W1);
    expect(res.master).toBe("А3 - А - Ф3");
    expect(res.ignored).toEqual(["doNotIncludeInNumbering"]);
  });

  it("create_layout_subset fills every Subset field with defaults", async () => {
    const h = await harness(fakeApi({ GetNavigatorItemTree: navTreeHandler, CreateLayoutSubset: () => ({ createdSubsetId: { guid: W2 } }) }));
    const res = await h.call("create_layout_subset", { name: "Разрезы", ownPrefix: "S-", numberingStyle: "01" });
    expect(h.requests.at(-1)?.parameters).toEqual({
      subsetParameters: {
        name: "Разрезы",
        includeToIDSequence: true,
        customNumbering: false,
        continueNumbering: false,
        useUpperPrefix: true,
        addOwnPrefix: true,
        customNumber: "",
        autoNumber: "",
        numberingStyle: "01",
        startAt: 1,
        ownPrefix: "S-",
      },
      parentNavigatorItemId: { guid: LB_ROOT },
    });
    expect((res.json as any).subsetId).toBe(W2);
  });

  it("get_publisher_sets lists names and expands one set", async () => {
    const h = await harness(fakeApi({ GetPublisherSetNames: () => ({ publisherSetNames: ["1 - Виды", "2 - Макеты"] }), GetNavigatorItemTree: navTreeHandler }));
    expect((await h.call("get_publisher_sets", {})).json).toEqual({ publisherSets: ["1 - Виды", "2 - Макеты"] });
    const one = (await h.call("get_publisher_sets", { name: "виды" })).json as any;
    expect(one.name).toBe("1 - Виды");
    expect(one).toMatchObject({ total: 1, truncated: false });
    expect(one.items[0].id).toBe(VM_TOP);
    expect((await h.call("get_publisher_sets", { name: "zzz" })).isError).toBe(true);
  });
});

describe("attributes", () => {
  it("get_attribute_folders expands folders and resolves attribute names", async () => {
    const h = await harness(
      fakeApi({
        GetAttributeFolder: (p) => ({ attributeFolder: { attributeType: "Layer", path: p.attributeFolder.path ?? [], attributeFolderId: { guid: SYS } } }),
        GetAttributeFolderContent: (p) =>
          p.attributeFolder.attributeFolderId?.guid === SYS
            ? {
                attributeFolderContent: {
                  subfolders: [{ attributeType: "Layer", path: ["Конструктив"], attributeFolderId: { guid: FOLDER } }],
                  attributeIds: [{ attributeId: { guid: LAYER1 } }],
                },
              }
            : { attributeFolderContent: { subfolders: [], attributeIds: [{ attributeId: { guid: LAYER2 } }] } },
        GetLayerAttributes: (p) => ({
          attributes: guids(p, "attributeIds", "attributeId").map((g: string) => ({ layerAttribute: { attributeId: { guid: g }, name: g === LAYER1 ? "Слой Archicad" : "Конструктив - Стены" } })),
        }),
      }),
    );
    const res = await h.call("get_attribute_folders", { attributeType: "Layer" });
    expect(res.json).toEqual({
      attributeType: "Layer",
      path: [],
      guid: SYS,
      attributes: [{ guid: LAYER1, name: "Слой Archicad" }],
      subfolders: [{ path: ["Конструктив"], guid: FOLDER, attributes: [{ guid: LAYER2, name: "Конструктив - Стены" }] }],
    });
    expect(h.requests.filter((r) => r.command === "API.GetLayerAttributes")).toHaveLength(1);
  });

  it("create / delete attribute folders accept 'A/B' strings and per-folder types; the root is protected", async () => {
    const h = await harness(
      fakeApi({
        CreateAttributeFolders: (p) => ({ executionResults: p.attributeFolders.map(() => ({ success: true })) }),
        DeleteAttributeFolders: (p) => ({ executionResults: p.attributeFolders.map(() => ({ success: true })) }),
        GetAttributeFolder: () => ({ attributeFolder: { attributeFolderId: { guid: FOLDER } } }),
      }),
    );
    const created = (await h.call("create_attribute_folders", { attributeType: "Layer", folders: ["Проект/Стены", { attributeType: "Surface", path: ["Отделка"] }] })).json as any;
    const req = h.requests.find((r) => r.command === "API.CreateAttributeFolders")!;
    expect(req.parameters).toEqual({
      attributeFolders: [
        { attributeType: "Layer", path: ["Проект", "Стены"] },
        { attributeType: "Surface", path: ["Отделка"] },
      ],
    });
    expect(created.results[0]).toMatchObject({ folder: "/Проект/Стены", ok: true, guid: FOLDER });
    const root = await h.call("delete_attribute_folders", { attributeType: "Layer", folders: [[]] });
    expect(root.isError).toBe(true);
    const noType = await h.call("delete_attribute_folders", { folders: ["A"] });
    expect(noType.isError).toBe(true);
    expect(noType.text).toContain("attributeType");
    await h.call("delete_attribute_folders", { attributeType: "Layer", folders: [{ guid: FOLDER }] });
    expect(h.requests.at(-1)?.parameters).toEqual({ attributeFolders: [{ attributeType: "Layer", attributeFolderId: { guid: FOLDER } }] });
  });

  it("move_attributes_to_folder resolves attribute names; rename_attribute_folder protects the root", async () => {
    const h = await harness(
      fakeApi({
        GetAttributesByType: () => ({ attributeIds: [{ attributeId: { guid: LAYER1 } }, { attributeId: { guid: LAYER2 } }] }),
        GetLayerAttributes: (p) => ({
          attributes: guids(p, "attributeIds", "attributeId").map((g: string) => ({ layerAttribute: { attributeId: { guid: g }, name: g === LAYER1 ? "Слой Archicad" : "Конструктив - Стены" } })),
        }),
        MoveAttributesAndFolders: () => ({}),
        GetAttributeFolder: (p) => ({ attributeFolder: { attributeType: "Layer", path: p.attributeFolder.path ?? ["X"], attributeFolderId: { guid: FOLDER } } }),
        RenameAttributeFolder: () => ({}),
      }),
    );
    const res = await h.call("move_attributes_to_folder", { attributeType: "Layer", target: ["Проект"], attributes: ["конструктив - стены", LAYER1], folders: ["Старое"] });
    expect(res.isError).toBe(false);
    expect(h.requests.find((r) => r.command === "API.MoveAttributesAndFolders")?.parameters).toEqual({
      folders: [{ attributeType: "Layer", path: ["Старое"] }],
      attributeIds: [{ attributeId: { guid: LAYER2 } }, { attributeId: { guid: LAYER1 } }],
      targetFolder: { attributeType: "Layer", path: ["Проект"] },
    });
    const bad = await h.call("move_attributes_to_folder", { attributeType: "Layer", target: [], attributes: ["Нет"] });
    expect(bad.isError).toBe(true);
    expect(bad.text).toContain("get_attributes");
    const special = await harness(
      fakeApi({ MoveAttributesAndFolders: () => ({ succeeded: false, error: { code: 6124, message: "Cannot move AC layer attribute" } }) }),
    );
    const sp = await special.call("move_attributes_to_folder", { attributeType: "Layer", target: ["A"], attributes: [LAYER1] });
    expect(sp.isError).toBe(true);
    expect(sp.text).toContain("root folder");
    const root = await h.call("rename_attribute_folder", { attributeType: "Layer", folder: [], newName: "X" });
    expect(root.isError).toBe(true);
    const ok = await h.call("rename_attribute_folder", { attributeType: "Layer", folder: "Проект", newName: "Проект 2" });
    expect(ok.isError).toBe(false);
    expect(h.requests.find((r) => r.command === "API.RenameAttributeFolder")?.parameters).toEqual({
      attributeFolder: { attributeType: "Layer", path: ["Проект"] },
      newName: "Проект 2",
    });
  });

  it("get_profile_preview returns PNG image content plus profile info", async () => {
    const h = await harness(
      fakeApi({
        GetProfileAttributePreview: () => ({ previewImages: [{ image: { content: "iVBORw0KGgo=" } }] }),
        GetProfileAttributes: () => ({ attributes: [{ profileAttribute: { attributeId: { guid: PROFILE }, name: "Профиль", useWith: ["Wall"], width: 0.23 } }] }),
      }),
    );
    const res = await h.call("get_profile_preview", { profiles: [PROFILE], width: 100, height: 80, background: "#FF0000" });
    expect(res.isError).toBe(false);
    expect(h.requests.find((r) => r.command === "API.GetProfileAttributePreview")?.parameters).toEqual({
      attributeIds: [{ attributeId: { guid: PROFILE } }],
      imageWidth: 100,
      imageHeight: 80,
      backgroundColor: { red: 1, green: 0, blue: 0 },
    });
    expect(res.content[0]).toMatchObject({ type: "image", mimeType: "image/png", data: "iVBORw0KGgo=" });
    expect(JSON.parse(res.text)).toEqual({ profiles: [{ guid: PROFILE, name: "Профиль", useWith: ["Wall"], width: 0.23, image: 1 }] });
  });

  it("get_profile_preview rejects attribute indices without the add-on", async () => {
    const h = await harness(fakeApi({}));
    const res = await h.call("get_profile_preview", { profiles: [3] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("GUID");
  });

  it("get_active_pen_tables names both tables and can list pens", async () => {
    const h = await harness(
      fakeApi({
        GetActivePenTables: () => ({ modelViewPenTableId: { attributeId: { guid: PT1 } }, layoutBookPenTableId: { attributeId: { guid: PT2 } } }),
        GetPenTableAttributes: (p) => ({
          attributes: guids(p, "attributeIds", "attributeId").map((g: string) => ({
            penTableAttribute: {
              attributeId: { guid: g },
              name: g === PT1 ? "01 Архитектурный" : "02 Архитектурный",
              pens: [
                { pen: { index: 1, name: "1", color: { red: 0, green: 0, blue: 0 }, weight: 0.15, description: "Общее" } },
                { pen: { index: 2, name: "2", color: { red: 1, green: 0.5, blue: 0 }, weight: 0.25, description: "" } },
              ],
            },
          })),
        }),
      }),
    );
    const res = await h.call("get_active_pen_tables", { includePens: true, pens: [2] });
    expect(res.json).toEqual({
      modelView: { guid: PT1, name: "01 Архитектурный", pens: [{ index: 2, color: "#FF8000", weight: 0.25 }] },
      layoutBook: { guid: PT2, name: "02 Архитектурный", pens: [{ index: 2, color: "#FF8000", weight: 0.25 }] },
    });
  });

  it("surfaces official API failures as tool errors", async () => {
    const h = await harness(() => ({ succeeded: false, error: { code: 6005, message: "Teamwork project required" } }));
    const res = await h.call("get_publisher_sets", {});
    expect(res.isError).toBe(true);
    expect(res.text).toContain("Teamwork project required");
  });
});
