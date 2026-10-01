/**
 * Zod schemas of the attribute tools (field names match addon/Src/Commands/Attributes.cpp 1:1:
 * the same names are used when creating, modifying and reading attributes).
 * Units: lengths in meters, pen widths in paper millimeters, angles in degrees, colors "#RRGGBB".
 */

import { z } from "zod";

import { AttrRef, LibPartRef, PenIndex, Polygon } from "./schemas.js";

export const ATTRIBUTE_TYPES = [
  "Pen",
  "Layer",
  "Line",
  "Fill",
  "Composite",
  "Surface",
  "LayerCombination",
  "ZoneCategory",
  "Font",
  "Profile",
  "PenTable",
  "DimensionStandard",
  "ModelViewOption",
  "MEPSystem",
  "OperationProfile",
  "BuildingMaterial",
] as const;

export const AttributeType = z
  .enum(ATTRIBUTE_TYPES)
  .describe(
    "Attribute type: Pen, Layer, Line (line types), Fill (fill/hatch patterns), Composite (layered wall/slab/roof/shell structures), " +
      "Surface (3D materials), LayerCombination, ZoneCategory, Font, Profile (complex profiles), PenTable (pen sets), DimensionStandard, " +
      "ModelViewOption, MEPSystem, OperationProfile (energy evaluation), BuildingMaterial",
  );

/** Types that can be deleted (pens and fonts cannot). */
export const DeletableAttributeType = z
  .enum(ATTRIBUTE_TYPES.filter((t) => t !== "Pen" && t !== "Font") as [string, ...string[]])
  .describe("Attribute type (pens and fonts cannot be deleted)");

/** Types that can be copied with duplicate_attributes. */
export const CopyableAttributeType = DeletableAttributeType.describe(
  "Attribute type to copy (every type except Pen and Font)",
);

export const Color = z
  .union([
    z.string().regex(/^#?[0-9A-Fa-f]{6}$/, 'must be "#RRGGBB"'),
    z.object({
      red: z.number().min(0).max(1),
      green: z.number().min(0).max(1),
      blue: z.number().min(0).max(1),
    }),
  ])
  .describe('Color as "#RRGGBB" (e.g. "#B22222") or {red, green, blue} with components 0..1');

export const BackgroundPen = z
  .number()
  .int()
  .min(-1)
  .max(255)
  .describe("Background pen: 1-255, 0 = transparent, -1 = window background color");

export const Folder = z
  .union([z.array(z.string().min(1)), z.string()])
  .describe(
    "Attribute folder path in the Attribute Manager, e.g. ['Walls', 'Concrete'] or 'Walls/Concrete' (created when missing; [] = root folder). " +
      "Only Layer, Composite, Profile, Surface and BuildingMaterial attributes have folders in Archicad 26",
  );

export const IfExists = z
  .enum(["error", "skip", "update"])
  .describe(
    "What to do when an attribute with the same name already exists: 'error' (default), 'skip' (return the existing one unchanged), " +
      "'update' (apply the given fields to the existing one)",
  );

export const BasedOn = AttrRef.describe(
  "Copy this existing attribute of the same type (all settings); the other given fields are applied on top",
);

/** Fields every create item accepts. */
export const CreateCommon = {
  name: z.string().min(1).max(255).describe("Name of the new attribute (must be unique within its type)"),
  basedOn: BasedOn.optional(),
  folder: Folder.optional(),
  ifExists: IfExists.optional().describe("Per-item override of the tool-level ifExists"),
};

/** Fields every modify item accepts. */
export const ModifyCommon = {
  attribute: AttrRef.describe("The attribute to change: exact name (localized), index, or {guid}"),
  name: z.string().min(1).max(255).optional().describe("Rename to this name"),
  folder: Folder.optional().describe("Move the attribute into this folder (created when missing; [] = root)"),
};

// --- Layers ------------------------------------------------------------------

export const LayerStateFields = {
  hidden: z.boolean().optional().describe("true = hide the layer (its elements are not shown and cannot be edited)"),
  visible: z.boolean().optional().describe("Alternative to hidden: visible: false hides, visible: true shows"),
  locked: z.boolean().optional().describe("true = lock the layer (elements stay visible but cannot be selected/edited)"),
  wireframe: z.boolean().optional().describe("true = show the layer's elements as wireframe in 3D"),
  intersectionGroup: z
    .number()
    .int()
    .min(0)
    .optional()
    .describe("Intersection group number: elements on layers with the SAME number join/intersect each other; 0 = never intersect (new layers default to 1)"),
};

export const LayerFields = { ...LayerStateFields };

export const LayerStateItem = z
  .object({
    layer: AttrRef.optional().describe("Layer name (localized, exact) or index"),
    match: z
      .string()
      .min(1)
      .optional()
      .describe(
        "Instead of 'layer': select many layers by name. With * or ? it is a case-insensitive wildcard pattern ('*' = all layers, " +
          "'Structural*'); without wildcards a case-insensitive substring",
      ),
    ...LayerStateFields,
  })
  .refine((i) => i.layer !== undefined || i.match !== undefined, { message: "give 'layer' or 'match'" });

export const LayerCombinationFields = {
  base: z
    .enum(["current", "allVisible", "allHidden", "unchanged"])
    .optional()
    .describe(
      "Starting layer states before 'layers' is applied: 'current' = the layer states in effect now (default for new combinations), " +
        "'allVisible', 'allHidden', 'unchanged' = keep the combination's stored states (default when modifying or copying). " +
        "Layers missing from a combination are hidden in it",
    ),
  layers: z
    .array(LayerStateItem)
    .optional()
    .describe("Per-layer states stored in the combination, applied in order (e.g. [{match: '*', hidden: true}, {layer: 'Walls', visible: true}])"),
};

// --- Pens --------------------------------------------------------------------

export const PenFields = {
  color: Color.optional(),
  width: z.number().min(0).max(100).optional().describe("Pen weight in millimeters on paper (e.g. 0.25)"),
  description: z.string().max(127).optional().describe("Pen description text"),
};

export const PenSpec = z.object({ index: PenIndex, ...PenFields });

export const PenTableFields = {
  pens: z.array(PenSpec).optional().describe("Pens of this pen set to change: [{index 1-255, color?, width?, description?}]"),
};

// --- Line types --------------------------------------------------------------

export const DashItem = z.object({
  dash: z.number().min(0).describe("Visible length"),
  gap: z.number().min(0).describe("Invisible length after the dash"),
});

export const LineItem = z.object({
  type: z
    .enum(["Separator", "CenterDot", "CenterLine", "Dot", "RightAngle", "Parallel", "Line", "Circle", "Arc"])
    .describe("Symbol item kind"),
  centerOffset: z.number().optional().describe("Vertical distance from the line axis (Separator, CenterDot, CenterLine)"),
  length: z.number().optional().describe("Item length along the line (CenterLine, RightAngle, Parallel)"),
  begin: z.object({ x: z.number(), y: z.number() }).optional().describe("Start position (Dot, RightAngle, Parallel, Line, Circle center, Arc center)"),
  end: z.object({ x: z.number(), y: z.number() }).optional().describe("End position (Line)"),
  radius: z.number().optional().describe("Radius (Circle, Arc)"),
  beginAngle: z.number().optional().describe("Arc start angle in degrees, measured from the VERTICAL axis"),
  endAngle: z.number().optional().describe("Arc end angle in degrees, measured from the vertical axis"),
});

export const LineTypeFields = {
  lineType: z
    .enum(["Solid", "Dashed", "Symbol"])
    .optional()
    .describe("Line type kind (default: Dashed when dashes are given, Symbol when items are given, else Solid)"),
  scaleWithPlan: z
    .boolean()
    .optional()
    .describe(
      "false (default) = scale-independent: lengths are MILLIMETERS on paper; true = scaled with the plan: lengths are METERS in the model (at defineScale)",
    ),
  defineScale: z.number().positive().optional().describe("Scale denominator the pattern is defined at (only for scaleWithPlan, e.g. 100 for 1:100)"),
  dashes: z
    .array(DashItem)
    .min(1)
    .optional()
    .describe("Dashed pattern, repeated along the line: e.g. [{dash: 3, gap: 1.5}] (mm unless scaleWithPlan). The period is computed"),
  items: z.array(LineItem).min(1).optional().describe("Symbol line items (advanced; copying an existing symbol line with duplicate_attributes is easier)"),
  period: z.number().positive().optional().describe("Symbol lines: length of one repetition"),
  height: z.number().min(0).optional().describe("Symbol lines: height of the symbol"),
};

// --- Fills -------------------------------------------------------------------

export const FillUsage = z
  .object({
    drafting: z.boolean().optional().describe("Available as drafting fill (2D fills, hatches)"),
    cut: z.boolean().optional().describe("Available as cut fill (building materials, cut elements)"),
    cover: z.boolean().optional().describe("Available as cover fill (slab/zone/mesh surfaces on the plan)"),
  })
  .describe("Where the fill can be chosen (new fills: all three)");

export const HatchLine = z.object({
  angle: z.number().optional().describe("Direction of the parallel lines in degrees (0 = horizontal)"),
  spacing: z
    .number()
    .positive()
    .describe("Distance between the parallel lines in pattern units (× spacingX/Y = meters; new fills: mm on paper, or m in the model with scaleWithPlan)"),
  offset: z.object({ x: z.number(), y: z.number() }).optional().describe("Origin offset of this line family (pattern units)"),
  offsetAlongLine: z.number().optional().describe("Shift of the dash pattern along the lines (pattern units)"),
  dashes: z
    .array(z.number().min(0))
    .optional()
    .describe("Alternating dash, gap, dash, gap... lengths in pattern units (even count); omit for continuous lines"),
});

export const FillTexture = z.object({
  name: z.string().min(1).optional().describe("Image file name from the loaded libraries (required for new image fills)"),
  width: z.number().positive().optional().describe("Image width in meters"),
  height: z.number().positive().optional().describe("Image height in meters"),
  angle: z.number().optional().describe("Rotation in degrees"),
  mirrorX: z.boolean().optional(),
  mirrorY: z.boolean().optional(),
});

export const FillFields = {
  fillType: z
    .enum(["Solid", "Empty", "Vector", "Symbol", "LinearGradient", "RadialGradient", "Image"])
    .optional()
    .describe(
      "Fill kind (new fills: Vector when lines are given, else Solid). Symbol fills can only be copied (duplicate_attributes). " +
        "Cannot be changed on an existing fill",
    ),
  usage: FillUsage.optional(),
  scaleWithPlan: z
    .boolean()
    .optional()
    .describe("true = pattern scales with the drawing (lengths in meters in the model); false = fixed on paper (lengths in millimeters)"),
  bitmapPattern: z
    .union([z.string().regex(/^(#|0x)?[0-9A-Fa-f]{16}$/), z.array(z.number().int().min(0).max(255)).length(8)])
    .optional()
    .describe(
      "8x8 screen/bitmap pattern: 16 hex digits = 8 rows, first row first ('FFFFFFFFFFFFFFFF' = solid, '55AA55AA55AA55AA' = 50%), or 8 row bytes. " +
        "Solid fills with a partial pattern are percentage fills",
    ),
  spacingX: z
    .number()
    .positive()
    .optional()
    .describe(
      "Vector/symbol fills: pattern scale factor X — pattern units × spacingX = METERS (on paper for scale-independent fills, in the " +
        "model with scaleWithPlan). New fills default to 0.001 (line values in mm on paper) or 1 with scaleWithPlan (values in m). " +
        "Existing fills: real spacing = lines[i].spacing × spacingX",
    ),
  spacingY: z.number().positive().optional().describe("Vector/symbol fills: pattern scale factor Y (same meaning and default as spacingX)"),
  angle: z.number().optional().describe("Vector/symbol fills: rotation of the whole pattern in degrees"),
  percent: z
    .number()
    .min(0)
    .max(100)
    .optional()
    .describe(
      "Solid fills only: coverage of a percentage (screen) fill, 0..1 or 0..100 (e.g. 25 = a 25 % fill); without bitmapPattern a matching 8x8 screen pattern is generated",
    ),
  lines: z
    .array(HatchLine)
    .min(1)
    .optional()
    .describe(
      "Vector fills: families of parallel hatch lines, e.g. 45° diagonal hatch: [{angle: 45, spacing: 2}]; " +
        "grid: [{angle: 0, spacing: 2}, {angle: 90, spacing: 2}] (mm on paper unless scaleWithPlan)",
    ),
  texture: FillTexture.optional().describe("Image fills: the picture and its size"),
};

// --- Composites --------------------------------------------------------------

export const Skin = z.object({
  thickness: z.number().positive().optional().describe("Skin thickness in meters (required for new skins)"),
  buildingMaterial: AttrRef.optional().describe("Building material of the skin (required for new skins; list with get_attributes type BuildingMaterial)"),
  core: z.boolean().optional().describe("Core skin (structural; reference lines like 'CoreOutside' use it)"),
  finish: z.boolean().optional().describe("Finish skin (can be hidden in partial structure display)"),
  endLinePen: PenIndex.optional().describe("Pen of the skin's end lines"),
});

export const SkinLine = z.object({
  lineType: AttrRef.optional().describe("Line type (name/index; 1 = solid)"),
  pen: PenIndex.optional().describe("Line pen"),
});

export const CompositeUsage = z
  .object({
    walls: z.boolean().optional(),
    slabs: z.boolean().optional(),
    roofs: z.boolean().optional(),
    shells: z.boolean().optional(),
  })
  .describe("Element types that may use the composite (new composites: all)");

export const CompositeFields = {
  skins: z
    .array(Skin)
    .min(1)
    .max(100)
    .optional()
    .describe(
      "Skins from the OUTSIDE (reference side / top) to the INSIDE (bottom); the total thickness is their sum. When modifying, " +
        "the list replaces all skins, but omitted fields of skin i keep the current values of skin i",
    ),
  skinLines: z
    .array(SkinLine)
    .optional()
    .describe("Exactly skins.length + 1 lines: outer contour, each separator between skins, inner contour (default: solid line, pen 1)"),
  usage: CompositeUsage.optional(),
};

// --- Building materials ------------------------------------------------------

export const BuildingMaterialFields = {
  id: z.string().optional().describe("Building material ID text"),
  manufacturer: z.string().optional(),
  description: z.string().optional(),
  cutFill: AttrRef.optional().describe("Cut fill (Fill attribute name/index) shown where elements are cut"),
  cutFillPen: PenIndex.optional().describe("Cut fill foreground pen"),
  cutFillBackgroundPen: BackgroundPen.optional(),
  surface: AttrRef.optional().describe("Surface (3D material) of the cut/visible faces"),
  cutFillOrientation: z.enum(["ProjectOrigin", "ElementOrigin", "FitToSkin"]).optional().describe("How the cut fill pattern is aligned"),
  uiPriority: z
    .number()
    .int()
    .min(0)
    .max(999)
    .optional()
    .describe(
      "Intersection priority 0-999 exactly as shown in Archicad's Building Material settings (and as 'connectionPriority' in the official " +
        "JSON API); where elements meet, the higher value wins (e.g. brick 540, concrete 720 — copy values of existing materials from " +
        "get_attributes to rank a new one between them). Preferred over 'priority'",
    ),
  priority: z
    .number()
    .int()
    .min(0)
    .optional()
    .describe("Advanced: the internal stored priority value (e.g. 5169; converted to/from uiPriority by Archicad). Give only one of priority/uiPriority"),
  thermalConductivity: z.number().min(0).optional().describe("λ in W/(m·K)"),
  density: z.number().min(0).optional().describe("kg/m³"),
  heatCapacity: z.number().min(0).optional().describe("J/(kg·K)"),
  embodiedEnergy: z.number().min(0).optional().describe("Embodied energy (MJ/kg)"),
  embodiedCarbon: z.number().min(0).optional().describe("Embodied carbon (kgCO2/kg)"),
  showUncutLines: z.boolean().optional().describe("Show the skin's uncut lines"),
  participatesInCollisionDetection: z.boolean().optional(),
};

// --- Surfaces ----------------------------------------------------------------

export const SurfaceTexture = z.object({
  name: z.string().min(1).optional().describe("Texture image file name from the loaded libraries (required when adding a texture)"),
  width: z.number().positive().optional().describe("Texture tile width in meters"),
  height: z.number().positive().optional().describe("Texture tile height in meters"),
  angle: z.number().optional().describe("Texture rotation in degrees"),
  mirrorX: z.boolean().optional(),
  mirrorY: z.boolean().optional(),
  randomShift: z.boolean().optional(),
  useAlphaChannel: z.boolean().optional(),
  alphaAffects: z
    .object({
      surfaceColor: z.boolean().optional(),
      ambientColor: z.boolean().optional(),
      specularColor: z.boolean().optional(),
      diffuseColor: z.boolean().optional(),
      bump: z.boolean().optional(),
      transparency: z.boolean().optional(),
    })
    .optional()
    .describe("What the alpha channel modulates"),
  fit: z.enum(["none", "fillRectangle", "fitPicture"]).optional().describe("Natural-aspect fitting mode"),
});

export const SurfaceFields = {
  materialType: z
    .enum(["General", "Simple", "Matte", "Metal", "Plastic", "Glass", "Glowing", "Constant"])
    .optional()
    .describe("Basic-engine material type"),
  color: Color.optional().describe("Surface color"),
  ambient: z.number().min(0).max(100).optional().describe("Ambient reflection 0-100"),
  diffuse: z.number().min(0).max(100).optional().describe("Diffuse reflection 0-100"),
  specular: z.number().min(0).max(100).optional().describe("Specular reflection 0-100"),
  transparency: z.number().min(0).max(100).optional().describe("Transparency 0-100 (0 = opaque)"),
  shininess: z.number().min(0).max(100).optional().describe("Shininess 0-100"),
  transparencyAttenuation: z.number().min(0).max(4).optional().describe("Transparency attenuation 0-4"),
  specularColor: Color.optional(),
  emissionColor: Color.optional(),
  emissionAttenuation: z.number().min(0).max(327).optional().describe("Emission attenuation 0-327"),
  fill: z.union([AttrRef, z.literal(false)]).optional().describe("Vectorial 3D hatch (Fill attribute) shown on the surface; false = none"),
  fillPen: z.number().int().min(0).max(255).optional().describe("Pen of the 3D hatch (0 = use the surface color)"),
  texture: z.union([SurfaceTexture, z.literal(false)]).optional().describe("Texture picture; false removes it"),
};

// --- Zone categories -----------------------------------------------------------

export const ZoneCategoryFields = {
  code: z.string().max(255).optional().describe("Category code shown in zone stamps, e.g. 'LIV'"),
  color: Color.optional().describe("Zone fill color on the plan"),
  stamp: LibPartRef.optional().describe("Zone stamp library part (localized name, use search_library_parts); resets the stamp parameters to its defaults"),
};

// --- Profiles ----------------------------------------------------------------

export const ProfileUsage = z
  .object({
    walls: z.boolean().optional(),
    beams: z.boolean().optional(),
    columns: z.boolean().optional(),
    handrails: z.boolean().optional(),
    otherObjects: z.boolean().optional().describe("Other GDL-based objects (railings etc.)"),
  })
  .describe("Element types that may use the profile (new profiles: walls, beams, columns)");

export const ProfileShape = z.object({
  polygon: Polygon.describe(
    "Cross-section polygon in meters, x = horizontal, y = vertical (for walls y is the height direction); straight edges only; holes allowed",
  ),
  buildingMaterial: AttrRef.describe("Building material of this part (fill, pens, surface come from it)"),
  core: z.boolean().optional().describe("Core part (default true)"),
  finish: z.boolean().optional().describe("Finish part (default false)"),
  contourVisible: z.boolean().optional().describe("Draw the contour lines (default true)"),
  contourPen: PenIndex.optional().describe("Contour pen (default: the material's cut fill pen)"),
  contourLineType: AttrRef.optional().describe("Contour line type (default 1 = solid)"),
  cutEndLinePen: PenIndex.optional(),
  cutEndLineType: AttrRef.optional(),
});

export const ProfileFields = {
  usage: ProfileUsage.optional(),
  shapes: z
    .array(ProfileShape)
    .min(1)
    .optional()
    .describe("Cross-section parts (replaces the whole geometry). Required for new profiles unless basedOn is given"),
};

// --- MEP systems / operation profiles -----------------------------------------

export const MEPSystemFields = {
  ductwork: z.boolean().optional().describe("Usable in the Ductwork domain"),
  pipework: z.boolean().optional().describe("Usable in the Pipework domain"),
  cabling: z.boolean().optional().describe("Usable in the Cabling domain"),
  contourPen: PenIndex.optional(),
  fillPen: PenIndex.optional(),
  fillBackgroundPen: BackgroundPen.optional(),
  centerLinePen: PenIndex.optional(),
  fill: AttrRef.optional().describe("Fill attribute"),
  centerLineType: AttrRef.optional().describe("Center line type"),
  surface: AttrRef.optional().describe("Body surface"),
  insulationSurface: AttrRef.optional().describe("Insulation surface"),
};

export const OperationProfileFields = {
  occupancyType: z.enum(["Residential", "NonResidential"]).optional(),
  hotWaterLoad: z.number().min(0).optional().describe("Hot water load per capita"),
  humanHeatGain: z.number().min(0).optional().describe("Human heat gain per capita"),
  humidity: z.number().min(0).max(100).optional().describe("Humidity load"),
};

/** Type-specific field shapes (documented in detail on the create_* tools). */
export const FIELDS_BY_TYPE: Record<string, z.ZodRawShape> = {
  Layer: LayerFields,
  LayerCombination: LayerCombinationFields,
  Composite: CompositeFields,
  BuildingMaterial: BuildingMaterialFields,
  Surface: SurfaceFields,
  Fill: FillFields,
  Line: LineTypeFields,
  ZoneCategory: ZoneCategoryFields,
  Pen: PenFields,
  PenTable: PenTableFields,
  Profile: ProfileFields,
  MEPSystem: MEPSystemFields,
  OperationProfile: OperationProfileFields,
  DimensionStandard: {},
  ModelViewOption: {},
};

/** "Layer: hidden, visible, ...; Composite: skins, ..." — generated from the shapes so it never drifts. */
export function fieldSummary(): string {
  return Object.entries(FIELDS_BY_TYPE)
    .map(([type, shape]) => `${type}: ${Object.keys(shape).join(", ") || "(rename/folder only)"}`)
    .join("; ");
}

export const ModifiableAttributeType = z
  .enum(ATTRIBUTE_TYPES.filter((t) => t !== "Font") as [string, ...string[]])
  .describe("Attribute type (fonts cannot be modified)");

/**
 * One modify item. Type-specific fields pass through to the add-on, which validates them (unknown
 * fields are rejected with the list of allowed ones); a full per-type union schema would cost the
 * model ~8k tokens of tool definition for no extra information over the create_* tools.
 */
export const ModifyItem = z
  .object({
    type: ModifiableAttributeType,
    ...ModifyCommon,
  })
  .passthrough()
  .superRefine((item, ctx) => {
    const allowed = new Set(["type", "attribute", "name", "folder", ...Object.keys(FIELDS_BY_TYPE[item.type] ?? {})]);
    if (item.type === "Pen") {
      allowed.delete("name");
      allowed.delete("folder");
      if (typeof item.attribute !== "number" || item.attribute < 1 || item.attribute > 255) {
        ctx.addIssue({ code: z.ZodIssueCode.custom, path: ["attribute"], message: "a pen is referenced by its index 1-255" });
      }
    }
    const unknown = Object.keys(item).filter((k) => !allowed.has(k));
    if (unknown.length > 0) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        message: `unknown field(s) for ${item.type}: ${unknown.join(", ")}. Allowed: ${[...allowed].join(", ")}`,
      });
    }
    const shape = FIELDS_BY_TYPE[item.type];
    if (shape) {
      const parsed = z.object(shape).safeParse(Object.fromEntries(Object.entries(item).filter(([k]) => k in shape)));
      if (!parsed.success) {
        for (const issue of parsed.error.issues) ctx.addIssue({ ...issue, path: issue.path });
      }
    }
  })
  .describe(
    "{type, attribute, name?, folder?, ...fields to change}. Fields per type (same meaning/units as in the create_* tools and " +
      "get_attributes output): " +
      fieldSummary(),
  );

/** Normalizes an attribute reference to the object form the add-on reads from lists. */
export function refObject(ref: z.infer<typeof AttrRef>): Record<string, unknown> {
  if (typeof ref === "number") return { index: ref };
  if (typeof ref === "string") return { name: ref };
  return ref as Record<string, unknown>;
}
