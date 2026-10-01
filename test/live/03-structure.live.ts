import { LiveTest, guids, near } from "./lib.js";

const t = await new LiveTest("structure").start();
const X = 200;
const rect = (x: number, y: number, w: number, h: number) => [{ x, y }, { x: x + w, y }, { x: x + w, y: y + h }, { x, y: y + h }];

// slabs
const s = await t.ok("create_slabs (plain, hole+arc, composite-free)", "create_slabs", { slabs: [
  { polygon: rect(X, 0, 10, 8), thickness: 0.25, level: 0 },
  { polygon: { points: rect(X + 12, 0, 6, 6), arcs: [{ index: 1, angle: 60 }], holes: [{ points: rect(X + 14, 2, 1, 1) }] }, thickness: 0.2 },
] });
const [s1, s2] = guids(s);
await t.fails("slab self-intersecting rejected or regularized", "create_slabs", { slabs: [{ polygon: [{ x: X, y: 20 }, { x: X + 2, y: 22 }, { x: X + 2, y: 20 }, { x: X, y: 22 }, { x: X, y: 20.001 }] }] }).catch(() => {});
await t.ok("modify_slabs thickness+polygon", "modify_slabs", { slabs: [{ guid: s1, thickness: 0.3, polygon: rect(X, 0, 11, 8) }] });
const sd = await t.ok("slab details", "get_element_details", { elements: [s1, s2] });
t.check("slab1 modified", near(sd?.elements?.[0]?.details?.thickness, 0.3) && sd?.elements?.[0]?.details?.polygon?.points?.length === 4 && near(Math.max(...sd.elements[0].details.polygon.points.map((p: { x: number }) => p.x)), X + 11), JSON.stringify(sd?.elements?.[0]?.details?.polygon).slice(0, 200));
t.check("slab2 has hole+arc", sd?.elements?.[1]?.details?.polygon?.holes?.length === 1 && sd?.elements?.[1]?.details?.polygon?.arcs?.length === 1, JSON.stringify(sd?.elements?.[1]?.details?.polygon).slice(0, 300));

// roofs
const r = await t.ok("create_roofs single + multi plane", "create_roofs", { roofs: [
  { roofClass: "SinglePlane", polygon: rect(X, 30, 8, 6), pivotLine: { begin: { x: X, y: 30 }, end: { x: X + 8, y: 30 } }, slopeAngle: 25, level: 3, thickness: 0.2 },
  { roofClass: "MultiPlane", pivotPolygon: rect(X + 20, 30, 10, 8), slopeAngle: 30, level: 3, eavesOverhang: 0.5, thickness: 0.25 },
] });
const [r1, r2] = guids(r);
await t.ok("modify_roofs slope", "modify_roofs", { roofs: [{ guid: r1, slopeAngle: 35 }] });
const rd = await t.ok("roof details", "get_element_details", { elements: [r1, r2] });
t.check("roof1 slope 35", near(rd?.elements?.[0]?.details?.slopeAngle ?? rd?.elements?.[0]?.details?.angle, 35), JSON.stringify(rd?.elements?.[0]?.details).slice(0, 300));
t.check("roof2 multiplane", /multi/i.test(JSON.stringify(rd?.elements?.[1]?.details?.roofClass ?? "")), JSON.stringify(rd?.elements?.[1]?.details).slice(0, 200));

// shells
const sh = await t.ok("create_shells extruded + revolved", "create_shells", { shells: [
  { shellClass: "Extruded", profile: [{ x: 0, y: 0 }, { x: 2, y: 2 }, { x: 4, y: 0 }], begin: { x: X, y: 50 }, extrusion: { x: 0, y: 6 }, thickness: 0.15 },
  { shellClass: "Revolved", profile: [{ x: 3, y: 0 }, { x: 3, y: 2 }, { x: 0, y: 4 }], axisOrigin: { x: X + 20, y: 55 }, revolutionAngle: 360, thickness: 0.15 },
] });
// meshes
const m = await t.ok("create_meshes with z", "create_meshes", { meshes: [{ polygon: [{ x: X + 40, y: 0, z: 0 }, { x: X + 50, y: 0, z: 1 }, { x: X + 50, y: 10, z: 2 }, { x: X + 40, y: 10, z: 0.5 }], level: 0 }] });
const md = await t.ok("mesh details", "get_element_details", { elements: guids(m) });
t.check("mesh z preserved", md?.elements?.[0]?.details?.polygon?.points?.some((p: { z?: number }) => near(p.z ?? -1, 2)), JSON.stringify(md?.elements?.[0]?.details?.polygon).slice(0, 300));

// columns & beams
const c = await t.ok("create_columns rect/circular/slanted", "create_columns", { columns: [
  { origin: { x: X, y: 70 }, height: 3, shape: "Rectangular", width: 0.3, depth: 0.5, rotationAngle: 15 },
  { origin: { x: X + 3, y: 70 }, height: 3, shape: "Circular", diameter: 0.4 },
  { origin: { x: X + 6, y: 70 }, height: 3, slanted: true, slantAngle: 75, width: 0.3, depth: 0.3 },
] });
const cg = guids(c);
const cd = await t.ok("column details", "get_element_details", { elements: cg });
const c0 = cd?.elements?.[0]?.details;
t.check("column1 height/size", near(c0?.height, 3) && JSON.stringify(c0).includes("0.5"), JSON.stringify(c0).slice(0, 400));
await t.ok("modify_columns height", "modify_columns", { columns: [{ guid: cg[0], height: 3.5 }] });
const b = await t.ok("create_beams straight+curved", "create_beams", { beams: [
  { begin: { x: X, y: 75 }, end: { x: X + 6, y: 75 }, level: 3, width: 0.25, height: 0.5 },
  { begin: { x: X, y: 78 }, end: { x: X + 6, y: 78 }, level: 3, arcAngle: 45, width: 0.2, height: 0.4 },
] });
const bg = guids(b);
await t.ok("modify_beams height", "modify_beams", { beams: [{ guid: bg[0], height: 0.6, holes: [{ distanceFromBegin: 2, shape: "Circular", diameter: 0.1 }] }] }).catch(() => {});
const bd = await t.ok("beam details", "get_element_details", { elements: bg });
t.check("beam1 height 0.6", JSON.stringify(bd?.elements?.[0]?.details ?? {}).includes("0.6"), JSON.stringify(bd?.elements?.[0]?.details).slice(0, 400));
await t.ok("cleanup", "execute_addon_command", { command: "DeleteElements", parameters: { elements: [s1, s2, r1, r2, ...guids(sh), ...guids(m), ...cg, ...bg].filter(Boolean) } });
await t.finish();
