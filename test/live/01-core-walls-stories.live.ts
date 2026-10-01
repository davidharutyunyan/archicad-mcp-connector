import { LiveTest, guids, near } from "./lib.js";

const t = await new LiveTest("core-walls-stories").start();

// --- system / generic
await t.ok("archicad_status", "archicad_status", {}, (j) => j.connected && j.connectorAddOn?.commandCount > 100 || `status: ${JSON.stringify(j).slice(0, 200)}`);
await t.ok("list_addon_commands", "list_addon_commands", {}, (j) => j.commands?.length > 100);
await t.ok("get_supported_element_types", "get_supported_element_types", {}, (j) => j.types?.some((x: { type: string; create: boolean }) => x.type === "Wall" && x.create));
await t.ok("get_connector_guide", "get_connector_guide", {}, (_j, r) => r.text.length > 500 || "guide too short");
await t.ok("execute_json_api_command", "execute_json_api_command", { command: "API.GetProductInfo" }, (j) => j.version === 26);
await t.ok("execute_addon_command", "execute_addon_command", { command: "Ping" }, (j) => j.ok === true);

// --- walls
const w = await t.ok("create_walls (straight, curved, trapezoid, slanted)", "create_walls", {
  walls: [
    { begin: { x: 0, y: 0 }, end: { x: 6, y: 0 }, height: 3, thickness: 0.3, elementId: "LT-W1" },
    { begin: { x: 6, y: 0 }, end: { x: 6, y: 5 }, height: 3, thickness: 0.3, arcAngle: 40 },
    { begin: { x: 6, y: 5 }, end: { x: 0, y: 5 }, height: 3, thickness: 0.3, endThickness: 0.5 },
    { begin: { x: 0, y: 5 }, end: { x: 0, y: 0 }, height: 3, thickness: 0.3, slantAlpha: 80 },
  ],
});
const [w1, w2, w3, w4] = guids(w);
const d = await t.ok("get_element_details walls", "get_element_details", { elements: [w1, w2, w3, w4] });
const e = d?.elements ?? [];
t.check("wall1 height 3 & unlinked", near(e[0]?.details?.height, 3) && e[0]?.details?.topLinkedStory === 0, JSON.stringify(e[0]?.details).slice(0, 300));
t.check("wall1 elementId", e[0]?.elementId === "LT-W1", e[0]?.elementId);
t.check("wall2 arcAngle 40", near(Math.abs(e[1]?.details?.arcAngle), 40), String(e[1]?.details?.arcAngle));
t.check("wall3 trapezoid", e[2]?.details?.wallType === "Trapezoid" && near(e[2]?.details?.endThickness, 0.5), JSON.stringify(e[2]?.details).slice(0, 200));
t.check("wall4 slanted", near(e[3]?.details?.slantAlpha, 80), String(e[3]?.details?.slantAlpha));
await t.ok("modify_elements wall height/refline/composite-by-index", "modify_elements", { elements: [{ guid: w1, height: 2.7, referenceLine: "Center" }] });
const d2 = await t.ok("read back modify", "get_element_details", { elements: [w1] });
t.check("modify applied", near(d2?.elements?.[0]?.details?.height, 2.7) && d2?.elements?.[0]?.details?.referenceLine === "Center", JSON.stringify(d2?.elements?.[0]?.details).slice(0, 200));
await t.fails("create_walls missing end rejected", "create_walls", { walls: [{ begin: { x: 0, y: 0 } }] });
await t.fails("bad layer name", "create_walls", { walls: [{ begin: { x: 0, y: 10 }, end: { x: 1, y: 10 }, layer: "No such layer XYZ" }] }, /not found/i);
await t.fails("modify unknown guid", "modify_elements", { elements: [{ guid: "11111111-2222-3333-4444-555555555555", height: 2 }] });

// --- stories
const s0 = await t.ok("get_stories", "get_stories", { includeElementCounts: true });
const n0 = s0?.stories?.length ?? 0;
console.log(`    stories: ${JSON.stringify((s0?.stories ?? []).map((s: { index: number; name: string; level: number; height: number }) => [s.index, s.name, s.level, s.height]))}`);
const cs = await t.ok("create_stories top", "create_stories", { stories: [{ name: "LT Roof", height: 3.2, position: "Top" }] });
const s1 = await t.ok("get_stories after create", "get_stories", {});
const top = (s1?.stories ?? []).find((s: { name: string }) => s.name === "LT Roof");
t.check("new story exists at top", Boolean(top) && s1.stories.length === n0 + 1, JSON.stringify(cs).slice(0, 300));
if (top) {
  await t.ok("modify_stories rename+height", "modify_stories", { stories: [{ story: { name: "LT Roof" }, name: "LT Roof 2", height: 2.9 }] });
  const s2 = await t.ok("get_stories after modify", "get_stories", {});
  const top2 = (s2?.stories ?? []).find((s: { name: string }) => s.name === "LT Roof 2");
  t.check("rename applied", Boolean(top2), JSON.stringify(s2?.stories?.map((s: { name: string }) => s.name)));
  await t.ok("set_current_story", "set_current_story", { story: { name: "LT Roof 2" } });
  await t.ok("set_current_story back to 0", "set_current_story", { story: 0 });
  await t.ok("delete_stories dryRun", "delete_stories", { stories: [{ name: "LT Roof 2" }], dryRun: true });
  await t.ok("delete_stories", "delete_stories", { stories: [{ name: "LT Roof 2" }] });
  const pi = await t.ok("current story preserved after delete", "get_project_info", {});
  t.check("current story still 0", pi?.currentStory?.index === 0, JSON.stringify(pi?.currentStory));
  const s3 = await t.ok("get_stories after delete", "get_stories", {});
  t.check("story deleted", (s3?.stories?.length ?? 0) === n0, String(s3?.stories?.length));
}
await t.fails("modify missing story", "modify_stories", { stories: [{ story: { name: "No such story" }, name: "x" }] });
await t.finish();
