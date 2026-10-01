/**
 * Objects, lamps, libraries, library parts and GDL.
 * Add-on side: addon/Src/Commands/ObjectsLibrary.cpp (field names match 1:1).
 */

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import { CREATES, defineTool, DESTRUCTIVE, MODIFIES, READ_ONLY, type ToolContext } from "./define.js";
import { createElements } from "./elements.js";
import { AttrRef, CommonElementFields, ElementRefs, Guid, guidOf, LibPartRef, Point2D } from "./schemas.js";
import { GDL_CHEAT_SHEET, SUBTYPE_KEYWORDS } from "./objects-library-gdl.js";

// ---------------------------------------------------------------------------------------------
// Shared schemas
// ---------------------------------------------------------------------------------------------

export const LIB_PART_TYPES = [
  "Object",
  "Door",
  "Window",
  "Lamp",
  "Zone",
  "Label",
  "Skylight",
  "Macro",
  "Picture",
  "PlanSign",
  "ListScheme",
  "Property",
  "OpeningSymbol",
  "Spec",
] as const;

export const LibPartType = z.enum(LIB_PART_TYPES).describe("Library part type");

/** One GDL parameter value. Nested arrays ([[..],[..]]) are 2-dimensional GDL arrays. */
export const GdlValue = z.union([
  z.number(),
  z.string(),
  z.boolean(),
  z.array(z.number()),
  z.array(z.string()),
  z.array(z.array(z.number())),
  z.array(z.array(z.string())),
  z.array(z.object({ values: z.array(z.union([z.number(), z.string()])) })),
]);

export const GdlValues = z
  .record(GdlValue)
  .refine((o) => Object.keys(o).every((k) => k.trim().length > 0), { message: "GDL parameter names must not be empty" })
  .describe(
    "GDL parameter values by parameter NAME (the variable name like 'gs_cont_pen', not the localized description; list them with " +
      "get_gdl_parameters / get_library_part_details). Length in meters, Angle in DEGREES, Boolean as true/false, Integer/RealNum as numbers, " +
      "String as text. Surface / BuildingMaterial / LineType / Fill / Profile parameters accept the attribute NAME (localized) or index. " +
      "Numeric parameters with a value list also accept the value's description text. Arrays: [..] (1-D) or [[..],[..]] (2-D, rows). " +
      "'A' and 'B' are the X/Y sizes. Changes run the part's parameter script, exactly like the settings dialog.",
  );

type GdlValueT = z.infer<typeof GdlValue>;

/** Converts nested arrays to the add-on's row format [{values: [...]}]. */
export function normalizeGdlValue(value: GdlValueT): unknown {
  if (Array.isArray(value) && value.length > 0 && Array.isArray(value[0])) {
    return (value as unknown[][]).map((row) => ({ values: row }));
  }
  return value;
}

export function normalizeGdlValues(values: Record<string, GdlValueT> | undefined): Record<string, unknown> | undefined {
  if (values === undefined) return undefined;
  const out: Record<string, unknown> = {};
  for (const [k, v] of Object.entries(values)) out[k] = normalizeGdlValue(v);
  return out;
}

type LibPartRefT = z.infer<typeof LibPartRef>;

/** Library part reference as an object ({name} | {index} | {guid}) — the add-on's array form. */
export function libPartRefObject(ref: LibPartRefT): Record<string, unknown> {
  if (typeof ref === "string") return { name: ref };
  if (typeof ref === "number") return { index: ref };
  return ref as Record<string, unknown>;
}

const Pen = z.number().int().min(1).max(255);

const StoryVisibility = z
  .union([
    z.enum(["HomeOnly", "AllRelevant", "AllStories"]),
    z.object({
      home: z.boolean().optional().describe("Show on the home story"),
      allAbove: z.boolean().optional().describe("Show on all stories above"),
      allBelow: z.boolean().optional().describe("Show on all stories below"),
      above: z.number().int().min(0).optional().describe("Show on this many stories above"),
      below: z.number().int().min(0).optional().describe("Show on this many stories below"),
    }),
  ])
  .describe(
    "Stories the element is shown on: 'HomeOnly', 'AllRelevant' (every story its 3D body intersects), 'AllStories', or a custom " +
      "{home, allAbove, allBelow, above, below}. Default: tool setting",
  );

/** Fields shared by objects and lamps (create and modify). */
export const ObjectFields = {
  libraryPart: LibPartRef.describe(
    "Library part to place: exact LOCALIZED name (this Archicad may be Russian), index, or {guid} — find it with search_library_parts, " +
      "or use the guid returned by create_library_part",
  ),
  position: Point2D.describe("Insertion point = the part's local origin (for most parts a bottom-left corner of the A x B box), meters"),
  elevation: z.number().optional().describe("Base elevation above the home story level in m (default: tool setting, usually 0)"),
  angle: z.number().optional().describe("Rotation in degrees, counter-clockwise, around the insertion point (default 0)"),
  mirrored: z.boolean().optional().describe("Mirror the part (reflected about its local Y axis)"),
  sizeA: z.number().positive().optional().describe("GDL A = size along the part's local X axis (width) in m; default: the part's default"),
  sizeB: z.number().positive().optional().describe("GDL B = size along the part's local Y axis (depth) in m; default: the part's default"),
  height: z.number().positive().optional().describe("GDL ZZYZX = 3D height in m (only for parts having a ZZYZX parameter)"),
  params: GdlValues.optional(),
  pen: Pen.optional().describe("Override pen of all 2D/3D lines (sets useObjectPens=false unless given)"),
  useObjectPens: z.boolean().optional().describe("true = use the part's own pens (no override)"),
  lineType: AttrRef.optional().describe("Override line type (name/index; sets useObjectLineTypes=false unless given)"),
  useObjectLineTypes: z.boolean().optional().describe("true = use the part's own line types"),
  overrideSurface: z
    .union([AttrRef, z.literal(false)])
    .optional()
    .describe("One surface (name/index) for the whole 3D body, or false = use the part's own surfaces"),
  sectionFill: AttrRef.optional().describe("Override cut fill (name/index) in sections"),
  sectionFillPen: Pen.optional().describe("Override cut fill foreground pen"),
  sectionBackgroundPen: z.number().int().min(0).max(255).optional().describe("Override cut fill background pen (0 = transparent)"),
  sectionContourPen: Pen.optional().describe("Override cut contour pen"),
  useObjectSectionAttributes: z.boolean().optional().describe("true = the part's own section attributes (set automatically to false when a section override is given)"),
  showOnStories: StoryVisibility.optional(),
  ...CommonElementFields,
};

export const LampFields = {
  ...ObjectFields,
  libraryPart: LibPartRef.describe(
    "Lamp library part (type 'Lamp'): exact LOCALIZED name, index or {guid} — find it with search_library_parts {type: 'Lamp'}",
  ),
  lightOn: z.boolean().optional().describe("Switch the light on/off (the part's Light Switch parameter)"),
  lightColor: z
    .object({
      red: z.number().min(0).max(1),
      green: z.number().min(0).max(1),
      blue: z.number().min(0).max(1),
    })
    .optional()
    .describe("Light color, components 0..1 (the part's RGB color parameters)"),
  lightIntensity: z.number().min(0).optional().describe("Light intensity (the part's Intensity parameter, usually 0-100 %)"),
};

export const ObjectSpec = z.object(ObjectFields);
export const LampSpec = z.object(LampFields);

function withNormalizedParams<T extends { params?: Record<string, GdlValueT> }>(spec: T): Record<string, unknown> {
  const out: Record<string, unknown> = { ...spec };
  if (spec.params !== undefined) out["params"] = normalizeGdlValues(spec.params);
  return out;
}

const SCRIPT_KEYS = [
  "masterScript",
  "script2D",
  "script3D",
  "parameterScript",
  "interfaceScript",
  "propertiesScript",
  "forwardMigrationScript",
  "backwardMigrationScript",
] as const;

const GDL_PARAM_TYPES = [
  "Length",
  "Angle",
  "RealNum",
  "Integer",
  "Boolean",
  "String",
  "Surface",
  "BuildingMaterial",
  "Pen",
  "LineType",
  "Fill",
  "Profile",
  "LightSwitch",
  "ColorRGB",
  "Intensity",
  "Separator",
  "Title",
] as const;

const GdlParamDef = z
  .object({
    name: z
      .string()
      .regex(/^[A-Za-z_~][A-Za-z0-9_~]{0,30}$/, "GDL names: 1-31 ASCII letters/digits/_ , not starting with a digit")
      .optional()
      .describe("Variable name used in the scripts (ASCII, max 31 chars). Required except for Separator/Title. Do not declare A or B"),
    type: z.enum(GDL_PARAM_TYPES).describe("Parameter type"),
    description: z.string().optional().describe("Label shown in the settings dialog (any language; default: the name)"),
    value: z
      .union([
        z.number(),
        z.string(),
        z.boolean(),
        z.array(z.number()),
        z.array(z.string()),
        z.array(z.array(z.number())),
        z.array(z.array(z.string())),
      ])
      .optional()
      .describe(
        "Default value: Length in m, Angle in degrees, Boolean true/false, String text, Surface/BuildingMaterial/LineType/Fill/Profile as " +
          "attribute name or index, Pen index. An array value makes an array parameter ([..] 1-D, [[..],[..]] 2-D)",
      ),
    hidden: z.boolean().optional().describe("Hide from the settings dialog"),
    bold: z.boolean().optional().describe("Show the name in bold"),
    child: z.boolean().optional().describe("Indent under the previous parameter"),
    unique: z.boolean().optional().describe("Value is not transferred when swapping library parts"),
    arrayDims: z
      .object({ dim1: z.number().int().min(1), dim2: z.number().int().min(0) })
      .optional()
      .describe("Array size when no array value is given (dim2 = 0 for a 1-D array); cells default to 0 / ''"),
  })
  .describe("GDL parameter definition");

// ---------------------------------------------------------------------------------------------
// Client-side filters
// ---------------------------------------------------------------------------------------------

interface GdlParameterItem {
  name?: string;
  description?: string;
  [k: string]: unknown;
}

interface GdlParametersResponse {
  elements?: Array<{
    parameters?: GdlParameterItem[];
    parameterCount?: number;
    libraryPart?: { index?: number };
    [k: string]: unknown;
  }>;
  [k: string]: unknown;
}

interface LibPartDetailsResponse {
  libraryParts?: Array<{ index?: number; parameters?: GdlParameterItem[]; [k: string]: unknown }>;
}

/**
 * Placed elements' parameters may come without descriptions (older add-on builds): fills them in place from the
 * library parts' default parameters (one GetLibraryPartDetails call per 20 distinct library parts).
 */
export async function fillMissingDescriptions(
  res: GdlParametersResponse,
  fetchDetails: (indices: number[]) => Promise<LibPartDetailsResponse>,
): Promise<void> {
  const needed = new Set<number>();
  for (const e of res.elements ?? []) {
    const index = e.libraryPart?.index;
    if (typeof index === "number" && index > 0 && (e.parameters ?? []).some((p) => !p.description)) needed.add(index);
  }
  if (needed.size === 0) return;
  const byPart = new Map<number, Map<string, string>>();
  const all = [...needed];
  for (let i = 0; i < all.length; i += 20) {
    let details: LibPartDetailsResponse;
    try {
      details = await fetchDetails(all.slice(i, i + 20));
    } catch {
      return; // descriptions are a convenience; never fail the read because of them
    }
    for (const lp of details.libraryParts ?? []) {
      if (typeof lp.index !== "number") continue;
      const names = new Map<string, string>();
      for (const p of lp.parameters ?? []) if (p.name && p.description) names.set(p.name.toLowerCase(), p.description);
      byPart.set(lp.index, names);
    }
  }
  for (const e of res.elements ?? []) {
    const names = typeof e.libraryPart?.index === "number" ? byPart.get(e.libraryPart.index) : undefined;
    if (!names) continue;
    for (const p of e.parameters ?? []) {
      if (!p.description && p.name) {
        const d = names.get(p.name.toLowerCase());
        if (d) p.description = d;
      }
    }
  }
}

/** Keeps the parameters whose name or description contains `needle` (lowercase). */
export function filterGdlParameters(res: GdlParametersResponse, needle: string): GdlParametersResponse {
  const matches = (p: GdlParameterItem) =>
    (p.name ?? "").toLowerCase().includes(needle) || (p.description ?? "").toLowerCase().includes(needle);
  return {
    ...res,
    elements: (res.elements ?? []).map((e) => {
      if (!Array.isArray(e.parameters)) return e;
      const parameters = e.parameters.filter(matches);
      return { ...e, parameters, parameterCount: parameters.length };
    }),
  };
}

interface SubtypeItem {
  name?: string;
  path?: string;
  [k: string]: unknown;
}

interface SubtypesResponse {
  subtypes?: SubtypeItem[];
  [k: string]: unknown;
}

/** Case-insensitive filter of subtypes by name/path, then offset/limit. */
export function filterSubtypes(res: SubtypesResponse, needle: string, offset: number, limit: number) {
  const hits = (res.subtypes ?? []).filter(
    (s) => (s.name ?? "").toLowerCase().includes(needle) || (s.path ?? "").toLowerCase().includes(needle),
  );
  const subtypes = hits.slice(offset, offset + limit);
  return { subtypes, total: hits.length, offset, hasMore: offset + subtypes.length < hits.length };
}

// ---------------------------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------------------------

export function registerObjectLibraryTools(server: McpServer, ctx: ToolContext): void {
  // --- Placement ------------------------------------------------------------------------------

  defineTool(server, ctx, {
    name: "create_objects",
    title: "Place objects (library parts)",
    description:
      "Places GDL objects (furniture, equipment, fixtures, custom parts from create_library_part, ...) in one undo step. Each item needs a " +
      "libraryPart (Object type — names are LOCALIZED, so find them first with search_library_parts {query, type: 'Object'}) and a position " +
      "(meters). Optional: elevation above the home story, angle (degrees CCW), mirrored, sizes sizeA (X) / sizeB (Y) / height (ZZYZX), any GDL " +
      "params by name (see get_library_part_details), pen/line/surface overrides, story visibility, layer, storyIndex. Returns " +
      "[{guid, type} | {error}] in input order. Lamps: create_lamps. Doors/windows: the opening tools. Later changes: modify_elements " +
      "(same fields), set_gdl_parameters, change_library_part.",
    input: {
      objects: z.array(ObjectSpec).min(1).max(500).describe("Objects to place"),
      undoName: z.string().optional().describe("Name of the undo step"),
    },
    annotations: CREATES,
    handler: async ({ objects, undoName }, c) =>
      createElements(
        c,
        objects.map((o) => ({ type: "Object", ...withNormalizedParams(o) })),
        undoName ?? "Create objects (Claude)",
      ),
  });

  defineTool(server, ctx, {
    name: "create_lamps",
    title: "Place lamps",
    description:
      "Places lamps (light-emitting GDL library parts of type 'Lamp': ceiling lights, spots, floor lamps, ...) in one undo step. Same fields " +
      "as create_objects plus lightOn, lightColor {red, green, blue} (0..1) and lightIntensity. Find lamp names with search_library_parts " +
      "{type: 'Lamp'} (names are localized). Typical ceiling light: elevation = ceiling height minus the lamp height. Returns [{guid, type} | {error}].",
    input: {
      lamps: z.array(LampSpec).min(1).max(500).describe("Lamps to place"),
      undoName: z.string().optional().describe("Name of the undo step"),
    },
    annotations: CREATES,
    handler: async ({ lamps, undoName }, c) =>
      createElements(
        c,
        lamps.map((l) => ({ type: "Lamp", ...withNormalizedParams(l) })),
        undoName ?? "Create lamps (Claude)",
      ),
  });

  // --- Library part discovery ------------------------------------------------------------------

  defineTool(server, ctx, {
    name: "search_library_parts",
    title: "Search library parts",
    description:
      "Finds library parts (objects, doors, windows, lamps, zone stamps, labels, skylights, macros ...) in the loaded libraries. Names are " +
      "LOCALIZED — on a Russian Archicad search Russian words (e.g. 'стол' table, 'стул' chair, 'дверь' door, 'окно' window, 'светильник' lamp, " +
      "'кровать' bed, 'шкаф' cabinet, 'диван' sofa, 'унитаз' WC, 'раковина' sink, 'дерево' tree). The query is a case-insensitive substring; " +
      "with several words all must match the name or file name. Exact and prefix matches come first. Filter by type, by subtype (category: a " +
      "keyword like 'Furnishing', 'Plant', 'Light' or a template from get_library_part_subtypes), or embeddedOnly (parts created with " +
      "create_library_part). Returns {libraryParts: [{index, name, guid, type, fileName, subtype}], total, hasMore}. Use the name or " +
      "{guid} as libraryPart in create_objects / create_lamps / change_library_part.",
    input: {
      query: z.string().optional().describe("Substring(s) of the name / file name; omit to list everything matching the filters"),
      type: z.union([LibPartType, z.array(LibPartType).min(1)]).optional().describe("Only these library part types"),
      subtypeOf: z
        .union([z.string(), z.object({ guid: z.string() }), z.object({ index: z.number().int() }), z.object({ name: z.string() })])
        .optional()
        .describe(`Only descendants of this subtype: a keyword (${SUBTYPE_KEYWORDS.join(", ")}) or a template name/{guid} from get_library_part_subtypes`),
      placeableOnly: z.boolean().optional().describe("Only parts that can be placed (default true; false also lists macros and templates)"),
      embeddedOnly: z.boolean().optional().describe("Only parts stored in the project's embedded library"),
      offset: z.number().int().min(0).optional().describe("Skip this many results (default 0)"),
      limit: z.number().int().min(1).max(2000).optional().describe("Maximum results (default 100)"),
    },
    annotations: READ_ONLY,
    handler: async (args, { ac }) => ac.addon("SearchLibraryParts", args),
  });

  defineTool(server, ctx, {
    name: "get_library_part_subtypes",
    title: "Library part subtypes (categories)",
    description:
      "Lists the library part subtype tree (categories such as Model Element > Building Element > Furnishing > Chair — names localized) with " +
      "each subtype's path, parent, type and number of placeable parts directly under it. Use it to browse the library by category, then " +
      "search_library_parts {subtypeOf: {guid}} to list the parts of a category, or pass a subtype to create_library_part.",
    input: {
      query: z.string().optional().describe("Case-insensitive substring of the subtype name or its path"),
      type: z.union([LibPartType, z.array(LibPartType).min(1)]).optional().describe("Only subtypes of these library part types"),
      offset: z.number().int().min(0).optional().describe("Skip this many results"),
      limit: z.number().int().min(1).max(2000).optional().describe("Maximum results (default 300)"),
    },
    annotations: READ_ONLY,
    handler: async ({ query, offset, limit, ...rest }, { ac }) => {
      const needle = query?.trim().toLowerCase();
      if (!needle) return ac.addon("GetLibraryPartSubtypes", { ...rest, ...(offset !== undefined ? { offset } : {}), ...(limit !== undefined ? { limit } : {}) });
      // Case-insensitive filtering of the name / path is done here on the full list, then paginated.
      const all = await ac.addon<SubtypesResponse>("GetLibraryPartSubtypes", { ...rest, limit: 2000 });
      return filterSubtypes(all, needle, offset ?? 0, limit ?? 300);
    },
  });

  defineTool(server, ctx, {
    name: "get_library_part_details",
    title: "Library part details",
    description:
      "Everything about library parts before placing them: identity (name, guid, index, type, file), file location and containing library, " +
      "subtype/ancestry, creator tool, sections, comment/keywords, default sizes (sizeA, sizeB, height) and the DEFAULT GDL parameters " +
      "[{name, type, description, value, valueDescription?, hidden?, arrayDims?, valueList?}] — the parameter names are what create_objects " +
      "params and set_gdl_parameters expect. includeValueLists adds the allowed values/ranges from the parameter script (slower: use with " +
      "parameterNames).",
    input: {
      libraryParts: z.array(LibPartRef).min(1).max(20).describe("Library parts (name, index or {guid})"),
      includeParameters: z.boolean().optional().describe("Include the default GDL parameters (default true)"),
      includeHidden: z.boolean().optional().describe("Also include hidden parameters (default false)"),
      includeValueLists: z.boolean().optional().describe("Add allowed values / ranges of each returned parameter (default false)"),
      parameterNames: z.array(z.string()).optional().describe("Only these parameters (exact names, case-insensitive)"),
    },
    annotations: READ_ONLY,
    handler: async ({ libraryParts, ...rest }, { ac }) =>
      ac.addon("GetLibraryPartDetails", { ...rest, libraryParts: libraryParts.map(libPartRefObject) }),
  });

  defineTool(server, ctx, {
    name: "get_library_part_scripts",
    title: "Library part GDL scripts",
    description:
      "Returns the GDL source of a library part: masterScript, script2D, script3D, parameterScript, interfaceScript, propertiesScript (default), " +
      "and on request forwardMigrationScript, backwardMigrationScript, comment, keywords. Great for learning how a standard part works or " +
      "as a starting point for create_library_part. Encrypted parts return errors. Long scripts are cut at maxLength characters.",
    input: {
      libraryPart: LibPartRef,
      scripts: z
        .array(z.enum([...SCRIPT_KEYS, "comment", "keywords"]))
        .min(1)
        .optional()
        .describe("Which scripts (default: the 6 main scripts)"),
      maxLength: z.number().int().min(100).max(170000).optional().describe("Max characters per script (default 60000)"),
    },
    annotations: READ_ONLY,
    handler: async (args, { ac }) => ac.addon("GetLibraryPartScripts", args),
  });

  // --- Library part creation ---------------------------------------------------------------------

  defineTool(server, ctx, {
    name: "create_library_part",
    title: "Create a GDL library part",
    description:
      "Creates a custom GDL library part (object, lamp, door, window, skylight, label, zone stamp) in the project's EMBEDDED library from " +
      "GDL scripts and a parameter list, then returns its {libraryPart: {name, guid, index}} so create_objects / create_lamps can place it. " +
      "Use it for anything the standard library lacks (custom furniture, built-ins, fixtures, signage, parametric equipment). " +
      "overwrite: true replaces an existing embedded part of the same name (placed instances keep their link and update). " +
      "Defaults: type 'Object' with subtype ModelElement; a ZZYZX (height) Length parameter is added for objects/lamps; when script2D is " +
      "omitted for objects/lamps it is 'PROJECT2 3, 270, 2' (plan symbol = top view of the 3D model). Verify with get_library_part_scripts, " +
      "then place it and look at it (3D view / get_element_details); GDL errors show up as missing geometry.\n\n" +
      GDL_CHEAT_SHEET,
    input: {
      name: z.string().min(1).max(199).describe("Library part name (becomes the .gsm file name; no / \\ : * ? \" < > |). Must be unique in the loaded libraries"),
      type: z
        .enum(["Object", "Lamp", "Window", "Door", "Skylight", "Label", "Zone"])
        .optional()
        .describe("Library part type (default Object, or implied by subtype)"),
      subtype: z
        .union([z.string(), z.object({ guid: z.string() }), z.object({ name: z.string() }), z.object({ index: z.number().int() })])
        .optional()
        .describe(
          `Parent subtype: a keyword (${SUBTYPE_KEYWORDS.join(", ")}) or a template library part name/{guid} from get_library_part_subtypes. ` +
            "Default by type: Object → ModelElement, Lamp → Light, Window → WindowWall, Door → DoorWall, Skylight, Label, Zone → ZoneStamp",
        ),
      scripts: z
        .object({
          masterScript: z.string().optional().describe("Runs before every other script: shared calculations"),
          script2D: z.string().optional().describe("Floor plan symbol (default for objects: PROJECT2 3, 270, 2)"),
          script3D: z.string().optional().describe("3D model"),
          parameterScript: z.string().optional().describe("VALUES / RANGE / LOCK / PARAMETERS logic"),
          interfaceScript: z.string().optional().describe("Custom settings-dialog page (UI_ commands)"),
          propertiesScript: z.string().optional().describe("Listing / component properties"),
          forwardMigrationScript: z.string().optional(),
          backwardMigrationScript: z.string().optional(),
        })
        .optional()
        .describe("GDL scripts (plain text, newline separated)"),
      parameters: z.array(GdlParamDef).max(500).optional().describe("Parameter list in dialog order (A, B are implicit — use a / b)"),
      a: z.number().positive().optional().describe("Default A (X size) in m (default 1)"),
      b: z.number().positive().optional().describe("Default B (Y size) in m (default 1)"),
      height: z.number().positive().optional().describe("Default ZZYZX (height) in m for the automatic height parameter (default 1)"),
      addHeightParameter: z.boolean().optional().describe("Add a ZZYZX Length parameter when not declared (default true for Object/Lamp)"),
      autoHotspots: z.boolean().optional().describe("Automatic bounding-box hotspots (default true; set false when the 2D script places HOTSPOT2s)"),
      fixSize: z.boolean().optional().describe("Size cannot be stretched (default false)"),
      placeable: z.boolean().optional().describe("Can be placed (default true; false = macro for CALL)"),
      template: z.boolean().optional().describe("Can be used as a subtype of other parts (default false)"),
      comment: z.string().optional().describe("Description shown in the library browser"),
      keywords: z.string().optional().describe("Search keywords"),
      author: z.string().optional().describe("Author / copyright"),
      folder: z.string().optional().describe("Sub-folder in the embedded library (default 'Claude Objects'; '' = root)"),
      overwrite: z.boolean().optional().describe("Replace an existing embedded library part with the same name (default false)"),
    },
    annotations: CREATES,
    handler: async ({ parameters, ...rest }, { ac }) => {
      const params: Record<string, unknown> = { ...rest };
      if (parameters !== undefined) {
        params["parameters"] = parameters.map((p) => {
          const out: Record<string, unknown> = { ...p };
          if (p.value !== undefined) out["value"] = normalizeGdlValue(p.value);
          return out;
        });
      }
      return ac.addon("CreateLibraryPart", params);
    },
  });

  // --- Libraries ------------------------------------------------------------------------------------

  defineTool(server, ctx, {
    name: "get_libraries",
    title: "Loaded libraries",
    description:
      "Lists the libraries loaded in the project (Library Manager): name, path, type (Local, Embedded, BuiltIn, Server, Url ...), available, " +
      "readOnly, plus the total number of library parts.",
    input: {},
    annotations: READ_ONLY,
    handler: async (_a, { ac }) => ac.addon("GetLibraries"),
  });

  defineTool(server, ctx, {
    name: "add_libraries",
    title: "Add libraries",
    description:
      "Adds local libraries to the project and loads them: absolute folder paths (or .lcf library container files) on this Mac; '~/' is " +
      "expanded. Already loaded paths are reported as alreadyLoaded. NOT undoable. Returns per-path results and the new library list.",
    input: {
      paths: z.array(z.string().min(1)).min(1).max(50).describe("Absolute folder or .lcf paths, e.g. '/Users/me/Documents/My Objects'"),
    },
    annotations: MODIFIES,
    handler: async (args, { ac }) => ac.addon("AddLibraries", args),
  });

  defineTool(server, ctx, {
    name: "remove_libraries",
    title: "Remove libraries",
    description:
      "Removes libraries from the project by path or name (see get_libraries). The embedded and built-in libraries cannot be removed. " +
      "Placed elements using parts of a removed library become 'missing objects'. NOT undoable.",
    input: {
      libraries: z.array(z.string().min(1)).min(1).max(50).describe("Library paths or names exactly as listed by get_libraries"),
    },
    annotations: DESTRUCTIVE,
    handler: async (args, { ac }) => ac.addon("RemoveLibraries", args),
  });

  defineTool(server, ctx, {
    name: "reload_libraries",
    title: "Reload libraries",
    description: "Reloads all loaded libraries (picks up library part files changed on disk, fixes stale 'missing' parts).",
    input: {},
    annotations: MODIFIES,
    handler: async (_a, { ac }) => ac.addon("ReloadLibraries"),
  });

  // --- GDL parameters of placed elements -----------------------------------------------------------------

  defineTool(server, ctx, {
    name: "get_gdl_parameters",
    title: "Get GDL parameters of elements",
    description:
      "Returns the GDL parameters of placed library-part based elements (objects, lamps, doors, windows, skylights, zones, symbol labels, ...): " +
      "per element {guid, type, libraryPart, parameters: [{name, type, description, value, valueDescription?, hidden?, disabled?, arrayDims?, " +
      "valueList?}]}. Lengths in m, angles in degrees. Filter with names (exact) or search (substring of name or localized description). " +
      "includeValueLists adds allowed values/ranges and 'locked' state from the parameter script. Hidden parameters are skipped unless " +
      "includeHidden or named explicitly.",
    input: {
      elements: ElementRefs.max(200),
      names: z.array(z.string()).optional().describe("Only these parameters (exact variable names, case-insensitive)"),
      search: z.string().optional().describe("Only parameters whose name or description contains this text (case-insensitive)"),
      includeHidden: z.boolean().optional().describe("Include hidden parameters (default false)"),
      includeValueLists: z.boolean().optional().describe("Add value lists / ranges / locked flags (default false; slower)"),
    },
    annotations: READ_ONLY,
    handler: async ({ elements, search, ...rest }, { ac }) => {
      // 'search' is filtered here (case-insensitive substring of name or description), not in the add-on.
      const res = await ac.addon<GdlParametersResponse>("GetGdlParameters", { ...rest, elements: elements.map(guidOf) });
      await fillMissingDescriptions(res, (indices) =>
        ac.addon<LibPartDetailsResponse>("GetLibraryPartDetails", {
          libraryParts: indices.map((index) => ({ index })),
          includeHidden: true,
        }),
      );
      const needle = search?.trim().toLowerCase();
      if (!needle) return res;
      return filterGdlParameters(res, needle);
    },
  });

  defineTool(server, ctx, {
    name: "set_gdl_parameters",
    title: "Set GDL parameters of elements",
    description:
      "Changes GDL parameters of placed objects, lamps, doors, windows, skylights, zones or labels in one undo step, running each part's " +
      "parameter script like the settings dialog (dependent parameters update, invalid values are corrected or rejected). Give per-element " +
      "params, and/or top-level params applied to every listed element (per-element values win). Changing A/B also resizes the element. " +
      "Returns [{guid, parameters: resulting values} | {error}]. Parameter names: get_gdl_parameters.",
    input: {
      elements: z
        .array(z.object({ guid: Guid, params: GdlValues.optional() }))
        .min(1)
        .max(1000)
        .describe("Elements to change: {guid, params?}"),
      params: GdlValues.optional().describe("Values applied to every element (merged under each element's own params)"),
      undoName: z.string().optional().describe("Name of the undo step"),
    },
    annotations: MODIFIES,
    handler: async ({ elements, params, undoName }, { ac }) => {
      const items = elements.map((e) => {
        const merged = { ...(params ?? {}), ...(e.params ?? {}) };
        if (Object.keys(merged).length === 0) {
          throw new Error(`No parameters given for element ${e.guid}: pass params (top-level or per element).`);
        }
        return { guid: e.guid, params: normalizeGdlValues(merged) };
      });
      return ac.addon("SetGdlParameters", { elements: items, ...(undoName ? { undoName } : {}) });
    },
  });

  defineTool(server, ctx, {
    name: "change_library_part",
    title: "Change library part of elements",
    description:
      "Swaps the library part of placed objects, lamps, doors, windows, skylights, zones or symbol labels (e.g. replace a chair model, change a " +
      "door type) in one undo step, keeping position/orientation. The new part must have the same type (Object for objects, Door for doors ...). " +
      "keepParameters (default true) copies values of same-named, same-typed, visible, non-unique GDL parameters like Archicad does; keepSize " +
      "(default true for doors/windows/skylights, false otherwise) keeps A/B. Extra params / sizeA / sizeB / height are applied afterwards. " +
      "A top-level libraryPart applies to every element without its own. Returns [{guid, type, libraryPart, carriedOverParameters} | {error}].",
    input: {
      elements: z
        .array(
          z.object({
            guid: Guid,
            libraryPart: LibPartRef.optional().describe("New library part for this element (default: the top-level libraryPart)"),
            keepParameters: z.boolean().optional(),
            keepSize: z.boolean().optional(),
            params: GdlValues.optional(),
            sizeA: z.number().positive().optional().describe("Objects/lamps: new A in m"),
            sizeB: z.number().positive().optional().describe("Objects/lamps: new B in m"),
            height: z.number().positive().optional().describe("Objects/lamps: new ZZYZX in m"),
          }),
        )
        .min(1)
        .max(500),
      libraryPart: LibPartRef.optional().describe("New library part for all listed elements (localized name, index or {guid})"),
      keepParameters: z.boolean().optional().describe("Default for all elements: copy matching parameter values (default true)"),
      keepSize: z.boolean().optional().describe("Default for all elements: keep A/B sizes"),
      undoName: z.string().optional().describe("Name of the undo step"),
    },
    annotations: MODIFIES,
    handler: async ({ elements, libraryPart, keepParameters, keepSize, undoName }, { ac }) => {
      const items = elements.map((e) => {
        const lp = e.libraryPart ?? libraryPart;
        if (lp === undefined) {
          throw new Error(`No libraryPart for element ${e.guid}: give it per element or at the top level (find parts with search_library_parts).`);
        }
        const item: Record<string, unknown> = { ...e, libraryPart: lp };
        if (item["keepParameters"] === undefined && keepParameters !== undefined) item["keepParameters"] = keepParameters;
        if (item["keepSize"] === undefined && keepSize !== undefined) item["keepSize"] = keepSize;
        if (e.params !== undefined) item["params"] = normalizeGdlValues(e.params);
        return item;
      });
      return ac.addon("ChangeLibraryPart", { elements: items, ...(undoName ? { undoName } : {}) });
    },
  });
}
