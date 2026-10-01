/**
 * Wall tools — reference implementation of a typed "create_<type>s" tool.
 * The add-on adapter lives in addon/Src/Commands/Walls.cpp; field names match 1:1.
 */

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import { CREATES, defineTool, type ToolContext } from "./define.js";
import { createElements } from "./elements.js";
import { AttrRef, CommonElementFields, Point2D } from "./schemas.js";

export const WallFields = {
  begin: Point2D.describe("Start point of the wall reference line"),
  end: Point2D.describe("End point of the wall reference line"),
  height: z.number().positive().optional().describe("Height in m. Giving a height unlinks the wall top unless topLinkedStory is also given"),
  thickness: z.number().positive().optional().describe("Thickness in m (ignored for composite walls: the composite defines it)"),
  endThickness: z.number().positive().optional().describe("Thickness at the end point; makes a trapezoid wall"),
  bottomOffset: z.number().optional().describe("Bottom elevation relative to the home story level (m); the wall then spans bottomOffset .. bottomOffset + height"),
  topLinkedStory: z.number().int().min(0).optional().describe("Link the top to the story N levels above the home story (0 = unlinked, use height)"),
  topOffset: z.number().optional().describe("Offset from the linked top story (m, e.g. -0.3 under the slab)"),
  arcAngle: z.number().optional().describe("Curved wall: central angle in degrees (0 = straight; sign sets the bulge side)"),
  referenceLine: z.enum(["Outside", "Center", "Inside", "CoreOutside", "CoreCenter", "CoreInside"]).optional()
    .describe(
      "Where the reference line (begin→end) sits in the wall section (verified live, with flipped false = the create default): " +
        "'Outside' = the reference line is the wall's outer face and the body lies to the RIGHT of begin→end — so draw exterior walls " +
        "CLOCKWISE and the walls stay inside the outline; 'Inside' = body to the LEFT; 'Center' = centered. Core* variants refer to the " +
        "core skin of composite walls",
    ),
  flipped: z.boolean().optional().describe("Mirror the wall to the other side of its reference line (default false on create, regardless of the Wall tool default): with 'Outside' the body then lies to the LEFT"),
  offset: z.number().optional().describe("Offset of the wall body from the reference line (m)"),
  buildingMaterial: AttrRef.optional().describe("Basic structure: building material name/index"),
  composite: AttrRef.optional().describe("Composite structure: composite name/index (sets layered skins and total thickness)"),
  profile: AttrRef.optional().describe("Complex profile name/index"),
  refSurface: z.union([AttrRef, z.literal(false)]).optional().describe("Override surface on the reference side (false = remove override)"),
  oppSurface: z.union([AttrRef, z.literal(false)]).optional().describe("Override surface on the opposite side"),
  sideSurface: z.union([AttrRef, z.literal(false)]).optional().describe("Override surface on the wall ends"),
  zoneRelation: z.enum(["Boundary", "ReduceArea", "None"]).optional().describe("How the wall affects zone boundaries"),
  slantAlpha: z.number().optional().describe("Slanted wall: angle of the first face in degrees (90 = vertical)"),
  slantBeta: z.number().optional().describe("Trapezoid/double-slanted wall: angle of the second face in degrees"),
  ...CommonElementFields,
};

export const WallSpec = z.object(WallFields);

export function registerWallTools(server: McpServer, ctx: ToolContext): void {
  defineTool(server, ctx, {
    name: "create_walls",
    title: "Create walls",
    description:
      "Creates straight, curved, trapezoid or slanted walls (one undo step). Coordinates in meters, angles in degrees, on the given story " +
      "(default: current story). Unspecified settings come from the Wall tool defaults. Chain walls end-to-start so Archicad joins the corners. " +
      "Returns [{guid, type} | {error}] in input order.",
    input: { walls: z.array(WallSpec).min(1).max(500) },
    annotations: CREATES,
    handler: async ({ walls }, c) => createElements(c, walls.map((w) => ({ type: "Wall", ...w })), "Create walls (Claude)"),
  });
}
