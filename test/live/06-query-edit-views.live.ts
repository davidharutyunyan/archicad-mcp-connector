import { LiveTest, guids, near } from "./lib.js";

const t = await new LiveTest("query-edit-views").start();
const X = 500;
await t.ok("story 0", "set_current_story", { story: 0 });
await t.ok("open floor plan", "open_view", { window: "FloorPlan", story: 0 });
const w = await t.ok("setup walls", "create_walls", { walls: [
  { begin: { x: X, y: 0 }, end: { x: X + 8, y: 0 }, height: 3, thickness: 0.3 }, { begin: { x: X + 8, y: 0 }, end: { x: X + 8, y: 6 }, height: 3, thickness: 0.3 },
  { begin: { x: X + 8, y: 6 }, end: { x: X, y: 6 }, height: 3, thickness: 0.3 }, { begin: { x: X, y: 6 }, end: { x: X, y: 0 }, height: 3, thickness: 0.3 },
] });
const wg = guids(w);
const sl = await t.ok("setup slab", "create_slabs", { slabs: [{ polygon: [{ x: X, y: 0 }, { x: X + 8, y: 0 }, { x: X + 8, y: 6 }, { x: X, y: 6 }], thickness: 0.2 }] });
const win = await t.ok("setup window", "create_windows", { windows: [{ wall: wg[0], position: 4, width: 1.2, height: 1.4, sillHeight: 0.9 }] });

// query
await t.ok("find_elements region", "find_elements", { types: ["Wall"], region: { xMin: X - 1, xMax: X + 9, yMin: -1, yMax: 7 } }, (j) => j.total === 4 || `total ${j.total}`);
await t.ok("get_element_counts", "get_element_counts", {}, (j) => JSON.stringify(j).includes("Wall"));
await t.ok("get_element_quantities wall/slab/window", "get_element_quantities", { elements: [wg[0], guids(sl)[0], guids(win)[0]] }, (j) => JSON.stringify(j).match(/volume|area/i) !== null);
await t.ok("get_connected_elements wall->window", "get_connected_elements", { elements: [wg[0]] }, (j) => JSON.stringify(j).includes(guids(win)[0]!) || JSON.stringify(j).slice(0, 300));
await t.ok("get_element_relations", "get_element_relations", { elements: [wg[0]] });
await t.ok("set_selection", "set_selection", { mode: "set", elements: wg.slice(0, 2) });
await t.ok("get_selection", "get_selection", {}, (j) => (j.elements?.length ?? j.total) === 2 || JSON.stringify(j).slice(0, 200));
await t.ok("clear selection", "set_selection", { mode: "clear" });
await t.ok("get_element_2d_geometry", "get_element_2d_geometry", { elements: [wg[0]], summaryOnly: true });
await t.ok("get_element_3d_geometry", "get_element_3d_geometry", { elements: [guids(sl)[0]], mode: "summary" });

// edit
const cp = await t.ok("copy_elements", "copy_elements", { elements: [wg[0]], vector: { x: 0, y: 10 } }, (j) => JSON.stringify(j).match(/[0-9A-F]{8}-/) !== null);
const cpG: string[] = JSON.stringify(cp).match(/[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}/g)?.filter((g) => g !== wg[0]) ?? [];
await t.ok("move_elements", "move_elements", { elements: [cpG[0]], vector: { x: 1, y: 0 } });
const md = await t.ok("moved wall position", "get_element_details", { elements: [cpG[0]] });
t.check("moved by (1,10)", near(md?.elements?.[0]?.details?.begin?.x, X + 1) && near(md?.elements?.[0]?.details?.begin?.y, 10), JSON.stringify(md?.elements?.[0]?.details?.begin));
await t.ok("rotate_elements", "rotate_elements", { elements: [cpG[0]], angle: 90, center: { x: X + 1, y: 10 } });
await t.ok("mirror_elements copy", "mirror_elements", { elements: [cpG[0]], axis: "Vertical", through: { x: X + 20, y: 10 }, copy: true });
await t.ok("elevate_elements", "elevate_elements", { elements: [cpG[0]], deltaZ: 0.5 });
await t.ok("group_elements", "group_elements", { elements: wg });
await t.ok("ungroup_elements", "ungroup_elements", { elements: wg });
await t.ok("lock_elements", "lock_elements", { elements: [wg[1]] });
await t.fails("locked wall cannot be modified", "modify_elements", { elements: [{ guid: wg[1], height: 2 }] });
await t.ok("unlock_elements", "unlock_elements", { elements: [wg[1]] });
await t.ok("set_draw_order", "set_draw_order", { elements: [guids(sl)[0]], action: "SendToBack" });
await t.ok("copy_elements_to_stories", "copy_elements_to_stories", { elements: [wg[0]], stories: [1] });
const box = await t.ok("morph for solid op", "create_morphs", { morphs: [{ box: { origin: { x: X + 2, y: -0.5, z: 0.5 }, size: { x: 1, y: 1, z: 1 } } }] });
await t.ok("solid_operation subtract", "solid_operation", { target: wg[0], operators: [guids(box)[0]], operation: "Subtract" });
await t.ok("remove_solid_operation", "remove_solid_operation", { links: [{ target: wg[0], operator: guids(box)[0] }] });
await t.ok("delete_elements", "delete_elements", { elements: cpG });

// views
await t.ok("get_current_window", "get_current_window", {});
await t.ok("zoom elements", "zoom", { mode: "elements", elements: wg });
const cap = await t.raw("capture_view", { maxSize: 800 });
t.check("capture_view floor plan image", !cap.isError && cap.images.length === 1, cap.text.slice(0, 300));
console.log(`    image: ${cap.images[0]}`);
await t.ok("set_3d_view axonometric", "set_3d_view", { mode: "axonometric", azimuth: 225, altitude: 35 });
await t.ok("get_3d_view", "get_3d_view", {});
await t.ok("show_in_3d", "show_in_3d", { mode: "all" });
const cap3 = await t.raw("capture_view", { threeD: { mode: "perspective", camera: { x: X - 10, y: -12, z: 8 }, target: { x: X + 4, y: 3, z: 1 } }, width: 1000, height: 700, format: "jpeg" });
t.check("capture_view 3D image", !cap3.isError && cap3.images.length === 1, cap3.text.slice(0, 300));
console.log(`    image: ${cap3.images[0]}`);
const sec = await t.ok("create_sections", "create_sections", { sections: [{ begin: { x: X - 2, y: 3 }, end: { x: X + 10, y: 3 }, name: "LT Section" }] });
await t.ok("create_elevations", "create_elevations", { elevations: [{ begin: { x: X - 2, y: -5 }, end: { x: X + 10, y: -5 }, name: "LT Elevation" }] });
await t.ok("create_interior_elevations", "create_interior_elevations", { interiorElevations: [{ points: [{ x: X + 0.5, y: 0.5 }, { x: X + 7.5, y: 0.5 }, { x: X + 7.5, y: 5.5 }, { x: X + 0.5, y: 5.5 }], closed: true, name: "LT IE" }] });
await t.ok("create_details", "create_details", { details: [{ box: { xMin: X - 1, yMin: -1, xMax: X + 2, yMax: 2 }, name: "LT Detail" }] });
await t.ok("create_worksheets", "create_worksheets", { worksheets: [{ box: { xMin: X + 6, yMin: -1, xMax: X + 9, yMax: 2 }, name: "LT Worksheet" }] });
await t.ok("open section view", "open_view", { window: "Section", element: guids(sec)[0] });
const capS = await t.raw("capture_view", { zoom: { mode: "fit" }, maxSize: 800 });
t.check("capture section", !capS.isError && capS.images.length === 1, capS.text.slice(0, 300));
console.log(`    image: ${capS.images[0]}`);
await t.ok("get_view_settings", "get_view_settings", {});
await t.ok("back to plan", "open_view", { window: "FloorPlan", story: 0 });
// render_view needs a working CineRender engine; on this machine it can crash and leave a modal
// dialog that blocks the following suites, so it only runs with LIVE_RENDER=1.
if (process.env["LIVE_RENDER"]) {
  const rv = await t.raw("render_view", { width: 640, height: 400 });
  t.check("render_view", !rv.isError && rv.images.length === 1, rv.text.slice(0, 300));
}
await t.finish();
