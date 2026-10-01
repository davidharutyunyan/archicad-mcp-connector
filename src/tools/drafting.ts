/**
 * Drafting tools — 2D elements: lines, arcs, circles, polylines, splines, hatches (fills), texts,
 * labels, hotspots and pictures. The add-on adapters live in addon/Src/Commands/Drafting.cpp and
 * DraftingText.cpp; field names match 1:1, so the same fields work with modify_elements and are
 * returned by get_element_details.
 *
 * Units: coordinates/lengths in METERS (model), angles in DEGREES (counter-clockwise from +X),
 * text sizes, arrow sizes and line weights in MILLIMETERS (paper).
 */

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import { CREATES, defineTool, type ToolContext } from "./define.js";
import { createElements } from "./elements.js";
import { ArcSpec, AttrRef, CommonElementFields, DraftingCommonFields, ElementRef, GdlParams, guidOf, LibPartRef, Point2D, Polygon } from "./schemas.js";

// --- Shared building blocks ------------------------------------------------------------

const Pen = z.number().int().min(1).max(255);
const PenOrZero = z.number().int().min(0).max(255);

const WHERE =
  "Elements go into the database of the ACTIVE window: on a floor plan the given storyIndex (default: current story), " +
  "otherwise the open section / elevation / detail / worksheet / layout (a 3D window cannot hold 2D elements).";

export const ARROW_TYPES = [
  "EmptyCircle",
  "CrossCircle",
  "FullCircle",
  "OpenArrow15",
  "ClosedArrow15",
  "FullArrow15",
  "OpenArrow30",
  "ClosedArrow30",
  "FullArrow30",
  "SlashLine45",
  "OpenArrow45",
  "ClosedArrow45",
  "FullArrow45",
  "SlashLine60",
  "SlashLine75",
  "SlashLine90",
  "PepitaCircle",
  "BandArrow",
  "HalfArrowCcw15",
  "HalfArrowCw15",
  "HalfArrowCcw30",
  "HalfArrowCw30",
  "HalfArrowCcw45",
  "HalfArrowCw45",
] as const;

export const Arrows = z
  .object({
    begin: z.boolean().optional().describe("Arrow at the start point"),
    end: z.boolean().optional().describe("Arrow at the end point"),
    type: z.enum(ARROW_TYPES).optional().describe("Arrowhead shape (number = opening angle in degrees)"),
    size: z.number().positive().optional().describe("Arrowhead size in mm (paper)"),
    pen: Pen.optional().describe("Pen of the arrowhead (1-255)"),
  })
  .describe("Arrowheads (omitted fields keep the tool defaults)");

const LineWeight = z
  .number()
  .refine((v) => v > 0 || v === -1, "must be a weight in mm (> 0) or -1")
  .describe("Line weight override in mm (e.g. 0.35); -1 = use the pen's own weight (default)");

/** Style fields of every line-type element (line, arc, circle, polyline, spline). */
export const LineStyleFields = {
  pen: Pen.optional().describe("Pen index 1-255 (colour + weight). Default: tool setting"),
  colorOverridePen: PenOrZero.optional().describe("Draw with the COLOUR of this pen while keeping the weight of 'pen' (0 = no override)"),
  lineType: AttrRef.optional().describe(
    "Line type attribute: exact LOCALIZED name or index (1 is usually solid) — list them with get_attributes {type: 'Line'}",
  ),
  lineWeight: LineWeight.optional(),
  category: z
    .enum(["Drafting", "Cut", "SkinSeparator"])
    .optional()
    .describe("Line category: 'Drafting' (default drafting line), 'Cut' (cut/contour line), 'SkinSeparator' (inner skin line)"),
  zoneBoundary: z.boolean().optional().describe("true = the line acts as a zone (room) boundary for automatic zones"),
};

const ArrowsField = { arrows: Arrows.optional() };

const StyledContentFields = {
  pen: Pen.optional().describe("Text pen (colour)"),
  font: AttrRef.optional().describe("Font by name (e.g. 'Arial') or Font attribute index — list them with get_attributes {type: 'Font'}"),
  size: z.number().positive().optional().describe("Character height in mm (paper), e.g. 2.5"),
  bold: z.boolean().optional(),
  italic: z.boolean().optional(),
  underline: z.boolean().optional(),
  strikeout: z.boolean().optional(),
  superscript: z.boolean().optional(),
  subscript: z.boolean().optional(),
};

export const TextRun = z
  .object({
    text: z.string().describe("Text of this run; may contain '\\n' line breaks"),
    ...StyledContentFields,
  })
  .describe("A differently styled piece of text; omitted style fields use the element-level style");

const TextContentFields = {
  text: z
    .string()
    .min(1)
    .optional()
    .describe("Text content (UTF-8, any language). Use '\\n' for new lines. Give either text or runs"),
  runs: z
    .array(TextRun)
    .min(1)
    .optional()
    .describe("Rich text: runs concatenated in order, each with its own style (e.g. a bold title run + a normal run). Give either text or runs"),
  ...StyledContentFields,
  justification: z.enum(["Left", "Center", "Right", "Full"]).optional().describe("Paragraph alignment"),
  lineSpacing: z
    .number()
    .optional()
    .describe("Archicad line-spacing value of the paragraphs (advanced; keep the tool default unless you know the convention)"),
};

const TextLayoutFields = {
  angle: z.number().optional().describe("Rotation in degrees (counter-clockwise)"),
  widthFactor: z.number().positive().optional().describe("Character width factor (1 = normal, 0.8 = condensed)"),
  charSpacing: z.number().positive().optional().describe("Character spacing factor (1 = normal)"),
  wrapWidth: z
    .number()
    .min(0)
    .optional()
    .describe("Wrap the text inside a box of this width in mm (paper); 0 = no wrapping (lines break only at '\\n')"),
  fixedSize: z.boolean().optional().describe("true = the text size does not scale with the output scale"),
  framePen: Pen.optional().describe("Pen of the text frame"),
  background: z.boolean().optional().describe("Opaque background behind the text"),
  backgroundPen: Pen.optional().describe("Pen (colour) of the background"),
  alwaysReadable: z.boolean().optional().describe("Keep the text readable (flip it) when rotated upside down"),
};

function exactlyOneContent(v: { text?: unknown; runs?: unknown }): boolean {
  return (v.text === undefined) !== (v.runs === undefined);
}

const ColorValue = z
  .union([
    z.string().regex(/^#?[0-9A-Fa-f]{6}$/, "must be #RRGGBB"),
    z.object({ r: z.number().min(0).max(255), g: z.number().min(0).max(255), b: z.number().min(0).max(255) }),
    z.literal(false),
  ])
  .describe("RGB colour '#RRGGBB' or {r, g, b} (0-255); false removes the colour override");

// --- Element specs ---------------------------------------------------------------------

export const LineSpec = z.object({
  begin: Point2D.describe("Start point (m)"),
  end: Point2D.describe("End point (m), different from begin"),
  ...LineStyleFields,
  ...ArrowsField,
  ...DraftingCommonFields,
});

export const ArcSpecFields = z
  .object({
    center: Point2D.optional().describe("Form A: centre point (m)"),
    radius: z.number().positive().optional().describe("Form A: radius in m (for ellipses: the semi-axis along axisAngle)"),
    beginAngle: z.number().optional().describe("Form A: start angle in degrees, counter-clockwise from +X; the arc runs CCW to endAngle"),
    endAngle: z.number().optional().describe("Form A: end angle in degrees"),
    minorRadius: z.number().positive().optional().describe("Form A, elliptical arc: the other semi-axis in m (default = radius)"),
    ratio: z.number().positive().optional().describe("Form A, elliptical arc: radius / minorRadius (alternative to minorRadius)"),
    axisAngle: z.number().optional().describe("Form A, elliptical arc: direction of the main axis in degrees"),
    begin: Point2D.optional().describe("Forms B/C: start point of the arc (m)"),
    end: Point2D.optional().describe("Forms B/C: end point of the arc (m)"),
    arcAngle: z
      .number()
      .optional()
      .describe("Form B: signed sweep in degrees (+ = counter-clockwise from begin to end, - = clockwise), e.g. 90 or -180"),
    through: Point2D.optional().describe("Form C: a third point the arc passes through between begin and end"),
    reflected: z.boolean().optional().describe("Archicad 'reflected' flag (mirrored elliptical arc)"),
    ...LineStyleFields,
    ...ArrowsField,
    ...DraftingCommonFields,
  })
  .superRefine((v, ctx) => {
    const formA = v.center !== undefined || v.radius !== undefined || v.beginAngle !== undefined || v.endAngle !== undefined;
    const formBC = v.begin !== undefined || v.end !== undefined || v.arcAngle !== undefined || v.through !== undefined;
    const fail = (message: string) => ctx.addIssue({ code: z.ZodIssueCode.custom, message });
    if (formA && formBC) fail("give EITHER center/radius/beginAngle/endAngle (form A) OR begin/end + arcAngle|through (forms B/C)");
    else if (formA) {
      if (v.center === undefined || v.radius === undefined || v.beginAngle === undefined || v.endAngle === undefined)
        fail("form A needs center, radius, beginAngle and endAngle");
    } else if (formBC) {
      if (v.begin === undefined || v.end === undefined) fail("forms B/C need begin and end");
      if ((v.arcAngle === undefined) === (v.through === undefined)) fail("give exactly one of arcAngle (form B) or through (form C)");
      if (v.minorRadius !== undefined || v.ratio !== undefined || v.axisAngle !== undefined)
        fail("minorRadius/ratio/axisAngle (elliptical arcs) need form A");
    } else fail("an arc needs center + radius + beginAngle + endAngle, or begin + end + arcAngle, or begin + through + end");
    if (v.minorRadius !== undefined && v.ratio !== undefined) fail("give either minorRadius or ratio, not both");
  });

export const CircleSpec = z
  .object({
    center: Point2D.describe("Centre point (m)"),
    radius: z.number().positive().describe("Radius in m (for ellipses: the semi-axis along axisAngle)"),
    minorRadius: z.number().positive().optional().describe("Ellipse: the other semi-axis in m (default = radius, i.e. a circle)"),
    ratio: z.number().positive().optional().describe("Ellipse: radius / minorRadius (alternative to minorRadius)"),
    axisAngle: z.number().optional().describe("Ellipse: direction of the main axis in degrees"),
    ...LineStyleFields,
    ...DraftingCommonFields,
  })
  .refine((v) => v.minorRadius === undefined || v.ratio === undefined, "give either minorRadius or ratio, not both");

export const PolyLineSpec = z.object({
  points: z.array(Point2D).min(2).describe("Vertices in order (m). For a closed shape set closed: true instead of repeating the first point"),
  arcs: z.array(ArcSpec).optional().describe("Curved segments: {index, angle} makes the edge points[index] -> points[index+1] an arc (+ = CCW)"),
  closed: z.boolean().optional().describe("Close the polyline back to the first point (the closing edge has index points.length-1)"),
  continuousPattern: z
    .boolean()
    .optional()
    .describe("true = the line type pattern runs continuously around corners; false = it restarts on every segment"),
  ...LineStyleFields,
  ...ArrowsField,
  ...DraftingCommonFields,
});

export const SplineDirection = z
  .object({
    angle: z.number().describe("Tangent direction at the point in degrees"),
    lengthPrev: z.number().min(0).optional().describe("Length of the incoming Bezier handle in m (default 0)"),
    lengthNext: z.number().min(0).optional().describe("Length of the outgoing Bezier handle in m (default 0)"),
  })
  .describe("Bezier handle of one control point");

export const SplineSpec = z
  .object({
    points: z.array(Point2D).min(2).describe("Points the spline passes through, in order (m); closed splines need >= 3"),
    closed: z.boolean().optional().describe("Closed curve (do not repeat the first point)"),
    directions: z
      .array(SplineDirection)
      .optional()
      .describe("Explicit Bezier handles, one per point. Omit for a smooth automatic (natural) spline"),
    ...LineStyleFields,
    ...ArrowsField,
    ...DraftingCommonFields,
  })
  .refine((v) => v.directions === undefined || v.directions.length === v.points.length, "directions needs one entry per point")
  .refine((v) => !v.closed || v.points.length >= 3, "a closed spline needs at least 3 points");

export const HatchOrientation = z
  .object({
    type: z
      .enum(["Global", "Rotated", "Distorted", "Radial"])
      .optional()
      .describe("Pattern orientation: 'Global' (aligned to the project origin), 'Rotated' (angle), 'Distorted' (xAxis/yAxis), 'Radial' (centered)"),
    origin: Point2D.optional().describe("Local origin of the fill pattern (m); makes the origin local"),
    angle: z.number().optional().describe("Pattern rotation in degrees (sets type 'Rotated' unless type is given)"),
    xAxis: Point2D.optional().describe("Distortion: pattern X direction vector (sets type 'Distorted'); give together with yAxis"),
    yAxis: Point2D.optional().describe("Distortion: pattern Y direction vector"),
    innerRadius: z.number().min(0).optional().describe("Radial fills: inner radius in m (0 = none)"),
    fitX: z.boolean().optional().describe("Symbol fills: stretch the pattern width to the xAxis length"),
    fitY: z.boolean().optional().describe("Symbol fills: stretch the pattern height to the yAxis length"),
    keepProportion: z.boolean().optional().describe("Symbol fills: keep the pattern proportion when fitting"),
    globalOrigin: z.boolean().optional().describe("true = pattern origin at the project origin"),
  })
  .describe("Fill pattern placement");

export const HatchSpec = z.object({
  polygon: Polygon.describe(
    "Fill outline (m): array of points or {points, arcs?, holes?: [{points, arcs?}]}. Do not repeat the first point; orientation is normalized",
  ),
  fillType: AttrRef.optional().describe(
    "Fill attribute (pattern): exact LOCALIZED name or index — list with get_attributes {type: 'Fill'}. Makes it a plain fill hatch " +
      "(with buildingMaterial: overrides the material's fill)",
  ),
  buildingMaterial: AttrRef.optional().describe(
    "Building material: the hatch shows the material's cut fill and pens (get_attributes {type: 'BuildingMaterial'})",
  ),
  fillPen: Pen.optional().describe("Foreground (pattern) pen; on a building-material hatch this overrides the material's pen"),
  fillColorOverridePen: PenOrZero.optional().describe("Draw the pattern with this pen's colour (0 = none)"),
  backgroundPen: PenOrZero.optional().describe("Background pen (0 = transparent); overrides the material's on building-material hatches"),
  overrideBuildingMaterialPens: z
    .boolean()
    .optional()
    .describe("Building-material hatch: true = use fillPen/backgroundPen, false = back to the material's pens"),
  foregroundColor: ColorValue.optional().describe("RGB override of the pattern colour ('#RRGGBB' | {r,g,b} | false)"),
  backgroundColor: ColorValue.optional().describe("RGB override of the background colour ('#RRGGBB' | {r,g,b} | false)"),
  fillCategory: z.enum(["Drafting", "Cut", "Cover"]).optional().describe("Fill category (affects graphic overrides/model view options)"),
  contour: z.boolean().optional().describe("Draw the contour line (false = no contour)"),
  contourPen: Pen.optional().describe("Contour pen (implies contour: true)"),
  contourColorOverridePen: PenOrZero.optional().describe("Draw the contour with this pen's colour (0 = none)"),
  contourLineType: AttrRef.optional().describe("Contour line type (name/index, get_attributes {type: 'Line'})"),
  contourLineWeight: LineWeight.optional(),
  orientation: HatchOrientation.optional(),
  showArea: z.boolean().optional().describe("Show the area text (placed in the middle of the polygon unless areaText.position is given)"),
  areaText: z
    .object({
      position: Point2D.optional().describe("Area text position (m)"),
      pen: Pen.optional(),
      font: AttrRef.optional().describe("Font name or index"),
      size: z.number().positive().optional().describe("Text height in mm"),
      angle: z.number().optional().describe("Rotation in degrees"),
    })
    .optional()
    .describe("Area text settings (with showArea: true)"),
  ...CommonElementFields,
});

export const TextSpec = z
  .object({
    position: Point2D.describe("Anchor point of the text (m)"),
    ...TextContentFields,
    anchor: z
      .enum(["TopLeft", "TopCenter", "TopRight", "MiddleLeft", "Center", "MiddleRight", "BottomLeft", "BottomCenter", "BottomRight"])
      .optional()
      .describe("Which point of the text block sits at 'position' (default: tool setting)"),
    ...TextLayoutFields,
    frame: z.boolean().optional().describe("Draw a frame around the text"),
    frameOffset: z.number().min(0).optional().describe("Gap between text and frame (mm)"),
    ...DraftingCommonFields,
  })
  .refine(exactlyOneContent, "give exactly one of 'text' or 'runs'");

export const LabelLeader = z
  .object({
    show: z.boolean().optional().describe("Show the leader line"),
    shape: z.enum(["Segmented", "Spline", "SquareRoot"]).optional().describe("Leader shape"),
    pen: Pen.optional().describe("Leader pen"),
    lineType: AttrRef.optional().describe("Leader line type (name/index)"),
    arrows: Arrows.optional().describe("Arrowhead of the leader (the arrow point is 'begin')"),
    anchor: z.enum(["Middle", "Top", "Bottom", "Underlined"]).optional().describe("Where the leader attaches to the text"),
    squareRootAngle: z.number().optional().describe("Angle of the 'SquareRoot' leader shape in degrees"),
  })
  .describe("Leader line settings");

export const LabelSpec = z
  .object({
    parent: ElementRef.optional().describe(
      "ASSOCIATIVE label: GUID of the element to label (wall, slab, object, door, zone, hatch ...). The label follows the element and " +
        "its autotext shows the element's data. Omit for an independent label",
    ),
    labelClass: z
      .enum(["Text", "Symbol"])
      .optional()
      .describe("'Text' = text label (text/runs); 'Symbol' = GDL label library part (libraryPart). Default: Label tool setting"),
    begin: Point2D.optional().describe(
      "Leader start = arrow point on the element (m). Required for independent labels; default for associative labels: a point of the parent",
    ),
    middle: Point2D.optional().describe("Leader break point (m); needs end. Default: halfway between begin and end"),
    end: Point2D.optional().describe(
      "Leader end where the text sits (m). Omit on associative labels to let Archicad use its default label position",
    ),
    leader: LabelLeader.optional(),
    frame: z.boolean().optional().describe("Frame around the label text"),
    frameOffset: z.number().min(0).optional().describe("Gap between the text and its frame"),
    textOrientation: z
      .enum(["Parallel", "Horizontal", "Vertical", "General"])
      .optional()
      .describe(
        "Text direction: 'Horizontal' (default for new text labels), 'Parallel' (to the labelled element), 'Vertical', or 'General' (uses angle)",
      ),
    alwaysReadable: z.boolean().optional().describe("Flip the text so it is never upside down"),
    angle: z.number().optional().describe("Text/symbol rotation in degrees (with textOrientation 'General')"),
    // text labels
    text: TextContentFields.text.describe(
      "Text label content; '\\n' = new line. Omit to keep the Label tool's default content (e.g. autotext of an associative label)",
    ),
    runs: TextContentFields.runs,
    pen: Pen.optional().describe("Text pen (text labels) / symbol pen (symbol labels)"),
    font: StyledContentFields.font,
    size: StyledContentFields.size,
    bold: z.boolean().optional(),
    italic: z.boolean().optional(),
    underline: z.boolean().optional(),
    strikeout: z.boolean().optional(),
    superscript: z.boolean().optional(),
    subscript: z.boolean().optional(),
    justification: TextContentFields.justification,
    lineSpacing: TextContentFields.lineSpacing,
    widthFactor: TextLayoutFields.widthFactor,
    charSpacing: TextLayoutFields.charSpacing,
    wrapWidth: TextLayoutFields.wrapWidth.describe(
      "Wrap the label text inside a box of this width in mm (paper); new text labels do not wrap unless given (lines break only at '\\n')",
    ),
    fixedSize: TextLayoutFields.fixedSize,
    framePen: TextLayoutFields.framePen,
    background: z.boolean().optional().describe("Opaque background behind the label text"),
    backgroundPen: Pen.optional().describe("Background pen (colour)"),
    // symbol labels
    libraryPart: LibPartRef.optional().describe(
      "Symbol labels: a Label library part (LOCALIZED name — find it with search_library_parts {type: 'Label'})",
    ),
    gdlParameters: GdlParams.optional().describe("Symbol labels: GDL parameter values {name: value} of the label library part"),
    wrapText: z.boolean().optional().describe("Symbol labels: wrap the text"),
    ...DraftingCommonFields,
  })
  .refine((v) => v.begin !== undefined || v.parent !== undefined, "an independent label needs 'begin' (or give 'parent')")
  .refine((v) => v.middle === undefined || v.end !== undefined, "'middle' needs 'end'")
  .refine((v) => v.text === undefined || v.runs === undefined, "give either 'text' or 'runs', not both");

export const HotspotSpec = z.object({
  position: Point2D.describe("Hotspot position (m)"),
  height: z.number().optional().describe("Elevation of the hotspot (m, relative to the home story) — snaps in 3D/sections"),
  pen: Pen.optional(),
  ...DraftingCommonFields,
});

export const PictureSpec = z.object({
  file: z
    .string()
    .min(1)
    .describe("ABSOLUTE path of an image file on this Mac (PNG, JPEG, GIF, TIFF or BMP; max 32767 px per side, 200 MB)"),
  position: Point2D.describe("Where the picture's anchor point goes (m)"),
  width: z.number().positive().optional().describe("Width in m; with only width or height the other follows the image aspect ratio"),
  height: z.number().positive().optional().describe("Height in m"),
  anchor: z
    .enum(["TopLeft", "TopCenter", "TopRight", "MiddleLeft", "Center", "MiddleRight", "BottomLeft", "BottomCenter", "BottomRight"])
    .optional()
    .describe("Which point of the picture sits at 'position' (default BottomLeft; other anchors need width or height)"),
  angle: z.number().optional().describe("Rotation in degrees"),
  mirrored: z.boolean().optional().describe("Mirror horizontally"),
  transparent: z.boolean().optional().describe("Transparent background (white/alpha)"),
  name: z.string().max(255).optional().describe("Picture name (default: the file name)"),
  ...DraftingCommonFields,
});

// --- Tools -------------------------------------------------------------------------------

const RESULT = "Returns [{guid, type} | {error}] in input order; one failing item does not stop the others.";
const MODIFY_HINT = "Change them later with modify_elements using the same field names; read them back with get_element_details.";

export function registerDraftingTools(server: McpServer, ctx: ToolContext): void {
  defineTool(server, ctx, {
    name: "create_lines",
    title: "Create lines",
    description:
      "Draws straight 2D lines (one undo step). Each line: begin/end points in meters plus optional pen, colorOverridePen, lineType, " +
      `lineWeight (mm), category, zoneBoundary and arrows. Unspecified settings come from the Line tool defaults. ${WHERE} ` +
      `For connected segments prefer create_polylines. ${RESULT} ${MODIFY_HINT}`,
    input: { lines: z.array(LineSpec).min(1).max(2000).describe("Lines to draw") },
    annotations: CREATES,
    handler: async ({ lines }, c) => createElements(c, lines.map((l) => ({ type: "Line", ...l })), "Create lines (Claude)"),
  });

  defineTool(server, ctx, {
    name: "create_arcs",
    title: "Create arcs",
    description:
      "Draws 2D circular or elliptical arcs. Three ways to define each arc: (A) center + radius + beginAngle + endAngle (degrees, CCW " +
      "from +X; add minorRadius/axisAngle for elliptical arcs); (B) begin + end + arcAngle (signed sweep, + = counter-clockwise); " +
      "(C) begin + through + end (three points). Archicad stores arcs counter-clockwise, so a clockwise arc (negative arcAngle) is " +
      `returned with begin/end swapped. Style fields as create_lines. Full circles: create_circles. ${WHERE} ${RESULT} ${MODIFY_HINT}`,
    input: { arcs: z.array(ArcSpecFields).min(1).max(2000).describe("Arcs to draw") },
    annotations: CREATES,
    handler: async ({ arcs }, c) => createElements(c, arcs.map((a) => ({ type: "Arc", ...a })), "Create arcs (Claude)"),
  });

  defineTool(server, ctx, {
    name: "create_circles",
    title: "Create circles / ellipses",
    description:
      "Draws full 2D circles (center + radius in m) or ellipses (+ minorRadius and axisAngle in degrees). Style fields: pen, " +
      `colorOverridePen, lineType, lineWeight, category, zoneBoundary. ${WHERE} ${RESULT} ${MODIFY_HINT}`,
    input: { circles: z.array(CircleSpec).min(1).max(2000).describe("Circles/ellipses to draw") },
    annotations: CREATES,
    handler: async ({ circles }, c) => createElements(c, circles.map((x) => ({ type: "Circle", ...x })), "Create circles (Claude)"),
  });

  defineTool(server, ctx, {
    name: "create_polylines",
    title: "Create polylines",
    description:
      "Draws 2D polylines: connected straight and curved segments (points + optional arcs {index, angle}), open or closed. Ideal " +
      "for outlines, symbols and room-separator chains (zoneBoundary: true). Style fields as create_lines plus continuousPattern. " +
      `${WHERE} ${RESULT} ${MODIFY_HINT} Modifying 'arcs' or 'closed' requires sending 'points' too (the geometry is replaced).`,
    input: { polylines: z.array(PolyLineSpec).min(1).max(1000).describe("Polylines to draw") },
    annotations: CREATES,
    handler: async ({ polylines }, c) =>
      createElements(c, polylines.map((p) => ({ type: "PolyLine", ...p })), "Create polylines (Claude)"),
  });

  defineTool(server, ctx, {
    name: "create_splines",
    title: "Create splines",
    description:
      "Draws smooth 2D Bezier splines through the given points (automatic natural spline), or with explicit Bezier handles " +
      "(directions: one {angle, lengthPrev, lengthNext} per point). Open or closed. Style fields as create_lines. NOTE: Archicad cannot " +
      "change the geometry of an existing spline — to reshape one, create a new spline and delete the old one (delete_elements); " +
      `modify_elements can still change its pen, lineType, arrows etc. ${WHERE} ${RESULT}`,
    input: { splines: z.array(SplineSpec).min(1).max(500).describe("Splines to draw") },
    annotations: CREATES,
    handler: async ({ splines }, c) => createElements(c, splines.map((s) => ({ type: "Spline", ...s })), "Create splines (Claude)"),
  });

  defineTool(server, ctx, {
    name: "create_hatches",
    title: "Create hatches (fills)",
    description:
      "Creates 2D fill polygons (Archicad 'Fill' tool): a polygon in meters with optional arcs and holes, filled with a Fill pattern " +
      "(fillType) or a building material's cut fill (buildingMaterial), with pens, RGB colour overrides, contour (on/off, pen, line " +
      "type, weight), pattern orientation (rotated / distorted / radial, local origin), fill category and an optional area text. " +
      "Attribute names are LOCALIZED — list them with get_attributes {type: 'Fill' | 'BuildingMaterial' | 'Line'}. get_element_details " +
      `returns the polygon plus area and perimeter. ${WHERE} ${RESULT} ${MODIFY_HINT}`,
    input: { hatches: z.array(HatchSpec).min(1).max(1000).describe("Fills to create") },
    annotations: CREATES,
    handler: async ({ hatches }, c) => createElements(c, hatches.map((h) => ({ type: "Hatch", ...h })), "Create fills (Claude)"),
  });

  defineTool(server, ctx, {
    name: "create_texts",
    title: "Create texts",
    description:
      "Places text blocks (Archicad 'Text' tool): position (m) + text ('\\n' = new line, any language incl. Cyrillic) or rich-text " +
      "runs with per-run pen/font/size/bold/italic/underline/strikeout/superscript/subscript; plus anchor, justification, angle, " +
      "widthFactor, charSpacing, wrapWidth (mm), frame, background, alwaysReadable. Text size is in MILLIMETERS on paper (it scales " +
      "with the drawing scale unless fixedSize). Fonts by name ('Arial') — get_attributes {type: 'Font'}. " +
      `${WHERE} ${RESULT} ${MODIFY_HINT} Restyling via modify_elements (e.g. {guid, size: 5, bold: true}) keeps the content.`,
    input: { texts: z.array(TextSpec).min(1).max(1000).describe("Texts to place") },
    annotations: CREATES,
    handler: async ({ texts }, c) => createElements(c, texts.map((t) => ({ type: "Text", ...t })), "Create texts (Claude)"),
  });

  defineTool(server, ctx, {
    name: "create_labels",
    title: "Create labels",
    description:
      "Places labels with a leader line. ASSOCIATIVE labels (parent = element GUID) stick to the element and can show its data via the " +
      "Label tool's autotext default content; INDEPENDENT labels need 'begin'. Leader: begin (arrow point) -> middle -> end (text). " +
      "Text labels take text/runs and the same text style fields as create_texts; symbol labels (labelClass 'Symbol') use a Label " +
      "library part (libraryPart, gdlParameters — find parts with search_library_parts {type: 'Label'}). For associative labels without " +
      `'end' Archicad uses its default label position. ${WHERE} ${RESULT} ${MODIFY_HINT} The parent and the class of an existing ` +
      "label cannot be changed (delete and recreate).",
    input: { labels: z.array(LabelSpec).min(1).max(1000).describe("Labels to place") },
    annotations: CREATES,
    handler: async ({ labels }, c) =>
      createElements(
        c,
        labels.map(({ parent, ...rest }) => ({ type: "Label", ...rest, ...(parent !== undefined ? { parent: guidOf(parent) } : {}) })),
        "Create labels (Claude)",
      ),
  });

  defineTool(server, ctx, {
    name: "create_hotspots",
    title: "Create hotspots",
    description:
      "Places hotspots (snap points) at 2D positions (m) with an optional elevation (height, m) and pen — useful as reference/snap " +
      `points for later drawing. ${WHERE} ${RESULT} ${MODIFY_HINT}`,
    input: { hotspots: z.array(HotspotSpec).min(1).max(5000).describe("Hotspots to place") },
    annotations: CREATES,
    handler: async ({ hotspots }, c) => createElements(c, hotspots.map((h) => ({ type: "Hotspot", ...h })), "Create hotspots (Claude)"),
  });

  defineTool(server, ctx, {
    name: "create_pictures",
    title: "Place pictures",
    description:
      "Places raster images (PNG, JPEG, GIF, TIFF, BMP) from ABSOLUTE file paths on this Mac, e.g. a logo on a layout or a site photo " +
      "on a worksheet. Size in meters via width and/or height (aspect ratio kept when only one is given); without a size the picture " +
      "is placed at its pixel size with its bottom-left corner at position. Optional anchor, angle, mirrored, transparent, name. " +
      `${WHERE} ${RESULT} Position/size/angle can be changed later with modify_elements; the image itself cannot be replaced.`,
    input: { pictures: z.array(PictureSpec).min(1).max(50).describe("Pictures to place") },
    annotations: CREATES,
    handler: async ({ pictures }, c) => createElements(c, pictures.map((p) => ({ type: "Picture", ...p })), "Place pictures (Claude)"),
  });
}
