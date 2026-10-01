/**
 * Property tools: property groups & definitions (built-in + user-defined, option sets, expressions,
 * availability), property values of elements / tool defaults / attributes, IFC data, and
 * classification system authoring.
 *
 * Backed by the add-on commands in addon/Src/Commands/Properties*.cpp:
 *   GetPropertyDefinitions, Create/Modify/DeletePropertyGroups, Create/Modify/DeletePropertyDefinitions,
 *   ImportPropertyDefinitionsXml, Get/SetPropertyValues, Get/SetAttributePropertyValues, GetIfcData,
 *   SetIfcProperties, CreateClassificationSystem, CreateClassificationItems, ModifyClassificationSystem,
 *   ModifyClassificationItems, DeleteClassificationItems, DeleteClassificationSystems, ImportClassificationsXml.
 * Classifying ELEMENTS (get/set element classification) is done with the official-API tools.
 */

import { readFile } from "node:fs/promises";

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import { CREATES, defineTool, DESTRUCTIVE, MODIFIES, READ_ONLY, type ToolContext } from "./define.js";
import { ElementRef } from "./schemas.js";
import {
  AttributeTarget,
  Availability,
  AvailabilityItemRef,
  builtInNamesByGuid,
  ClassificationItemRef,
  ClassificationItemSpec,
  ClassificationSystemRef,
  compact,
  elementGuids,
  ElementTypeName,
  EnumOption,
  expandBuiltInExpressionTokens,
  failOnProblems,
  PropertyGroupRef,
  PropertyRef,
  PropertyType,
  PropertyValue,
  resolveBuiltInRef,
  resolveBuiltInRefs,
  ScalarType,
} from "./properties-schemas.js";

const ElementList = z.array(ElementRef).min(1).max(10000);
const UndoName = z.string().optional().describe("Name of the undo step shown in Archicad");

const VALUE_UNITS =
  "Units: lengths m, areas m², volumes m³, angles DEGREES; option sets by display value.";

const CELL_NOTE =
  "Each cell is {value, display?, isDefault?} (display = Archicad's formatted text incl. units, only when it differs from value; " +
  "isDefault only for user-defined properties: true = the element has no own value and shows the default/expression) or " +
  "{status: 'NotAvailable' (property not available for this element/classification) | 'NotEvaluated' | 'Undefined' (value set to " +
  "Undefined) | 'Empty'}.";

const EnumValueType = ScalarType.describe(
  "Value type of the options of a singleEnum/multiEnum (default string; Archicad's UI option sets are strings)",
);

// Fields shared by create and modify of definitions.
const DefaultFields = {
  defaultValue: PropertyValue.optional().describe(
    "Default value shown on every element that has no own value. " + VALUE_UNITS + " Default when omitted on create: Undefined.",
  ),
  defaultExpressions: z
    .array(z.string().min(1))
    .min(1)
    .optional()
    .describe(
      "Makes the property EXPRESSION-BASED (calculated, read-only on elements): Archicad property expressions (operators + - * / " +
        "and functions such as CONCAT, ROUND, IF, ...). Reference other properties with placeholders that are " +
        "converted to Archicad's reference syntax: {builtIn:General_Area} (built-ins, language independent), {ref:Group/Name} or " +
        "{ref:GUID} (any property), e.g. [\"{builtIn:General_Area} * 1.1\"] or [\"CONCAT ( {ref:CC-Group/Code}, \\\"-\\\", " +
        "{builtIn:General_ElementID} )\"]. Archicad's own syntax (expressionReference from get_property_definitions {detail: 'full'}) " +
        "also works. The property type must match the expression result (area expression -> type area). Several expressions = " +
        "alternatives evaluated in order (first applicable wins).",
    ),
};

const AvailabilityFields = {
  addAvailability: z.array(AvailabilityItemRef).min(1).optional().describe("Add these classification items (or {system} = all its items) to the availability"),
  availableForElements: ElementList.optional().describe(
    "Make the property available for these elements: adds their current classification items to the availability " +
      "(elements without classification produce a warning — classify them first)",
  ),
};

export function registerPropertyTools(server: McpServer, ctx: ToolContext): void {
  // --- Definitions (read) ---------------------------------------------------------------

  defineTool(server, ctx, {
    name: "get_property_definitions",
    title: "Get property definitions",
    description:
      "Lists property definitions — built-in (ID, areas, volumes, heights, ...) and user-defined (custom) — so you can address them in " +
      "get_property_values / set_property_values. Summary per definition: {guid, name, group, type (string|integer|number|length|area|" +
      "volume|angle|boolean|<type>List|singleEnum|multiEnum), kind (Custom|BuiltIn|DynamicBuiltIn), editable, description?, " +
      "expressionBased?, enumValues? (options of option sets), availabilityCount? (custom), builtInName? (language-independent name of " +
      "built-ins, e.g. General_ElementID)}. detail 'full' adds groupGuid, collectionType/valueType/measureType, defaultValue or " +
      "defaultExpressions, enumOptions with keys, expressionReference (for expressions) and availability (classification items). " +
      "Names are LOCALIZED (Russian Archicad: Russian group and property names) — use search with a Russian word, or builtInName for " +
      "built-ins. Filter with search / groups / kind, or " +
      "with elements / elementTypes to get only what is available for them. Returns {definitions, total, offset, returned, hasMore, groups?}.",
    input: {
      search: z.string().min(1).optional().describe("Case-insensitive substring of 'Group/Name' or the description, e.g. 'огнест' or 'Площадь'"),
      groups: z.array(PropertyGroupRef).min(1).optional().describe("Only definitions in these groups"),
      properties: z.array(PropertyRef).min(1).optional().describe("Only these definitions (resolve references to full details)"),
      kind: z.enum(["All", "Custom", "BuiltIn"]).optional().describe("All (default), Custom (user-defined) or BuiltIn"),
      elements: ElementList.optional().describe("Only definitions available for these elements (see elementMatch)"),
      elementTypes: z.array(ElementTypeName).min(1).optional().describe("Only definitions available for the tool defaults of these element types"),
      elementMatch: z.enum(["all", "any"]).optional().describe("With elements/elementTypes: available for all of them (default) or any"),
      detail: z.enum(["summary", "full"]).optional().describe("summary (default) or full"),
      includeAvailability: z.boolean().optional().describe("Resolve availability to classification items {guid, id, name?, system} (default: true with detail full)"),
      includeGroups: z.boolean().optional().describe("Also return all property groups {guid, name, kind, description?, definitionCount}"),
      includeBuiltInNames: z.boolean().optional().describe("Add builtInName to built-in definitions (default true; 2 extra official API calls, cached)"),
      offset: z.number().int().min(0).optional().describe("Skip this many definitions"),
      limit: z.number().int().min(1).max(2000).optional().describe("Return at most this many (default 500)"),
    },
    annotations: READ_ONLY,
    handler: async (args, { ac }) => {
      const { includeBuiltInNames, elements, properties, ...rest } = args;
      const params: Record<string, unknown> = compact({ ...rest, elements: elementGuids(elements) });
      if (properties) params["properties"] = await resolveBuiltInRefs(ac, properties);
      const result = await ac.addon<{ definitions?: Array<Record<string, unknown>> }>("GetPropertyDefinitions", params);
      if (includeBuiltInNames !== false && result.definitions?.some((d) => d["kind"] !== "Custom")) {
        try {
          const names = await builtInNamesByGuid(ac);
          for (const d of result.definitions) {
            if (d["kind"] === "Custom" || typeof d["guid"] !== "string") continue;
            const name = names.get(d["guid"].toUpperCase());
            if (name) d["builtInName"] = name;
          }
        } catch {
          /* names are a convenience only */
        }
      }
      return result;
    },
  });

  // --- Groups -----------------------------------------------------------------------------------

  defineTool(server, ctx, {
    name: "create_property_groups",
    title: "Create property groups",
    description:
      "Creates user-defined property groups (the folders of the Property Manager) in one undo step. An existing custom group with the " +
      "same name is returned with alreadyExisted: true (idempotent). create_property_definitions also creates missing groups itself. " +
      "Returns {results: [{guid, name, alreadyExisted?} | {error}]}.",
    input: {
      groups: z
        .array(z.object({ name: z.string().min(1).max(255).describe("Group name"), description: z.string().optional() }))
        .min(1)
        .max(500),
      undoName: UndoName,
    },
    annotations: CREATES,
    handler: async (args, { ac }) => ac.addon("CreatePropertyGroups", compact(args)),
  });

  defineTool(server, ctx, {
    name: "modify_property_groups",
    title: "Modify property groups",
    description: "Renames user-defined property groups and/or changes their description (built-in groups are read-only). One undo step.",
    input: {
      groups: z
        .array(
          z.object({
            group: PropertyGroupRef.describe("Group to change (current name or GUID)"),
            name: z.string().min(1).max(255).optional().describe("New name"),
            description: z.string().optional().describe("New description"),
          }),
        )
        .min(1)
        .max(500),
      undoName: UndoName,
    },
    annotations: MODIFIES,
    handler: async (args, { ac }) => {
      failOnProblems(
        args.groups.flatMap((g, i) => (g.name === undefined && g.description === undefined ? [`groups[${i}]: give name and/or description`] : [])),
      );
      return ac.addon("ModifyPropertyGroups", compact(args));
    },
  });

  defineTool(server, ctx, {
    name: "delete_property_groups",
    title: "Delete property groups",
    description:
      "Deletes user-defined property groups in one undo step (undoable with undo). A group that still contains definitions is refused " +
      "unless deleteDefinitions is true — then its definitions and all their element values are deleted too. Built-in groups cannot be deleted.",
    input: {
      groups: z.array(PropertyGroupRef).min(1).max(500),
      deleteDefinitions: z.boolean().optional().describe("Also delete the definitions inside (default false = refuse non-empty groups)"),
      undoName: UndoName,
    },
    annotations: DESTRUCTIVE,
    handler: async (args, { ac }) => ac.addon("DeletePropertyGroups", compact(args)),
  });

  // --- Definitions (write) ---------------------------------------------------------------------

  defineTool(server, ctx, {
    name: "create_property_definitions",
    title: "Create property definitions",
    description:
      "Creates user-defined (custom) properties in one undo step — like the Property Manager: any value type, option sets " +
      "(singleEnum/multiEnum with enumValues), default values or expression-based defaults, and availability per classification. " +
      "IMPORTANT: custom properties appear only on elements whose classification is in the availability — the default 'all' " +
      "makes it available for every classification item that exists now (elements with no classification never show custom " +
      "properties). Missing groups are created (createMissingGroups, default true). Then set values with set_property_values. " +
      "Returns {results: [{guid, name, group, type, availabilityCount, warnings?} | {error}]} in input order.",
    input: {
      definitions: z
        .array(
          z.object({
            group: PropertyGroupRef.describe("Custom group (name or GUID); a new name creates the group"),
            name: z.string().min(1).max(255).describe("Property name (unique within the group)"),
            type: PropertyType,
            description: z.string().optional().describe("Description shown in the Property Manager / tooltips"),
            enumValues: z.array(EnumOption).min(1).max(1000).optional().describe("Options — REQUIRED for singleEnum/multiEnum, not allowed otherwise"),
            enumValueType: EnumValueType.optional(),
            ...DefaultFields,
            availability: Availability.optional().describe(
              "Classifications the property is available for: 'all' (default when neither availability, addAvailability nor " +
                "availableForElements is given), 'none', or item refs / {system}",
            ),
            ...AvailabilityFields,
          }),
        )
        .min(1)
        .max(500),
      createMissingGroups: z.boolean().optional().describe("Create groups that do not exist yet (default true)"),
      undoName: UndoName,
    },
    annotations: CREATES,
    handler: async (args, { ac }) => {
      const problems: string[] = [];
      args.definitions.forEach((d, i) => {
        const isEnum = d.type === "singleEnum" || d.type === "multiEnum";
        if (isEnum && !d.enumValues) problems.push(`definitions[${i}] (${d.name}): type ${d.type} needs enumValues`);
        if (!isEnum && d.enumValues) problems.push(`definitions[${i}] (${d.name}): enumValues only apply to singleEnum/multiEnum`);
        if (!isEnum && d.enumValueType) problems.push(`definitions[${i}] (${d.name}): enumValueType only applies to singleEnum/multiEnum`);
        if (d.defaultValue !== undefined && d.defaultExpressions) problems.push(`definitions[${i}] (${d.name}): give defaultValue OR defaultExpressions`);
        if (d.availability !== undefined && d.addAvailability) problems.push(`definitions[${i}] (${d.name}): use availability OR addAvailability`);
      });
      failOnProblems(problems);
      const definitions = await Promise.all(
        args.definitions.map(async (d) =>
          compact({
            ...d,
            defaultExpressions: d.defaultExpressions ? await expandBuiltInExpressionTokens(ac, d.defaultExpressions) : undefined,
            availableForElements: elementGuids(d.availableForElements),
          }),
        ),
      );
      return ac.addon("CreatePropertyDefinitions", compact({ ...args, definitions }));
    },
  });

  defineTool(server, ctx, {
    name: "modify_property_definitions",
    title: "Modify property definitions",
    description:
      "Changes user-defined properties in one undo step: rename, move to another group, description, option set options (replace " +
      "the list — unchanged options keep their identity so element values survive — or add/remove/rename options), default value " +
      "or expressions, and availability (replace, add, remove, or add the classifications of given elements). Built-in properties " +
      "and the value type cannot be changed (delete and recreate instead). Returns {results: [{guid, name, group, type, " +
      "availabilityCount, warnings?} | {error}]}.",
    input: {
      definitions: z
        .array(
          z.object({
            property: PropertyRef.describe("User-defined property to change"),
            name: z.string().min(1).max(255).optional().describe("New name"),
            description: z.string().optional().describe("New description"),
            group: PropertyGroupRef.optional().describe("Move into this custom group"),
            enumValues: z.array(EnumOption).min(1).max(1000).optional().describe("Replace the options (options with the same display value keep their key)"),
            addEnumValues: z.array(EnumOption).min(1).optional().describe("Append options"),
            removeEnumValues: z.array(z.union([z.string(), z.number(), z.boolean()])).min(1).optional().describe("Remove options (by display value)"),
            renameEnumValues: z
              .array(z.object({ from: z.union([z.string(), z.number()]), to: z.union([z.string(), z.number()]) }))
              .min(1)
              .optional()
              .describe("Rename options, keeping element values: [{from: 'old', to: 'new'}]"),
            ...DefaultFields,
            defaultUndefined: z.boolean().optional().describe("true = reset the default to Undefined (and remove expressions)"),
            availability: Availability.optional().describe("Replace the availability: 'all', 'none' or item refs / {system}"),
            removeAvailability: z
              .union([z.literal("all"), z.array(AvailabilityItemRef).min(1)])
              .optional()
              .describe("Remove these items from the availability ('all' = clear)"),
            ...AvailabilityFields,
          }),
        )
        .min(1)
        .max(500),
      undoName: UndoName,
    },
    annotations: MODIFIES,
    handler: async (args, { ac }) => {
      const problems: string[] = [];
      args.definitions.forEach((d, i) => {
        const defaults = [d.defaultValue !== undefined, d.defaultExpressions !== undefined, d.defaultUndefined === true].filter(Boolean).length;
        if (defaults > 1) problems.push(`definitions[${i}]: give only one of defaultValue, defaultExpressions, defaultUndefined`);
        const changes = Object.keys(d).filter((k) => k !== "property" && (d as Record<string, unknown>)[k] !== undefined);
        if (changes.length === 0) problems.push(`definitions[${i}]: nothing to change`);
      });
      failOnProblems(problems);
      const definitions = await Promise.all(
        args.definitions.map(async (d) =>
          compact({
            ...d,
            property: await resolveBuiltInRef(ac, d.property),
            defaultExpressions: d.defaultExpressions ? await expandBuiltInExpressionTokens(ac, d.defaultExpressions) : undefined,
            availableForElements: elementGuids(d.availableForElements),
          }),
        ),
      );
      return ac.addon("ModifyPropertyDefinitions", compact({ ...args, definitions }));
    },
  });

  defineTool(server, ctx, {
    name: "delete_property_definitions",
    title: "Delete property definitions",
    description:
      "Deletes user-defined property definitions in one undo step — their values on ALL elements are lost (undo restores them). " +
      "Built-in properties cannot be deleted. Returns {results: [{guid, name, group, type, deleted} | {error}]}.",
    input: {
      properties: z.array(PropertyRef).min(1).max(1000),
      undoName: UndoName,
    },
    annotations: DESTRUCTIVE,
    handler: async (args, { ac }) =>
      ac.addon("DeletePropertyDefinitions", compact({ ...args, properties: await resolveBuiltInRefs(ac, args.properties) })),
  });

  defineTool(server, ctx, {
    name: "import_property_definitions_xml",
    title: "Import property definitions (XML)",
    description:
      "Imports property groups and definitions from an Archicad property XML file (the Property Manager's Export format, " +
      "<BuildingInformation><PropertyDefinitionGroups>...). Give the XML text or a local file path. One undo step. " +
      "Returns {created: [{guid, name, group, type}], groupsCreated, definitionCount}.",
    input: {
      xml: z.string().min(1).optional().describe("XML content"),
      filePath: z.string().min(1).optional().describe("Absolute path of an .xml file on this computer (alternative to xml)"),
      conflictPolicy: z
        .enum(["Skip", "Replace", "Append"])
        .optional()
        .describe("When a definition with the same name exists: Skip (default, keep existing), Replace (overwrite), Append (add alongside)"),
      undoName: UndoName,
    },
    annotations: CREATES,
    handler: async ({ xml, filePath, ...rest }, { ac }) => {
      if ((xml === undefined) === (filePath === undefined)) throw new Error("Invalid input: give exactly one of xml or filePath");
      const content = xml ?? (await readFile(filePath!, "utf8"));
      return ac.addon("ImportPropertyDefinitionsXml", compact({ ...rest, xml: content }));
    },
  });

  // --- Values -----------------------------------------------------------------------------------------

  defineTool(server, ctx, {
    name: "get_property_values",
    title: "Get property values",
    description:
      "Reads property values (built-in and user-defined) of many elements — and/or of element TOOL DEFAULTS — as one table: " +
      "{properties: [{guid, name, group, type}], results: [{guid | elementType, values: [cell per property, same order]} | " +
      "{guid, error}]}. " +
      CELL_NOTE +
      " " +
      VALUE_UNITS +
      " Without properties: every property available for the targets (scope UserDefined by default; BuiltIn/All can be hundreds). " +
      "Address built-ins language-independently with {builtIn: 'General_ElementID'} etc.",
    input: {
      elements: ElementList.optional().describe("Elements to read"),
      elementDefaults: z.array(ElementTypeName).min(1).optional().describe("Read the tool default settings of these element types"),
      properties: z.array(PropertyRef).min(1).max(500).optional().describe("Properties to read (columns). Omit = all available (see scope)"),
      scope: z.enum(["UserDefined", "BuiltIn", "All"]).optional().describe("Used only without properties: which properties to return (default UserDefined)"),
      includeDisplay: z.boolean().optional().describe("Add Archicad's formatted display text (default true)"),
    },
    annotations: READ_ONLY,
    handler: async (args, { ac }) => {
      if (!args.elements && !args.elementDefaults) throw new Error("Invalid input: give elements and/or elementDefaults");
      const params = compact({
        ...args,
        elements: elementGuids(args.elements),
        properties: args.properties ? await resolveBuiltInRefs(ac, args.properties) : undefined,
      });
      return ac.addon("GetPropertyValues", params);
    },
  });

  const ValueMode = {
    value: PropertyValue.optional().describe("New value. " + VALUE_UNITS),
    reset: z.boolean().optional().describe("true = remove the element's own value so it shows the default / expression again (user-defined properties)"),
    setUndefined: z.boolean().optional().describe("true = set the value to Undefined (empty)"),
  };

  function checkModes(entries: Array<{ value?: unknown; reset?: boolean; setUndefined?: boolean }>, problems: string[]): void {
    entries.forEach((e, i) => {
      const n = [e.value !== undefined, e.reset === true, e.setUndefined === true].filter(Boolean).length;
      if (n !== 1) problems.push(`values[${i}]: give exactly one of value, reset: true, setUndefined: true`);
    });
  }

  defineTool(server, ctx, {
    name: "set_property_values",
    title: "Set property values",
    description:
      "Sets property values on many elements (and/or element tool defaults) in ONE undo step. Each entry sets one property on a " +
      "list of elements: {elements, property, value} — or reset: true (back to the default/expression) or setUndefined: true. " +
      "Works for user-defined properties and editable built-ins (e.g. {builtIn: 'General_ElementID'} = Element ID). " +
      VALUE_UNITS +
      " Fails per element (reported, others continue) when the property is not available for the element's classification (fix: " +
      "modify_property_definitions {availableForElements}) or read-only (calculated / expression-based). Returns {results: [{property, " +
      "guid, succeeded, failed?: [{guid|elementType, error}]} | {error}], succeeded, failed}.",
    input: {
      values: z
        .array(
          z.object({
            elements: ElementList.optional().describe("Elements to change"),
            elementDefaults: z.array(ElementTypeName).min(1).optional().describe("Also/instead change the tool defaults of these element types"),
            property: PropertyRef,
            ...ValueMode,
          }),
        )
        .min(1)
        .max(2000),
      undoName: UndoName,
    },
    annotations: MODIFIES,
    handler: async (args, { ac }) => {
      const problems: string[] = [];
      args.values.forEach((e, i) => {
        if (!e.elements && !e.elementDefaults) problems.push(`values[${i}]: give elements and/or elementDefaults`);
      });
      checkModes(args.values, problems);
      failOnProblems(problems);
      const resolved = await resolveBuiltInRefs(ac, args.values.map((v) => v.property));
      const values = args.values.map((v, i) => compact({ ...v, property: resolved[i], elements: elementGuids(v.elements) }));
      return ac.addon("SetPropertyValues", compact({ ...args, values }));
    },
  });

  defineTool(server, ctx, {
    name: "get_attribute_property_values",
    title: "Get attribute property values",
    description:
      "Reads property values of ATTRIBUTES — mainly building materials (properties/classifications of building materials, e.g. " +
      "thermal or product data) — as a table like get_property_values: {properties: [...], results: [{attribute: {type, index, name, " +
      "guid}, values: [...]}]}. " +
      CELL_NOTE +
      " Without properties: all available ones (scope UserDefined default).",
    input: {
      attributes: z.array(AttributeTarget).min(1).max(1000).describe("Attributes, e.g. [{type: 'BuildingMaterial', attribute: 'Бетон - Конструкционный'}]"),
      properties: z.array(PropertyRef).min(1).max(500).optional(),
      scope: z.enum(["UserDefined", "BuiltIn", "All"]).optional().describe("Used only without properties (default UserDefined)"),
      includeDisplay: z.boolean().optional().describe("Add formatted display text (default true)"),
    },
    annotations: READ_ONLY,
    handler: async (args, { ac }) =>
      ac.addon(
        "GetAttributePropertyValues",
        compact({ ...args, properties: args.properties ? await resolveBuiltInRefs(ac, args.properties) : undefined }),
      ),
  });

  defineTool(server, ctx, {
    name: "set_attribute_property_values",
    title: "Set attribute property values",
    description:
      "Sets property values of attributes (building materials, ...) in one undo step. Each entry: {attributes: [{type, attribute}], " +
      "property, value | reset: true | setUndefined: true}. The property must be available for the attribute's classification. " +
      VALUE_UNITS,
    input: {
      values: z
        .array(z.object({ attributes: z.array(AttributeTarget).min(1).max(1000), property: PropertyRef, ...ValueMode }))
        .min(1)
        .max(1000),
      undoName: UndoName,
    },
    annotations: MODIFIES,
    handler: async (args, { ac }) => {
      const problems: string[] = [];
      checkModes(args.values, problems);
      failOnProblems(problems);
      const resolved = await resolveBuiltInRefs(ac, args.values.map((v) => v.property));
      const values = args.values.map((v, i) => compact({ ...v, property: resolved[i] }));
      return ac.addon("SetAttributePropertyValues", compact({ ...args, values }));
    },
  });

  // --- IFC ------------------------------------------------------------------------------------------------

  const IfcScalar = z.union([z.string(), z.number(), z.boolean()]);
  const IfcValue = z
    .union([IfcScalar, z.object({ value: IfcScalar, valueType: z.string().optional().describe("IFC type, e.g. IfcLabel") })])
    .describe("IFC value: plain value (type from valueType or inferred) or {value, valueType}");
  const IfcValueType = z
    .string()
    .min(3)
    .describe(
      "IFC value type: IfcLabel, IfcText, IfcIdentifier, IfcBoolean, IfcLogical, IfcInteger, IfcReal, IfcCountMeasure, " +
        "IfcLengthMeasure, IfcPositiveLengthMeasure, IfcAreaMeasure, IfcVolumeMeasure, IfcPlaneAngleMeasure, IfcMassMeasure, " +
        "IfcThermalTransmittanceMeasure, ... (default inferred: string→IfcLabel, integer→IfcInteger, number→IfcReal, boolean→IfcBoolean)",
    );

  defineTool(server, ctx, {
    name: "get_ifc_data",
    title: "Get IFC data",
    description:
      "Returns IFC data of elements: ifcGlobalId (22-char IFC GUID used on export), archicadIfcId, externalIfcGlobalId (elements " +
      "imported from IFC), ifcType (e.g. IfcWall) and typeObjectIfcType, IFC properties grouped by property set ({propertySet, name, " +
      "type Single|List|Bounded|Enumerated|Table, value/values/lower/upper/options, valueType, readOnly?}), IFC attributes (Name, " +
      "Description, ObjectType, Tag, ...) and IFC classification references. Can also FIND elements by IFC GlobalId (ifcGlobalIds). " +
      "Returns {elements: [{guid, ...} | {guid, error}], lookup?: [{ifcGlobalId, elements}]}.",
    input: {
      elements: ElementList.optional(),
      ifcGlobalIds: z.array(z.string().min(1)).min(1).max(1000).optional().describe("Find elements by IFC GlobalId (22 characters) and include them"),
      include: z
        .array(z.enum(["identity", "type", "properties", "attributes", "classificationReferences"]))
        .min(1)
        .optional()
        .describe("Parts to return (default all)"),
      propertySets: z.array(z.string().min(1)).min(1).optional().describe("Only IFC properties of these property sets, e.g. ['Pset_WallCommon']"),
      storedOnly: z
        .boolean()
        .optional()
        .describe("true = only data stored on the element; false (default) = also properties/attributes the IFC translator would export"),
    },
    annotations: READ_ONLY,
    handler: async (args, { ac }) => {
      if (!args.elements && !args.ifcGlobalIds) throw new Error("Invalid input: give elements and/or ifcGlobalIds");
      return ac.addon("GetIfcData", compact({ ...args, elements: elementGuids(args.elements) }));
    },
  });

  defineTool(server, ctx, {
    name: "set_ifc_properties",
    title: "Set IFC properties / attributes",
    description:
      "Adds or changes IFC properties stored on elements (custom psets such as 'CC_Pset' or standard ones like Pset_WallCommon), " +
      "removes stored IFC properties, sets IFC attributes (Name, Description, ObjectType, Tag, LongName, ...) and adds/removes " +
      "stored IFC classification references (IfcClassificationReference) — one undo step. " +
      "Property kinds: Single {value}, List {values}, Bounded {lower?, upper?}, Enumerated {values (selected), options}, Table " +
      "{definingValues, definedValues}. Values are written as given (no unit conversion). Returns {properties?, attributes?, " +
      "classificationReferences?} with per-entry {succeeded, failed?: [{guid, error}]} | {error}. " +
      "Read back with get_ifc_data {storedOnly: true}.",
    input: {
      properties: z
        .array(
          z.object({
            elements: ElementList,
            propertySet: z.string().min(1).describe("Property set name, e.g. 'Pset_WallCommon' or a custom pset"),
            name: z.string().min(1).describe("IFC property name, e.g. 'FireRating'"),
            remove: z.boolean().optional().describe("true = remove this stored IFC property from the elements (no values needed)"),
            type: z.enum(["Single", "List", "Bounded", "Enumerated", "Table"]).optional().describe("Default: Single (List when values, Bounded when lower/upper, Enumerated when options)"),
            valueType: IfcValueType.optional(),
            value: IfcValue.optional().describe("Single value"),
            values: z.array(IfcValue).min(1).optional().describe("List values, or the selected values of an Enumerated property"),
            lower: IfcValue.optional().describe("Bounded: lower bound"),
            upper: IfcValue.optional().describe("Bounded: upper bound"),
            options: z.array(IfcValue).min(1).optional().describe("Enumerated: the allowed values (enumeration reference)"),
            definingValues: z.array(IfcValue).min(1).optional().describe("Table: defining values"),
            definedValues: z.array(IfcValue).min(1).optional().describe("Table: defined values (same length)"),
            description: z.string().optional(),
          }),
        )
        .min(1)
        .max(1000)
        .optional(),
      attributes: z
        .array(
          z.object({
            elements: ElementList,
            name: z.string().min(1).describe("IFC attribute name: Name, Description, ObjectType, Tag, LongName, PredefinedType, ..."),
            value: z.union([z.string(), z.number(), z.boolean()]).optional().describe("New value (stored as text)"),
            clear: z.boolean().optional().describe("true = remove the value"),
          }),
        )
        .min(1)
        .max(1000)
        .optional(),
      classificationReferences: z
        .array(
          z.object({
            elements: ElementList,
            referenceName: z
              .string()
              .min(1)
              .optional()
              .describe("Key of the reference on the element (default: identification, else name); used to update/remove it"),
            identification: z.string().optional().describe("Item code in the classification, e.g. 'Ss_25_10_30' (IFC Identification/ItemReference)"),
            name: z.string().optional().describe("Item name, e.g. 'Brick walling systems'"),
            location: z.string().optional().describe("URL of the item"),
            source: z
              .object({
                name: z.string().min(1).describe("Classification system name, e.g. 'Uniclass'"),
                source: z.string().optional().describe("Publisher / source"),
                edition: z.string().optional().describe("Edition, e.g. '2015'"),
                editionDate: z.string().optional().describe("Edition date text, e.g. '2015-01-01'"),
                description: z.string().optional(),
                location: z.string().optional().describe("URL of the classification system"),
              })
              .optional()
              .describe("The classification system (IfcClassification) — required unless remove"),
            remove: z.boolean().optional().describe("true = remove the stored reference with this referenceName/identification (source.name narrows the match)"),
          }),
        )
        .min(1)
        .max(1000)
        .optional()
        .describe(
          "IFC classification references STORED on elements (exported as IfcRelAssociatesClassification). References derived from " +
            "Archicad classifications are read-only here — classify elements with set_element_classifications instead.",
        ),
      undoName: UndoName,
    },
    annotations: MODIFIES,
    handler: async (args, { ac }) => {
      const problems: string[] = [];
      if (!args.properties && !args.attributes && !args.classificationReferences) {
        problems.push("give properties, attributes and/or classificationReferences");
      }
      args.classificationReferences?.forEach((r, i) => {
        if (r.referenceName === undefined && r.identification === undefined && r.name === undefined) {
          problems.push(`classificationReferences[${i}]: give referenceName, identification or name`);
        }
        if (!r.remove && !r.source) problems.push(`classificationReferences[${i}]: source {name, ...} is required (unless remove: true)`);
      });
      args.properties?.forEach((p, i) => {
        if (p.remove) return;
        const kind = p.type ?? (p.lower !== undefined || p.upper !== undefined ? "Bounded" : p.options ? "Enumerated" : p.values ? "List" : "Single");
        if (kind === "Single" && p.value === undefined) problems.push(`properties[${i}]: a Single property needs value`);
        if ((kind === "List" || kind === "Enumerated") && !p.values) problems.push(`properties[${i}]: a ${kind} property needs values`);
        if (kind === "Bounded" && p.lower === undefined && p.upper === undefined) problems.push(`properties[${i}]: a Bounded property needs lower and/or upper`);
        if (kind === "Table" && (!p.definingValues || !p.definedValues)) problems.push(`properties[${i}]: a Table property needs definingValues and definedValues`);
      });
      args.attributes?.forEach((a, i) => {
        if ((a.value === undefined) === (a.clear !== true)) problems.push(`attributes[${i}]: give exactly one of value or clear: true`);
      });
      failOnProblems(problems);
      return ac.addon(
        "SetIfcProperties",
        compact({
          ...args,
          properties: args.properties?.map((p) => compact({ ...p, elements: elementGuids(p.elements) })),
          attributes: args.attributes?.map((a) => compact({ ...a, elements: elementGuids(a.elements) })),
          classificationReferences: args.classificationReferences?.map((r) => compact({ ...r, elements: elementGuids(r.elements) })),
        }),
      );
    },
  });

  // --- Classification authoring ------------------------------------------------------------------------

  const DateString = z
    .string()
    .regex(/^\d{4}-\d{2}-\d{2}$/, "use YYYY-MM-DD")
    .describe("Edition date YYYY-MM-DD");

  defineTool(server, ctx, {
    name: "create_classification_system",
    title: "Create classification system",
    description:
      "Creates a classification system (like the Classification Manager's 'New'), optionally with its whole item tree, in one undo " +
      "step. name + editionVersion must be unique. Items: [{id, name?, description?, children?: [...]}] (nested) — or flat with " +
      "parent: '<id of an item earlier in the list>'. Then classify elements with the official classification tools and use the items " +
      "in property availability. Returns {system: {guid, name, editionVersion, ...}, items: [{guid, id, path, parent?} | {id, error}], created, failed}.",
    input: {
      name: z.string().min(1).max(255).describe("System name, e.g. 'CC-test-System' or 'Uniclass 2015'"),
      editionVersion: z.string().min(1).optional().describe("Edition/version (default '1.0')"),
      description: z.string().optional(),
      source: z.string().optional().describe("Source, e.g. a URL"),
      editionDate: DateString.optional().describe("Edition date YYYY-MM-DD (default today)"),
      items: z.array(ClassificationItemSpec).min(1).max(5000).optional().describe("Items to create (tree)"),
      reuseExisting: z.boolean().optional().describe("If the system already exists, add the items to it instead of failing (default false)"),
      undoName: UndoName,
    },
    annotations: CREATES,
    handler: async (args, { ac }) => ac.addon("CreateClassificationSystem", compact(args)),
  });

  defineTool(server, ctx, {
    name: "create_classification_items",
    title: "Create classification items",
    description:
      "Adds classification items to an existing classification system in one undo step: nested {id, name?, description?, children?} " +
      "trees, under an optional parent, inserted at the end or before a sibling. Item IDs must be unique within the system. " +
      "Returns {system, items: [{guid, id, path, parent?} | {id, error}], created, failed}.",
    input: {
      system: ClassificationSystemRef,
      parent: ClassificationItemRef.optional().describe("Create the items under this item (default: at the root)"),
      items: z.array(ClassificationItemSpec).min(1).max(5000),
      undoName: UndoName,
    },
    annotations: CREATES,
    handler: async (args, { ac }) => ac.addon("CreateClassificationItems", compact(args)),
  });

  defineTool(server, ctx, {
    name: "modify_classification_items",
    title: "Modify classification items",
    description: "Changes the ID, name and/or description of classification items in one undo step. Returns {results: [{guid, id, name?, description?} | {error}]}.",
    input: {
      system: ClassificationSystemRef.optional().describe("System in which item IDs are looked up (needed when an ID exists in several systems)"),
      items: z
        .array(
          z.object({
            item: ClassificationItemRef.describe("Item to change"),
            id: z.string().min(1).optional().describe("New ID"),
            name: z.string().optional().describe("New name"),
            description: z.string().optional().describe("New description"),
          }),
        )
        .min(1)
        .max(5000),
      undoName: UndoName,
    },
    annotations: MODIFIES,
    handler: async (args, { ac }) => {
      failOnProblems(
        args.items.flatMap((it, i) =>
          it.id === undefined && it.name === undefined && it.description === undefined ? [`items[${i}]: give id, name and/or description`] : [],
        ),
      );
      return ac.addon("ModifyClassificationItems", compact(args));
    },
  });

  defineTool(server, ctx, {
    name: "delete_classification_items",
    title: "Delete classification items",
    description:
      "Deletes classification items together with all their children in one undo step (elements classified with them become " +
      "unclassified in that system). Returns {results: [{guid, id, deleted, deletedChildren} | {error}]}.",
    input: {
      system: ClassificationSystemRef.optional().describe("System in which item IDs are looked up"),
      items: z.array(ClassificationItemRef).min(1).max(5000),
      undoName: UndoName,
    },
    annotations: DESTRUCTIVE,
    handler: async (args, { ac }) => ac.addon("DeleteClassificationItems", compact(args)),
  });

  defineTool(server, ctx, {
    name: "modify_classification_system",
    title: "Modify classification system",
    description: "Changes a classification system's name, editionVersion, description, source or editionDate. Returns {system}.",
    input: {
      system: ClassificationSystemRef,
      name: z.string().min(1).max(255).optional(),
      editionVersion: z.string().min(1).optional(),
      description: z.string().optional(),
      source: z.string().optional(),
      editionDate: DateString.optional(),
      undoName: UndoName,
    },
    annotations: MODIFIES,
    handler: async (args, { ac }) => {
      const { system, undoName, ...changes } = args;
      if (Object.values(changes).every((v) => v === undefined)) throw new Error("Invalid input: nothing to change");
      return ac.addon("ModifyClassificationSystem", compact({ system, undoName, ...changes }));
    },
  });

  defineTool(server, ctx, {
    name: "delete_classification_systems",
    title: "Delete classification systems",
    description:
      "Deletes whole classification systems with all their items in one undo step (element classifications in them are removed). " +
      "Returns {results: [{guid, name, deleted} | {error}]}.",
    input: {
      systems: z.array(ClassificationSystemRef).min(1).max(100),
      undoName: UndoName,
    },
    annotations: DESTRUCTIVE,
    handler: async (args, { ac }) => ac.addon("DeleteClassificationSystems", compact(args)),
  });

  defineTool(server, ctx, {
    name: "import_classifications_xml",
    title: "Import classifications (XML)",
    description:
      "Imports classification systems from an Archicad classification XML (Classification Manager export / downloaded systems " +
      "such as Uniclass, OmniClass). Give the XML text or a local file path. One undo step. Returns {systems: [{guid, name, " +
      "editionVersion, new, itemCount, ...}]}.",
    input: {
      xml: z.string().min(1).optional().describe("XML content"),
      filePath: z.string().min(1).optional().describe("Absolute path of the .xml file on this computer (alternative to xml)"),
      systemConflictPolicy: z.enum(["Merge", "Replace", "Skip"]).optional().describe("When a system with the same name/version exists (default Merge)"),
      itemConflictPolicy: z.enum(["Replace", "Skip"]).optional().describe("When an item ID exists (default Replace)"),
      undoName: UndoName,
    },
    annotations: CREATES,
    handler: async ({ xml, filePath, ...rest }, { ac }) => {
      if ((xml === undefined) === (filePath === undefined)) throw new Error("Invalid input: give exactly one of xml or filePath");
      const content = xml ?? (await readFile(filePath!, "utf8"));
      return ac.addon("ImportClassificationsXml", compact({ ...rest, xml: content }));
    },
  });
}
