/**
 * Attribute tools: read every attribute type, create layers / layer combinations / composites /
 * building materials / surfaces / fills / line types / zone categories / profiles, copy any
 * attribute, modify and delete attributes, change pens, apply layer combinations and set layer states.
 *
 * Backed by the add-on commands in addon/Src/Commands/Attributes.cpp (GetAttributes, CreateAttributes,
 * ModifyAttributes, DeleteAttributes, ModifyPens, ApplyLayerCombination, SetLayerStates).
 * Field names are identical on input and output.
 */

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import { CREATES, defineTool, DESTRUCTIVE, MODIFIES, READ_ONLY, type ToolContext } from "./define.js";
import { AttrRef } from "./schemas.js";
import {
  AttributeType,
  BuildingMaterialFields,
  CompositeFields,
  CopyableAttributeType,
  CreateCommon,
  DeletableAttributeType,
  FillFields,
  IfExists,
  LayerCombinationFields,
  LayerFields,
  LayerStateItem,
  LineTypeFields,
  ModifyItem,
  PenSpec,
  ProfileFields,
  refObject,
  SurfaceFields,
  ZoneCategoryFields,
} from "./attributes-schemas.js";

const RESULT_NOTE =
  "Runs in one undo step. Returns {results: [{index, name, guid, created: true} | {..., existed: true} | {error}]} in input order; " +
  "one failing item does not stop the others. Names are localized (Russian Archicad): look existing attributes up with get_attributes first.";

const ifExistsInput = IfExists.optional().describe(
  "Name collision handling for all items: 'error' (default), 'skip' (reuse the existing attribute), 'update' (apply the fields to it)",
);

/** Attribute types that have folders in Archicad 26 (others fail or even trip assertions in Archicad). */
export const FOLDER_TYPES = new Set(["Layer", "Composite", "Profile", "Surface", "BuildingMaterial"]);

function checkFolders(type: string, items: Record<string, unknown>[]): void {
  if (FOLDER_TYPES.has(type)) return;
  const bad = items.findIndex((i) => i["folder"] !== undefined);
  if (bad >= 0) {
    throw new Error(
      `Item ${bad}: ${type} attributes cannot be put in folders (Archicad 26 has attribute folders only for ` +
        `${[...FOLDER_TYPES].join(", ")}). Remove 'folder'.`,
    );
  }
}

async function createAttributes(
  ctx: ToolContext,
  type: string,
  items: Record<string, unknown>[],
  ifExists: string | undefined,
  undoName: string,
): Promise<unknown> {
  checkFolders(type, items);
  const params: Record<string, unknown> = { type, attributes: items, undoName };
  if (ifExists) params["ifExists"] = ifExists;
  return ctx.ac.addon("CreateAttributes", params);
}

/** 8x8 ordered-dither (Bayer) threshold matrix used for percentage (screen) fills. */
const BAYER8 = [
  [0, 32, 8, 40, 2, 34, 10, 42],
  [48, 16, 56, 24, 50, 18, 58, 26],
  [12, 44, 4, 36, 14, 46, 6, 38],
  [60, 28, 52, 20, 62, 30, 54, 22],
  [3, 35, 11, 43, 1, 33, 9, 41],
  [51, 19, 59, 27, 49, 17, 57, 25],
  [15, 47, 7, 39, 13, 45, 5, 37],
  [63, 31, 55, 23, 61, 29, 53, 21],
];

/** 16-hex-digit 8x8 screen pattern with the given coverage (0..1), e.g. 0.5 -> "AA55AA55AA55AA55". */
export function screenPattern(coverage: number): string {
  const on = Math.round(Math.min(1, Math.max(0, coverage)) * 64);
  return BAYER8.map((row) =>
    row
      .reduce((bits, threshold, col) => (threshold < on ? bits | (0x80 >> col) : bits), 0)
      .toString(16)
      .toUpperCase()
      .padStart(2, "0"),
  ).join("");
}

/**
 * Normalizes a create_fills item (works with every add-on build):
 * - new vector fills (no basedOn): the pattern scale factor turns the hatch-line values into meters, so default it to
 *   0.001 (values = mm on paper) or 1 with scaleWithPlan (values = m in the model);
 * - percent 0..100 -> 0..1, plus a matching screen pattern when no bitmapPattern is given.
 */
export function normalizeFill<T extends Record<string, unknown>>(fill: T): T {
  const out: Record<string, unknown> = { ...fill };
  if (fill["basedOn"] === undefined && fill["lines"] !== undefined) {
    const unit = fill["scaleWithPlan"] === true ? 1 : 0.001;
    out["spacingX"] = fill["spacingX"] ?? unit;
    out["spacingY"] = fill["spacingY"] ?? fill["spacingX"] ?? unit;
  }
  return normalizeFillPercent(out) as T;
}

/** percent 0..100 -> 0..1 and a matching screen pattern when no bitmapPattern is given (create and modify). */
export function normalizeFillPercent<T extends Record<string, unknown>>(fill: T): T {
  if (typeof fill["percent"] !== "number") return fill;
  const pc = fill["percent"] > 1 ? fill["percent"] / 100 : fill["percent"];
  const out: Record<string, unknown> = { ...fill, percent: pc };
  if (fill["bitmapPattern"] === undefined) out["bitmapPattern"] = screenPattern(pc);
  return out as T;
}

/** Index of the first percentage (screen) fill of the project, if any. */
async function findPercentFill(ctx: ToolContext): Promise<number | undefined> {
  const res = (await ctx.ac.addon("GetAttributes", { type: "Fill", limit: 5000 })) as {
    attributes?: { index: number; fillType?: string; percentFill?: boolean }[];
  };
  return res.attributes?.find((f) => f.fillType === "Solid" && f.percentFill === true)?.index;
}

export function registerAttributeTools(server: McpServer, ctx: ToolContext): void {
  // --- Read -------------------------------------------------------------------

  defineTool(server, ctx, {
    name: "get_attributes",
    title: "Get attributes",
    description:
      "Lists Archicad attributes of one type with their settings: {index, name, guid, folder?, ...type fields} using the SAME field names " +
      "that the create_*/modify_attributes tools accept. Summary fields per type — Layer: hidden, locked, wireframe, intersectionGroup; " +
      "Pen: color, width (mm); Line: lineType, scaleWithPlan, period; Fill: fillType, usage, bitmapPattern, spacingX/Y (pattern units " +
      "× spacingX = meters), percent; Composite: " +
      "totalThickness, skinCount, usage; Surface: color, transparency, reflection, fill, texture; LayerCombination: active, layerCount; " +
      "ZoneCategory: code, color, stamp; BuildingMaterial: cutFill, pens, surface, uiPriority (0-999 as in the UI), thermal properties; Profile: usage; PenTable: " +
      "activeForModel/Layout; MEPSystem, OperationProfile. detailed: true adds composite skins and skin lines, per-layer states of layer " +
      "combinations, dash/symbol items of line types, vector hatch lines, all pens of a pen table, profile size, dimension formats. " +
      "Without type: returns the number of attributes of every type. Layer/LayerCombination results include the active layer combination.",
    input: {
      type: AttributeType.optional().describe("Attribute type to list (omit to get counts per type)"),
      attributes: z
        .array(AttrRef)
        .min(1)
        .max(1000)
        .optional()
        .describe("Only these attributes (names, indices or {guid}); nameFilter/offset/limit are ignored then"),
      nameFilter: z
        .string()
        .optional()
        .describe("Case-insensitive name substring, or a wildcard pattern with * and ? (e.g. 'Бетон*')"),
      detailed: z.boolean().optional().describe("Include definition details (skins, layer states, dashes, hatch lines, pens...). Default false"),
      offset: z.number().int().min(0).optional().describe("Skip this many matches (paging)"),
      limit: z
        .number()
        .int()
        .min(1)
        .max(5000)
        .optional()
        .describe("Return at most this many (default 300; the response has total/hasMore — narrow with nameFilter instead of paging when possible)"),
    },
    annotations: READ_ONLY,
    handler: async ({ type, attributes, nameFilter, detailed, offset, limit }, { ac }) => {
      if (!type && (attributes || nameFilter || detailed || offset !== undefined || limit !== undefined)) {
        throw new Error("Pass 'type' (e.g. 'Layer') together with attributes/nameFilter/detailed/offset/limit.");
      }
      const params: Record<string, unknown> = {};
      if (type) params["type"] = type;
      if (attributes) params["attributes"] = attributes.map(refObject);
      if (nameFilter) params["nameFilter"] = nameFilter;
      if (detailed !== undefined) params["detailed"] = detailed;
      if (offset !== undefined) params["offset"] = offset;
      if (!attributes && type) params["limit"] = limit ?? 300;
      return ac.addon("GetAttributes", params);
    },
  });

  // --- Create -----------------------------------------------------------------

  defineTool(server, ctx, {
    name: "create_layers",
    title: "Create layers",
    description:
      "Creates layers (visible and unlocked unless stated; intersection group 1). Then place elements on them with the 'layer' field of " +
      "any create_*/modify_elements tool. " +
      RESULT_NOTE,
    input: {
      layers: z.array(z.object({ ...CreateCommon, ...LayerFields })).min(1).max(500),
      ifExists: ifExistsInput,
    },
    annotations: CREATES,
    handler: async ({ layers, ifExists }, c) => createAttributes(c, "Layer", layers, ifExists, "Create layers (Claude)"),
  });

  defineTool(server, ctx, {
    name: "create_layer_combinations",
    title: "Create layer combinations",
    description:
      "Creates layer combinations (saved sets of layer visibility/lock/wireframe/intersection states). By default a new combination " +
      "captures the CURRENT state of every layer, then 'layers' overrides are applied in order, e.g. {name: 'Plan - structure', base: " +
      "'allHidden', layers: [{match: 'Structural*', visible: true}]}. Activate one with apply_layer_combination. " +
      RESULT_NOTE,
    input: {
      layerCombinations: z.array(z.object({ ...CreateCommon, ...LayerCombinationFields })).min(1).max(200),
      ifExists: ifExistsInput,
    },
    annotations: CREATES,
    handler: async ({ layerCombinations, ifExists }, c) =>
      createAttributes(c, "LayerCombination", layerCombinations, ifExists, "Create layer combinations (Claude)"),
  });

  defineTool(server, ctx, {
    name: "create_composites",
    title: "Create composite structures",
    description:
      "Creates composite (multi-skin) structures for walls, slabs, roofs and shells. Skins are listed from the outside/reference side to " +
      "the inside, each {thickness (m), buildingMaterial, core?, finish?}; the total thickness is the sum. Example: {name: 'Wall 380', " +
      "skins: [{thickness: 0.02, buildingMaterial: '<plaster>', finish: true}, {thickness: 0.25, buildingMaterial: '<brick>', core: true}, " +
      "{thickness: 0.1, buildingMaterial: '<insulation>'}, {thickness: 0.01, buildingMaterial: '<render>', finish: true}]} " +
      "(get building material names with get_attributes type BuildingMaterial). Use the composite with the 'composite' field of " +
      "create_walls / create_slabs / create_roofs. basedOn copies an existing composite. " +
      RESULT_NOTE,
    input: {
      composites: z.array(z.object({ ...CreateCommon, ...CompositeFields })).min(1).max(200),
      ifExists: ifExistsInput,
    },
    annotations: CREATES,
    handler: async ({ composites, ifExists }, c) => createAttributes(c, "Composite", composites, ifExists, "Create composites (Claude)"),
  });

  defineTool(server, ctx, {
    name: "create_building_materials",
    title: "Create building materials",
    description:
      "Creates building materials (the material of elements and composite skins: cut fill + pens, surface, intersection priority, " +
      "physical properties). Unspecified settings are copied from 'basedOn' or, when omitted, from the first building material of the " +
      "project — so give at least cutFill, surface and uiPriority for a meaningful material. " +
      RESULT_NOTE,
    input: {
      buildingMaterials: z.array(z.object({ ...CreateCommon, ...BuildingMaterialFields })).min(1).max(500),
      ifExists: ifExistsInput,
    },
    annotations: CREATES,
    handler: async ({ buildingMaterials, ifExists }, c) =>
      createAttributes(c, "BuildingMaterial", buildingMaterials, ifExists, "Create building materials (Claude)"),
  });

  defineTool(server, ctx, {
    name: "create_surfaces",
    title: "Create surfaces",
    description:
      "Creates surfaces (3D materials: color, reflection, transparency, 3D hatch, texture). Unspecified settings are copied from " +
      "'basedOn' or, when omitted, from the first surface of the project (without its texture). Example: {name: 'Red paint', color: " +
      "'#B22222', transparency: 0}. Glass: {materialType: 'Glass', color: '#9FC5E8', transparency: 70}. Advanced Cineware (CineRender) " +
      "channels are not accessible through the API. " +
      RESULT_NOTE,
    input: {
      surfaces: z.array(z.object({ ...CreateCommon, ...SurfaceFields })).min(1).max(500),
      ifExists: ifExistsInput,
    },
    annotations: CREATES,
    handler: async ({ surfaces, ifExists }, c) => createAttributes(c, "Surface", surfaces, ifExists, "Create surfaces (Claude)"),
  });

  defineTool(server, ctx, {
    name: "create_fills",
    title: "Create fill types",
    description:
      "Creates fill types: Solid (percentage/screen fill with percent: 25, or an explicit bitmapPattern, e.g. '55AA55AA55AA55AA' = 50%), " +
      "Empty, Vector (hatch lines in mm on paper, e.g. {name: 'Diagonal 2mm', lines: [{angle: 45, spacing: 2}]}; with scaleWithPlan: true " +
      "the values are meters in the model, e.g. a 0.3 m tile grid [{angle: 0, spacing: 0.3}, {angle: 90, spacing: 0.3}]), " +
      "LinearGradient / RadialGradient, Image (texture). " +
      "Symbol fills and any other fill can be copied with basedOn / duplicate_attributes. Use fills in building materials (cutFill), " +
      "hatches, zone/slab cover fills. " +
      RESULT_NOTE,
    input: {
      fills: z.array(z.object({ ...CreateCommon, ...FillFields })).min(1).max(200),
      ifExists: ifExistsInput,
    },
    annotations: CREATES,
    handler: async ({ fills, ifExists }, c) => {
      let items = fills.map(normalizeFill);
      // Percentage fills carry internal flags the API cannot set: a percentage fill built from scratch stays a
      // plain solid fill, so new ones are copied from an existing percentage fill (e.g. "25 %").
      if (items.some((f) => f.percent !== undefined && f.basedOn === undefined)) {
        const template = await findPercentFill(c);
        if (template !== undefined) {
          items = items.map((f) => (f.percent !== undefined && f.basedOn === undefined ? { ...f, basedOn: { index: template } } : f));
        }
      }
      return createAttributes(c, "Fill", items, ifExists, "Create fills (Claude)");
    },
  });

  defineTool(server, ctx, {
    name: "create_line_types",
    title: "Create line types",
    description:
      "Creates line types: Solid, Dashed ({name: 'Dash 3-1.5', dashes: [{dash: 3, gap: 1.5}]} — millimeters on paper unless " +
      "scaleWithPlan: true, then meters in the model) or Symbol (items; easier: copy an existing symbol line with basedOn). " +
      RESULT_NOTE,
    input: {
      lineTypes: z.array(z.object({ ...CreateCommon, ...LineTypeFields })).min(1).max(200),
      ifExists: ifExistsInput,
    },
    annotations: CREATES,
    handler: async ({ lineTypes, ifExists }, c) => createAttributes(c, "Line", lineTypes, ifExists, "Create line types (Claude)"),
  });

  defineTool(server, ctx, {
    name: "create_zone_categories",
    title: "Create zone categories",
    description:
      "Creates zone categories {name, code, color, stamp?}. Without 'stamp' the zone stamp (and its parameters) is copied from 'basedOn' " +
      "or from the first zone category of the project. Assign it to zones with the zone tools' category field. " +
      RESULT_NOTE,
    input: {
      zoneCategories: z.array(z.object({ ...CreateCommon, ...ZoneCategoryFields })).min(1).max(200),
      ifExists: ifExistsInput,
    },
    annotations: CREATES,
    handler: async ({ zoneCategories, ifExists }, c) =>
      createAttributes(c, "ZoneCategory", zoneCategories, ifExists, "Create zone categories (Claude)"),
  });

  defineTool(server, ctx, {
    name: "create_profiles",
    title: "Create complex profiles",
    description:
      "Creates complex profiles (custom cross-sections for walls, beams, columns, handrails) from polygons: each shape is " +
      "{polygon (m, x horizontal / y vertical, straight edges), buildingMaterial, core?}. Example: a 0.3 x 0.5 m rectangular beam: " +
      "{name: 'Beam 300x500', usage: {beams: true, columns: true}, shapes: [{polygon: [{x:-0.15,y:0},{x:0.15,y:0},{x:0.15,y:0.5},{x:-0.15,y:0.5}], " +
      "buildingMaterial: '<concrete>'}]}. Stretch zones/parameters are not supported (edit in Archicad's Profile Manager). basedOn copies " +
      "an existing profile. Use it with the 'profile' field of walls/columns/beams. " +
      RESULT_NOTE,
    input: {
      profiles: z.array(z.object({ ...CreateCommon, ...ProfileFields })).min(1).max(100),
      ifExists: ifExistsInput,
    },
    annotations: CREATES,
    handler: async ({ profiles, ifExists }, c) => createAttributes(c, "Profile", profiles, ifExists, "Create profiles (Claude)"),
  });

  defineTool(server, ctx, {
    name: "duplicate_attributes",
    title: "Duplicate attributes",
    description:
      "Copies existing attributes of any type except Pen/Font (e.g. a pen set, a symbol fill, an MEP system, a model view option, a " +
      "dimension standard, an operation profile, a composite) under a new name, optionally changing fields on the copy: each item is " +
      "{source, name, folder?, ...fields of that type as in the create_* tools / modify_attributes}. " +
      RESULT_NOTE,
    input: {
      type: CopyableAttributeType,
      attributes: z
        .array(
          z
            .object({
              source: AttrRef.describe("Attribute to copy (name, index or {guid})"),
              name: z.string().min(1).max(255).describe("Name of the copy"),
              folder: z.union([z.array(z.string().min(1)), z.string()]).optional().describe("Folder of the copy, e.g. ['Imported']"),
            })
            .passthrough()
            .describe("Extra type-specific fields are applied to the copy"),
        )
        .min(1)
        .max(200),
      ifExists: ifExistsInput,
    },
    annotations: CREATES,
    handler: async ({ type, attributes, ifExists }, c) =>
      createAttributes(
        c,
        type,
        attributes.map(({ source, ...rest }) => ({ ...rest, basedOn: source })),
        ifExists,
        `Duplicate ${type} attributes (Claude)`,
      ),
  });

  // --- Modify / delete --------------------------------------------------------

  defineTool(server, ctx, {
    name: "modify_attributes",
    title: "Modify attributes",
    description:
      "Changes existing attributes of any type in one undo step. Each item is {type, attribute (name/index/{guid}), name? (rename), " +
      "folder? (move), ...fields to change} with the same field names as get_attributes returns and the create_* tools accept; only the " +
      "given fields change. Examples: {type: 'Layer', attribute: 'Мебель', locked: true}; {type: 'Surface', attribute: 'Red paint', color: " +
      "'#AA0000'}; {type: 'Composite', attribute: 'Wall 380', skins: [...]}; {type: 'Pen', attribute: 12, color: '#FF0000', width: 0.5}; " +
      "{type: 'LayerCombination', attribute: 'Plan', base: 'current'} (re-captures the current layer states). Fonts cannot be modified. " +
      "Returns {results: [{index, name, guid} | {error}]}.",
    input: {
      attributes: z.array(ModifyItem).min(1).max(500),
      undoName: z.string().optional().describe("Name of the undo step"),
    },
    annotations: MODIFIES,
    handler: async ({ attributes, undoName }, { ac }) => {
      for (const a of attributes) checkFolders(a.type, [a]);
      return ac.addon("ModifyAttributes", {
        attributes: attributes.map((a) => (a.type === "Fill" ? normalizeFillPercent(a) : a)),
        ...(undoName ? { undoName } : {}),
      });
    },
  });

  defineTool(server, ctx, {
    name: "delete_attributes",
    title: "Delete attributes",
    description:
      "Deletes attributes of one type (one undo step). Archicad reassigns elements that used a deleted attribute (check them afterwards). " +
      "WARNING: deleting a LAYER deletes every element on it — the tool refuses layers that still hold elements unless force: true " +
      "(move elements with modify_elements first). Pens, fonts, the Archicad layer and some built-in attributes (the last one of a type, " +
      "Solid/Empty fills, line type 1) cannot be deleted. Returns {results: [{index, name, deleted: true} | {error}]}.",
    input: {
      type: DeletableAttributeType,
      attributes: z.array(AttrRef).min(1).max(1000).describe("Attributes to delete: names (localized), indices or {guid}"),
      force: z.boolean().optional().describe("Layers only: also delete layers that still hold elements (the elements are deleted too)"),
      undoName: z.string().optional(),
    },
    annotations: DESTRUCTIVE,
    handler: async ({ type, attributes, force, undoName }, { ac }) =>
      ac.addon("DeleteAttributes", {
        type,
        attributes: attributes.map(refObject),
        ...(force !== undefined ? { force } : {}),
        ...(undoName ? { undoName } : {}),
      }),
  });

  defineTool(server, ctx, {
    name: "modify_pens",
    title: "Modify pens",
    description:
      "Changes pen colors, weights and descriptions (pens cannot be created: there are always 255). Without penTable the pens in " +
      "effect in the model are changed; with penTable a stored pen set is edited (it does not become active). Read the current pens " +
      "with get_attributes {type: 'Pen'} or {type: 'PenTable', detailed: true}. Returns {results: [{index, color, width} | {error}], " +
      "activePenTable | penTable}.",
    input: {
      pens: z.array(PenSpec).min(1).max(255).describe("[{index 1-255, color?: '#RRGGBB', width?: mm on paper, description?}]"),
      penTable: AttrRef.optional().describe("Pen set (PenTable attribute) to edit instead of the active pens"),
      undoName: z.string().optional(),
    },
    annotations: MODIFIES,
    handler: async ({ pens, penTable, undoName }, { ac }) =>
      ac.addon("ModifyPens", { pens, ...(penTable !== undefined ? { penTable } : {}), ...(undoName ? { undoName } : {}) }),
  });

  // --- Layer states -----------------------------------------------------------

  defineTool(server, ctx, {
    name: "apply_layer_combination",
    title: "Apply layer combination",
    description:
      "Makes a saved layer combination active in the current model window (sets the visibility/lock/wireframe of all layers). List " +
      "combinations with get_attributes {type: 'LayerCombination'}. Returns {ok, layerCombination, activeLayerCombination}.",
    input: {
      layerCombination: AttrRef.describe("Layer combination name (localized), index or {guid}"),
    },
    annotations: MODIFIES,
    handler: async ({ layerCombination }, { ac }) => ac.addon("ApplyLayerCombination", { layerCombination }),
  });

  defineTool(server, ctx, {
    name: "set_layer_states",
    title: "Set layer states",
    description:
      "Shows/hides, locks/unlocks, switches wireframe or the intersection group of layers directly (the active layer settings; saved " +
      "combinations are not changed — use modify_attributes type LayerCombination for those). Items run in order in one undo step, so " +
      "'isolate' works as [{match: '*', hidden: true}, {layer: 'Стены', visible: true}]. The Archicad layer cannot be hidden/locked " +
      "(skipped for patterns). Returns per item {changed: [{index, name}], changedCount, alreadyInStateCount, skipped?} | {error}.",
    input: {
      layers: z.array(LayerStateItem).min(1).max(500),
      undoName: z.string().optional(),
    },
    annotations: MODIFIES,
    handler: async ({ layers, undoName }, { ac }) => ac.addon("SetLayerStates", { layers, ...(undoName ? { undoName } : {}) }),
  });
}
