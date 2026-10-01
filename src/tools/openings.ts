/**
 * Opening tools: windows, doors, skylights and Opening-tool openings (extrusion cuts).
 * The add-on adapters live in addon/Src/Commands/Openings*.cpp; field names match 1:1.
 * Created elements are sent through the generic CreateElements command ({type: 'Window'|..., ...fields}).
 */

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import { CREATES, defineTool, MODIFIES, READ_ONLY, type ToolContext } from "./define.js";
import { createElements } from "./elements.js";
import { AttrRef, CommonElementFields, ElementRef, ElementRefs, GdlParams, Guid, guidOf, LibPartRef, PenIndex, Point2D } from "./schemas.js";

// --- Shared field groups ----------------------------------------------------------------

/** layer / renovationStatus / elementId (openings always live on their host's story, so no storyIndex). */
const { storyIndex: _storyIndex, ...HostStoryCommonFields } = CommonElementFields;

const Point3DOptZ = z
  .object({ x: z.number(), y: z.number(), z: z.number().optional() })
  .describe("Plan point in meters; optional z = elevation above the home story");

const Vector3D = z.object({ x: z.number(), y: z.number(), z: z.number() }).describe("3D direction vector (any length > 0)");

const DisplayOption = z
  .enum(["Projected", "ProjectedWithOverhead", "CutOnly", "OutlinesOnly", "OverheadAll", "Symbolic"])
  .describe("Floor plan display of the opening");

/** Attributes common to windows, doors and skylights (API_OpeningBaseType). */
const OpeningBaseAttributeFields = {
  surface: AttrRef.optional().describe("Surface (material) used for the whole object; also switches useObjectSurfaces off (list names with get_attributes type Surface)"),
  useObjectSurfaces: z.boolean().optional().describe("true = use the surfaces set in the library part's own GDL parameters"),
  pen: PenIndex.optional().describe("Contour pen; also switches useObjectPens off"),
  useObjectPens: z.boolean().optional().describe("true = use the pens set in the library part's parameters"),
  lineType: AttrRef.optional().describe("Line type; also switches useObjectLineTypes off"),
  useObjectLineTypes: z.boolean().optional().describe("true = use the line types set in the library part's parameters"),
  cutFill: AttrRef.optional().describe("Fill of cut (section) parts; also switches useObjectSectionAttributes off"),
  cutFillPen: PenIndex.optional().describe("Pen of the cut fill"),
  cutFillBackgroundPen: z.number().int().min(0).max(255).optional().describe("Background pen of the cut fill (0 = transparent)"),
  cutContourPen: PenIndex.optional().describe("Pen of the cut contour"),
  useObjectSectionAttributes: z.boolean().optional().describe("true = use the section attributes set in the library part"),
  cutLineType: AttrRef.optional().describe("Line type of cut lines"),
  overheadLinePen: PenIndex.optional().describe("Pen of overhead lines (floor plan display 'OverheadAll')"),
  overheadLineType: AttrRef.optional().describe("Line type of overhead lines"),
  uncutLinePen: PenIndex.optional().describe("Pen of uncut lines"),
  uncutLineType: AttrRef.optional().describe("Line type of uncut lines (display 'OutlinesOnly')"),
  displayOption: DisplayOption.optional(),
};

const WallVerticalAnchor = z
  .enum(["SillToWallBottom", "SillToStory", "HeaderToWallBottom", "HeaderToStory", "SillToWallTop", "HeaderToWallTop"])
  .describe("What stays fixed vertically when the wall / story changes: the sill or the header, relative to the wall bottom/top or a story");

const SkylightVerticalAnchor = z
  .enum(["ToRoofPivot", "ToStory", "ToShellBase"])
  .describe("Vertical reference of the skylight anchor: the roof pivot line, a story, or the shell base");

function libraryPartField(kind: "Window" | "Door" | "Skylight") {
  return LibPartRef.optional().describe(
    `${kind} library part: exact LOCALIZED name (find it with search_library_parts, type ${kind}), index, or {guid}. ` +
      `Default: the ${kind} tool's current part (with the tool's current parameters). When given, the part starts from its own default GDL parameters ` +
      "(run through its parameter script together with gdlParams, width and height)",
  );
}

function windowDoorFields(kind: "Window" | "Door") {
  const noun = kind.toLowerCase();
  return {
    wall: ElementRef.describe(`GUID of the host wall (straight, curved or trapezoid; not polygonal). The ${noun} is always on the wall's story`),
    position: z
      .number()
      .min(0)
      .optional()
      .describe(
        `Distance in m along the wall reference line from the wall's BEGIN point to the ${noun}'s CENTRE (see positionReference). ` +
          "Must lie within 0..wall length (get_element_details on the wall returns begin/end/length). Give position OR point",
      ),
    point: Point2D.optional().describe(`Plan point {x,y}; the ${noun} is centred on its perpendicular projection onto the wall reference line. Give position OR point`),
    positionReference: z
      .enum(["Center", "Begin", "End"])
      .optional()
      .describe(`What position/point refers to: the ${noun} centre (default), or its edge nearer the wall's begin ('Begin') / end ('End'), e.g. position 0.5 + 'Begin' = 0.5 m gap from the wall start`),
    sillHeight: z
      .number()
      .optional()
      .describe(`Sill (parapet) height in m: bottom of the ${noun} above the wall bottom (default reference; see verticalAnchor). Doors usually 0`),
    width: z.number().positive().optional().describe(`Nominal ${noun} width in m (default: tool / library part default)`),
    height: z.number().positive().optional().describe(`Nominal ${noun} height in m (default: tool / library part default)`),
    libraryPart: libraryPartField(kind),
    emptyHole: z.boolean().optional().describe("Create only an empty rectangular hole without a library part (create only; not combinable with libraryPart/gdlParams)"),
    gdlParams: GdlParams.optional().describe(
      "GDL parameter values {name: value} (lengths m, angles deg), applied through the part's parameter script. Parameter names are not localized; list them with get_gdl_parameters",
    ),
    flipped: z.boolean().optional().describe(`Flip the ${noun} to the other side of the wall (opening direction / reveal side)`),
    referenceSideFlipped: z.boolean().optional().describe("Reference (reveal) side flag; follows 'flipped' unless given explicitly"),
    mirrored: z.boolean().optional().describe(`Mirror left/right (hinge / handle side of the ${noun})`),
    anchor: z.enum(["Begin", "Center", "End"]).optional().describe(`Which point stays fixed when the width changes: the ${noun} edge towards the wall begin, the centre, or the edge towards the wall end`),
    verticalAnchor: WallVerticalAnchor.optional(),
    verticalAnchorStory: z
      .union([z.number().int(), z.string()])
      .optional()
      .describe("Story index or name for verticalAnchor 'SillToStory' / 'HeaderToStory' (default: the wall's story)"),
    reveal: z.boolean().optional().describe("Enable the reveal (opening set back into the wall)"),
    revealDepth: z.number().optional().describe("Reveal depth in m: inset of the frame from the reference side of the wall (or of its core, see revealDepthLocation)"),
    revealDepthLocation: z.enum(["WallSide", "Core"]).optional().describe("revealDepth is measured from the wall side or from the core skin of a composite wall"),
    jambDepthHead: z.number().optional().describe("Reveal inset on top (m)"),
    jambDepthSill: z.number().optional().describe("Reveal inset at the bottom (m)"),
    jambDepthLeft: z.number().optional().describe("Reveal inset on the left (m)"),
    jambDepthRight: z.number().optional().describe("Reveal inset on the right (m)"),
    orientationInSlantedWall: z.enum(["FollowWall", "Vertical"]).optional().describe("In slanted walls: follow the wall plane or stay vertical"),
    inheritWallCut: z.boolean().optional().describe("Inherit the wall's gables / trims (cut with the wall)"),
    subFloorThickness: z.number().optional().describe("Sub-floor thickness (parapet correction) in m"),
    ...OpeningBaseAttributeFields,
    ...HostStoryCommonFields,
  };
}

export const WindowSpec = z.object(windowDoorFields("Window"));
export const DoorSpec = z.object(windowDoorFields("Door"));

export const SkylightFields = {
  owner: ElementRef.describe("GUID of the host Roof or Shell. The skylight is always on the host's story"),
  point: Point2D.describe("Plan position {x,y} of the skylight anchor point; must lie on the roof/shell in plan"),
  anchorLevel: z
    .number()
    .optional()
    .describe(
      "Absolute Z (from Project Zero) of the anchor point. Normally omit it: for single-plane roofs it is computed on the roof's pivot plane at 'point'; " +
        "otherwise the Skylight tool default is kept (check the result with get_element_details)",
    ),
  anchor: z
    .enum(["BottomCenter", "BottomLeft", "BottomRight", "TopCenter", "TopLeft", "TopRight"])
    .optional()
    .describe("Which point of the skylight is placed at 'point' (bottom = downslope edge)"),
  fixMode: z.enum(["Horizontal", "Vertical"]).optional().describe("What is kept when the roof changes: the plan (horizontal) position or the vertical position"),
  azimuthAngle: z.number().optional().describe("Rotation in the horizontal plane in degrees, measured clockwise"),
  pivotVertexId: z.number().int().min(0).optional().describe("Multi-plane roofs only: pivot polygon vertex ID the skylight belongs to (advanced; normally omit)"),
  width: z.number().positive().optional().describe("Skylight width in m"),
  height: z.number().positive().optional().describe("Skylight length (in the slope direction) in m"),
  libraryPart: libraryPartField("Skylight"),
  emptyHole: z.boolean().optional().describe("Create only an empty hole without a library part (create only)"),
  gdlParams: GdlParams.optional().describe("GDL parameter values {name: value} (lengths m, angles deg); list names with get_gdl_parameters"),
  flipped: z.boolean().optional().describe("Flip the skylight (mirror about its x axis)"),
  referenceSideFlipped: z.boolean().optional().describe("Reference side flag; follows 'flipped' unless given"),
  mirrored: z.boolean().optional().describe("Mirror left/right"),
  verticalAnchor: SkylightVerticalAnchor.optional(),
  verticalAnchorStory: z.union([z.number().int(), z.string()]).optional().describe("Story index or name for verticalAnchor 'ToStory' (default: the host's story)"),
  subFloorThickness: z.number().optional().describe("Sub-floor thickness in m"),
  ...OpeningBaseAttributeFields,
  ...HostStoryCommonFields,
};
export const SkylightSpec = z.object(SkylightFields);

const OpeningAnchor = z
  .enum(["TopLeft", "TopCenter", "TopRight", "CenterLeft", "Center", "CenterRight", "BottomLeft", "BottomCenter", "BottomRight"])
  .describe("Which point of the base shape sits at the anchor point (default 'Center')");

export const OpeningFields = {
  owner: ElementRef.describe(
    "GUID of the element to cut: Wall, Slab, Roof, Shell, Beam, Column or Mesh. The opening is always on the host's story and follows the host",
  ),
  position: z
    .number()
    .min(0)
    .optional()
    .describe("Straight wall hosts only: distance in m along the wall reference line from the wall's begin point to the ANCHOR point"),
  point: Point3DOptZ.optional().describe(
    "Anchor point {x, y, z?} in plan (projected onto the wall reference line for wall hosts); z = anchor elevation above the home story. Give position OR point",
  ),
  bottomElevation: z
    .number()
    .optional()
    .describe("Horizontal / aligned openings (e.g. in walls): elevation in m of the opening's bottom edge above the home story level"),
  anchorAltitude: z.number().optional().describe("Alternative to bottomElevation: elevation in m of the anchor point above the home story"),
  shape: z.enum(["Rectangular", "Circular"]).optional().describe("Base shape (default: tool setting, usually Rectangular)"),
  width: z.number().positive().optional().describe("Width in m (circular: diameter). Required on create"),
  height: z.number().positive().optional().describe("Height in m (circular: defaults to width). Required on create for rectangular openings"),
  linkedSize: z.boolean().optional().describe("Link width and height (keeps them equal)"),
  anchor: OpeningAnchor.optional(),
  constraint: z
    .enum(["Horizontal", "Vertical", "Aligned", "Free"])
    .optional()
    .describe(
      "Extrusion orientation: 'Horizontal' (default for walls, beams, columns: perpendicular to the wall), 'Vertical' (default for slabs, roofs, shells, meshes: shafts), " +
        "'Aligned' (perpendicular to the host surface) or 'Free' (use extrusionDirection)",
    ),
  extrusionDirection: Vector3D.optional().describe("Explicit extrusion direction (default derived from constraint and host)"),
  xAxis: Vector3D.optional().describe("Direction of the base shape's width axis (made perpendicular to the extrusion direction)"),
  rotation: z.number().optional().describe("Rotation of the base shape in degrees about the extrusion direction (e.g. rotate a rectangular slab opening)"),
  limit: z
    .enum(["Infinite", "Finite", "HalfInfinite"])
    .optional()
    .describe("Extrusion extent: through everything along the axis, a finite body (startOffset + depth), or from startOffset to infinity"),
  startOffset: z.number().optional().describe("Finite / HalfInfinite: start of the body from the anchor plane along the extrusion direction (m)"),
  depth: z.number().positive().optional().describe("Finite: length of the extrusion body (m)"),
  floorPlanDisplay: z.enum(["Symbolic", "SymbolicCut", "SymbolicOverhead"]).optional().describe("Floor plan display mode"),
  connectionMode: z.enum(["Connected", "Disconnected"]).optional().describe("Floor plan connection mode"),
  outlinesStyle: z.enum(["HideBorder", "ShowUncutBorder", "ShowOverheadBorder"]).optional().describe("Floor plan outline style"),
  showReferenceAxis: z.boolean().optional().describe("Show the opening's reference axis on the floor plan"),
  ...HostStoryCommonFields,
};
export const OpeningSpec = z.object(OpeningFields);

// --- Modify patch schema (union of all opening fields) ------------------------------------

const ModifyOpeningPatch = z
  .object({
    guid: Guid.describe("GUID of the window, door, skylight or opening to change"),
    position: z.number().min(0).optional().describe("Windows/doors: new centre distance from the wall begin (m). Wall openings: anchor distance"),
    point: Point3DOptZ.optional().describe("Windows/doors: plan point projected onto the wall. Skylights: new anchor position {x,y}. Openings: new anchor point {x,y,z?}"),
    positionReference: z.enum(["Center", "Begin", "End"]).optional().describe("Windows/doors: what position/point refers to (default Center)"),
    sillHeight: z.number().optional().describe("Windows/doors: sill height (m)"),
    width: z.number().positive().optional().describe("Width in m"),
    height: z.number().positive().optional().describe("Height in m"),
    libraryPart: LibPartRef.optional().describe("Windows/doors/skylights: swap the library part (current size kept unless width/height given; GDL params reset to the new part's defaults)"),
    gdlParams: GdlParams.optional().describe("Windows/doors/skylights: GDL parameter values {name: value} (through the parameter script)"),
    flipped: z.boolean().optional().describe("Windows/doors/skylights: flip to the other side"),
    referenceSideFlipped: z.boolean().optional(),
    mirrored: z.boolean().optional().describe("Windows/doors/skylights: mirror left/right"),
    anchor: z
      .string()
      .optional()
      .describe("Windows/doors: Begin|Center|End. Skylights: BottomCenter|BottomLeft|BottomRight|TopCenter|TopLeft|TopRight. Openings: TopLeft..Center..BottomRight"),
    verticalAnchor: z
      .enum([...WallVerticalAnchor.options, ...SkylightVerticalAnchor.options])
      .optional()
      .describe("Windows/doors: SillToWallBottom|SillToStory|HeaderToWallBottom|HeaderToStory|SillToWallTop|HeaderToWallTop. Skylights: ToRoofPivot|ToStory|ToShellBase"),
    verticalAnchorStory: z.union([z.number().int(), z.string()]).optional().describe("Story for story-based verticalAnchor values"),
    reveal: z.boolean().optional(),
    revealDepth: z.number().optional(),
    revealDepthLocation: z.enum(["WallSide", "Core"]).optional(),
    jambDepthHead: z.number().optional(),
    jambDepthSill: z.number().optional(),
    jambDepthLeft: z.number().optional(),
    jambDepthRight: z.number().optional(),
    orientationInSlantedWall: z.enum(["FollowWall", "Vertical"]).optional(),
    inheritWallCut: z.boolean().optional(),
    subFloorThickness: z.number().optional(),
    anchorLevel: z.number().optional().describe("Skylights: absolute Z of the anchor point"),
    fixMode: z.enum(["Horizontal", "Vertical"]).optional().describe("Skylights"),
    azimuthAngle: z.number().optional().describe("Skylights: rotation in degrees (clockwise)"),
    pivotVertexId: z.number().int().min(0).optional().describe("Skylights on multi-plane roofs"),
    bottomElevation: z.number().optional().describe("Openings: bottom edge elevation above the home story (m)"),
    anchorAltitude: z.number().optional().describe("Openings: anchor point elevation above the home story (m)"),
    shape: z.enum(["Rectangular", "Circular"]).optional().describe("Openings"),
    linkedSize: z.boolean().optional().describe("Openings"),
    constraint: z.enum(["Horizontal", "Vertical", "Aligned", "Free"]).optional().describe("Openings"),
    extrusionDirection: Vector3D.optional().describe("Openings"),
    xAxis: Vector3D.optional().describe("Openings"),
    rotation: z.number().optional().describe("Openings: rotation about the extrusion direction (deg)"),
    limit: z.enum(["Infinite", "Finite", "HalfInfinite"]).optional().describe("Openings"),
    startOffset: z.number().optional().describe("Openings"),
    depth: z.number().positive().optional().describe("Openings"),
    floorPlanDisplay: z.enum(["Symbolic", "SymbolicCut", "SymbolicOverhead"]).optional().describe("Openings"),
    connectionMode: z.enum(["Connected", "Disconnected"]).optional().describe("Openings"),
    outlinesStyle: z.enum(["HideBorder", "ShowUncutBorder", "ShowOverheadBorder"]).optional().describe("Openings"),
    showReferenceAxis: z.boolean().optional().describe("Openings"),
    ...OpeningBaseAttributeFields,
    ...HostStoryCommonFields,
  })
  .describe("Patch: guid + only the fields to change");

// --- Validation helpers ------------------------------------------------------------------

function requirePlacement(items: Array<{ position?: number; point?: unknown }>, listName: string, what: string): void {
  items.forEach((item, i) => {
    if (item.position === undefined && item.point === undefined) {
      throw new Error(`${listName}[${i}]: give 'position' (m along the wall from its begin point) or 'point' ({x,y}) to place the ${what}.`);
    }
    if (item.position !== undefined && item.point !== undefined) {
      throw new Error(`${listName}[${i}]: give either 'position' or 'point', not both.`);
    }
  });
}

function withType<T extends object>(type: string, items: T[]): Record<string, unknown>[] {
  return items.map((item) => ({ type, ...item }));
}

// --- Tools -------------------------------------------------------------------------------

export function registerOpeningTools(server: McpServer, ctx: ToolContext): void {
  defineTool(server, ctx, {
    name: "create_windows",
    title: "Create windows",
    description:
      "Places windows into walls (one undo step). Each window needs its host 'wall' GUID and either 'position' (m from the wall's begin point to the window " +
      "centre along the reference line) or 'point' ({x,y} projected onto the wall). sillHeight = bottom of the window above the wall bottom. " +
      "Unspecified settings (library part, size, sill, reveal, surfaces) come from the Window tool defaults. Library part names are LOCALIZED: " +
      "find them with search_library_parts (type Window). Polygonal walls cannot host windows. Returns [{guid, type} | {error}] in input order; " +
      "inspect results with get_element_details (position, location, sill/header height, library part, key GDL params).",
    input: { windows: z.array(WindowSpec).min(1).max(500) },
    annotations: CREATES,
    handler: async ({ windows }, c) => {
      requirePlacement(windows, "windows", "window");
      return createElements(c, withType("Window", windows), "Create windows (Claude)");
    },
  });

  defineTool(server, ctx, {
    name: "create_doors",
    title: "Create doors",
    description:
      "Places doors into walls (one undo step). Each door needs its host 'wall' GUID and either 'position' (m from the wall's begin point to the door " +
      "centre along the reference line) or 'point' ({x,y} projected onto the wall). Use flipped (opening side) and mirrored (hinge side) to orient it; " +
      "sillHeight is usually 0. Unspecified settings come from the Door tool defaults. Library part names are LOCALIZED: find them with " +
      "search_library_parts (type Door). Returns [{guid, type} | {error}] in input order.",
    input: { doors: z.array(DoorSpec).min(1).max(500) },
    annotations: CREATES,
    handler: async ({ doors }, c) => {
      requirePlacement(doors, "doors", "door");
      return createElements(c, withType("Door", doors), "Create doors (Claude)");
    },
  });

  defineTool(server, ctx, {
    name: "create_skylights",
    title: "Create skylights",
    description:
      "Places skylights into roofs or shells (one undo step). Each skylight needs the host 'owner' (Roof or Shell GUID) and 'point' ({x,y}: plan position " +
      "of its anchor on the roof). Size, library part and parameters default to the Skylight tool settings; library part names are LOCALIZED " +
      "(search_library_parts, type Skylight). Returns [{guid, type} | {error}] in input order.",
    input: { skylights: z.array(SkylightSpec).min(1).max(500) },
    annotations: CREATES,
    handler: async ({ skylights }, c) => createElements(c, withType("Skylight", skylights), "Create skylights (Claude)"),
  });

  defineTool(server, ctx, {
    name: "create_openings",
    title: "Create openings (Opening tool)",
    description:
      "Creates Opening-tool openings: rectangular or circular extrusion bodies that cut holes through their host (wall, slab, roof, shell, beam, column, mesh) — " +
      "e.g. shafts through slabs, service holes in walls or beams. Each needs 'owner', 'width' (+ 'height' unless circular) and 'point' ({x,y,z?} plan anchor) " +
      "or, for walls, 'position' along the wall. Wall openings extrude horizontally through the wall: set their height with bottomElevation (bottom edge above " +
      "the home story). Slab/roof openings extrude vertically by default (limit 'Infinite' cuts through the whole host). Custom polygonal shapes are not " +
      "supported by the Archicad 26 API. Returns [{guid, type} | {error}] in input order.",
    input: { openings: z.array(OpeningSpec).min(1).max(500) },
    annotations: CREATES,
    handler: async ({ openings }, c) => {
      openings.forEach((o, i) => {
        if (o.position === undefined && o.point === undefined) {
          throw new Error(`openings[${i}]: give 'point' ({x, y, z?}) or, for a wall host, 'position' (m along the wall).`);
        }
        if (o.width === undefined || (o.height === undefined && o.shape !== "Circular")) {
          throw new Error(`openings[${i}]: 'width' and 'height' are required (a Circular opening needs only 'width' = diameter).`);
        }
      });
      return createElements(c, withType("Opening", openings), "Create openings (Claude)");
    },
  });

  defineTool(server, ctx, {
    name: "modify_openings",
    title: "Modify windows, doors, skylights, openings",
    description:
      "Changes existing windows, doors, skylights and Opening-tool openings in one undo step. Each item is {guid, ...fields to change} with the same " +
      "field names as create_windows / create_doors / create_skylights / create_openings (e.g. move a window: position; resize: width/height; " +
      "swap the library part: libraryPart; GDL: gdlParams; flip: flipped / mirrored). Openings keep their anchorAltitude when anchor/height change — " +
      "pass bottomElevation to keep the bottom edge instead. The host (wall / roof / owner) cannot be changed — delete and recreate instead. " +
      "Returns [{guid} | {error}].",
    input: { openings: z.array(ModifyOpeningPatch).min(1).max(1000) },
    annotations: MODIFIES,
    handler: async ({ openings }, { ac }) => ac.addon("ModifyElements", { elements: openings, undoName: "Modify openings (Claude)" }),
  });

  defineTool(server, ctx, {
    name: "get_host_openings",
    title: "Openings of host elements",
    description:
      "Lists the windows, doors, skylights and Opening-tool openings placed in the given host elements (walls, roofs, shells, slabs, beams, ...). " +
      "details=false (default) returns GUIDs only; details=true returns the full element details of every opening (position, size, library part...).",
    input: {
      hosts: ElementRefs.max(500).describe("Host element GUIDs (walls, roofs, shells, slabs, beams, columns ...)"),
      details: z.boolean().optional().describe("Include full element details of each opening (default false)"),
    },
    annotations: READ_ONLY,
    handler: async ({ hosts, details }, { ac }) =>
      ac.addon("GetHostOpenings", { hosts: hosts.map(guidOf), ...(details !== undefined ? { details } : {}) }),
  });
}
