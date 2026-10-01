/**
 * Zod field shapes of the slabs-roofs family (Slab / Roof / Shell / Mesh).
 * Field names match the add-on adapters 1:1 (addon/Src/Commands/SlabsRoofs*.cpp).
 * Units: meters and DEGREES everywhere.
 */

import { z } from "zod";

import { AttrRef, CommonElementFields, Guid, PenIndex, Point2D, Polygon, Polyline } from "./schemas.js";

// --- Shared building blocks -----------------------------------------------------------------------

export const SurfaceOverride = z
  .union([AttrRef, z.literal(false)])
  .describe("Surface override: surface name/index (localized — list with get_attributes type 'Surface'), or false to remove the override");

const PenOverride = z.union([PenIndex, z.literal(false)]);

export const EdgeTrimType = z
  .enum(["Vertical", "Perpendicular", "Horizontal", "CustomAngle", "AlignWithCut"])
  .describe("Edge trim: Vertical, Perpendicular (to the element plane), Horizontal, CustomAngle (with an angle), AlignWithCut");

/** Slab edges: Archicad keeps only vertical and custom-angle edges on flat slabs. */
export const SlabEdgeTrimType = z
  .enum(["Vertical", "Perpendicular", "CustomAngle"])
  .describe(
    "Slab edge trim: Vertical (default), Perpendicular (same as Vertical on a flat slab) or CustomAngle (with an angle). Horizontal/AlignWithCut exist only for roofs and shells",
  );

const EdgeAngle = z
  .number()
  .gt(0)
  .lt(180)
  .describe("Custom edge angle in degrees (0-180 exclusive, 90 = vertical edge); implies edgeTrim 'CustomAngle'");

export const StoryVisibility = z
  .object({
    onHomeStory: z.boolean().optional().describe("Show on the home story"),
    allAbove: z.boolean().optional().describe("Show on all stories above the home story"),
    allBelow: z.boolean().optional().describe("Show on all stories below the home story"),
    storiesAbove: z.number().int().min(0).max(999).optional().describe("Show on this many stories above the home story"),
    storiesBelow: z.number().int().min(0).max(999).optional().describe("Show on this many stories below the home story"),
  })
  .describe("Floor plan story visibility of the contour and cover fill (only the given flags change)");

const Structure = {
  buildingMaterial: AttrRef.optional().describe("Basic structure: building material name/index (localized — get_attributes type 'BuildingMaterial')"),
  composite: AttrRef.optional().describe(
    "Composite structure: composite name/index (get_attributes type 'Composite'; its 'usage' must include this element type, e.g. usage.slabs for slabs); the composite defines the layers and total thickness",
  ),
};

const Surfaces = {
  topSurface: SurfaceOverride.optional().describe("Top surface override (surface name/index, false = use the structure's surface)"),
  bottomSurface: SurfaceOverride.optional().describe("Bottom surface override"),
  sideSurface: SurfaceOverride.optional().describe("Edge/side surface override (applies to all edges; per-edge surfaces via 'edges')"),
  surfacesChained: z.boolean().optional().describe("Chain the surface overrides (Archicad UI chain icon)"),
};

/** Floor plan attributes shared by all four types (pens 1-255, attributes by name/index). */
const FloorPlan = {
  contourPen: PenIndex.optional().describe("Pen of the uncut contour on the floor plan"),
  contourLineType: AttrRef.optional().describe("Line type of the uncut contour (get_attributes type 'Line')"),
  cutContourPen: PenIndex.optional().describe("Pen of the cut contour"),
  cutFillPen: PenOverride.optional().describe("Override the cut fill foreground pen (1-255); false = use the structure's pen"),
  cutFillBackgroundPen: PenOverride.optional().describe("Override the cut fill background pen (1-255); false = use the structure's pen"),
  showCoverFill: z.boolean().optional().describe("Show the floor plan cover fill"),
  coverFill: AttrRef.optional().describe("Cover fill type name/index (get_attributes type 'Fill')"),
  coverFillPen: PenIndex.optional().describe("Cover fill foreground pen"),
  coverFillBackgroundPen: PenIndex.optional().describe("Cover fill background pen"),
  storyVisibility: StoryVisibility.optional(),
};

// --- Slab ---------------------------------------------------------------------------------------------

export const SlabEdge = z
  .object({
    contour: z.number().int().min(0).optional().describe("0 = outline (default), 1..N = holes in the order given"),
    index: z.number().int().min(0).describe("Edge index: edge i runs from point i to point i+1 (the last edge closes the contour)"),
    trim: SlabEdgeTrimType.optional(),
    angle: EdgeAngle.optional(),
    surface: SurfaceOverride.optional().describe("Surface of this edge (false = structure's surface)"),
  })
  .describe("Per-edge override. Indices refer to the points of the polygon given in the same call, otherwise to the stored polygon as returned by get_element_details");

const SlabFieldsBase = {
  thickness: z.number().positive().optional().describe("Thickness in m (ignored for composite slabs: the composite defines it)"),
  level: z
    .number()
    .optional()
    .describe("Elevation of the reference plane relative to the home story level (m). With referencePlane 'Top' (usual) this is the slab TOP; e.g. 0 = top at story level"),
  referencePlane: z.enum(["Top", "CoreTop", "CoreBottom", "Bottom"]).optional().describe("Which plane of the slab 'level' refers to"),
  ...Structure,
  ...Surfaces,
  edgeTrim: SlabEdgeTrimType.optional().describe("Trim of ALL slab edges (per-edge overrides via 'edges'): Vertical, Perpendicular or CustomAngle (+ edgeAngle)"),
  edgeAngle: EdgeAngle.optional(),
  edges: z.array(SlabEdge).optional().describe("Per-edge trim / surface overrides"),
  ...FloorPlan,
  cutContourLineType: AttrRef.optional().describe("Line type of the cut contour"),
  hiddenContourPen: PenIndex.optional().describe("Pen of hidden contour lines"),
  hiddenContourLineType: AttrRef.optional().describe("Line type of hidden contour lines"),
  coverFillFromSurface: z.boolean().optional().describe("Use the vectorial 3D hatch of the top surface as cover fill"),
  ...CommonElementFields,
};

const SlabPolygon = Polygon.describe(
  "Slab outline in plan (m, project coordinates): array of points, or {points, arcs?, holes?}. Do not repeat the first point. Holes must lie inside the outline",
);

export const SlabSpec = z.object({ polygon: SlabPolygon, ...SlabFieldsBase });
export const SlabPatch = z.object({
  guid: Guid.describe("Slab GUID"),
  polygon: SlabPolygon.optional().describe("New outline incl. holes (replaces the whole shape; per-edge data is reset to the slab-wide trim/surface)"),
  ...SlabFieldsBase,
});

// --- Roof / shell common (API_ShellBaseType) -------------------------------------------------------------

const ShellBaseFields = {
  thickness: z.number().positive().optional().describe("Thickness in m (ignored for composite structures)"),
  ...Structure,
  ...Surfaces,
  edgeTrim: EdgeTrimType.optional().describe("Default trim of the edges"),
  edgeAngle: EdgeAngle.optional(),
  connectionBody: z
    .enum(["Editable", "ContoursDown", "PivotLinesDown", "UpwardsExtrusion", "DownwardsExtrusion"])
    .optional()
    .describe("Body extension used for connections/trims (Archicad 'Extend body for connections')"),
  floorPlanDisplay: z
    .enum(["Projected", "ProjectedWithOverhead", "CutOnly", "OutlinesOnly", "OverheadAll", "CutAll"])
    .optional()
    .describe("Floor plan display option"),
  viewDepth: z.enum(["FloorPlanRange", "AbsoluteLimit", "EntireElement"]).optional().describe("Show projection: to floor plan range, to absolute limit, entire element"),
  ...FloorPlan,
  cutContourLineType: AttrRef.optional().describe("Line type of the cut contour"),
  overheadLinePen: PenIndex.optional().describe("Pen of overhead lines"),
  overheadLineType: AttrRef.optional().describe("Line type of overhead lines"),
  coverFillFromSurface: z.boolean().optional().describe("Use the fill of the surface (vectorial 3D hatch) as cover fill"),
  coverFillAlignedToPivot: z.boolean().optional().describe("Align the cover fill with the pivot line (roofs)"),
  coverFillDistorted: z.boolean().optional().describe("Distort the cover fill with the slope"),
  autoStoryVisibility: z.boolean().optional().describe("Let Archicad compute the story visibility from the vertical extent"),
  ...CommonElementFields,
};

// --- Roof ---------------------------------------------------------------------------------------------------

const SlopeAngle = z.number().min(0).lt(90);

export const RoofEdge = SlabEdge.extend({
  trim: EdgeTrimType.optional(),
  edgeType: z
    .enum(["Undefined", "Ridge", "Valley", "Gable", "Hip", "Eaves", "Peak", "SideWall", "EndWall", "Dome", "Hollow"])
    .optional()
    .describe("Roof edge type (used by roof accessories/schedules)"),
});

export const PivotEdge = z
  .object({
    contour: z.number().int().min(0).optional().describe("0 = pivot polygon outline (default), 1..N = holes"),
    index: z.number().int().min(0).describe("Pivot edge index: edge i runs from pivotPolygon point i to point i+1"),
    angle: z.number().gt(0).lt(90).optional().describe("Slope of this roof plane in degrees (all levels)"),
    angles: z.array(z.number().gt(0).lt(90)).optional().describe("Slope per roof level (index 0 = lowest level)"),
    gable: z.boolean().optional().describe("true = vertical gable end on this edge (no roof plane), false = sloped plane"),
    eavesOverhang: z.number().optional().describe("Eaves overhang of this plane beyond the pivot edge (m)"),
  })
  .describe("Per-plane (pivot edge) settings of a multi-plane roof");

export const RoofLevel = z.object({
  angle: z.number().gt(0).lt(90).describe("Slope of this roof level in degrees"),
  height: z
    .number()
    .positive()
    .optional()
    .describe(
      "Height above the pivot polygon (m) where this level ENDS and the next level begins (the pitch break). Required for every level except the last; the top level always continues to the ridge. Strictly increasing",
    ),
});

const RoofLevels = z
  .array(RoofLevel)
  .min(1)
  .max(16)
  .superRefine((levels, ctx) => {
    let previous = 0;
    levels.forEach((lv, i) => {
      const last = i === levels.length - 1;
      if (lv.height === undefined) {
        if (!last)
          ctx.addIssue({ code: z.ZodIssueCode.custom, path: [i, "height"], message: `levels[${i}] needs 'height' (where it ends and levels[${i + 1}] begins)` });
        return;
      }
      if (lv.height <= previous)
        ctx.addIssue({ code: z.ZodIssueCode.custom, path: [i, "height"], message: "level heights must increase from bottom to top" });
      previous = lv.height;
    });
  })
  .describe(
    "MultiPlane: roof levels from the pivot polygon upwards, e.g. mansard [{angle: 70, height: 2}, {angle: 25}] = 70° up to 2 m above the pivot polygon, then 25° to the ridge",
  );

const RoofFieldsBase = {
  level: z
    .number()
    .optional()
    .describe("Elevation of the pivot line / pivot polygon relative to the home story level (m), e.g. the wall top height"),
  slopeAngle: SlopeAngle.optional().describe("Roof pitch in degrees (0-90). Multi-plane: sets a single roof level with this pitch"),
  // single-plane
  pivotLine: z
    .object({ begin: Point2D, end: Point2D })
    .optional()
    .describe("SinglePlane: horizontal line the plane pivots around (usually the eaves line), at elevation 'level'"),
  risesToLeft: z
    .boolean()
    .optional()
    .describe("SinglePlane: true = the plane rises on the left side of pivotLine begin→end. Default on create: towards the polygon"),
  edges: z.array(RoofEdge).optional().describe("SinglePlane: per-edge trim / surface / edge type overrides of the roof outline"),
  // multi-plane
  levels: RoofLevels.optional(),
  eavesOverhang: z.number().optional().describe("MultiPlane: eaves overhang beyond the pivot polygon (m); the roof contour is the pivot polygon offset by this"),
  arcSegments: z.number().int().min(1).max(360).optional().describe("MultiPlane: segments per curved pivot edge (resolution by arc)"),
  circleSegments: z.number().int().min(3).max(360).optional().describe("MultiPlane: segments per full circle (resolution by circle)"),
  fitSkylightsToCurve: z.boolean().optional().describe("MultiPlane: place skylights tangentially on curved planes"),
  pivotEdges: z.array(PivotEdge).optional().describe("MultiPlane: per-plane overrides (slope, gable end, overhang) by pivot polygon edge"),
  ...ShellBaseFields,
};

const RoofPolygon = Polygon.describe(
  "SinglePlane: roof outline projected to plan (m) incl. overhangs: array of points or {points, arcs?, holes?}",
);
const PivotPolygon = Polygon.describe(
  "MultiPlane: closed pivot polygon (usually the outer face of the walls at 'level'); every edge gets a roof plane. {points, arcs?, holes?}",
);

function checkRoof(
  roof: { roofClass?: string; pivotPolygon?: unknown; polygon?: unknown; pivotLine?: unknown; [k: string]: unknown },
  ctx: z.RefinementCtx,
  creating: boolean,
): void {
  const multi = roof.roofClass === "MultiPlane" || (roof.roofClass === undefined && roof.pivotPolygon !== undefined);
  const single = roof.roofClass === "SinglePlane" || (roof.roofClass === undefined && roof.pivotPolygon === undefined);
  const singleOnly = ["pivotLine", "risesToLeft", "edges", "polygon"];
  const multiOnly = ["pivotPolygon", "levels", "eavesOverhang", "arcSegments", "circleSegments", "fitSkylightsToCurve", "pivotEdges"];
  if (creating && multi) {
    for (const k of singleOnly) {
      if (roof[k] !== undefined) ctx.addIssue({ code: z.ZodIssueCode.custom, path: [k], message: `'${k}' is only for SinglePlane roofs` });
    }
    if (roof.pivotPolygon === undefined) ctx.addIssue({ code: z.ZodIssueCode.custom, path: ["pivotPolygon"], message: "MultiPlane roofs need 'pivotPolygon'" });
  }
  if (creating && single) {
    for (const k of multiOnly) {
      if (roof[k] !== undefined) ctx.addIssue({ code: z.ZodIssueCode.custom, path: [k], message: `'${k}' is only for MultiPlane roofs` });
    }
    if (roof.polygon === undefined) ctx.addIssue({ code: z.ZodIssueCode.custom, path: ["polygon"], message: "SinglePlane roofs need 'polygon'" });
    if (roof.pivotLine === undefined) ctx.addIssue({ code: z.ZodIssueCode.custom, path: ["pivotLine"], message: "SinglePlane roofs need 'pivotLine' {begin, end}" });
  }
}

export const RoofSpec = z
  .object({
    roofClass: z
      .enum(["SinglePlane", "MultiPlane"])
      .optional()
      .describe("SinglePlane (polygon + pivotLine + slopeAngle) or MultiPlane (pivotPolygon + slopeAngle/levels). Default: MultiPlane when pivotPolygon is given"),
    polygon: RoofPolygon.optional(),
    pivotPolygon: PivotPolygon.optional(),
    ...RoofFieldsBase,
  })
  .superRefine((r, ctx) => checkRoof(r, ctx, true));

export const RoofPatch = z.object({
  guid: Guid.describe("Roof GUID"),
  polygon: RoofPolygon.optional().describe("SinglePlane: new outline (replaces the shape; per-edge data is reset)"),
  pivotPolygon: PivotPolygon.optional().describe("MultiPlane: new pivot polygon"),
  ...RoofFieldsBase,
});

// --- Shell --------------------------------------------------------------------------------------------------

const Vec3 = z.object({ x: z.number(), y: z.number(), z: z.number().optional() });

const MatrixFrame = z
  .object({ matrix: z.array(z.number()).length(12).describe("Row-major 3x4 matrix; translation at indices 3, 7, 11") })
  .strict();

const AxesFrame = z
  .object({
    origin: Vec3.optional().describe("Origin (m), default 0,0,0"),
    xAxis: Vec3.optional().describe("Local X axis direction (default 1,0,0)"),
    yAxis: Vec3.optional().describe("Local Y axis direction (default 0,1,0; orthogonalized against xAxis; zAxis = xAxis × yAxis)"),
    zAxis: Vec3.optional().describe("Ignored on input (always xAxis × yAxis); accepted so frames from get_element_details can be passed back"),
  })
  .strict()
  .refine((f) => f.origin !== undefined || f.xAxis !== undefined || f.yAxis !== undefined, {
    message: "Give origin / xAxis / yAxis (or {matrix: [12 numbers]})",
  });

export const Frame = z
  .union([MatrixFrame, AxesFrame])
  .describe("3D coordinate frame {origin, xAxis, yAxis} or {matrix: [12]}; get_element_details returns {origin, xAxis, yAxis, zAxis}");

const ShellProfile = Polyline.describe(
  "Profile polyline in the shell's profile plane (m): array of points or {points, arcs?}. Open profiles need ≥2 points; with closedProfile: true ≥3 points",
);

const ShellFieldsBase = {
  closedProfile: z.boolean().optional().describe("true = closed profile (e.g. a tube); false (default) = open profile (arch, vault, dome)"),
  flipped: z.boolean().optional().describe("Apply the thickness to the other side of the reference surface"),
  level: z.number().optional().describe("Base level offset of the shell from its home story (m)"),
  basePlane: Frame.optional().describe("Advanced: the shell's local coordinate system (read it from get_element_details of an existing shell)"),
  defaultEdgeType: z
    .enum(["Undefined", "Ridge", "Valley", "Gable", "Hip", "Eaves", "Peak", "SideWall", "EndWall", "Dome", "Hollow"])
    .optional()
    .describe("Default contour edge type"),
  // extruded
  begin: Vec3.optional().describe("Extruded: start point of the extrusion (m; z relative to the home story, default 0)"),
  extrusion: Vec3.optional().describe("Extruded: extrusion vector (direction and length, m), e.g. {x: 10, y: 0, z: 0} for a 10 m long vault along X"),
  profileDirection: Point2D.optional().describe("Extruded, advanced: 2D direction of the profile's x axis"),
  slantAngle: z.number().optional().describe("Extruded/Revolved: slant of the extrusion / revolution in degrees"),
  profilePlaneTilt: z.number().optional().describe("Extruded: tilt of the profile plane relative to the extrusion vector (degrees)"),
  beginPlaneTilt: z.number().optional().describe("Extruded: tilt of the start cap plane (degrees, 90 = perpendicular)"),
  endPlaneTilt: z.number().optional().describe("Extruded: tilt of the end cap plane (degrees)"),
  // revolved
  axisOrigin: Vec3.optional().describe("Revolved: point on the vertical revolution axis (m), default 0,0,0"),
  profileRotation: z.number().optional().describe("Revolved: direction (degrees from +X in plan) of the profile's x axis (radial direction)"),
  axisBase: Frame.optional().describe("Revolved, advanced: frame of the revolution axis relative to basePlane (z axis = axis)"),
  revolutionAngle: z.number().gt(0).max(360).optional().describe("Revolved: revolution angle in degrees (360 = full dome/tower, default 360 on create)"),
  beginAngle: z.number().optional().describe("Revolved: start angle of the revolution in degrees"),
  distortionAngle: z.number().optional().describe("Revolved: shear angle in degrees (90 = none)"),
  segmentedSurfaces: z.boolean().optional().describe("Revolved: build the surface from planar segments"),
  arcSegments: z.number().int().min(1).max(360).optional().describe("Revolved: segments of the revolution arc"),
  circleSegments: z.number().int().min(3).max(360).optional().describe("Revolved: segments per full circle"),
  // ruled
  profile2: ShellProfile.optional().describe("Ruled: second profile"),
  plane1: Frame.optional().describe("Ruled: plane (frame) of the first profile"),
  plane2: Frame.optional().describe("Ruled: plane (frame) of the second profile"),
  morphingRule: z.enum(["Paired", "Smooth"]).optional().describe("Ruled: how the two profiles are connected"),
  ...ShellBaseFields,
};

export const ShellSpec = z
  .object({
    shellClass: z
      .enum(["Extruded", "Revolved", "Ruled"])
      .optional()
      .describe("Extruded (profile swept along 'extrusion'), Revolved (profile revolved around a vertical axis) or Ruled (surface between two profiles). Default Extruded"),
    profile: ShellProfile.describe(
      "Profile polyline (m). Extruded: points in the profile plane {x across, y up}. Revolved: {x = distance from the axis, y = height}. Ruled: first profile in plane1",
    ),
    ...ShellFieldsBase,
  })
  .superRefine((s, ctx) => {
    const cls = s.shellClass ?? "Extruded";
    if (cls === "Extruded" && s.extrusion === undefined)
      ctx.addIssue({ code: z.ZodIssueCode.custom, path: ["extrusion"], message: "Extruded shells need 'extrusion' (3D vector)" });
    if (cls === "Ruled") {
      for (const k of ["profile2", "plane1", "plane2"] as const) {
        if (s[k] === undefined) ctx.addIssue({ code: z.ZodIssueCode.custom, path: [k], message: `Ruled shells need '${k}'` });
      }
    }
  });

export const ShellPatch = z.object({
  guid: Guid.describe("Shell GUID"),
  profile: ShellProfile.optional().describe("New profile (replaces the profile; clipping/hole contours are kept)"),
  ...ShellFieldsBase,
});

// --- Mesh ---------------------------------------------------------------------------------------------------

const MeshPolygon = Polygon.describe(
  "Mesh outline {points: [{x, y, z}], arcs?, holes?}: x/y in plan (m), z = height of the point above the mesh 'level' (omitted z = 0). Do not repeat the first point",
);

export const LevelLine = z
  .object({
    points: z.array(z.object({ x: z.number(), y: z.number(), z: z.number() })).min(2).describe("Points of the level line; z = height above the mesh level (m)"),
  })
  .describe("Inner level line (ridge / contour line) of the mesh");

const MeshFieldsBase = {
  level: z.number().optional().describe("Elevation of the mesh base plane relative to the home story (m); point z values are relative to it"),
  skirt: z
    .enum(["SolidBody", "SkirtWithoutBottom", "SurfaceOnly"])
    .optional()
    .describe("Body type: SolidBody (skirt + bottom), SkirtWithoutBottom, SurfaceOnly"),
  skirtLevel: z.number().optional().describe("Distance of the mesh bottom (skirt) from the base plane (m)"),
  ridges: z.enum(["AllSharp", "AllSmooth", "UserDefined"]).optional().describe("Ridge display: all sharp, all smooth, or only user-defined ridges sharp"),
  showAllLines: z.boolean().optional().describe("Show all (also secondary) mesh lines on the floor plan"),
  levelLines: z
    .array(LevelLine)
    .optional()
    .describe("Inner level lines (replace existing ones); [] removes all level lines"),
  buildingMaterial: AttrRef.optional().describe("Building material name/index (meshes have no composites)"),
  ...Surfaces,
  ...FloorPlan,
  levelLinePen: PenIndex.optional().describe("Pen of the level lines"),
  ...CommonElementFields,
};

export const MeshSpec = z.object({ polygon: MeshPolygon, ...MeshFieldsBase });
export const MeshPatch = z.object({
  guid: Guid.describe("Mesh GUID"),
  polygon: MeshPolygon.optional().describe("New outline with heights (replaces the whole polygon incl. holes)"),
  ...MeshFieldsBase,
});
