/**
 * View tools — windows, navigation, zoom, 3D view, "Claude's eyes" (capture_view / render_view),
 * section / elevation / interior elevation / detail / worksheet markers and view settings.
 *
 * Add-on commands (addon/Src/Commands/Views*.cpp): GetCurrentWindow, ListViews, OpenView, GoToView, Zoom,
 * ShowIn3D, Get3DView, Set3DView, CaptureView, RenderView, GetViewSettings, SetViewSettings; the marker
 * element types (CutPlane, Elevation, InteriorElevation, Detail, Worksheet) go through CreateElements.
 */

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import { CREATES, defineTool, imageResult, MODIFIES, READ_ONLY, type ToolContext } from "./define.js";
import { createElements } from "./elements.js";
import { AttrRef, CommonElementFields, ElementRef, ElementRefs, Guid, guidOf, Point2D, Point3D, Polygon, StoryRef } from "./schemas.js";
import {
  DEFAULT_MAX_SIZE,
  keepCopy,
  loadImage,
  makeTempImagePath,
  removeQuietly,
  waitForFile,
  type ImageFormat,
} from "./views-image.js";

type Json = Record<string, unknown>;

// =============================================================================
// Shared schemas
// =============================================================================

const WindowType = z
  .enum(["FloorPlan", "3D", "3DModel", "Section", "Elevation", "InteriorElevation", "Detail", "Worksheet", "Layout", "MasterLayout", "DocumentFrom3D"])
  .describe("Window type ('3D' and '3DModel' are the same: the 3D window)");

export const ViewRef = z
  .union([
    z.string().min(1),
    z.object({ guid: Guid.describe("Navigator item GUID (from list_views)") }),
    z.object({ name: z.string().min(1).describe("View name or '<ID> <name>' as shown in the Navigator") }),
  ])
  .describe(
    "Navigator item: GUID string (from list_views / the official Navigator tree), or the view name / '<ID> <name>' as shown in the " +
      "Navigator (localized, e.g. Russian; exact match first, then case-insensitive, then 'contains'). Ambiguous names return the candidates.",
  );

export const Box2D = z
  .object({
    xMin: z.number().describe("Left edge (m, project coordinates; paper mm-coordinates in layouts)"),
    yMin: z.number().describe("Bottom edge"),
    xMax: z.number().describe("Right edge"),
    yMax: z.number().describe("Top edge"),
  })
  .describe("Axis-aligned rectangle");

export const OpenViewFields = {
  window: WindowType.optional().describe(
    "Window to open: 'FloorPlan' (+ story), '3D' (+ projection), or a viewpoint type 'Section' | 'Elevation' | 'InteriorElevation' | " +
      "'Detail' | 'Worksheet' | 'Layout' | 'MasterLayout' | 'DocumentFrom3D' (+ name, or the only one of that type)",
  ),
  story: StoryRef.optional().describe("Floor plan story to show (index or localized name, see get_stories). Implies window 'FloorPlan'"),
  name: z
    .string()
    .min(1)
    .optional()
    .describe("Name, reference ID or title of the section/elevation/detail/worksheet/layout to open (see list_views; localized)"),
  element: ElementRef.optional().describe("GUID of a section (CutPlane), elevation, interior elevation, detail or worksheet MARKER element: opens its viewpoint"),
  segmentIndex: z.number().int().min(0).optional().describe("Interior elevation marker only: which segment's view to open (default 0)"),
  database: Guid.optional().describe("Database GUID of the viewpoint/layout (the 'database' field of list_views / get_current_window)"),
  navigatorItem: ViewRef.optional().describe("Project Map / Layout Book / View Map item to open (window only; use go_to_view to also apply a saved view's settings)"),
  projection: z.enum(["perspective", "axonometric"]).optional().describe("3D window only: switch the projection mode before opening"),
};

const ProjectionPreset = z
  .enum([
    "Isometric",
    "Dimetric",
    "Monometric",
    "Frontal",
    "TopView",
    "FrontView",
    "SideView",
    "BottomView",
    "FrontalBottom",
    "MonometricBottom",
    "IsometricBottom",
    "DimetricBottom",
    "Parallel",
    "CustomAxonometry",
  ])
  .describe(
    "Axonometric (parallel) projection preset. TopView = plan from above, FrontView/SideView = elevations, Isometric/Dimetric/Monometric/" +
      "Frontal = classic axonometries (rotated by azimuth), *Bottom = seen from below, Parallel = free parallel view (use with azimuth + altitude)",
  );

export const Set3DFields = {
  mode: z
    .enum(["perspective", "axonometric"])
    .optional()
    .describe("Projection mode. Default: perspective when camera/target/viewCone/roll/distance are given, axonometric when projection/tranmat are given, otherwise the current mode"),
  camera: Point3D.optional().describe("Perspective: camera (eye) position in m, z = absolute elevation (e.g. 1.6 above a story level for eye height)"),
  target: Point3D.optional().describe("Perspective: point the camera looks at (m). With azimuth/altitude it is the orbit center (default: center of the model)"),
  azimuth: z
    .number()
    .optional()
    .describe(
      "Degrees CCW from +X (east; 90 = north). Perspective orbit: direction FROM the target TO the camera, e.g. 225 = camera south-west of " +
        "the model looking north-east. Axonometric: the camera azimuth of the parallel projection",
    ),
  altitude: z
    .number()
    .min(-90)
    .max(90)
    .optional()
    .describe("Degrees above the horizon of the view direction's source: perspective orbit camera elevation angle (default 30); axonometric: makes a free 'Parallel' view from that elevation (experimental)"),
  distance: z.number().positive().optional().describe("Perspective orbit: camera–target distance in m (default: the whole model fits in view)"),
  viewCone: z.number().min(1).max(179).optional().describe("Perspective field of view in degrees (default: current or 60; 35-50 = natural, 70-90 = wide interior shots)"),
  roll: z.number().optional().describe("Perspective camera roll in degrees (0 = level horizon)"),
  twoPointPerspective: z.boolean().optional().describe("Perspective: keep vertical lines vertical (2-point perspective)"),
  projection: ProjectionPreset.optional(),
  tranmat: z
    .array(z.number())
    .length(12)
    .optional()
    .describe("Axonometric: raw 3x4 view matrix (row-major, as returned by get_3d_view) — copy it from a view you liked"),
  sun: z
    .object({
      azimuth: z.number().optional().describe("Sun azimuth in degrees (CCW from +X)"),
      altitude: z.number().min(-90).max(90).optional().describe("Sun altitude in degrees"),
    })
    .optional()
    .describe("Sun position for shading/shadows (sets 'given by angles')"),
  style: z.string().min(1).optional().describe("3D style name to make current (localized; see get_3d_view style.available)"),
  styleSettings: z
    .object({
      model: z
        .enum(["Block", "Wireframe", "HiddenLine", "Shading"])
        .optional()
        .describe("3D rendering mode: Shading (colored surfaces), HiddenLine (black lines, good for reading geometry), Wireframe, Block"),
      transparency: z.boolean().optional().describe("Show transparent surfaces (glass) as transparent in Shading mode"),
      monochrome: z.boolean().optional().describe("Monochrome (white model) display"),
      contours: z.enum(["Draft", "Off", "Best"]).optional().describe("Shading contours"),
      sunShadows: z
        .enum(["Off", "AllSurfacesContoursOff", "AllSurfacesContoursOn", "OneLevelContoursOff", "OneLevelContoursOn"])
        .optional()
        .describe("Vectorial sun shadows"),
      vectorHatching: z.boolean().optional().describe("Vectorial 3D hatching (surface patterns)"),
      castShadowPercent: z.number().int().min(0).max(100).optional().describe("Cast shadow intensity %"),
      shadingPercent: z.number().int().min(0).max(100).optional().describe("Shading intensity %"),
      backgroundAsInRendering: z.boolean().optional().describe("Use the rendering background"),
      skyColor: z.object({ r: z.number().min(0).max(1), g: z.number().min(0).max(1), b: z.number().min(0).max(1) }).optional().describe("Sky (background) color, components 0..1"),
      groundColor: z.object({ r: z.number().min(0).max(1), g: z.number().min(0).max(1), b: z.number().min(0).max(1) }).optional().describe("Ground color, components 0..1"),
    })
    .optional()
    .describe(
      "Edits the CURRENT 3D style definition (applied after 'style'; affects every view using that style), e.g. {model:'HiddenLine'} for a " +
        "line drawing or {model:'Shading', sunShadows:'AllSurfacesContoursOn'}. Current values: get_3d_view style",
    ),
  windowSize: z
    .object({
      width: z.number().int().min(16).max(8000).optional().describe("px"),
      height: z.number().int().min(16).max(8000).optional().describe("px"),
    })
    .optional()
    .describe("Size of the 3D window image in pixels (persists; capture_view width/height changes it only temporarily)"),
  stories: z
    .object({
      all: z.boolean().optional().describe("true = show all stories"),
      from: StoryRef.optional().describe("Lowest story to show"),
      to: StoryRef.optional().describe("Highest story to show"),
      trim: z.boolean().optional().describe("Trim elements to the story range"),
    })
    .optional()
    .describe("3D story range filter (Filter and Cut Elements in 3D)"),
  elementTypes: z
    .array(z.string().min(1))
    .min(1)
    .optional()
    .describe("Show only these element types in 3D, e.g. ['Wall','Slab','Roof','Column'] (type names as in get_supported_element_types), or ['all']"),
  cutPlanes: z
    .object({
      enabled: z.boolean().optional().describe("Turn the 3D cutting planes on/off"),
      shapes: z
        .array(
          z.object({
            a: z.number(),
            b: z.number(),
            c: z.number(),
            d: z.number(),
            status: z.number().int().optional().describe("Archicad cut status code (default 2)"),
            pen: z.number().int().min(1).max(255).optional(),
            material: AttrRef.optional().describe("Surface of the cut faces"),
          }),
        )
        .max(64)
        .optional()
        .describe("Raw cutting plane shapes (advanced; copy them from get_3d_view cutPlanes.shapes)"),
    })
    .optional()
    .describe("3D cutting planes"),
};

const Set3DSchema = z.object(Set3DFields);

const ZoomMode = z.enum(["fit", "box", "elements", "selection", "in", "out", "previous", "redraw"]);

export const ZoomFields = {
  mode: ZoomMode.optional().describe(
    "fit = fit the whole drawing/model in the window (default; 'zoom extents'), box = show the rectangle 'box' (2D windows), elements = zoom to " +
      "'elements', selection = zoom to the selected elements, in/out = zoom by 'factor', previous = undo zoom 'steps', redraw = just redraw",
  ),
  box: Box2D.optional().describe("mode 'box': area to show (m)"),
  elements: ElementRefs.max(5000).optional().describe("mode 'elements': elements to zoom to"),
  factor: z.number().gt(1).max(100).optional().describe("mode 'in'/'out': zoom factor (default 2)"),
  center: Point2D.optional().describe("mode 'in'/'out' in 2D: zoom around this point (default: window center)"),
  margin: z.number().min(0).max(5).optional().describe("2D: extra space around fit/box/elements/selection as a fraction of the area (e.g. 0.1 = 10% on each side)"),
  steps: z.number().int().min(1).max(50).optional().describe("mode 'previous': how many zoom steps to go back (default 1)"),
};

const ZoomSchema = z.object(ZoomFields);

function zoomParams(z0: z.infer<typeof ZoomSchema>): Json {
  const params: Json = {};
  const mode = z0.mode ?? (z0.box ? "box" : z0.elements ? "elements" : "fit");
  params["mode"] = mode;
  if (mode === "box" && !z0.box) throw new Error("Invalid input: zoom mode 'box' needs 'box' {xMin, yMin, xMax, yMax}");
  if (mode === "elements" && !z0.elements) throw new Error("Invalid input: zoom mode 'elements' needs 'elements' (GUIDs)");
  if (z0.box) params["box"] = z0.box;
  if (z0.elements) params["elements"] = z0.elements.map(guidOf);
  if (z0.factor !== undefined) params["factor"] = z0.factor;
  if (z0.center) params["center"] = z0.center;
  if (z0.margin !== undefined) params["margin"] = z0.margin;
  if (z0.steps !== undefined) params["steps"] = z0.steps;
  return params;
}

/** Drops undefined values and converts element refs to GUID strings. */
function openViewParams(v: Json): Json {
  const out: Json = {};
  for (const [k, val] of Object.entries(v)) {
    if (val === undefined) continue;
    out[k] = k === "element" ? guidOf(val as z.infer<typeof ElementRef>) : val;
  }
  if (Object.keys(out).length === 0) {
    throw new Error("Invalid input: tell open_view what to open (window, story, name, element, database or navigatorItem)");
  }
  return out;
}

function compact(v: Json): Json {
  const out: Json = {};
  for (const [k, val] of Object.entries(v)) if (val !== undefined) out[k] = val;
  return out;
}

/**
 * Short summary of the preparation steps of capture_view / render_view for the image metadata: the step
 * results without their (repeated) window description, keeping warnings such as an unverified camera.
 */
function stepSummary(steps: Json): Json | undefined {
  const names = Object.keys(steps);
  if (names.length === 0) return undefined;
  const out: Json = {};
  for (const name of names) {
    const r = steps[name];
    if (r && typeof r === "object" && !Array.isArray(r)) {
      const rest: Json = { ...(r as Json) };
      delete rest["window"];
      out[name] = rest;
    } else {
      out[name] = r ?? { ok: true };
    }
  }
  return out;
}

// =============================================================================
// Marker specs
// =============================================================================

const VerticalRange = z
  .union([
    z.literal("Infinite"),
    z.object({
      min: z.number().describe("Bottom of the vertical range: absolute elevation in m"),
      max: z.number().describe("Top of the vertical range: absolute elevation in m"),
    }),
  ])
  .describe("Vertical range of the view: 'Infinite' or {min, max} absolute elevations (m)");

const MarkerNameFields = {
  name: z.string().max(255).optional().describe("Viewpoint name shown in the Navigator and in the marker (e.g. 'Section A-A')"),
  referenceId: z.string().max(255).optional().describe("Reference ID shown in the marker (e.g. 'A'); Archicad may auto-number when omitted"),
};

export const CutLineFields = {
  begin: Point2D.describe("Start of the cut line (m)"),
  end: Point2D.describe("End of the cut line (m)"),
  viewSide: z
    .enum(["left", "right"])
    .optional()
    .describe("Which side of the line begin→end the view looks at (default 'left'; e.g. begin (0,0) → end (10,0) with 'left' looks north, +Y)"),
  depth: z
    .number()
    .positive()
    .optional()
    .describe("Horizontal depth: distance of the depth limit line from the cut line in m (default 10 m when limited). Giving it makes the range 'Limited'"),
  horizontalRange: z
    .enum(["Infinite", "Limited", "ZeroDepth"])
    .optional()
    .describe("Horizontal range: Infinite = everything behind the line, Limited = up to 'depth', ZeroDepth = only the cut"),
  verticalRange: VerticalRange.optional(),
  ...MarkerNameFields,
  ...CommonElementFields,
};

export const CutLineSpec = z.object(CutLineFields);

export const InteriorElevationSpec = z.object({
  points: z
    .array(Point2D)
    .min(2)
    .max(200)
    .describe("Polyline of the elevation lines (m), usually along the inside of the walls of a room; each segment becomes one elevation view"),
  closed: z.boolean().optional().describe("Close the polyline (last point back to the first), e.g. all 4 walls of a room"),
  depth: z.number().positive().optional().describe("Depth of each segment's view in m (how far behind the line elements are shown; default tool setting or 1 m)"),
  offset: z.number().optional().describe("Horizontal offset of the segments (m, Archicad's creation offset; default 0)"),
  horizontalRange: z.enum(["Infinite", "Limited", "ZeroDepth"]).optional(),
  verticalRange: VerticalRange.optional(),
  ...MarkerNameFields,
  ...CommonElementFields,
});

export const DetailSpec = z.object({
  polygon: Polygon.optional().describe("Boundary of the detail/worksheet region (m). Give polygon or box"),
  box: Box2D.optional().describe("Rectangular boundary instead of polygon"),
  markerPosition: Point2D.optional().describe("Where the marker is placed (default: 0.5 m beyond the top-right corner of the boundary)"),
  markerAngle: z.number().optional().describe("Marker rotation in degrees"),
  horizontalMarker: z.boolean().optional().describe("Keep the marker text horizontal"),
  ...MarkerNameFields,
  ...CommonElementFields,
});

type DetailSpecValue = z.infer<typeof DetailSpec>;

function checkDetailSpecs(items: DetailSpecValue[], key: string): void {
  const problems: string[] = [];
  items.forEach((d, i) => {
    if (!d.polygon && !d.box) problems.push(`${key}[${i}]: give 'polygon' or 'box'`);
    if (d.polygon && d.box) problems.push(`${key}[${i}]: give either 'polygon' or 'box', not both`);
  });
  if (problems.length > 0) throw new Error(`Invalid input: ${problems.join("; ")}`);
}

const DETAIL_KEYS = [
  "name",
  "referenceId",
  "database",
  "hasViewpoint",
  "sourceMarker",
  "begin",
  "end",
  "viewSide",
  "depth",
  "horizontalRange",
  "verticalRange",
  "segments",
  "markerPosition",
  "polygon",
];

/**
 * Creates marker elements through CreateElements, then reads their details (viewpoint database, name, ID,
 * line) so the caller can open the new view right away.
 */
async function createMarkers(ctx: ToolContext, type: string, specs: Json[], undoName: string): Promise<unknown> {
  const created = (await createElements(ctx, specs.map((s) => ({ type, ...s })), undoName)) as { results?: Json[] };
  const results = Array.isArray(created?.results) ? created.results : [];
  const guids = results.map((r) => r["guid"]).filter((g): g is string => typeof g === "string");
  if (guids.length === 0) return created;
  let details: Json[] = [];
  try {
    const d = (await ctx.ac.addon("GetElementDetails", { elements: guids })) as { elements?: Json[] };
    details = Array.isArray(d?.elements) ? d.elements : [];
  } catch {
    return { ...created, note: "Created; details could not be read (use get_element_details)." };
  }
  const byGuid = new Map<string, Json>();
  for (const e of details) if (typeof e["guid"] === "string") byGuid.set(e["guid"] as string, (e["details"] as Json) ?? {});
  const merged = results.map((r) => {
    const g = r["guid"];
    if (typeof g !== "string") return r;
    const det = byGuid.get(g);
    if (!det) return r;
    const extra: Json = {};
    for (const k of DETAIL_KEYS) if (det[k] !== undefined) extra[k] = det[k];
    return { ...r, ...extra };
  });
  return {
    results: merged,
    hint: "Open a new view with open_view {element: guid} (or {database}), then look at it with capture_view.",
  };
}

// =============================================================================
// Image capture helpers
// =============================================================================

const ImageOutputFields = {
  format: z.enum(["png", "jpeg"]).optional().describe("Image format (default png; jpeg is smaller for shaded 3D views and renders)"),
  maxSize: z
    .number()
    .int()
    .min(64)
    .max(4096)
    .optional()
    .describe(`Longest side of the returned image in px (default ${DEFAULT_MAX_SIZE}); larger pictures are downscaled`),
  saveTo: z.string().min(1).optional().describe("Also keep a full-resolution copy at this absolute file path"),
};

async function returnImage(
  file: string,
  format: ImageFormat,
  opts: { maxSize?: number; maxWidth?: number; maxHeight?: number; saveTo?: string },
  meta: Json,
) {
  const img = await loadImage(file, { format, maxSize: opts.maxSize, maxWidth: opts.maxWidth, maxHeight: opts.maxHeight });
  const info: Json = { ...meta, image: compact({ width: img.width, height: img.height, format: img.format, bytes: img.bytes, resized: img.resized, originalWidth: img.originalWidth, originalHeight: img.originalHeight, note: img.note }) };
  if (opts.saveTo) info["savedTo"] = await keepCopy(file, opts.saveTo);
  return imageResult(img.data, img.mimeType, info);
}

/**
 * Archicad builds the 3D model asynchronously (in its idle loop), so a 3D picture saved right after the
 * view/content changed can be empty or partial (verified live). Polls the 3D model size until it is
 * stable for two consecutive samples. No-op for 2D windows or older add-on builds.
 */
async function waitFor3DModel(ac: ToolContext["ac"], force3D: boolean, timeoutMs = 15_000): Promise<{ waitedMs: number; bodies?: number } | undefined> {
  if (!force3D) {
    const win = (await ac.addon<Json>("GetCurrentWindow").catch(() => ({}))) as Json;
    if (!/3D/i.test(String(win["type"] ?? ""))) return undefined;
  }
  const start = Date.now();
  let last = "";
  let stableCount = 0;
  let bodies: number | undefined;
  await new Promise((r) => setTimeout(r, 300));
  while (Date.now() - start < timeoutMs) {
    let stats: Json;
    try {
      stats = await ac.addon<Json>("Get3DModelStats");
    } catch {
      return undefined; // older add-on without the command
    }
    if (typeof stats["bodies"] !== "number") return undefined; // unknown response: do not wait
    const key = `${stats["bodies"]}/${stats["polygons"]}/${stats["vertices"]}`;
    bodies = stats["bodies"] as number;
    // stable twice in a row (an empty model must stay empty a little longer before we trust it)
    if (key === last) {
      if (++stableCount >= (bodies > 0 ? 2 : 4)) break;
    } else {
      stableCount = 0;
      last = key;
    }
    await new Promise((r) => setTimeout(r, 400));
  }
  return { waitedMs: Date.now() - start, bodies };
}

// =============================================================================
// Registration
// =============================================================================

export function registerViewTools(server: McpServer, ctx: ToolContext): void {
  defineTool(server, ctx, {
    name: "get_current_window",
    title: "Get current window",
    description:
      "Returns the active Archicad window: {type: FloorPlan | 3DModel | Section | Elevation | InteriorElevation | Detail | Worksheet | Layout | " +
      "MasterLayout | DocumentFrom3D | ..., database (GUID), name, reference, title, linkedElement, story {index, name, level} (floor plan), " +
      "drawingScale (N of 1:N), zoom {xMin,yMin,xMax,yMax} (2D, m), projection (3D)}. Call it before capture_view/zoom to know what is shown.",
    input: {},
    annotations: READ_ONLY,
    handler: async (_a, { ac }) => ac.addon("GetCurrentWindow"),
  });

  defineTool(server, ctx, {
    name: "list_views",
    title: "List views and viewpoints",
    description:
      "Lists everything that can be opened: stories (floor plans), viewpoints (sections, elevations, interior elevations, details, worksheets, " +
      "3D documents: {type, database, name, reference, title}), layouts, and the View Map (saved views: {guid, name, id, fullName, itemType, " +
      "folder, windowType, database}). Use the results with open_view (database/name) and go_to_view (guid). Names are localized (Russian).",
    input: {
      include: z
        .array(z.enum(["stories", "viewpoints", "layouts", "viewMap", "projectMap", "all"]))
        .min(1)
        .optional()
        .describe("Parts to return (default stories, viewpoints, layouts, viewMap). projectMap = the full Project Map tree items"),
      types: z
        .array(z.enum(["Section", "Elevation", "InteriorElevation", "Detail", "Worksheet", "DocumentFrom3D", "Layout", "MasterLayout", "FloorPlan", "3D", "3DModel"]))
        .min(1)
        .optional()
        .describe("Only these viewpoint / view types"),
      nameContains: z.string().min(1).optional().describe("Case-insensitive filter on names, IDs and folders"),
      limit: z.number().int().min(1).max(5000).optional().describe("Maximum items per list (default 1000)"),
    },
    annotations: READ_ONLY,
    handler: async (args, { ac }) => ac.addon("ListViews", compact(args)),
  });

  defineTool(server, ctx, {
    name: "open_view",
    title: "Open view / window",
    description:
      "Brings a window to the front: a floor plan story ({window:'FloorPlan', story}), the 3D window ({window:'3D', projection?}), a section/" +
      "elevation/interior elevation/detail/worksheet/3D document/layout by name ({window:'Section', name:'A-A'}), by marker element ({element}), " +
      "by database GUID ({database}) or by Navigator item ({navigatorItem}). Returns {opened, window}. Saved views with their layer/scale " +
      "settings: go_to_view. To look at the result use capture_view.",
    input: OpenViewFields,
    annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: true },
    handler: async (args, { ac }) => ac.addon("OpenView", openViewParams(args as Json)),
  });

  defineTool(server, ctx, {
    name: "go_to_view",
    title: "Go to saved view",
    description:
      "Opens a saved view of the View Map exactly like double-clicking it in the Navigator: switches window/story and applies the view's " +
      "layer combination, scale, model view options, pen set, graphic overrides and zoom. Accepts the Navigator item GUID or its name " +
      "(see list_views include ['viewMap']). Project Map / Layout Book items are opened without view settings. Returns {view, method, window}.",
    input: { view: ViewRef },
    annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: true },
    handler: async ({ view }, { ac }) => ac.addon("GoToView", { view }),
  });

  defineTool(server, ctx, {
    name: "zoom",
    title: "Zoom",
    description:
      "Zooms the active window: fit (whole drawing/model), box (2D rectangle in m), elements (GUIDs), selection, in/out by a factor, previous " +
      "(undo zoom) or redraw. margin adds space around 2D zooms. Returns {mode, window: {..., zoom}}. Tip: zoom {mode:'elements', elements, " +
      "margin: 0.2} and then capture_view to inspect specific elements.",
    input: ZoomFields,
    annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: true },
    handler: async (args, { ac }) => ac.addon("Zoom", zoomParams(args)),
  });

  defineTool(server, ctx, {
    name: "get_3d_view",
    title: "Get 3D view settings",
    description:
      "Returns the 3D view state: projection {mode, perspective: {camera {x,y,z}, target {x,y,z}, viewCone, roll, distance, azimuth, " +
      "viewDirection, sun} | axonometric: {projection preset, azimuth, tranmat, derived view direction, sun}}, 3D style {current, available, " +
      "model (Shading/HiddenLine/...), ...}, windowSize, filter {allStories, fromStory, toStory, mode (all/selection/marquee), elementTypes}, " +
      "cutPlanes, rendering {scenes, imageSize} and modelExtent (3D bounding box, m). Angles in degrees.",
    input: {},
    annotations: READ_ONLY,
    handler: async (_a, { ac }) => ac.addon("Get3DView"),
  });

  defineTool(server, ctx, {
    name: "set_3d_view",
    title: "Set 3D view",
    description:
      "Sets up the 3D window. Easiest overview: {mode:'perspective', azimuth: 225, altitude: 30} (camera south-west of the model, fits the " +
      "whole model). Exact camera: {mode:'perspective', camera:{x,y,z}, target:{x,y,z}, viewCone}. Axonometry: {mode:'axonometric', " +
      "projection:'Isometric', azimuth} or {projection:'TopView'}. Also: sun, style (3D style name), styleSettings (shading/hidden line, " +
      "shadows...), windowSize, stories range, elementTypes " +
      "filter, cutPlanes. Opens the 3D window unless open3D is false. Returns {changed, camera {verified, warning?} (perspective) | axonometric {viewMatrixChanged, warning?}, projection, window}. " +
      "Follow with capture_view to see the result.",
    input: {
      ...Set3DFields,
      open3D: z.boolean().optional().describe("Bring the 3D window to the front afterwards (default true)"),
    },
    annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: true },
    handler: async (args, { ac }) => ac.addon("Set3DView", compact(args as Json)),
  });

  defineTool(server, ctx, {
    name: "show_in_3d",
    title: "Show in 3D",
    description:
      "Shows elements in the 3D window: mode 'all' (Show All in 3D, resets a previous selection filter), 'selection' (the selected elements " +
      "only) or give 'elements' (GUIDs: they get selected and only they are shown). Returns {mode, window}. Then use set_3d_view / capture_view.",
    input: {
      mode: z.enum(["all", "selection", "elements"]).optional().describe("Default: 'elements' when elements are given, else 'all'"),
      elements: ElementRefs.max(10000).optional().describe("Elements to show (replaces the current selection)"),
    },
    annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: true },
    handler: async ({ mode, elements }, { ac }) => {
      if (mode === "elements" && !elements) throw new Error("Invalid input: mode 'elements' needs 'elements' (GUIDs)");
      const params: Json = {};
      if (mode) params["mode"] = mode;
      if (elements) params["elements"] = elements.map(guidOf);
      return ac.addon("ShowIn3D", params);
    },
  });

  defineTool(server, ctx, {
    name: "capture_view",
    title: "Capture view (screenshot)",
    description:
      "Claude's eyes: saves the ACTIVE Archicad window (floor plan, section, elevation, detail, worksheet, layout or 3D) as a picture and " +
      "returns it as an image. Optional preparation in the same call, in this order: goToView (saved view), view (same fields as open_view), " +
      "threeD (same fields as set_3d_view; switches to 3D), zoom (same fields as zoom). 2D windows are captured as currently zoomed (use zoom " +
      "{mode:'fit'} or {mode:'elements'} first); 3D captures use the 3D window size — width/height set it temporarily. Images are downscaled " +
      `to maxSize (default ${DEFAULT_MAX_SIZE}px). Examples: {view:{window:'FloorPlan', story:0}, zoom:{mode:'fit'}}; ` +
      "{threeD:{mode:'perspective', azimuth:225, altitude:25}}.",
    input: {
      goToView: ViewRef.optional().describe("First open this saved view (go_to_view)"),
      view: z.object(OpenViewFields).optional().describe("First open this window (open_view fields)"),
      threeD: Set3DSchema.optional().describe("First set up the 3D view (set_3d_view fields) and switch to the 3D window"),
      zoom: ZoomSchema.optional().describe("Zoom before capturing (zoom tool fields)"),
      width: z.number().int().min(16).max(8000).optional().describe("3D: capture width in px (temporarily resizes the 3D window); 2D: max width of the returned image"),
      height: z.number().int().min(16).max(8000).optional().describe("3D: capture height in px; 2D: max height of the returned image"),
      keepSelectionHighlight: z.boolean().optional().describe("Keep the selection highlight in the picture (default false)"),
      cropToWindow: z.boolean().optional().describe("true (default) = only the visible window area; false = the whole drawing"),
      ...ImageOutputFields,
    },
    annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: true },
    handler: async (args, c) => {
      const { ac } = c;
      const format: ImageFormat = args.format ?? "png";
      const steps: Json = {};
      if (args.goToView) steps["goToView"] = await ac.addon("GoToView", { view: args.goToView });
      if (args.view) steps["openView"] = await ac.addon("OpenView", openViewParams(args.view as Json));
      if (args.threeD) steps["set3DView"] = await ac.addon("Set3DView", { ...compact(args.threeD as Json), open3D: true });
      if (args.zoom) steps["zoom"] = await ac.addon("Zoom", zoomParams(args.zoom));
      // An axonometric view has no camera position: without a fit it may frame an empty part of the
      // model (verified live), so fit the 3D window to the shown model unless the caller zooms itself.
      else if (args.threeD && (args.threeD as Json)["mode"] !== "perspective" && (args.threeD as Json)["camera"] === undefined) {
        steps["zoom"] = await ac.addon("Zoom", { mode: "fit" }).catch((e: unknown) => ({ warning: String(e) }));
      }
      const model3D = await waitFor3DModel(ac, Boolean(args.threeD));
      if (model3D) steps["waitFor3DModel"] = model3D;

      const file = await makeTempImagePath("capture", format);
      try {
        const params: Json = { path: file, format };
        if (args.width !== undefined) params["width"] = args.width;
        if (args.height !== undefined) params["height"] = args.height;
        if (args.keepSelectionHighlight !== undefined) params["keepSelectionHighlight"] = args.keepSelectionHighlight;
        if (args.cropToWindow !== undefined) params["cropToWindow"] = args.cropToWindow;
        const res = (await ac.addon("CaptureView", params, { timeoutMs: 180_000 })) as Json;
        if (!(await waitForFile(file, 5_000))) throw new Error("Archicad did not write the picture file. Is a model window active? Try open_view first.");
        const window = res["window"] as Json | undefined;
        const is3D = window?.["type"] === "3DModel";
        return await returnImage(
          file,
          format,
          {
            maxSize: args.maxSize,
            maxWidth: is3D ? undefined : args.width,
            maxHeight: is3D ? undefined : args.height,
            saveTo: args.saveTo,
          },
          compact({ window, preparation: stepSummary(steps) }),
        );
      } finally {
        await removeQuietly(file);
      }
    },
  });

  defineTool(server, ctx, {
    name: "render_view",
    title: "Photo render",
    description:
      "Photo-renders the current 3D view (camera / projection of the 3D window) with Archicad's rendering engine and returns the image. " +
      "Optionally set up the camera first (threeD, same fields as set_3d_view), pick a rendering scene (names in get_3d_view rendering.scenes) " +
      "and the output size (width/height px, restored afterwards). Rendering can take minutes (timeoutSeconds, default 600). For quick looks " +
      "use capture_view of the 3D window instead.",
    input: {
      threeD: Set3DSchema.optional().describe("First set up the 3D view (set_3d_view fields)"),
      scene: z.string().min(1).optional().describe("Rendering scene name (localized; see get_3d_view rendering.scenes)"),
      width: z.number().int().min(16).max(16000).optional().describe("Render width in px (default: scene setting)"),
      height: z.number().int().min(16).max(16000).optional().describe("Render height in px (default: scene setting; keeps proportions when only width is given)"),
      timeoutSeconds: z.number().int().min(10).max(3600).optional().describe("Maximum rendering time to wait (default 600)"),
      ...ImageOutputFields,
    },
    annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: true },
    handler: async (args, { ac }) => {
      const format: ImageFormat = args.format ?? "jpeg";
      const steps: Json = {};
      if (args.threeD) steps["set3DView"] = await ac.addon("Set3DView", { ...compact(args.threeD as Json), open3D: true });
      const model3D = await waitFor3DModel(ac, true);
      if (model3D) steps["waitFor3DModel"] = model3D;
      const file = await makeTempImagePath("render", format);
      try {
        const params: Json = { path: file, format };
        if (args.scene) params["scene"] = args.scene;
        if (args.width !== undefined) params["width"] = args.width;
        if (args.height !== undefined) params["height"] = args.height;
        const timeoutMs = (args.timeoutSeconds ?? 600) * 1000;
        let res: Json;
        try {
          res = (await ac.addon("RenderView", params, { timeoutMs })) as Json;
        } catch (err) {
          // When the CineRender engine (Cineware helper process) crashes, Archicad drops the request and
          // shows a warning dialog that blocks every further call (verified live on macOS 26).
          await new Promise((r) => setTimeout(r, 2000));
          const dialog = await ac.modalDialog();
          if (dialog || (err instanceof Error && /fetch failed|Cannot reach/i.test(err.message))) {
            throw new Error(
              "Rendering failed: Archicad's rendering engine (CineRender/Cineware) seems to have crashed" +
                (dialog ? ` and Archicad now shows a dialog ("${dialog}") — ask the user to close it before any other call` : "") +
                ". Use capture_view of the 3D window (axonometric or perspective) for a picture instead.",
            );
          }
          throw err;
        }
        if (!(await waitForFile(file, 30_000))) {
          throw new Error("Archicad finished the render command but no image file appeared. Check the rendering settings (engine, scene) in Archicad.");
        }
        const meta: Json = compact({
          scene: res["scene"],
          renderSize: res["imageSize"],
          projection: res["projection"],
          preparation: stepSummary(steps),
        });
        const result = await returnImage(file, format, { maxSize: args.maxSize, saveTo: args.saveTo }, meta);
        // A blank (uniform) picture compresses to almost nothing: typical when the CineRender engine
        // (Cineware helper process) fails on the machine. Say so instead of returning a white image silently.
        const img = (result.meta as Json | undefined)?.["image"] as { width?: number; height?: number; bytes?: number } | undefined;
        if (img?.width && img.height && img.bytes !== undefined && img.bytes / (img.width * img.height) < 0.05) {
          (result.meta as Json)["warning"] =
            "The rendered image looks blank (uniform). Archicad's CineRender engine probably failed (check ~/Library/Logs/DiagnosticReports for " +
            "Cineware crash reports). Use capture_view of the 3D window instead, or try another scene.";
        }
        return result;
      } finally {
        await removeQuietly(file);
      }
    },
  });

  defineTool(server, ctx, {
    name: "create_sections",
    title: "Create sections",
    description:
      "Creates section markers (cut planes) on the floor plan, each with its own new section viewpoint (one undo step). Each item: begin/end " +
      "of the cut line (m), viewSide (left/right of begin→end, default left), depth (limits how far the section looks), vertical range, " +
      "name ('Разрез 1-1'), referenceId, storyIndex/layer. Returns [{guid, type, name, referenceId, database, begin, end, viewSide, depth} | " +
      "{error}]. Open one with open_view {element: guid} and look at it with capture_view.",
    input: { sections: z.array(CutLineSpec).min(1).max(100), undoName: z.string().optional() },
    annotations: CREATES,
    handler: async ({ sections, undoName }, c) => createMarkers(c, "CutPlane", sections as Json[], undoName ?? "Create sections (Claude)"),
  });

  defineTool(server, ctx, {
    name: "create_elevations",
    title: "Create elevations",
    description:
      "Creates elevation markers on the floor plan (one undo step), each with a new elevation viewpoint. begin/end = the elevation line (m) " +
      "placed OUTSIDE the building, viewSide = the side the elevation looks at (towards the building), depth = how far it sees. E.g. south " +
      "facade of a building spanning y 0..10: begin (-5,-5), end (20,-5), viewSide 'left' (looks north). Returns [{guid, name, database, ...}].",
    input: { elevations: z.array(CutLineSpec).min(1).max(100), undoName: z.string().optional() },
    annotations: CREATES,
    handler: async ({ elevations, undoName }, c) => createMarkers(c, "Elevation", elevations as Json[], undoName ?? "Create elevations (Claude)"),
  });

  defineTool(server, ctx, {
    name: "create_interior_elevations",
    title: "Create interior elevations",
    description:
      "Creates interior elevation markers (one undo step): 'points' is a polyline inside a room along its walls; every segment becomes one " +
      "interior elevation view (closed: true for all walls). depth = view depth per segment (m). If a view looks the wrong way, recreate it with " +
      "the points in reverse order. Returns [{guid, name, segments: [{index, name, database}]}] — open a segment with open_view {element, segmentIndex}.",
    input: { interiorElevations: z.array(InteriorElevationSpec).min(1).max(100), undoName: z.string().optional() },
    annotations: CREATES,
    handler: async ({ interiorElevations, undoName }, c) =>
      createMarkers(c, "InteriorElevation", interiorElevations as Json[], undoName ?? "Create interior elevations (Claude)"),
  });

  defineTool(server, ctx, {
    name: "create_details",
    title: "Create details",
    description:
      "Creates detail markers (one undo step) in the current floor plan/section/elevation (or the floor plan when another window is active): " +
      "the boundary polygon or box (m) defines the region copied into a new detail viewpoint; the marker is placed at markerPosition. " +
      "Returns [{guid, name, referenceId, database, markerPosition, polygon}]. Open the detail with open_view {element: guid}.",
    input: { details: z.array(DetailSpec).min(1).max(100), undoName: z.string().optional() },
    annotations: CREATES,
    handler: async ({ details, undoName }, c) => {
      checkDetailSpecs(details, "details");
      return createMarkers(c, "Detail", details as Json[], undoName ?? "Create details (Claude)");
    },
  });

  defineTool(server, ctx, {
    name: "create_worksheets",
    title: "Create worksheets",
    description:
      "Creates worksheet markers (one undo step) like create_details: boundary polygon or box (m) + marker position; each gets a new worksheet " +
      "viewpoint (a 2D drafting sheet seeded with the region's drawing). Returns [{guid, name, database, ...}]. Open with open_view {element: guid}.",
    input: { worksheets: z.array(DetailSpec).min(1).max(100), undoName: z.string().optional() },
    annotations: CREATES,
    handler: async ({ worksheets, undoName }, c) => {
      checkDetailSpecs(worksheets, "worksheets");
      return createMarkers(c, "Worksheet", worksheets as Json[], undoName ?? "Create worksheets (Claude)");
    },
  });

  defineTool(server, ctx, {
    name: "get_view_settings",
    title: "Get view settings",
    description:
      "Without 'view': settings of the ACTIVE window — drawingScale (N of 1:N), layerCombination, penSet, structureDisplay, renovationFilter " +
      "(+ available filters) and the model view option combinations matching the current options. With 'view' (View Map item GUID/name): " +
      "the settings stored in that saved view (layer combination, model view options, pen set, dimension style, scale, structure display, " +
      "zoom, renovation filter, graphic overrides, 3D style, rendering scene; storedInView tells which ones the view stores).",
    input: { view: ViewRef.optional().describe("Saved view (View Map). Omit for the current window") },
    annotations: READ_ONLY,
    handler: async ({ view }, { ac }) => ac.addon("GetViewSettings", view !== undefined ? { view } : {}),
  });

  defineTool(server, ctx, {
    name: "set_view_settings",
    title: "Set view settings",
    description:
      "Changes view settings of the ACTIVE window (drawingScale, layerCombination, modelViewOptions, structureDisplay, renovationFilter, " +
      "style3D) or — with 'view' — of a saved View Map view (additionally penSet, dimensionStyle, graphicOverrides, zoom, ignoreSavedZoom, " +
      "renderingScene; the view then stores these settings). Attribute names are localized: list them with get_attributes (types " +
      "LayerCombination, ModelViewOption, PenTable, DimensionStandard). Returns {changed, settings}.",
    input: {
      view: ViewRef.optional().describe("Saved view to change (View Map). Omit to change the current window"),
      drawingScale: z.number().min(1).max(100000).optional().describe("Scale denominator N of 1:N, e.g. 50 or 100"),
      layerCombination: AttrRef.optional().describe("Layer combination (name/index)"),
      modelViewOptions: AttrRef.optional().describe("Model view options combination (name/index)"),
      penSet: AttrRef.optional().describe("Saved views only: pen set (PenTable attribute)"),
      dimensionStyle: AttrRef.optional().describe("Saved views only: dimension standard"),
      graphicOverrides: z.string().min(1).optional().describe("Saved views only: graphic override combination name"),
      renovationFilter: z.string().min(1).optional().describe("Renovation filter name or GUID (see get_view_settings availableRenovationFilters)"),
      structureDisplay: z.enum(["EntireStructure", "CoreOnly", "WithoutFinishes", "StructureOnly"]).optional().describe("Partial structure display"),
      style3D: z.string().min(1).optional().describe("3D style name (current 3D window, or stored in a saved 3D view)"),
      renderingScene: z.string().min(1).optional().describe("Saved views only: rendering scene name"),
      zoom: Box2D.optional().describe("Saved views only: stored zoom area (m)"),
      ignoreSavedZoom: z.boolean().optional().describe("Saved views only: open the view without its stored zoom"),
    },
    annotations: MODIFIES,
    handler: async (args, { ac }) => {
      const params = compact(args as Json);
      if (Object.keys(params).filter((k) => k !== "view").length === 0) {
        throw new Error("Invalid input: nothing to change — give at least one setting (drawingScale, layerCombination, modelViewOptions, ...)");
      }
      return ac.addon("SetViewSettings", params);
    },
  });
}
