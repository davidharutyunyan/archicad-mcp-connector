/**
 * Zone tools (rooms / spaces). The add-on adapter lives in addon/Src/Commands/Zones.cpp; field names match 1:1.
 *
 *   create_zones  — manual zones from a polygon, or automatic zones from a reference point inside walls
 *   modify_zones  — change name/number/category/heights/polygon/stamp/fill of existing zones
 *   update_zones  — re-detect the boundaries of automatic zones after walls changed ("Update Zones")
 *   get_zones     — zone schedule: names, numbers, areas, volumes, totals (optionally polygons & relations)
 */

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import type { ArchicadClient } from "../archicad/client.js";
import { CREATES, defineTool, MODIFIES, READ_ONLY, type ToolContext } from "./define.js";
import { createElements } from "./elements.js";
import { AttrRef, CommonElementFields, ElementRef, GdlParams, Guid, guidOf, LibPartRef, Point2D, Polygon, toElementIds } from "./schemas.js";

const Pen = z.number().int().min(1).max(255);

/** Zone number: text (e.g. "1.01", "B-12") or a plain number (sent as text). */
const ZoneNumber = z
  .union([z.string().max(31), z.number()])
  .describe("Zone number shown in the stamp (roomNoStr, max 31 characters), e.g. '101', '1.05', 'B-12'");

/** Fields shared by create_zones and modify_zones (everything except the geometry mode). */
const ZoneSettingsFields = {
  name: z.string().max(255).optional().describe("Zone name shown in the stamp, e.g. 'Кухня', 'Living room'"),
  number: ZoneNumber.optional(),
  category: AttrRef.optional().describe(
    "Zone category attribute (sets the stamp color / schedule category). Names are LOCALIZED (Russian Archicad) — list them with get_attributes (type ZoneCategory)",
  ),
  boundary: z
    .enum(["InnerEdge", "ReferenceLine"])
    .optional()
    .describe(
      "Automatic zones only (default: the Zone tool setting): 'InnerEdge' = outline follows the inner faces of the surrounding walls (net room), " +
        "'ReferenceLine' = gross room measured to the wall reference lines (read back as referenceLinePolygon; 'polygon' stays the inner-edge outline)",
    ),
  height: z
    .number()
    .positive()
    .optional()
    .describe("Zone (room) height in m. Giving a height unlinks the zone top from a story unless topLinkedStory is also given"),
  topLinkedStory: z
    .number()
    .int()
    .min(0)
    .optional()
    .describe("Link the zone top to the story N levels above the home story (0 = not linked: 'height' is used)"),
  topOffset: z.number().optional().describe("Offset of the top from the linked story (m), only with topLinkedStory > 0"),
  bottomOffset: z.number().optional().describe("Zone bottom relative to the home story level (m, default from the Zone tool, usually 0)"),
  floorThickness: z.number().min(0).optional().describe("Sub-floor thickness (m) used by Archicad's zone calculations"),
  areaReduction: z.number().min(0).max(100).optional().describe("Reduce the calculated zone area by this percentage (0-100)"),
  showFoundPolygon: z
    .boolean()
    .optional()
    .describe("Advanced (ReferenceLine zones only): Archicad's show_found_poly flag — draw the reference-line polygon instead of the inner-edge outline"),

  stampPosition: Point2D.optional().describe(
    "Zone stamp (label) position in m. Default on create: an interior point of the polygon (manual zones) or the referencePoint (automatic zones)",
  ),
  stampAngle: z.number().optional().describe("Zone stamp rotation in degrees"),
  fixedStampAngle: z.boolean().optional().describe("Keep the stamp angle fixed when the zone is rotated"),
  stampPen: Pen.optional().describe("Pen (1-255) of the zone stamp"),
  useStampPens: z.boolean().optional().describe("Archicad's useStampPens flag (single stamp pen vs. the stamp library part's own pens)"),
  stamp: LibPartRef.optional().describe(
    "Zone stamp library part (a library part of type Zone; names are localized — find them with search_library_parts). " +
      "Changing the stamp resets its parameters to the new part's defaults (plus stampParameters)",
  ),
  stampParameters: GdlParams.optional().describe(
    "GDL parameters of the zone stamp {name: value} (lengths in m, angles in degrees). Read the names from an existing zone with get_gdl_parameters",
  ),

  surface: AttrRef.optional().describe("Surface (material) of the zone's 3D body"),
  useSurfaceForAllFaces: z.boolean().optional().describe("Use 'surface' for all faces of the 3D zone body"),
  showFill: z.boolean().optional().describe("Show the floor plan cover fill of the zone"),
  fill: AttrRef.optional().describe("Cover fill type (Fill attribute name/index); also turns showFill on unless showFill is given"),
  fillPen: Pen.optional().describe("Foreground pen of the cover fill"),
  fillBackgroundPen: z.number().int().min(0).max(255).optional().describe("Background pen of the cover fill (0-255)"),
  fillFromSurface: z.boolean().optional().describe("Use the fill of the zone's surface (3D hatching) instead of 'fill'"),
  showContour: z.boolean().optional().describe("Draw the zone contour line on the floor plan"),
  contourPen: Pen.optional().describe("Pen of the zone contour line"),
  contourLineType: AttrRef.optional().describe("Line type (attribute name/index) of the contour; also turns showContour on unless given"),
  fillOrigin: Point2D.optional().describe("Local origin of the cover fill pattern (m); switches the fill to local orientation"),
  fillAngle: z.number().optional().describe("Angle of the cover fill pattern in degrees (local orientation)"),
  localFillOrientation: z.boolean().optional().describe("true = fill pattern uses fillOrigin/fillAngle, false = project origin"),
};

export const ZoneFields = {
  polygon: Polygon.optional().describe(
    "MANUAL zone outline in m: array of points or {points, arcs?, holes?}. 2D only (no z). Exactly one of polygon / referencePoint is required",
  ),
  referencePoint: Point2D.optional().describe(
    "AUTOMATIC zone: a point INSIDE a room fully enclosed by walls (zoneRelation 'Boundary'), columns, curtain walls or room-separator " +
      "lines on the zone's story; Archicad detects the boundary. Exactly one of polygon / referencePoint is required",
  ),
  ...ZoneSettingsFields,
  ...CommonElementFields,
};

function exactlyOneGeometry(v: { polygon?: unknown; referencePoint?: unknown }): boolean {
  return (v.polygon === undefined) !== (v.referencePoint === undefined);
}

export const ZoneSpec = z.object(ZoneFields).refine(exactlyOneGeometry, {
  message: "Give exactly one of 'polygon' (manual zone) or 'referencePoint' (automatic zone)",
});

export const ZonePatch = z
  .object({
    guid: Guid.describe("GUID of the zone to change"),
    polygon: Polygon.optional().describe("New outline (m); makes the zone MANUAL"),
    referencePoint: Point2D.optional().describe(
      "New reference point inside a closed room; makes the zone AUTOMATIC. Archicad cannot move a zone's reference point in place, so the " +
        "zone is RE-CREATED at the new point (settings, stamp parameters, Element ID, classifications and custom properties are kept; the " +
        "stamp moves along) and gets a NEW GUID: the result item then contains {guid: old, newGuid}",
    ),
    automatic: z
      .boolean()
      .optional()
      .describe("false = freeze an automatic zone as a manual zone keeping its current polygon; true requires referencePoint for manual zones"),
    ...ZoneSettingsFields,
    ...CommonElementFields,
  })
  .refine((v) => !(v.polygon !== undefined && v.referencePoint !== undefined), {
    message: "Give either 'polygon' (manual) or 'referencePoint' (automatic), not both",
  });

/** Normalizes numeric zone numbers to text (the add-on stores roomNoStr as a string). */
function normalizeNumber<T extends { number?: string | number }>(item: T): T {
  if (typeof item.number === "number") return { ...item, number: String(item.number) };
  return item;
}

type ItemResult = { guid?: string; error?: { code?: number; message: string } } & Record<string, unknown>;

interface TypesOfElements {
  typesOfElements?: Array<{ typeOfElement?: { elementId?: { guid?: string }; elementType?: string }; error?: unknown }>;
}

/**
 * Modifies zones through the generic ModifyElements command, but first checks the element types with the official
 * API.GetTypesOfElements: ModifyElements would silently "succeed" for a wall patched with zone-only fields (they are
 * ignored), so non-zone GUIDs get an error result here instead. Missing GUIDs are left to ModifyElements (it reports them).
 */
export async function modifyZones(ac: ArchicadClient, patches: Array<Record<string, unknown> & { guid: string }>): Promise<{ results: ItemResult[] }> {
  let types: Array<string | undefined> = [];
  try {
    const res = await ac.api<TypesOfElements>("API.GetTypesOfElements", { elements: toElementIds(patches.map((p) => p.guid)) });
    types = (res.typesOfElements ?? []).map((t) => t.typeOfElement?.elementType);
  } catch {
    types = []; // type check is best effort; ModifyElements still validates the GUIDs
  }
  const results: ItemResult[] = new Array(patches.length);
  const send: Array<Record<string, unknown>> = [];
  const sendIndex: number[] = [];
  patches.forEach((patch, i) => {
    const type = types[i];
    if (type !== undefined && type !== "Zone") {
      results[i] = {
        guid: patch.guid,
        error: {
          message: `Element ${patch.guid} is a ${type}, not a Zone: nothing was changed. Use modify_elements (or the ${type}-specific modify tool) for it; find zones with get_zones.`,
        },
      };
    } else {
      send.push(patch);
      sendIndex.push(i);
    }
  });
  // Archicad cannot move a zone's reference point in place: those zones are re-created at the new
  // point first (RelocateZones, new GUID), then their other fields are applied to the new zone.
  const relocateIdx = sendIndex.filter((_, k) => send[k]!["referencePoint"] !== undefined);
  if (relocateIdx.length > 0) {
    const rel = await ac.addon<{ results?: Array<ItemResult & { newGuid?: string }> }>("RelocateZones", {
      zones: relocateIdx.map((i) => ({ guid: patches[i]!.guid, referencePoint: patches[i]!["referencePoint"] })),
    });
    relocateIdx.forEach((original, k) => {
      const r = rel.results?.[k];
      const kIdx = sendIndex.indexOf(original);
      const rest = { ...send[kIdx]! };
      delete rest["referencePoint"];
      if (!r || r.error || !r.newGuid) {
        results[original] = r ?? { guid: patches[original]!.guid, error: { message: "No result returned by RelocateZones" } };
        send[kIdx] = {};                      // drop from the ModifyElements batch
      } else {
        results[original] = r;
        rest["guid"] = r.newGuid;
        send[kIdx] = Object.keys(rest).length > 1 ? rest : {};
      }
    });
  }
  const pending = send.map((p, k) => [p, sendIndex[k]!] as const).filter(([p]) => Object.keys(p).length > 0);
  send.length = 0;
  sendIndex.length = 0;
  for (const [p, i] of pending) {
    send.push(p);
    sendIndex.push(i);
  }
  if (send.length > 0) {
    const res = await ac.addon<{ results?: ItemResult[] }>("ModifyElements", { elements: send, undoName: "Modify zones (Claude)" });
    const sent = res.results ?? [];
    sendIndex.forEach((original, k) => {
      const prior = results[original] as (ItemResult & { newGuid?: string }) | undefined;
      const r = sent[k] ?? { guid: patches[original]!.guid, error: { message: "No result returned by ModifyElements" } };
      // keep the relocation info (old guid + newGuid) when a relocated zone also got other changes
      results[original] = prior?.newGuid ? { ...prior, ...(r.error ? { error: r.error } : {}) } : r;
    });
  }
  return { results };
}

const StoryIndices = z
  .array(z.number().int())
  .min(1)
  .describe("Only zones on these story indices (0 = ground floor, negative = basements; see get_stories)");

export function registerZoneTools(server: McpServer, ctx: ToolContext): void {
  defineTool(server, ctx, {
    name: "create_zones",
    title: "Create zones",
    description:
      "Creates zones (rooms/spaces) in one undo step. Two modes per item: " +
      "(1) MANUAL: 'polygon' = the outline in meters; " +
      "(2) AUTOMATIC: 'referencePoint' = a point inside a room enclosed by walls/columns/room separators on that story — Archicad " +
      "detects the outline (boundary 'InnerEdge' or 'ReferenceLine'); it fails with a clear error if the point is not enclosed. " +
      "Create the walls first. Set name, number, category (localized ZoneCategory attribute, see get_attributes), height (m), " +
      "bottomOffset, stamp position/angle, stamp library part + GDL parameters, fill/contour. Unspecified settings come from the Zone tool defaults. " +
      "Returns [{guid, type} | {error}] in input order. Read areas/volumes back with get_zones.",
    input: {
      zones: z.array(ZoneSpec).min(1).max(500).describe("Zones to create"),
    },
    annotations: CREATES,
    handler: async ({ zones }, c) =>
      createElements(
        c,
        zones.map((zone) => ({ type: "Zone", ...normalizeNumber(zone) })),
        "Create zones (Claude)",
      ),
  });

  defineTool(server, ctx, {
    name: "modify_zones",
    title: "Modify zones",
    description:
      "Changes existing zones in one undo step: name, number, category, height/topLinkedStory/bottomOffset, area reduction, stamp " +
      "(position, angle, library part, GDL parameters), fill/contour/surface, layer/story/elementId. Geometry: 'polygon' sets a new outline " +
      "and makes the zone manual; 'referencePoint' makes it automatic around a new point; automatic:false freezes the current outline. " +
      "Only the given fields change. Non-zone GUIDs are rejected per item. Returns {results: [{guid} | {guid?, error}]} in input order. " +
      "Use get_zones / get_element_details to read current values.",
    input: {
      zones: z.array(ZonePatch).min(1).max(1000).describe("Patches: {guid, field: newValue, ...}"),
    },
    annotations: MODIFIES,
    handler: async ({ zones }, { ac }) => modifyZones(ac, zones.map((z0) => normalizeNumber(z0))),
  });

  defineTool(server, ctx, {
    name: "update_zones",
    title: "Update zones (re-detect boundaries)",
    description:
      "Equivalent of Archicad's Design > Update Zones for AUTOMATIC zones: after walls/columns/room separators were moved, added or deleted, " +
      "re-detects each zone's boundary from its reference point (a temporary zone is placed there and deleted again) and applies it. " +
      "Manual zones are skipped (change them with modify_zones). Default: all automatic zones of the project (narrow with 'zones' or 'stories'). " +
      "method 'copyBoundary' (default) keeps the zone GUID and writes the new polygon into it; 'recreate' replaces each zone by a new one " +
      "with the same settings, stamp parameters, element ID, classifications and custom property values (NEW GUIDs, associative labels are lost) " +
      "— use it only if copyBoundary reports an error or verified:false. dryRun:true only reports which zones are 'outdated'. " +
      "Returns {results: [{guid, status: updated|upToDate|outdated|recreated|skipped, before/after areas, verified, newGuid?} | {guid, error}], summary}.",
    input: {
      zones: z.array(ElementRef).min(1).max(2000).optional().describe("Zones to update (default: every automatic zone)"),
      stories: StoryIndices.optional(),
      dryRun: z.boolean().optional().describe("Only check: report outdated zones (with the re-detected polygon) without changing them"),
      method: z
        .enum(["copyBoundary", "recreate"])
        .optional()
        .describe("'copyBoundary' (default, keeps GUIDs) or 'recreate' (fallback: new zones, new GUIDs)"),
    },
    annotations: MODIFIES,
    handler: async ({ zones, stories, dryRun, method }, { ac }) => {
      const params: Record<string, unknown> = {};
      if (zones) params["zones"] = zones.map(guidOf);
      if (stories) params["stories"] = stories;
      if (dryRun !== undefined) params["dryRun"] = dryRun;
      if (method) params["method"] = method;
      return ac.addon("UpdateZones", params);
    },
  });

  defineTool(server, ctx, {
    name: "get_zones",
    title: "Get zones (room schedule)",
    description:
      "Lists zones (rooms) like a room schedule, sorted by story then number: guid, name, number, category, story, construction method " +
      "(Manual / InnerEdge / ReferenceLine), area, netArea, calculatedArea (after reductions, as in the stamp) in m², perimeter (m), volume (m³), " +
      "height, bottomElevation, stamp position, plus totals over ALL matching zones. Filter by guids, stories, category or a search text " +
      "(name or number). Optional: includePolygon, includeQuantities (walls/doors/windows surfaces, corners, extracted areas...), " +
      "includeRelations (boundary walls/beams, contained elements by type), includeReductions (area reductions with type/percent/area/polygon). " +
      "For every setting of one zone use get_element_details.",
    input: {
      zones: z.array(ElementRef).min(1).max(5000).optional().describe("Only these zones"),
      stories: StoryIndices.optional(),
      category: AttrRef.optional().describe("Only zones of this zone category (localized name or index)"),
      search: z.string().min(1).optional().describe("Case-insensitive text contained in the zone name or number"),
      includePolygon: z
        .boolean()
        .optional()
        .describe("Add each zone's outline 'polygon' (inner edges); ReferenceLine zones also get 'referenceLinePolygon' (the measured gross outline)"),
      includeQuantities: z.boolean().optional().describe("Add the full Archicad zone quantity set"),
      includeRelations: z.boolean().optional().describe("Add boundary walls/beams/curtain wall segments and contained elements by type"),
      includeReductions: z.boolean().optional().describe("Add the area reduction polygons (walls, columns, fills, low-height parts)"),
      offset: z.number().int().min(0).optional().describe("Skip this many zones"),
      limit: z.number().int().min(1).max(5000).optional().describe("Return at most this many zones (default 500)"),
    },
    annotations: READ_ONLY,
    handler: async (args, { ac }) => {
      const params: Record<string, unknown> = { ...args };
      if (args.zones) params["zones"] = args.zones.map(guidOf);
      return ac.addon("GetZones", params);
    },
  });
}
