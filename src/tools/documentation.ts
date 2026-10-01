/**
 * Documentation tools — databases, drawings on layouts, publishing, exports (IFC, DWG/DXF, PDF, 3D, module),
 * merging and hotlinks.
 *
 * Add-on commands (addon/Src/Commands/Documentation*.cpp):
 *   GetDatabases, GetLayoutDrawings, PlaceDrawings, ModifyDrawings, DeleteDrawings, UpdateDrawings,
 *   GetPublisherSets, PublishPublisherSet, ListIfcTranslators, ExportIfc, ExportPdf, ExportModule,
 *   ExportDxf, Export3DModel, GetHotlinks, PlaceHotlinks, UpdateHotlinks, DeleteHotlinks, MergeFile.
 * Creating layouts / subsets and layout settings are official JSON API commands (official family:
 * create_layout, create_layout_subset, get_layout_settings, set_layout_settings, get_publisher_sets).
 *
 * Units: lengths in meters (drawing positions/frames in PAPER meters on the sheet), angles in degrees.
 */

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import { CREATES, defineTool, DESTRUCTIVE, MODIFIES, READ_ONLY, type ToolContext } from "./define.js";
import {
  checkDwgTarget,
  convertDxfToDwg,
  DWG_UNAVAILABLE_MESSAGE,
  DWG_VERSIONS,
  dxfNameFor,
  findOdaConverter,
  makeDwgWorkFolder,
  removeFolderQuietly,
} from "./documentation-dwg.js";
import { AttrRef, ElementRef, ElementRefs, Guid, guidOf, LibPartRef, PenIndex, Point2D, Polygon, StoryRef } from "./schemas.js";

type Json = Record<string, unknown>;

/** Exports / publishing can take minutes on big projects. */
const LONG = { timeoutMs: 30 * 60_000 };
const MEDIUM = { timeoutMs: 10 * 60_000 };

// =============================================================================
// Shared schemas
// =============================================================================

export const DatabaseType = z
  .enum(["FloorPlan", "Section", "Elevation", "InteriorElevation", "Detail", "Worksheet", "DocumentFrom3D", "Layout", "MasterLayout"])
  .describe("Database type");

export const DatabaseRef = z
  .string()
  .min(1)
  .describe(
    "Database: \"FloorPlan\", the databaseRef / databaseGuid of get_databases, a navigator item guid (layout, view, viewpoint), or the exact " +
      "database name / reference ID / title (localized, e.g. Russian; case-insensitive fallback)",
  );

export const LayoutRef = z
  .string()
  .min(1)
  .describe(
    "Layout (or master layout): databaseGuid or navigatorItemGuid from get_databases {types: ['Layout']}, or the layout name / 'ID name' " +
      "exactly as in the Layout Book (localized, e.g. 'План на отм. ±0.000')",
  );

const ViewGuid = Guid.describe(
  "Navigator item guid of a view (View Map) or viewpoint (Project Map: story, section, elevation, detail, worksheet, 3D document, " +
    "schedule, index). Find them with get_databases {includeViews: true}, list_views or get_navigator_tree",
);

/** Which window/view an export shows. With none of them the current front window is exported. */
const TargetFields = {
  view: ViewGuid.optional().describe(
    "Open this navigator view/viewpoint first (a View Map view applies its saved story, layer combination, scale, zoom), then export",
  ),
  layout: LayoutRef.optional().describe("Export this layout (sheet)"),
  database: DatabaseRef.optional().describe("Export this database (section, elevation, detail, worksheet, 3D document, layout, 'FloorPlan')"),
  storyIndex: StoryRef.optional().describe("Export the floor plan of this story (index or localized name)"),
};

const OutputFileFields = {
  overwrite: z.boolean().optional().describe("Replace an existing file (default false: existing files are an error)"),
  createFolders: z.boolean().optional().describe("Create missing parent folders (default false)"),
};

const RestoreWindow = z
  .boolean()
  .optional()
  .describe("Bring back the window that was in front before the export (default true)");

const Anchor = z
  .enum(["LeftTop", "CenterTop", "RightTop", "LeftCenter", "Center", "RightCenter", "LeftBottom", "CenterBottom", "RightBottom"])
  .describe("Which point of the drawing's bounding box sits at 'position' (default Center)");

const DrawingFields = {
  position: Point2D.optional().describe(
    "Placement point on the sheet in PAPER meters from the sheet's bottom-left corner (A3 landscape is 0.42 x 0.297, its center 0.21, 0.1485). " +
      "Default: the sheet center. Sheet sizes: get_databases (sheet.width/height)",
  ),
  anchor: Anchor.optional(),
  useViewOrigin: z.boolean().optional().describe("Place by the view's own origin (project 0,0) instead of the bounding-box anchor"),
  scale: z
    .number()
    .positive()
    .optional()
    .describe("Scale denominator on paper: 100 = 1:100, 50 = 1:50. Default: the view's own scale. Use scale OR ratio"),
  ratio: z.number().positive().optional().describe("Magnification relative to the view's own scale (1 = as the view, 2 = twice as large)"),
  angle: z.number().optional().describe("Rotation on the sheet in degrees (Archicad's drawing angle, which the API defines clockwise)"),
  name: z.string().optional().describe("Custom drawing name (sets nameType 'Custom' unless nameType is given)"),
  nameType: z
    .enum(["ViewOrSourceFileName", "ViewIdAndName", "Custom"])
    .optional()
    .describe("Where the drawing name comes from: the view name, the view ID + name, or the custom 'name'"),
  number: z.string().optional().describe("Custom drawing number (sets numbering 'Custom' unless numbering is given)"),
  numbering: z.enum(["ByLayout", "ByViewId", "Custom"]).optional().describe("Drawing numbering: by layout sequence, by the view ID, or the custom 'number'"),
  includeInNumbering: z.boolean().optional().describe("Take part in the layout's drawing numbering sequence"),
  includeInAutoTexts: z.boolean().optional().describe("Include in drawing-scale autotexts and in drawing/layout indexes"),
  manualUpdate: z.boolean().optional().describe("true = the drawing is only refreshed by update_drawings; false = automatic update (default of the tool)"),
  colorMode: z.enum(["OriginalColors", "BlackAndWhite", "GrayScale"]).optional().describe("Pen color mode of the placed drawing"),
  penTable: z
    .union([z.literal("Own"), z.literal("Model"), AttrRef])
    .optional()
    .describe("Pen set used for the drawing: 'Own' (the view's), 'Model' (the model's) or a pen set name/index"),
  transparentBackground: z.boolean().optional().describe("Transparent drawing background"),
  border: z
    .object({
      show: z.boolean().optional().describe("Draw a border (default true when 'border' is given on placement)"),
      pen: PenIndex.optional(),
      lineType: AttrRef.optional().describe("Line type name/index of the border"),
      size: z.number().min(0).optional().describe("Border line thickness in m"),
    })
    .optional()
    .describe("Border line around the drawing"),
  title: z
    .union([z.literal(false), LibPartRef])
    .optional()
    .describe("Drawing title: false = no title, or a drawing-title library part (localized name, e.g. from search_library_parts)"),
  frame: z
    .union([z.literal(false), Polygon])
    .optional()
    .describe("Crop the drawing with this polygon (PAPER meters on the sheet); false = remove the crop"),
  layer: AttrRef.optional().describe("Layer of the drawing element"),
};

const DrawingPlacement = z.object({
  layout: LayoutRef.describe("Target layout (get_databases {types: ['Layout']}; create one with create_layout)"),
  view: ViewGuid.optional().describe("Source view/viewpoint navigator guid (preferred: a View Map view carries layers/scale/zoom)"),
  database: DatabaseRef.optional().describe(
    "Alternative source: a database (section, elevation, detail, worksheet, 'FloorPlan' + storyIndex); its View Map view is used, else its Project Map viewpoint",
  ),
  storyIndex: StoryRef.optional().describe("With database 'FloorPlan': which story (default: the current story)"),
  ...DrawingFields,
});

const DrawingChange = z.object({
  guid: Guid.describe("Drawing element guid (get_layout_drawings)"),
  ...DrawingFields,
});

const IfcScope = z
  .enum(["EntireProject", "VisibleOnAllStories", "AllOnCurrentStory", "VisibleOnCurrentStory", "Selection", "Elements"])
  .describe(
    "What to export: EntireProject (default), VisibleOnAllStories (respecting layer visibility), AllOnCurrentStory / VisibleOnCurrentStory " +
      "(use storyIndex), Selection (current selection), Elements (the 'elements' list; default when elements is given)",
  );

const Paper = z
  .object({
    width: z.number().positive().optional().describe("Page width in m (A4 portrait 0.21, A3 landscape 0.42)"),
    height: z.number().positive().optional().describe("Page height in m (A4 portrait 0.297, A3 landscape 0.297)"),
    margin: z.number().min(0).optional().describe("All four margins in m"),
    margins: z
      .object({
        left: z.number().min(0).optional(),
        top: z.number().min(0).optional(),
        right: z.number().min(0).optional(),
        bottom: z.number().min(0).optional(),
      })
      .optional()
      .describe("Individual margins in m"),
  })
  .describe("PDF page. Default: the layout's own sheet size for layouts, otherwise A3 landscape with 10 mm margins");

const PdfItem = z.object({
  path: z.string().min(1).describe("Absolute output path of the PDF, e.g. /private/tmp/claude-connector-tests/plan.pdf"),
  ...TargetFields,
  paper: Paper.optional(),
  restoreWindow: RestoreWindow,
  ...OutputFileFields,
});

const HotlinkPlacementFields = {
  position: z
    .object({ x: z.number(), y: z.number(), z: z.number().optional() })
    .optional()
    .describe("Where the source file's origin (0,0,0) lands in this project, meters (default 0,0,0; z = vertical offset)"),
  angle: z.number().optional().describe("Rotation around the insertion point in degrees, counter-clockwise (default 0)"),
  mirrored: z.boolean().optional().describe("Mirror the module (about its local Y axis) before rotating"),
  storyIndex: StoryRef.optional().describe("Home story of the placed module in this project (default: the current story)"),
  floorDifference: z.number().int().optional().describe("Advanced: story offset between the source's and this project's stories (default 0 / tool default)"),
  skipNested: z.boolean().optional().describe("Skip modules nested inside the source file"),
  ignoreTopFloorLinks: z.boolean().optional().describe("Ignore top links of walls/columns on the top story of the module"),
  relinkWallOpenings: z.boolean().optional().describe("Re-link wall openings of the module"),
  adjustLevelDiffs: z.boolean().optional().describe("Adjust elevations of elements with relative level linking"),
  suspendFixAngle: z.boolean().optional().describe("Adjust fixed-angle elements (e.g. texts) to the module rotation"),
  layer: AttrRef.optional().describe("Layer of the hotlink instance"),
};

const StoryRangeFields = {
  storyRange: z
    .enum(["AllStories", "SingleStory"])
    .optional()
    .describe("Hotlink all stories of the source (default) or a single one (then give sourceStory)"),
  sourceStory: z.number().int().optional().describe("Story index IN THE SOURCE FILE for a single-story hotlink (implies storyRange 'SingleStory')"),
};

const ModuleSourcePath = z
  .string()
  .min(1)
  .describe("Absolute path of an Archicad project (.pln), archive (.pla) or module (.mod, see export_module) file");

// =============================================================================
// Helpers
// =============================================================================

function exactlyOneTarget(args: Json, context: string): void {
  const given = ["view", "layout", "database", "storyIndex"].filter((k) => args[k] !== undefined);
  if (given.length > 1) throw new Error(`${context}: pass only one of view, layout, database, storyIndex (got ${given.join(", ")}).`);
}

function refsToGuids(refs: z.infer<typeof ElementRef>[] | undefined): string[] | undefined {
  return refs?.map(guidOf);
}

// =============================================================================
// Registration
// =============================================================================

export function registerDocumentationTools(server: McpServer, ctx: ToolContext): void {
  // ---------------------------------------------------------------------------
  // Databases
  // ---------------------------------------------------------------------------
  defineTool(server, ctx, {
    name: "get_databases",
    title: "List databases (plans, sections, layouts ...)",
    description:
      "Lists Archicad databases — the floor plan, sections, elevations, interior elevations, details, worksheets, 3D documents, layouts and " +
      "master layouts — with databaseRef (\"FloorPlan\" or a guid), type, name, reference ID, title and the linked marker element. Layouts also " +
      "get their Layout Book navigatorItemGuid, layoutId, master layout and sheet {width, height, margins} in meters. The databaseRef / " +
      "navigatorItemGuid values are what place_drawing, get_layout_drawings, update_drawings and the export tools take. includeViews adds, per " +
      "database, the View Map views that show it (navigatorItemGuid, scale) — the best sources for place_drawing. Also returns the current " +
      "database/window. Names are localized (Russian Archicad).",
    input: {
      types: z.array(DatabaseType).optional().describe("Only these database types (default: all)"),
      search: z.string().min(1).optional().describe("Only databases whose name, reference ID or title contains this text (case-insensitive)"),
      includeViews: z.boolean().optional().describe("Add the View Map views of each database (default false)"),
      includeLayoutInfo: z.boolean().optional().describe("Add sheet size/margins/numbering of layouts (default true)"),
      navigatorItems: z.array(Guid).optional().describe("Also resolve these navigator item guids to their database and scale"),
    },
    annotations: READ_ONLY,
    handler: async (args, { ac }) => ac.addon("GetDatabases", args),
  });

  // ---------------------------------------------------------------------------
  // Drawings
  // ---------------------------------------------------------------------------
  defineTool(server, ctx, {
    name: "get_layout_drawings",
    title: "Drawings placed on layouts",
    description:
      "Lists the drawings placed on layouts: guid, name, number, source view (navigatorItemGuid, name, type, link type, viewDeleted), status " +
      "(UpToDate | Modified), position and bounds in PAPER meters, anchor, angle, ratio, viewScale and effective scale (denominator, e.g. 100), " +
      "crop frame polygon, title, border, pen table / color mode, update mode. Default: every layout. Use it before modify_drawings / " +
      "update_drawings, and to check what a layout contains.",
    input: {
      layouts: z.array(LayoutRef).optional().describe("Only these layouts (default: all layouts)"),
      includeMasterLayouts: z.boolean().optional().describe("With no 'layouts': also scan master layouts (default false)"),
      includeFrame: z.boolean().optional().describe("Include the crop/bounding polygon of each drawing (default true)"),
      includeLinkInfo: z.boolean().optional().describe("Include name/number and source link details (default true)"),
    },
    annotations: READ_ONLY,
    handler: async (args, { ac }) => ac.addon("GetLayoutDrawings", args),
  });

  defineTool(server, ctx, {
    name: "place_drawing",
    title: "Place views on layouts",
    description:
      "Places views as drawings on layouts (one undo step for the batch). Each item: layout + source ('view' navigator guid — preferably a View " +
      "Map view from get_databases {includeViews: true} — or 'database', e.g. a section databaseRef, or 'FloorPlan' + storyIndex), then optional " +
      "position (paper meters from the sheet's bottom-left corner, default the sheet center), anchor, scale (100 = 1:100) or ratio, angle, name / " +
      "number, crop frame, title, border, pen set, update mode, layer. The first target layout is brought to the front unless restoreWindow. Returns " +
      "[{guid, layout, source, drawing: {position, bounds, scale, status ...}} | {error}] in input order. Create layouts with create_layout.",
    input: {
      drawings: z.array(DrawingPlacement).min(1).max(100).describe("Drawings to place"),
      restoreWindow: z.boolean().optional().describe("Return to the previous front window afterwards (default false: the layout stays in front)"),
      undoName: z.string().optional().describe("Name of the undo step"),
    },
    annotations: CREATES,
    handler: async (args, { ac }) => {
      args.drawings.forEach((d, i) => {
        if ((d.view === undefined) === (d.database === undefined)) {
          throw new Error(`drawings[${i}]: give exactly one source — 'view' (navigator guid) or 'database' (databaseRef).`);
        }
        if (d.scale !== undefined && d.ratio !== undefined) throw new Error(`drawings[${i}]: pass either scale or ratio, not both.`);
      });
      return ac.addon("PlaceDrawings", args, MEDIUM);
    },
  });

  defineTool(server, ctx, {
    name: "modify_drawings",
    title: "Change placed drawings",
    description:
      "Changes drawings already placed on layouts (one undo step): position (paper m), anchor, angle, scale / ratio, name, number, numbering, crop " +
      "frame (polygon or false), title (false or a title library part), border, pen set, color mode, update mode, layer. The source view of a " +
      "drawing cannot be changed in Archicad 26 — place a new drawing and delete the old one instead. Returns [{guid, drawing} | {error}].",
    input: {
      drawings: z.array(DrawingChange).min(1).max(200).describe("Drawings to change: guid + the fields to set"),
      undoName: z.string().optional().describe("Name of the undo step"),
    },
    annotations: MODIFIES,
    handler: async (args, { ac }) => {
      args.drawings.forEach((d, i) => {
        if (d.scale !== undefined && d.ratio !== undefined) throw new Error(`drawings[${i}]: pass either scale or ratio, not both.`);
      });
      return ac.addon("ModifyDrawings", args);
    },
  });

  defineTool(server, ctx, {
    name: "delete_drawings",
    title: "Delete placed drawings",
    description: "Deletes drawing elements from layouts (one undo step). The source views are not touched. Returns [{guid, deleted} | {error}].",
    input: {
      drawings: ElementRefs.describe("Drawing element guids (get_layout_drawings)"),
      undoName: z.string().optional().describe("Name of the undo step"),
    },
    annotations: DESTRUCTIVE,
    handler: async ({ drawings, undoName }, { ac }) => ac.addon("DeleteDrawings", { drawings: refsToGuids(drawings), ...(undoName ? { undoName } : {}) }),
  });

  defineTool(server, ctx, {
    name: "update_drawings",
    title: "Update drawings from their views",
    description:
      "Refreshes placed drawings from their source views: pass drawing guids, layouts, or all: true. Archicad 26 has no direct update call, so " +
      "the connector opens each affected layout (Archicad refreshes outdated auto-update drawings when a layout is shown), temporarily switching " +
      "manual-update drawings to automatic (includeManual, default true). Returns per drawing statusBefore / statusAfter (UpToDate | Modified | " +
      "Unknown) and counts. Publishing a set (publish_publisher_set) also updates the drawings it contains.",
    input: {
      drawings: z.array(ElementRef).min(1).optional().describe("Drawing element guids (get_layout_drawings)"),
      layouts: z.array(LayoutRef).min(1).optional().describe("Update every drawing on these layouts"),
      all: z.boolean().optional().describe("Update the drawings of every layout"),
      includeManual: z.boolean().optional().describe("Also refresh drawings set to manual update (default true)"),
      restoreWindow: RestoreWindow,
    },
    annotations: MODIFIES,
    handler: async (args, { ac }) => {
      const modes = [args.drawings !== undefined, args.layouts !== undefined, args.all === true].filter(Boolean).length;
      if (modes !== 1) throw new Error("Pass exactly one of: drawings (guids), layouts, or all: true.");
      return ac.addon("UpdateDrawings", { ...args, drawings: refsToGuids(args.drawings) }, LONG);
    },
  });

  // ---------------------------------------------------------------------------
  // Publishing
  // ---------------------------------------------------------------------------
  defineTool(server, ctx, {
    name: "publish_publisher_set",
    title: "Publish a publisher set",
    description:
      "Runs File > Publish for one publisher set (list them with get_publisher_sets; names are localized, e.g. '2 - Макеты'). The set's own " +
      "formats apply (PDF, DWG, DXF, IFC, images, BIMx ... as configured in Archicad's Publisher). outputPath overrides the set's folder. " +
      "Pass 'items' (publisher item navigator guids from get_publisher_sets {name}) to publish only those. Returns {published, outputFolder, " +
      "files: [{path, sizeBytes}] written during the run, durationSeconds}. Can take minutes; Archicad is busy meanwhile.",
    input: {
      name: z.string().min(1).optional().describe("Publisher set name (exact, then case-insensitive)"),
      index: z.number().int().min(0).optional().describe("Alternatively the 0-based index of the set"),
      outputPath: z
        .string()
        .min(1)
        .optional()
        .describe("Absolute output FOLDER (default: the set's own path), e.g. /private/tmp/claude-connector-tests/publish"),
      createFolders: z.boolean().optional().describe("Create the output folder when missing (default false)"),
      items: z.array(Guid).min(1).optional().describe("Publish only these items of the set (navigator item guids)"),
    },
    annotations: CREATES,
    handler: async (args, { ac }) => {
      if ((args.name === undefined) === (args.index === undefined)) throw new Error("Pass either 'name' or 'index' of the publisher set.");
      return ac.addon("PublishPublisherSet", args, LONG);
    },
  });

  // ---------------------------------------------------------------------------
  // IFC
  // ---------------------------------------------------------------------------
  defineTool(server, ctx, {
    name: "get_ifc_translators",
    title: "IFC export translators",
    description: "Lists the IFC export translators of the project (localized names; the first one is Archicad's default). Use a name in export_ifc.",
    input: {},
    annotations: READ_ONLY,
    handler: async (_args, { ac }) => ac.addon("ListIfcTranslators", {}),
  });

  defineTool(server, ctx, {
    name: "export_ifc",
    title: "Export IFC",
    description:
      "Saves the model as IFC (.ifc) or ifcXML (.ifcxml) with an IFC translator (default: the first/default one; see get_ifc_translators). " +
      "Scope: the entire project (default), visible elements, the current story, the selection, or an explicit element list. Returns {file: " +
      "{path, sizeBytes}, translator, scope, durationSeconds}. Big models can take minutes.",
    input: {
      path: z.string().min(1).describe("Absolute output path, e.g. /private/tmp/claude-connector-tests/model.ifc"),
      translator: z.string().min(1).optional().describe("Translator name (exact, case-insensitive, or a unique part of it)"),
      scope: IfcScope.optional(),
      elements: z.array(ElementRef).min(1).optional().describe("Elements to export (scope 'Elements')"),
      storyIndex: StoryRef.optional().describe("Story for the *CurrentStory scopes (switches the floor plan to it)"),
      format: z.enum(["ifc", "ifcxml"]).optional().describe("File format (default: from the path extension, else ifc)"),
      includeBoundingBoxGeometry: z.boolean().optional().describe("Also write bounding-box representations (default false)"),
      ...OutputFileFields,
    },
    annotations: CREATES,
    handler: async (args, { ac }) => ac.addon("ExportIfc", { ...args, elements: refsToGuids(args.elements) }, LONG),
  });

  // ---------------------------------------------------------------------------
  // DWG / DXF
  // ---------------------------------------------------------------------------
  defineTool(server, ctx, {
    name: "export_dwg",
    title: "Export DWG / DXF (2D)",
    description:
      "Exports a 2D window — floor plan story, section, elevation, detail, worksheet, 3D document or a whole layout (with its drawings and master " +
      "layout) — as DXF, or DWG. The file is written from the primitives Archicad draws for every visible element, so it matches the view: " +
      "lines, arcs, circles, polylines (with arcs), texts and fill pattern lines; layers = Archicad layers, colors = pen numbers, units mm by " +
      "default. Line types and solid fills are not transferred (fills: fillBoundaries). A section/elevation window holds only its own 2D " +
      "drafting — to get the model cut, place it on a layout and export the layout. format 'dwg' converts with the ODA File Converter (must be " +
      "installed); for Archicad's own DWG translator publish a Publisher Set configured for DWG. Returns {file: {path, sizeBytes}, entityCount, " +
      "counts, layerCount}.",
    input: {
      path: z.string().min(1).describe("Absolute output path ending in .dxf or .dwg, e.g. /private/tmp/claude-connector-tests/plan.dxf"),
      format: z.enum(["dxf", "dwg"]).optional().describe("Output format (default: from the path extension)"),
      ...TargetFields,
      elements: z.array(ElementRef).min(1).optional().describe("Export only these elements (they must be in the exported window's database)"),
      units: z.enum(["mm", "cm", "m", "in", "ft"]).optional().describe("Drawing units of the file (default mm)"),
      fillPatterns: z.boolean().optional().describe("Include the pattern lines of fills (default true)"),
      fillBoundaries: z.boolean().optional().describe("Also write fill areas as closed polylines, solid triangles as SOLIDs (default false)"),
      includeMasterLayout: z.boolean().optional().describe("Layouts: include the master layout content (title block) (default true)"),
      maxEntities: z.number().int().min(1000).max(20_000_000).optional().describe("Safety limit of written entities (default 3,000,000)"),
      dwgVersion: z.enum(DWG_VERSIONS).optional().describe("DWG version for format 'dwg' (default ACAD2018)"),
      restoreWindow: RestoreWindow,
      ...OutputFileFields,
    },
    annotations: CREATES,
    handler: async (args, { ac }) => {
      exactlyOneTarget(args, "export_dwg");
      const { format: formatArg, dwgVersion, path, ...rest } = args;
      const ext = path.toLowerCase().endsWith(".dwg") ? "dwg" : path.toLowerCase().endsWith(".dxf") ? "dxf" : undefined;
      const format = formatArg ?? ext ?? "dxf";
      const params: Json = { ...rest, elements: refsToGuids(args.elements) };
      if (format === "dxf") {
        if (ext === "dwg") throw new Error("The path ends with .dwg but format is 'dxf': use a .dxf path or format 'dwg'.");
        return ac.addon("ExportDxf", { ...params, path }, LONG);
      }

      // DWG: DXF into a work folder, then ODA File Converter.
      const converter = findOdaConverter();
      if (!converter) throw new Error(DWG_UNAVAILABLE_MESSAGE);
      await checkDwgTarget(path, args.overwrite === true, args.createFolders === true);
      const work = await makeDwgWorkFolder();
      try {
        const dxfName = dxfNameFor(path);
        const dxf = await ac.addon<Json>("ExportDxf", { ...params, path: `${work.input}/${dxfName}`, overwrite: true, createFolders: false }, LONG);
        const { sizeBytes } = await convertDxfToDwg(converter, work.input, work.output, dxfName, path, dwgVersion ?? "ACAD2018");
        return { ...dxf, file: { path, exists: true, sizeBytes }, format: `DWG (${dwgVersion ?? "ACAD2018"}, converted from DXF by ODA File Converter)` };
      } finally {
        await removeFolderQuietly(work.root);
      }
    },
  });

  // ---------------------------------------------------------------------------
  // PDF
  // ---------------------------------------------------------------------------
  defineTool(server, ctx, {
    name: "export_pdf",
    title: "Export PDF",
    description:
      "Saves a window as PDF (Archicad's Save As PDF): a layout (sheet size from the layout), a floor plan story, section, elevation, detail, " +
      "worksheet, 3D document, a View Map view, or the current front window. Batch with 'exports' (one PDF per item; common options apply to all). " +
      "Page size/margins via 'paper' (meters; default: layout sheet, else A3 landscape). For many layouts in one go prefer publish_publisher_set. " +
      "Returns {file: {path, sizeBytes}, exported, paper} (or results[] for a batch).",
    input: {
      path: z.string().min(1).optional().describe("Absolute output path of a single PDF, e.g. /private/tmp/claude-connector-tests/layout.pdf"),
      ...TargetFields,
      paper: Paper.optional(),
      exports: z.array(PdfItem).min(1).max(100).optional().describe("Batch: several PDFs, each {path, view | layout | database | storyIndex, paper?}"),
      restoreWindow: RestoreWindow,
      ...OutputFileFields,
    },
    annotations: CREATES,
    handler: async (args, { ac }) => {
      if ((args.path === undefined) === (args.exports === undefined)) throw new Error("Pass either 'path' (one PDF) or 'exports' (batch).");
      if (args.exports) args.exports.forEach((e, i) => exactlyOneTarget(e, `exports[${i}]`));
      else exactlyOneTarget(args, "export_pdf");
      return ac.addon("ExportPdf", args, LONG);
    },
  });

  // ---------------------------------------------------------------------------
  // 3D
  // ---------------------------------------------------------------------------
  defineTool(server, ctx, {
    name: "export_3d_model",
    title: "Export 3D model (OBJ / STL / GSM)",
    description:
      "Exports 3D geometry: .obj (Wavefront, with a .mtl of surface colors/transparency next to it), .stl (binary by default, triangulated, for " +
      "3D printing / analysis) or .gsm (Archicad GDL object of the 3D window via Save as Object). Source: what the 3D window shows (default — " +
      "set it up first with the views tools, e.g. show selected elements in 3D, 3D cutaway), every 3D element of the project (source " +
      "'AllElements'), explicit 'elements', or the current selection. Coordinates are project coordinates; OBJ defaults to Y-up (Blender/ " +
      "three.js), STL to Z-up; units m by default. Returns {file, materialFile?, bodyCount, elementCount, vertexCount, faceCount|triangleCount, " +
      "boundingBox (m, Z-up)}.",
    input: {
      path: z.string().min(1).describe("Absolute output path ending in .obj, .stl or .gsm, e.g. /private/tmp/claude-connector-tests/model.obj"),
      format: z.enum(["obj", "stl", "gsm"]).optional().describe("Output format (default: from the extension)"),
      elements: z.array(ElementRef).min(1).optional().describe("Export only these 3D elements (curtain walls/stairs/railings include their parts)"),
      useSelection: z.boolean().optional().describe("Export the currently selected elements"),
      source: z
        .enum(["3DWindow", "AllElements"])
        .optional()
        .describe("Without elements/useSelection: '3DWindow' (default: what the 3D window currently shows) or 'AllElements' (every 3D element on every story)"),
      includeSubelements: z.boolean().optional().describe("With elements: include curtain wall / stair / railing / segmented beam-column parts (default true)"),
      units: z.enum(["m", "mm", "cm", "in", "ft"]).optional().describe("File units (default m)"),
      upAxis: z.enum(["Y", "Z"]).optional().describe("Up axis in the file (default Y for obj, Z for stl)"),
      materials: z.boolean().optional().describe("obj: write the .mtl material file (default true)"),
      binary: z.boolean().optional().describe("stl: binary (default true) or ASCII"),
      includeInvisible: z.boolean().optional().describe("Also export polygons Archicad marks invisible (default false)"),
      gdlMode: z.enum(["Binary", "Text"]).optional().describe("gsm: binary 3D data (default) or editable GDL text"),
      placeable: z.boolean().optional().describe("gsm: make the object placeable (default true)"),
      restoreWindow: RestoreWindow,
      ...OutputFileFields,
    },
    annotations: CREATES,
    handler: async (args, { ac }) => {
      if (args.elements && args.useSelection) throw new Error("Pass either 'elements' or useSelection: true, not both.");
      if (args.source && (args.elements || args.useSelection)) throw new Error("'source' is only used without elements/useSelection.");
      return ac.addon("Export3DModel", { ...args, elements: refsToGuids(args.elements) }, LONG);
    },
  });

  defineTool(server, ctx, {
    name: "export_module",
    title: "Save elements as module (.mod)",
    description:
      "Saves elements (or the current selection) as an Archicad module file (.mod) — the file format for hotlinked modules (place_hotlink) and " +
      "for merging into other projects (merge_file). All elements must be in one database (floor plan, or one section/detail/worksheet). " +
      "Returns {file: {path, sizeBytes}, elementCount}.",
    input: {
      path: z.string().min(1).describe("Absolute output path ending in .mod, e.g. /private/tmp/claude-connector-tests/core.mod"),
      elements: z.array(ElementRef).min(1).optional().describe("Elements to save"),
      useSelection: z.boolean().optional().describe("Save the current selection instead"),
      ...OutputFileFields,
    },
    annotations: CREATES,
    handler: async (args, { ac }) => {
      if (!args.elements === !args.useSelection) throw new Error("Pass either 'elements' or useSelection: true.");
      return ac.addon("ExportModule", { ...args, elements: refsToGuids(args.elements) }, MEDIUM);
    },
  });

  // ---------------------------------------------------------------------------
  // Merge / hotlinks
  // ---------------------------------------------------------------------------
  defineTool(server, ctx, {
    name: "merge_file",
    title: "Merge a .pln / .mod file",
    description:
      "Merges another Archicad project (.pln), archive (.pla) or module (.mod) into this project as own, editable elements (Archicad 26 has no " +
      "merge API: the file is placed as a hotlinked module and the hotlink is broken). Optional placement (position m, angle deg, mirrored, home " +
      "story) and story range. keepHotlink: true leaves it as a live hotlink instead. IFC / DWG / DXF cannot be merged through the API (use " +
      "Archicad's File > Interoperability > Merge). Refuses files that are already hotlinked (breaking would convert those too). Returns {merged, " +
      "elementCount, countsByType, elements: [guid]}. Undo: two steps (place + break).",
    input: {
      path: ModuleSourcePath,
      name: z.string().optional().describe("Name of the temporary hotlink (default 'Merge - <file name>')"),
      keepHotlink: z.boolean().optional().describe("Keep it as a hotlinked module instead of breaking it into own elements (default false)"),
      maxGuids: z.number().int().min(0).max(20000).optional().describe("Return at most this many new element guids (default 500; counts are always complete)"),
      ...StoryRangeFields,
      ...HotlinkPlacementFields,
    },
    annotations: CREATES,
    handler: async (args, { ac }) => ac.addon("MergeFile", args, LONG),
  });

  defineTool(server, ctx, {
    name: "get_hotlinks",
    title: "Hotlinks (sources and instances)",
    description:
      "Lists hotlink nodes — hotlinked modules (.pln/.pla/.mod) and XRefs (DWG/DXF) — as a tree: nodeGuid, name, type, source path, " +
      "sourceStatus (Available | Missing | NotAccessible | ...), story range, last update, nested children, and the placed instances (guid, home " +
      "story, position {x,y,z} m, angle deg, mirrored, options, optional elementCount). Use nodeGuid with place_hotlink {node}, update_hotlinks " +
      "and delete_hotlinks.",
    input: {
      types: z.array(z.enum(["Module", "XRef"])).optional().describe("Only these hotlink types (default both)"),
      includeInstances: z.boolean().optional().describe("List the placed instances of each node (default true)"),
      includeElementCounts: z.boolean().optional().describe("Count the elements inside each instance (default false; slower)"),
    },
    annotations: READ_ONLY,
    handler: async (args, { ac }) => ac.addon("GetHotlinks", args),
  });

  defineTool(server, ctx, {
    name: "place_hotlink",
    title: "Place hotlinked modules",
    description:
      "Places hotlinked modules (one undo step): from a file ('source': .pln / .pla / .mod — a hotlink node is created, or an existing node with " +
      "the same file and story settings is reused) or another instance of an existing node ('node' from get_hotlinks). Position/angle/mirror " +
      "set where the source's origin lands; storyIndex is the home story. The module stays linked: refresh it with update_hotlinks, break it " +
      "into own elements with delete_hotlinks {keepElements: true} (or use merge_file). Returns [{guid, nodeGuid, nodeCreated, instance, node} | " +
      "{error}]. XRef (DWG) attachment is not available through the Archicad 26 API.",
    input: {
      hotlinks: z
        .array(
          z.object({
            source: ModuleSourcePath.optional(),
            node: Guid.optional().describe("Existing hotlink node guid (get_hotlinks) — places another instance of it"),
            name: z.string().optional().describe("Name of a newly created hotlink node (default: the file name)"),
            reuseNode: z.boolean().optional().describe("Reuse an existing node of the same source file and settings (default true)"),
            ...StoryRangeFields,
            ...HotlinkPlacementFields,
          }),
        )
        .min(1)
        .max(100)
        .describe("Hotlink instances to place"),
      undoName: z.string().optional().describe("Name of the undo step"),
    },
    annotations: CREATES,
    handler: async (args, { ac }) => {
      args.hotlinks.forEach((h, i) => {
        if ((h.source === undefined) === (h.node === undefined)) throw new Error(`hotlinks[${i}]: give exactly one of 'source' (file path) or 'node' (hotlink node guid).`);
      });
      return ac.addon("PlaceHotlinks", args, LONG);
    },
  });

  defineTool(server, ctx, {
    name: "update_hotlinks",
    title: "Update / relink hotlinks",
    description:
      "Refreshes hotlinked modules / XRefs from their source files ('nodes' or all: true — like Hotlink Manager > Update) and/or relinks nodes to " +
      "another file or changes their name / story range ('relink': [{node, source?, name?, storyRange?, sourceStory?}], relinked nodes are " +
      "refreshed too). Missing sources are reported with a relink hint. In Teamwork reserve the hotlink cache first (reserve_elements {objectSets: ['HotlinkCacheManagement']}). Returns per node " +
      "{nodeGuid, updated|relinked, node} | {error}, updatedCount.",
    input: {
      nodes: z.array(Guid).min(1).optional().describe("Hotlink node guids to refresh (get_hotlinks; an instance guid is accepted too)"),
      all: z.boolean().optional().describe("Refresh every top-level hotlink"),
      relink: z
        .array(
          z.object({
            node: Guid.describe("Hotlink node guid (or an instance guid)"),
            source: ModuleSourcePath.optional().describe("New source file (absolute .pln / .pla / .mod path)"),
            name: z.string().optional().describe("New node name"),
            ...StoryRangeFields,
          }),
        )
        .min(1)
        .optional()
        .describe("Nodes to relink / reconfigure (one undo step), then refreshed"),
    },
    annotations: MODIFIES,
    handler: async (args, { ac }) => {
      if (!args.nodes && !args.all && !args.relink) throw new Error("Pass 'nodes', all: true, or 'relink'.");
      return ac.addon("UpdateHotlinks", args, LONG);
    },
  });

  defineTool(server, ctx, {
    name: "delete_hotlinks",
    title: "Delete or break hotlinks",
    description:
      "Removes hotlink nodes with ALL their placed instances (one undo step). keepElements: true breaks the link instead: the elements stay in the " +
      "project as own, editable elements. Nested hotlinks go with their parent. Returns [{nodeGuid, name, deleted|broken, instanceCount} | {error}].",
    input: {
      nodes: z.array(Guid).min(1).describe("Hotlink node guids (get_hotlinks; an instance guid selects its node)"),
      keepElements: z.boolean().optional().describe("true = break (keep the elements), false = delete them with the hotlink (default false)"),
    },
    annotations: DESTRUCTIVE,
    handler: async (args, { ac }) => ac.addon("DeleteHotlinks", args, MEDIUM),
  });
}
