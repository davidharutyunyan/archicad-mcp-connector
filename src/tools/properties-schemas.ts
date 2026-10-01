/**
 * Zod schemas and helpers for the "properties" tool family (property definitions / values,
 * attribute property values, IFC data, classification authoring).
 *
 * Property references accepted everywhere (the add-on resolves the first three, the MCP server
 * resolves {builtIn} through the official API.GetPropertyIds before calling the add-on):
 *   "GUID" | "Group/Name" (localized) | "Name" (when unique) | {guid} | {group, name} | {builtIn: "General_ElementID"}
 */

import { z } from "zod";

import type { ArchicadClient } from "../archicad/client.js";
import { AttrRef, ElementRef, Guid, guidOf } from "./schemas.js";

// --- Property references ------------------------------------------------------------------

export const PropertyRef = z
  .union([
    z.string().min(1),
    z.object({ guid: Guid }),
    z.object({
      group: z
        .string()
        .min(1)
        .optional()
        .describe("Localized property group name, e.g. 'ИНФОРМАЦИЯ О ПРОДУКТЕ' or a custom group (omit when the name is unique)"),
      name: z.string().min(1).describe("Localized property name (inside the group)"),
    }),
    z.object({
      builtIn: z
        .string()
        .min(1)
        .describe("Language-independent built-in property name, e.g. General_ElementID, General_Area, General_Height, General_Thickness"),
    }),
  ])
  .describe(
    "Property: definition GUID, localized \"Group/Name\" (e.g. \"ИНФОРМАЦИЯ О ПРОДУКТЕ/Модель\"), a unique name alone, {guid}, " +
      "{group?, name}, or {builtIn: 'General_ElementID'} (built-in properties, language independent — preferred for built-ins). " +
      "Matching is exact, then case-insensitive; ambiguous names fail with the candidates. Find names with get_property_definitions {search}.",
  );
export type PropertyRefValue = z.infer<typeof PropertyRef>;

const Scalar = z.union([z.string(), z.number(), z.boolean()]);

export const PropertyValue = z
  .union([Scalar, z.array(Scalar)])
  .describe(
    "Typed value matching the property type: string; integer; number; length in m; area in m²; volume in m³; angle in DEGREES; " +
      "boolean (true/false); list types take an array; singleEnum takes one option display value (e.g. 'REI 60'); multiEnum an array " +
      "of option display values. Numeric strings like '2.5' are accepted for numeric types.",
  );

export const ScalarType = z.enum(["string", "integer", "number", "length", "area", "volume", "angle", "boolean"]);

export const PropertyType = z
  .enum([
    "string",
    "integer",
    "number",
    "length",
    "area",
    "volume",
    "angle",
    "boolean",
    "stringList",
    "integerList",
    "numberList",
    "lengthList",
    "areaList",
    "volumeList",
    "angleList",
    "booleanList",
    "singleEnum",
    "multiEnum",
  ])
  .describe(
    "Value type: string | integer | number (plain real) | length (m) | area (m²) | volume (m³) | angle (degrees) | boolean; " +
      "<type>List = list of values (e.g. stringList); singleEnum = option set with one choice; multiEnum = option set with multiple " +
      "choices (options in enumValues). The type cannot be changed later.",
  );

export const EnumOption = z
  .union([
    z.string().min(1),
    z.number(),
    z.object({
      value: Scalar.describe("Display value of the option"),
      nonLocalizedValue: z.string().optional().describe("Optional language-independent key of the option (used by IFC mapping/expressions)"),
    }),
  ])
  .describe("Option of an option set: its display value, or {value, nonLocalizedValue?}");

export const PropertyGroupRef = z
  .union([z.string().min(1), z.object({ guid: Guid }), z.object({ name: z.string().min(1) })])
  .describe("Property group: localized name or GUID (see get_property_definitions {includeGroups: true})");

// --- Classification references -------------------------------------------------------------

export const ClassificationSystemRef = z
  .union([
    z.string().min(1),
    z.object({ guid: Guid }),
    z.object({ name: z.string().min(1), editionVersion: z.string().optional().describe("Needed when several versions share the name") }),
  ])
  .describe("Classification system: name (e.g. 'Классификация Archicad'), 'Name version', GUID, or {name, editionVersion}");

export const ClassificationItemRef = z
  .union([
    z.string().min(1),
    z.object({ guid: Guid }),
    z.object({ system: ClassificationSystemRef, id: z.string().min(1) }),
    z.object({ id: z.string().min(1) }),
  ])
  .describe(
    "Classification item: GUID, item ID (localized, e.g. 'Стена'; must be unique across systems unless a system is given), " +
      "{system, id} or {id}",
  );

export const AvailabilityItemRef = z
  .union([...ClassificationItemRef.options, z.object({ system: ClassificationSystemRef }).describe("Every item of this system")])
  .describe("Classification item (GUID | item ID | {system, id}) or {system} = every item of that system");

export const Availability = z
  .union([z.enum(["all", "none"]), z.array(AvailabilityItemRef).min(1)])
  .describe(
    "Classifications the property is available for (custom properties show only on elements classified with one of them): " +
      "'all' = every item of every classification system, 'none', or a list of item refs / {system}.",
  );

// --- Classification item tree ------------------------------------------------------------------

const ItemFields = {
  id: z.string().min(1).describe("Item ID (unique within the system, shown in the Classification Manager), e.g. 'CC-01'"),
  name: z.string().optional().describe("Item name"),
  description: z.string().optional(),
  parent: ClassificationItemRef.optional().describe(
    "Parent item (GUID or ID in this system, may be an item created earlier in the same call); default: the enclosing item or the root",
  ),
  before: ClassificationItemRef.optional().describe("Insert before this sibling item (default: append at the end)"),
};

function itemSpec(depth: number): z.ZodTypeAny {
  const children =
    depth > 0
      ? z.array(itemSpec(depth - 1))
      : z.array(z.object({ id: z.string() }).passthrough()).describe("Deeper levels: same shape (id, name, description, children)");
  return z.object({ ...ItemFields, children: children.optional().describe("Child items (same shape, recursively)") });
}

export const ClassificationItemSpec = itemSpec(4).describe(
  "Classification item to create: {id, name?, description?, parent?, before?, children?: [...]}",
);

// --- Targets -------------------------------------------------------------------------------------

export const ElementTypeName = z
  .string()
  .min(1)
  .describe("Element type name as used by the add-on: Wall, Column, Beam, Slab, Roof, Shell, Mesh, Zone, Window, Door, Skylight, Object, Lamp, Stair, Railing, CurtainWall, Morph, ...");

export const AttributeTarget = z
  .object({
    type: z
      .string()
      .optional()
      .describe("Attribute type: BuildingMaterial (default), Composite, Surface, Profile, Layer, ... (properties are mainly used on building materials)"),
    attribute: AttrRef,
  })
  .describe("Attribute: {type, attribute: name | index | {guid}}");

export function elementGuids(refs: z.infer<typeof ElementRef>[] | undefined): string[] | undefined {
  return refs?.map(guidOf);
}

// --- Built-in property name resolution (official API) -----------------------------------------------

interface PropertyIdResult {
  properties?: Array<{ propertyId?: { guid: string }; error?: { code?: number; message?: string } }>;
}

function isBuiltInRef<T>(ref: T): ref is T & { builtIn: string } {
  return typeof ref === "object" && ref !== null && typeof (ref as { builtIn?: unknown }).builtIn === "string";
}

/** Resolves {builtIn: "..."} references to {guid} with one API.GetPropertyIds call; other references pass through. */
export async function resolveBuiltInRefs<T>(ac: ArchicadClient, refs: T[]): Promise<Array<T | { guid: string }>> {
  const names = [...new Set(refs.filter(isBuiltInRef).map((r) => r.builtIn))];
  if (names.length === 0) return refs;
  const result = await ac.api<PropertyIdResult>("API.GetPropertyIds", {
    properties: names.map((nonLocalizedName) => ({ type: "BuiltIn", nonLocalizedName })),
  });
  const map = new Map<string, string>();
  const unknown: string[] = [];
  names.forEach((name, i) => {
    const guid = result.properties?.[i]?.propertyId?.guid;
    if (guid) map.set(name, guid);
    else unknown.push(name);
  });
  if (unknown.length > 0) {
    throw new Error(
      `Unknown built-in property name(s): ${unknown.join(", ")}. Built-in names look like General_ElementID, General_Area, ` +
        "General_Height, General_Width, General_Thickness, Geometry_...; get_property_definitions {kind: 'BuiltIn'} lists them (builtInName field).",
    );
  }
  return refs.map((r) => (isBuiltInRef(r) ? { guid: map.get(r.builtIn)! } : r));
}

export async function resolveBuiltInRef<T>(ac: ArchicadClient, ref: T): Promise<T | { guid: string }> {
  const [resolved] = await resolveBuiltInRefs(ac, [ref]);
  return resolved!;
}

const builtInNameCache = new WeakMap<ArchicadClient, Map<string, string>>();

/** GUID (upper case) -> non-localized built-in name, from API.GetAllPropertyNames + API.GetPropertyIds (cached per client). */
export async function builtInNamesByGuid(ac: ArchicadClient): Promise<Map<string, string>> {
  const cached = builtInNameCache.get(ac);
  if (cached) return cached;
  const all = await ac.api<{ properties?: Array<{ type?: string; nonLocalizedName?: string }> }>("API.GetAllPropertyNames");
  const names = (all.properties ?? []).filter((p) => p.type === "BuiltIn" && p.nonLocalizedName).map((p) => p.nonLocalizedName!);
  const map = new Map<string, string>();
  if (names.length > 0) {
    const ids = await ac.api<PropertyIdResult>("API.GetPropertyIds", {
      properties: names.map((nonLocalizedName) => ({ type: "BuiltIn", nonLocalizedName })),
    });
    names.forEach((name, i) => {
      const guid = ids.properties?.[i]?.propertyId?.guid;
      if (guid) map.set(guid.toUpperCase(), name);
    });
  }
  builtInNameCache.set(ac, map);
  return map;
}

const BUILT_IN_TOKEN = /\{\s*builtIn\s*:\s*([A-Za-z0-9_]+)\s*\}/g;

/**
 * Replaces {builtIn:General_Area} tokens in property expressions with {ref:GUID} (resolved with one
 * API.GetPropertyIds call); the add-on then turns {ref:...} into Archicad's own reference syntax.
 */
export async function expandBuiltInExpressionTokens(ac: ArchicadClient, expressions: string[]): Promise<string[]> {
  const names = [...new Set(expressions.flatMap((e) => [...e.matchAll(BUILT_IN_TOKEN)].map((m) => m[1]!)))];
  if (names.length === 0) return expressions;
  const refs = await resolveBuiltInRefs(ac, names.map((builtIn) => ({ builtIn })));
  const guids = new Map(names.map((n, i) => [n, (refs[i] as { guid: string }).guid]));
  return expressions.map((e) => e.replace(BUILT_IN_TOKEN, (_m, name: string) => `{ref:${guids.get(name)}}`));
}

/** Throws one aggregated "Invalid input" error when problems were collected. */
export function failOnProblems(problems: string[]): void {
  if (problems.length > 0) throw new Error(`Invalid input: ${problems.join("; ")}`);
}

/** Removes undefined fields (keeps the request payload minimal and predictable). */
export function compact<T extends Record<string, unknown>>(obj: T): Partial<T> {
  const out: Record<string, unknown> = {};
  for (const [k, v] of Object.entries(obj)) if (v !== undefined) out[k] = v;
  return out as Partial<T>;
}
