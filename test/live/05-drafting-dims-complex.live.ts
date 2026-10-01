import { LiveTest, guids, near } from "./lib.js";

const t = await new LiveTest("drafting-dims-complex").start();
const X = 400;
await t.ok("story 0", "set_current_story", { story: 0 });

// drafting
const ln = await t.ok("create_lines", "create_lines", { lines: [{ begin: { x: X, y: 0 }, end: { x: X + 5, y: 0 }, pen: 3 }, { begin: { x: X, y: 1 }, end: { x: X + 5, y: 2 } }] });
await t.ok("create_arcs center/radius", "create_arcs", { arcs: [{ center: { x: X + 8, y: 0 }, radius: 2, beginAngle: 0, endAngle: 120 }] });
await t.ok("create_circles", "create_circles", { circles: [{ center: { x: X + 12, y: 0 }, radius: 1 }] });
await t.ok("create_polylines", "create_polylines", { polylines: [{ points: [{ x: X, y: 5 }, { x: X + 3, y: 5 }, { x: X + 3, y: 8 }], arcs: [{ index: 1, angle: 45 }] }] });
await t.ok("create_splines", "create_splines", { splines: [{ points: [{ x: X, y: 10 }, { x: X + 2, y: 12 }, { x: X + 4, y: 10 }, { x: X + 6, y: 12 }] }] });
const fills = await t.ok("fills", "get_attributes", { type: "Fill", limit: 3 });
const fillName = (fills?.attributes ?? fills?.items)?.[0]?.name;
const ha = await t.ok("create_hatches", "create_hatches", { hatches: [{ polygon: [{ x: X, y: 15 }, { x: X + 4, y: 15 }, { x: X + 4, y: 18 }, { x: X, y: 18 }], fillType: fillName, showArea: true }] });
const tx = await t.ok("create_texts multi-line + cyrillic", "create_texts", { texts: [{ position: { x: X, y: 20 }, text: "Hello Archicad\nВторая строка", size: 3.5, bold: true, angle: 15 }] });
const txd = await t.ok("text details", "get_element_details", { elements: guids(tx) });
t.check("text content round-trip", JSON.stringify(txd).includes("Вторая строка"), JSON.stringify(txd?.elements?.[0]?.details).slice(0, 300));
const w = await t.ok("wall for label", "create_walls", { walls: [{ begin: { x: X, y: 25 }, end: { x: X + 6, y: 25 }, height: 3 }] });
await t.ok("create_labels associative + free", "create_labels", { labels: [{ parent: guids(w)[0], text: "Wall label" }, { begin: { x: X, y: 30 }, text: "Free label" }] });
await t.ok("create_hotspots", "create_hotspots", { hotspots: [{ position: { x: X + 1, y: 32 }, height: 1 }] });
await t.ok("create_pictures", "create_pictures", { pictures: [{ file: "/private/tmp/claude-connector-tests/red.png", position: { x: X, y: 34 }, width: 2, height: 2 }] });
await t.ok("modify line via modify_elements", "modify_elements", { elements: [{ guid: guids(ln)[0], end: { x: X + 6, y: 0 } }] });

// Element ID: Archicad has none for lines/polylines/texts/labels/dimensions/...; hatches have one.
// Regression: a failed elementId used to leave the created element behind.
await t.fails("create_polylines rejects elementId (schema)", "create_polylines", { polylines: [{ points: [{ x: X, y: 36 }, { x: X + 1, y: 36 }], elementId: "LT-P" }] }, /elementId/);
const plBefore = await t.ok("count polylines", "get_element_counts", { types: ["PolyLine"] });
await t.fails("create_elements PolyLine + elementId: item error", "create_elements", { elements: [{ type: "PolyLine", points: [{ x: X, y: 36 }, { x: X + 1, y: 36 }], elementId: "LT-P" }] }, /no Element ID/);
await t.ok("no orphan polyline", "get_element_counts", { types: ["PolyLine"] }, (j) => j.total === plBefore?.total || `before ${plBefore?.total}, after ${j.total}`);
await t.fails("modify_elements line elementId: item error", "modify_elements", { elements: [{ guid: guids(ln)[0], elementId: "LT-L" }] }, /no Element ID/);
const hid = await t.ok("hatch keeps elementId", "create_hatches", { hatches: [{ polygon: [{ x: X + 6, y: 15 }, { x: X + 8, y: 15 }, { x: X + 8, y: 16 }], elementId: "LT-H" }] });
await t.ok("hatch elementId read back", "get_element_details", { elements: guids(hid) }, (j) => j.elements?.[0]?.elementId === "LT-H" || `id ${j.elements?.[0]?.elementId}`);

// dimensions
await t.ok("create_dimensions static", "create_dimensions", { dimensions: [{ points: [{ x: X, y: 40 }, { x: X + 3, y: 40 }, { x: X + 7, y: 40 }], linePoint: { x: X, y: 39 } }] });
const dw = await t.ok("walls for dims", "create_walls", { walls: [{ begin: { x: X, y: 45 }, end: { x: X + 8, y: 45 }, height: 3 }, { begin: { x: X + 8, y: 45 }, end: { x: X + 8, y: 50 }, height: 3 }] });
await t.ok("dimension_walls", "dimension_walls", { walls: guids(dw), mode: "EachWall" });
await t.ok("create_level_dimensions", "create_level_dimensions", { levelDimensions: [{ position: { x: X + 10, y: 40 }, level: 0 }] });
const arc = await t.ok("arc for radial dim", "create_arcs", { arcs: [{ center: { x: X + 20, y: 40 }, radius: 3, beginAngle: 0, endAngle: 90 }] });
await t.ok("create_radial_dimensions", "create_radial_dimensions", { radialDimensions: [{ element: guids(arc)[0], at: { x: X + 22, y: 42 } }] });
await t.ok("create_angle_dimensions", "create_angle_dimensions", { angleDimensions: [{ line1: { begin: { x: X + 30, y: 40 }, end: { x: X + 34, y: 40 } }, line2: { begin: { x: X + 30, y: 40 }, end: { x: X + 33, y: 43 } } }] });

// complex elements
const mo = await t.ok("create_morphs box/extrusion/mesh", "create_morphs", { morphs: [
  { box: { origin: { x: X, y: 60, z: 0 }, size: { x: 2, y: 3, z: 1.5 } } },
  { extrusion: { polygon: [{ x: X + 5, y: 60 }, { x: X + 8, y: 60 }, { x: X + 6.5, y: 63 }], zBottom: 0, zTop: 2 } },
  { mesh: { vertices: [{ x: X + 10, y: 60, z: 0 }, { x: X + 12, y: 60, z: 0 }, { x: X + 11, y: 62, z: 0 }, { x: X + 11, y: 61, z: 2 }], faces: [[0, 2, 1], [0, 1, 3], [1, 2, 3], [2, 0, 3]] } },
] });
await t.ok("get_morph_geometry", "get_morph_geometry", { elements: guids(mo).slice(0, 1) });
const cw = await t.ok("create_curtain_walls", "create_curtain_walls", { curtainWalls: [{ path: [{ x: X, y: 70 }, { x: X + 6, y: 70 }, { x: X + 6, y: 74 }], height: 3, primarySpacing: 1.5 }] });
await t.ok("curtain wall details", "get_element_details", { elements: guids(cw) });
await t.ok("create_stairs straight", "create_stairs", { stairs: [{ baseline: [{ x: X + 20, y: 60 }, { x: X + 24, y: 60 }], height: 3, width: 1.2 }] });
await t.ok("create_railings", "create_railings", { railings: [{ path: [{ x: X + 20, y: 65 }, { x: X + 25, y: 65 }, { x: X + 25, y: 68 }], height: 1.1 }] });
await t.finish();
