/**
 * Project tools — project/application info, Project Info fields (autotexts), save / save as /
 * open / new / close / quit, Project Preferences, project location (geo), rebuild, undo/redo.
 *
 * Add-on commands: addon/Src/Commands/Project.cpp (+ ProjectSettings.inl.hpp: preferences / geo location,
 * ProjectEditMenu.inl.hpp: undo / redo). Field names match the C++ side 1:1.
 *
 * Setters are verified by reading back (project-verify.ts): Archicad 26 reports success for some changes it
 * silently ignores, and the tools report those fields as notApplied instead of pretending they worked.
 */

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import { ArchicadConnectionError, type ArchicadClient } from "../archicad/client.js";
import { defineTool, DESTRUCTIVE, MODIFIES, READ_ONLY, type ToolContext } from "./define.js";
import { diffRequested, type Mismatch } from "./project-verify.js";
import { AttrRef } from "./schemas.js";

/** Timings (overridable by tests). */
export const projectTiming = {
  /** Wait after scheduling an undo/redo before reading its result (ms, + per step). */
  undoSettleMs: 400,
  undoSettlePerStepMs: 150,
  undoTitleWaitMs: 1000, // the add-on waits up to ~1 s per step for the Edit menu title to change
  /** Polling while Archicad switches projects after a dropped connection. */
  pollIntervalMs: 1000,
  switchTimeoutMs: 90_000,
  /** HTTP timeout for save/open/new (large projects take long). */
  longOperationMs: 600_000,
};

const sleep = (ms: number) => new Promise<void>((resolve) => setTimeout(resolve, ms));

type Json = Record<string, unknown>;

/** Drops undefined values so only the fields the caller gave reach the add-on. */
function compact<T extends Record<string, unknown>>(obj: T): Json {
  const out: Json = {};
  for (const [k, v] of Object.entries(obj)) if (v !== undefined) out[k] = v;
  return out;
}

/** After a connection drop during a project switch: waits until Archicad answers again and returns the project info. */
async function waitForProject(ac: ArchicadClient): Promise<unknown> {
  const deadline = Date.now() + projectTiming.switchTimeoutMs;
  let last: unknown;
  while (Date.now() < deadline) {
    try {
      return await ac.addon("GetProjectInfo", {}, { timeoutMs: 10_000 });
    } catch (err) {
      last = err;
    }
    await sleep(projectTiming.pollIntervalMs);
  }
  throw new Error(
    `Archicad did not answer within ${Math.round(projectTiming.switchTimeoutMs / 1000)} s after the project switch ` +
      `(${last instanceof Error ? last.message : String(last)}). A dialog may be waiting in Archicad (e.g. missing libraries); check Archicad, then call get_project_info.`,
  );
}

/** Runs an operation that replaces the open project; tolerates the HTTP connection dropping meanwhile. */
async function switchProject(ac: ArchicadClient, command: string, params: Json): Promise<unknown> {
  try {
    return await ac.addon(command, params, { timeoutMs: projectTiming.longOperationMs });
  } catch (err) {
    if (err instanceof ArchicadConnectionError && !err.message.includes("did not answer")) {
      const project = await waitForProject(ac);
      return {
        status: "connectionDroppedDuringSwitch",
        note: "The HTTP connection dropped while Archicad switched projects. This is the project that is open now — verify it is the expected one.",
        project,
      };
    }
    throw err;
  }
}

function requireUnsavedDecision(args: { saveFirst?: boolean; discardChanges?: boolean }, what: string): void {
  if (args.saveFirst && args.discardChanges) throw new Error("Pass either saveFirst: true or discardChanges: true, not both.");
  if (!args.saveFirst && !args.discardChanges) {
    throw new Error(
      `${what} closes the current project. Decide what happens to its unsaved changes: saveFirst: true (save it first — it must have been saved before; ` +
        "for an untitled project call save_project_as first) or discardChanges: true (unsaved changes are lost).",
    );
  }
}

// --- Shared schema pieces ------------------------------------------------------------------------

const UnsavedChanges = {
  saveFirst: z.boolean().optional().describe("Save the current project in place before continuing (fails for untitled projects: use save_project_as first)"),
  discardChanges: z.boolean().optional().describe("Continue WITHOUT saving: unsaved changes of the current project are lost. Exactly one of saveFirst / discardChanges must be true"),
};

const LengthUnit = z
  .enum(["Meter", "Decimeter", "Centimeter", "Millimeter", "FootFracInch", "FootDecInch", "DecFoot", "FracInch", "DecInch", "Kilometer", "Yard"])
  .describe("Length unit");
const AreaUnit = z
  .enum(["SquareMeter", "SquareKilometer", "SquareDecimeter", "SquareCentimeter", "SquareMillimeter", "SquareFoot", "SquareInch", "SquareYard"])
  .describe("Area unit");
const VolumeUnit = z
  .enum(["CubicMeter", "CubicKilometer", "Liter", "CubicCentimeter", "CubicMillimeter", "CubicFoot", "CubicInch", "CubicYard", "Gallon"])
  .describe("Volume unit");
const AngleUnit = z.enum(["DecimalDegree", "DegreeMinSec", "Grad", "Radian", "Surveyors"]).describe("Angle unit");
const ExtraAccuracy = z
  .enum(["Off", "Small5", "Small25", "Small1", "Small01", "Fractions"])
  .describe("Extra accuracy shown as small digits after the decimals (Off = none; Fractions only for inch units)");
const Decimals = z.number().int().min(0).max(4);
const RoundInch = z
  .union([z.literal(1), z.literal(2), z.literal(4), z.literal(8), z.literal(16), z.literal(32), z.literal(64)])
  .describe("Fractional inch rounding denominator (1/N inch), only used by inch units");

const LengthDimFormat = z
  .object({
    unit: LengthUnit.optional(),
    decimals: Decimals.optional().describe("Number of decimals 0-4"),
    roundInch: RoundInch.optional(),
    extraAccuracy: ExtraAccuracy.optional(),
    hideZeroDecimals: z.boolean().optional().describe("Hide trailing zero decimals"),
    showZeroWhole: z.boolean().optional().describe("Show a leading zero for values below 1 (e.g. 0.5 instead of .5)"),
    showZeroInch: z.boolean().optional().describe("Show 0 inches (feet-inch units)"),
  })
  .strict();
const AngleDimFormat = z
  .object({
    unit: AngleUnit.optional(),
    decimals: Decimals.optional(),
    accuracy: Decimals.optional().describe("Angle accuracy 0-4"),
    hideZeroDecimals: z.boolean().optional(),
  })
  .strict();
const AreaDimFormat = z
  .object({ unit: AreaUnit.optional(), decimals: Decimals.optional(), hideZeroDecimals: z.boolean().optional() })
  .strict();

const PreferenceSections = {
  workingUnits: z
    .object({
      lengthUnit: LengthUnit.optional().describe("Model unit (Options > Project Preferences > Working Units)"),
      areaUnit: AreaUnit.optional(),
      volumeUnit: VolumeUnit.optional(),
      angleUnit: AngleUnit.optional(),
      lengthDecimals: Decimals.optional(),
      areaDecimals: Decimals.optional(),
      volumeDecimals: Decimals.optional(),
      angleDecimals: Decimals.optional(),
      angleAccuracy: Decimals.optional(),
      roundInch: RoundInch.optional(),
    })
    .strict()
    .describe("Working units (display only — the API always works in meters/degrees)"),
  dimensions: z
    .object({
      standard: z
        .union([z.literal("Custom"), AttrRef])
        .optional()
        .describe(
          "Select a Dimension Standard attribute (index or localized name, list with get_attributes {type: 'DimensionStandard'}) " +
            "whose formats become active, or 'Custom'. While a standard is selected Archicad ignores changed formats, so format " +
            "changes switch to Custom settings automatically (reported in 'note')",
        ),
      linear: LengthDimFormat.optional().describe("Linear dimensions"),
      angular: AngleDimFormat.optional().describe("Angular dimensions"),
      radial: LengthDimFormat.optional().describe("Radial dimensions"),
      level: LengthDimFormat.optional().describe("Level dimensions"),
      elevation: LengthDimFormat.optional().describe("Elevation dimensions"),
      doorWindow: LengthDimFormat.optional().describe("Door, window and skylight dimensions"),
      sillHeight: LengthDimFormat.optional().describe("Sill height dimensions"),
      area: AreaDimFormat.optional().describe("Area calculations"),
    })
    .strict()
    .describe("Dimension display formats (Project Preferences > Dimensions)"),
  calculationUnits: z
    .object({
      length: z.object({ unit: LengthUnit.optional(), decimals: Decimals.optional(), roundInch: RoundInch.optional(), extraAccuracy: ExtraAccuracy.optional() }).strict().optional(),
      area: z.object({ unit: AreaUnit.optional(), decimals: Decimals.optional(), extraAccuracy: ExtraAccuracy.optional() }).strict().optional(),
      volume: z.object({ unit: VolumeUnit.optional(), decimals: Decimals.optional(), extraAccuracy: ExtraAccuracy.optional() }).strict().optional(),
      angle: z.object({ unit: AngleUnit.optional(), decimals: Decimals.optional(), accuracy: Decimals.optional() }).strict().optional(),
      useDisplayedValues: z.boolean().optional().describe("Use displayed (rounded) values in calculations"),
    })
    .strict()
    .describe("Calculation units used by schedules/properties (Project Preferences > Calculation Units & Rules)"),
  calculationRules: z
    .object({
      elementRules: z
        .array(
          z.object({
            property: z.enum(["Volume", "Surface", "Length"]).describe("Which quantity the hole rule applies to"),
            holeLimit: z.number().min(0).describe("Holes smaller than this are NOT subtracted (m³ for Volume, m² for Surface, m for Length)"),
            elementTypes: z.array(z.string()).min(1).describe("Element types, e.g. ['Wall', 'Slab', 'Roof', 'Shell', 'Beam', 'Column']"),
          }),
        )
        .optional()
        .describe("Replaces ALL conditional hole-subtraction rules"),
      wallInsulationMaterials: z
        .array(AttrRef)
        .optional()
        .describe("BUILDING MATERIALS (localized names or indices, see get_attributes {type: 'BuildingMaterial'}) whose skins count as wall insulation — replaces the list"),
      wallAirMaterials: z.array(AttrRef).optional().describe("Building materials whose skins count as wall air space — replaces the list"),
      roofInsulationMaterials: z.array(AttrRef).optional().describe("Building materials whose skins count as roof insulation — replaces the list"),
      shellInsulationMaterials: z.array(AttrRef).optional().describe("Building materials whose skins count as shell insulation — replaces the list"),
    })
    .strict()
    .describe("Calculation rules (conditional hole subtraction, special skins)"),
  referenceLevels: z
    .object({
      level1: z.object({ name: z.string().optional(), elevation: z.number().optional().describe("m, relative to Project Zero") }).strict().optional(),
      level2: z.object({ name: z.string().optional(), elevation: z.number().optional().describe("m, relative to Project Zero") }).strict().optional(),
    })
    .strict()
    .describe("The two custom reference levels (e.g. Sea Level) used by elevation values"),
  legacy: z
    .object({
      columnConnectionPriority: z.number().int().optional().describe("Raw column/beam connection priority value as returned by get_preferences"),
      aboveLineType: AttrRef.optional().describe("Line type for elements above the cut plane (legacy display)"),
      belowLineType: AttrRef.optional().describe("Line type for elements below the cut plane (legacy display)"),
      roofContourDisplay: z.enum(["AllVisibleContours", "OnlyTopSurface"]).optional(),
      useLegacyIntersections: z.boolean().optional(),
      hideZonesOnSections: z.boolean().optional(),
    })
    .strict()
    .describe("Legacy preferences"),
  zones: z
    .object({
      recesses: z
        .object({
          depth: z.number().optional().describe("Wall recess (niche) depth limit (m)"),
          size: z.number().optional().describe("Wall recess size limit"),
          combine: z.enum(["And", "Or"]).optional().describe("Recess counts when deeper AND/OR larger than the limits"),
          includeWindows: z.boolean().optional(),
          includeDoors: z.boolean().optional(),
          checkDepth: z.boolean().optional(),
          checkSize: z.boolean().optional(),
        })
        .strict()
        .optional()
        .describe("Wall recesses added to zone areas"),
      walls: z.object({ subtract: z.boolean().optional(), percent: z.number().int().min(0).max(100).optional(), sizeLimit: z.number().optional() }).strict().optional()
        .describe("Subtraction of wall parts inside zones"),
      columns: z.object({ subtract: z.boolean().optional(), percent: z.number().int().min(0).max(100).optional(), sizeLimit: z.number().optional() }).strict().optional()
        .describe("Subtraction of columns inside zones"),
      lowHeightReductions: z
        .array(z.object({ heightLimit: z.number().describe("m"), reductionPercent: z.number().int().min(0).max(100) }))
        .max(4)
        .optional()
        .describe("Area reduction of low room parts (up to 4 rows) — replaces the table"),
    })
    .strict()
    .describe("Zone area calculation preferences"),
  imagingAndCalculation: z
    .object({
      autoRebuild3D: z.boolean().optional(),
      activate3DOnChange: z.boolean().optional(),
      keepZoomedSection: z.boolean().optional(),
      showProgressWindow: z.boolean().optional(),
      interruptOnError: z.boolean().optional(),
      progressSounds: z.boolean().optional().describe("READ-ONLY: Archicad 26 ignores changes (passing it fails)"),
      write3DReport: z.boolean().optional(),
      reportLevel: z.enum(["None", "Brief", "Short", "Detailed", "Full"]).optional(),
      selectedElementsListing: z.enum(["ShowAlert", "ListAll", "UseFilters"]).optional(),
    })
    .strict()
    .describe("Imaging and calculation preferences"),
  floorPlanCutPlane: z
    .object({
      cutHeight: z.number().optional().describe("Cut plane height relative to the current story (m)"),
      topLevel: z.number().optional().describe("Top of the displayed range, relative to the story given by topStoryOffset (m)"),
      topStoryOffset: z.number().int().optional().describe("Top level's story, relative to the current story (1 = one above)"),
      bottomLevel: z.number().optional().describe("Bottom of the displayed range, relative to the story given by bottomStoryOffset (m)"),
      bottomStoryOffset: z.number().int().optional().describe("Bottom level's story, relative to the current story (0 = current, -1 = below)"),
      fixedLevel: z.number().optional().describe("Absolute display limit level (m, relative to Project Zero)"),
    })
    .strict()
    .describe("Floor Plan Cut Plane settings (Document > Floor Plan Cut Plane)"),
  layouts: z
    .object({
      masterItemColor: z
        .object({ red: z.number().min(0).max(1), green: z.number().min(0).max(1), blue: z.number().min(0).max(1) })
        .optional()
        .describe("READ-ONLY: Archicad 26 ignores changes of the master item color (passing it fails)"),
      useOwnMasterColor: z.boolean().optional(),
      adjustDrawingFrameToViewZoom: z.boolean().optional(),
    })
    .strict()
    .describe("Layout preferences"),
  environment: z
    .object({
      autoIntersect: z.boolean().optional().describe("Auto-intersect elements (Design > Auto Intersection)"),
      suspendGroups: z.boolean().optional().describe("Suspend Groups (edit grouped elements individually)"),
      autoTextEnabled: z.boolean().optional().describe("Autotext conversion on: '<KEY>' placeholders in texts are shown as values"),
    })
    .strict()
    .describe("Session switches (not stored in Project Preferences). autoGroup and exportTolerance are read-only"),
};

const PreferenceSectionName = z.enum([
  "workingUnits",
  "dimensions",
  "calculationUnits",
  "calculationRules",
  "referenceLevels",
  "legacy",
  "zones",
  "imagingAndCalculation",
  "floorPlanCutPlane",
  "layouts",
  "dataSafety",
  "environment",
]);

const GeoReference = z
  .object({
    name: z.string().optional().describe("Coordinate reference system name, e.g. 'EPSG:32638'"),
    description: z.string().optional(),
    geodeticDatum: z.string().optional().describe("e.g. 'WGS84'"),
    verticalDatum: z.string().optional(),
    mapProjection: z.string().optional().describe("e.g. 'UTM'"),
    mapZone: z.string().optional().describe("e.g. '38N'"),
    eastings: z.number().optional().describe("Easting of the Survey Point in the map CRS (m)"),
    northings: z.number().optional().describe("Northing of the Survey Point in the map CRS (m)"),
    orthogonalHeight: z.number().optional().describe("Height of the Survey Point above the vertical datum (m)"),
    xAxisAbscissa: z.number().optional().describe("X-axis direction vector, easting component (cos of the rotation)"),
    xAxisOrdinate: z.number().optional().describe("X-axis direction vector, northing component (sin of the rotation)"),
    scale: z.number().positive().optional().describe("Map scale factor (1 = none)"),
  })
  .strict()
  .describe("IFC map conversion / coordinate reference system data");

// --- Read-back verification ------------------------------------------------------------------------------

const NOT_APPLIED_NOTE =
  "Archicad accepted the call but kept the old value of these fields (the Archicad 26 API ignores some changes). " +
  "'value' shows the settings now in effect.";

/** Marks preference sections whose read-back value differs from the request (ok: false + notApplied). */
export function verifyPreferenceResults(params: Json, response: { results?: Record<string, Json> }): unknown {
  const results = response.results;
  if (!results) return response;
  const out: Record<string, unknown> = {};
  let allOk = true;
  for (const [section, result] of Object.entries(results)) {
    if (!result || typeof result !== "object" || !("value" in result) || result["ok"] !== true) {
      allOk = false;
      out[section] = result;
      continue;
    }
    const skip = section === "dimensions" ? ["standard"] : [];
    const mismatches = diffRequested(params[section], result["value"], { skip }, `${section}.`);
    if (mismatches.length > 0) {
      allOk = false;
      out[section] = { ...result, ok: false, notApplied: mismatches, note: NOT_APPLIED_NOTE };
    } else {
      out[section] = result;
    }
  }
  return { allApplied: allOk, results: out };
}

/** Compares a set_geo_location request with the returned location. */
export function verifyGeoLocation(params: Json, result: unknown): unknown {
  if (!result || typeof result !== "object") return result;
  const requested: Json = { ...params };
  delete requested["unlockSurveyPoint"];
  const mismatches: Mismatch[] = diffRequested(requested, result, {
    angles: ["northDirection"],
    rename: { surveyPointVisible: "surveyPoint.visible", surveyPointLocked: "surveyPoint.locked", surveyPoint: "surveyPoint.position" },
  });
  if (mismatches.length === 0) return { allApplied: true, ...(result as Json) };
  return { allApplied: false, notApplied: mismatches, note: NOT_APPLIED_NOTE, ...(result as Json) };
}

/** Turns set_project_info_fields items whose value Archicad silently kept into errors / warnings. */
export function verifyProjectInfoResults(requested: { value: string }[], response: { results?: unknown[] }): unknown {
  if (!Array.isArray(response.results)) return response;
  const results = response.results.map((r, i) => {
    const want = requested[i]?.value;
    if (!r || typeof r !== "object" || "error" in r || want === undefined) return r;
    const item = r as Json;
    if (typeof item["value"] !== "string" || item["value"] === want || "warning" in item) return r;
    if (item["category"] === "Other") {
      return {
        error: {
          message:
            `Archicad ignored the new value of '${String(item["name"])}' (${String(item["key"])}): fields of category 'Other' are computed by ` +
            `Archicad (dates, file name/path, layout/drawing/revision data) and cannot be set. Current value: '${item["value"]}'.`,
        },
      };
    }
    return { ...item, warning: `Archicad stored '${item["value"]}' instead of the requested '${want}'.` };
  });
  return { ...response, results };
}

// --- Registration ------------------------------------------------------------------------------------

export function registerProjectTools(server: McpServer, ctx: ToolContext): void {
  defineTool(server, ctx, {
    name: "get_project_info",
    title: "Project info",
    description:
      "Returns facts about the open project and Archicad: projectName, untitled (never saved), file {path, fileName, folder, fileType " +
      "(SoloProject/Archive/Template...), exists, writable}, teamwork + teamworkInfo, current window/database (type, name) and current story " +
      "(index, name, level), application {mainVersion, buildNumber, language, jsonApiPort...}, special folders (temporary, templates, " +
      "userDocuments, application, projectTemporary...). Pass includeTemplates: true to also list installed .tpl templates (for new_project). " +
      "Call this before save/open/new/close operations.",
    input: {
      includeTemplates: z.boolean().optional().describe("Also scan the templates/defaults folders and return 'templates': [.tpl paths] (slower)"),
    },
    annotations: READ_ONLY,
    handler: async (args, { ac }) => ac.addon("GetProjectInfo", compact(args)),
  });

  defineTool(server, ctx, {
    name: "get_project_info_fields",
    title: "Get Project Info fields",
    description:
      "Lists the Project Info fields (File > Info > Project Info), i.e. the project autotexts used in title blocks and layouts: " +
      "[{name (localized UI label, e.g. Russian), key (database key such as 'PROJECTNAME', 'CLIENT', 'autotext-<GUID>' for custom fields), " +
      "value, category}]. Categories: Fixed = built-in project fields, Custom = user-added fields, Other = values computed by Archicad " +
      "(dates, layout/drawing data; usually not settable). Use the key with set_project_info_fields.",
    input: {
      category: z.enum(["Fixed", "Custom", "Other", "All"]).optional().describe("Filter by category (default All)"),
      search: z.string().optional().describe("Case-insensitive substring matched against name, key and value"),
      nonEmptyOnly: z.boolean().optional().describe("Only fields with a non-empty value"),
    },
    annotations: READ_ONLY,
    handler: async (args, { ac }) => ac.addon("GetProjectInfoFields", compact(args)),
  });

  defineTool(server, ctx, {
    name: "set_project_info_fields",
    title: "Set Project Info fields",
    description:
      "Sets Project Info field values (shown by autotexts in title blocks/stamps). Identify each field by its database key (preferred; from " +
      "get_project_info_fields) or by its exact localized name. createIfMissing: true adds a CUSTOM field for names that do not exist. " +
      "Returns per-item results [{name, key, value, category, created?} | {error}]. Delete custom fields with delete_project_info_fields.",
    input: {
      fields: z
        .array(
          z
            .object({
              key: z.string().min(1).optional().describe("Database key, e.g. 'PROJECTNAME', 'CLIENT', 'autotext-...' (see get_project_info_fields)"),
              name: z.string().min(1).optional().describe("Localized field name as shown in Project Info (used when no key is given)"),
              value: z.union([z.string(), z.number()]).describe("New value (text; numbers are converted to text). \"\" clears the field"),
            })
            .strict()
            .refine((f) => f.key !== undefined || f.name !== undefined, { message: "each field needs 'key' or 'name'" }),
        )
        .min(1)
        .max(200),
      createIfMissing: z.boolean().optional().describe("Create a custom Project Info field when 'name' matches no existing field (default false)"),
    },
    annotations: MODIFIES,
    handler: async ({ fields, createIfMissing }, { ac }) => {
      const items = fields.map((f) => ({ key: f.key, name: f.name, value: typeof f.value === "number" ? String(f.value) : f.value }));
      const response = await ac.addon<{ results?: unknown[] }>(
        "SetProjectInfoFields",
        compact({ fields: items.map((f) => compact(f)), createIfMissing }),
      );
      return verifyProjectInfoResults(items, response);
    },
  });

  defineTool(server, ctx, {
    name: "delete_project_info_fields",
    title: "Delete custom Project Info fields",
    description:
      "Deletes CUSTOM Project Info fields (category Custom) by database key or name. Built-in fields cannot be deleted (set their value to \"\" instead). " +
      "Returns [{key, deleted} | {error}].",
    input: {
      fields: z.array(z.string().min(1)).min(1).max(100).describe("Keys ('autotext-<GUID>') or exact names of custom fields"),
    },
    annotations: DESTRUCTIVE,
    handler: async ({ fields }, { ac }) => ac.addon("DeleteProjectInfoFields", { fields }),
  });

  defineTool(server, ctx, {
    name: "save_project",
    title: "Save project",
    description:
      "Saves the open project to its own file (File > Save). Fails for untitled (never saved) projects — use save_project_as — and for " +
      "read-only projects. Returns {saved, project}.",
    input: {},
    annotations: MODIFIES,
    handler: async (_args, { ac }) => ac.addon("SaveProject", {}, { timeoutMs: projectTiming.longOperationMs }),
  });

  defineTool(server, ctx, {
    name: "save_project_as",
    title: "Save project as",
    description:
      "Saves the project under a new file (File > Save As). Afterwards the open project refers to the NEW file. Formats: pln (solo project), " +
      "pla (archive incl. library parts), tpl (template), pln25/pla25 (Archicad 25 = previous version). The format defaults to the path " +
      "extension; a missing extension is added. For IFC/DWG/PDF/image exports use the export tools instead. Returns {saved, format, file, project}.",
    input: {
      path: z.string().min(2).describe("Absolute target path on the Archicad machine, e.g. '/Users/me/Projects/House.pln'"),
      format: z.enum(["pln", "pla", "tpl", "pln25", "pla25"]).optional().describe("File format (default: from the extension, else pln)"),
      overwrite: z.boolean().optional().describe("Replace an existing file (default false: fails if the file exists)"),
      createFolders: z.boolean().optional().describe("Create missing parent folders (default false)"),
      archive: z
        .object({
          includeTextures: z.boolean().optional().describe("default true"),
          includeBackgroundPictures: z.boolean().optional().describe("default true"),
          includePropertyObjects: z.boolean().optional().describe("Include property library parts (default true)"),
          includeAllLibraryParts: z.boolean().optional().describe("Include the full loaded library, not only used parts (default false)"),
          picturesInTIFF: z.boolean().optional().describe("Store pictures as TIFF for cross-platform use (default false)"),
        })
        .strict()
        .optional()
        .describe("Options for pla/pla25 archives"),
    },
    annotations: MODIFIES,
    handler: async (args, { ac }) => ac.addon("SaveProjectAs", compact(args), { timeoutMs: projectTiming.longOperationMs }),
  });

  defineTool(server, ctx, {
    name: "open_project",
    title: "Open project",
    description:
      "Opens a project file (.pln, .pla archive, .tpl template → untitled project, .bpn backup), CLOSING the current project. You must decide " +
      "about unsaved changes: saveFirst: true or discardChanges: true. BIMcloud/Teamwork projects cannot be opened by path. Opening can take " +
      "minutes and Archicad may show dialogs (e.g. missing libraries). Returns {opened, project}.",
    input: {
      path: z.string().min(2).describe("Absolute path of the file to open"),
      ...UnsavedChanges,
      archiveLibraryFolder: z.string().optional().describe("For .pla archives: folder where the embedded library is extracted (optional)"),
    },
    annotations: DESTRUCTIVE,
    handler: async (args, { ac }) => {
      requireUnsavedDecision(args, "Opening a project");
      return switchProject(ac, "OpenProject", compact(args));
    },
  });

  defineTool(server, ctx, {
    name: "new_project",
    title: "New project",
    description:
      "Creates a new untitled project, CLOSING the current one: from a template (.tpl path — list installed ones with get_project_info " +
      "{includeTemplates: true}) or with File > New (reset: true = New & Reset, i.e. default settings). You must decide about unsaved changes: " +
      "saveFirst: true or discardChanges: true. Returns {created, project}.",
    input: {
      template: z.string().optional().describe("Absolute path of a .tpl template to start from"),
      reset: z.boolean().optional().describe("Without template: New & Reset (all settings back to defaults). Default false"),
      ...UnsavedChanges,
    },
    annotations: DESTRUCTIVE,
    handler: async (args, { ac }) => {
      requireUnsavedDecision(args, "Creating a new project");
      if (args.template !== undefined && args.reset !== undefined) throw new Error("'reset' only applies without 'template'.");
      return switchProject(ac, "NewProject", compact(args));
    },
  });

  defineTool(server, ctx, {
    name: "close_project",
    title: "Close project",
    description:
      "Closes the current project. WARNING: the Archicad JSON API only listens while a project is open, so after this NO tool works until a " +
      "project is opened by hand in Archicad. Prefer open_project/new_project to switch projects. Requires confirm: true and saveFirst or discardChanges.",
    input: {
      confirm: z.literal(true).describe("Must be true"),
      ...UnsavedChanges,
    },
    annotations: DESTRUCTIVE,
    handler: async (args, { ac }) => {
      requireUnsavedDecision(args, "Closing the project");
      try {
        return await ac.addon("CloseProject", compact(args), { timeoutMs: projectTiming.longOperationMs });
      } catch (err) {
        if (err instanceof ArchicadConnectionError) {
          return { closed: true, note: "The connection dropped as expected: the JSON API is unavailable until a project is opened in Archicad." };
        }
        throw err;
      }
    },
  });

  defineTool(server, ctx, {
    name: "quit_archicad",
    title: "Quit Archicad",
    description:
      "Quits Archicad. Without saveFirst: true, unsaved changes are DISCARDED. After quitting no tool works until Archicad is started again. " +
      "Requires confirm: true.",
    input: {
      confirm: z.literal(true).describe("Must be true"),
      saveFirst: z.boolean().optional().describe("Save the project in place first (fails — and does not quit — for untitled/read-only projects)"),
    },
    annotations: DESTRUCTIVE,
    handler: async ({ saveFirst }, { ac }) => {
      if (saveFirst) await ac.addon("SaveProject", {}, { timeoutMs: projectTiming.longOperationMs });
      try {
        const result = await ac.addon("QuitArchicad", { confirm: true });
        return { quit: true, saved: Boolean(saveFirst), result };
      } catch (err) {
        if (err instanceof ArchicadConnectionError) {
          return { quit: true, saved: Boolean(saveFirst), note: "The connection dropped while Archicad was quitting (expected)." };
        }
        throw err;
      }
    },
  });

  defineTool(server, ctx, {
    name: "get_preferences",
    title: "Get Project Preferences",
    description:
      "Reads Project Preferences: workingUnits, dimensions (display formats per dimension type), calculationUnits, calculationRules, " +
      "referenceLevels, legacy, zones, imagingAndCalculation, floorPlanCutPlane, layouts, dataSafety (temporary folder) and environment " +
      "switches (autoIntersect, autoGroup, suspendGroups, autoTextEnabled, exportTolerance). Units shown here only affect display; the " +
      "connector always uses meters/degrees. Field names are the ones set_preferences accepts.",
    input: {
      sections: z.array(PreferenceSectionName).min(1).optional().describe("Sections to read (default: all)"),
    },
    annotations: READ_ONLY,
    handler: async ({ sections }, { ac }) => ac.addon("GetPreferences", compact({ sections })),
  });

  defineTool(server, ctx, {
    name: "set_preferences",
    title: "Set Project Preferences",
    description:
      "Changes Project Preferences (only the given sections/fields; read the current values with get_preferences first). Examples: " +
      "{workingUnits: {lengthUnit: 'Millimeter', lengthDecimals: 0}}, {dimensions: {linear: {unit: 'Centimeter', decimals: 1}}}, " +
      "{floorPlanCutPlane: {cutHeight: 1.2}}, {environment: {autoIntersect: false}}. Returns per-section {ok, value} | {error}. " +
      "dataSafety is read-only. In Teamwork, Project Preferences must be reserved.",
    input: Object.fromEntries(Object.entries(PreferenceSections).map(([k, v]) => [k, v.optional()])) as {
      [K in keyof typeof PreferenceSections]: z.ZodOptional<(typeof PreferenceSections)[K]>;
    },
    annotations: MODIFIES,
    handler: async (args, { ac }) => {
      const params = compact(args as Record<string, unknown>);
      if (Object.keys(params).length === 0) {
        throw new Error(
          `Pass at least one section to change: ${Object.keys(PreferenceSections).join(", ")} (dataSafety is read-only; unknown section names are ignored).`,
        );
      }
      const readOnly: string[] = [];
      if ((params["layouts"] as Json | undefined)?.["masterItemColor"] !== undefined) readOnly.push("layouts.masterItemColor");
      if ((params["imagingAndCalculation"] as Json | undefined)?.["progressSounds"] !== undefined) readOnly.push("imagingAndCalculation.progressSounds");
      if (readOnly.length > 0) {
        throw new Error(`${readOnly.join(" and ")} cannot be changed through the Archicad 26 API (Archicad keeps the old value). Remove them and retry; change them in Archicad's Options menu.`);
      }
      const response = await ac.addon<{ results?: Record<string, Json> }>("SetPreferences", params);
      return verifyPreferenceResults(params, response);
    },
  });

  defineTool(server, ctx, {
    name: "get_geo_location",
    title: "Get project location",
    description:
      "Returns the project location: latitude/longitude (degrees), altitude (m), northDirection (degrees, CCW from the +X axis; 90 = north " +
      "points to +Y/up on the plan), timeZoneMinutes, summerTime, dateTime and sun {azimuth, altitude} used for sun studies, surveyPoint " +
      "{position (project coordinates, m), visible, locked, projectOriginInSurveyCoordinates}, geoReference (IFC CRS / map conversion: " +
      "eastings, northings, orthogonalHeight...) and editable.",
    input: {},
    annotations: READ_ONLY,
    handler: async (_args, { ac }) => ac.addon("GetGeoLocation"),
  });

  defineTool(server, ctx, {
    name: "set_geo_location",
    title: "Set project location",
    description:
      "Changes the project location (Options > Project Preferences > Location / Survey Point). Give only the fields to change. Changing " +
      "northDirection or the survey point needs an unlocked Survey Point (pass unlockSurveyPoint: true to unlock temporarily). Date/location " +
      "changes also recompute the sun position. Returns the new location (as get_geo_location).",
    input: {
      latitude: z.number().min(-90).max(90).optional().describe("Degrees, + = North"),
      longitude: z.number().min(-180).max(180).optional().describe("Degrees, + = East"),
      altitude: z.number().optional().describe("Altitude above sea level (m)"),
      northDirection: z.number().optional().describe("Direction of geographic North on the plan in degrees, CCW from the +X axis (90 = up/+Y)"),
      timeZoneMinutes: z.number().int().min(-720).max(840).optional().describe("Time zone offset from UTC in minutes, e.g. 240 = UTC+4"),
      timeZoneOffset: z.number().int().optional().describe("Archicad's time zone disambiguation index (keep the value from get_geo_location)"),
      summerTime: z.boolean().optional().describe("Daylight saving time (+1 h)"),
      dateTime: z
        .object({
          year: z.number().int().min(1).max(9999).optional(),
          month: z.number().int().min(1).max(12).optional(),
          day: z.number().int().min(1).max(31).optional(),
          hour: z.number().int().min(0).max(23).optional(),
          minute: z.number().int().min(0).max(59).optional(),
          second: z.number().int().min(0).max(59).optional(),
        })
        .strict()
        .optional()
        .describe("Date and local time for the sun position (given parts only)"),
      displayUnits: z
        .object({
          north: z.enum(["DecimalDegree", "DegreeMinSec"]).optional(),
          altitude: z.enum(["Meter", "DecFoot"]).optional(),
          longitudeLatitude: z.enum(["DecimalDegree", "DegreeMinSec"]).optional(),
        })
        .strict()
        .optional()
        .describe("How Archicad displays these values (does not change the API units)"),
      surveyPoint: z
        .object({ x: z.number(), y: z.number(), z: z.number().optional() })
        .strict()
        .optional()
        .describe("Survey Point position in project coordinates (m); z optional"),
      surveyPointVisible: z.boolean().optional().describe("Show the Survey Point marker"),
      surveyPointLocked: z.boolean().optional().describe("Lock/unlock the Survey Point permanently"),
      unlockSurveyPoint: z.boolean().optional().describe("Temporarily unlock a locked Survey Point for this change (re-locked afterwards)"),
      geoReference: GeoReference.optional(),
    },
    annotations: MODIFIES,
    handler: async (args, { ac }) => {
      const params = compact(args);
      if (Object.keys(params).length === 0) throw new Error("Pass at least one field to change (see get_geo_location for the current values).");
      return verifyGeoLocation(params, await ac.addon("SetGeoLocation", params));
    },
  });

  defineTool(server, ctx, {
    name: "rebuild_model",
    title: "Rebuild / redraw",
    description:
      "Refreshes the current (front) window: 'Rebuild' (default, View > Refresh > Rebuild), 'Regenerate' (Rebuild & Regenerate: recomputes " +
      "all element geometry — use after library/parameter changes or when the display looks stale) or 'Redraw' (repaint only).",
    input: {
      mode: z.enum(["Rebuild", "Regenerate", "Redraw"]).optional().describe("Default Rebuild"),
    },
    annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: true },
    handler: async (args, { ac }) => ac.addon("RebuildModel", compact(args), { timeoutMs: projectTiming.longOperationMs }),
  });

  const editMenuTool = (name: "undo" | "redo", command: "Undo" | "Redo") =>
    defineTool(server, ctx, {
      name,
      title: command,
      description:
        `${command}s the last ${name === "undo" ? "" : "undone "}Archicad operation(s), exactly like Edit > ${command}. Every connector ` +
        "create/modify tool call is ONE undo step (its name ends with '(Claude)'), so steps: 1 reverts one whole tool call. The Archicad 26 " +
        `API has no ${name} function: this triggers Archicad's Edit menu item (macOS). Returns {performed, requested, ${name === "undo" ? "undone" : "redone"} ` +
        `(menu titles, e.g. '${command} Create walls (Claude)'), next}. dryRun: true only reports what would be ${name === "undo" ? "undone" : "redone"} next. ` +
        "Note: undo also reverts changes made by the user or other tools.",
      input: {
        steps: z.number().int().min(1).max(50).optional().describe("Number of steps (default 1)"),
        dryRun: z.boolean().optional().describe(`Only report the current Edit > ${command} menu title/enabled state and the last run`),
        force: z.boolean().optional().describe("Perform even if Archicad reports the menu item as disabled (use only if the state looks stale)"),
      },
      annotations: DESTRUCTIVE,
      handler: async ({ steps, dryRun, force }, { ac }) => {
        if (dryRun) return ac.addon(command, { dryRun: true });
        const scheduled = await ac.addon<{ runId?: number; steps?: number; menuItem?: unknown }>(command, compact({ steps, force }));
        const n = steps ?? 1;
        // The add-on performs one step per Archicad main-loop turn: poll until the run has finished.
        const deadline = Date.now() + projectTiming.undoSettleMs + (projectTiming.undoSettlePerStepMs * 4 + projectTiming.undoTitleWaitMs) * n;
        await sleep(projectTiming.undoSettleMs + projectTiming.undoSettlePerStepMs * n);
        let state = await ac.addon<{ menuItem?: unknown; lastRun?: Json }>(command, { dryRun: true });
        while (!(state.lastRun?.["runId"] === scheduled.runId && state.lastRun?.["finished"] === true) && Date.now() < deadline) {
          await sleep(projectTiming.undoSettlePerStepMs);
          state = await ac.addon<{ menuItem?: unknown; lastRun?: Json }>(command, { dryRun: true });
        }
        const lastRun = state.lastRun ?? {};
        if (lastRun["runId"] === scheduled.runId && lastRun["finished"] === true) {
          return compact({
            performed: lastRun["performed"],
            requested: n,
            [name === "undo" ? "undone" : "redone"]: lastRun["titles"],
            stoppedReason: lastRun["stoppedReason"],
            next: state.menuItem,
          });
        }
        return {
          status: "pending",
          scheduled,
          note: `Archicad has not run the ${command} yet (it may be busy). Call ${name} with dryRun: true in a moment to see lastRun.`,
        };
      },
    });
  editMenuTool("undo", "Undo");
  editMenuTool("redo", "Redo");
}
