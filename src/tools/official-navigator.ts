/**
 * Official JSON API wrappers for the Navigator: trees (Project Map, View Map, Layout Book, publisher sets),
 * item details, rename / move / delete, cloning Project Map items into the View Map, view-map folders,
 * layouts, layout subsets and layout settings, publisher sets.
 */

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import type { ArchicadClient } from "../archicad/client.js";
import { CREATES, defineTool, DESTRUCTIVE, MODIFIES, READ_ONLY, type ToolContext } from "./define.js";
import {
  execResults,
  findNavItemIn,
  findNavRaw,
  flattenNav,
  getNavigatorRoot,
  gid,
  innerGuid,
  isGuid,
  lc,
  navIdItems,
  NavId,
  navNode,
  NavigatorItemType,
  normGuid,
  unwrap,
  type Json,
  type NavFlat,
  type TreeKind,
} from "./official-helpers.js";

const TreeKindSchema = z
  .enum(["ProjectMap", "ViewMap", "LayoutBook", "MyViewMap", "PublisherSet"])
  .describe(
    "Navigator tree: 'ProjectMap' (stories, sections, elevations, details, worksheets, 3D, schedules, indexes, lists), 'ViewMap' (saved views), " +
      "'LayoutBook' (subsets, layouts, drawings, master layouts), 'MyViewMap' (Teamwork only), 'PublisherSet' (needs publisherSet)",
  );

/**
 * Layout settings. IMPORTANT (verified live): paper size and margins are stored in the MASTER layout — setting them on a
 * layout (or passing them to API.CreateLayout) changes the master and therefore every layout that uses it.
 */
const MasterGeometryFields = {
  horizontalSize: z.number().positive().max(100000).optional().describe("Paper width in MILLIMETERS (e.g. A3 landscape = 420)"),
  verticalSize: z.number().positive().max(100000).optional().describe("Paper height in MILLIMETERS (e.g. A3 landscape = 297)"),
  leftMargin: z.number().min(0).optional().describe("Left margin, mm"),
  topMargin: z.number().min(0).optional().describe("Top margin, mm"),
  rightMargin: z.number().min(0).optional().describe("Right margin, mm"),
  bottomMargin: z.number().min(0).optional().describe("Bottom margin, mm"),
};

const GEOMETRY_KEYS = Object.keys(MasterGeometryFields);

/** Master-only flag (verified live: Archicad ignores it on normal layouts, which always report false). */
const MasterOnlyFields = {
  displayMasterLayoutBelow: z
    .boolean()
    .optional()
    .describe("MASTER layouts only: draw the master's content below (true) or above (false) the layouts' content"),
};

/**
 * The only settings Archicad applies per layout (verified live: layoutPageNumber / actPageIndex and the revision
 * fields are derived and silently ignored by API.SetLayoutSettings, so they are output-only).
 */
const LayoutOwnFields = {
  customLayoutNumbering: z.boolean().optional().describe("true = use customLayoutNumber as the layout ID instead of the automatic subset numbering"),
  customLayoutNumber: z.string().optional().describe("Custom layout ID (used when customLayoutNumbering is true), e.g. 'A-101'"),
  doNotIncludeInNumbering: z.boolean().optional().describe("Exclude this layout from the automatic ID sequence of its subset"),
};

const LayoutSettingsFields = { ...MasterGeometryFields, ...MasterOnlyFields, ...LayoutOwnFields };

/** Fields of `patch` whose read-back value differs from what was requested (Archicad ignored them). */
function ignoredFields(patch: Json, after: Json): string[] {
  return Object.keys(patch).filter((k) => {
    const want = patch[k];
    const got = after[k];
    if (typeof want === "number" && typeof got === "number") return Math.abs(want - got) > 1e-6;
    return want !== got;
  });
}

const LAYOUT_PARAM_KEYS = [
  "horizontalSize",
  "verticalSize",
  "leftMargin",
  "topMargin",
  "rightMargin",
  "bottomMargin",
  "customLayoutNumber",
  "customLayoutNumbering",
  "doNotIncludeInNumbering",
  "displayMasterLayoutBelow",
  "layoutPageNumber",
  "actPageIndex",
  "currentRevisionId",
  "currentFinalRevisionId",
  "hasIssuedRevision",
  "hasActualRevision",
] as const;

const LayoutRef = z
  .string()
  .min(1)
  .describe("Layout: navigator item id (GUID from get_navigator_tree {tree: 'LayoutBook'}) or layout name / 'ID name' (e.g. '03 План на отм. ±0.000')");

function pickLayoutFields(src: Json): Json {
  const out: Json = {};
  for (const k of Object.keys(LayoutSettingsFields)) if (src[k] !== undefined) out[k] = src[k];
  return out;
}

async function getLayoutSettings(ac: ArchicadClient, guid: string): Promise<Json> {
  const res = await ac.api<{ layoutParameters?: Json }>("API.GetLayoutSettings", { layoutNavigatorItemId: gid(guid) });
  if (!res.layoutParameters) throw new Error(`No layout settings returned for ${guid}.`);
  return res.layoutParameters;
}

/** Full LayoutParameters object for the API: base settings (master/current) overridden by the given fields. */
function mergeLayoutParams(base: Json, patch: Json): Json {
  const out: Json = {};
  const defaults: Json = {
    horizontalSize: 420,
    verticalSize: 297,
    leftMargin: 10,
    topMargin: 10,
    rightMargin: 10,
    bottomMargin: 10,
    customLayoutNumber: "",
    customLayoutNumbering: false,
    doNotIncludeInNumbering: false,
    displayMasterLayoutBelow: false,
    layoutPageNumber: 1,
    actPageIndex: 1,
    currentRevisionId: "",
    currentFinalRevisionId: "",
    hasIssuedRevision: false,
    hasActualRevision: false,
  };
  for (const k of LAYOUT_PARAM_KEYS) out[k] = patch[k] ?? base[k] ?? defaults[k];
  return out;
}

const LAYOUT_TYPES = new Set(["LayoutItem", "MasterLayoutItem"]);

const LOCATE_TREES: TreeKind[] = ["LayoutBook", "ViewMap", "ProjectMap"];

/** Finds an item by id in the Layout Book / View Map / Project Map (in that order). */
async function locateNavItem(ac: ArchicadClient, guid: string): Promise<{ tree: TreeKind; item: NavFlat } | undefined> {
  const g = normGuid(guid);
  for (const tree of LOCATE_TREES) {
    let items: NavFlat[];
    try {
      items = flattenNav(await getNavigatorRoot(ac, tree));
    } catch {
      continue;
    }
    const item = items.find((f) => normGuid(f.id) === g);
    if (item) return { tree, item };
  }
  return undefined;
}

async function resolveLayout(ac: ArchicadClient, ref: string): Promise<NavFlat | { id: string }> {
  if (isGuid(ref)) return { id: normGuid(ref) };
  return findNavItemIn(ac, "LayoutBook", ref, (f) => LAYOUT_TYPES.has(f.type), "Layout");
}

const DETAIL_COMMANDS: Record<string, string> = {
  StoryItem: "API.GetStoryNavigatorItems",
  SectionItem: "API.GetSectionNavigatorItems",
  ElevationItem: "API.GetElevationNavigatorItems",
  InteriorElevationItem: "API.GetInteriorElevationNavigatorItems",
  WorksheetItem: "API.GetWorksheetNavigatorItems",
  DetailItem: "API.GetDetailNavigatorItems",
  DocumentFrom3DItem: "API.GetDocument3DNavigatorItems",
  UndefinedItem: "API.GetBuiltInContainerNavigatorItems",
};

function navDetailsOut(type: string, v: Json): Json {
  const o: Json = {};
  if (v["prefix"]) o["prefix"] = v["prefix"];
  if (v["name"] !== undefined) o["name"] = v["name"];
  if (type === "StoryItem") {
    o["storyIndex"] = v["floorNumber"];
    o["elevation"] = v["floorLevel"];
  }
  if (Array.isArray(v["contentIds"])) o["contents"] = (v["contentIds"] as unknown[]).map((c) => innerGuid(c)).filter(Boolean);
  return o;
}

export function registerOfficialNavigatorTools(server: McpServer, ctx: ToolContext): void {
  defineTool(server, ctx, {
    name: "get_navigator_tree",
    title: "Navigator tree",
    description:
      "Returns a Navigator tree: Project Map (viewpoints: stories, sections, elevations, interior elevations, worksheets, details, 3D documents, " +
      "3D views, schedules, project indexes, lists), View Map (saved views, each with sourceId = its Project Map viewpoint), Layout Book " +
      "(subsets, layouts with their drawings, master layouts) or a publisher set. Nodes: {id, type, prefix?, name, sourceId?, children?}. " +
      "With `types` / `nameFilter` or format 'flat' you get a flat list {id, type, prefix, name, path, depth, parentId, sourceId, childCount}. " +
      "The ids are used by get_navigator_items, rename/move/delete_navigator_items, clone_project_map_item_to_view_map, create_layout, " +
      "get/set_layout_settings and the view/documentation tools.",
    input: {
      tree: TreeKindSchema.optional().describe("Which tree (default 'ProjectMap')"),
      publisherSet: z.string().min(1).optional().describe("Publisher set name for tree 'PublisherSet' (see get_publisher_sets)"),
      root: NavId.optional().describe("Only the subtree below this item id"),
      maxDepth: z.number().int().min(0).max(30).optional().describe("Levels below the root to return (0 = root only). Default: all"),
      types: z.array(NavigatorItemType).min(1).optional().describe("Only items of these types (flat list), e.g. ['LayoutItem'] or ['StoryItem','SectionItem']"),
      nameFilter: z.string().min(1).optional().describe("Case-insensitive substring of prefix/ID or name (flat list)"),
      format: z.enum(["tree", "flat"]).optional().describe("'tree' (default) or 'flat'"),
      limit: z.number().int().min(1).max(10000).optional().describe("Flat list: maximum items (default 2000)"),
    },
    annotations: READ_ONLY,
    handler: async ({ tree, publisherSet, root, maxDepth, types, nameFilter, format, limit }, { ac }) => {
      const kind = (tree ?? (publisherSet ? "PublisherSet" : "ProjectMap")) as TreeKind;
      let rootRaw = await getNavigatorRoot(ac, kind, publisherSet);
      if (root) {
        const found = findNavRaw(rootRaw, root);
        if (!found) throw new Error(`Item ${root} is not in the ${kind} tree. Check the id with get_navigator_tree {tree: '${kind}'} (other trees: ProjectMap, ViewMap, LayoutBook).`);
        rootRaw = found;
      }
      if (types || nameFilter || format === "flat") {
        let items = flattenNav(rootRaw, maxDepth);
        if (types) {
          const set = new Set<string>(types);
          items = items.filter((i) => set.has(i.type));
        }
        if (nameFilter) {
          const s = lc(nameFilter);
          items = items.filter((i) => lc(`${i.prefix ?? ""} ${i.name}`).includes(s));
        }
        const max = limit ?? 2000;
        return { tree: kind, total: items.length, truncated: items.length > max, items: items.slice(0, max) };
      }
      return { tree: kind, root: navNode(rootRaw, 0, maxDepth) };
    },
  });

  defineTool(server, ctx, {
    name: "get_navigator_items",
    title: "Navigator item details",
    description:
      "Returns details of navigator items by id: type, name, prefix (ID), location (tree + path + parentId + sourceId), and type-specific data — " +
      "stories: storyIndex and elevation (m); built-in folders: contents; layouts and master layouts: layoutSettings (paper size in mm, margins, " +
      "numbering). Type-specific data of viewpoints is only available for Project Map items (a View Map item's sourceId is its viewpoint). " +
      "Output: {items: [{id, type, ...} | {id, error}]}.",
    input: {
      ids: z.array(NavId).min(1).max(500).describe("Navigator item ids"),
      locate: z.boolean().optional().describe("Find each item in the Project Map / View Map / Layout Book trees to add tree, path, name, parentId, sourceId (default true)"),
    },
    annotations: READ_ONLY,
    handler: async ({ ids, locate }, { ac }) => {
      const guids = ids.map(normGuid);
      const typesRes = await ac.api<{ navigatorItemIdAndTypeList?: unknown[] }>("API.GetNavigatorItemsType", { navigatorItemIds: navIdItems(guids) });
      const typeList = typesRes.navigatorItemIdAndTypeList ?? [];
      const rows: Json[] = guids.map((g, i) => {
        const u = unwrap<Json>(typeList[i]);
        return u.ok ? { id: g, type: u.value["navigatorItemType"] } : { id: g, error: u.error };
      });
      // type-specific details, one call per type
      const byType = new Map<string, number[]>();
      rows.forEach((r, i) => {
        if (r["error"]) return;
        const t = String(r["type"]);
        if (DETAIL_COMMANDS[t] || LAYOUT_TYPES.has(t)) (byType.get(t) ?? byType.set(t, []).get(t)!).push(i);
      });
      await Promise.all(
        [...byType].map(async ([t, idx]) => {
          if (LAYOUT_TYPES.has(t)) {
            await Promise.all(
              idx.map(async (i) => {
                try {
                  rows[i]!["layoutSettings"] = await getLayoutSettings(ac, guids[i]!);
                } catch (e) {
                  rows[i]!["detailsError"] = e instanceof Error ? e.message : String(e);
                }
              }),
            );
            return;
          }
          const res = await ac.api<{ navigatorItems?: unknown[] }>(DETAIL_COMMANDS[t]!, { navigatorItemIds: navIdItems(idx.map((i) => guids[i]!)) });
          const list = res.navigatorItems ?? [];
          idx.forEach((i, k) => {
            const u = unwrap<Json>(list[k]);
            if (u.ok) Object.assign(rows[i]!, navDetailsOut(t, u.value));
            else if (t !== "UndefinedItem") rows[i]!["detailsNote"] = "Type details are only available for Project Map items (use this item's sourceId).";
          });
        }),
      );
      if (locate !== false) {
        const wanted = new Set(guids);
        const trees: TreeKind[] = ["ProjectMap", "ViewMap", "LayoutBook"];
        const flats = await Promise.all(
          trees.map(async (t) => {
            try {
              return { t, items: flattenNav(await getNavigatorRoot(ac, t)) };
            } catch {
              return { t, items: [] as NavFlat[] };
            }
          }),
        );
        for (const { t, items } of flats) {
          for (const f of items) {
            const g = normGuid(f.id);
            if (!wanted.has(g)) continue;
            for (const r of rows) {
              if (r["id"] !== g || r["tree"]) continue;
              r["tree"] = t;
              r["path"] = f.path;
              if (r["name"] === undefined) r["name"] = f.name;
              if (r["prefix"] === undefined && f.prefix) r["prefix"] = f.prefix;
              if (f.parentId) r["parentId"] = f.parentId;
              if (f.sourceId) r["sourceId"] = f.sourceId;
              if (f.childCount) r["childCount"] = f.childCount;
            }
          }
        }
      }
      return { items: rows };
    },
  });

  defineTool(server, ctx, {
    name: "rename_navigator_item",
    title: "Rename navigator item",
    description:
      "Renames a navigator item (view, View Map folder, layout, subset, viewpoint...): new name and/or new ID (the prefix shown before the " +
      "name, e.g. layout number '03' or subset ID 'АР'). Pass at least one of newName / newId. The item is read back from the Layout Book / " +
      "View Map / Project Map: output {ok, id, tree, prefix, name, warning?} — `warning` tells when Archicad kept a different name/ID " +
      "(for sheet numbers of layouts prefer set_layout_settings {customLayoutNumbering: true, customLayoutNumber}). A new ID of an " +
      "auto-numbered SUBSET does not last: Archicad recomputes it on the next Layout Book change. For a lasting subset ID create the subset " +
      "with create_layout_subset {customNumbering: true, customNumber} and move the layouts into it (move_navigator_item).",
    input: {
      item: NavId,
      newName: z.string().min(1).optional().describe("New name"),
      newId: z.string().optional().describe("New ID / prefix ('' clears it where allowed)"),
    },
    annotations: MODIFIES,
    handler: async ({ item, newName, newId }, { ac }) => {
      if (newName === undefined && newId === undefined) throw new Error("Pass newName and/or newId.");
      const params: Json = { navigatorItemId: gid(item) };
      if (newName !== undefined) params["newName"] = newName;
      if (newId !== undefined) params["newId"] = newId;
      await ac.api("API.RenameNavigatorItem", params);
      const out: Json = { ok: true, id: normGuid(item) };
      const found = await locateNavItem(ac, item);
      if (!found) {
        if (newName !== undefined) out["name"] = newName;
        if (newId !== undefined) out["prefix"] = newId;
        return out;
      }
      const prefix = found.item.prefix ?? "";
      Object.assign(out, { tree: found.tree, type: found.item.type, prefix, name: found.item.name });
      const missed: string[] = [];
      if (newName !== undefined && found.item.name !== newName) missed.push(`name '${newName}'`);
      if (newId !== undefined && prefix.trim() !== newId.trim()) missed.push(`ID '${newId}'`);
      if (missed.length) {
        out["warning"] =
          `Archicad accepted the rename but the item shows ID '${prefix}' and name '${found.item.name}' (requested ${missed.join(" and ")}). ` +
          "IDs of auto-numbered layouts/subsets and of viewpoints are derived by Archicad: use set_layout_settings {customLayoutNumbering: true, " +
          "customLayoutNumber} for sheet numbers, or change the subset numbering.";
      } else if (newId !== undefined && found.item.type === "SubsetItem") {
        // Verified live: subset 'AP1' renamed to 'ЭО' read back as 'ЭО', then showed 'AP1' again after layouts were added elsewhere.
        out["warning"] =
          "Subset IDs are recomputed by Archicad on the next Layout Book change unless the subset uses custom numbering, which the " +
          "Archicad 26 API cannot switch on for an existing subset. For a lasting ID: create_layout_subset {customNumbering: true, " +
          "customNumber} and move the layouts into it with move_navigator_item.";
      }
      return out;
    },
  });

  defineTool(server, ctx, {
    name: "move_navigator_item",
    title: "Move navigator item",
    description:
      "Moves a navigator item (e.g. a view into a View Map folder, a layout into another subset) under `parent`, as its first child or " +
      "right after the sibling `after`. Works inside one tree (View Map / Layout Book). Get ids with get_navigator_tree. The item is read " +
      "back: output {ok, id, tree, parent, path}.",
    input: {
      item: NavId.describe("Item to move"),
      parent: NavId.describe("New parent folder / subset id"),
      after: NavId.optional().describe("Insert after this child of `parent` (default: first position)"),
    },
    annotations: MODIFIES,
    handler: async ({ item, parent, after }, { ac }) => {
      const params: Json = { navigatorItemIdToMove: gid(item), parentNavigatorItemId: gid(parent) };
      if (after) params["previousNavigatorItemId"] = gid(after);
      try {
        await ac.api("API.MoveNavigatorItem", params);
      } catch (e) {
        const msg = e instanceof Error ? e.message : String(e);
        if (/not found/i.test(msg)) {
          throw new Error(
            `${msg}. The parent (and 'after') must be in the SAME tree as the item: View Map folders for saved views, Layout Book subsets for ` +
              "layouts. Project Map items cannot be moved. Check ids with get_navigator_items.",
          );
        }
        throw e;
      }
      const out: Json = { ok: true, id: normGuid(item), parent: normGuid(parent) };
      const found = await locateNavItem(ac, item);
      if (found) {
        Object.assign(out, { tree: found.tree, path: found.item.path });
        if (found.item.parentId && normGuid(found.item.parentId) !== normGuid(parent)) {
          out["parent"] = found.item.parentId;
          out["warning"] = `The item is still under ${found.item.parentId}, not ${normGuid(parent)}: items can only move within their own tree (View Map or Layout Book).`;
        }
      }
      return out;
    },
  });

  defineTool(server, ctx, {
    name: "delete_navigator_items",
    title: "Delete navigator items",
    description:
      "Deletes navigator items: saved views and View Map folders, layouts (with their drawings), subsets, Project Map viewpoints where " +
      "Archicad allows it. This cannot be undone through this connector — check ids with get_navigator_items first. " +
      "Output: {results: [{id, ok: true} | {id, error}]}.",
    input: { items: z.array(NavId).min(1).max(500).describe("Navigator item ids to delete") },
    annotations: DESTRUCTIVE,
    handler: async ({ items }, { ac }) => {
      const guids = items.map(normGuid);
      const res = await ac.api<{ executionResults?: unknown[] }>("API.DeleteNavigatorItems", { navigatorItemIds: navIdItems(guids) });
      const r = execResults(res.executionResults);
      return { results: guids.map((g, i) => ({ id: g, ...(r[i] ?? { error: "no result" }) })) };
    },
  });

  defineTool(server, ctx, {
    name: "clone_project_map_item_to_view_map",
    title: "Save view from Project Map item",
    description:
      "Saves a Project Map viewpoint (story, section, elevation, detail, worksheet, 3D view, schedule, index...) as a new view in the View Map " +
      "(like 'Clone a view' in the Navigator), with the current view settings. The new view can then be placed on layouts. " +
      "Output: {viewId} = the new View Map item id.",
    input: {
      item: NavId.describe("Project Map item id (get_navigator_tree {tree: 'ProjectMap'})"),
      parent: NavId.optional().describe("View Map folder to put the view in (default: the View Map's top folder)"),
    },
    annotations: CREATES,
    handler: async ({ item, parent }, { ac }) => {
      let parentId = parent ? normGuid(parent) : undefined;
      if (!parentId) {
        const root = await getNavigatorRoot(ac, "ViewMap");
        const kids = ((root["children"] as Json[] | undefined) ?? []).map((c) => (c["navigatorItem"] ?? c) as Json);
        parentId = innerGuid((kids[0] ?? root)["navigatorItemId"]);
      }
      if (!parentId) throw new Error("Could not determine the View Map folder: pass 'parent'.");
      const res = await ac.api<{ createdNavigatorItemId?: Json }>("API.CloneProjectMapItemToViewMap", {
        projectMapNavigatorItemId: gid(item),
        parentNavigatorItemId: gid(parentId),
      });
      return { viewId: innerGuid(res.createdNavigatorItemId), parent: parentId };
    },
  });

  defineTool(server, ctx, {
    name: "create_view_map_folder",
    title: "Create View Map folder",
    description:
      "Creates a folder in the View Map (to organize saved views), under `parent` (default: the View Map's top folder), optionally after " +
      "the sibling `after`. Output: {folderId}. Move views into it with move_navigator_item.",
    input: {
      name: z.string().min(1).describe("Folder name"),
      parent: NavId.optional().describe("Parent View Map folder id"),
      after: NavId.optional().describe("Insert after this sibling"),
    },
    annotations: CREATES,
    handler: async ({ name, parent, after }, { ac }) => {
      const params: Json = { folderParameters: { name } };
      if (parent) params["parentNavigatorItemId"] = gid(parent);
      if (after) params["previousNavigatorItemId"] = gid(after);
      const res = await ac.api<{ createdFolderNavigatorItemId?: Json }>("API.CreateViewMapFolder", params);
      return { folderId: innerGuid(res.createdFolderNavigatorItemId), name };
    },
  });

  defineTool(server, ctx, {
    name: "create_layout",
    title: "Create layout",
    description:
      "Creates a layout (sheet) in the Layout Book from a master layout, inside a subset (or the Layout Book root). The paper size and " +
      "margins come from the master (they are stored in the master and shared by all its layouts) — pick a master with the wanted size " +
      "(see get_layout_settings on MasterLayoutItem ids from get_navigator_tree {tree: 'LayoutBook', types: ['MasterLayoutItem']}). " +
      "Output: {layoutId, name, master, parent, settings, ignored?}. The layout ID/number is assigned by the subset numbering (e.g. subset " +
      "'08' + own prefix 'T-' + '01' = '08T-01') unless customLayoutNumbering. New layouts are inserted as the FIRST child of the subset " +
      "(reorder with move_navigator_item). Place drawings on it with the documentation tools.",
    input: {
      name: z.string().min(1).describe("Layout name"),
      master: z.string().min(1).optional().describe("Master layout: id or name (e.g. 'А3 - А - Ф3'). Default: the first master layout"),
      parent: z.string().min(1).optional().describe("Subset id or name (e.g. 'Планы'), default: the Layout Book root"),
      ...LayoutOwnFields,
    },
    annotations: CREATES,
    handler: async ({ name, master, parent, ...settings }, { ac }) => {
      const flat = flattenNav(await getNavigatorRoot(ac, "LayoutBook"));
      const masters = flat.filter((f) => f.type === "MasterLayoutItem");
      let masterItem: NavFlat | undefined;
      if (master) masterItem = await findNavItemIn(ac, "LayoutBook", master, (f) => f.type === "MasterLayoutItem", "Master layout");
      else masterItem = masters[0];
      if (!masterItem) throw new Error("The Layout Book has no master layout; create one in Archicad first.");
      let parentItem: NavFlat | undefined;
      if (parent) parentItem = await findNavItemIn(ac, "LayoutBook", parent, (f) => f.type === "SubsetItem" || f.type === "LayoutBookRootItem", "Subset");
      else parentItem = flat.find((f) => f.type === "LayoutBookRootItem");
      if (!parentItem) throw new Error("Could not find the Layout Book root; pass 'parent' (a subset id).");
      // The master's own settings MUST be passed unchanged: CreateLayout writes size/margins into the master.
      const base = await getLayoutSettings(ac, masterItem.id);
      const layoutParameters = mergeLayoutParams(
        { ...base, layoutPageNumber: 1, actPageIndex: 1, displayMasterLayoutBelow: false, customLayoutNumbering: false, customLayoutNumber: "", doNotIncludeInNumbering: false },
        pickLayoutFields(settings as Json),
      );
      const res = await ac.api<{ createdNavigatorItemId?: Json }>("API.CreateLayout", {
        layoutName: name,
        layoutParameters,
        masterNavigatorItemId: gid(masterItem.id),
        parentNavigatorItemId: gid(parentItem.id),
      });
      const layoutId = innerGuid(res.createdNavigatorItemId);
      let finalSettings: Json | undefined;
      if (layoutId) {
        try {
          finalSettings = await getLayoutSettings(ac, layoutId);
        } catch {
          /* optional */
        }
      }
      const out: Json = { layoutId, name, master: masterItem.name, parent: parentItem.name, settings: finalSettings ?? layoutParameters };
      const ignored = finalSettings ? ignoredFields(pickLayoutFields(settings as Json), finalSettings) : [];
      if (ignored.length) out["ignored"] = ignored;
      return out;
    },
  });

  defineTool(server, ctx, {
    name: "create_layout_subset",
    title: "Create layout subset",
    description:
      "Creates a subset (folder with its own numbering) in the Layout Book, under `parent` (subset id/name, default: root). Layout IDs in the " +
      "subset are [upper prefix][own prefix][number in numberingStyle, from startAt] (e.g. ownPrefix 'A-', style '01', startAt 1 -> A-01, A-02...), " +
      "or continue the previous subset's sequence. Output: {subsetId}. Check the resulting IDs with get_navigator_tree {tree: 'LayoutBook'}.",
    input: {
      name: z.string().min(1).describe("Subset name"),
      parent: z.string().min(1).optional().describe("Parent subset id or name (default: Layout Book root)"),
      includeToIDSequence: z.boolean().optional().describe("Include the subset's layouts in the ID sequence (default true)"),
      useUpperPrefix: z.boolean().optional().describe("Prepend the parent subset's prefix (default true)"),
      addOwnPrefix: z.boolean().optional().describe("Add ownPrefix to layout IDs (default: true when ownPrefix is given)"),
      ownPrefix: z.string().optional().describe("Own prefix, e.g. 'A-' (default '')"),
      continueNumbering: z
        .boolean()
        .optional()
        .describe("Continue the IDs of the previous subset (own prefix and startAt are then not used). Default: false when ownPrefix or startAt is given, else true"),
      startAt: z.number().int().min(0).optional().describe("First number when continueNumbering is false (default 1)"),
      numberingStyle: z
        .enum(["1", "01", "001", "0001", "abc", "ABC", "noID", "Undefined"])
        .optional()
        .describe("Number style of layout IDs (default '1'; 'noID' = no numbers)"),
      customNumbering: z.boolean().optional().describe("Use customNumber as the subset ID instead of automatic numbering (default false)"),
      customNumber: z.string().optional().describe("Custom subset ID (with customNumbering)"),
      autoNumber: z.string().optional().describe("Automatic number text (advanced, default '')"),
    },
    annotations: CREATES,
    handler: async (args, { ac }) => {
      let parentId: string | undefined;
      if (args.parent) {
        parentId = (await findNavItemIn(ac, "LayoutBook", args.parent, (f) => f.type === "SubsetItem" || f.type === "LayoutBookRootItem", "Subset")).id;
      } else {
        parentId = flattenNav(await getNavigatorRoot(ac, "LayoutBook")).find((f) => f.type === "LayoutBookRootItem")?.id;
      }
      if (!parentId) throw new Error("Could not find the Layout Book root; pass 'parent'.");
      const ownPrefix = args.ownPrefix ?? "";
      const subsetParameters = {
        name: args.name,
        includeToIDSequence: args.includeToIDSequence ?? true,
        customNumbering: args.customNumbering ?? false,
        continueNumbering: args.continueNumbering ?? (args.ownPrefix === undefined && args.startAt === undefined),
        useUpperPrefix: args.useUpperPrefix ?? true,
        addOwnPrefix: args.addOwnPrefix ?? ownPrefix !== "",
        customNumber: args.customNumber ?? "",
        autoNumber: args.autoNumber ?? "",
        numberingStyle: args.numberingStyle ?? "1",
        startAt: args.startAt ?? 1,
        ownPrefix,
      };
      const res = await ac.api<{ createdSubsetId?: Json }>("API.CreateLayoutSubset", { subsetParameters, parentNavigatorItemId: gid(parentId) });
      return { subsetId: innerGuid(res.createdSubsetId), name: args.name, parent: parentId };
    },
  });

  defineTool(server, ctx, {
    name: "get_layout_settings",
    title: "Get layout settings",
    description:
      "Returns the settings of layouts or master layouts: paper size (horizontalSize x verticalSize, MILLIMETERS) and margins (mm) — both " +
      "defined by the master layout —, custom numbering (customLayoutNumbering, customLayoutNumber), doNotIncludeInNumbering, " +
      "displayMasterLayoutBelow (meaningful on master layouts; always false on layouts), and the read-only page count (layoutPageNumber, " +
      "actPageIndex) and revision state. Layouts by id or name. Output: {layouts: [{layout, name?, settings} | {layout, error}]}. Change them with set_layout_settings.",
    input: { layouts: z.array(LayoutRef).min(1).max(200) },
    annotations: READ_ONLY,
    handler: async ({ layouts }, { ac }) => {
      const out = await Promise.all(
        layouts.map(async (ref) => {
          try {
            const l = await resolveLayout(ac, ref);
            const settings = await getLayoutSettings(ac, l.id);
            return { layout: l.id, ...("name" in l ? { name: l.name } : {}), settings };
          } catch (e) {
            return { layout: ref, error: e instanceof Error ? e.message : String(e) };
          }
        }),
      );
      return { layouts: out };
    },
  });

  defineTool(server, ctx, {
    name: "set_layout_settings",
    title: "Set layout settings",
    description:
      "Changes settings of layouts or master layouts. Per-layout fields: customLayoutNumbering + customLayoutNumber (custom sheet ID) and " +
      "doNotIncludeInNumbering. Paper size and margins (MILLIMETERS) belong to the MASTER layout: set them on a MasterLayoutItem id — or on a " +
      "layout with applyToMaster: true — and every layout using that master changes. displayMasterLayoutBelow works on MasterLayoutItem ids " +
      "only. Page count / page index / revision state are read-only (see get_layout_settings). Only the given fields change; the result is " +
      "read back, and fields Archicad did not apply are listed in `ignored`. Output: {results: [{layout, type, settings, ignored?} | {layout, error}]}.",
    input: {
      layouts: z
        .array(
          z.object({
            layout: LayoutRef,
            ...LayoutSettingsFields,
            applyToMaster: z
              .boolean()
              .optional()
              .describe("Required to change size/margins through a normal layout (they are shared with its master and all its layouts)"),
          }),
        )
        .min(1)
        .max(200)
        .describe("Per-layout changes: {layout, field: value, ...}"),
    },
    annotations: MODIFIES,
    handler: async ({ layouts }, { ac }) => {
      const results: Json[] = [];
      for (const item of layouts) {
        const { layout, applyToMaster, ...fields } = item;
        try {
          const l = await resolveLayout(ac, layout);
          const patch = pickLayoutFields(fields as Json);
          if (Object.keys(patch).length === 0) throw new Error("No settings to change were given.");
          const current = await getLayoutSettings(ac, l.id);
          let type: string | undefined = "type" in l ? l.type : undefined;
          if (!type) {
            const t = await ac.api<{ navigatorItemIdAndTypeList?: unknown[] }>("API.GetNavigatorItemsType", { navigatorItemIds: navIdItems([l.id]) });
            const u = unwrap<Json>(t.navigatorItemIdAndTypeList?.[0]);
            type = u.ok ? String(u.value["navigatorItemType"]) : undefined;
          }
          const isMaster = type === "MasterLayoutItem";
          const geometryChange = GEOMETRY_KEYS.filter((k) => patch[k] !== undefined && patch[k] !== current[k]);
          if (geometryChange.length && !isMaster && !applyToMaster) {
            throw new Error(
              `Paper size / margins (${geometryChange.join(", ")}) belong to the master layout of this layout: changing them changes the master and EVERY layout that ` +
                "uses it. Pass applyToMaster: true to do that, set them on the MasterLayoutItem id, or recreate the layout from a master " +
                "with the wanted paper size (create_layout {master}).",
            );
          }
          if (patch["displayMasterLayoutBelow"] !== undefined && !isMaster) {
            throw new Error(
              "displayMasterLayoutBelow is a master layout setting (Archicad ignores it on normal layouts): set it on the MasterLayoutItem id " +
                "(get_navigator_tree {tree: 'LayoutBook', types: ['MasterLayoutItem']}).",
            );
          }
          await ac.api("API.SetLayoutSettings", { layoutParameters: mergeLayoutParams(current, patch), layoutNavigatorItemId: gid(l.id) });
          const after = await getLayoutSettings(ac, l.id);
          const ignored = ignoredFields(patch, after);
          results.push({ layout: l.id, ...(type ? { type } : {}), settings: after, ...(ignored.length ? { ignored } : {}) });
        } catch (e) {
          results.push({ layout, error: e instanceof Error ? e.message : String(e) });
        }
      }
      return { results };
    },
  });

  defineTool(server, ctx, {
    name: "get_publisher_sets",
    title: "Publisher sets",
    description:
      "Lists the publisher sets of the project (Navigator > Publisher). With `name`, also returns that set's items as a flat list " +
      "{id, type, name, path, sourceId} (sourceId = the View Map view or layout that is published). Publish with the documentation tools.",
    input: {
      name: z.string().min(1).optional().describe("Publisher set name to expand (exact, case-insensitive or unique substring)"),
      maxDepth: z.number().int().min(1).max(30).optional().describe("With name: levels to list (default: all)"),
      limit: z.number().int().min(1).max(10000).optional().describe("With name: maximum items (default 1000)"),
    },
    annotations: READ_ONLY,
    handler: async ({ name, maxDepth, limit }, { ac }) => {
      const res = await ac.api<{ publisherSetNames?: string[] }>("API.GetPublisherSetNames");
      const names = res.publisherSetNames ?? [];
      if (!name) return { publisherSets: names };
      const exact = names.find((n) => n === name) ?? names.find((n) => lc(n) === lc(name)) ?? names.find((n) => lc(n).includes(lc(name)));
      if (!exact) throw new Error(`Publisher set '${name}' not found. Available: ${names.map((n) => `'${n}'`).join(", ") || "(none)"}.`);
      const items = flattenNav(await getNavigatorRoot(ac, "PublisherSet", exact), maxDepth).slice(1);
      const max = limit ?? 1000;
      return { publisherSets: names, name: exact, total: items.length, truncated: items.length > max, items: items.slice(0, max) };
    },
  });
}
