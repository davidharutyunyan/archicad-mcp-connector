import { LiveTest, guids } from "./lib.js";
import { existsSync, rmSync, mkdirSync } from "node:fs";

const t = await new LiveTest("project").start();
const D = "/private/tmp/claude-connector-tests/project";
rmSync(D, { recursive: true, force: true });
mkdirSync(D, { recursive: true });

await t.ok("get_project_info", "get_project_info", { includeTemplates: true }, (j) => j.projectOpen && j.currentStory !== undefined);
const f = await t.ok("get_project_info_fields", "get_project_info_fields", { category: "Fixed" });
const field = (f?.fields ?? f?.Fixed ?? []).find?.((x: { key: string; value?: string }) => x.key) ?? null;
console.log(`    first field: ${JSON.stringify(field).slice(0, 150)}`);
await t.ok("set_project_info_fields custom create", "set_project_info_fields", { fields: [{ name: "LT Field", value: "42" }], createIfMissing: true });
await t.ok("read custom field", "get_project_info_fields", { category: "Custom", search: "LT Field" }, (j) => JSON.stringify(j).includes("42"));
await t.ok("delete_project_info_fields", "delete_project_info_fields", { fields: ["LT Field"] });
if (field) await t.ok("set fixed field", "set_project_info_fields", { fields: [{ key: field.key, value: "Claude Live Test" }] });
const prefs = await t.ok("get_preferences all", "get_preferences", {});
const dec = prefs?.workingUnits?.lengthDecimals;
await t.ok("set_preferences working units decimals", "set_preferences", { workingUnits: { lengthDecimals: dec === 3 ? 2 : 3 } });
await t.ok("restore decimals", "set_preferences", { workingUnits: { lengthDecimals: dec ?? 3 } });
const geo = await t.ok("get_geo_location", "get_geo_location", {});
await t.ok("set_geo_location Yerevan", "set_geo_location", { latitude: 40.1792, longitude: 44.4991, altitude: 990 });
await t.ok("geo read back", "get_geo_location", {}, (j) => Math.abs((j.latitude ?? j.location?.latitude ?? 0) - 40.1792) < 1e-3 || JSON.stringify(j).slice(0, 300));
await t.ok("rebuild_model", "rebuild_model", { mode: "Rebuild" });

// undo / redo
await t.ok("plan", "open_view", { window: "FloorPlan", story: 0 });
const w = await t.ok("wall to undo", "create_walls", { walls: [{ begin: { x: 900, y: 0 }, end: { x: 905, y: 0 }, height: 3 }] });
const wg = guids(w)[0];
await t.ok("undo", "undo", {});
await new Promise((r) => setTimeout(r, 1500));
const ex = await t.raw("get_element_details", { elements: [wg] });
t.check("wall gone after undo", JSON.stringify(ex.json ?? ex.text).match(/not found|error/i) !== null, ex.text.slice(0, 200));
await t.ok("redo", "redo", {});
await new Promise((r) => setTimeout(r, 1500));
await t.ok("wall back after redo", "get_element_details", { elements: [wg] });
// multi-step undo reports each step's own title (regression: all steps showed the first title)
for (const tag of ["A", "B", "C"])
  await t.ok(`step ${tag}`, "create_elements", { undoName: `LT step ${tag} (Claude)`, elements: [{ type: "Line", begin: { x: 900, y: 2 }, end: { x: 901, y: 2 } }] });
await t.ok("undo 3 steps: distinct titles", "undo", { steps: 3 }, (j) => {
  const titles: string[] = j.undone ?? [];
  return (j.performed === 3 && ["C", "B", "A"].every((tag, i) => titles[i]?.includes(`LT step ${tag}`))) || JSON.stringify(j).slice(0, 300);
});

// save / open / new / close
await t.ok("save_project_as pln", "save_project_as", { path: `${D}/lt-test.pln`, overwrite: true }, () => existsSync(`${D}/lt-test.pln`) || "pln missing");
await t.ok("save_project", "save_project", {});
await t.ok("save_project_as pla", "save_project_as", { path: `${D}/lt-test.pla`, format: "pla", overwrite: true }, () => existsSync(`${D}/lt-test.pla`) || "pla missing");
await t.ok("new_project (discard)", "new_project", { discardChanges: true });
await t.ok("new project is untitled/empty", "find_elements", { types: ["Wall"], limit: 1 }, (j) => j.total === 0 || `walls: ${j.total}`);
await t.ok("open_project saved pln", "open_project", { path: `${D}/lt-test.pln`, discardChanges: true });
await t.ok("opened project has the wall", "get_element_details", { elements: [wg] });
await t.ok("get_project_info after open", "get_project_info", {}, (j) => JSON.stringify(j).includes("lt-test"));
await t.finish();
