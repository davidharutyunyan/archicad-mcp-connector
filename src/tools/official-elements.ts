/**
 * Official JSON API wrappers: element listing, element types, bounding boxes, zone relations,
 * element components with their property values, and property-name -> GUID lookup.
 */

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import type { ArchicadClient } from "../archicad/client.js";
import { defineTool, READ_ONLY, type ToolContext } from "./define.js";
import {
  builtInCatalog,
  elementIdItems,
  formatPropertyValue,
  getPropertyDefinitions,
  innerGuid,
  lc,
  normGuid,
  OfficialElementType,
  PropertyRef,
  propIdItems,
  refGuid,
  resolveProperties,
  unwrap,
  userDefinedCatalog,
  type GuidRef,
  type Json,
  type PropertyDef,
  type PropertyRefT,
} from "./official-helpers.js";
import { ElementRef, ElementRefs, Guid, Pagination, paginate } from "./schemas.js";

/** Above this many elements list_elements types only the returned page (GetTypesOfElements cost). */
const TYPE_ALL_LIMIT = 20_000;

export async function typesOf(ac: ArchicadClient, guids: string[]): Promise<Map<string, string>> {
  const map = new Map<string, string>();
  if (guids.length === 0) return map;
  const res = await ac.api<{ typesOfElements?: unknown[] }>("API.GetTypesOfElements", { elements: elementIdItems(guids) });
  const list = res.typesOfElements ?? [];
  guids.forEach((g, i) => {
    const u = unwrap<Json>(list[i]);
    map.set(normGuid(g), u.ok ? String(u.value["elementType"] ?? "Unknown") : "Unknown");
  });
  return map;
}

function guidsOf(list: unknown[] | undefined): string[] {
  return (list ?? []).map((x) => innerGuid(x)).filter((g): g is string => !!g);
}

function countBy(types: Iterable<string>): Record<string, number> {
  const out: Record<string, number> = {};
  for (const t of types) out[t] = (out[t] ?? 0) + 1;
  return out;
}

const ANGLE_TYPES = new Set(["angle", "angleList"]);

/** Converts official property values to plain values; angles radians -> degrees (connector convention). */
export function plainValue(pv: unknown): { value: unknown; status?: string } {
  const f = formatPropertyValue(pv);
  if (f.status) return { value: null, status: f.status };
  if (f.type && ANGLE_TYPES.has(f.type)) {
    const deg = (v: unknown) => (typeof v === "number" ? (v * 180) / Math.PI : v);
    return { value: Array.isArray(f.value) ? f.value.map(deg) : deg(f.value) };
  }
  return { value: f.value };
}

interface ComponentPair {
  element: string;
  component: string;
}

/** Property values of element components: [{element, component, values, unavailable?} | {error}]. */
async function componentValues(ac: ArchicadClient, pairs: ComponentPair[], props: PropertyRefT[]) {
  const resolved = await resolveProperties(ac, props);
  const good = resolved.filter((r) => r.guid);
  const propErrors: Record<string, string> = {};
  for (const r of resolved) if (r.error) propErrors[r.input] = r.error;
  let lists: unknown[] = [];
  if (pairs.length && good.length) {
    const res = await ac.api<{ propertyValuesForElementComponents?: unknown[] }>("API.GetPropertyValuesOfElementComponents", {
      elementComponents: pairs.map((p) => ({ elementComponentId: { elementId: { guid: p.element }, componentId: { guid: p.component } } })),
      properties: propIdItems(good.map((g) => g.guid!)),
    });
    lists = res.propertyValuesForElementComponents ?? [];
  }
  const rows = pairs.map((p, i) => {
    const row: Json = { element: p.element, component: p.component };
    if (!good.length) return row;
    const u = unwrap<unknown[]>(lists[i]);
    if (!u.ok) return { ...row, error: u.error };
    const values: Json = {};
    const unavailable: Json = {};
    good.forEach((g, k) => {
      const item = (u.value ?? [])[k];
      const iu = unwrap(item);
      if (!iu.ok) unavailable[g.input] = iu.error;
      else {
        const v = plainValue(iu.value);
        if (v.status) unavailable[g.input] = v.status;
        else values[g.input] = v.value;
      }
    });
    row["values"] = values;
    if (Object.keys(unavailable).length) row["unavailable"] = unavailable;
    return row;
  });
  return { rows, propErrors };
}

async function componentsOf(ac: ArchicadClient, refs: GuidRef[]): Promise<Array<{ guid: string; components?: string[]; error?: string }>> {
  const guids = refs.map(refGuid);
  const res = await ac.api<{ componentsOfElements?: unknown[] }>("API.GetComponentsOfElements", { elements: elementIdItems(guids) });
  const list = res.componentsOfElements ?? [];
  return guids.map((g, i) => {
    const u = unwrap<unknown[]>(list[i]);
    if (!u.ok) return { guid: g, error: u.error };
    const comps = (Array.isArray(u.value) ? u.value : [])
      .map((c) => innerGuid(((c as Json)["elementComponentId"] as Json | undefined)?.["componentId"]))
      .filter((x): x is string => !!x);
    return { guid: g, components: comps };
  });
}

/** GUIDs of the properties available for the elements (union or intersection); failed elements go to `errors`. */
async function availablePropertyIds(
  ac: ArchicadClient,
  refs: GuidRef[],
  match: "any" | "all",
  propertyType: string | undefined,
  errors: Json[],
): Promise<Set<string>> {
  const guids = refs.map(refGuid);
  const params: Json = { elements: elementIdItems(guids) };
  if (propertyType) params["propertyType"] = propertyType;
  const res = await ac.api<{ propertyIdsOfElements?: unknown[] }>("API.GetAllPropertyIdsOfElements", params);
  const list = res.propertyIdsOfElements ?? [];
  const sets: Set<string>[] = [];
  guids.forEach((g, i) => {
    const u = unwrap<Json>(list[i]);
    if (!u.ok) {
      errors.push({ guid: g, error: u.error });
      return;
    }
    sets.push(new Set(guidsOf(u.value["propertyIds"] as unknown[] | undefined).map(normGuid)));
  });
  if (sets.length === 0) return new Set();
  if (match === "any") return new Set(sets.flatMap((s) => [...s]));
  return new Set([...sets[0]!].filter((g) => sets.every((s) => s.has(g))));
}

const COMPONENT_SUMMARY: PropertyRefT[] = [
  "BuildingMaterial_Name",
  "Component_Thickness",
  "Component_NetVolume",
  "Component_GrossVolume",
  "Component_NetProjectedArea",
  "Component_CrossSectionArea",
];

function boxOut(raw: unknown): Json | undefined {
  if (!raw || typeof raw !== "object") return undefined;
  return raw as Json;
}

function unionBoxes(boxes: Json[], keys: string[]): Json | undefined {
  if (boxes.length === 0) return undefined;
  const out: Json = {};
  for (const k of keys) {
    const vals = boxes.map((b) => b[k]).filter((v): v is number => typeof v === "number");
    out[k] = k.endsWith("Min") ? Math.min(...vals) : Math.max(...vals);
  }
  return out;
}

export function registerOfficialElementTools(server: McpServer, ctx: ToolContext): void {
  defineTool(server, ctx, {
    name: "list_elements",
    title: "List elements",
    description:
      "Lists element GUIDs of the whole project (all stories) through the official JSON API, optionally only some types or only the current " +
      "selection, with each element's type and the number of elements per type (countsByType). Returns {total, offset, returned, hasMore, " +
      "countsByType, elements: [{guid, type}]}. For attribute/geometry filters (story, layer, region, ID...) use find_elements; for details " +
      "of the returned elements use get_element_details, for extents get_bounding_boxes. Works without the Claude Connector add-on.",
    input: {
      types: z
        .array(OfficialElementType)
        .min(1)
        .optional()
        .describe("Only these element types. Omit for every element (including types not in this list, reported as 'Unknown')"),
      selectedOnly: z.boolean().optional().describe("Only the elements currently selected in Archicad (default false)"),
      onlyEditable: z.boolean().optional().describe("With selectedOnly: skip selected elements that are locked / on locked layers / not reserved"),
      includeTypes: z
        .boolean()
        .optional()
        .describe("Return {guid, type} items and countsByType (default true). false returns plain GUID strings — fastest for huge projects"),
      ...Pagination,
    },
    annotations: READ_ONLY,
    handler: async ({ types, selectedOnly, onlyEditable, includeTypes, offset, limit }, { ac }) => {
      const withTypes = includeTypes !== false;
      let guids: string[];
      let typeMap = new Map<string, string>();
      if (selectedOnly) {
        const params: Json = {};
        if (onlyEditable !== undefined) params["onlyEditable"] = onlyEditable;
        guids = guidsOf((await ac.api<{ elements?: unknown[] }>("API.GetSelectedElements", params)).elements);
        if ((withTypes || types) && guids.length <= TYPE_ALL_LIMIT) typeMap = await typesOf(ac, guids);
        if (types) {
          if (guids.length > TYPE_ALL_LIMIT) typeMap = await typesOf(ac, guids);
          const wanted = new Set<string>(types);
          guids = guids.filter((g) => wanted.has(typeMap.get(normGuid(g)) ?? ""));
        }
      } else if (types) {
        const lists = await Promise.all(
          types.map(async (t) => ({ t, g: guidsOf((await ac.api<{ elements?: unknown[] }>("API.GetElementsByType", { elementType: t })).elements) })),
        );
        guids = [];
        for (const { t, g } of lists) for (const x of g) {
          guids.push(x);
          typeMap.set(normGuid(x), t);
        }
      } else {
        guids = guidsOf((await ac.api<{ elements?: unknown[] }>("API.GetAllElements")).elements);
        if (withTypes && guids.length <= TYPE_ALL_LIMIT) typeMap = await typesOf(ac, guids);
      }
      const page = paginate(guids, offset ?? 0, limit ?? 500);
      if (!withTypes) {
        return { total: page.total, offset: page.offset, returned: page.items.length, hasMore: page.hasMore, elements: page.items };
      }
      const missing = page.items.filter((g) => !typeMap.has(normGuid(g)));
      if (missing.length) for (const [k, v] of await typesOf(ac, missing)) typeMap.set(k, v);
      const out: Json = { total: page.total, offset: page.offset, returned: page.items.length, hasMore: page.hasMore };
      if (typeMap.size >= guids.length) out["countsByType"] = countBy(guids.map((g) => typeMap.get(normGuid(g)) ?? "Unknown"));
      out["elements"] = page.items.map((g) => ({ guid: g, type: typeMap.get(normGuid(g)) ?? "Unknown" }));
      return out;
    },
  });

  defineTool(server, ctx, {
    name: "get_element_types",
    title: "Get element types",
    description:
      "Returns the type of each element (Wall, Slab, Door, Zone, ...) — e.g. to sort GUIDs returned by other tools. " +
      "Output: {elements: [{guid, type} | {guid, error}]} in input order ('Unknown' for types the official API does not name, e.g. 2D elements).",
    input: { elements: ElementRefs.max(10000) },
    annotations: READ_ONLY,
    handler: async ({ elements }, { ac }) => {
      const guids = elements.map(refGuid);
      const res = await ac.api<{ typesOfElements?: unknown[] }>("API.GetTypesOfElements", { elements: elementIdItems(guids) });
      const list = res.typesOfElements ?? [];
      return {
        elements: guids.map((g, i) => {
          const u = unwrap<Json>(list[i]);
          return u.ok ? { guid: g, type: u.value["elementType"] } : { guid: g, error: u.error };
        }),
      };
    },
  });

  defineTool(server, ctx, {
    name: "get_bounding_boxes",
    title: "Get bounding boxes",
    description:
      "Returns axis-aligned bounding boxes of elements in project coordinates (meters): 3D boxes {xMin, yMin, zMin, xMax, yMax, zMax} " +
      "(z absolute, from project zero) and/or 2D floor-plan boxes {xMin, yMin, xMax, yMax}. Also returns `overall`, the union of all boxes — " +
      "handy to frame a view, place new elements next to existing ones, or check overlaps. Output: {elements: [{guid, box3D?, box2D?} | " +
      "{guid, error}], overall3D?, overall2D?}.",
    input: {
      elements: ElementRefs.max(10000),
      kind: z.enum(["3D", "2D", "both"]).optional().describe("Which boxes: '3D' (default), '2D' (floor plan) or 'both'"),
    },
    annotations: READ_ONLY,
    handler: async ({ elements, kind }, { ac }) => {
      const guids = elements.map(refGuid);
      const k = kind ?? "3D";
      const want3 = k !== "2D";
      const want2 = k !== "3D";
      const [r3, r2] = await Promise.all([
        want3 ? ac.api<{ boundingBoxes3D?: unknown[] }>("API.Get3DBoundingBoxes", { elements: elementIdItems(guids) }) : undefined,
        want2 ? ac.api<{ boundingBoxes2D?: unknown[] }>("API.Get2DBoundingBoxes", { elements: elementIdItems(guids) }) : undefined,
      ]);
      const ok3: Json[] = [];
      const ok2: Json[] = [];
      const rows = guids.map((g, i) => {
        const row: Json = { guid: g };
        const errors: string[] = [];
        if (r3) {
          const u = unwrap(r3.boundingBoxes3D?.[i]);
          if (u.ok) {
            row["box3D"] = boxOut(u.value);
            ok3.push(u.value as Json);
          } else errors.push(u.error);
        }
        if (r2) {
          const u = unwrap(r2.boundingBoxes2D?.[i]);
          if (u.ok) {
            row["box2D"] = boxOut(u.value);
            ok2.push(u.value as Json);
          } else if (!errors.includes(u.error)) errors.push(u.error);
        }
        if (errors.length) row["error"] = errors.join("; ");
        return row;
      });
      const out: Json = { elements: rows };
      const o3 = unionBoxes(ok3, ["xMin", "yMin", "zMin", "xMax", "yMax", "zMax"]);
      const o2 = unionBoxes(ok2, ["xMin", "yMin", "xMax", "yMax"]);
      if (o3) out["overall3D"] = o3;
      if (o2) out["overall2D"] = o2;
      return out;
    },
  });

  defineTool(server, ctx, {
    name: "get_elements_related_to_zones",
    title: "Elements related to zones",
    description:
      "For each zone (room), returns the elements that bound or belong to it (walls, columns, doors, windows, slabs, objects... as Archicad " +
      "relates them), grouped by type. Output: {zones: [{zone, total, elements: {Wall: [guid...], Door: [...]}} | {zone, error}]}. " +
      "Get zone GUIDs with list_elements {types: ['Zone']}. The passed GUIDs must be zones.",
    input: {
      zones: ElementRefs.max(1000).describe("Zone GUIDs"),
      elementTypes: z
        .array(OfficialElementType)
        .min(1)
        .optional()
        .describe("Only related elements of these types (default: all types)"),
      groupByType: z.boolean().optional().describe("Group the related elements by type (default true); false returns a flat GUID list"),
    },
    annotations: READ_ONLY,
    handler: async ({ zones, elementTypes, groupByType }, { ac }) => {
      const guids = zones.map(refGuid);
      const params: Json = { zones: elementIdItems(guids) };
      if (elementTypes) params["elementTypes"] = elementTypes;
      const res = await ac.api<{ elementsRelatedToZones?: unknown[] }>("API.GetElementsRelatedToZones", params);
      const list = res.elementsRelatedToZones ?? [];
      const perZone = guids.map((g, i): { zone: string; error?: string; related?: string[] } => {
        const u = unwrap<unknown>(list[i]);
        if (!u.ok) {
          const hint = /not the expected/i.test(u.error) ? " — this element is not a zone (get zone GUIDs with list_elements {types: ['Zone']})" : "";
          return { zone: g, error: u.error + hint };
        }
        const arr = Array.isArray(u.value) ? u.value : ((u.value as Json)?.["elements"] as unknown[] | undefined) ?? [];
        return { zone: g, related: guidsOf(arr) };
      });
      const group = groupByType !== false;
      let typeMap = new Map<string, string>();
      if (group) {
        const all = [...new Set(perZone.flatMap((z) => z.related ?? []))];
        typeMap = await typesOf(ac, all);
      }
      return {
        zones: perZone.map((z) => {
          if (!z.related) return { zone: z.zone, error: z.error };
          if (!group) return { zone: z.zone, total: z.related.length, elements: z.related };
          const byType: Record<string, string[]> = {};
          for (const g of z.related) (byType[typeMap.get(normGuid(g)) ?? "Unknown"] ??= []).push(g);
          return { zone: z.zone, total: z.related.length, elements: byType };
        }),
      };
    },
  });

  defineTool(server, ctx, {
    name: "get_element_components",
    title: "Get element components",
    description:
      "Lists the components of elements — the building-material parts that Archicad lists in component schedules (one per skin of a " +
      "composite, per profile part, per basic structure). By default each component comes with a summary: BuildingMaterial_Name, " +
      "Component_Thickness (m), Component_NetVolume / Component_GrossVolume (m³), Component_NetProjectedArea / Component_CrossSectionArea (m²). " +
      "Pass `properties` to read other values instead, or [] for ids only. Output: {elements: [{guid, components: [{component, values: {name: value}, " +
      "unavailable?: {name: status}}]} | {guid, error}]}. Use get_component_property_values to read more properties of specific components.",
    input: {
      elements: ElementRefs.max(500),
      properties: z
        .array(PropertyRef)
        .max(50)
        .optional()
        .describe("Properties to read per component (default: the summary above; [] = component ids only)"),
    },
    annotations: READ_ONLY,
    handler: async ({ elements, properties }, { ac }) => {
      const comps = await componentsOf(ac, elements);
      const pairs: ComponentPair[] = [];
      for (const c of comps) for (const k of c.components ?? []) pairs.push({ element: c.guid, component: k });
      const props = (properties ?? COMPONENT_SUMMARY) as PropertyRefT[];
      const { rows, propErrors } = await componentValues(ac, pairs, props);
      let r = 0;
      const out: Json = {
        elements: comps.map((c) => {
          if (c.error) return { guid: c.guid, error: c.error };
          const list = (c.components ?? []).map(() => {
            const row = { ...rows[r++]! };
            delete row["element"];
            return row;
          });
          return { guid: c.guid, components: list };
        }),
      };
      if (Object.keys(propErrors).length) out["propertyErrors"] = propErrors;
      return out;
    },
  });

  defineTool(server, ctx, {
    name: "get_component_property_values",
    title: "Get component property values",
    description:
      "Reads property values of element components (building-material parts — see get_element_components): built-in quantities such as " +
      "Component_Thickness, Component_NetVolume, Component_NetProjectedArea, BuildingMaterial_Name/ID/Manufacturer/Description, or any " +
      "user-defined property available for components. Lengths m, areas m², volumes m³, angles degrees. Either pass `components` " +
      "[{element, component}] or `elements` (= all their components). Output: {components: [{element, component, values: {property: value}, " +
      "unavailable?: {property: 'notAvailable'|'notEvaluated'|'userUndefined'|error}} | {element, component, error}], propertyErrors?}.",
    input: {
      components: z
        .array(z.object({ element: ElementRef, component: Guid.describe("Component GUID from get_element_components") }))
        .min(1)
        .max(5000)
        .optional()
        .describe("Specific components"),
      elements: ElementRefs.max(500).optional().describe("Read all components of these elements"),
      properties: z.array(PropertyRef).min(1).max(100).describe("Properties to read"),
    },
    annotations: READ_ONLY,
    handler: async ({ components, elements, properties }, { ac }) => {
      if (!components && !elements) throw new Error("Pass 'components' ([{element, component}]) or 'elements' (all their components).");
      const pairs: ComponentPair[] = (components ?? []).map((c) => ({ element: refGuid(c.element), component: normGuid(c.component) }));
      const elementErrors: Json[] = [];
      if (elements) {
        for (const c of await componentsOf(ac, elements)) {
          if (c.error) elementErrors.push({ element: c.guid, error: c.error });
          for (const k of c.components ?? []) pairs.push({ element: c.guid, component: k });
        }
      }
      const { rows, propErrors } = await componentValues(ac, pairs, properties as PropertyRefT[]);
      const out: Json = { components: [...rows, ...elementErrors] };
      if (Object.keys(propErrors).length) out["propertyErrors"] = propErrors;
      return out;
    },
  });

  defineTool(server, ctx, {
    name: "get_property_ids_by_name",
    title: "Find property definitions",
    description:
      "Resolves property names to GUIDs and definitions, or searches the property catalog. Built-in properties have stable non-localized names " +
      "(e.g. 'General_ElementID', 'General_Width', 'Zone_CalculatedArea', 'Component_Thickness'); user-defined ones are 'Group/Name' with " +
      "localized names (e.g. 'ИНФОРМАЦИЯ О ПРОДУКТЕ/Модель'). Pass `properties` to resolve specific names, or browse with `search` " +
      "(case-insensitive substring of built-in name, group or name, any language), `group`, `propertyType` and/or `elements` (only the " +
      "properties those elements have — e.g. which user-defined properties their classification makes available). Returns [{guid, " +
      "builtInName?, kind: 'BuiltIn'|'UserDefined', group, name, type, editable, description?, enumValues?, defaultValue?}]. The GUIDs work " +
      "in every tool taking property references. Works without the Claude Connector add-on.",
    input: {
      properties: z.array(PropertyRef).min(1).max(500).optional().describe("Names/GUIDs to resolve (output in input order, with {input, error} for misses)"),
      search: z.string().min(1).optional().describe("Substring to search in built-in names, group names and property names (e.g. 'Area', 'Площадь', 'Thickness')"),
      group: z.string().min(1).optional().describe("Only properties of this (localized) group, e.g. 'ИНФОРМАЦИЯ О ПРОДУКТЕ' or 'Компоненты'"),
      propertyType: z.enum(["BuiltIn", "UserDefined"]).optional().describe("Browse only built-in or only user-defined properties"),
      elements: ElementRefs.max(1000).optional().describe("Browse only properties available for these elements (see elementMatch)"),
      elementMatch: z
        .enum(["any", "all"])
        .optional()
        .describe("With elements: 'any' (default) = available for at least one of them, 'all' = available for every one of them"),
      includeDetails: z.boolean().optional().describe("Include description, enum values and default value (default: true for `properties`, false for browsing)"),
      ...Pagination,
    },
    annotations: READ_ONLY,
    handler: async ({ properties, search, group, propertyType, elements, elementMatch, includeDetails, offset, limit }, { ac }) => {
      const slim = (d: PropertyDef & { kind?: string }, details: boolean): Json => {
        const o: Json = { guid: d.guid };
        if (d.builtInName) o["builtInName"] = d.builtInName;
        if (d.kind) o["kind"] = d.kind;
        o["group"] = d.group;
        o["name"] = d.name;
        o["type"] = d.type;
        o["editable"] = d.editable;
        if (details) {
          if (d.description) o["description"] = d.description;
          if (d.enumValues) o["enumValues"] = d.enumValues;
          if (d.defaultValue !== undefined) o["defaultValue"] = d.defaultValue;
        }
        return o;
      };
      if (properties) {
        const resolved = await resolveProperties(ac, properties as PropertyRefT[]);
        const guids = resolved.filter((r) => r.guid).map((r) => r.guid!);
        const defs = await getPropertyDefinitions(ac, guids);
        const byGuid = new Map(defs.map((d) => [normGuid(d.guid), d]));
        const userDefined = new Set(
          ((await ac.api<{ propertyIds?: unknown[] }>("API.GetAllPropertyIds", { propertyType: "UserDefined" })).propertyIds ?? [])
            .map((x) => innerGuid(x))
            .filter((g): g is string => !!g)
            .map(normGuid),
        );
        const results: Json[] = [];
        for (const r of resolved) {
          if (!r.guid) {
            results.push({ input: r.input, error: r.error ?? "not found" });
            continue;
          }
          const d = byGuid.get(normGuid(r.guid));
          if (!d || ("error" in d && (d as { error?: string }).error)) {
            results.push({ input: r.input, guid: r.guid, error: (d as { error?: string } | undefined)?.error ?? "definition not found" });
            continue;
          }
          const def = { ...(d as PropertyDef) };
          if (r.builtInName) def.builtInName = r.builtInName;
          results.push({
            input: r.input,
            ...slim(def, includeDetails !== false),
            kind: userDefined.has(normGuid(def.guid)) ? "UserDefined" : "BuiltIn",
          });
        }
        return { properties: results };
      }
      if (elementMatch && !elements) throw new Error("elementMatch needs 'elements'.");
      const defs: Array<PropertyDef & { kind: string }> = [];
      if (propertyType !== "BuiltIn") defs.push(...(await userDefinedCatalog(ac)).map((d) => ({ ...d, kind: "UserDefined" })));
      if (propertyType !== "UserDefined") defs.push(...(await builtInCatalog(ac)).map((d) => ({ ...d, kind: "BuiltIn" })));
      let hits = defs;
      const elementErrors: Json[] = [];
      if (elements) {
        const available = await availablePropertyIds(ac, elements, elementMatch ?? "any", propertyType, elementErrors);
        hits = hits.filter((d) => available.has(normGuid(d.guid)));
      }
      if (group) hits = hits.filter((d) => d.group !== undefined && lc(d.group) === lc(group));
      if (search) {
        const s = lc(search);
        hits = hits.filter(
          (d) =>
            (d.builtInName !== undefined && lc(d.builtInName).includes(s)) ||
            (d.name !== undefined && lc(d.name).includes(s)) ||
            (d.group !== undefined && lc(d.group).includes(s)) ||
            lc(`${d.group ?? ""}/${d.name ?? ""}`).includes(s),
        );
      }
      const page = paginate(hits, offset ?? 0, limit ?? 200);
      const out: Json = { total: page.total, offset: page.offset, returned: page.items.length, hasMore: page.hasMore };
      if (!search && !group) out["groups"] = countBy(hits.map((d) => d.group ?? ""));
      if (elementErrors.length) out["elementErrors"] = elementErrors;
      out["properties"] = page.items.map((d) => slim(d, includeDetails === true));
      return out;
    },
  });
}
