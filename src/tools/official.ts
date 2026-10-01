/**
 * "official" tool family: typed, friendly tools over the built-in Archicad 26 JSON API commands
 * (ac.api("API.X")) — they work without the Claude Connector add-on.
 *
 * Covered official commands (the rest are wrapped by other families or by execute_json_api_command):
 *  - elements:        GetAllElements, GetElementsByType, GetSelectedElements, GetTypesOfElements,
 *                     Get2DBoundingBoxes, Get3DBoundingBoxes, GetElementsRelatedToZones,
 *                     GetComponentsOfElements, GetPropertyValuesOfElementComponents
 *  - properties:      GetAllPropertyNames, GetPropertyIds, GetAllPropertyIds, GetDetailsOfProperties
 *  - classifications: GetAllClassificationSystems, GetAllClassificationsInSystem, GetDetailsOfClassificationItems*,
 *                     GetClassificationsOfElements, SetClassificationsOfElements, GetElementsByClassification,
 *                     GetClassificationItemAvailability, GetPropertyDefinitionAvailability
 *  - navigator:       GetNavigatorItemTree, GetNavigatorItemsType, Get{Story,Section,Elevation,InteriorElevation,Worksheet,
 *                     Detail,Document3D,BuiltInContainer}NavigatorItems, RenameNavigatorItem, MoveNavigatorItem,
 *                     DeleteNavigatorItems, CloneProjectMapItemToViewMap, CreateViewMapFolder, GetPublisherSetNames
 *  - layouts:         CreateLayout, CreateLayoutSubset, GetLayoutSettings, SetLayoutSettings
 *  - attributes:      GetAttributeFolder, GetAttributeFolderContent, CreateAttributeFolders, DeleteAttributeFolders,
 *                     MoveAttributesAndFolders, RenameAttributeFolder, GetAttributesByType, Get<Type>Attributes (names),
 *                     GetProfileAttributePreview, GetActivePenTables
 *  (* item details come from the system tree, which also yields paths)
 */

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";

import type { ToolContext } from "./define.js";
import { registerOfficialAttributeTools } from "./official-attributes.js";
import { registerOfficialClassificationTools } from "./official-classifications.js";
import { registerOfficialElementTools } from "./official-elements.js";
import { registerOfficialNavigatorTools } from "./official-navigator.js";

export function registerOfficialTools(server: McpServer, ctx: ToolContext): void {
  registerOfficialElementTools(server, ctx);
  registerOfficialClassificationTools(server, ctx);
  registerOfficialNavigatorTools(server, ctx);
  registerOfficialAttributeTools(server, ctx);
}
