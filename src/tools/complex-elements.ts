/**
 * Complex element tools: morphs, curtain walls, stairs, railings.
 * The add-on adapters live in addon/Src/Commands/ComplexElements*.cpp; field names match 1:1.
 *
 *   create_morphs / create_curtain_walls / create_stairs / create_railings  -> CreateElements ({type, ...fields})
 *   modify_morphs / modify_curtain_walls / modify_stairs / modify_railings  -> ModifyElements (after a type check)
 *   modify_curtain_wall_parts                                                -> ModifyElements on CW panels / frames
 *   get_morph_geometry                                                       -> GetMorphGeometry (editable BREP)
 * get_element_details returns the same field names (plus counts, part GUIDs, grids, classes, bounding boxes).
 */

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import { CREATES, defineTool, MODIFIES, READ_ONLY, type ToolContext } from "./define.js";
import { createElements } from "./elements.js";
import {
  ArcSpec,
  AttrRef,
  CommonElementFields,
  ElementRefs,
  Guid,
  guidOf,
  PenIndex,
  Point2D,
  Point3D,
  PointMaybeZ,
  Polygon,
  Polyline,
} from "./schemas.js";

// --- Shared building blocks -----------------------------------------------------------------

const SurfaceOverride = z
  .union([AttrRef, z.literal(false)])
  .describe("Surface (material) name/index (localized names — list with get_attributes type Surface), or false = no override");

const PenOverride = z.union([PenIndex, z.literal(false)]);

const FloorPlanDisplay = z
  .enum(["Projected", "ProjectedWithOverhead", "CutOnly", "OutlinesOnly", "OverheadAll", "SymbolicCut"])
  .describe("Floor plan display option (Floor Plan and Section > Show on Floor Plan)");

const ViewDepth = z
  .enum(["ToFloorPlanRange", "ToAbsoluteLimit", "EntireElement"])
  .describe("How far the element is shown on the floor plan (Show Projection)");

const LineType = AttrRef.describe("Line type name/index (localized — list with get_attributes type Line)");

function hasPath(v: Record<string, unknown>, key: string): boolean {
  return v[key] !== undefined || (v["begin"] !== undefined && v["end"] !== undefined);
}

// --- Morph ----------------------------------------------------------------------------------

const MorphVertex = z
  .union([Point3D, z.tuple([z.number(), z.number(), z.number()])])
  .describe("Vertex {x, y, z} or [x, y, z] in meters: x/y project coordinates, z relative to the home story level");

const IndexLoop = z
  .array(z.number().int().min(0))
  .min(3)
  .describe("0-based indices into 'vertices', counter-clockwise when seen from OUTSIDE the body (right-hand rule = outward normal)");

const MeshFace = z
  .union([
    IndexLoop,
    z.object({
      vertices: IndexLoop,
      holes: z
        .array(z.union([IndexLoop, z.object({ vertices: IndexLoop })]))
        .optional()
        .describe("Openings inside the face, each a loop of vertex indices wound opposite to the outer loop (clockwise seen from outside)"),
      surface: SurfaceOverride.optional().describe("Surface of this face (default: faceSurface, else the morph's 'surface')"),
    }),
  ])
  .describe("Planar face: [i, j, k, ...] or {vertices, holes?, surface?}");

const PartSurfaces = {
  topSurface: SurfaceOverride.optional().describe("Surface of the top face"),
  bottomSurface: SurfaceOverride.optional().describe("Surface of the bottom face"),
  sideSurface: SurfaceOverride.optional().describe("Surface of the side faces"),
};

const MorphBox = z
  .object({
    origin: Point3D.describe("Box corner with the smallest local x/y/z (m; z relative to the home story)"),
    size: z
      .object({ x: z.number().positive(), y: z.number().positive(), z: z.number().positive() })
      .describe("Box dimensions in m along its local x (after rotation), y and the vertical"),
    rotation: z.number().optional().describe("Rotation in degrees, counter-clockwise seen from above, around the vertical axis through origin (default 0)"),
    ...PartSurfaces,
  })
  .describe("Rectangular box");

const MorphExtrusion = z
  .object({
    polygon: Polygon.describe("Plan polygon (m): points, optional arcs (curved edges, tessellated) and holes"),
    zBottom: z.number().describe("Bottom elevation in m relative to the home story level"),
    zTop: z.number().describe("Top elevation in m relative to the home story level (must be > zBottom)"),
    arcSegmentAngle: z.number().min(0.5).max(90).optional().describe("Max angle in degrees per straight segment when tessellating arcs (default 10)"),
    ...PartSurfaces,
  })
  .describe("Vertical extrusion of a plan polygon between zBottom and zTop (prism with holes)");

const MorphMesh = z
  .object({
    vertices: z.array(MorphVertex).min(3).max(200000).describe("Vertex list; faces reference it by 0-based index"),
    faces: z.array(MeshFace).min(1).max(200000).describe(
      "Faces. For a closed solid every edge must be shared by exactly two faces traversed in opposite directions; open meshes become Surface bodies",
    ),
  })
  .describe("Arbitrary polyhedral mesh (BREP). get_morph_geometry returns existing morphs in this format");

const MorphGeometryFields = {
  box: MorphBox.optional(),
  extrusion: MorphExtrusion.optional(),
  mesh: MorphMesh.optional(),
};

const MorphFieldsBase = {
  bodyType: z
    .enum(["Solid", "Surface"])
    .optional()
    .describe("Solid (closed volume) or Surface (open shell). Default: Solid when the mesh is closed, else Surface"),
  edgeType: z.enum(["SoftHidden", "HardHidden", "HardVisible"]).optional().describe("Default edge display of the body"),
  surface: SurfaceOverride.optional().describe("Surface of all faces without their own surface (false = building material surface)"),
  faceSurface: SurfaceOverride.optional().describe(
    "Per-face surface given to EVERY face (on create/replace: faces without their own 'surface'); false = clear per-face overrides",
  ),
  buildingMaterial: AttrRef.optional().describe("Building material name/index (localized — get_attributes type BuildingMaterial)"),
  castShadow: z.boolean().optional().describe("Body casts shadows"),
  receiveShadow: z.boolean().optional().describe("Body receives shadows"),
  floorPlanDisplay: FloorPlanDisplay.optional(),
  viewDepth: ViewDepth.optional(),
  cutLinePen: PenIndex.optional().describe("Pen of cut lines"),
  cutLineType: LineType.optional(),
  uncutLinePen: PenIndex.optional().describe("Pen of uncut (projected) lines"),
  uncutLineType: LineType.optional(),
  overheadLinePen: PenIndex.optional().describe("Pen of overhead lines"),
  overheadLineType: LineType.optional(),
  cutFillPen: PenOverride.optional().describe("Override the cut fill foreground pen (false = pen of the building material)"),
  cutFillBackgroundPen: PenOverride.optional().describe("Override the cut fill background pen (false = pen of the building material)"),
  showCoverFill: z.boolean().optional().describe("Show a cover fill on the uncut part"),
  coverFill: AttrRef.optional().describe("Cover fill type name/index (turns showCoverFill on)"),
  coverFillPen: PenIndex.optional(),
  coverFillBackgroundPen: PenIndex.optional(),
  coverFillFromSurface: z.boolean().optional().describe("Use the fill of the surface as cover fill"),
  outlineContourDisplay: z.boolean().optional().describe("Show only the outline contour on the floor plan"),
};

function countGeometry(v: Record<string, unknown>): number {
  return ["box", "extrusion", "mesh"].filter((k) => v[k] !== undefined).length;
}

export const MorphSpec = z
  .object({ ...MorphGeometryFields, ...MorphFieldsBase, ...CommonElementFields })
  .refine((v) => countGeometry(v) === 1, { message: "give exactly one of box, extrusion or mesh" })
  .describe("New morph: exactly one of box / extrusion / mesh plus optional settings");

export const MorphPatch = z
  .object({
    guid: Guid.describe("GUID of the morph to change"),
    ...MorphGeometryFields,
    offset: Point3D.optional().describe("Move the morph by this vector (m)"),
    level: z.number().optional().describe("Move the morph vertically so its base is at this height (m, relative to the home story)"),
    ...MorphFieldsBase,
    ...CommonElementFields,
  })
  .refine((v) => countGeometry(v) <= 1, { message: "give at most one of box, extrusion or mesh (it replaces the whole body)" })
  .refine((v) => !(countGeometry(v) === 1 && v.level !== undefined), { message: "a new geometry defines its own height: do not combine it with level" })
  .describe("Morph changes: only the given fields change; box/extrusion/mesh REPLACES the body");

type LoopInput = number[] | { vertices: number[] };
type FaceInput = number[] | { vertices: number[]; holes?: LoopInput[]; surface?: unknown };

/** Converts tuple vertices / array faces into the object form the add-on reads. */
export function normalizeMesh(mesh: { vertices: Array<{ x: number; y: number; z: number } | [number, number, number]>; faces: FaceInput[] }) {
  return {
    vertices: mesh.vertices.map((v) => (Array.isArray(v) ? { x: v[0], y: v[1], z: v[2] } : v)),
    faces: mesh.faces.map((f) => {
      if (Array.isArray(f)) return { vertices: f };
      const face: Record<string, unknown> = { vertices: f.vertices };
      if (f.holes) face["holes"] = f.holes.map((h) => (Array.isArray(h) ? { vertices: h } : h));
      if (f.surface !== undefined) face["surface"] = f.surface;
      return face;
    }),
  };
}

function normalizeMorph<T extends Record<string, unknown>>(spec: T): T {
  const mesh = spec["mesh"] as Parameters<typeof normalizeMesh>[0] | undefined;
  return mesh ? { ...spec, mesh: normalizeMesh(mesh) } : spec;
}

// --- Curtain wall ---------------------------------------------------------------------------

const CWGrid = z
  .object({
    logic: z
      .enum(["FixedSizes", "BestDivision", "NumberOfDivisions"])
      .optional()
      .describe(
        "FixedSizes: 'sizes' repeat exactly, the remainder goes to the end; BestDivision: 'sizes' are target sizes stretched to fit whole modules; " +
          "NumberOfDivisions: 'divisions' equal parts (default: keep the current logic)",
      ),
    sizes: z.array(z.number().min(0.001)).min(1).max(1000).optional().describe("Module sizes in m, repeated along the grid direction"),
    divisions: z.number().int().min(1).max(10000).optional().describe("Number of equal parts (only for NumberOfDivisions)"),
    origin: z
      .enum(["StartWithPattern", "StartFromCenter", "AlignToCenter", "EndWithPattern"])
      .optional()
      .describe("Where the pattern starts"),
    flexible: z.array(z.number().int().min(0)).optional().describe("0-based indices of 'sizes' items that may stretch to absorb the remainder (default [0]; Archicad needs at least one flexible module when sizes change)"),
    endWith: z.number().int().min(0).optional().describe("0-based index of the 'sizes' item the pattern ends with (default: last)"),
  })
  .describe("Curtain wall grid pattern. NOTE: changing the number of pattern items resets all cells to the first cell's panel/frame classes");

const CWFieldsBase = {
  height: z.number().positive().optional().describe("Height in m"),
  flipped: z.boolean().optional().describe("Mirror the curtain wall to the other side of its base line"),
  zoneRelation: z.enum(["Boundary", "ReduceArea", "None"]).optional().describe("How the curtain wall affects zone boundaries"),
  boundaryFramePosition: z.enum(["Outside", "Center", "Inside"]).optional().describe("Position of the boundary frames relative to the base line"),
  primaryGrid: CWGrid.optional().describe("Primary grid = module WIDTHS along the curtain wall length (vertical mullions)"),
  secondaryGrid: CWGrid.optional().describe("Secondary grid = module HEIGHTS (horizontal transoms)"),
  primarySpacing: z.number().min(0.001).optional().describe("Shorthand: primaryGrid {logic: FixedSizes, sizes: [value]} (m)"),
  secondarySpacing: z.number().min(0.001).optional().describe("Shorthand: secondaryGrid {logic: FixedSizes, sizes: [value]} (m)"),
  panelSurface: SurfaceOverride.optional().describe("Outer, inner and edge surface of ALL panel classes (e.g. a glass surface)"),
  panelOuterSurface: SurfaceOverride.optional().describe("Outer surface of all panel classes"),
  panelInnerSurface: SurfaceOverride.optional().describe("Inner surface of all panel classes"),
  panelThickness: z.number().positive().optional().describe("Thickness of all panel classes (m)"),
  frameSurface: AttrRef.optional().describe("Surface of all frame classes (incl. corner/boundary frames)"),
  frameBuildingMaterial: AttrRef.optional().describe("Building material of all frame classes"),
  floorPlanDisplay: FloorPlanDisplay.optional(),
  viewDepth: ViewDepth.optional(),
};

export const CurtainWallSpec = z
  .object({
    path: Polyline.optional().describe(
      "Base line on the plan (m): points (and arcs for curved segments). Each segment becomes a curtain wall segment; repeat the first point at the end to close it",
    ),
    begin: Point2D.optional().describe("Straight curtain wall: start point (instead of path)"),
    end: Point2D.optional().describe("Straight curtain wall: end point (instead of path)"),
    bottomOffset: z.number().optional().describe("Base elevation relative to the home story (m, create only)"),
    ...CWFieldsBase,
    ...CommonElementFields,
  })
  .refine((v) => hasPath(v, "path"), { message: "give 'path' or 'begin' + 'end'" })
  .describe("New curtain wall");

export const CurtainWallPatch = z
  .object({ guid: Guid.describe("GUID of the curtain wall to change"), ...CWFieldsBase, ...CommonElementFields })
  .describe("Curtain wall changes (the base line cannot be changed: move/rotate or recreate)");

export const CurtainWallPartPatch = z
  .object({
    guid: Guid.describe("GUID of a CurtainWallPanel or CurtainWallFrame (see 'panels'/'frames' in get_element_details of the curtain wall)"),
    outerSurface: SurfaceOverride.optional().describe("Panel only: outer surface (customizes the panel)"),
    innerSurface: SurfaceOverride.optional().describe("Panel only: inner surface (customizes the panel)"),
    cutSurface: SurfaceOverride.optional().describe("Panel only: edge/cut surface (customizes the panel)"),
    thickness: z.number().positive().optional().describe("Panel only: thickness in m (customizes the panel)"),
    deleted: z.literal(true).optional().describe("Panel only: true = delete the panel (leaves an empty cell)"),
    surface: AttrRef.optional().describe("Frame only: surface (customizes the frame)"),
    buildingMaterial: AttrRef.optional().describe("Panel or frame: building material (customizes it)"),
    classId: z
      .number()
      .int()
      .min(0)
      .optional()
      .describe("Assign a class: panels 0 = deleted, 1+ = panelClasses[classId-1]; frames 0 Merged, 1 Division, 2 Corner, 3 Boundary, 4+ = frameClasses[classId-4]"),
    layer: CommonElementFields.layer,
  })
  .describe("Curtain wall panel / frame change");

// --- Stair ------------------------------------------------------------------------------------

const LinePosition = z.enum(["Left", "Center", "Right"]);

const StairFieldsBase = {
  baselinePosition: LinePosition.optional().describe("Where the baseline runs in the flight (create default: Center)"),
  baselineOffset: z.number().optional().describe("Offset of the flight from the baseline (m)"),
  height: z
    .number()
    .positive()
    .optional()
    .describe(
      "Total stair height (m). Unlinks the top unless topLinkedStory is also given. height = riserCount × riserHeight is always kept: " +
        "give any two of the three; height alone keeps the current riser height as closely as possible (rounded riser count)",
    ),
  topLinkedStory: z.number().int().min(0).optional().describe("Link the top to the story N levels above (0 = unlinked, use height)"),
  topOffset: z.number().optional().describe("Offset from the linked top story (m)"),
  width: z.number().positive().optional().describe("Flight width (m)"),
  riserCount: z
    .number()
    .int()
    .min(1)
    .max(1000)
    .optional()
    .describe("Number of risers (riserHeight = height / riserCount unless riserHeight is given; with riserHeight and no height, height = riserCount × riserHeight)"),
  riserHeight: z
    .number()
    .positive()
    .optional()
    .describe("Target riser height (m). Without riserCount the count is rounded to fit the height and the riser height adjusted to divide it exactly"),
  treadDepth: z.number().positive().optional().describe("Tread (going) depth in m; locks the tread depth unless treadDepthLocked is given"),
  treadDepthLocked: z.boolean().optional().describe("Keep treadDepth fixed when the stair is recalculated"),
  totalHeightLocked: z.boolean().optional().describe("Keep the total height fixed when the stair is recalculated"),
  walkingLinePosition: LinePosition.optional().describe("Position of the walking line"),
  walkingLineOffset: z.number().optional().describe("Offset of the walking line (m)"),
  direction: z.enum(["Upward", "Inverse"]).optional().describe("Upward = the stair climbs from the first to the last baseline point"),
  numbering: z.enum(["Treads", "Risers"]).optional().describe("What the step numbering counts"),
  extraTopTread: z.boolean().optional().describe("Add a tread at the top"),
  extraBottomTread: z.boolean().optional().describe("Add a tread at the bottom"),
  treadThickness: z.number().positive().optional().describe("Tread thickness of flights and landings (m)"),
  riserThickness: z.number().positive().optional().describe("Riser thickness (m)"),
  riserCrossSection: z.enum(["Simple", "Slanted"]).optional().describe("Riser cross section"),
  riserAngle: z.number().optional().describe("Slant angle of slanted risers (degrees)"),
  ignoreRules: z
    .boolean()
    .optional()
    .describe("true = switch off the stair rule checks (riser/tread limits, 2R+G, pitch) — use when creation fails because of the rules"),
};

/** height, riserCount and riserHeight must agree when all three are given (height = riserCount × riserHeight). */
function risersConsistent(v: { height?: number; riserCount?: number; riserHeight?: number }): boolean {
  if (v.height === undefined || v.riserCount === undefined || v.riserHeight === undefined) return true;
  return Math.abs(v.riserCount * v.riserHeight - v.height) <= 1e-4;
}

const RISERS_MESSAGE = {
  message: "height, riserCount and riserHeight disagree (height must equal riserCount × riserHeight): give only two of them",
};

export const StairSpec = z
  .object({
    baseline: Polyline.optional().describe(
      "Stair baseline on the plan (m), from the bottom to the top step: 2 points = straight flight; more points = turns (landings/winders by the tool defaults)",
    ),
    begin: Point2D.optional().describe("Straight stair: bottom point of the baseline (instead of baseline)"),
    end: Point2D.optional().describe("Straight stair: top point of the baseline (instead of baseline)"),
    ...StairFieldsBase,
    ...CommonElementFields,
  })
  .refine((v) => hasPath(v, "baseline"), { message: "give 'baseline' or 'begin' + 'end'" })
  .refine(risersConsistent, RISERS_MESSAGE)
  .describe("New stair");

export const StairPatch = z
  .object({ guid: Guid.describe("GUID of the stair to change"), ...StairFieldsBase, ...CommonElementFields })
  .refine(risersConsistent, RISERS_MESSAGE)
  .describe("Stair changes (the baseline cannot be changed: move/rotate or recreate)");

// --- Railing ------------------------------------------------------------------------------------

const RailingPath = z
  .union([
    z.array(PointMaybeZ).min(2),
    z.object({ points: z.array(PointMaybeZ).min(2), arcs: z.array(ArcSpec).optional() }),
  ])
  .describe(
    "Railing reference line (m): points {x, y, z?} (z = elevation of that point above the railing base, e.g. to follow a ramp; all or none) and optional arcs. " +
      "Repeat the first point at the end for a closed railing",
  );

const RailingFieldsBase = {
  height: z.number().positive().optional().describe("Railing (segment) height in m"),
  bottomOffset: z.number().optional().describe("Base elevation relative to the home story (m)"),
  referenceLine: z.enum(["Left", "Center", "Right"]).optional().describe("Where the reference line sits in the railing"),
  offset: z.number().optional().describe("Horizontal offset of the segments from the reference line (m)"),
  postOffset: z.number().optional().describe("Offset of the posts at the nodes (m)"),
  referenceLinePen: PenIndex.optional(),
  contourPen: PenIndex.optional(),
};

export const RailingSpec = z
  .object({
    path: RailingPath.optional(),
    begin: PointMaybeZ.optional().describe("Straight railing: start point (instead of path)"),
    end: PointMaybeZ.optional().describe("Straight railing: end point (instead of path)"),
    ...RailingFieldsBase,
    ...CommonElementFields,
  })
  .refine((v) => hasPath(v, "path"), { message: "give 'path' or 'begin' + 'end'" })
  .describe("New railing");

export const RailingPatch = z
  .object({ guid: Guid.describe("GUID of the railing to change"), ...RailingFieldsBase, ...CommonElementFields })
  .describe("Railing changes (the path cannot be changed: move/rotate or recreate)");

// --- Typed modify helper ------------------------------------------------------------------------

interface TypesResult {
  typesOfElements?: Array<{ typeOfElement?: { elementType?: string }; error?: { code?: number; message?: string } }>;
}

interface ResultsResponse {
  results?: unknown[];
}

const BAD_PARAMS = -2130313112;

/**
 * Sends patches to ModifyElements after checking with API.GetTypesOfElements that every GUID has one of the
 * expected types (and passes `check`); failing items get a per-item error, the others are still modified.
 */
export async function modifyComplexTyped(
  ctx: ToolContext,
  patches: Array<Record<string, unknown> & { guid: string }>,
  expectedTypes: readonly string[],
  toolName: string,
  undoName: string,
  check?: (type: string, patch: Record<string, unknown>) => string | undefined,
): Promise<{ results: unknown[] }> {
  const results: unknown[] = new Array(patches.length).fill(undefined);
  let types: TypesResult["typesOfElements"];
  try {
    const res = await ctx.ac.api<TypesResult>("API.GetTypesOfElements", {
      elements: patches.map((p) => ({ elementId: { guid: p.guid } })),
    });
    types = res.typesOfElements;
  } catch {
    types = undefined; // type check unavailable: the add-on validates each element
  }

  const send: Array<Record<string, unknown>> = [];
  const sentIndex: number[] = [];
  patches.forEach((patch, i) => {
    const entry = types?.[i];
    const type = entry?.typeOfElement?.elementType;
    if (types !== undefined) {
      let message: string | undefined;
      if (type === undefined) {
        message = `Element ${patch.guid} was not found${entry?.error?.message ? ` (${entry.error.message})` : ""}. Check the GUID with find_elements / get_element_details.`;
      } else if (!expectedTypes.includes(type)) {
        message = `Element ${patch.guid} is a ${type}, not a ${expectedTypes.join(" or ")}. Use modify_elements or the ${type} tool instead of ${toolName}.`;
      } else if (check) {
        message = check(type, patch);
      }
      if (message !== undefined) {
        results[i] = { guid: patch.guid, error: { code: entry?.error?.code ?? BAD_PARAMS, message } };
        return;
      }
    }
    send.push(patch);
    sentIndex.push(i);
  });

  if (send.length > 0) {
    const res = await ctx.ac.addon<ResultsResponse>("ModifyElements", { elements: send, undoName });
    const list = Array.isArray(res.results) ? res.results : [];
    sentIndex.forEach((original, k) => {
      results[original] = list[k] ?? { guid: patches[original]!.guid, error: { message: "No result returned by the add-on" } };
    });
  }
  return { results };
}

const PANEL_ONLY = ["outerSurface", "innerSurface", "cutSurface", "thickness", "deleted"] as const;
const FRAME_ONLY = ["surface"] as const;

function checkCurtainWallPart(type: string, patch: Record<string, unknown>): string | undefined {
  const wrong = (type === "CurtainWallFrame" ? PANEL_ONLY : FRAME_ONLY).filter((k) => patch[k] !== undefined);
  if (wrong.length === 0) return undefined;
  return `Field(s) ${wrong.join(", ")} do not apply to a ${type}: panels take outerSurface/innerSurface/cutSurface/thickness/deleted, frames take surface.`;
}

// --- Registration -------------------------------------------------------------------------------

const UNITS = "Coordinates in meters, angles in degrees, on the given story (default: current story); unspecified settings come from the tool defaults.";

export function registerComplexElementTools(server: McpServer, ctx: ToolContext): void {
  defineTool(server, ctx, {
    name: "create_morphs",
    title: "Create morphs",
    description:
      "Creates morphs (free-form 3D bodies) in one undo step. Each item needs exactly ONE geometry: " +
      "'box' {origin {x,y,z}, size {x,y,z}, rotation?}; 'extrusion' {polygon, zBottom, zTop} (prism of a plan polygon with holes/arcs); or " +
      "'mesh' {vertices: [{x,y,z}], faces: [[i,j,k,...]]} (any polyhedron; faces counter-clockwise seen from outside, planar). " +
      "x/y are project coordinates, z is relative to the home story level. Closed bodies become Solid, open meshes Surface. " +
      "Per-face surfaces: box/extrusion top/bottom/sideSurface, mesh face {vertices, surface}; 'surface' covers the rest. " +
      UNITS +
      " Returns [{guid, type} | {error}] in input order. Read back with get_element_details (body counts, bounds) or get_morph_geometry; " +
      "change with modify_morphs; combine with solid_operation.",
    input: {
      morphs: z.array(MorphSpec).min(1).max(500).describe("Morphs to create"),
      undoName: z.string().optional().describe("Name of the undo step shown in Archicad"),
    },
    annotations: CREATES,
    handler: async ({ morphs, undoName }, c) =>
      createElements(
        c,
        morphs.map((m) => normalizeMorph({ type: "Morph", ...(m as Record<string, unknown>) })),
        undoName ?? "Create morphs (Claude)",
      ),
  });

  defineTool(server, ctx, {
    name: "modify_morphs",
    title: "Modify morphs",
    description:
      "Changes existing morphs in one undo step: replace the whole body (box / extrusion / mesh — e.g. edit the output of get_morph_geometry and send it back), " +
      "move it (offset {x,y,z}, level), set every face's surface (faceSurface) or the default surface, building material, body/edge type, shadows and " +
      "floor plan display. Only the given fields change. Returns [{guid} | {error}] in input order.",
    input: {
      morphs: z.array(MorphPatch).min(1).max(500).describe("Morph patches"),
      undoName: z.string().optional().describe("Name of the undo step shown in Archicad"),
    },
    annotations: MODIFIES,
    handler: async ({ morphs, undoName }, c) =>
      modifyComplexTyped(
        c,
        morphs.map((m) => normalizeMorph(m as Record<string, unknown> & { guid: string })),
        ["Morph"],
        "modify_morphs",
        undoName ?? "Modify morphs (Claude)",
      ),
  });

  defineTool(server, ctx, {
    name: "get_morph_geometry",
    title: "Get morph geometry",
    description:
      "Returns the editable geometry of morphs: {elements: [{guid, bodyType, vertexCount, faceCount, vertices: [{x,y,z}], " +
      "faces: [{vertices: [i,...], holes?: [{vertices}], surface?, hidden?}]}]} — the same format as the 'mesh' input of create_morphs / modify_morphs, " +
      "with the morph transformation applied (x/y project coordinates, z relative to the home story). Per-item errors for non-morphs.",
    input: {
      elements: ElementRefs.max(100).describe("Morph GUIDs"),
      maxVertices: z.number().int().min(1).max(200000).optional().describe("Refuse bodies with more vertices than this (default 20000)"),
    },
    annotations: READ_ONLY,
    handler: async ({ elements, maxVertices }, { ac }) =>
      ac.addon("GetMorphGeometry", { elements: elements.map(guidOf), ...(maxVertices !== undefined ? { maxVertices } : {}) }),
  });

  defineTool(server, ctx, {
    name: "create_curtain_walls",
    title: "Create curtain walls",
    description:
      "Creates curtain walls in one undo step along a straight ('begin'/'end') or polyline/curved ('path') base line with the Curtain Wall tool's default " +
      "scheme (frame/panel classes). Set 'height', the grid (primarySpacing = module width along the wall, secondarySpacing = module height, or full " +
      "primaryGrid/secondaryGrid patterns), panel surfaces/thickness and frame surface. " +
      UNITS +
      " Returns [{guid, type} | {error}]. get_element_details lists segments, grids, frame/panel classes and the GUIDs of every frame and panel " +
      "(edit single panels/frames with modify_curtain_wall_parts; change height/grid/classes with modify_curtain_walls).",
    input: {
      curtainWalls: z.array(CurtainWallSpec).min(1).max(200).describe("Curtain walls to create"),
      undoName: z.string().optional().describe("Name of the undo step shown in Archicad"),
    },
    annotations: CREATES,
    handler: async ({ curtainWalls, undoName }, c) =>
      createElements(c, curtainWalls.map((w) => ({ type: "CurtainWall", ...w })), undoName ?? "Create curtain walls (Claude)"),
  });

  defineTool(server, ctx, {
    name: "modify_curtain_walls",
    title: "Modify curtain walls",
    description:
      "Changes existing curtain walls in one undo step: height, flip, grid patterns (primaryGrid/secondaryGrid or the *Spacing shorthands), " +
      "surfaces/thickness of all panel classes, surface/building material of all frame classes, zone relation, floor plan display. " +
      "The base line cannot be changed (use move_elements/rotate_elements or recreate). Returns [{guid} | {error}].",
    input: {
      curtainWalls: z.array(CurtainWallPatch).min(1).max(200).describe("Curtain wall patches"),
      undoName: z.string().optional().describe("Name of the undo step shown in Archicad"),
    },
    annotations: MODIFIES,
    handler: async ({ curtainWalls, undoName }, c) =>
      modifyComplexTyped(c, curtainWalls, ["CurtainWall"], "modify_curtain_walls", undoName ?? "Modify curtain walls (Claude)"),
  });

  defineTool(server, ctx, {
    name: "modify_curtain_wall_parts",
    title: "Modify curtain wall panels / frames",
    description:
      "Changes individual curtain wall PANELS (outer/inner/cut surface, thickness, building material, delete, class) and FRAMES (surface, building " +
      "material, class) in one undo step. Setting a property customizes the part (it leaves its class) unless classId is given. " +
      "Get the part GUIDs from get_element_details of the curtain wall ('panels', 'frames') or get_subelements. Returns [{guid} | {error}].",
    input: {
      parts: z.array(CurtainWallPartPatch).min(1).max(2000).describe("Panel / frame patches"),
      undoName: z.string().optional().describe("Name of the undo step shown in Archicad"),
    },
    annotations: MODIFIES,
    handler: async ({ parts, undoName }, c) =>
      modifyComplexTyped(
        c,
        parts,
        ["CurtainWallPanel", "CurtainWallFrame"],
        "modify_curtain_wall_parts",
        undoName ?? "Modify curtain wall parts (Claude)",
        checkCurtainWallPart,
      ),
  });

  defineTool(server, ctx, {
    name: "create_stairs",
    title: "Create stairs",
    description:
      "Creates stairs in one undo step from a baseline (bottom to top): 'begin'/'end' for a straight flight or a 'baseline' polyline for turning stairs " +
      "(landings/winders follow the Stair tool defaults). Typical: {begin: {x:0,y:0}, end: {x:4.5,y:0}, height: 3, width: 1.2, riserCount: 17}. " +
      "The baseline must be long enough for the treads (≈ (riserCount-1) × treadDepth); if Archicad rejects the geometry because of the stair " +
      "rules, adjust the values or pass ignoreRules: true. " +
      UNITS +
      " Returns [{guid, type} | {error}]. get_element_details shows riser/tread counts, pitch, baseline, boundaries and part GUIDs; add a railing " +
      "along a boundary with create_railings.",
    input: {
      stairs: z.array(StairSpec).min(1).max(200).describe("Stairs to create"),
      undoName: z.string().optional().describe("Name of the undo step shown in Archicad"),
    },
    annotations: CREATES,
    handler: async ({ stairs, undoName }, c) =>
      createElements(c, stairs.map((s) => ({ type: "Stair", ...s })), undoName ?? "Create stairs (Claude)"),
  });

  defineTool(server, ctx, {
    name: "modify_stairs",
    title: "Modify stairs",
    description:
      "Changes existing stairs in one undo step: height / top link, width, riser count/height, tread depth and locks, baseline/walking line position, " +
      "direction, numbering, tread/riser thickness, rule checks. The baseline cannot be changed (move/rotate or recreate). Returns [{guid} | {error}].",
    input: {
      stairs: z.array(StairPatch).min(1).max(200).describe("Stair patches"),
      undoName: z.string().optional().describe("Name of the undo step shown in Archicad"),
    },
    annotations: MODIFIES,
    handler: async ({ stairs, undoName }, c) => modifyComplexTyped(c, stairs, ["Stair"], "modify_stairs", undoName ?? "Modify stairs (Claude)"),
  });

  defineTool(server, ctx, {
    name: "create_railings",
    title: "Create railings",
    description:
      "Creates railings in one undo step along a reference line: 'begin'/'end' or a 'path' polyline (points may carry z to follow a slope; add arcs for " +
      "curves) with the Railing tool's default posts/rails/panels. Set 'height', 'bottomOffset' and the reference line side. " +
      UNITS +
      " Tip: to guard a stair, use its leftBoundary/rightBoundary from get_element_details as the path. Returns [{guid, type} | {error}]; " +
      "get_element_details lists the segments and the GUIDs of posts, rails, handrails, panels and balusters.",
    input: {
      railings: z.array(RailingSpec).min(1).max(500).describe("Railings to create"),
      undoName: z.string().optional().describe("Name of the undo step shown in Archicad"),
    },
    annotations: CREATES,
    handler: async ({ railings, undoName }, c) =>
      createElements(c, railings.map((r) => ({ type: "Railing", ...r })), undoName ?? "Create railings (Claude)"),
  });

  defineTool(server, ctx, {
    name: "modify_railings",
    title: "Modify railings",
    description:
      "Changes existing railings in one undo step: height of all segments, bottom offset, reference line side, segment offset, post offset, pens. " +
      "The path cannot be changed (move/rotate or recreate). Returns [{guid} | {error}].",
    input: {
      railings: z.array(RailingPatch).min(1).max(500).describe("Railing patches"),
      undoName: z.string().optional().describe("Name of the undo step shown in Archicad"),
    },
    annotations: MODIFIES,
    handler: async ({ railings, undoName }, c) =>
      modifyComplexTyped(c, railings, ["Railing"], "modify_railings", undoName ?? "Modify railings (Claude)"),
  });
}
