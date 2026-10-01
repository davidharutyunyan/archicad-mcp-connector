/**
 * Shared zod schemas. Units everywhere: meters and DEGREES.
 * Every schema carries a .describe() because the descriptions are what Claude reads.
 */

import { z } from "zod";

export const Guid = z
  .string()
  .regex(/^\{?[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\}?$/, "must be a GUID like 1A2B3C4D-....")
  .describe("Element/attribute GUID");

/** Accepts "GUID" or {guid: "GUID"} (official JSON API ElementId form). */
export const ElementRef = z
  .union([Guid, z.object({ guid: Guid })])
  .describe('Element GUID string, or {"guid": "..."}');

export const ElementRefs = z.array(ElementRef).min(1).describe("Element GUIDs");

export function guidOf(ref: z.infer<typeof ElementRef>): string {
  return typeof ref === "string" ? ref : ref.guid;
}

export function toElementIds(refs: z.infer<typeof ElementRef>[]): { elementId: { guid: string } }[] {
  return refs.map((r) => ({ elementId: { guid: guidOf(r) } }));
}

export const Point2D = z.object({ x: z.number(), y: z.number() }).describe("2D point in meters (project coordinates)");
export const Point3D = z
  .object({ x: z.number(), y: z.number(), z: z.number() })
  .describe("3D point in meters (z is absolute unless stated otherwise)");
export const PointMaybeZ = z
  .object({ x: z.number(), y: z.number(), z: z.number().optional() })
  .describe("Point in meters; z optional (used by meshes)");

export const ArcSpec = z
  .object({
    index: z.number().int().min(0).describe("Edge from points[index] to points[index+1] (wrapping) is curved"),
    angle: z.number().describe("Central angle in degrees; positive = counter-clockwise arc"),
  })
  .describe("Curved polygon edge");

export const Contour = z.object({
  points: z.array(PointMaybeZ).min(3).describe("Vertices in order, do NOT repeat the first point at the end"),
  arcs: z.array(ArcSpec).optional().describe("Curved edges"),
});

export const Polygon = z
  .union([
    z.array(PointMaybeZ).min(3),
    Contour.extend({
      holes: z.array(Contour).optional().describe("Inner holes, each {points, arcs?}"),
    }),
  ])
  .describe("Polygon: array of points, or {points, arcs?, holes?: [{points, arcs?}]}. Orientation is normalized automatically.");

export const Polyline = z
  .union([
    z.array(Point2D).min(2),
    z.object({ points: z.array(Point2D).min(2), arcs: z.array(ArcSpec).optional() }),
  ])
  .describe("Open polyline: array of points, or {points, arcs?}");

/** Attribute reference: index, name, or {index}|{name}|{guid}. */
export const AttrRef = z
  .union([
    z.number().int(),
    z.string().min(1),
    z.object({ index: z.number().int() }),
    z.object({ name: z.string() }),
    z.object({ guid: Guid }),
  ])
  .describe("Attribute by name (exact, localized — list with get_attributes), index, or {guid}");

export const PenIndex = z.number().int().min(1).max(255).describe("Pen index 1-255");

export const StoryRef = z
  .union([
    z.number().int(),
    z.string(),
    z.object({ name: z.string() }),
    z.object({ index: z.number().int() }),
    z.object({ floorId: z.number().int().describe("Stable story id from get_stories") }),
    z.object({ displayNumber: z.number().int().describe("Story number as shown in the Navigator (see get_stories)") }),
  ])
  .describe(
    "Story index (0 = ground floor, negative = basements), story name (localized), {floorId} (stable id) or {displayNumber} " +
      "(the number shown in the Navigator, which can differ from the index) — see get_stories",
  );

export const LibPartRef = z
  .union([z.string().min(1), z.number().int(), z.object({ name: z.string() }), z.object({ guid: z.string() }), z.object({ index: z.number().int() })])
  .describe("Library part name (localized! use search_library_parts), index, or {guid: '{MAIN}-{REV}'}");

export const RenovationStatus = z.enum(["Existing", "New", "Demolished", "Default"]).describe("Renovation status");

/** Fields every element create/modify spec accepts (handled by the add-on core). */
export const CommonElementFields = {
  layer: AttrRef.optional().describe("Layer name or index (defaults to the tool's default layer)"),
  storyIndex: StoryRef.optional().describe("Home story (defaults to the current story)"),
  renovationStatus: RenovationStatus.optional(),
  elementId: z.string().optional().describe("Element ID text shown in the Info Box / schedules"),
};

/** Common fields of 2D drafting / annotation elements: Archicad has no Element ID for lines, polylines, arcs,
 * circles, splines, texts, labels, hotspots, pictures and dimensions (hatches have one: use CommonElementFields). */
const { elementId: _elementId, ...DraftingCommonFieldsBase } = CommonElementFields;
export const DraftingCommonFields = DraftingCommonFieldsBase;

export const GdlParams = z
  .record(z.union([z.number(), z.string(), z.boolean(), z.array(z.number()), z.array(z.object({ values: z.array(z.number()) }))]))
  .describe("GDL parameter values by parameter name: lengths in meters, angles in degrees, booleans as true/false");

export const Pagination = {
  offset: z.number().int().min(0).optional().describe("Skip this many results"),
  limit: z.number().int().min(1).max(5000).optional().describe("Return at most this many results (default 500)"),
};

export function paginate<T>(items: T[], offset = 0, limit = 500): { items: T[]; total: number; offset: number; hasMore: boolean } {
  const slice = items.slice(offset, offset + limit);
  return { items: slice, total: items.length, offset, hasMore: offset + slice.length < items.length };
}
