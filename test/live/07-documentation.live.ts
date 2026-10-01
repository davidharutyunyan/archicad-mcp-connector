import { LiveTest, guids } from "./lib.js";
import { existsSync, rmSync, mkdirSync, statSync } from "node:fs";

const t = await new LiveTest("documentation").start();
const D = "/private/tmp/claude-connector-tests/docs";
rmSync(D, { recursive: true, force: true });
mkdirSync(D, { recursive: true });
const X = 600;
await t.ok("story 0 plan", "open_view", { window: "FloorPlan", story: 0 });
const w = await t.ok("model", "create_walls", { walls: [{ begin: { x: X, y: 0 }, end: { x: X + 6, y: 0 }, height: 3 }, { begin: { x: X + 6, y: 0 }, end: { x: X + 6, y: 4 }, height: 3 }] });

const lay = await t.ok("create_layout (official)", "create_layout", { name: "LT Layout" });
const layoutGuid = JSON.stringify(lay).match(/[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}/)?.[0];
console.log(`    layout: ${JSON.stringify(lay).slice(0, 200)}`);
const dbs = await t.ok("get_databases layouts", "get_databases", { types: ["Layout"] });
const layoutDb = (dbs?.databases ?? []).find((d: { name?: string }) => (d.name ?? "").includes("LT Layout"));
t.check("layout database listed", Boolean(layoutDb), JSON.stringify(dbs).slice(0, 400));
const views = await t.ok("list_views", "list_views", { include: ["viewMap", "stories"] });
const planView = (views?.viewMap ?? []).find((v: { windowType?: string }) => v.windowType === "FloorPlan");
const pd = await t.ok("place_drawing floor plan", "place_drawing", { drawings: [
  { layout: "LT Layout", view: planView?.guid, position: { x: 0.1, y: 0.1 }, scale: 100 },
  { layout: "LT Layout", database: "FloorPlan", storyIndex: 0, position: { x: 0.1, y: 0.2 }, scale: 200 },
] });
const drawingGuid = guids(pd)[0];
await t.ok("get_layout_drawings", "get_layout_drawings", { layouts: ["LT Layout"] }, (j) => JSON.stringify(j).includes(drawingGuid ?? "none") || JSON.stringify(j).slice(0, 300));
await t.ok("database+story places the story's own plan view", "get_layout_drawings", { layouts: ["LT Layout"] }, (j) => JSON.stringify(j).includes("1-й этаж") || JSON.stringify(j).slice(0, 400));
if (drawingGuid) await t.ok("modify_drawings scale", "modify_drawings", { drawings: [{ guid: drawingGuid, scale: 50, name: "LT Plan" }] });

// Anchored placement moves the content AND the frame (the drawing title follows the frame), and repositioning keeps an
// unclipped drawing unclipped (writing the frame polygon switches clipping on in Archicad).
const near = (a: unknown, b: number) => typeof a === "number" && Math.abs(a - b) < 1e-4;
const toc = (views?.viewMap ?? []).find((v: { itemType?: string }) => v.itemType === "TableOfContents");
if (toc) {
  const pt = await t.ok("place_drawing anchored (index)", "place_drawing", { drawings: [{ layout: "LT Layout", view: toc.guid, anchor: "LeftTop", position: { x: 0.03, y: 0.28 } }] },
    (j) => (near(j.results?.[0]?.drawing?.bounds?.xMin, 0.03) && near(j.results?.[0]?.drawing?.bounds?.yMax, 0.28)) || JSON.stringify(j.results?.[0]?.drawing?.bounds));
  const tg = guids(pt)[0];
  const clipped = pt?.results?.[0]?.drawing?.clipToFrame;
  if (tg) await t.ok("reposition keeps clipToFrame", "modify_drawings", { drawings: [{ guid: tg, anchor: "LeftTop", position: { x: 0.05, y: 0.27 } }] },
    (j) => (j.results?.[0]?.drawing?.clipToFrame === clipped && near(j.results?.[0]?.drawing?.bounds?.xMin, 0.05)) || JSON.stringify(j.results?.[0]?.drawing));
  if (tg) await t.ok("modify_elements reposition keeps clipToFrame", "modify_elements", { elements: [{ guid: tg, anchor: "LeftTop", position: { x: 0.04, y: 0.27 } }] });
  if (tg) await t.ok("…read back", "get_layout_drawings", { layouts: ["LT Layout"] }, (j) => {
    const d = (j.layouts?.[0]?.drawings ?? []).find((x: { guid?: string }) => x.guid === tg);
    return (d?.clipToFrame === clipped && near(d?.bounds?.xMin, 0.04)) || JSON.stringify(d);
  });
}

// A saved view's new scale reaches new drawings right away (set_view_settings opens the view once).
if (planView) {
  await t.ok("floor plan (snapshot)", "open_view", { window: "FloorPlan", story: 0 });
  await t.ok("floor plan at 1:100 (differs from the view's 1:250)", "set_view_settings", { drawingScale: 100 });
  const planBefore = await t.ok("floor plan settings", "get_view_settings", {});
  await t.ok("layout in front", "open_view", { window: "Layout", name: "LT Layout" });
  const before = await t.ok("get_view_settings plan view", "get_view_settings", { view: { guid: planView.guid } });
  const oldScale = before?.drawingScale?.value ?? 100;
  await t.ok("set_view_settings scale refreshes the view", "set_view_settings", { view: { guid: planView.guid }, drawingScale: 250 },
    (j) => typeof j.refreshed === "string" || JSON.stringify(j).slice(0, 300));
  await t.ok("place_drawing uses the new view scale", "place_drawing", { drawings: [{ layout: "LT Layout", view: planView.guid, position: { x: 0.3, y: 0.1 } }] },
    (j) => j.results?.[0]?.drawing?.viewScale === 250 || `viewScale ${j.results?.[0]?.drawing?.viewScale} of ${planView.name}: ${JSON.stringify(j.results?.[0]?.error ?? "")}`);
  await t.ok("restore view scale", "set_view_settings", { view: { guid: planView.guid }, drawingScale: oldScale });
  // ...and the refresh put the floor plan's own display state back (it has layer settings separate from the layout in front)
  await t.ok("floor plan", "open_view", { window: "FloorPlan", story: 0 });
  await t.ok("floor plan layers/scale unchanged by the refresh", "get_view_settings", {},
    (j) => (JSON.stringify(j.layerCombination) === JSON.stringify(planBefore?.layerCombination) && j.drawingScale === planBefore?.drawingScale) ||
      `before ${JSON.stringify(planBefore?.layerCombination)} 1:${planBefore?.drawingScale}, after ${JSON.stringify(j.layerCombination)} 1:${j.drawingScale}`);
}
await t.ok("update_drawings", "update_drawings", { all: true });
await t.ok("export_pdf layout", "export_pdf", { path: `${D}/layout.pdf`, layout: layoutDb?.guid ?? "LT Layout" }, () => existsSync(`${D}/layout.pdf`) && statSync(`${D}/layout.pdf`).size > 1000 || "pdf missing");
await t.ok("export_pdf floor plan", "export_pdf", { path: `${D}/plan.pdf`, storyIndex: 0 }, () => existsSync(`${D}/plan.pdf`) || "pdf missing");
await t.ok("get_ifc_translators", "get_ifc_translators", {});
await t.ok("export_ifc", "export_ifc", { path: `${D}/model.ifc` }, () => existsSync(`${D}/model.ifc`) && statSync(`${D}/model.ifc`).size > 10000 || "ifc missing");
await t.ok("export_ifc elements", "export_ifc", { path: `${D}/walls.ifc`, scope: "Elements", elements: guids(w) }, () => existsSync(`${D}/walls.ifc`) || "ifc missing");
await t.ok("export_dwg (dxf) floor plan", "export_dwg", { path: `${D}/plan.dxf`, format: "dxf", storyIndex: 0 }, () => existsSync(`${D}/plan.dxf`) || "dxf missing");
await t.ok("export_3d_model obj", "export_3d_model", { path: `${D}/walls.obj`, format: "obj", elements: guids(w) }, () => existsSync(`${D}/walls.obj`) || "obj missing");
await t.ok("export_3d_model stl", "export_3d_model", { path: `${D}/walls.stl`, format: "stl", elements: guids(w) }, () => existsSync(`${D}/walls.stl`) || "stl missing");
await t.ok("export_module", "export_module", { path: `${D}/walls.mod`, elements: guids(w) }, () => existsSync(`${D}/walls.mod`) || "mod missing");
await t.ok("merge_file module", "merge_file", { path: `${D}/walls.mod`, position: { x: 20, y: 0 } });
const ph = await t.ok("place_hotlink", "place_hotlink", { hotlinks: [{ source: `${D}/walls.mod`, position: { x: X, y: 20 } }] });
const hl = await t.ok("get_hotlinks", "get_hotlinks", { includeInstances: true }, (j) => JSON.stringify(j).includes("walls") || JSON.stringify(j).slice(0, 300));
await t.ok("update_hotlinks", "update_hotlinks", { all: true });
const sets = await t.ok("get_publisher_sets", "get_publisher_sets", {});
const setName = sets?.publisherSets?.[1] ?? sets?.publisherSets?.[0];
console.log(`    publisher sets: ${JSON.stringify(sets).slice(0, 200)}`);
if (setName) await t.ok("publish_publisher_set", "publish_publisher_set", { name: setName, outputPath: `${D}/published`, createFolders: true });
if (drawingGuid) await t.ok("delete_drawings", "delete_drawings", { drawings: [drawingGuid] });
await t.finish();
