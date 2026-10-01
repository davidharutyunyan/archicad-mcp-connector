/**
 * Element query tools: finding, counting and analysing elements.
 * Add-on commands live in addon/Src/Commands/ElementQuery*.cpp; field names match 1:1.
 *
 *   find_elements            FindElements
 *   get_element_counts       GetElementCounts
 *   get_element_quantities   GetElementQuantities
 *   get_connected_elements   GetConnectedElements
 *   get_element_relations    GetElementRelations
 *   get_subelements          GetSubelements
 *   get_selection            GetSelection
 *   set_selection            SetSelection
 *   get_element_2d_geometry  GetElement2DGeometry
 *   get_element_3d_geometry  GetElement3DGeometry
 */

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import { defineTool, READ_ONLY, type ToolContext } from "./define.js";
import { AttrRef, ElementRef, ElementRefs, Guid, guidOf, RenovationStatus, StoryRef } from "./schemas.js";

/** Every element type name understood by the add-on (same spelling as the official JSON API). */
export const ELEMENT_TYPES = [
  "Wall", "Column", "Beam", "Window", "Door", "Object", "Lamp", "Slab", "Roof", "Mesh",
  "Dimension", "RadialDimension", "LevelDimension", "AngleDimension", "Text", "Label", "Zone",
  "Hatch", "Line", "PolyLine", "Arc", "Circle", "Spline", "Hotspot", "CutPlane", "Camera", "CamSet",
  "Group", "SectElem", "Drawing", "Picture", "Detail", "Elevation", "InteriorElevation", "Worksheet", "Hotlink",
  "CurtainWall", "CurtainWallSegment", "CurtainWallFrame", "CurtainWallPanel", "CurtainWallJunction", "CurtainWallAccessory",
  "Shell", "Skylight", "Morph", "ChangeMarker", "Stair", "Riser", "Tread", "StairStructure",
  "Railing", "RailingToprail", "RailingHandrail", "RailingRail", "RailingPost", "RailingInnerPost", "RailingBaluster",
  "RailingPanel", "RailingSegment", "RailingNode", "RailingBalusterSet", "RailingPattern", "RailingToprailEnd",
  "RailingHandrailEnd", "RailingRailEnd", "RailingToprailConnection", "RailingHandrailConnection", "RailingRailConnection",
  "RailingEndFinish", "BeamSegment", "ColumnSegment", "Opening",
] as const;

/** Sub-element types (parts of curtain walls, stairs, railings, beams, columns). */
export const SUBELEMENT_TYPES = [
  "CurtainWallSegment", "CurtainWallFrame", "CurtainWallPanel", "CurtainWallJunction", "CurtainWallAccessory",
  "Riser", "Tread", "StairStructure",
  "RailingToprail", "RailingHandrail", "RailingRail", "RailingPost", "RailingInnerPost", "RailingBaluster", "RailingPanel",
  "RailingSegment", "RailingNode", "RailingBalusterSet", "RailingPattern", "RailingToprailEnd", "RailingHandrailEnd",
  "RailingRailEnd", "RailingToprailConnection", "RailingHandrailConnection", "RailingRailConnection", "RailingEndFinish",
  "BeamSegment", "ColumnSegment",
] as const;

export const ElementType = z.enum(ELEMENT_TYPES).describe("Element type name (case-sensitive), e.g. 'Wall', 'Slab', 'Door', 'Object'");

export const VISIBILITY_FILTERS = [
  "OnVisibleLayer", "Editable", "OnActiveStory", "In3D", "InMyWorkspace", "Independent", "OnActiveLayout",
  "InCroppedView", "HasAccessRight", "VisibleByRenovation", "Overridden", "InStructureDisplay", "FromFloorPlan",
] as const;

const VisibilityFilter = z.enum(VISIBILITY_FILTERS).describe(
  "Archicad element filter: OnVisibleLayer (layer shown), Editable (not locked, layer unlocked, reserved in Teamwork), " +
    "OnActiveStory (home story = current story), In3D (shown in the 3D window with the current 3D filter), " +
    "InMyWorkspace (Teamwork: in your workspace), Independent (not part of another element), OnActiveLayout, " +
    "InCroppedView (inside the crop of the active view), HasAccessRight, VisibleByRenovation (shown by the current " +
    "renovation filter), Overridden (affected by a graphic override), InStructureDisplay (shown by the current partial " +
    "structure display), FromFloorPlan (list floor plan elements even when another window is active)",
);

const Region = z
  .object({
    xMin: z.number().optional().describe("West limit, m"),
    yMin: z.number().optional().describe("South limit, m"),
    xMax: z.number().optional().describe("East limit, m"),
    yMax: z.number().optional().describe("North limit, m"),
    zMin: z.number().optional().describe("Lower limit, m, ABSOLUTE (relative to project zero, not to the story)"),
    zMax: z.number().optional().describe("Upper limit, m, ABSOLUTE"),
    mode: z.enum(["Intersects", "Inside"]).optional().describe("Intersects (default): element bounding box touches the region; Inside: box lies completely inside"),
  })
  .describe(
    "Spatial filter on the element's 3D bounding box (2D elements have a flat box at z=0). Give any subset of the limits; " +
      "e.g. {xMin:0, yMin:0, xMax:10, yMax:8} for a plan rectangle",
  );

/** Filters shared by find_elements and get_element_counts (same names as the C++ command). */
export const ElementFilterFields = {
  types: z.array(ElementType).optional().describe(
    "Only these element types. Default: every main type (sub-elements such as curtain wall panels, stair treads or railing posts are " +
      "only included when listed here or with includeSubelements=true)",
  ),
  excludeTypes: z.array(ElementType).optional().describe("Skip these element types"),
  includeSubelements: z.boolean().optional().describe("Also list sub-elements (curtain wall/stair/railing parts, beam/column segments) when 'types' is not given, and the hidden GDL part Objects/Lamps owned by curtain walls, railings and stairs (skipped otherwise, counted in stats.ownedPartObjectsSkipped). Default false"),
  storyIndex: StoryRef.optional().describe("Only elements whose HOME story is this one (index or localized name, see get_stories)"),
  stories: z.array(StoryRef).optional().describe("Only elements whose home story is one of these"),
  layers: z.array(AttrRef).optional().describe("Only elements on these layers (layer names are localized, e.g. Russian — take them from get_attributes or earlier results)"),
  renovationStatus: z.array(RenovationStatus).optional().describe("Only elements with one of these renovation statuses"),
  filters: z.array(VisibilityFilter).optional().describe("Archicad visibility/editability filters; an element must pass ALL of them"),
  selectedOnly: z.boolean().optional().describe("Only elements in the current selection"),
  withinElements: z.array(ElementRef).max(20000).optional().describe("Restrict the search to these elements (e.g. to refine an earlier result)"),
  elementId: z.string().optional().describe(
    "Element ID (the ID shown in the Info Box) wildcard pattern, case-insensitive: '*' = any characters, '?' = one character; " +
      "without wildcards the ID must match exactly. Examples: 'W-*', '*01', 'D-0??'",
  ),
  libraryPart: z.string().optional().describe(
    "Library part name contains this text (case-insensitive). Applies to objects, lamps, windows, doors, skylights and zone stamps; " +
      "other types are excluded. Names are localized",
  ),
  groupGuid: Guid.optional().describe("Only members of this group (nested groups included); groupGuid comes from get_element_details"),
  grouped: z.boolean().optional().describe("true = only grouped elements, false = only ungrouped ones"),
  hotlinkGuid: Guid.optional().describe("Only elements belonging to this hotlink instance"),
  inHotlink: z.boolean().optional().describe("true = only elements that come from a hotlinked module, false = only own elements"),
  locked: z.boolean().optional().describe("true = only locked elements, false = only unlocked ones"),
  region: Region.optional(),
};

export function registerElementQueryTools(server: McpServer, ctx: ToolContext): void {
  defineTool(server, ctx, {
    name: "find_elements",
    title: "Find elements",
    description:
      "Finds elements in the open project with combinable filters (type, story, layer, renovation status, visibility/editability, " +
      "selection, Element ID pattern, library part name, group, hotlink, lock state, spatial region) and returns a paginated list " +
      "[{guid, type, storyIndex, layer: {index, name}, elementId, boundingBox?, libraryPart?}] plus total/hasMore. All filters are " +
      "optional and AND-combined; with no filter every main element is listed. Use the GUIDs with get_element_details, " +
      "get_element_quantities, modify_elements, set_selection etc. For counts only use get_element_counts. Coordinates in meters.",
    input: {
      ...ElementFilterFields,
      includeBoundingBox: z.boolean().optional().describe("Add boundingBox {xMin,yMin,zMin,xMax,yMax,zMax} (m, z absolute) to each element. Default false"),
      includeLibraryPart: z.boolean().optional().describe("Add the library part name of objects/lamps/doors/windows/skylights/zones. Default false"),
      includeElementId: z.boolean().optional().describe("Include the Element ID string. Default true"),
      offset: z.number().int().min(0).optional().describe("Skip this many matches (pagination). Default 0"),
      limit: z.number().int().min(1).max(5000).optional().describe("Return at most this many matches. Default 500"),
    },
    annotations: READ_ONLY,
    handler: async (args, { ac }) => ac.addon("FindElements", normalizeFilterArgs(args)),
  });

  defineTool(server, ctx, {
    name: "get_element_counts",
    title: "Count elements",
    description:
      "Counts elements per type — optionally also per story, per layer and per renovation status — with the same filters as " +
      "find_elements. Output: {total, byType: {Wall: 12, ...}, byStory?: [{storyIndex, storyName, total, byType}], " +
      "byLayer?: [{layer, total, byType}] (largest first), byRenovationStatus?}. Good first call to get an overview of a project.",
    input: {
      ...ElementFilterFields,
      groupBy: z.array(z.enum(["story", "layer", "renovationStatus"])).optional().describe("Extra breakdowns; per-type counts are always returned"),
    },
    annotations: READ_ONLY,
    handler: async (args, { ac }) => ac.addon("GetElementCounts", normalizeFilterArgs(args)),
  });

  defineTool(server, ctx, {
    name: "get_element_quantities",
    title: "Get element quantities",
    description:
      "Calculated quantities of elements, as Archicad lists them: every field of the element type's quantity record with " +
      "descriptive names. Units: lengths m, areas m², volumes m³, angles degrees, counts integers. Examples — Wall: volume, " +
      "grossVolume, surfaceReferenceSide, surfaceOppositeSide, length, area (plan), minHeight/maxHeight, windowsSurface, doorsSurface; " +
      "Slab: volume, topSurface, bottomSurface, edgeSurface, perimeter, holesSurface; Zone: area, netArea, calculatedArea, volume, " +
      "perimeter, wallsSurface; Column/Beam: core/veneer volumes & surfaces; Window/Door: surface, width/height per side, sill/head " +
      "heights; Roof/Shell/Mesh/Morph/Object/CurtainWall/Stair/Railing and their parts are supported too. " +
      "'Conditional' values follow the project's calculation rules. Also returns composite skins per building material and per-type " +
      "totals (sums of additive fields) + building material volume totals. Get GUIDs from find_elements first.",
    input: {
      elements: ElementRefs.max(500).describe("Elements to measure (max 500 per call)"),
      includeComposites: z.boolean().optional().describe("List skin volumes / projected areas per building material (walls, slabs, roofs, shells...). Default true"),
      includeParts: z.boolean().optional().describe("Also return per-part quantities (e.g. each roof plane of a multi-plane roof, each story of a morph). Default false"),
      includeTotals: z.boolean().optional().describe("Add 'totals' per element type (count + sums of lengths, areas, volumes, counts) and 'buildingMaterialTotals'. Default true"),
      minOpeningSize: z.number().min(0).optional().describe("m²: openings smaller than this do not reduce wall surfaces/volumes (default 0 = every opening reduces them)"),
      includeExposedSurfaces: z.boolean().optional().describe("Also compute exposed (uncovered) surface areas per surface material. Default false"),
      coverElements: z.array(ElementRef).max(500).optional().describe("Elements that cover the measured ones for the exposed-surface calculation"),
    },
    annotations: READ_ONLY,
    handler: async ({ elements, coverElements, ...rest }, { ac }) =>
      ac.addon("GetElementQuantities", {
        elements: elements.map(guidOf),
        ...(coverElements ? { coverElements: coverElements.map(guidOf) } : {}),
        ...rest,
      }),
  });

  defineTool(server, ctx, {
    name: "get_connected_elements",
    title: "Get connected elements",
    description:
      "Elements attached to or hosted by each given element: windows/doors of a wall, skylights of a roof/shell, openings cut " +
      "into walls/slabs/beams, labels attached to the element; plus the owner/host (wall of a door, roof of a skylight, element of a " +
      "label, curtain wall of a panel...). Optionally solid element operations (operators cutting this element / targets it cuts) and " +
      "roof/shell trims. Output: [{guid, type, connectedCount, connected: {Window: [guid...], Door: [...], ...}, owner?, solidOperations?, trims?}]. " +
      "For zone boundaries and wall joins use get_element_relations; for curtain wall/stair/railing parts use get_subelements.",
    input: {
      elements: ElementRefs.max(200),
      types: z.array(ElementType).optional().describe("Connected element types to look for. Default: Window, Door, Skylight, Opening, Label"),
      includeOwner: z.boolean().optional().describe("Return the host/owner element. Default true"),
      includeSolidOperations: z.boolean().optional().describe("Return solid element operation links. Default false"),
      includeTrims: z.boolean().optional().describe("Return trim relations (elements trimmed to roofs/shells). Default false"),
      includeTypes: z.boolean().optional().describe("Return connected elements as {guid, type} instead of plain GUIDs. Default false"),
    },
    annotations: READ_ONLY,
    handler: async ({ elements, ...rest }, { ac }) => ac.addon("GetConnectedElements", { elements: elements.map(guidOf), ...rest }),
  });

  defineTool(server, ctx, {
    name: "get_element_relations",
    title: "Get element relations",
    description:
      "Topological relations computed by Archicad. Zone: relatedElementsByType (walls, columns, slabs, doors, windows, curtain walls " +
      "... bounding or inside the zone), wallParts/beamParts/curtainWallSegmentParts (boundary pieces: zoneEdgeIndex, tBegin, tEnd), " +
      "niches (height, polygon). Wall: connectionPolygon (real plan outline after joins) and the walls connected at its begin/end, to " +
      "its reference line, with their ends, or crossing it. Beam: the same for beams, plus per segment. Window/Door/Skylight/" +
      "CurtainWallPanel: fromZone/toZone (the zones on both sides). Roof/Shell: zones below. Other types return an error item.",
    input: {
      elements: ElementRefs.max(200),
      includePolygons: z.boolean().optional().describe("Include wall/beam connection polygons and zone niche polygons ({points, arcs?, holes?}, m). Default true"),
    },
    annotations: READ_ONLY,
    handler: async ({ elements, ...rest }, { ac }) => ac.addon("GetElementRelations", { elements: elements.map(guidOf), ...rest }),
  });

  defineTool(server, ctx, {
    name: "get_subelements",
    title: "Get sub-elements",
    description:
      "Parts of hierarchical elements, grouped by type: CurtainWall → CurtainWallSegment/Frame/Panel/Junction/Accessory (frames and " +
      "panels with className, begin/end or centroid, hidden/degenerate flags); Stair → Riser/Tread/StairStructure (sequenceNumber, " +
      "landing flag); Railing → RailingSegment/Node/Post/InnerPost/Toprail/Handrail/Rail/Panel/BalusterSet/Baluster/Pattern and rail " +
      "ends/connections; Beam → BeamSegment; Column → ColumnSegment. Passing a sub-element GUID returns its owner. The part GUIDs work " +
      "with get_element_details, get_element_quantities and get_element_3d_geometry.",
    input: {
      elements: ElementRefs.max(100),
      types: z.array(z.enum(SUBELEMENT_TYPES)).optional().describe("Only these sub-element types"),
      maxPerType: z.number().int().min(0).max(100000).optional().describe("List at most this many parts per type (counts are always complete). Default 500"),
      includeDetails: z.boolean().optional().describe("Include the type-specific fields of each part. Default true"),
    },
    annotations: READ_ONLY,
    handler: async ({ elements, ...rest }, { ac }) => ac.addon("GetSubelements", { elements: elements.map(guidOf), ...rest }),
  });

  defineTool(server, ctx, {
    name: "get_selection",
    title: "Get selection",
    description:
      "Returns what is currently selected in Archicad: {selectionType: None|Elements|MarqueePolygon|MarqueeBox|MarqueeRotatedBox, " +
      "total, editableCount, elements: [{guid, type, storyIndex, layer, elementId, partial?}], marquee?: {box, polygon, " +
      "boxRotationAngle, multiStory}}. Use it when the user refers to 'the selected elements' or 'this'.",
    input: {
      onlyEditable: z.boolean().optional().describe("Only editable selected elements. Default false"),
      includePartial: z.boolean().optional().describe("Include partially selected elements (e.g. with the marquee). Default false"),
      marqueeRelation: z.enum(["InsidePartially", "InsideEntirely", "OutsidePartially", "OutsideEntirely"]).optional()
        .describe("When a marquee is active: which elements count as selected relative to it. Default InsidePartially"),
      includeElementId: z.boolean().optional().describe("Include Element IDs. Default true"),
      offset: z.number().int().min(0).optional().describe("Skip this many selected elements. Default 0"),
      limit: z.number().int().min(1).max(10000).optional().describe("Return at most this many. Default 1000"),
    },
    annotations: READ_ONLY,
    handler: async (args, { ac }) => ac.addon("GetSelection", args),
  });

  defineTool(server, ctx, {
    name: "set_selection",
    title: "Set selection",
    description:
      "Changes the Archicad selection (shows the user which elements you mean, or prepares a selection-based command). " +
      "mode 'set' replaces the selection (no elements = clear), 'add' adds, 'remove' deselects the given elements, 'clear' deselects " +
      "everything. Elements on hidden/locked layers or on another story than the active floor plan may fail — see 'failed'. " +
      "Output: {mode, requested, applied, failed: [{guid, error}], selectionCount}. Selection changes are not undo steps.",
    input: {
      mode: z.enum(["set", "add", "remove", "clear"]).optional().describe("Default 'set'"),
      elements: z.array(ElementRef).max(20000).optional().describe("Elements to (de)select; required for 'add' and 'remove'"),
    },
    annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: true },
    handler: async ({ mode, elements }, { ac }) => {
      const m = mode ?? "set";
      if ((m === "add" || m === "remove") && (!elements || elements.length === 0)) {
        throw new Error(`set_selection mode '${m}' needs 'elements' (GUIDs, e.g. from find_elements).`);
      }
      return ac.addon("SetSelection", { mode: m, ...(elements && m !== "clear" ? { elements: elements.map(guidOf) } : {}) });
    },
  });

  defineTool(server, ctx, {
    name: "get_element_2d_geometry",
    title: "Get element 2D geometry",
    description:
      "The 2D drawing primitives Archicad draws for elements in the active window (floor plan, section, layout...): lines, arcs/" +
      "circles/ellipses, polylines, polygons (with holes; fills), texts and pictures, with pen numbers and a role for special parts " +
      "(fill, openingDimension, arrow, drawingBorder). Helps to understand what a plan looks like (e.g. a door's swing, an object's " +
      "symbol). Output per element: {guid, type, total, counts, extent {xMin,yMin,xMax,yMax}, primitives: [{kind, pen, ...}], " +
      "truncated?, hotspots?}. Coordinates are [x, y] arrays in meters; angles in degrees; arcs in polygons use {index, angle} like polygon inputs. " +
      "Hatch pattern lines are only counted unless includeFillPatterns=true. Use summaryOnly for large elements.",
    input: {
      elements: ElementRefs.max(50),
      maxPrimitives: z.number().int().min(0).max(20000).optional().describe("Max primitives listed per element (all are counted). Default 300"),
      maxPoints: z.number().int().min(0).max(200000).optional().describe("Max coordinates listed per element. Default 20000"),
      kinds: z.array(z.enum(["point", "line", "arc", "polyline", "polygon", "text", "picture"])).optional()
        .describe("Only list these primitive kinds ('arc' includes circles and ellipses)"),
      summaryOnly: z.boolean().optional().describe("Only counts and extent, no primitive list. Default false"),
      includeFillPatterns: z.boolean().optional().describe("Also list the individual hatch pattern lines of fills. Default false"),
      includeHotspots: z.boolean().optional().describe("Also return the element's hotspots (snap points) as [x, y, z] (m; z of 2D hotspots is 0). Default false"),
    },
    annotations: READ_ONLY,
    handler: async ({ elements, ...rest }, { ac }) => ac.addon("GetElement2DGeometry", { elements: elements.map(guidOf), ...rest }),
  });

  defineTool(server, ctx, {
    name: "get_element_3d_geometry",
    title: "Get element 3D geometry",
    description:
      "3D model data of elements as generated by Archicad. mode 'summary' (default): bodyCount, vertexCount, edgeCount, polygonCount, " +
      "world boundingBox {xMin..zMax} (m, z absolute) and the materials used (surface attribute or GDL material name, polygon count). " +
      "mode 'mesh': additionally bodies: [{source (element/part GUID), bodyIndex, closed, material, vertices: [[x,y,z], ...], polygons: " +
      "[{v: [0-based vertex indices of the outer contour], holes?, material?, normal?}]}] within a vertex/polygon budget. Curtain " +
      "walls, stairs, railings, beams and columns include their parts. 2D elements have no 3D model (error item).",
    input: {
      elements: ElementRefs.max(50),
      mode: z.enum(["summary", "mesh"]).optional().describe("Default 'summary'"),
      includeSubelements: z.boolean().optional().describe("Include parts of curtain walls/stairs/railings/beams/columns. Default true"),
      includeMaterials: z.boolean().optional().describe("Report materials with polygon counts. Default true"),
      includeNormals: z.boolean().optional().describe("mesh mode: add world-space polygon normals. Default false"),
      maxVertices: z.number().int().min(0).max(200000).optional().describe("mesh mode: vertex budget per element; bodies beyond it are summarized only. Default 5000"),
      maxPolygons: z.number().int().min(0).max(200000).optional().describe("mesh mode: polygon budget per element. Default 5000"),
    },
    annotations: READ_ONLY,
    handler: async ({ elements, ...rest }, { ac }) => ac.addon("GetElement3DGeometry", { elements: elements.map(guidOf), ...rest }),
  });
}

/** Converts element references inside the shared filter args to plain GUID strings. */
function normalizeFilterArgs<T extends { withinElements?: z.infer<typeof ElementRef>[] }>(args: T): Record<string, unknown> {
  const { withinElements, ...rest } = args;
  return { ...rest, ...(withinElements ? { withinElements: withinElements.map(guidOf) } : {}) };
}
