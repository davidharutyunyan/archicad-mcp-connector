import { LiveTest } from "./lib.js";

const t = await new LiveTest("attributes").start();
const P = "LT-attr-";

for (const type of ["Pen", "Layer", "Line", "Fill", "Composite", "Surface", "LayerCombination", "ZoneCategory", "Profile", "PenTable", "DimensionStandard", "ModelViewOption", "MEPSystem", "OperationProfile", "BuildingMaterial", "Font"]) {
  await t.ok(`get_attributes ${type}`, "get_attributes", { type, limit: 3, detailed: true }, (j) => (j.attributes?.length ?? j.items?.length ?? 0) > 0 || `no items: ${JSON.stringify(j).slice(0, 200)}`);
}
const bm = await t.ok("get_attributes BuildingMaterial names", "get_attributes", { type: "BuildingMaterial", limit: 2 });
const bmName = (bm?.attributes ?? bm?.items)?.[0]?.name;
console.log(`    first building material: ${bmName}`);

await t.ok("create_layers", "create_layers", { layers: [{ name: P + "Layer A" }, { name: P + "Layer B", hidden: true, locked: true }] });
await t.ok("create_layers ifExists skip", "create_layers", { layers: [{ name: P + "Layer A", ifExists: "skip" }] });
await t.fails("create_layers duplicate error", "create_layers", { layers: [{ name: P + "Layer A", ifExists: "error" }] });
await t.ok("set_layer_states", "set_layer_states", { layers: [{ layer: P + "Layer B", hidden: false, locked: false }] });
const lb = await t.ok("read layer B", "get_attributes", { type: "Layer", attributes: [P + "Layer B"], detailed: true });
const lbItem = (lb?.attributes ?? lb?.items)?.[0];
t.check("layer B visible+unlocked", lbItem && lbItem.hidden === false && lbItem.locked === false, JSON.stringify(lbItem).slice(0, 300));
await t.ok("create_layer_combinations", "create_layer_combinations", { layerCombinations: [{ name: P + "Combo", base: "allVisible", layers: [{ layer: P + "Layer B", hidden: true }] }] });
await t.ok("apply_layer_combination", "apply_layer_combination", { layerCombination: P + "Combo" });
await t.ok("create_building_materials", "create_building_materials", { buildingMaterials: [{ name: P + "BM", thermalConductivity: 0.5, density: 1200, heatCapacity: 900 }] });
await t.ok("create_composites", "create_composites", { composites: [{ name: P + "Comp", skins: [{ thickness: 0.02, buildingMaterial: P + "BM", finish: true }, { thickness: 0.2, buildingMaterial: bmName, core: true }] }] });
const comp = await t.ok("read composite", "get_attributes", { type: "Composite", attributes: [P + "Comp"], detailed: true });
const compItem = (comp?.attributes ?? comp?.items)?.[0];
t.check("composite thickness 0.22", compItem && Math.abs((compItem.totalThickness ?? compItem.thickness) - 0.22) < 1e-6, JSON.stringify(compItem).slice(0, 400));
await t.ok("create_surfaces", "create_surfaces", { surfaces: [{ name: P + "Surf", color: "#3366CC", transparency: 20 }] });
await t.ok("create_fills solid", "create_fills", { fills: [{ name: P + "Fill", fillType: "Solid" }] });
await t.ok("create_line_types dashed", "create_line_types", { lineTypes: [{ name: P + "Dash", lineType: "Dashed", dashes: [{ dash: 0.005, gap: 0.002 }] }] });
await t.ok("create_zone_categories", "create_zone_categories", { zoneCategories: [{ name: P + "ZoneCat", code: "LT", color: "#FFCC00" }] });
await t.ok("create_profiles", "create_profiles", { profiles: [{ name: P + "Prof", shapes: [{ polygon: [{ x: 0, y: 0 }, { x: 0.2, y: 0 }, { x: 0.2, y: 0.4 }, { x: 0, y: 0.4 }], buildingMaterial: bmName }] }] });
await t.ok("duplicate_attributes", "duplicate_attributes", { type: "Surface", attributes: [{ source: P + "Surf", name: P + "Surf copy" }] });
await t.ok("modify_attributes rename", "modify_attributes", { attributes: [{ type: "Surface", attribute: P + "Surf copy", name: P + "Surf renamed" }] });
await t.ok("modify_pens", "modify_pens", { pens: [{ index: 200, width: 0.35 }] });
// wall using created composite
const w = await t.ok("wall with created composite", "create_walls", { walls: [{ begin: { x: 100, y: 0 }, end: { x: 104, y: 0 }, height: 3, composite: P + "Comp", layer: P + "Layer A" }] });
const wg = w?.results?.[0]?.guid;
await t.fails("delete layer with elements refused", "delete_attributes", { type: "Layer", attributes: [P + "Layer A"] });
if (wg) await t.ok("delete wall", "execute_addon_command", { command: "DeleteElements", parameters: { elements: [wg] } });
for (const [type, names] of [["LayerCombination", ["Combo"]], ["Layer", ["Layer A", "Layer B"]], ["Composite", ["Comp"]], ["Profile", ["Prof"]], ["BuildingMaterial", ["BM"]], ["Surface", ["Surf", "Surf renamed"]], ["Fill", ["Fill"]], ["Line", ["Dash"]], ["ZoneCategory", ["ZoneCat"]]] as const) {
  await t.ok(`delete_attributes ${type}`, "delete_attributes", { type, attributes: names.map((n) => P + n), force: true });
}
const left = await t.ok("nothing left", "get_attributes", { type: "Layer", nameFilter: P });
t.check("layers cleaned", ((left?.attributes ?? left?.items)?.length ?? 0) === 0, JSON.stringify(left).slice(0, 200));
await t.finish();
