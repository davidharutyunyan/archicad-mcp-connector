/**
 * Official JSON API wrappers for classifications: systems, item trees, item details, element
 * classifications (get/set), elements by classification and property availability.
 * Classification items are referenced by GUID, by their (localized) item id such as 'Стена' / 'Ss_25_10',
 * or by path 'Parent > Child'.
 */

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import type { ArchicadClient } from "../archicad/client.js";
import { defineTool, MODIFIES, READ_ONLY, type ToolContext } from "./define.js";
import { typesOf } from "./official-elements.js";
import {
  classItemIdItems,
  ClassificationContext,
  elementIdItems,
  execResults,
  getPropertyDefinitions,
  gid,
  innerGuid,
  itemSummary,
  lc,
  normGuid,
  PropertyRef,
  propIdItems,
  refGuid,
  resolveProperties,
  uniq,
  unwrap,
  type ClassItem,
  type ClassTreeNode,
  type Json,
  type PropertyDef,
  type PropertyRefT,
} from "./official-helpers.js";
import { ElementRefs, Guid } from "./schemas.js";

export const ClassItemRef = z
  .union([z.string().min(1), z.object({ guid: Guid })])
  .describe(
    "Classification item: GUID, item id as shown in Archicad (localized, e.g. 'Стена', 'Перекрытие', 'Ss_25_10_30'), or path 'Parent > Child'. " +
      "Browse with get_classification_tree.",
  );

export const SystemRef = z
  .string()
  .min(1)
  .describe("Classification system name (e.g. 'Классификация Archicad') or GUID; may be omitted when the project has only one system");

type ItemRefT = z.infer<typeof ClassItemRef>;

function buildTree(roots: Json[], maxDepth: number | undefined, withDescriptions: boolean, depth = 0): ClassTreeNode[] {
  return roots.map((n) => {
    const ci = (n["classificationItem"] ?? n) as Json;
    const node: ClassTreeNode = { guid: innerGuid(ci["classificationItemId"]) ?? "", id: String(ci["id"] ?? "") };
    if (ci["name"]) node.name = String(ci["name"]);
    if (withDescriptions && ci["description"]) node.description = String(ci["description"]);
    const kids = (ci["children"] as Json[] | undefined) ?? [];
    if (kids.length) {
      if (maxDepth !== undefined && depth + 1 >= maxDepth) node.childCount = kids.length;
      else node.children = buildTree(kids, maxDepth, withDescriptions, depth + 1);
    }
    return node;
  });
}

function findRawItem(roots: Json[], guid: string): Json | undefined {
  const g = normGuid(guid);
  const stack = [...roots];
  while (stack.length) {
    const n = stack.pop()!;
    const ci = (n["classificationItem"] ?? n) as Json;
    if (normGuid(innerGuid(ci["classificationItemId"]) ?? "") === g) return n;
    stack.push(...(((ci["children"] as Json[] | undefined) ?? []) as Json[]));
  }
  return undefined;
}

/**
 * Above this many classification items, get_elements_by_classification scans the classifications of all elements
 * (a few requests) instead of one API.GetElementsByClassification per item (~85 ms each: a 600-item branch took 51 s).
 */
const PER_ITEM_QUERY_LIMIT = 12;
const SCAN_CHUNK = 5000;

async function elementsByItems(ac: ArchicadClient, targets: ClassItem[]): Promise<Array<{ guid: string; item: ClassItem }>> {
  const found: Array<{ guid: string; item: ClassItem }> = [];
  const batches = await Promise.all(
    targets.map(async (t) => ({
      t,
      r: await ac.api<{ elements?: unknown[] }>("API.GetElementsByClassification", { classificationItemId: gid(t.guid) }),
    })),
  );
  for (const { t, r } of batches) {
    for (const e of r.elements ?? []) {
      const g = innerGuid(e);
      if (g) found.push({ guid: normGuid(g), item: t });
    }
  }
  return found;
}

async function elementsInBranchByScan(ac: ArchicadClient, systemGuid: string, targets: ClassItem[]): Promise<Array<{ guid: string; item: ClassItem }>> {
  const wanted = new Map(targets.map((t) => [normGuid(t.guid), t]));
  const all = ((await ac.api<{ elements?: unknown[] }>("API.GetAllElements")).elements ?? [])
    .map((x) => innerGuid(x))
    .filter((g): g is string => !!g)
    .map(normGuid);
  const found: Array<{ guid: string; item: ClassItem }> = [];
  for (let i = 0; i < all.length; i += SCAN_CHUNK) {
    const chunk = all.slice(i, i + SCAN_CHUNK);
    const res = await ac.api<{ elementClassifications?: unknown[] }>("API.GetClassificationsOfElements", {
      elements: elementIdItems(chunk),
      classificationSystemIds: [{ classificationSystemId: gid(systemGuid) }],
    });
    const list = res.elementClassifications ?? [];
    chunk.forEach((g, k) => {
      const u = unwrap<unknown[]>(list[k]);
      if (!u.ok || !Array.isArray(u.value)) return;
      for (const c of u.value) {
        const cu = unwrap<Json>(c);
        const itemGuid = cu.ok ? innerGuid(cu.value["classificationItemId"]) : undefined;
        const item = itemGuid ? wanted.get(normGuid(itemGuid)) : undefined;
        if (item) found.push({ guid: g, item });
      }
    });
  }
  return found;
}

function propSummary(d: PropertyDef | { guid: string; error: string }): Json {
  if ("error" in d && (d as { error?: string }).error) return { guid: d.guid, error: (d as { error: string }).error };
  const p = d as PropertyDef;
  return { guid: p.guid, group: p.group, name: p.name, type: p.type };
}

export function registerOfficialClassificationTools(server: McpServer, ctx: ToolContext): void {
  defineTool(server, ctx, {
    name: "get_classification_systems",
    title: "Classification systems",
    description:
      "Lists the classification systems of the project (e.g. 'Классификация Archicad', Uniclass, OmniClass) with GUID, name, source, version, " +
      "date and the number of items. Start here before classifying elements; then browse items with get_classification_tree.",
    input: {
      includeItemCounts: z.boolean().optional().describe("Also count the items of each system (default true)"),
    },
    annotations: READ_ONLY,
    handler: async ({ includeItemCounts }, { ac }) => {
      const cc = new ClassificationContext(ac);
      const systems = await cc.allSystems();
      if (includeItemCounts === false) return { systems };
      const withCounts = await Promise.all(systems.map(async (s) => ({ ...s, itemCount: (await cc.tree(s.guid)).items.length })));
      return { systems: withCounts };
    },
  });

  defineTool(server, ctx, {
    name: "get_classification_tree",
    title: "Classification tree",
    description:
      "Returns the items of a classification system as a tree ({guid, id, name?, children?}) or, with `search` / format 'flat', as a flat " +
      "list with full paths ('Parent > Child') and depth. Item ids are what Archicad shows (localized in Russian Archicad, e.g. 'Стена', " +
      "'Перекрытие'). Use `root` to get one branch and `maxDepth` to limit large systems (childCount tells what was cut). The item GUIDs/ids " +
      "are used by set_element_classifications, get_elements_by_classification and get_classification_item_details.",
    input: {
      system: SystemRef.optional(),
      search: z.string().min(1).optional().describe("Case-insensitive substring of item id, name or description; returns a flat list of matches"),
      root: ClassItemRef.optional().describe("Return only the branch below this item"),
      maxDepth: z.number().int().min(1).max(20).optional().describe("Levels to return (1 = top-level items only). Default: all"),
      format: z.enum(["tree", "flat"]).optional().describe("'tree' (default) nested children, 'flat' list with paths"),
      includeDescriptions: z.boolean().optional().describe("Include item descriptions (default false; can be long)"),
      limit: z.number().int().min(1).max(5000).optional().describe("Flat/search results: max items (default 500)"),
    },
    annotations: READ_ONLY,
    handler: async ({ system, search, root, maxDepth, format, includeDescriptions, limit }, { ac }) => {
      const cc = new ClassificationContext(ac);
      const sys = await cc.system(system);
      const tree = await cc.tree(sys.guid);
      const head = { system: { guid: sys.guid, name: sys.name }, totalItems: tree.items.length };
      let rootItem: ClassItem | undefined;
      if (root) rootItem = await cc.item(root as ItemRefT, sys.guid);
      if (search || format === "flat") {
        let items = tree.items;
        if (rootItem) items = items.filter((i) => i.path === rootItem!.path || i.path.startsWith(rootItem!.path + " > "));
        if (maxDepth !== undefined) {
          const base = rootItem ? rootItem.depth + 1 : 0;
          items = items.filter((i) => i.depth < base + maxDepth);
        }
        if (search) {
          const s = lc(search);
          items = items.filter((i) => lc(i.id).includes(s) || lc(i.name).includes(s) || lc(i.description).includes(s));
        }
        const max = limit ?? 500;
        return {
          ...head,
          matches: items.length,
          truncated: items.length > max,
          items: items.slice(0, max).map((i) => {
            const o: Json = { guid: i.guid, id: i.id };
            if (i.name) o["name"] = i.name;
            if (includeDescriptions && i.description) o["description"] = i.description;
            o["path"] = i.path;
            o["depth"] = i.depth;
            if (i.childCount) o["childCount"] = i.childCount;
            return o;
          }),
        };
      }
      let roots = tree.roots;
      if (rootItem) {
        const raw = findRawItem(tree.roots, rootItem.guid);
        const ci = ((raw?.["classificationItem"] ?? raw) ?? {}) as Json;
        roots = (ci["children"] as Json[] | undefined) ?? [];
        return { ...head, root: itemSummary(rootItem), tree: buildTree(roots, maxDepth, includeDescriptions === true) };
      }
      return { ...head, tree: buildTree(roots, maxDepth, includeDescriptions === true) };
    },
  });

  defineTool(server, ctx, {
    name: "get_classification_item_details",
    title: "Classification item details",
    description:
      "Returns id, name, description, full path, parent and children of classification items (by GUID, id or path). " +
      "Output: {items: [{guid, id, name, description, path, system, parent?, children?} | {input, error}]}.",
    input: {
      items: z.array(ClassItemRef).min(1).max(500),
      system: SystemRef.optional().describe("Restrict id lookup to this system (default: search all systems)"),
      includeChildren: z.boolean().optional().describe("List the direct children of each item (default true)"),
    },
    annotations: READ_ONLY,
    handler: async ({ items, system, includeChildren }, { ac }) => {
      const cc = new ClassificationContext(ac);
      const systems = await cc.allSystems();
      const sysName = (g: string) => systems.find((s) => normGuid(s.guid) === normGuid(g))?.name;
      const out: Json[] = [];
      for (const ref of items as ItemRefT[]) {
        try {
          const it = await cc.item(ref, system);
          const tree = await cc.tree(it.system);
          const o: Json = { guid: it.guid, id: it.id, name: it.name, description: it.description, path: it.path, system: sysName(it.system) };
          if (it.parent) {
            const p = tree.items.find((x) => x.guid === it.parent);
            if (p) o["parent"] = { guid: p.guid, id: p.id };
          }
          if (includeChildren !== false && it.childCount) {
            o["children"] = tree.items.filter((x) => x.parent === it.guid).map((x) => ({ guid: x.guid, id: x.id, ...(x.name ? { name: x.name } : {}) }));
          }
          out.push(o);
        } catch (e) {
          out.push({ input: typeof ref === "string" ? ref : ref.guid, error: e instanceof Error ? e.message : String(e) });
        }
      }
      return { items: out };
    },
  });

  defineTool(server, ctx, {
    name: "get_element_classifications",
    title: "Get element classifications",
    description:
      "Returns how elements are classified in each classification system: {elements: [{guid, classifications: [{system, systemGuid, " +
      "item: {guid, id, name?, path} | null (= unclassified)}]} | {guid, error}]}. Default: all systems of the project.",
    input: {
      elements: ElementRefs.max(5000),
      systems: z.array(SystemRef).min(1).optional().describe("Only these systems (names or GUIDs). Default: all"),
    },
    annotations: READ_ONLY,
    handler: async ({ elements, systems }, { ac }) => {
      const cc = new ClassificationContext(ac);
      const sysList = systems ? await Promise.all(systems.map((s) => cc.system(s))) : await cc.allSystems();
      if (sysList.length === 0) return { elements: elements.map((e) => ({ guid: refGuid(e), classifications: [] })) };
      const guids = elements.map(refGuid);
      const res = await ac.api<{ elementClassifications?: unknown[] }>("API.GetClassificationsOfElements", {
        elements: elementIdItems(guids),
        classificationSystemIds: sysList.map((s) => ({ classificationSystemId: gid(s.guid) })),
      });
      const list = res.elementClassifications ?? [];
      const itemsByGuid = new Map<string, ClassItem>();
      for (const s of sysList) for (const i of (await cc.tree(s.guid)).items) itemsByGuid.set(normGuid(i.guid), i);
      return {
        elements: guids.map((g, i) => {
          const u = unwrap<unknown[]>(list[i]);
          if (!u.ok) return { guid: g, error: u.error };
          const cls = (Array.isArray(u.value) ? u.value : []).map((c, k) => {
            const cu = unwrap<Json>(c);
            const sys = sysList[k]!;
            if (!cu.ok) return { system: sys.name, systemGuid: sys.guid, error: cu.error };
            const itemGuid = innerGuid(cu.value["classificationItemId"]);
            const item = itemGuid ? itemsByGuid.get(normGuid(itemGuid)) : undefined;
            return {
              system: sys.name,
              systemGuid: sys.guid,
              item: itemGuid ? (item ? itemSummary(item) : { guid: itemGuid }) : null,
            };
          });
          return { guid: g, classifications: cls };
        }),
      };
    },
  });

  defineTool(server, ctx, {
    name: "set_element_classifications",
    title: "Set element classifications",
    description:
      "Classifies elements (one undo step per call handled by Archicad). Each assignment gives elements and the classification item " +
      "(GUID, localized id like 'Стена' / 'Перекрытие', or path 'A > B'); item null makes the elements unclassified in that system. " +
      "An element can have one item per system. Output: {results: [{guid, item?, system, ok: true} | {guid, error}]} per element. " +
      "Check the result with get_element_classifications. Classification decides which user-defined properties are available for elements.",
    input: {
      assignments: z
        .array(
          z.object({
            elements: ElementRefs.max(5000),
            item: ClassItemRef.nullable().describe("Classification item; null = unclassified in `system`"),
            system: SystemRef.optional().describe("System of the item (default: top-level `system`, or the system containing the item)"),
          }),
        )
        .min(1)
        .max(200),
      system: SystemRef.optional().describe("Default system for all assignments"),
    },
    annotations: MODIFIES,
    handler: async ({ assignments, system }, { ac }) => {
      const cc = new ClassificationContext(ac);
      const entries: Array<{ guid: string; system: string; systemName: string; item?: ClassItem }> = [];
      const early: Json[] = [];
      for (const a of assignments) {
        const sysRef = a.system ?? system;
        if (a.item === null) {
          const s = await cc.system(sysRef);
          for (const e of a.elements) entries.push({ guid: refGuid(e), system: s.guid, systemName: s.name });
          continue;
        }
        let item: ClassItem;
        try {
          item = await cc.item(a.item as ItemRefT, sysRef);
        } catch (err) {
          for (const e of a.elements) early.push({ guid: refGuid(e), error: err instanceof Error ? err.message : String(err) });
          continue;
        }
        const s = await cc.system(item.system);
        for (const e of a.elements) entries.push({ guid: refGuid(e), system: s.guid, systemName: s.name, item });
      }
      if (entries.length === 0) return { results: early };
      const res = await ac.api<{ executionResults?: unknown[] }>("API.SetClassificationsOfElements", {
        elementClassifications: entries.map((e) => ({
          elementId: gid(e.guid),
          classificationId: {
            classificationSystemId: gid(e.system),
            ...(e.item ? { classificationItemId: gid(e.item.guid) } : {}),
          },
        })),
      });
      const results = execResults(res.executionResults);
      return {
        results: [
          ...entries.map((e, i) => {
            const r = results[i] ?? { error: "no result returned" };
            if ("error" in r) return { guid: e.guid, error: r.error };
            return { guid: e.guid, system: e.systemName, item: e.item ? e.item.path : null, ok: true };
          }),
          ...early,
        ],
      };
    },
  });

  defineTool(server, ctx, {
    name: "get_elements_by_classification",
    title: "Elements by classification",
    description:
      "Returns the elements classified with an item (GUID, localized id or path), optionally including all sub-items of the branch. " +
      "Output: {item, total, countsByType, elements: [{guid, type, item?}]}.",
    input: {
      item: ClassItemRef,
      system: SystemRef.optional(),
      includeSubItems: z.boolean().optional().describe("Also elements classified with any descendant item (default false)"),
      includeTypes: z.boolean().optional().describe("Add each element's type and countsByType (default true)"),
    },
    annotations: READ_ONLY,
    handler: async ({ item, system, includeSubItems, includeTypes }, { ac }) => {
      const cc = new ClassificationContext(ac);
      const it = await cc.item(item as ItemRefT, system);
      let targets = [it];
      if (includeSubItems) {
        const tree = await cc.tree(it.system);
        targets = tree.items.filter((x) => x.path === it.path || x.path.startsWith(it.path + " > "));
      }
      const found =
        targets.length > PER_ITEM_QUERY_LIMIT ? await elementsInBranchByScan(ac, it.system, targets) : await elementsByItems(ac, targets);
      const types = includeTypes !== false ? await typesOf(ac, uniq(found.map((f) => f.guid))) : undefined;
      const out: Json = { item: itemSummary(it), total: found.length };
      if (types) {
        const counts: Record<string, number> = {};
        for (const f of found) {
          const t = types.get(normGuid(f.guid)) ?? "Unknown";
          counts[t] = (counts[t] ?? 0) + 1;
        }
        out["countsByType"] = counts;
      }
      out["elements"] = found.map((f) => {
        const o: Json = { guid: f.guid };
        if (types) o["type"] = types.get(normGuid(f.guid)) ?? "Unknown";
        if (includeSubItems) o["item"] = f.item.path;
        return o;
      });
      return out;
    },
  });

  defineTool(server, ctx, {
    name: "get_classification_availability",
    title: "Classification / property availability",
    description:
      "Shows which property definitions are available for classification items (user-defined properties appear on an element only when its " +
      "classification makes them available), and/or for which classification items given properties are available. " +
      "Pass `items` -> {items: [{item, properties: [{guid, group, name, type}]}]}; pass `properties` -> {properties: [{property, " +
      "availableForItems: count, items: [{guid, id, path}]}]}. Change availability with modify_property_definitions {addAvailability} " +
      "(properties family).",
    input: {
      items: z.array(ClassItemRef).min(1).max(200).optional().describe("Classification items to inspect"),
      properties: z.array(PropertyRef).min(1).max(200).optional().describe("Property definitions to inspect"),
      system: SystemRef.optional().describe("System for item id lookup (default: all systems)"),
      limit: z.number().int().min(1).max(5000).optional().describe("Max classification items listed per property (default 200)"),
    },
    annotations: READ_ONLY,
    handler: async ({ items, properties, system, limit }, { ac }) => {
      if (!items && !properties) throw new Error("Pass 'items' (classification items) and/or 'properties' (property definitions).");
      const cc = new ClassificationContext(ac);
      const out: Json = {};
      if (items) {
        const resolved: Array<ClassItem | { input: string; error: string }> = [];
        for (const r of items as ItemRefT[]) {
          try {
            resolved.push(await cc.item(r, system));
          } catch (e) {
            resolved.push({ input: typeof r === "string" ? r : r.guid, error: e instanceof Error ? e.message : String(e) });
          }
        }
        const good = resolved.filter((r): r is ClassItem => "guid" in r);
        const res = good.length
          ? await ac.api<{ classificationItemAvailabilityList?: unknown[] }>("API.GetClassificationItemAvailability", {
              classificationItemIds: classItemIdItems(good.map((g) => g.guid)),
            })
          : { classificationItemAvailabilityList: [] };
        const list = res.classificationItemAvailabilityList ?? [];
        const perItem = good.map((g, i) => {
          const u = unwrap<Json>(list[i]);
          if (!u.ok) return { item: itemSummary(g), error: u.error };
          const props = ((u.value["availableProperties"] as unknown[] | undefined) ?? []).map((p) => innerGuid(p)).filter((x): x is string => !!x);
          return { item: itemSummary(g), guids: props };
        });
        const allProps = uniq(perItem.flatMap((p) => ("guids" in p ? p.guids! : [])).map(normGuid));
        const defs = await getPropertyDefinitions(ac, allProps);
        const byGuid = new Map(defs.map((d) => [normGuid(d.guid), d]));
        out["items"] = [
          ...perItem.map((p) =>
            "guids" in p
              ? { item: p.item, properties: p.guids!.map((g) => propSummary(byGuid.get(normGuid(g)) ?? { guid: g, error: "unknown" })) }
              : p,
          ),
          ...resolved.filter((r) => !("guid" in r)),
        ];
      }
      if (properties) {
        const resolved = await resolveProperties(ac, properties as PropertyRefT[]);
        const good = resolved.filter((r) => r.guid);
        const res = good.length
          ? await ac.api<{ propertyDefinitionAvailabilityList?: unknown[] }>("API.GetPropertyDefinitionAvailability", {
              propertyIds: propIdItems(good.map((g) => g.guid!)),
            })
          : { propertyDefinitionAvailabilityList: [] };
        const list = res.propertyDefinitionAvailabilityList ?? [];
        const defs = await getPropertyDefinitions(
          ac,
          good.map((g) => g.guid!),
        );
        const systems = await cc.allSystems();
        const itemsByGuid = new Map<string, ClassItem>();
        for (const s of systems) for (const i of (await cc.tree(s.guid)).items) itemsByGuid.set(normGuid(i.guid), i);
        const max = limit ?? 200;
        out["properties"] = [
          ...good.map((g, i) => {
            const u = unwrap<Json>(list[i]);
            const prop = { input: g.input, ...propSummary(defs[i] ?? { guid: g.guid!, error: "unknown" }) };
            if (!u.ok) return { property: prop, error: u.error };
            const ids = ((u.value["availableClassifications"] as unknown[] | undefined) ?? []).map((c) => innerGuid(c)).filter((x): x is string => !!x);
            return {
              property: prop,
              availableForItems: ids.length,
              ...(ids.length === 0
                ? { note: "Not bound to any classification item (built-in properties depend on the element type, not on classification)." }
                : {}),
              truncated: ids.length > max,
              items: ids.slice(0, max).map((id) => {
                const it = itemsByGuid.get(normGuid(id));
                return it ? { guid: it.guid, id: it.id, path: it.path } : { guid: id };
              }),
            };
          }),
          ...resolved.filter((r) => !r.guid).map((r) => ({ property: { input: r.input }, error: r.error })),
        ];
      }
      return out;
    },
  });
}
