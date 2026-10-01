import { LiveTest, guids } from "./lib.js";

const t = await new LiveTest("properties").start();
const X = 700;
// Regression: model elements must be creatable while a layout is the active window.
await t.ok("layout for regression", "create_layout", { name: "LT Layout P" });
await t.ok("open layout window", "open_view", { window: "Layout", name: "LT Layout P" });
const w = await t.ok("model (created while a layout is active)", "create_walls", { walls: [{ begin: { x: X, y: 0 }, end: { x: X + 5, y: 0 }, height: 3 }, { begin: { x: X, y: 3 }, end: { x: X + 5, y: 3 }, height: 3 }] });
const wg = guids(w);
await t.ok("get_property_definitions summary", "get_property_definitions", { kind: "Custom", limit: 20 });
await t.ok("get_property_definitions for elements", "get_property_definitions", { elements: [wg[0]], limit: 10 });
await t.ok("create_property_groups", "create_property_groups", { groups: [{ name: "LT Group", description: "live test" }] });
await t.ok("create_property_definitions", "create_property_definitions", { definitions: [
  { group: "LT Group", name: "LT Text", type: "string", defaultValue: "n/a" },
  { group: "LT Group", name: "LT Length", type: "length", defaultValue: 1.5 },
  { group: "LT Group", name: "LT Flag", type: "boolean", defaultValue: false },
  { group: "LT Group", name: "LT Grade", type: "singleEnum", enumValues: ["A", "B", "C"], defaultValue: "B" },
] });
await t.ok("set_property_values by name", "set_property_values", { values: [
  { elements: wg, property: "LT Group/LT Text", value: "Hello" },
  { elements: [wg[0]], property: { group: "LT Group", name: "LT Length" }, value: 2.25 },
  { elements: [wg[0]], property: "LT Group/LT Flag", value: true },
  { elements: [wg[1]], property: "LT Group/LT Grade", value: "C" },
] });
const pv = await t.ok("get_property_values", "get_property_values", { elements: wg, properties: ["LT Group/LT Text", "LT Group/LT Length", "LT Group/LT Flag", "LT Group/LT Grade"] });
const txt = JSON.stringify(pv);
t.check("values read back", txt.includes("Hello") && txt.includes("2.25") && txt.includes('"C"'), txt.slice(0, 500));
await t.ok("builtin property values", "get_property_values", { elements: [wg[0]], scope: "BuiltIn", includeDisplay: true }, (j) => JSON.stringify(j).length > 200);
await t.ok("modify_property_definitions add enum", "modify_property_definitions", { definitions: [{ property: "LT Group/LT Grade", addEnumValues: ["D"] }] });
await t.ok("get_attribute_property_values", "get_attribute_property_values", { attributes: [{ type: "BuildingMaterial", attribute: 1 }] });
// IFC
await t.ok("get_ifc_data", "get_ifc_data", { elements: [wg[0]], include: ["identity", "type", "properties"] }, (j) => JSON.stringify(j).match(/GlobalId|globalId|ifcGuid/i) !== null || JSON.stringify(j).slice(0, 300));
await t.ok("set_ifc_properties", "set_ifc_properties", { properties: [{ elements: [wg[0]], propertySet: "Pset_LiveTest", name: "Note", value: "ok" }] });
const ifc2 = await t.ok("read ifc property", "get_ifc_data", { elements: [wg[0]], include: ["properties"], propertySets: ["Pset_LiveTest"] });
t.check("ifc property stored", JSON.stringify(ifc2).includes("Pset_LiveTest"), JSON.stringify(ifc2).slice(0, 300));
// classifications
await t.ok("get_classification_systems", "get_classification_systems", { includeItemCounts: true });
await t.ok("create_classification_system", "create_classification_system", { name: "LT Class", editionVersion: "1.0", items: [{ id: "LT-1", name: "Walls", children: [{ id: "LT-1.1", name: "Exterior walls" }] }] });
await t.ok("create_classification_items", "create_classification_items", { system: "LT Class", items: [{ id: "LT-2", name: "Slabs" }] });
await t.ok("get_classification_tree", "get_classification_tree", { system: "LT Class" }, (j) => JSON.stringify(j).includes("LT-1.1"));
await t.ok("set_element_classifications", "set_element_classifications", { assignments: [{ elements: wg, system: "LT Class", item: "LT-1.1" }] });
await t.ok("get_element_classifications", "get_element_classifications", { elements: wg, systems: ["LT Class"] }, (j) => JSON.stringify(j).includes("LT-1.1"));
await t.ok("modify_classification_items", "modify_classification_items", { system: "LT Class", items: [{ item: "LT-2", name: "Floor slabs" }] });
// cleanup
await t.ok("delete_classification_systems", "delete_classification_systems", { systems: ["LT Class"] });
await t.ok("delete_property_definitions", "delete_property_definitions", { properties: ["LT Group/LT Text", "LT Group/LT Length", "LT Group/LT Flag", "LT Group/LT Grade"] });
await t.ok("delete_property_groups", "delete_property_groups", { groups: ["LT Group"] });
await t.finish();
