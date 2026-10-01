/**
 * Column and beam tools (AC26 segmented columns / beams).
 * The add-on adapters live in addon/Src/Commands/ColumnsBeams.cpp (+ ColumnsBeamsSegments.cpp); field names match 1:1.
 *
 *   create_columns / create_beams  -> CreateElements   ({type: 'Column'|'Beam', ...fields})
 *   modify_columns / modify_beams  -> ModifyElements   (after checking the element types with API.GetTypesOfElements)
 *   get_element_details returns the same field names (plus segments[], cuts[], holes[], elevations).
 */

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import { CREATES, defineTool, MODIFIES, type ToolContext } from "./define.js";
import { createElements } from "./elements.js";
import { AttrRef, CommonElementFields, Guid, PenIndex, Point2D } from "./schemas.js";

// --- Shared building blocks -----------------------------------------------------------------

export const ANCHOR_NAMES = [
  "TopLeft",
  "TopCenter",
  "TopRight",
  "MiddleLeft",
  "Center",
  "MiddleRight",
  "BottomLeft",
  "BottomCenter",
  "BottomRight",
] as const;

const SurfaceOverride = z
  .union([AttrRef, z.literal(false)])
  .describe("Surface (material) override: surface name/index (localized names, list with get_attributes type Surface), or false to remove the override");

const ShowOnStories = z
  .union([
    z.enum(["Auto", "HomeOnly", "AllStories"]),
    z.object({
      homeStory: z.boolean().optional().describe("Show on the home story"),
      allAbove: z.boolean().optional().describe("Show on all stories above"),
      allBelow: z.boolean().optional().describe("Show on all stories below"),
      storiesAbove: z.number().int().min(0).optional().describe("Show on this many stories above the home story"),
      storiesBelow: z.number().int().min(0).optional().describe("Show on this many stories below the home story"),
    }),
  ])
  .describe(
    "Floor plan story visibility: 'Auto' (Archicad shows it on every story it physically reaches), 'HomeOnly', 'AllStories', " +
      "or an explicit {homeStory, allAbove, allBelow, storiesAbove, storiesBelow}",
  );

const FloorPlanDisplay = z
  .enum(["Projected", "ProjectedWithOverhead", "CutOnly", "OutlinesOnly", "OverheadAll", "SymbolicCut"])
  .describe("Floor plan display option (Floor Plan and Section > Show on Floor Plan)");

const ViewDepthLimitation = z
  .enum(["ToFloorPlanRange", "ToAbsoluteLimit", "EntireElement"])
  .describe("How far the element is shown on the floor plan (Show Projection)");

const PenOverride = z.union([PenIndex, z.literal(false)]);

const FloorPlanFillFields = {
  cutFillPen: PenOverride.optional().describe("Override the cut fill foreground pen (1-255); false = use the pen of the building material/profile"),
  cutFillBackgroundPen: PenOverride.optional().describe("Override the cut fill background pen (1-255); false = use the structure's pen"),
  coverFill: z
    .union([
      z.boolean(),
      z.object({
        enabled: z.boolean().optional().describe("Show the cover fill (default true when fill is given)"),
        fill: AttrRef.optional().describe("Fill type name/index (localized, list with get_attributes type Fill)"),
        pen: PenIndex.optional().describe("Fill foreground pen"),
        backgroundPen: PenIndex.optional().describe("Fill background pen"),
        fromSurface: z.boolean().optional().describe("Use the fill of the element's surface instead of 'fill'"),
        orientationFrom3D: z.boolean().optional().describe("Orient the fill as in the 3D texture"),
      }),
    ])
    .optional()
    .describe("Floor plan cover fill of the uncut (projected) part; true/false toggles it"),
};

const LineStyle = z
  .object({ pen: PenIndex.optional(), lineType: AttrRef.optional().describe("Line type name/index (localized, get_attributes type Line)") })
  .describe("{pen, lineType}");

const StructureFields = {
  buildingMaterial: AttrRef.optional().describe(
    "Basic structure: building material name/index (localized, list with get_attributes type BuildingMaterial). Composites are not possible for columns/beams",
  ),
  profile: AttrRef.optional().describe(
    "Complex profile structure: profile attribute name/index (the profile must be enabled for this element type; list with get_attributes type Profile). " +
      "Makes the section follow the profile; give buildingMaterial instead to go back to a basic section",
  ),
};

const SchemeFields = {
  length: z.number().positive().optional().describe("Fixed segment length in m (measured along the element axis)"),
  lengthProportion: z
    .number()
    .positive()
    .optional()
    .describe("Proportional segment: relative share of the length left after the fixed segments (weights, normalized to add up to 1)"),
};

export const CutSpec = z
  .object({
    type: z
      .enum(["Horizontal", "Vertical", "Custom"])
      .optional()
      .describe("Cut plane type. Keep the tool default (columns normally Horizontal) unless you need a Custom angle"),
    angle: z.number().gt(-90).lt(90).optional().describe("Custom cut angle in degrees (setting it switches the cut to Custom)"),
  })
  .describe("Segment end cut");

const CUTS_DESCRIPTION =
  "Segment cuts, exactly segmentCount + 1 items: [start cut, cut between segment 0 and 1, ..., end cut] " +
  "(read the current ones with get_element_details)";

const SEGMENTS_NOTE =
  "Setting a different number of items changes the number of segments (new segments copy the last one). " +
  "Use {} for a segment you do not want to change. Each item accepts the section/surface fields of this tool plus length | lengthProportion; " +
  "they override the top-level section fields for that segment. Proportional lengths are normalized so they add up to 1.";

function noBoth(a: string, b: string) {
  return (v: Record<string, unknown>) => !(v[a] !== undefined && v[b] !== undefined);
}

// --- Columns ----------------------------------------------------------------------------------

export const ColumnSectionFields = {
  shape: z
    .enum(["Rectangular", "Circular"])
    .optional()
    .describe("Cross-section shape (default: Column tool setting). width/depth imply Rectangular, diameter implies Circular"),
  width: z.number().positive().optional().describe("Rectangular section size along the column's local x axis (m, before rotationAngle)"),
  depth: z.number().positive().optional().describe("Rectangular section size along the column's local y axis (m)"),
  diameter: z.number().positive().optional().describe("Circular section diameter (m); makes the section circular"),
  tapered: z.boolean().optional().describe("Tapered segment: the section changes linearly from bottom to top (give endWidth/endDepth or endDiameter)"),
  endWidth: z.number().positive().optional().describe("Tapered: width at the top of the segment (m)"),
  endDepth: z.number().positive().optional().describe("Tapered: depth at the top of the segment (m)"),
  endDiameter: z.number().positive().optional().describe("Tapered circular: diameter at the top of the segment (m)"),
  ...StructureFields,
  surface: SurfaceOverride.optional().describe("Override the surface of all faces (sides and ends); false removes the overrides"),
  sideSurface: SurfaceOverride.optional().describe("Override the surface of the column sides (extrusion faces)"),
  endsSurface: SurfaceOverride.optional().describe("Override the surface of the top and bottom faces"),
  surfacesChained: z
    .boolean()
    .optional()
    .describe("Chain side and end surfaces (one surface for all). Giving differing sideSurface/endsSurface unchains automatically"),
  veneerType: z.enum(["Core", "Finish", "Other"]).optional().describe("Veneer (cover) skin type"),
  veneerThickness: z.number().min(0).optional().describe("Veneer thickness in m around the core (0 = no veneer)"),
  veneerBuildingMaterial: AttrRef.optional().describe("Building material of the veneer"),
};

export const ColumnSegmentSpec = z
  .object({ ...ColumnSectionFields, ...SchemeFields })
  .refine(noBoth("length", "lengthProportion"), { message: "give either length or lengthProportion, not both" })
  .refine(noBoth("buildingMaterial", "profile"), { message: "give either buildingMaterial or profile, not both" })
  .describe("One column segment (bottom to top): the section/surface/veneer fields documented at the top level, for this segment only, plus length | lengthProportion");

const ColumnFieldsBase = {
  height: z
    .number()
    .positive()
    .optional()
    .describe("Column height in m. Giving a height unlinks the top unless topLinkedStory is also given (tool defaults are often top-linked)"),
  bottomOffset: z.number().optional().describe("Base elevation relative to the home story level (m, e.g. -0.3 to start below the floor)"),
  topLinkedStory: z
    .number()
    .int()
    .min(0)
    .optional()
    .describe("Link the top to the story N levels above the home story (1 = next story; 0 = unlinked, use height)"),
  topOffset: z.number().optional().describe("Offset of the top from the linked story (m, e.g. -0.25 to stop under the slab)"),
  rotationAngle: z.number().optional().describe("Rotation of the column around its own axis on the plan, degrees counter-clockwise"),
  slanted: z.boolean().optional().describe("Slanted column (tilted axis). Usually set implicitly by slantAngle"),
  slantAngle: z
    .number()
    .gt(0)
    .lt(180)
    .optional()
    .describe("Slant angle of the axis in degrees measured from the horizontal plane as in the Column Settings dialog (90 = vertical). Other values make the column slanted"),
  slantDirection: z.number().optional().describe("Direction the slanted column leans towards on the plan, degrees counter-clockwise from the x axis"),
  flipped: z.boolean().optional().describe("Mirror the column (relevant for asymmetric complex profiles)"),
  anchor: z
    .union([z.enum(ANCHOR_NAMES), z.number().int().min(0).max(8)])
    .optional()
    .describe(
      "Which point of the section sits on 'origin': 3x3 grid in the column's local axes (before rotationAngle; Top = +y side, Left = -x side): " +
        "0 TopLeft, 1 TopCenter, 2 TopRight, 3 MiddleLeft, 4 Center (default), 5 MiddleRight, 6 BottomLeft, 7 BottomCenter, 8 BottomRight. " +
        "E.g. BottomLeft puts the section's min-x/min-y corner on origin. On modify without 'origin' the column body stays in place",
    ),
  wrapping: z.boolean().optional().describe("Wall wrapping: walls meeting the column wrap around it (skins continue around the column)"),
  zoneRelation: z.enum(["Boundary", "ReduceArea", "None"]).optional().describe("Relation to zones: zone boundary, reduce zone area, or no effect"),
  floorPlanSymbol: z.enum(["Plain", "Slash", "X", "Crosshair"]).optional().describe("Core symbol shown on the floor plan"),
  floorPlanDisplay: FloorPlanDisplay.optional(),
  viewDepthLimitation: ViewDepthLimitation.optional(),
  showOnStories: ShowOnStories.optional(),
  ...FloorPlanFillFields,
  lines: z
    .object({
      contour: LineStyle.optional().describe("Core contour lines (cut)"),
      uncut: LineStyle.optional().describe("Uncut (below/projected) lines"),
      overhead: LineStyle.optional().describe("Overhead lines"),
      hidden: LineStyle.optional().describe("Hidden lines"),
      veneer: LineStyle.optional().describe("Veneer contour lines"),
      symbol: z.object({ pen: PenIndex.optional() }).optional().describe("Core symbol (slash/X/crosshair) pen"),
    })
    .optional()
    .describe("Floor plan line pens / line types"),
  ...ColumnSectionFields,
  segments: z
    .array(ColumnSegmentSpec)
    .min(1)
    .max(100)
    .optional()
    .describe("Segments from bottom to top (multi-segment columns). " + SEGMENTS_NOTE),
  cuts: z.array(CutSpec).min(2).optional().describe(CUTS_DESCRIPTION + " — for columns: bottom cut, inner cuts, top cut"),
  ...CommonElementFields,
};

const columnChecks = <T extends z.ZodTypeAny>(schema: T) =>
  schema
    .refine(noBoth("buildingMaterial", "profile"), { message: "give either buildingMaterial or profile, not both" })
    .refine((v: Record<string, unknown>) => !(v["diameter"] !== undefined && v["shape"] === "Rectangular"), {
      message: "diameter requires shape 'Circular' (or omit shape)",
    });

export const ColumnFields = {
  origin: Point2D.describe(
    "Column position on the plan (m): the point where the section's anchor point sits (default anchor Center = the column axis). " +
      "get_element_details returns this same 'origin' plus 'center' (the section centre)",
  ),
  ...ColumnFieldsBase,
};

export const ColumnSpec = columnChecks(z.object(ColumnFields)).describe("New column");

export const ColumnPatch = columnChecks(
  z.object({
    guid: Guid.describe("GUID of the column to change"),
    origin: Point2D.optional().describe(
      "New position of the anchor point on the plan (m). Omitted: the anchor point stays where it is (size/rotation changes pivot around it; " +
        "changing only 'anchor' leaves the column body in place)",
    ),
    ...ColumnFieldsBase,
  }),
).describe("Column changes: only the given fields change. Top-level section/surface fields apply to ALL segments");

// --- Beams ------------------------------------------------------------------------------------

export const BeamSectionFields = {
  shape: z
    .enum(["Rectangular", "Circular"])
    .optional()
    .describe("Cross-section shape (default: Beam tool setting). width/height imply Rectangular, diameter implies Circular"),
  width: z.number().positive().optional().describe("Section width in m (horizontal, across the beam)"),
  height: z.number().positive().optional().describe("Section height in m (vertical size of the beam cross-section)"),
  diameter: z.number().positive().optional().describe("Circular section diameter (m); makes the section circular"),
  tapered: z.boolean().optional().describe("Tapered segment: the section changes linearly from start to end (give endWidth/endHeight or endDiameter)"),
  endWidth: z.number().positive().optional().describe("Tapered: width at the end of the segment (m)"),
  endHeight: z.number().positive().optional().describe("Tapered: height at the end of the segment (m)"),
  endDiameter: z.number().positive().optional().describe("Tapered circular: diameter at the end of the segment (m)"),
  ...StructureFields,
  surface: SurfaceOverride.optional().describe("Override the surface of every face (left, right, top, bottom, ends); false removes the overrides"),
  leftSurface: SurfaceOverride.optional().describe("Override the left side surface (left looking from begin to end)"),
  rightSurface: SurfaceOverride.optional().describe("Override the right side surface"),
  topSurface: SurfaceOverride.optional().describe("Override the top face surface"),
  bottomSurface: SurfaceOverride.optional().describe("Override the bottom face surface"),
  endsSurface: SurfaceOverride.optional().describe("Override the surface of both end faces"),
  surfacesChained: z
    .boolean()
    .optional()
    .describe("Chain all face surfaces (one surface for all). Giving differing individual face surfaces unchains automatically"),
};

export const BeamSegmentSpec = z
  .object({ ...BeamSectionFields, ...SchemeFields })
  .refine(noBoth("length", "lengthProportion"), { message: "give either length or lengthProportion, not both" })
  .refine(noBoth("buildingMaterial", "profile"), { message: "give either buildingMaterial or profile, not both" })
  .describe("One beam segment (begin to end): the section/surface fields documented at the top level, for this segment only, plus length | lengthProportion");

export const BeamHoleSpec = z
  .object({
    shape: z.enum(["Rectangular", "Circular"]).optional().describe("Hole shape (default: Beam tool hole setting; diameter implies Circular)"),
    distanceFromBegin: z.number().min(0).describe("Position of the hole centre along the beam axis, measured from the begin point (m)"),
    depthBelowTop: z
      .number()
      .optional()
      .describe("Vertical position of the hole centre: distance below the top of the beam (m), as the 'Position' in Beam hole settings. Default: the tool's hole level"),
    width: z.number().positive().optional().describe("Hole width along the beam (m); for circular holes the diameter"),
    height: z.number().positive().optional().describe("Rectangular hole height (m)"),
    diameter: z.number().positive().optional().describe("Circular hole diameter (m)"),
    showContour: z.boolean().optional().describe("Show the hole contour on the floor plan"),
  })
  .describe("Hole through the beam (perpendicular to the beam side faces)");

const BeamFieldsBase = {
  level: z
    .number()
    .optional()
    .describe("Height of the beam reference axis above the home story level (m) — Archicad's 'Offset to Home Story'; with the default anchor this is the TOP of the beam"),
  offset: z.number().optional().describe("Horizontal offset of the beam body from its reference axis (m; sign = side)"),
  anchor: z
    .union([z.enum(ANCHOR_NAMES), z.number().int().min(0).max(8)])
    .optional()
    .describe(
      "Which point of the cross-section the reference axis passes through: 3x3 grid, 0 TopLeft, 1 TopCenter, 2 TopRight, 3 MiddleLeft, 4 Center, 5 MiddleRight, " +
        "6 BottomLeft, 7 BottomCenter, 8 BottomRight (left = left looking from begin to end)",
    ),
  arcAngle: z
    .number()
    .gt(-360)
    .lt(360)
    .optional()
    .describe("Horizontally curved beam: central angle of the arc from begin to end in degrees (positive = counter-clockwise; 0 = straight)"),
  verticalCurveHeight: z.number().optional().describe("Vertically curved (arched) beam: rise of the arc above the chord in m (0 = straight)"),
  beamShape: z
    .enum(["Straight", "HorizontallyCurved", "VerticallyCurved"])
    .optional()
    .describe("Beam geometry type; usually derived from arcAngle / verticalCurveHeight"),
  slantAngle: z
    .number()
    .gt(-90)
    .lt(90)
    .optional()
    .describe("Slanted beam: angle of the axis from the horizontal in degrees (positive rises from begin to end; 0 = horizontal)"),
  profileRotationAngle: z.number().optional().describe("Rotation (twist) of the cross-section around the beam axis in degrees"),
  flipped: z.boolean().optional().describe("Mirror the beam cross-section (profiled beams only)"),
  sequence: z.number().int().min(0).max(999).optional().describe("Intersection priority sequence (0-999) used when beams meet in a junction"),
  showContourLines: z.enum(["Always", "Never", "ByModelViewOptions"]).optional().describe("Show the beam contour lines on the floor plan"),
  showReferenceAxis: z.enum(["Always", "Never", "ByModelViewOptions"]).optional().describe("Show the beam reference axis on the floor plan"),
  floorPlanDisplay: FloorPlanDisplay.optional(),
  viewDepthLimitation: ViewDepthLimitation.optional(),
  showOnStories: ShowOnStories.optional(),
  ...FloorPlanFillFields,
  lines: z
    .object({
      reference: LineStyle.optional().describe("Reference axis line"),
      cutContour: LineStyle.optional().describe("Cut contour lines"),
      uncut: LineStyle.optional().describe("Uncut (below/projected) lines"),
      overhead: LineStyle.optional().describe("Overhead lines"),
      hidden: LineStyle.optional().describe("Hidden lines"),
    })
    .optional()
    .describe("Floor plan line pens / line types"),
  ...BeamSectionFields,
  segments: z
    .array(BeamSegmentSpec)
    .min(1)
    .max(100)
    .optional()
    .describe("Segments from begin to end (multi-segment beams). " + SEGMENTS_NOTE),
  cuts: z.array(CutSpec).min(2).optional().describe(CUTS_DESCRIPTION + " — for beams: begin cut, inner cuts, end cut"),
  holes: z.array(BeamHoleSpec).max(500).optional().describe("Holes in the beam. Replaces ALL existing holes ([] removes them); hole ids are renumbered 1..n"),
  addHoles: z.array(BeamHoleSpec).min(1).max(500).optional().describe("Holes to add to the existing ones (modify) / to the default ones (create)"),
  removeHoles: z
    .array(z.number().int())
    .min(1)
    .optional()
    .describe("Ids of holes to remove (the 'id' values returned by get_element_details)"),
  ...CommonElementFields,
};

const beamChecks = <T extends z.ZodTypeAny>(schema: T) =>
  schema
    .refine(noBoth("buildingMaterial", "profile"), { message: "give either buildingMaterial or profile, not both" })
    .refine((v: Record<string, unknown>) => !(Boolean(v["arcAngle"]) && Boolean(v["verticalCurveHeight"])), {
      message: "a beam is either horizontally curved (arcAngle) or vertically curved (verticalCurveHeight), not both",
    })
    .refine((v: Record<string, unknown>) => !(v["diameter"] !== undefined && v["shape"] === "Rectangular"), {
      message: "diameter requires shape 'Circular' (or omit shape)",
    });

export const BeamFields = {
  begin: Point2D.describe("Start point of the beam reference axis on the plan (m)"),
  end: Point2D.describe("End point of the beam reference axis on the plan (m)"),
  ...BeamFieldsBase,
};

export const BeamSpec = beamChecks(z.object(BeamFields)).describe("New beam");

export const BeamPatch = beamChecks(
  z.object({
    guid: Guid.describe("GUID of the beam to change"),
    begin: Point2D.optional().describe("New start point of the reference axis (m)"),
    end: Point2D.optional().describe("New end point of the reference axis (m)"),
    ...BeamFieldsBase,
  }),
).describe("Beam changes: only the given fields change. Top-level section/surface fields apply to ALL segments");

// --- Typed modify helper ------------------------------------------------------------------------

interface TypesResult {
  typesOfElements?: Array<{ typeOfElement?: { elementType?: string }; error?: { code?: number; message?: string } }>;
}

interface ResultsResponse {
  results?: unknown[];
}

/**
 * Sends patches to ModifyElements after checking with API.GetTypesOfElements that every GUID is of the
 * expected type; mismatching / unknown items get a per-item error (the others are still modified).
 */
export async function modifyTyped(
  ctx: ToolContext,
  patches: Array<Record<string, unknown> & { guid: string }>,
  expectedType: "Column" | "Beam",
  toolHint: string,
  undoName?: string,
): Promise<{ results: unknown[] }> {
  const results: unknown[] = new Array(patches.length).fill(undefined);
  let types: TypesResult["typesOfElements"];
  try {
    const res = await ctx.ac.api<TypesResult>("API.GetTypesOfElements", {
      elements: patches.map((p) => ({ elementId: { guid: p.guid } })),
    });
    types = res.typesOfElements;
  } catch {
    types = undefined; // type check unavailable: let the add-on validate each element
  }

  const send: Array<Record<string, unknown>> = [];
  const sentIndex: number[] = [];
  patches.forEach((patch, i) => {
    const entry = types?.[i];
    const type = entry?.typeOfElement?.elementType;
    if (types !== undefined && type !== expectedType) {
      const message =
        type !== undefined
          ? `Element ${patch.guid} is a ${type}, not a ${expectedType}. Use modify_elements or the ${type} tool instead of ${toolHint}.`
          : `Element ${patch.guid} was not found${entry?.error?.message ? ` (${entry.error.message})` : ""}. Check the GUID with get_element_details / find_elements.`;
      results[i] = { guid: patch.guid, error: { code: entry?.error?.code ?? -2130313112, message } };
      return;
    }
    send.push(patch);
    sentIndex.push(i);
  });

  if (send.length > 0) {
    const params: Record<string, unknown> = { elements: send };
    if (undoName) params["undoName"] = undoName;
    const res = await ctx.ac.addon<ResultsResponse>("ModifyElements", params);
    const list = Array.isArray(res.results) ? res.results : [];
    sentIndex.forEach((original, k) => {
      results[original] = list[k] ?? { guid: patches[original]!.guid, error: { message: "No result returned by the add-on" } };
    });
  }
  return { results };
}

// --- Registration -------------------------------------------------------------------------------

export function registerColumnBeamTools(server: McpServer, ctx: ToolContext): void {
  defineTool(server, ctx, {
    name: "create_columns",
    title: "Create columns",
    description:
      "Creates columns (one undo step): rectangular, circular, complex-profile, tapered, slanted and multi-segment columns, with veneer, wall wrapping and " +
      "surface overrides. Coordinates in meters, angles in degrees, on the given story (default: current story). Unspecified settings come from the " +
      "Column tool defaults — give 'height' (unlinked) or 'topLinkedStory' (+ topOffset) so the height is what you expect. " +
      "Typical: {origin: {x: 0, y: 0}, height: 3, width: 0.4, depth: 0.4, buildingMaterial: '<name>'}. " +
      "Returns [{guid, type} | {error}] in input order; read the result back with get_element_details (segments, cuts, elevations).",
    input: {
      columns: z.array(ColumnSpec).min(1).max(1000).describe("Columns to create"),
      undoName: z.string().optional().describe("Name of the undo step shown in Archicad"),
    },
    annotations: CREATES,
    handler: async ({ columns, undoName }, c) =>
      createElements(c, columns.map((col) => ({ type: "Column", ...col })), undoName ?? "Create columns (Claude)"),
  });

  defineTool(server, ctx, {
    name: "create_beams",
    title: "Create beams",
    description:
      "Creates beams (one undo step): straight, horizontally or vertically curved, slanted, tapered, complex-profile and multi-segment beams, with holes " +
      "and surface overrides. Coordinates in meters, angles in degrees, on the given story (default: current story). 'level' is the height of the " +
      "reference axis (by default the beam top) above the home story — e.g. story height 3 m and a beam under the slab: level 2.8. Unspecified settings " +
      "come from the Beam tool defaults. Chain beams end-to-start so Archicad connects them. " +
      "Typical: {begin: {x: 0, y: 0}, end: {x: 6, y: 0}, level: 3, width: 0.3, height: 0.5}. Returns [{guid, type} | {error}] in input order.",
    input: {
      beams: z.array(BeamSpec).min(1).max(1000).describe("Beams to create"),
      undoName: z.string().optional().describe("Name of the undo step shown in Archicad"),
    },
    annotations: CREATES,
    handler: async ({ beams, undoName }, c) =>
      createElements(c, beams.map((b) => ({ type: "Beam", ...b })), undoName ?? "Create beams (Claude)"),
  });

  defineTool(server, ctx, {
    name: "modify_columns",
    title: "Modify columns",
    description:
      "Changes existing columns (one undo step). Each item is {guid, ...fields to change} using exactly the fields of create_columns " +
      "(origin, height, topLinkedStory, width/depth/diameter, buildingMaterial/profile, surfaces, veneer, slant, anchor, lines, ...); only the given " +
      "fields change and they are validated like create_columns. Top-level section/surface/veneer fields apply to ALL segments; use 'segments' " +
      "(full list, {} = unchanged segment; a different length changes the segment count) for single segments and 'cuts' for segment cuts. " +
      "Read the current values first with get_element_details. Non-column GUIDs are rejected per item. Returns [{guid} | {error}] in input order.",
    input: {
      columns: z
        .array(z.object({ guid: Guid }).passthrough())
        .min(1)
        .max(1000)
        .describe("Patches: {guid, <create_columns field>: newValue, ...}"),
      undoName: z.string().optional().describe("Name of the undo step shown in Archicad"),
    },
    annotations: MODIFIES,
    handler: async ({ columns, undoName }, c) => {
      const patches = z.array(ColumnPatch).parse(columns) as Array<Record<string, unknown> & { guid: string }>;
      return modifyTyped(c, patches, "Column", "modify_columns", undoName ?? "Modify columns (Claude)");
    },
  });

  defineTool(server, ctx, {
    name: "modify_beams",
    title: "Modify beams",
    description:
      "Changes existing beams (one undo step). Each item is {guid, ...fields to change} using exactly the fields of create_beams " +
      "(begin, end, level, width/height/diameter, arcAngle, slantAngle, buildingMaterial/profile, surfaces, anchor, lines, ...); only the given fields " +
      "change and they are validated like create_beams. Top-level section/surface fields apply to ALL segments; use 'segments' (full list, {} = " +
      "unchanged segment) for single segments or the segment count. Holes: 'holes' replaces all, 'addHoles' appends, 'removeHoles' deletes by id. " +
      "Read the current values first with get_element_details. Non-beam GUIDs are rejected per item. Returns [{guid} | {error}] in input order.",
    input: {
      beams: z
        .array(z.object({ guid: Guid }).passthrough())
        .min(1)
        .max(1000)
        .describe("Patches: {guid, <create_beams field>: newValue, ...}"),
      undoName: z.string().optional().describe("Name of the undo step shown in Archicad"),
    },
    annotations: MODIFIES,
    handler: async ({ beams, undoName }, c) => {
      const patches = z.array(BeamPatch).parse(beams) as Array<Record<string, unknown> & { guid: string }>;
      return modifyTyped(c, patches, "Beam", "modify_beams", undoName ?? "Modify beams (Claude)");
    },
  });
}
