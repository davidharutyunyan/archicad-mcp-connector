/**
 * Generic element tools backed by the add-on's adapter registry
 * (CreateElements / GetElementDetails / ModifyElements / GetSupportedElementTypes).
 */

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import { CREATES, defineTool, MODIFIES, READ_ONLY, type ToolContext } from "./define.js";
import { ElementRefs, Guid, guidOf } from "./schemas.js";

/** Sends element specs (each with a `type`) to CreateElements and returns the per-item results. */
export async function createElements(ctx: ToolContext, elements: Record<string, unknown>[], undoName?: string): Promise<unknown> {
  const params: Record<string, unknown> = { elements };
  if (undoName) params["undoName"] = undoName;
  return ctx.ac.addon("CreateElements", params);
}

export function registerElementTools(server: McpServer, ctx: ToolContext): void {
  defineTool(server, ctx, {
    name: "create_elements",
    title: "Create elements (any type)",
    description:
      "Creates elements of ANY supported type in one undo step. Each item is {type: 'Wall'|'Column'|'Beam'|'Slab'|'Roof'|'Shell'|'Mesh'|'Zone'|" +
      "'Window'|'Door'|'Skylight'|'Opening'|'Object'|'Lamp'|'Line'|'Arc'|'Circle'|'PolyLine'|'Spline'|'Hatch'|'Text'|'Label'|'Dimension'|" +
      "'LevelDimension'|'Hotspot'|'Morph'|'CurtainWall'|..., ...fields}. The fields are the same as in the type-specific create_* tools " +
      "(prefer those: they document every field). Returns [{guid, type} | {error}] in input order; one failing item does not stop the others.",
    input: {
      elements: z.array(z.object({ type: z.string() }).passthrough()).min(1).describe("Element specs, each with a 'type'"),
      undoName: z.string().optional().describe("Name of the undo step shown in Archicad"),
    },
    annotations: CREATES,
    handler: async ({ elements, undoName }, c) => createElements(c, elements as Record<string, unknown>[], undoName),
  });

  defineTool(server, ctx, {
    name: "get_element_details",
    title: "Get element details",
    description:
      "Returns everything known about elements: common fields (guid, type, storyIndex, layer, elementId, group/hotlink, renovation status, " +
      "lock) plus type-specific details (geometry, heights, structure/materials, library part & key params, relations such as the openings of a wall). " +
      "Works for all element types.",
    input: { elements: ElementRefs.max(1000) },
    annotations: READ_ONLY,
    handler: async ({ elements }, { ac }) => ac.addon("GetElementDetails", { elements: elements.map(guidOf) }),
  });

  defineTool(server, ctx, {
    name: "modify_elements",
    title: "Modify elements",
    description:
      "Changes fields of existing elements in one undo step. Each item is {guid, ...fields to change}; the field names are the same as the " +
      "type's create_* tool (e.g. a wall: height, thickness, begin, end, composite; any element: layer, storyIndex, renovationStatus, elementId). " +
      "Only the given fields change. Returns [{guid} | {error}].",
    input: {
      elements: z.array(z.object({ guid: Guid }).passthrough()).min(1).describe("Patches: {guid, field: newValue, ...}"),
      undoName: z.string().optional(),
    },
    annotations: MODIFIES,
    handler: async ({ elements, undoName }, { ac }) =>
      ac.addon("ModifyElements", { elements, ...(undoName ? { undoName } : {}) }),
  });

  defineTool(server, ctx, {
    name: "get_supported_element_types",
    title: "Supported element types",
    description: "Lists every Archicad element type and whether this connector can create it, read its details, and modify it.",
    input: {},
    annotations: READ_ONLY,
    handler: async (_a, { ac }) => ac.addon("GetSupportedElementTypes"),
  });
}
