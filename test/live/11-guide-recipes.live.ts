// Runs the recipes of docs/CLAUDE_GUIDE.md exactly as documented (regression for the guide itself).
import { LiveTest, guids, near } from "./lib.js";

const t = await new LiveTest("guide-recipes").start();
await t.ok("plan story 0", "open_view", { window: "FloorPlan", story: 0 });
const O = { x: 1000, y: 0 };
const P = (x: number, y: number) => ({ x: O.x + x, y: O.y + y });

// 4.1 discover
await t.ok("composites lookup", "get_attributes", { type: "Composite", limit: 20 });
const doors = await t.ok("doors lookup", "search_library_parts", { type: "Door", limit: 10 });
await t.ok("windows lookup", "search_library_parts", { type: "Window", limit: 10 });
// 4.1 walls (clockwise, Outside -> bodies inside the outline)
const w = await t.ok("outer + inner walls", "create_walls", { walls: [
  { begin: P(0, 0), end: P(0, 8), height: 3, thickness: 0.3, referenceLine: "Outside" },
  { begin: P(0, 8), end: P(10, 8), height: 3, thickness: 0.3, referenceLine: "Outside" },
  { begin: P(10, 8), end: P(10, 0), height: 3, thickness: 0.3, referenceLine: "Outside" },
  { begin: P(10, 0), end: P(0, 0), height: 3, thickness: 0.3, referenceLine: "Outside" },
  { begin: P(6, 0.3), end: P(6, 7.7), height: 3, thickness: 0.12, referenceLine: "Center" },
] });
const [west, north, east, south, inner] = guids(w);
const bb = await t.ok("walls inside outline", "get_bounding_boxes", { elements: [south], kind: "3D" });
t.check("south wall body at y 0..0.3", JSON.stringify(bb).includes('"yMax":0.3'), JSON.stringify(bb).slice(0, 200));
const outline = [P(0, 0), P(10, 0), P(10, 8), P(0, 8)];
await t.ok("floor slab", "create_slabs", { slabs: [{ polygon: outline, thickness: 0.25, level: 0 }] });
await t.ok("multi-plane roof", "create_roofs", { roofs: [{ pivotPolygon: outline, level: 3, slopeAngle: 30, eavesOverhang: 0.5, thickness: 0.25 }] });
await t.ok("door", "create_doors", { doors: [{ wall: south, position: 8, width: 0.9, height: 2.1 }] });
await t.ok("windows", "create_windows", { windows: [
  { wall: south, position: 2.5, width: 1.5, height: 1.4, sillHeight: 0.9 },
  { wall: north, position: 5, width: 1.2, height: 1.4, sillHeight: 0.9 },
] });
const z = await t.ok("zones", "create_zones", { zones: [
  { referencePoint: P(3, 4), name: "Living", number: "01" },
  { referencePoint: P(8, 4), name: "Bedroom", number: "02" },
] });
const zd = await t.ok("zone areas", "get_element_details", { elements: guids(z) });
const areas = (zd?.elements ?? []).map((e: any) => e.details?.quantities?.area);
console.log(`    zone areas: ${JSON.stringify(areas)}`);
t.check("living ~ 5.64*7.4", areas[0] > 38 && areas[0] < 45, String(areas[0]));
const c1 = await t.raw("capture_view", { threeD: { mode: "axonometric", azimuth: 225, altitude: 35 }, format: "jpeg", maxSize: 1200 });
t.check("3D capture", !c1.isError && c1.images.length === 1, c1.text.slice(0, 200));
const c2 = await t.raw("capture_view", { view: { window: "FloorPlan", story: 0 }, zoom: { mode: "elements", elements: guids(w) }, maxSize: 1200 });
t.check("plan capture", !c2.isError && c2.images.length === 1, c2.text.slice(0, 200));
console.log(`    images: ${c1.images[0]} ${c2.images[0]}`);
// 4.5 dimensions + labels
await t.ok("dimension_walls", "dimension_walls", { walls: [south, east], mode: "Chain", includeOpenings: true });
await t.ok("labels", "create_labels", { labels: [{ parent: inner, text: "Partition" }] });
await t.finish();
