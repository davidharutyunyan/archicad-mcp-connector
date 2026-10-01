/**
 * Dimension tools (linear, level, radial, angle). The add-on side lives in
 * addon/Src/Commands/Dimensions.cpp (+ DimensionsSpecial.cpp, DimensionsCommon.*); field names match 1:1.
 *
 *   create_dimensions         — linear dimension chains (static points and/or points linked to elements)
 *   create_level_dimensions   — level (elevation) markers on the plan
 *   create_radial_dimensions  — radius of arcs, circles, curved walls/beams
 *   create_angle_dimensions   — angle between two lines / walls
 *   dimension_walls           — automatic chains along walls (ends + window/door jambs, optional overall dimension)
 *                               or associative wall-thickness dimensions (mode "Thickness")
 *   modify_dimensions         — move the dimension line, change points / style / texts (pointTexts) of existing dimensions
 *   get_dimension_anchors     — points of elements that dimensions can link to
 */

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import { CREATES, defineTool, MODIFIES, READ_ONLY, type ToolContext } from "./define.js";
import { createElements } from "./elements.js";
import { AttrRef, DraftingCommonFields, ElementRef, ElementRefs, Guid, guidOf, Point2D } from "./schemas.js";

// --- Shared field schemas ---------------------------------------------------------------

const Pen = z.number().int().min(1).max(255);

const MarkerTypes = [
  "CrossLine",
  "EmptyCircle",
  "SlashLine60",
  "OpenArrow30",
  "ClosedArrow30",
  "FullArrow30",
  "SlashLine45",
  "CrossCircle",
  "OpenArrow90",
  "ClosedArrow90",
  "FullArrow90",
  "FullCircle",
  "PepitaCircle",
  "BandArrow",
  "OpenArrow60",
  "ClosedArrow60",
  "FullArrow60",
  "SlashLine75",
] as const;

const TextPosition = z
  .enum(["Above", "In", "Below"])
  .describe("Where the value text sits relative to the dimension line: 'Above' (default of most standards), 'In' (breaks the line), 'Below'");

const WitnessForm = z
  .enum(["None", "Small", "Large", "Fixed"])
  .describe("Witness (extension) line form: 'None', 'Small' (short tick), 'Large' (from the dimension line to near the element), 'Fixed' (fixed length)");

const WitnessVal = z
  .number()
  .describe(
    "Witness line value: the gap between the dimensioned point and the witness line (form 'Large') or the witness line length (form 'Fixed'), " +
      "in the unit Archicad stores — read witnessVal of an existing dimension (get_element_details) or of the tool defaults first",
  );

const Direction = z
  .union([
    z.enum(["Horizontal", "Vertical"]),
    z.number().describe("Angle in degrees (0 = +X, 90 = +Y)"),
    Point2D.describe("Direction vector {x, y} (any length)"),
  ])
  .describe("Dimension line direction: 'Horizontal' (measures X distances), 'Vertical' (Y distances), an angle in degrees, or a vector {x,y}");

const CustomText = z
  .union([z.string(), z.literal(false)])
  .describe("Custom text replacing the measured value (e.g. 'EQ', 'Ø 600'); \"\" or false = show the measured value again");

const TextStyleFields = {
  textPen: Pen.optional().describe("Pen (color) of the dimension text, 1-255"),
  textSize: z.number().positive().optional().describe("Text height in PAPER millimeters (e.g. 2.5), independent of the drawing scale"),
  textFont: AttrRef.optional().describe("Font by name (e.g. 'Arial') or font attribute index"),
  textBold: z.boolean().optional(),
  textItalic: z.boolean().optional(),
  textUnderline: z.boolean().optional(),
  textFrame: z.boolean().optional().describe("Draw a frame around the text"),
  textOpaque: z.boolean().optional().describe("Opaque (filled) text background"),
};

const MarkerFields = {
  markerType: z
    .enum(MarkerTypes)
    .optional()
    .describe("Dimension line end marker: ticks ('SlashLine45', 'SlashLine60', 'SlashLine75', 'CrossLine'), arrows ('OpenArrow30', 'FullArrow30', ...), dots ('FullCircle', 'EmptyCircle', 'PepitaCircle', 'CrossCircle'), 'BandArrow'"),
  markerPen: Pen.optional().describe("Pen of the markers"),
  markerSize: z.number().positive().optional().describe("Marker size in PAPER millimeters (e.g. 2)"),
};

const LinearStyleFields = {
  appearance: z
    .enum(["Normal", "Cumulative", "CumulativeSV", "Elevation"])
    .optional()
    .describe("Chain type: 'Normal' (segment lengths, default), 'Cumulative' (running distances from the first point), 'CumulativeSV' (Archicad's other cumulative variant), 'Elevation' (elevation-type values)"),
  textPosition: TextPosition.optional(),
  textDirection: z
    .enum(["Parallel", "Horizontal", "Vertical"])
    .optional()
    .describe("Text orientation: 'Parallel' to the dimension line (default), always 'Horizontal', or 'Vertical'"),
  linePen: Pen.optional().describe("Pen of the dimension and witness lines"),
  witnessForm: WitnessForm.optional().describe("Default witness line form of every point"),
  witnessVal: WitnessVal.optional(),
  horizontalText: z.boolean().optional().describe("Keep the texts horizontal"),
  onlyDimensionText: z.boolean().optional().describe("Show only the texts (no lines/markers)"),
  layout: z
    .enum(["Legacy", "Flexible", "Centered", "Off"])
    .optional()
    .describe("Text layout algorithm for crowded chains ('Flexible' moves colliding texts)"),
  clipOtherSide: z.boolean().optional().describe("Do not draw the witness line part beyond the dimensioned point"),
  ...MarkerFields,
  ...TextStyleFields,
};

// --- Linear dimensions ------------------------------------------------------------------------

const DimensionPoint = z
  .object({
    x: z.number().optional().describe("X in m. Alone with y: a static point. With 'element': a point NEAR the element point to link (snapped within snapTolerance)"),
    y: z.number().optional().describe("Y in m (see x)"),
    element: ElementRef.optional().describe("Link this point to an element (associative: the dimension follows when the element moves)"),
    at: z
      .enum(["begin", "end"])
      .optional()
      .describe("With a Wall / Beam / Line 'element': link to the begin or end of its reference line (no x,y needed)"),
    inIndex: z
      .number()
      .int()
      .optional()
      .describe(
        "EXPERT: explicit Archicad reference (API_Base) — copy element/inIndex/line/special from an existing dimension's details or get_dimension_anchors; " +
          "needs x,y too (approximate location). Wall thickness (DevKit convention): {element: wall, inIndex: 11, line: true, x, y} and " +
          "{element: wall, inIndex: 21, line: true, x, y} — the two wall faces — with a direction perpendicular to the wall",
      ),
    line: z.boolean().optional().describe("EXPERT (with inIndex): the reference is a line/edge of the element, not a point"),
    special: z.boolean().optional().describe("EXPERT (with inIndex): Archicad's 'special' reference flag (wall corners, opening holes)"),
    nodeId: z.number().int().min(0).optional().describe("EXPERT (with inIndex): polygon vertex id"),
    witnessForm: WitnessForm.optional().describe("Witness line form of this point only"),
    witnessVal: WitnessVal.optional(),
    text: CustomText.optional().describe(
      "Custom text shown at this point: in a 'Normal' chain the text of the segment ENDING here (e.g. 'EQ'; meaningless on the first point), " +
        "in 'Cumulative'/'Elevation' chains the point's own value",
    ),
  })
  .refine((p) => (p.x === undefined) === (p.y === undefined), { message: "give both x and y (or neither)" })
  .refine((p) => p.x !== undefined || p.element !== undefined, { message: "each point needs x,y (static point) or element (+ at, or x,y near the point to link)" })
  .refine((p) => p.element === undefined || p.at !== undefined || p.x !== undefined, {
    message: "a point on an element needs 'at' ('begin'|'end' of a wall/beam/line) or x,y near the element point to link (see get_dimension_anchors)",
  })
  .refine((p) => p.at === undefined || p.element !== undefined, { message: "'at' needs 'element' (a Wall, Beam or Line)" })
  .refine((p) => p.inIndex === undefined || (p.element !== undefined && p.x !== undefined), { message: "inIndex needs element and x,y" })
  .describe(
    "Dimension point: {x, y} (static) | {element, at: 'begin'|'end'} (wall/beam/line end, associative) | {element, x, y} (element point nearest to x,y, associative)",
  );

const LinearGeometryFields = {
  points: z
    .array(DimensionPoint)
    .min(2)
    .max(1000)
    .describe("The measured points IN ORDER along the chain (at least 2). Each consecutive pair gives one segment"),
  direction: Direction.optional().describe(
    "Dimension line direction (default: from the first point towards the next distinct point). Distances are measured PROJECTED on this direction, " +
      "e.g. 'Horizontal' gives X distances even for diagonal points",
  ),
  linePoint: Point2D.optional().describe("A point the dimension line passes through (m). Alternative to offset"),
  offset: z
    .number()
    .optional()
    .describe("Distance (m) of the dimension line from the FIRST point, perpendicular to the direction: + = left of the direction (above a left-to-right chain), - = right/below. Default 1"),
  associative: z.boolean().optional().describe("Link points that reference elements (default true). false = every point is static"),
  snapTolerance: z
    .number()
    .min(0)
    .optional()
    .describe("Max distance (m, default 0.02) between an {element, x, y} point and the element anchor it links to; farther points stay static"),
};

const LinearDimensionSpec = z.object({ ...LinearGeometryFields, ...LinearStyleFields, ...DraftingCommonFields });

// --- Level dimensions ---------------------------------------------------------------------------

const LevelFields = {
  position: Point2D.describe("Where the level marker is placed (m)"),
  element: ElementRef.optional().describe(
    "Show the level of this element at the marker (Slab, Mesh, Roof, Shell, Stair, Object...: typically its top surface). Omit = the story level",
  ),
  level: z.number().optional().describe("Static level value in m (makes the marker static: it no longer follows the story/element)"),
  static: z.boolean().optional().describe("Static marker (keeps its value); set false to follow the story/element again"),
  elevationReference: z
    .enum(["ProjectZero", "ReferenceLevel1", "ReferenceLevel2", "StoredOrigin", "SeaLevel"])
    .optional()
    .describe(
      "What the shown elevation is measured from: 'ProjectZero', 'ReferenceLevel1'/'ReferenceLevel2' (the project's reference levels), " +
        "'StoredOrigin', 'SeaLevel' (altitude). Default from the Level Dimension tool (usually ProjectZero)",
    ),
  markerStyle: z.number().int().min(0).max(9).optional().describe("Marker form 0..9 (Archicad's level dimension marker types)"),
  markerSize: z.number().positive().optional().describe("Marker size in PAPER millimeters"),
  pen: Pen.optional().describe("Pen of the marker"),
  markerAngle: z.number().optional().describe("Rotation of the marker in degrees"),
  showPlusSign: z.boolean().optional().describe("Show '+' before positive values"),
  text: CustomText.optional(),
  secondText: CustomText.optional().describe("Second note (marker styles 8 and 9 only)"),
  ...TextStyleFields,
};

const LevelDimensionSpec = z.object({ ...LevelFields, ...DraftingCommonFields });

// --- Radial dimensions ----------------------------------------------------------------------------

const RadialStyleFields = {
  showCenter: z.boolean().optional().describe("Mark the arc center"),
  prefix: z.string().max(64).optional().describe("Text before the value, e.g. 'R' or 'R='"),
  textPosition: TextPosition.optional(),
  textDirection: z.enum(["Horizontal", "Vertical", "Radial"]).optional().describe("Text orientation: 'Radial' follows the radius line"),
  linePen: Pen.optional().describe("Pen of the dimension line"),
  onlyDimensionText: z.boolean().optional().describe("Show only the text"),
  text: CustomText.optional(),
  ...MarkerFields,
  ...TextStyleFields,
};

const RadialDimensionSpec = z.object({
  element: ElementRef.describe("The Arc, Circle, curved Wall (arcAngle != 0) or curved Beam to dimension"),
  at: Point2D.optional().describe("A point near the arc where the dimension touches it (projected onto the arc). Default: the middle of the arc"),
  lineEnd: Point2D.optional().describe("End point of the radial dimension line (default: the arc point). Place it outside the arc to pull the text out"),
  inIndex: z.number().int().optional().describe("EXPERT: explicit reference index of the element (copy from an existing radial dimension's details)"),
  line: z.boolean().optional().describe("EXPERT (with inIndex): reference is the element's line (default true)"),
  special: z.boolean().optional().describe("EXPERT (with inIndex)"),
  ...RadialStyleFields,
  ...DraftingCommonFields,
});

// --- Angle dimensions ----------------------------------------------------------------------------

const LineSpec = z.object({ begin: Point2D, end: Point2D }).describe("Straight line {begin, end} in m");

const AngleStyleFields = {
  arcPoint: Point2D.optional().describe("A point the dimension arc passes through: picks the measured sector and the arc radius"),
  radius: z.number().positive().optional().describe("Arc radius in m (default 1; with arcPoint: keeps its direction, sets the distance)"),
  smallArc: z.boolean().optional().describe("Measure the smaller angle between the lines (default true)"),
  textPosition: TextPosition.optional(),
  textDirection: z.enum(["Parallel", "Horizontal", "Perpendicular"]).optional().describe("Text orientation relative to the arc"),
  witnessForm: WitnessForm.optional(),
  witnessVal: WitnessVal.optional(),
  linePen: Pen.optional().describe("Pen of the arc and witness lines"),
  onlyDimensionText: z.boolean().optional(),
  text: CustomText.optional(),
  ...MarkerFields,
  ...TextStyleFields,
};

const AngleDimensionSpec = z
  .object({
    line1: LineSpec.optional().describe("First line (static)"),
    line2: LineSpec.optional().describe("Second line (static)"),
    elements: z
      .array(ElementRef)
      .length(2)
      .optional()
      .describe("Instead of line1/line2: two straight Walls / Beams / Lines (their reference lines; linked to their end points when possible)"),
    associative: z.boolean().optional().describe("With elements: link to the element end points (default true)"),
    ...AngleStyleFields,
    ...DraftingCommonFields,
  })
  .refine((a) => a.elements !== undefined || (a.line1 !== undefined && a.line2 !== undefined), {
    message: "give 'elements' (two walls/beams/lines) or both 'line1' and 'line2'",
  });

// --- Modify ----------------------------------------------------------------------------------------

const DimensionPatch = z
  .object({
    guid: Guid.describe("Dimension GUID (Dimension, LevelDimension, RadialDimension or AngleDimension)"),
    // linear
    points: LinearGeometryFields.points.optional().describe("Linear: REPLACE all points (same format as create_dimensions)"),
    direction: Direction.optional().describe("Linear: new dimension line direction"),
    linePoint: Point2D.optional().describe("Linear: MOVE the dimension line so that it passes through this point"),
    offset: z.number().optional().describe("Linear: move the dimension line to this distance (m) from the first point (+ = left of the direction)"),
    makeStatic: z.boolean().optional().describe("Linear: detach every point from its element (points stay where they are)"),
    pointTexts: z
      .array(
        z.object({
          index: z.number().int().min(0).describe("0-based index into the dimension's points (as listed by get_element_details)"),
          text: CustomText,
        }),
      )
      .min(1)
      .optional()
      .describe(
        "Linear: set custom texts of single points WITHOUT replacing the points, e.g. [{index: 2, text: 'EQ'}] (in a Normal chain point i " +
          "carries the text of the segment from point i-1 to i); text \"\"/false restores the measured value",
      ),
    associative: z.boolean().optional().describe("Linear, with points: link element points (default true)"),
    snapTolerance: z.number().min(0).optional().describe("Linear, with points: see create_dimensions"),
    appearance: LinearStyleFields.appearance,
    horizontalText: LinearStyleFields.horizontalText,
    layout: LinearStyleFields.layout,
    clipOtherSide: LinearStyleFields.clipOtherSide,
    // level
    position: Point2D.optional().describe("Level: move the marker"),
    element: z
      .union([ElementRef, z.literal(""), z.literal(false)])
      .optional()
      .describe("Level: show this element's level; \"\" or false = back to the story level"),
    level: LevelFields.level,
    static: LevelFields.static,
    elevationReference: LevelFields.elevationReference,
    markerStyle: LevelFields.markerStyle,
    pen: LevelFields.pen,
    markerAngle: LevelFields.markerAngle,
    showPlusSign: LevelFields.showPlusSign,
    secondText: LevelFields.secondText,
    // radial
    at: Point2D.optional().describe("Radial: move the point where the dimension touches the arc"),
    lineEnd: Point2D.optional().describe("Radial: new end of the radial line"),
    showCenter: RadialStyleFields.showCenter,
    prefix: RadialStyleFields.prefix,
    // angle
    line1: LineSpec.optional().describe("Angle: new first line (makes the dimension static)"),
    line2: LineSpec.optional().describe("Angle: new second line (makes the dimension static)"),
    arcPoint: AngleStyleFields.arcPoint,
    radius: AngleStyleFields.radius,
    smallArc: AngleStyleFields.smallArc,
    // shared
    textPosition: TextPosition.optional(),
    textDirection: z
      .enum(["Parallel", "Horizontal", "Vertical", "Radial", "Perpendicular"])
      .optional()
      .describe("Linear: Parallel|Horizontal|Vertical; Radial: Horizontal|Vertical|Radial; Angle: Parallel|Horizontal|Perpendicular"),
    linePen: Pen.optional().describe("Pen of the dimension lines (linear, radial, angle)"),
    witnessForm: WitnessForm.optional().describe("Linear (applied to every point) and angle"),
    witnessVal: WitnessVal.optional(),
    onlyDimensionText: z.boolean().optional(),
    text: CustomText.optional().describe("Level / radial / angle: custom text (\"\" = measured value). Linear: use pointTexts (or points[].text)"),
    ...MarkerFields,
    ...TextStyleFields,
    ...DraftingCommonFields,
  })
  .describe("Fields to change on one dimension; only the fields that apply to its type are used");

// --- Helpers -----------------------------------------------------------------------------------------

type Json = Record<string, unknown>;

function refToGuid(value: unknown): unknown {
  if (value && typeof value === "object" && "guid" in (value as Json)) return (value as Json)["guid"];
  return value;
}

/** Normalizes element references ({guid} → "guid") so the add-on always receives GUID strings. */
function normalizeSpec(spec: Json): Json {
  const out: Json = { ...spec };
  if (out["element"] !== undefined) out["element"] = refToGuid(out["element"]);
  if (Array.isArray(out["elements"])) out["elements"] = (out["elements"] as unknown[]).map(refToGuid);
  if (Array.isArray(out["points"])) {
    out["points"] = (out["points"] as Json[]).map((p) => (p.element !== undefined ? { ...p, element: refToGuid(p.element) } : p));
  }
  return out;
}

const SUMMARY_KEYS: Record<string, string[]> = {
  Dimension: ["pointCount", "associativePoints", "segments", "total", "linePoint", "directionAngle"],
  LevelDimension: ["position", "level", "static", "element", "elementType", "storyLevel", "elevationReference"],
  RadialDimension: ["radius", "associative", "element", "elementType", "arcPoint", "lineEnd"],
  AngleDimension: ["angle", "radius", "center", "arcPoint", "associative"],
};

function summarize(detail: Json): Json {
  const type = String(detail["type"] ?? "");
  const d = (detail["details"] ?? {}) as Json;
  const out: Json = {};
  for (const k of SUMMARY_KEYS[type] ?? []) if (d[k] !== undefined) out[k] = d[k];
  return out;
}

/** Number of points of a linear spec that should become associative. */
function expectedLinkedPoints(spec: Json): number {
  if (spec["associative"] === false || !Array.isArray(spec["points"])) return 0;
  return (spec["points"] as Json[]).filter((p) => p.element !== undefined).length;
}

/**
 * Adds a compact summary (measured values, associativity) of every created/modified dimension to the
 * per-item results, and warns when element-referencing points ended up static.
 */
async function withSummaries(ctx: ToolContext, response: unknown, specs?: Json[]): Promise<unknown> {
  const results = (response as { results?: Json[] })?.results;
  if (!Array.isArray(results)) return response;
  const guids = results.map((r) => r["guid"]).filter((g): g is string => typeof g === "string");
  if (guids.length === 0) return response;
  let details: Json[] = [];
  try {
    const res = await ctx.ac.addon<{ elements?: Json[] }>("GetElementDetails", { elements: guids });
    details = res.elements ?? [];
  } catch {
    return response; // the dimensions exist; details are a convenience
  }
  const byGuid = new Map<string, Json>();
  for (const d of details) if (typeof d["guid"] === "string") byGuid.set(d["guid"] as string, d);
  return {
    ...(response as Json),
    results: results.map((r, i) => {
      const detail = typeof r["guid"] === "string" ? byGuid.get(r["guid"] as string) : undefined;
      if (!detail) return r;
      const summary = summarize(detail);
      const item: Json = { ...r, ...summary };
      const spec = specs?.[i];
      if (spec && detail["type"] === "Dimension") {
        const expected = expectedLinkedPoints(spec);
        const linked = Number(summary["associativePoints"] ?? 0);
        if (expected > linked) {
          item["warning"] =
            `${expected - linked} of ${expected} element-referencing point(s) could not be linked and are static (they will not follow the elements). ` +
            "Check the points with get_element_details / get_dimension_anchors.";
        }
      }
      return item;
    }),
  };
}

// --- Registration ------------------------------------------------------------------------------------

export function registerDimensionTools(server: McpServer, ctx: ToolContext): void {
  defineTool(server, ctx, {
    name: "create_dimensions",
    title: "Create linear dimensions",
    description:
      "Creates linear dimension chains (one undo step). Each dimension = ordered 'points' + a dimension line (direction + linePoint/offset). " +
      "Points: {x, y} static, or linked to elements (associative, the dimension follows edits): {element, at: 'begin'|'end'} for wall/beam/line " +
      "reference-line ends, {element, x, y} for the element anchor (hotspot, corner, opening point) nearest to x,y — list anchors with " +
      "get_dimension_anchors. Distances are measured along 'direction' (e.g. 'Horizontal' = X distances). Units: m; text/marker sizes in paper mm. " +
      "Style defaults come from the Dimension tool settings. Dimensions linked to model elements are placed on the Floor Plan (story: storyIndex, " +
      "default current); static ones go into the active window (plan, section, detail, worksheet, layout). Linked points that Archicad cannot " +
      "resolve fall back to static (see 'warning'). For walls prefer dimension_walls (mode 'Thickness' for wall widths). Returns [{guid, type, pointCount, associativePoints, " +
      "segments (measured lengths), total, linePoint} | {error}] in input order; one failing item does not stop the others.",
    input: { dimensions: z.array(LinearDimensionSpec).min(1).max(200) },
    annotations: CREATES,
    handler: async ({ dimensions }, c) => {
      const specs = dimensions.map((d) => normalizeSpec(d as Json));
      const res = await createElements(c, specs.map((s) => ({ type: "Dimension", ...s })), "Create dimensions (Claude)");
      return withSummaries(c, res, specs);
    },
  });

  defineTool(server, ctx, {
    name: "create_level_dimensions",
    title: "Create level dimensions",
    description:
      "Places level (elevation) dimension markers (one undo step). Each marker shows the story level at 'position', or the " +
      "level of an 'element' (slab/mesh/roof/stair top), or a static 'level' value. Markers on an element are placed on the Floor Plan " +
      "(story: storyIndex, default current); the others go into the active window (normally the Floor Plan). Units: m, degrees, " +
      "marker/text sizes in paper mm. " +
      "Unspecified settings come from the Level Dimension tool. Returns [{guid, type, position, level, static, element?, storyLevel} | {error}].",
    input: { levelDimensions: z.array(LevelDimensionSpec).min(1).max(500) },
    annotations: CREATES,
    handler: async ({ levelDimensions }, c) => {
      const specs = levelDimensions.map((d) => normalizeSpec(d as Json));
      const res = await createElements(c, specs.map((s) => ({ type: "LevelDimension", ...s })), "Create level dimensions (Claude)");
      return withSummaries(c, res);
    },
  });

  defineTool(server, ctx, {
    name: "create_radial_dimensions",
    title: "Create radial dimensions",
    description:
      "Dimensions the radius of arcs, circles, curved walls or curved beams (one undo step). The dimension is linked to the element and " +
      "verified (the measured radius must match the element). 'at' picks where it touches the arc, 'lineEnd' where the radial line ends. " +
      "Walls/beams must be on the Floor Plan; arcs/circles in the active window's drawing. Returns [{guid, type, radius, associative, " +
      "element, arcPoint, lineEnd} | {error}].",
    input: { radialDimensions: z.array(RadialDimensionSpec).min(1).max(200) },
    annotations: CREATES,
    handler: async ({ radialDimensions }, c) => {
      const specs = radialDimensions.map((d) => normalizeSpec(d as Json));
      const res = await createElements(c, specs.map((s) => ({ type: "RadialDimension", ...s })), "Create radial dimensions (Claude)");
      return withSummaries(c, res);
    },
  });

  defineTool(server, ctx, {
    name: "create_angle_dimensions",
    title: "Create angle dimensions",
    description:
      "Dimensions the angle between two non-parallel lines (one undo step): static 'line1'/'line2' {begin, end}, or two straight walls/beams/lines " +
      "('elements', linked to their end points when possible, verified, else static). The arc is centered on the lines' intersection; " +
      "'arcPoint' (a point on the arc) or 'radius' (m, default 1) places it; by default it sits between the far ends of both lines. " +
      "Returns [{guid, type, angle (degrees), radius, center, arcPoint, associative} | {error}].",
    input: { angleDimensions: z.array(AngleDimensionSpec).min(1).max(200) },
    annotations: CREATES,
    handler: async ({ angleDimensions }, c) => {
      const specs = angleDimensions.map((d) => normalizeSpec(d as Json));
      const res = await createElements(c, specs.map((s) => ({ type: "AngleDimension", ...s })), "Create angle dimensions (Claude)");
      return withSummaries(c, res);
    },
  });

  defineTool(server, ctx, {
    name: "dimension_walls",
    title: "Dimension walls",
    description:
      "Automatically dimensions walls on the Floor Plan (one undo step): wall end points plus window/door jambs (or centers), linked to the " +
      "walls/openings so the dimensions follow later edits. mode 'Chain' (default) = ONE chain through all given walls, measured along " +
      "'direction' (default: first wall's begin→end) — e.g. pass all walls of a facade, including the perpendicular ones whose ends should " +
      "appear; mode 'EachWall' = one dimension parallel to each wall; mode 'Thickness' = one dimension ACROSS each straight wall, linked to " +
      "both wall faces (shows the wall width, follows thickness changes), crossing the wall at 'thicknessPosition'. For Chain/EachWall the " +
      "line is placed 'distance' m beyond the outermost wall face on 'side' ('Auto' = away from the first wall's body); 'overall' adds a " +
      "second line with the total length. Get wall GUIDs first (e.g. list_elements with types ['Wall'], find_elements or get_selection). " +
      "Returns {results: [{guid, role: 'chain'|'overall'|'thickness', walls, pointCount, associativePoints, segments, total, linePoint, " +
      "warnings?} | {error, wall?}]}; in EachWall/Thickness mode one failing wall does not stop the others.",
    input: {
      walls: ElementRefs.max(500).describe("Wall GUIDs"),
      mode: z
        .enum(["Chain", "EachWall", "Thickness"])
        .optional()
        .describe("'Chain' (default): one chain for all walls; 'EachWall': one dimension per wall; 'Thickness': one width dimension across each wall"),
      direction: Direction.optional().describe("Chain direction (default: the first wall's begin→end direction). Not with mode 'Thickness'"),
      side: z
        .enum(["Auto", "Left", "Right"])
        .optional()
        .describe("Side of the dimension line relative to the first wall's begin→end (or 'direction'): 'Auto' (default) = the side without the wall body"),
      distance: z.number().min(0).optional().describe("Distance (m) from the outermost wall face to the dimension line (default 1)"),
      includeOpenings: z.boolean().optional().describe("Dimension windows/doors of walls parallel to the chain (default true)"),
      openingPoints: z.enum(["Jambs", "Centers"]).optional().describe("'Jambs' (default: both sides of each opening) or 'Centers'"),
      overall: z.boolean().optional().describe("Also create an overall dimension (first to last point) further out"),
      overallSpacing: z.number().positive().optional().describe("Distance (m) between the chain and the overall line (default 0.8)"),
      thicknessPosition: z
        .number()
        .optional()
        .describe(
          "Mode 'Thickness': where the dimension crosses the wall, in m from the wall's begin point along its reference line (default: the " +
            "middle). Values < 0 or > the wall length put the dimension beyond the wall ends",
        ),
      associative: z.boolean().optional().describe("Link points to walls/openings/wall faces (default true); false = static points"),
      storyIndex: DraftingCommonFields.storyIndex.describe(
        "Story of the dimensions (default: the wall's story — in 'Chain' mode the first wall's)",
      ),
      layer: DraftingCommonFields.layer,
      ...LinearStyleFields,
      undoName: z.string().optional().describe("Name of the undo step"),
    },
    annotations: CREATES,
    handler: async (args, { ac }) => {
      if (args.mode === "Thickness" && args.direction !== undefined) {
        throw new Error("'direction' does not apply to mode 'Thickness' (the dimension is always perpendicular to the wall); remove it.");
      }
      return ac.addon("DimensionWalls", { ...args, walls: args.walls.map(guidOf) });
    },
  });

  defineTool(server, ctx, {
    name: "modify_dimensions",
    title: "Modify dimensions",
    description:
      "Changes existing dimensions in one undo step. Each item is {guid, ...fields}; only the given fields change and only fields of the " +
      "dimension's type apply. Linear: move the dimension line (linePoint, or offset from the first point), change direction, replace points, " +
      "pointTexts (custom segment texts), makeStatic, style (pens, markers, texts, witness lines). Level: position, element, level/static, " +
      "elevationReference, marker, text. Radial: at, " +
      "lineEnd, prefix, showCenter. Angle: arcPoint/radius, smallArc, line1/line2. Common: layer, storyIndex. Read the current " +
      "values with get_element_details. Returns [{guid, ...summary} | {error}].",
    input: {
      dimensions: z.array(DimensionPatch).min(1).max(500),
      undoName: z.string().optional().describe("Name of the undo step"),
    },
    annotations: MODIFIES,
    handler: async ({ dimensions, undoName }, c) => {
      const patches = dimensions.map((d) => normalizeSpec(d as Json));
      const res = await c.ac.addon("ModifyElements", { elements: patches, undoName: undoName ?? "Modify dimensions (Claude)" });
      return withSummaries(c, res);
    },
  });

  defineTool(server, ctx, {
    name: "get_dimension_anchors",
    title: "Dimension anchor points",
    description:
      "Lists the points of elements that dimensions can link to (hotspots: wall ends and corners, column corners, opening points, slab " +
      "vertices, object hotspots...), with coordinates in m. Use a point's x,y with {element, x, y} in create_dimensions to create an " +
      "associative point. Pass 'near' to sort by distance (and 'radius' to filter). Only anchors with usableAsPoint = true can be linked. " +
      "Model elements (walls, columns, slabs, openings...) are read on the Floor Plan (plan coordinates) whatever window is active. " +
      "Returns {elements: [{guid, type, anchorCount, anchors: [{x, y, z, neig, inIndex, line, special, usableAsPoint, source, distance?}]} | {error}]}.",
    input: {
      elements: ElementRefs.max(200),
      near: Point2D.optional().describe("Sort anchors by distance to this point"),
      radius: z.number().positive().optional().describe("With near: only anchors within this distance (m)"),
      limit: z.number().int().min(1).max(2000).optional().describe("Max anchors per element (default 100)"),
    },
    annotations: READ_ONLY,
    handler: async (args, { ac }) => ac.addon("GetDimensionAnchors", { ...args, elements: args.elements.map(guidOf) }),
  });
}
