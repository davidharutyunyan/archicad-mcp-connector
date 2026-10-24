import { LiveTest, guids, near } from "./lib.js";

// The Opening tool (API_OpeningID): Archicad 26-28 go through API_OpeningType, Archicad 29+ through the C++ Opening API;
// both must keep the same JSON contract.
const t = await new LiveTest("opening-tool").start();
const X = 1000;
await t.ok("story 0", "set_current_story", { story: 0 });

const w = await t.ok("host wall", "create_walls", { walls: [{ begin: { x: X, y: 0 }, end: { x: X + 8, y: 0 }, height: 3, thickness: 0.3 }] });
const wall = guids(w)[0];
const o = await t.ok("wall openings by position", "create_openings", { openings: [
  { owner: wall, position: 2, width: 1, height: 1.2, bottomElevation: 0.9 },
  { owner: wall, position: 5, shape: "Circular", width: 0.6, anchorAltitude: 1.5 },
] });
const [o1, o2] = guids(o);

// eslint-disable-next-line @typescript-eslint/no-explicit-any
const details = async (label: string, guid: string | undefined): Promise<any> =>
  (await t.ok(label, "get_element_details", { elements: [guid] }))?.elements?.[0]?.details ?? {};

const d1 = await details("rect opening details", o1);
t.check("rect opening owner", d1.owner === wall && d1.ownerType === "Wall", JSON.stringify(d1).slice(0, 300));
t.check("rect opening size", near(d1.width, 1) && near(d1.height, 1.2) && d1.shape === "Rectangular", JSON.stringify(d1).slice(0, 300));
t.check("rect opening position", near(d1.position ?? -1, 2, 0.01), `position ${d1.position}`);
t.check("rect opening bottom elevation", near(d1.bottomElevation ?? -1, 0.9, 0.01) && d1.constraint === "Horizontal", JSON.stringify(d1).slice(0, 400));
const d2 = await details("circular opening details", o2);
t.check("circular opening", d2.shape === "Circular" && near(d2.width, 0.6) && near(d2.anchorAltitude ?? -1, 1.5, 0.01), JSON.stringify(d2).slice(0, 400));

await t.ok("modify size, elevation, depth", "modify_openings", { openings: [{ guid: o1, width: 1.4, bottomElevation: 1.0, limit: "Finite", depth: 0.2 }] });
const m1 = await details("modified details", o1);
t.check("modified size and elevation", near(m1.width, 1.4) && near(m1.bottomElevation ?? -1, 1.0, 0.01), JSON.stringify(m1).slice(0, 400));
t.check("modified limit", m1.limit === "Finite" && near(m1.depth ?? -1, 0.2), JSON.stringify(m1).slice(0, 400));
await t.ok("move along the wall", "modify_openings", { openings: [{ guid: o1, position: 3.5 }] });
const p1 = await details("moved details", o1);
t.check("moved position", near(p1.position ?? -1, 3.5, 0.01) && near(p1.width, 1.4), `position ${p1.position}, width ${p1.width}`);

// rotated opening in a slab
const sl = await t.ok("slab", "create_slabs", { slabs: [{ polygon: [{ x: X, y: 10 }, { x: X + 8, y: 10 }, { x: X + 8, y: 18 }, { x: X, y: 18 }], thickness: 0.25 }] });
const so = await t.ok("rotated slab opening", "create_openings", { openings: [{ owner: guids(sl)[0], point: { x: X + 4, y: 14 }, width: 2, height: 1, rotation: 30 }] });
const angleOf = (d: { frame?: { xAxis?: { x: number; y: number } } }) => (Math.atan2(d.frame?.xAxis?.y ?? 0, d.frame?.xAxis?.x ?? 1) * 180) / Math.PI;
const s1 = await details("slab opening details", guids(so)[0]);
t.check("slab opening vertical and rotated 30", s1.constraint === "Vertical" && near(Math.abs(angleOf(s1)) % 180, 30, 0.5) && near(s1.point?.x ?? 0, X + 4, 0.01), JSON.stringify(s1).slice(0, 500));
// rotation is absolute (from the default x axis of the extrusion direction), not added to the current one
await t.ok("set slab opening rotation", "modify_openings", { openings: [{ guid: guids(so)[0], rotation: 15 }] });
const s2 = await details("rotated details", guids(so)[0]);
t.check("slab opening rotation 15", near(Math.abs(angleOf(s2)) % 180, 15, 0.5) && near(s2.point?.x ?? 0, X + 4, 0.01), JSON.stringify(s2?.frame));

// another host: Archicad 29's Opening API can re-host an opening, the older API cannot
const w2 = await t.ok("second wall", "create_walls", { walls: [{ begin: { x: X, y: 4 }, end: { x: X + 8, y: 4 }, height: 3, thickness: 0.3 }] });
const wall2 = guids(w2)[0];
if (t.version >= 29) {
  await t.ok("move opening to another wall", "modify_openings", { openings: [{ guid: o2, owner: wall2, point: { x: X + 5, y: 4 } }] });
  const r2 = await details("re-hosted details", o2);
  t.check("re-hosted", r2.owner === wall2 && near(r2.position ?? -1, 5, 0.01), JSON.stringify(r2).slice(0, 300));
} else {
  await t.fails("re-hosting is not supported", "modify_openings", { openings: [{ guid: o2, owner: wall2 }] }, /another host/);
}

// further cut elements (Archicad 29+): a lining wall behind the host wall
const w3 = await t.ok("lining wall", "create_walls", { walls: [{ begin: { x: X, y: 0.5 }, end: { x: X + 8, y: 0.5 }, height: 3, thickness: 0.1 }] });
const lining = guids(w3)[0];
if (t.version >= 29) {
  await t.ok("cut the lining too", "modify_openings", { openings: [{ guid: o1, addCutElements: [lining] }] });
  const c1 = await details("cut elements", o1);
  t.check("lining is cut", (c1.cutElements ?? []).includes(lining), JSON.stringify(c1.cutElements));
  await t.ok("stop cutting the lining", "modify_openings", { openings: [{ guid: o1, removeCutElements: [lining] }] });
  const c2 = await details("cut elements after removal", o1);
  t.check("lining no longer cut", !(c2.cutElements ?? []).includes(lining), JSON.stringify(c2.cutElements));
} else {
  await t.fails("cut elements need Archicad 29", "modify_openings", { openings: [{ guid: o1, addCutElements: [lining] }] }, /Archicad 29/);
}

await t.fails("opening without owner", "create_openings", { openings: [{ point: { x: X, y: 0 }, width: 1, height: 1 }] });
await t.ok("cleanup", "delete_elements", { elements: [wall, wall2, lining, guids(sl)[0]].filter(Boolean) });
await t.finish();
