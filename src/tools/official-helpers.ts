/**
 * Helpers for the "official" tool family: thin, friendly wrappers around the built-in Archicad 26
 * JSON API commands (ac.api("API.X")). Everything here talks to the official API only, so these tools
 * work even when the Claude Connector add-on is not loaded.
 *
 * Conventions of the official API that we hide from Claude:
 *  - ids are nested objects ({elementId: {guid}}, {classificationSystemId: {guid}}, ...) -> we accept plain GUID strings;
 *  - GUIDs must be bare and are case-insensitive ("{...}" is rejected) -> normalized with normGuid();
 *  - list results are [{<wrapperKey>: payload} | {error: {code, message}}] -> unwrap() / apiError();
 *  - names of attributes / properties / classification items are LOCALIZED -> resolvers below match them.
 */

import { z } from "zod";

import type { ArchicadClient } from "../archicad/client.js";
import { Guid } from "./schemas.js";

export type Json = Record<string, unknown>;

// ---------------------------------------------------------------------------------------------
// GUIDs, ids, per-item results
// ---------------------------------------------------------------------------------------------

const GUID_RE = /^\{?[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\}?$/;

export function isGuid(s: unknown): boolean {
  return typeof s === "string" && GUID_RE.test(s.trim());
}

/** Bare upper-case GUID (the JSON API rejects braces). */
export function normGuid(g: string): string {
  return g.trim().replace(/[{}]/g, "").toUpperCase();
}

export function gid(g: string): { guid: string } {
  return { guid: normGuid(g) };
}

export type GuidRef = string | { guid: string };

export function refGuid(r: GuidRef): string {
  return normGuid(typeof r === "string" ? r : r.guid);
}

export function elementIdItems(refs: GuidRef[]): { elementId: { guid: string } }[] {
  return refs.map((r) => ({ elementId: gid(refGuid(r)) }));
}

export function navIdItems(ids: string[]): { navigatorItemId: { guid: string } }[] {
  return ids.map((g) => ({ navigatorItemId: gid(g) }));
}

export function attrIdItems(guids: string[]): { attributeId: { guid: string } }[] {
  return guids.map((g) => ({ attributeId: gid(g) }));
}

export function propIdItems(guids: string[]): { propertyId: { guid: string } }[] {
  return guids.map((g) => ({ propertyId: gid(g) }));
}

export function classItemIdItems(guids: string[]): { classificationItemId: { guid: string } }[] {
  return guids.map((g) => ({ classificationItemId: gid(g) }));
}

/** GUID inside {xxxId: {guid}} or {guid}. */
export function innerGuid(v: unknown): string | undefined {
  if (!v || typeof v !== "object") return undefined;
  const o = v as Json;
  if (typeof o["guid"] === "string") return o["guid"] as string;
  for (const val of Object.values(o)) {
    if (val && typeof val === "object" && typeof (val as Json)["guid"] === "string") return (val as Json)["guid"] as string;
  }
  return undefined;
}

export function apiErrorText(e: unknown): string {
  if (!e || typeof e !== "object") return String(e);
  const o = e as { code?: number; message?: string };
  return `${o.message ?? "unknown error"}${o.code !== undefined ? ` (code ${o.code})` : ""}`;
}

/** Splits an official "X or error" list item: returns the payload (the value of its single wrapper key) or {error}. */
export function unwrap<T = Json>(item: unknown): { ok: true; value: T } | { ok: false; error: string } {
  if (!item || typeof item !== "object") return { ok: false, error: "empty result item" };
  const o = item as Json;
  if (o["error"] !== undefined && Object.keys(o).length === 1) return { ok: false, error: apiErrorText(o["error"]) };
  const keys = Object.keys(o);
  if (keys.length === 1) return { ok: true, value: o[keys[0]!] as T };
  return { ok: true, value: o as T };
}

/** ExecutionResult list -> [{ok: true} | {error}] */
export function execResults(results: unknown): Array<{ ok: true } | { error: string }> {
  const list = Array.isArray(results) ? results : [];
  return list.map((r) => {
    const o = (r ?? {}) as { success?: boolean; error?: unknown };
    return o.success ? { ok: true as const } : { error: apiErrorText(o.error) };
  });
}

export function uniq<T>(xs: T[]): T[] {
  return [...new Set(xs)];
}

export function lc(s: string): string {
  return s.trim().toLocaleLowerCase();
}

/** Picks the single best candidate by name: exact, then case-insensitive, then unique substring. */
export function matchByName<T>(items: T[], name: string, getNames: (t: T) => string[]): { match?: T; candidates: T[] } {
  const exact = items.filter((t) => getNames(t).some((n) => n === name));
  if (exact.length >= 1) return { match: exact.length === 1 ? exact[0] : undefined, candidates: exact };
  const ci = items.filter((t) => getNames(t).some((n) => lc(n) === lc(name)));
  if (ci.length >= 1) return { match: ci.length === 1 ? ci[0] : undefined, candidates: ci };
  const sub = items.filter((t) => getNames(t).some((n) => n !== "" && lc(n).includes(lc(name))));
  return { match: sub.length === 1 ? sub[0] : undefined, candidates: sub };
}

export function rgbToHex(c: unknown): string | undefined {
  if (!c || typeof c !== "object") return undefined;
  const o = c as { red?: number; green?: number; blue?: number };
  const h = (v: number | undefined) =>
    Math.max(0, Math.min(255, Math.round((v ?? 0) * 255)))
      .toString(16)
      .padStart(2, "0");
  return `#${h(o.red)}${h(o.green)}${h(o.blue)}`.toUpperCase();
}

export function hexToRgb(hex: string): { red: number; green: number; blue: number } {
  const m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex.trim());
  if (!m) throw new Error(`Invalid color '${hex}': use '#RRGGBB' (e.g. '#FFFFFF') or {red, green, blue} with 0..1 components.`);
  return { red: parseInt(m[1]!, 16) / 255, green: parseInt(m[2]!, 16) / 255, blue: parseInt(m[3]!, 16) / 255 };
}

// ---------------------------------------------------------------------------------------------
// Shared zod schemas
// ---------------------------------------------------------------------------------------------

/** Element types known to the official AC26 JSON API. */
export const OFFICIAL_ELEMENT_TYPES = [
  "Wall",
  "Column",
  "Beam",
  "Window",
  "Door",
  "Object",
  "Lamp",
  "Slab",
  "Roof",
  "Mesh",
  "Zone",
  "CurtainWall",
  "Shell",
  "Skylight",
  "Morph",
  "Stair",
  "Railing",
  "Opening",
] as const;

export const OfficialElementType = z.enum(OFFICIAL_ELEMENT_TYPES).describe("Element type (official JSON API types)");

export const ATTRIBUTE_TYPES = [
  "BuildingMaterial",
  "Composite",
  "Fill",
  "Layer",
  "LayerCombination",
  "Line",
  "PenTable",
  "Profile",
  "Surface",
  "ZoneCategory",
] as const;

export type AttributeType = (typeof ATTRIBUTE_TYPES)[number];

export const OfficialAttributeType = z
  .enum(ATTRIBUTE_TYPES)
  .describe("Attribute type: BuildingMaterial, Composite, Fill, Layer, LayerCombination, Line, PenTable, Profile, Surface or ZoneCategory");

export const NAVIGATOR_ITEM_TYPES = [
  "UndefinedItem",
  "ProjectMapRootItem",
  "StoryItem",
  "SectionItem",
  "ElevationItem",
  "InteriorElevationItem",
  "WorksheetItem",
  "DetailItem",
  "DocumentFrom3DItem",
  "Perspective3DItem",
  "Axonometry3DItem",
  "CameraSetItem",
  "CameraItem",
  "ScheduleItem",
  "ProjectIndexItem",
  "TextListItem",
  "GraphicListItem",
  "InfoItem",
  "HelpItem",
  "FolderItem",
  "LayoutBookRootItem",
  "SubsetItem",
  "LayoutItem",
  "DrawingItem",
  "MasterFolderItem",
  "MasterLayoutItem",
] as const;

export const NavigatorItemType = z.enum(NAVIGATOR_ITEM_TYPES);

export const NavId = Guid.describe("Navigator item id (the 'id' GUID from get_navigator_tree)");

/** Attribute reference accepted by the official-family tools. */
export const OfficialAttrRef = z
  .union([z.string().min(1), z.number().int(), z.object({ guid: Guid }), z.object({ name: z.string().min(1) }), z.object({ index: z.number().int() })])
  .describe(
    "Attribute by GUID, exact (localized) name, or index. Find them with get_attributes or get_attribute_folders. Index lookup needs the Claude Connector add-on.",
  );

export type OfficialAttrRefT = z.infer<typeof OfficialAttrRef>;

export const FolderPath = z
  .union([z.array(z.string().min(1)), z.string()])
  .describe(
    "Attribute folder path: array of folder names from the root (e.g. ['Конструктив', 'Стены']) or a '/'-separated string ('Конструктив/Стены'). [] or '' = root folder.",
  );

export const FolderRef = z
  .union([FolderPath, z.object({ guid: Guid })])
  .describe("Attribute folder: path (array of names or 'A/B' string; [] = root) or {guid} from get_attribute_folders");

export type FolderRefT = z.infer<typeof FolderRef>;

export function folderPath(p: string[] | string): string[] {
  if (Array.isArray(p)) return p;
  return p
    .split("/")
    .map((s) => s.trim())
    .filter((s) => s.length > 0);
}

export function folderJson(type: AttributeType, ref: FolderRefT): Json {
  if (typeof ref === "object" && !Array.isArray(ref)) return { attributeType: type, attributeFolderId: gid(ref.guid) };
  return { attributeType: type, path: folderPath(ref) };
}

export function folderLabel(ref: FolderRefT): string {
  if (typeof ref === "object" && !Array.isArray(ref)) return ref.guid;
  return "/" + folderPath(ref).join("/");
}

// ---------------------------------------------------------------------------------------------
// Attributes (names <-> GUIDs through the official API)
// ---------------------------------------------------------------------------------------------

const ATTR_DETAIL_COMMAND: Record<AttributeType, string> = {
  BuildingMaterial: "API.GetBuildingMaterialAttributes",
  Composite: "API.GetCompositeAttributes",
  Fill: "API.GetFillAttributes",
  Layer: "API.GetLayerAttributes",
  LayerCombination: "API.GetLayerCombinationAttributes",
  Line: "API.GetLineAttributes",
  PenTable: "API.GetPenTableAttributes",
  Profile: "API.GetProfileAttributes",
  Surface: "API.GetSurfaceAttributes",
  ZoneCategory: "API.GetZoneCategoryAttributes",
};

export interface AttrInfo {
  guid: string;
  name?: string;
  error?: string;
  raw?: Json;
}

/** Detailed attribute objects (official format) for GUIDs, in input order. */
export async function getAttributeDetails(ac: ArchicadClient, type: AttributeType, guids: string[]): Promise<AttrInfo[]> {
  if (guids.length === 0) return [];
  const res = await ac.api<{ attributes?: unknown[] }>(ATTR_DETAIL_COMMAND[type], { attributeIds: attrIdItems(guids) });
  const list = res.attributes ?? [];
  return guids.map((g, i) => {
    const u = unwrap<Json>(list[i]);
    if (!u.ok) return { guid: normGuid(g), error: u.error };
    return { guid: normGuid(g), name: u.value["name"] as string | undefined, raw: u.value };
  });
}

export async function listAttributes(ac: ArchicadClient, type: AttributeType): Promise<AttrInfo[]> {
  const res = await ac.api<{ attributeIds?: unknown[] }>("API.GetAttributesByType", { attributeType: type });
  const guids = (res.attributeIds ?? []).map((x) => innerGuid(x)).filter((g): g is string => !!g);
  return getAttributeDetails(ac, type, guids);
}

/** Resolves attribute references (GUID / name / index) to GUIDs. Throws with an actionable message on failure. */
export async function resolveAttributes(
  ac: ArchicadClient,
  type: AttributeType,
  refs: OfficialAttrRefT[],
): Promise<Array<{ guid: string; name?: string }>> {
  let all: AttrInfo[] | undefined;
  const out: Array<{ guid: string; name?: string }> = [];
  for (const ref of refs) {
    const guid =
      typeof ref === "string" && isGuid(ref) ? ref : typeof ref === "object" && "guid" in ref ? ref.guid : undefined;
    if (guid) {
      out.push({ guid: normGuid(guid) });
      continue;
    }
    const index = typeof ref === "number" ? ref : typeof ref === "object" && "index" in ref ? ref.index : undefined;
    if (index !== undefined) {
      out.push(await attributeByIndex(ac, type, index));
      continue;
    }
    const name = typeof ref === "string" ? ref : (ref as { name: string }).name;
    all ??= await listAttributes(ac, type);
    const named = all.filter((a) => a.name !== undefined);
    const exact = named.filter((a) => a.name === name);
    const ci = exact.length ? exact : named.filter((a) => lc(a.name!) === lc(name));
    if (ci.length >= 1) {
      out.push({ guid: ci[0]!.guid, name: ci[0]!.name });
      continue;
    }
    const similar = named
      .filter((a) => lc(a.name!).includes(lc(name)) || lc(name).includes(lc(a.name!)))
      .slice(0, 8)
      .map((a) => `'${a.name}'`);
    throw new Error(
      `${type} attribute '${name}' not found (names are localized).` +
        (similar.length ? ` Similar: ${similar.join(", ")}.` : "") +
        ` List them with get_attributes {type: '${type}'} or get_attribute_folders.`,
    );
  }
  return out;
}

async function attributeByIndex(ac: ArchicadClient, type: AttributeType, index: number): Promise<{ guid: string; name?: string }> {
  try {
    const r = await ac.addon<{ attributes?: Array<{ guid?: string; name?: string }> }>("GetAttributes", { type, attributes: [{ index }] });
    const a = r.attributes?.[0];
    if (a?.guid) return { guid: normGuid(a.guid), name: a.name };
  } catch {
    /* fall through */
  }
  throw new Error(
    `Cannot resolve ${type} attribute index ${index}: the official JSON API identifies attributes by GUID only (index lookup needs the Claude Connector add-on). Pass the attribute's name or GUID instead (see get_attributes / get_attribute_folders).`,
  );
}

// ---------------------------------------------------------------------------------------------
// Properties (names <-> GUIDs)
// ---------------------------------------------------------------------------------------------

export const PropertyRef = z
  .union([
    z.string().min(1),
    z.object({ guid: Guid }),
    z.object({ builtIn: z.string().min(1).describe("Non-localized built-in name, e.g. 'General_ElementID'") }),
    z.object({ group: z.string().min(1), name: z.string().min(1) }).describe("Localized group + property name"),
  ])
  .describe(
    "Property: GUID; built-in non-localized name like 'General_ElementID', 'Component_Thickness', 'BuildingMaterial_Name'; " +
      "or localized 'Group/Name' (e.g. 'ИНФОРМАЦИЯ О ПРОДУКТЕ/Модель', also works for built-ins like 'Компоненты/Толщина Компонента'); " +
      "or {builtIn} / {group, name}. Find names with get_property_ids_by_name {search}.",
  );

export type PropertyRefT = z.infer<typeof PropertyRef>;

export interface PropertyDef {
  guid: string;
  group?: string;
  name?: string;
  builtInName?: string;
  type?: string;
  editable?: boolean;
  description?: string;
  enumValues?: Array<{ displayValue: string; nonLocalizedValue?: string }>;
  defaultValue?: unknown;
}

export function propertyDefFromApi(d: Json, builtInName?: string): PropertyDef {
  const group = (d["group"] as Json | undefined)?.["name"] as string | undefined;
  const out: PropertyDef = {
    guid: innerGuid(d["propertyId"]) ?? "",
    group,
    name: d["name"] as string | undefined,
    type: d["type"] as string | undefined,
    editable: d["isEditable"] as boolean | undefined,
  };
  if (builtInName) out.builtInName = builtInName;
  if (typeof d["description"] === "string" && d["description"]) out.description = d["description"] as string;
  const enums = d["possibleEnumValues"];
  if (Array.isArray(enums) && enums.length) {
    out.enumValues = enums.map((e) => {
      const v = ((e as Json)["enumValue"] ?? e) as Json;
      const r: { displayValue: string; nonLocalizedValue?: string } = { displayValue: String(v["displayValue"] ?? "") };
      if (v["nonLocalizedValue"] !== undefined) r.nonLocalizedValue = v["nonLocalizedValue"] as string;
      return r;
    });
  }
  const dv = d["defaultValue"] as Json | undefined;
  if (dv) {
    if (dv["basicDefaultValue"]) {
      const f = formatPropertyValue(dv["basicDefaultValue"]);
      if (!f.status) out.defaultValue = f.value;
    }
    else if (dv["expressions"]) out.defaultValue = { expressions: dv["expressions"] };
  }
  return out;
}

export async function getPropertyDefinitions(ac: ArchicadClient, guids: string[]): Promise<Array<PropertyDef | { guid: string; error: string }>> {
  if (guids.length === 0) return [];
  const res = await ac.api<{ propertyDefinitions?: unknown[] }>("API.GetDetailsOfProperties", { properties: propIdItems(guids) });
  const list = res.propertyDefinitions ?? [];
  return guids.map((g, i) => {
    const u = unwrap<Json>(list[i]);
    if (!u.ok) return { guid: normGuid(g), error: u.error };
    const def = propertyDefFromApi(u.value);
    if (!def.guid) def.guid = normGuid(g);
    return def;
  });
}

interface BuiltInCatalog {
  port: number | undefined;
  defs: PropertyDef[];
}

const builtInCache = new WeakMap<ArchicadClient, BuiltInCatalog>();

/** All built-in property definitions with their non-localized names (cached per Archicad instance: they never change). */
export async function builtInCatalog(ac: ArchicadClient): Promise<PropertyDef[]> {
  const cached = builtInCache.get(ac);
  if (cached && cached.port === ac.currentPort) return cached.defs;
  const names = (await ac.api<{ properties?: Json[] }>("API.GetAllPropertyNames")).properties ?? [];
  const builtIns = names.filter((n) => n["type"] === "BuiltIn" && typeof n["nonLocalizedName"] === "string");
  const ids = builtIns.length
    ? ((await ac.api<{ properties?: unknown[] }>("API.GetPropertyIds", { properties: builtIns })).properties ?? [])
    : [];
  const pairs: Array<{ guid: string; builtInName: string }> = [];
  builtIns.forEach((n, i) => {
    const u = unwrap(ids[i]);
    const g = u.ok ? innerGuid(u.value) ?? innerGuid(ids[i]) : undefined;
    if (g) pairs.push({ guid: g, builtInName: n["nonLocalizedName"] as string });
  });
  const defs = await getPropertyDefinitions(
    ac,
    pairs.map((p) => p.guid),
  );
  const out: PropertyDef[] = [];
  defs.forEach((d, i) => {
    if ("error" in d && d.error) return;
    out.push({ ...(d as PropertyDef), builtInName: pairs[i]!.builtInName });
  });
  builtInCache.set(ac, { port: ac.currentPort, defs: out });
  return out;
}

/** All user-defined property definitions (never cached: they can be created at any time). */
export async function userDefinedCatalog(ac: ArchicadClient): Promise<PropertyDef[]> {
  const ids = (await ac.api<{ propertyIds?: unknown[] }>("API.GetAllPropertyIds", { propertyType: "UserDefined" })).propertyIds ?? [];
  const guids = ids.map((x) => innerGuid(x)).filter((g): g is string => !!g);
  const defs = await getPropertyDefinitions(ac, guids);
  return defs.filter((d): d is PropertyDef => !("error" in d && (d as { error?: string }).error));
}

export function propertyLabel(ref: PropertyRefT): string {
  if (typeof ref === "string") return ref;
  if ("guid" in ref) return ref.guid;
  if ("builtIn" in ref) return ref.builtIn;
  return `${ref.group}/${ref.name}`;
}

export interface ResolvedProperty {
  input: string;
  guid?: string;
  /** set when the reference was resolved as a built-in property */
  builtInName?: string;
  error?: string;
}

/**
 * Resolves property references to GUIDs. Fast path: API.GetPropertyIds for exact built-in /
 * user-defined names; fallback: localized "Group/Name" match over all definitions.
 * Unresolvable references get an {error} with a hint (never throws for a single bad ref).
 */
export async function resolveProperties(ac: ArchicadClient, refs: PropertyRefT[]): Promise<ResolvedProperty[]> {
  const out: ResolvedProperty[] = refs.map((r) => ({ input: propertyLabel(r) }));
  const queries: Array<{ i: number; q: Json }> = [];
  refs.forEach((ref, i) => {
    if (typeof ref === "string") {
      const s = ref.trim();
      if (isGuid(s)) out[i]!.guid = normGuid(s);
      else if (s.includes("/")) {
        const k = s.indexOf("/");
        queries.push({ i, q: { type: "UserDefined", localizedName: [s.slice(0, k).trim(), s.slice(k + 1).trim()] } });
      } else queries.push({ i, q: { type: "BuiltIn", nonLocalizedName: s } });
    } else if ("guid" in ref) out[i]!.guid = normGuid(ref.guid);
    else if ("builtIn" in ref) queries.push({ i, q: { type: "BuiltIn", nonLocalizedName: ref.builtIn.trim() } });
    else queries.push({ i, q: { type: "UserDefined", localizedName: [ref.group.trim(), ref.name.trim()] } });
  });
  const misses: number[] = [];
  if (queries.length) {
    const res = await ac.api<{ properties?: unknown[] }>("API.GetPropertyIds", { properties: queries.map((q) => q.q) });
    const list = res.properties ?? [];
    queries.forEach(({ i }, k) => {
      const item = list[k];
      const g = item && typeof item === "object" && !(item as Json)["error"] ? innerGuid(item) : undefined;
      if (g) {
        out[i]!.guid = normGuid(g);
        const q = queries[k]!.q;
        if (q["type"] === "BuiltIn") out[i]!.builtInName = q["nonLocalizedName"] as string;
      } else misses.push(i);
    });
  }
  if (misses.length) {
    const defs = [...(await userDefinedCatalog(ac)), ...(await builtInCatalog(ac))];
    for (const i of misses) {
      const ref = refs[i]!;
      const wanted = typeof ref === "string" ? ref.trim() : "builtIn" in ref ? ref.builtIn : "group" in ref ? `${ref.group}/${ref.name}` : "";
      const full = (d: PropertyDef) => `${d.group ?? ""}/${d.name ?? ""}`;
      let hits = defs.filter((d) => lc(full(d)) === lc(wanted) || (d.builtInName !== undefined && lc(d.builtInName) === lc(wanted)));
      if (hits.length === 0 && !wanted.includes("/")) hits = defs.filter((d) => d.name !== undefined && lc(d.name) === lc(wanted));
      if (hits.length === 1 || (hits.length > 1 && new Set(hits.map((h) => h.guid)).size === 1)) {
        out[i]!.guid = normGuid(hits[0]!.guid);
        if (hits[0]!.builtInName) out[i]!.builtInName = hits[0]!.builtInName;
      } else if (hits.length > 1) {
        out[i]!.error = `Property '${wanted}' is ambiguous: ${hits
          .slice(0, 6)
          .map((h) => `'${full(h)}' (${h.guid})`)
          .join(", ")}. Use 'Group/Name' or the GUID.`;
      } else {
        const needle = lc(wanted.split("/").pop() ?? wanted);
        const similar = defs
          .filter((d) => lc(full(d)).includes(needle) || (d.builtInName && lc(d.builtInName).includes(needle)))
          .slice(0, 6)
          .map((d) => `'${d.builtInName ?? full(d)}'`);
        out[i]!.error =
          `Property '${wanted}' not found.` +
          (similar.length ? ` Similar: ${similar.join(", ")}.` : "") +
          " Search with get_property_ids_by_name {search: '...'}.";
      }
    }
  }
  return out;
}

/** Official property value -> plain JSON value (null when not normal) + status. */
export function formatPropertyValue(pv: unknown): { value: unknown; type?: string; status?: string } {
  if (!pv || typeof pv !== "object") return { value: null };
  const o = pv as Json;
  const status = o["status"] as string | undefined;
  const type = o["type"] as string | undefined;
  if (status && status !== "normal") return { value: null, type, status };
  const v = o["value"];
  if (type === "singleEnum") return { value: enumLabel(v), type };
  if (type === "multiEnum") return { value: Array.isArray(v) ? v.map((x) => enumLabel((x as Json)["enumValueId"] ?? x)) : [], type };
  return { value: v, type };
}

function enumLabel(v: unknown): unknown {
  if (!v || typeof v !== "object") return v;
  const o = v as Json;
  return o["displayValue"] ?? o["nonLocalizedValue"] ?? v;
}

// ---------------------------------------------------------------------------------------------
// Classification systems and items
// ---------------------------------------------------------------------------------------------

export interface ClassSystem {
  guid: string;
  name: string;
  description?: string;
  source?: string;
  version?: string;
  date?: string;
}

export interface ClassItem {
  guid: string;
  id: string;
  name: string;
  description: string;
  parent?: string;
  depth: number;
  path: string;
  childCount: number;
  system: string;
}

export interface ClassTreeNode {
  guid: string;
  id: string;
  name?: string;
  description?: string;
  children?: ClassTreeNode[];
  childCount?: number;
}

export async function getClassificationSystems(ac: ArchicadClient): Promise<ClassSystem[]> {
  const res = await ac.api<{ classificationSystems?: Json[] }>("API.GetAllClassificationSystems");
  return (res.classificationSystems ?? []).map((s) => {
    const out: ClassSystem = { guid: innerGuid(s["classificationSystemId"]) ?? "", name: String(s["name"] ?? "") };
    for (const k of ["description", "source", "version", "date"] as const) if (s[k]) out[k] = String(s[k]);
    return out;
  });
}

export class ClassificationContext {
  private systems?: ClassSystem[];
  private readonly trees = new Map<string, { roots: Json[]; items: ClassItem[] }>();

  constructor(private readonly ac: ArchicadClient) {}

  async allSystems(): Promise<ClassSystem[]> {
    this.systems ??= await getClassificationSystems(this.ac);
    return this.systems;
  }

  /** Resolves a system by GUID or name; undefined = the only system (error if several). */
  async system(ref?: string): Promise<ClassSystem> {
    const systems = await this.allSystems();
    const list = () => systems.map((s) => `'${s.name}' (${s.guid})`).join(", ");
    if (systems.length === 0) throw new Error("The project has no classification systems (import one in Options > Classification Manager).");
    if (ref === undefined || ref.trim() === "") {
      if (systems.length === 1) return systems[0]!;
      throw new Error(`Several classification systems exist — pass 'system' (name or GUID): ${list()}.`);
    }
    if (isGuid(ref)) {
      const g = normGuid(ref);
      const s = systems.find((x) => normGuid(x.guid) === g);
      if (!s) throw new Error(`Classification system ${ref} not found. Available: ${list()}.`);
      return s;
    }
    const { match, candidates } = matchByName(systems, ref, (s) => [s.name, `${s.name} ${s.version ?? ""}`.trim()]);
    if (match) return match;
    if (candidates.length > 1) throw new Error(`Classification system '${ref}' is ambiguous: ${candidates.map((s) => `'${s.name}' (${s.guid})`).join(", ")}. Pass the GUID.`);
    throw new Error(`Classification system '${ref}' not found. Available: ${list()}.`);
  }

  async tree(systemGuid: string): Promise<{ roots: Json[]; items: ClassItem[] }> {
    const key = normGuid(systemGuid);
    const cached = this.trees.get(key);
    if (cached) return cached;
    const res = await this.ac.api<{ classificationItems?: Json[] }>("API.GetAllClassificationsInSystem", { classificationSystemId: gid(key) });
    const roots = res.classificationItems ?? [];
    const items: ClassItem[] = [];
    const walk = (nodes: Json[], parent: ClassItem | undefined, depth: number) => {
      for (const n of nodes) {
        const ci = (n["classificationItem"] ?? n) as Json;
        const kids = (ci["children"] as Json[] | undefined) ?? [];
        const id = String(ci["id"] ?? "");
        const item: ClassItem = {
          guid: innerGuid(ci["classificationItemId"]) ?? "",
          id,
          name: String(ci["name"] ?? ""),
          description: String(ci["description"] ?? ""),
          depth,
          path: parent ? `${parent.path} > ${id}` : id,
          childCount: kids.length,
          system: key,
        };
        if (parent) item.parent = parent.guid;
        items.push(item);
        walk(kids, item, depth + 1);
      }
    };
    walk(roots, undefined, 0);
    const t = { roots, items };
    this.trees.set(key, t);
    return t;
  }

  /** Finds the item with this GUID in any system (loads trees as needed). */
  async findItemByGuid(guid: string, systemRef?: string): Promise<ClassItem | undefined> {
    const g = normGuid(guid);
    const systems = systemRef ? [await this.system(systemRef)] : await this.allSystems();
    for (const s of systems) {
      const hit = (await this.tree(s.guid)).items.find((i) => normGuid(i.guid) === g);
      if (hit) return hit;
    }
    return undefined;
  }

  /**
   * Resolves a classification item reference: GUID, item id ('Стена', 'Ss_25_10'), name, or a path
   * 'Parent > Child'. Searches the given system, or every system when omitted.
   */
  async item(ref: string | { guid: string }, systemRef?: string): Promise<ClassItem> {
    const raw = typeof ref === "string" ? ref.trim() : ref.guid;
    if (isGuid(raw)) {
      const hit = await this.findItemByGuid(raw, systemRef);
      if (!hit) throw new Error(`Classification item ${raw} not found${systemRef ? ` in system '${systemRef}'` : ""}. Browse items with get_classification_tree.`);
      return hit;
    }
    const systems = systemRef ? [await this.system(systemRef)] : await this.allSystems();
    const all: ClassItem[] = [];
    for (const s of systems) all.push(...(await this.tree(s.guid)).items);
    const want = lc(raw);
    const norm = (p: string) => lc(p.replace(/\s*>\s*/g, " > "));
    let hits = all.filter((i) => lc(i.id) === want);
    if (hits.length === 0) hits = all.filter((i) => norm(i.path) === norm(raw) || norm(i.path).endsWith(" > " + norm(raw)));
    if (hits.length === 0) hits = all.filter((i) => i.name !== "" && lc(i.name) === want);
    if (hits.length === 0) hits = all.filter((i) => i.name !== "" && lc(`${i.id} ${i.name}`) === want);
    if (hits.length === 1) return hits[0]!;
    if (hits.length > 1) {
      throw new Error(
        `Classification item '${raw}' is ambiguous: ${hits
          .slice(0, 8)
          .map((h) => `'${h.path}' (${h.guid})`)
          .join(", ")}. Pass the GUID, the full path 'A > B > C', or 'system'.`,
      );
    }
    const similar = all
      .filter((i) => lc(i.id).includes(want) || (i.name && lc(i.name).includes(want)))
      .slice(0, 8)
      .map((i) => `'${i.path}'`);
    throw new Error(
      `Classification item '${raw}' not found (ids are localized).` +
        (similar.length ? ` Similar: ${similar.join(", ")}.` : "") +
        " Browse with get_classification_tree {search: '...'}.",
    );
  }
}

export function itemSummary(i: ClassItem): Json {
  const o: Json = { guid: i.guid, id: i.id };
  if (i.name) o["name"] = i.name;
  o["path"] = i.path;
  return o;
}

// ---------------------------------------------------------------------------------------------
// Navigator
// ---------------------------------------------------------------------------------------------

export interface NavNode {
  id: string;
  type: string;
  prefix?: string;
  name: string;
  sourceId?: string;
  children?: NavNode[];
  childCount?: number;
}

export interface NavFlat {
  id: string;
  type: string;
  prefix?: string;
  name: string;
  sourceId?: string;
  parentId?: string;
  depth: number;
  path: string;
  childCount: number;
}

export function rawNavChildren(raw: Json): Json[] {
  return ((raw["children"] as Json[] | undefined) ?? []).map((c) => ((c as Json)["navigatorItem"] ?? c) as Json);
}

function navLabel(raw: Json): string {
  const prefix = String(raw["prefix"] ?? "").trim();
  const name = String(raw["name"] ?? "");
  return prefix && prefix !== name ? `${prefix} ${name}` : name;
}

export function navNode(raw: Json, depth: number, maxDepth: number | undefined): NavNode {
  const n: NavNode = { id: innerGuid(raw["navigatorItemId"]) ?? "", type: String(raw["type"] ?? "") } as NavNode;
  const prefix = String(raw["prefix"] ?? "");
  if (prefix.trim()) n.prefix = prefix;
  n.name = String(raw["name"] ?? "");
  const src = innerGuid(raw["sourceNavigatorItemId"]);
  if (src) n.sourceId = src;
  const kids = rawNavChildren(raw);
  if (kids.length) {
    if (maxDepth !== undefined && depth >= maxDepth) n.childCount = kids.length;
    else n.children = kids.map((k) => navNode(k, depth + 1, maxDepth));
  }
  return n;
}

export function flattenNav(root: Json, maxDepth?: number): NavFlat[] {
  const out: NavFlat[] = [];
  const walk = (raw: Json, parent: NavFlat | undefined, depth: number) => {
    const kids = rawNavChildren(raw);
    const label = navLabel(raw);
    const f = { id: innerGuid(raw["navigatorItemId"]) ?? "", type: String(raw["type"] ?? "") } as NavFlat;
    const prefix = String(raw["prefix"] ?? "");
    if (prefix.trim()) f.prefix = prefix;
    f.name = String(raw["name"] ?? "");
    f.path = parent ? (parent.path ? `${parent.path} / ${label}` : label) : label;
    f.depth = depth;
    f.childCount = kids.length;
    if (parent) f.parentId = parent.id;
    const src = innerGuid(raw["sourceNavigatorItemId"]);
    if (src) f.sourceId = src;
    out.push(f);
    if (maxDepth === undefined || depth < maxDepth) for (const k of kids) walk(k, f, depth + 1);
  };
  walk(root, undefined, 0);
  return out;
}

export function findNavRaw(root: Json, guid: string): Json | undefined {
  const g = normGuid(guid);
  const stack: Json[] = [root];
  while (stack.length) {
    const n = stack.pop()!;
    if (normGuid(innerGuid(n["navigatorItemId"]) ?? "") === g) return n;
    stack.push(...rawNavChildren(n));
  }
  return undefined;
}

export type TreeKind = "ProjectMap" | "ViewMap" | "LayoutBook" | "MyViewMap" | "PublisherSet";

export async function getNavigatorRoot(ac: ArchicadClient, tree: TreeKind, publisherSet?: string): Promise<Json> {
  let navigatorTreeId: Json;
  if (tree === "PublisherSet") {
    if (!publisherSet) throw new Error("tree 'PublisherSet' needs 'publisherSet' (a name from get_publisher_sets).");
    navigatorTreeId = { type: "PublisherSets", name: publisherSet };
  } else navigatorTreeId = { type: tree };
  let res: { navigatorTree?: { rootItem?: Json } };
  try {
    res = await ac.api<{ navigatorTree?: { rootItem?: Json } }>("API.GetNavigatorItemTree", { navigatorTreeId });
  } catch (e) {
    const msg = e instanceof Error ? e.message : String(e);
    if (tree === "MyViewMap" && /teamwork/i.test(msg)) throw new Error(`${msg}. 'MyViewMap' exists only in Teamwork projects; use 'ViewMap'.`);
    if (tree === "PublisherSet") throw new Error(`${msg}. Check the publisher set name with get_publisher_sets.`);
    throw e;
  }
  const root = res.navigatorTree?.rootItem;
  if (!root) throw new Error(`Navigator tree '${tree}' returned no root item.`);
  return root;
}

export async function findNavItemIn(
  ac: ArchicadClient,
  tree: TreeKind,
  ref: string,
  accept: (f: NavFlat) => boolean,
  what: string,
): Promise<NavFlat> {
  const flat = flattenNav(await getNavigatorRoot(ac, tree));
  const candidates = flat.filter(accept);
  if (isGuid(ref)) {
    const g = normGuid(ref);
    const hit = flat.find((f) => normGuid(f.id) === g || (f.sourceId !== undefined && normGuid(f.sourceId) === g));
    if (!hit) throw new Error(`${what} ${ref} not found in the ${tree}. Use get_navigator_tree {tree: '${tree}'} to find ids.`);
    return hit;
  }
  const { match, candidates: c } = matchByName(candidates, ref, (f) => [f.name, `${f.prefix ?? ""} ${f.name}`.trim(), f.path]);
  if (match) return match;
  if (c.length > 1) throw new Error(`${what} '${ref}' is ambiguous: ${c.slice(0, 8).map((f) => `'${f.path}' (${f.id})`).join(", ")}. Pass the id.`);
  throw new Error(
    `${what} '${ref}' not found. Available: ${candidates
      .slice(0, 25)
      .map((f) => `'${f.name}'`)
      .join(", ")}${candidates.length > 25 ? ", ..." : ""}. Use get_navigator_tree {tree: '${tree}'} to see all.`,
  );
}
