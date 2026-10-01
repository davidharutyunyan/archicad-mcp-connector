import { LiveTest, guids } from "./lib.js";
import { existsSync } from "node:fs";

const t = await new LiveTest("official-collab").start();
const X = 800;
await t.ok("plan", "open_view", { window: "FloorPlan", story: 0 });
const w = await t.ok("model", "create_walls", { walls: [
  { begin: { x: X, y: 0 }, end: { x: X + 5, y: 0 }, height: 3 }, { begin: { x: X + 5, y: 0 }, end: { x: X + 5, y: 4 }, height: 3 },
  { begin: { x: X + 5, y: 4 }, end: { x: X, y: 4 }, height: 3 }, { begin: { x: X, y: 4 }, end: { x: X, y: 0 }, height: 3 },
] });
const wg = guids(w);
const z = await t.ok("zone", "create_zones", { zones: [{ referencePoint: { x: X + 2.5, y: 2 }, name: "LT OffZone" }] });

// official wrappers
await t.ok("list_elements", "list_elements", { types: ["Wall"], limit: 5 }, (j) => (j.elements?.length ?? 0) > 0);
await t.ok("get_element_types", "get_element_types", { elements: wg.slice(0, 2) }, (j) => JSON.stringify(j).includes("Wall"));
await t.ok("get_bounding_boxes both", "get_bounding_boxes", { elements: wg.slice(0, 1), kind: "both" }, (j) => JSON.stringify(j).includes("zMax"));
await t.ok("get_elements_related_to_zones", "get_elements_related_to_zones", { zones: guids(z) }, (j) => JSON.stringify(j).includes(wg[0]!) || JSON.stringify(j).slice(0, 300));
await t.ok("get_element_components", "get_element_components", { elements: wg.slice(0, 1) });
await t.ok("get_property_ids_by_name", "get_property_ids_by_name", { search: "ID", limit: 5 });
const cls = await t.ok("get_classification_systems", "get_classification_systems", {});
const sysName = cls?.classificationSystems?.[0]?.name ?? cls?.systems?.[0]?.name;
const tree = await t.ok("get_classification_tree flat", "get_classification_tree", { system: sysName, format: "flat", limit: 10 });
const firstItem = (tree?.items ?? [])[0]?.guid;
if (firstItem) {
  await t.ok("get_classification_item_details", "get_classification_item_details", { items: [firstItem] });
  await t.ok("get_classification_availability", "get_classification_availability", { items: [firstItem], limit: 5 });
  await t.ok("get_elements_by_classification", "get_elements_by_classification", { item: firstItem });
}
const nav = await t.ok("get_navigator_tree ViewMap", "get_navigator_tree", { tree: "ViewMap", maxDepth: 2 });
const folder = await t.ok("create_view_map_folder", "create_view_map_folder", { name: "LT Folder" });
const fg = JSON.stringify(folder).match(/[0-9A-F]{8}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12}/)?.[0];
if (fg) {
  await t.ok("rename_navigator_item", "rename_navigator_item", { item: fg, newName: "LT Folder 2" });
  await t.ok("get_navigator_items", "get_navigator_items", { ids: [fg] });
  await t.ok("delete_navigator_items", "delete_navigator_items", { items: [fg] });
}
const pm = await t.ok("get_navigator_tree ProjectMap", "get_navigator_tree", { tree: "ProjectMap", maxDepth: 3 });
await t.ok("create_layout_subset", "create_layout_subset", { name: "LT Subset" });
const lay = await t.ok("create_layout in subset", "create_layout", { name: "LT Sheet" });
const lid = lay?.layoutId;
if (lid) {
  await t.ok("get_layout_settings", "get_layout_settings", { layouts: [lid] });
  await t.ok("set_layout_settings", "set_layout_settings", { layouts: [{ layout: lid, horizontalSize: 420, verticalSize: 297 }] });
}
await t.ok("get_attribute_folders", "get_attribute_folders", { attributeType: "Surface", depth: 1 });
await t.ok("create_attribute_folders", "create_attribute_folders", { attributeType: "Surface", folders: ["LT Folder"] });
await t.ok("rename_attribute_folder", "rename_attribute_folder", { attributeType: "Surface", folder: "LT Folder", newName: "LT Folder R" });
await t.ok("delete_attribute_folders", "delete_attribute_folders", { attributeType: "Surface", folders: ["LT Folder R"] });
const prev = await t.raw("get_profile_preview", { profiles: [1], width: 200, height: 200 });
t.check("get_profile_preview image", !prev.isError && prev.images.length >= 1, prev.text.slice(0, 200));
await t.ok("get_active_pen_tables", "get_active_pen_tables", {});

// collaboration
await t.ok("get_teamwork_status (solo project)", "get_teamwork_status", {});
const iss = await t.ok("create_issue", "create_issue", { issues: [{ name: "LT Issue", comment: "Check wall", attach: { highlight: wg.slice(0, 1) } }] });
await t.ok("get_issues", "get_issues", { search: "LT Issue", includeComments: true, includeElements: true }, (j) => JSON.stringify(j).includes("LT Issue"));
await t.ok("add_issue_comment", "add_issue_comment", { comments: [{ issue: "LT Issue", text: "Second comment", status: "Warning" }] });
await t.ok("get_issue_comments", "get_issue_comments", { issues: ["LT Issue"] }).catch(() => {});
await t.ok("attach_elements_to_issue", "attach_elements_to_issue", { issue: "LT Issue", elements: wg.slice(1, 2), type: "Modification" });
await t.ok("get_issue_elements", "get_issue_elements", { issues: ["LT Issue"] }, (j) => JSON.stringify(j).includes(wg[1]!));
await t.ok("detach_elements_from_issue", "detach_elements_from_issue", { issue: "LT Issue", elements: wg.slice(1, 2) }).catch(() => {});
await t.ok("export_bcf", "export_bcf", { path: "/private/tmp/claude-connector-tests/issues.bcfzip", overwrite: true }, () => existsSync("/private/tmp/claude-connector-tests/issues.bcfzip") || "bcf missing");
await t.ok("delete_issue", "delete_issue", { issues: ["LT Issue"] });
await t.ok("import_bcf", "import_bcf", { path: "/private/tmp/claude-connector-tests/issues.bcfzip" });
await t.ok("get_favorites", "get_favorites", {});
await t.ok("create_favorite from element", "create_favorite", { favorites: [{ name: "LT Fav Wall", element: wg[0] }] });
await t.ok("apply_favorite to defaults", "apply_favorite", { name: "LT Fav Wall", target: "Defaults" });
await t.ok("rename_favorite", "rename_favorite", { name: "LT Fav Wall", newName: "LT Fav Wall 2" });
await t.ok("export_favorites", "export_favorites", { path: "/private/tmp/claude-connector-tests/favs.prf", names: ["LT Fav Wall 2"], overwrite: true });
await t.ok("delete_favorite", "delete_favorite", { names: ["LT Fav Wall 2"] });
const td = await t.ok("get_tool_defaults Wall", "get_tool_defaults", { type: "Wall" }, (j) => JSON.stringify(j).includes("thickness"));
await t.ok("set_tool_defaults Wall thickness", "set_tool_defaults", { defaults: [{ type: "Wall", fields: { thickness: 0.37, height: 3.1 } }] });
const td2 = await t.ok("read defaults back", "get_tool_defaults", { type: "Wall" });
t.check("default thickness 0.37", JSON.stringify(td2).includes("0.37"), JSON.stringify(td2).slice(0, 300));
const nw = await t.ok("wall uses new defaults", "create_walls", { walls: [{ begin: { x: X, y: 10 }, end: { x: X + 3, y: 10 } }] });
const nd = await t.ok("new wall details", "get_element_details", { elements: guids(nw) });
t.check("new wall thickness 0.37", Math.abs((nd?.elements?.[0]?.details?.thickness ?? 0) - 0.37) < 1e-6, JSON.stringify(nd?.elements?.[0]?.details?.thickness));
await t.ok("restore wall default thickness", "set_tool_defaults", { defaults: [{ type: "Wall", fields: { thickness: 0.3, height: 3 } }] });
await t.ok("get_revisions", "get_revisions", {});
await t.finish();
