import { LiveTest, guids, near } from "./lib.js";

const t = await new LiveTest("openings-objects-zones").start();
const X = 300;
await t.ok("story 0", "set_current_story", { story: 0 });

// library search
const wins = await t.ok("search windows", "search_library_parts", { type: "Window", limit: 5 }, (j) => (j.libraryParts ?? j.items ?? j.results)?.length > 0);
const doors = await t.ok("search doors", "search_library_parts", { type: "Door", limit: 5 });
const objs = await t.ok("search objects 'стул' (chair)", "search_library_parts", { query: "стул", type: "Object", limit: 5 });
const list = (j: any) => j?.libraryParts ?? j?.items ?? j?.results ?? [];
const winName = list(wins)[0]?.name, doorName = list(doors)[0]?.name, chairName = list(objs)[0]?.name;
console.log(`    window: ${winName} | door: ${doorName} | chair: ${chairName}`);
await t.ok("get_library_part_details", "get_library_part_details", { libraryParts: [chairName], includeParameters: true }, (j) => JSON.stringify(j).length > 200);
await t.ok("get_library_part_scripts", "get_library_part_scripts", { libraryPart: chairName, scripts: ["script2D"], maxLength: 500 });
await t.ok("get_libraries", "get_libraries", {}, (j) => (j.libraries?.length ?? 0) > 0);

// host wall + openings
const w = await t.ok("host walls", "create_walls", { walls: [{ begin: { x: X, y: 0 }, end: { x: X + 10, y: 0 }, height: 3, thickness: 0.3 }, { begin: { x: X + 10, y: 0 }, end: { x: X + 10, y: 8 }, height: 3, thickness: 0.3 }] });
const [w1, w2] = guids(w);
const win = await t.ok("create_windows (default part + named part)", "create_windows", { windows: [
  { wall: w1, position: 2, sillHeight: 0.9, width: 1.2, height: 1.4 },
  { wall: w1, position: 6, sillHeight: 0.8, width: 1.0, height: 1.2, libraryPart: winName },
] });
const dr = await t.ok("create_doors", "create_doors", { doors: [{ wall: w2, position: 4, width: 0.9, height: 2.1, libraryPart: doorName }] });
await t.fails("window outside wall rejected", "create_windows", { windows: [{ wall: w1, position: 50, width: 1, height: 1 }] });
const wd = await t.ok("wall lists openings", "get_element_details", { elements: [w1] });
t.check("wall1 has 2 windows", wd?.elements?.[0]?.details?.windows?.length === 2, JSON.stringify(wd?.elements?.[0]?.details?.windows));
const wg = guids(win);
const od = await t.ok("window details", "get_element_details", { elements: wg });
t.check("window width 1.2 sill 0.9", near(od?.elements?.[0]?.details?.width, 1.2) && near(od?.elements?.[0]?.details?.sillHeight, 0.9), JSON.stringify(od?.elements?.[0]?.details).slice(0, 400));
await t.ok("modify_openings width/position", "modify_openings", { openings: [{ guid: wg[0], width: 1.5, position: 3 }] }).catch(() => {});
await t.ok("modify window via modify_elements", "modify_elements", { elements: [{ guid: wg[1], sillHeight: 1.0 }] });
const gp = await t.ok("get_gdl_parameters window", "get_gdl_parameters", { elements: [wg[0]], search: "" }, (j) => JSON.stringify(j).length > 100);
const gp2 = await t.ok("get_gdl_parameters with names", "get_gdl_parameters", { elements: [wg[0]], names: ["A", "B"] });

// opening tool in slab & wall
const sl = await t.ok("slab for opening", "create_slabs", { slabs: [{ polygon: [{ x: X, y: 20 }, { x: X + 8, y: 20 }, { x: X + 8, y: 28 }, { x: X, y: 28 }], thickness: 0.25 }] });
await t.ok("create_openings in slab", "create_openings", { openings: [{ owner: guids(sl)[0], point: { x: X + 4, y: 24 }, shape: "Rectangular", width: 1, height: 1.5 }] });

// objects & lamps
const ob = await t.ok("create_objects", "create_objects", { objects: [{ libraryPart: chairName, position: { x: X + 2, y: 4 }, angle: 30 }] });
const og = guids(ob);
await t.ok("object details", "get_element_details", { elements: og }, (j) => j.elements?.[0]?.details?.libraryPart?.name === chairName || `lp: ${JSON.stringify(j.elements?.[0]?.details).slice(0, 300)}`);
const lampList = await t.ok("search lamps", "search_library_parts", { type: "Lamp", limit: 3 });
const lampName = list(lampList)[0]?.name;
if (lampName) await t.ok("create_lamps", "create_lamps", { lamps: [{ libraryPart: lampName, position: { x: X + 4, y: 4 }, elevation: 2.5, lightOn: true }] });
const pnames = await t.ok("object params", "get_gdl_parameters", { elements: og });
const firstLen = (pnames?.elements?.[0]?.parameters ?? pnames?.results?.[0]?.parameters ?? []).find((p: { type: string; hidden?: boolean }) => p.type === "Length" && !p.hidden);
if (firstLen) {
  await t.ok(`set_gdl_parameters ${firstLen.name}`, "set_gdl_parameters", { elements: [{ guid: og[0], params: { [firstLen.name]: 0.77 } }] });
  const back = await t.ok("read back param", "get_gdl_parameters", { elements: og, names: [firstLen.name] });
  t.check("param changed", JSON.stringify(back).includes("0.77"), JSON.stringify(back).slice(0, 300));
}
const other = list(objs)[1]?.name;
if (other) await t.ok("change_library_part", "change_library_part", { elements: [{ guid: og[0], libraryPart: other }] });

// custom GDL part
const lp = await t.ok("create_library_part (GDL box)", "create_library_part", {
  name: "LT Test Box", type: "Object",
  parameters: [{ name: "boxH", type: "Length", value: 0.8, description: "Box height" }],
  scripts: { script3D: "BLOCK A, B, boxH", script2D: "PROJECT2 3, 270, 2" },
});
await t.ok("place custom part", "create_objects", { objects: [{ libraryPart: "LT Test Box", position: { x: X + 6, y: 4 }, sizeA: 1, sizeB: 0.5 }] });

// zones
const zc = await t.ok("create_zones polygon + auto", "create_zones", { zones: [
  { polygon: [{ x: X + 20, y: 0 }, { x: X + 26, y: 0 }, { x: X + 26, y: 5 }, { x: X + 20, y: 5 }], name: "LT Room", number: "101", height: 2.8 },
] });
const zg = guids(zc);
const zd = await t.ok("zone details area", "get_element_details", { elements: zg });
const zdet = zd?.elements?.[0]?.details;
t.check("zone area ~30", zdet && JSON.stringify(zdet).match(/"(area|netArea|grossArea|calculatedArea)":\s*(29\.|30)/) !== null, JSON.stringify(zdet).slice(0, 500));
// auto zone inside walls: build closed box
await t.ok("box walls", "create_walls", { walls: [
  { begin: { x: X + 40, y: 0 }, end: { x: X + 46, y: 0 }, height: 3, thickness: 0.2 }, { begin: { x: X + 46, y: 0 }, end: { x: X + 46, y: 4 }, height: 3, thickness: 0.2 },
  { begin: { x: X + 46, y: 4 }, end: { x: X + 40, y: 4 }, height: 3, thickness: 0.2 }, { begin: { x: X + 40, y: 4 }, end: { x: X + 40, y: 0 }, height: 3, thickness: 0.2 },
] });
const az = await t.ok("auto zone by reference point", "create_zones", { zones: [{ referencePoint: { x: X + 43, y: 2 }, name: "LT Auto" }] });
await t.ok("update_zones dryRun", "update_zones", { zones: guids(az), dryRun: true });
await t.ok("modify_zones name", "modify_zones", { zones: [{ guid: zg[0], name: "LT Room 2", number: "102" }] });

// automatic zone on a story that is NOT current: Archicad detects the boundary from the current story's
// walls, so the add-on switches stories temporarily (regression: story 1 zone took story 0's walls)
await t.ok("story-1 box (clockwise, Outside) + partition", "create_walls", { walls: [
  { begin: { x: X + 50, y: 0 }, end: { x: X + 50, y: 4 }, height: 3, thickness: 0.2, referenceLine: "Outside", storyIndex: 1 },
  { begin: { x: X + 50, y: 4 }, end: { x: X + 56, y: 4 }, height: 3, thickness: 0.2, referenceLine: "Outside", storyIndex: 1 },
  { begin: { x: X + 56, y: 4 }, end: { x: X + 56, y: 0 }, height: 3, thickness: 0.2, referenceLine: "Outside", storyIndex: 1 },
  { begin: { x: X + 56, y: 0 }, end: { x: X + 50, y: 0 }, height: 3, thickness: 0.2, referenceLine: "Outside", storyIndex: 1 },
  { begin: { x: X + 53, y: 0.2 }, end: { x: X + 53, y: 3.8 }, height: 3, thickness: 0.1, referenceLine: "Center", storyIndex: 1 },
] });
await t.ok("story 0 current", "set_current_story", { story: 0 });
const uz = await t.ok("auto zone on story 1 while story 0 is current", "create_zones", { zones: [{ referencePoint: { x: X + 51.5, y: 2 }, storyIndex: 1, name: "LT Upper" }] });
// inner faces: x X+50.2 .. X+52.95 (partition face), y 0.2 .. 3.8 -> 2.75 x 3.6
await t.ok("upper zone bounded by story 1 walls", "get_zones", { zones: guids(uz) }, (j) => near(j.zones?.[0]?.area ?? 0, 9.9, 0.02) || `area ${j.zones?.[0]?.area}`);
await t.ok("current story restored", "get_stories", {}, (j) => j.currentIndex === 0 || `current ${j.currentIndex}`);
await t.ok("update_zones on non-current story", "update_zones", { zones: guids(uz), dryRun: true }, (j) => j.results?.[0]?.status === "upToDate" || JSON.stringify(j.results?.[0]).slice(0, 300));
await t.finish();
