/**
 * Slab / roof / shell / mesh tools. The add-on adapters live in addon/Src/Commands/SlabsRoofs*.cpp;
 * field names match 1:1 and get_element_details returns the same names.
 *
 *   create_slabs / create_roofs / create_shells / create_meshes  -> CreateElements ({type, ...fields})
 *   modify_slabs / modify_roofs / modify_shells / modify_meshes  -> ModifyElements (after an element type check)
 */

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import { CREATES, defineTool, MODIFIES, type ToolContext } from "./define.js";
import { createElements } from "./elements.js";
import { MeshPatch, MeshSpec, RoofPatch, RoofSpec, ShellPatch, ShellSpec, SlabPatch, SlabSpec } from "./slabs-roofs-schemas.js";

type FamilyType = "Slab" | "Roof" | "Shell" | "Mesh";

interface TypesResult {
  typesOfElements?: Array<{ typeOfElement?: { elementId?: { guid?: string }; elementType?: string }; error?: { code?: number; message?: string } }>;
}

interface ResultsResponse {
  results?: unknown[];
}

const TOOL_OF: Record<FamilyType, string> = { Slab: "modify_slabs", Roof: "modify_roofs", Shell: "modify_shells", Mesh: "modify_meshes" };

/**
 * Sends patches to ModifyElements after checking with API.GetTypesOfElements that every GUID is of the
 * expected type (a slab patch applied to a wall would silently change the wall's thickness otherwise).
 * Mismatching / unknown items get a per-item error; the others are still modified in one undo step.
 */
export async function modifyOfType(
  ctx: ToolContext,
  patches: Array<Record<string, unknown> & { guid: string }>,
  expected: FamilyType,
  undoName?: string,
): Promise<{ results: unknown[] }> {
  const results: unknown[] = new Array(patches.length).fill(undefined);
  let types: TypesResult["typesOfElements"];
  try {
    const res = await ctx.ac.api<TypesResult>("API.GetTypesOfElements", {
      elements: patches.map((p) => ({ elementId: { guid: p.guid } })),
    });
    types = res.typesOfElements;
  } catch {
    types = undefined; // type check unavailable: the add-on still validates every field
  }

  const send: Array<Record<string, unknown>> = [];
  const sentIndex: number[] = [];
  patches.forEach((patch, i) => {
    const entry = types?.[i];
    const type = entry?.typeOfElement?.elementType;
    if (types !== undefined && type !== expected) {
      const message =
        type !== undefined
          ? `Element ${patch.guid} is a ${type}, not a ${expected}. Use ${TOOL_OF[type as FamilyType] ?? "modify_elements"} instead of ${TOOL_OF[expected]}.`
          : `Element ${patch.guid} was not found${entry?.error?.message ? ` (${entry.error.message})` : ""}. Check the GUID with find_elements / get_element_details.`;
      results[i] = { guid: patch.guid, error: { code: entry?.error?.code ?? -2130313112, message } };
      return;
    }
    send.push(patch);
    sentIndex.push(i);
  });

  if (send.length > 0) {
    const params: Record<string, unknown> = { elements: send };
    if (undoName) params["undoName"] = undoName;
    const res = await ctx.ac.addon<ResultsResponse>("ModifyElements", params);
    const list = Array.isArray(res.results) ? res.results : [];
    sentIndex.forEach((original, k) => {
      results[original] = list[k] ?? { guid: patches[original]!.guid, error: { message: "No result returned by the add-on" } };
    });
  }
  return { results };
}

const UndoName = z.string().optional().describe("Name of the undo step shown in Archicad");

const COMMON_NOTES =
  "Units: meters and degrees; coordinates are project coordinates on the home story (default: current story; 'storyIndex' to choose). " +
  "Unspecified settings come from the tool defaults. Attribute names (building materials, composites, surfaces, fills, line types, layers) are " +
  "LOCALIZED — look them up with get_attributes. Returns [{guid, type} | {error}] in input order (one undo step; a failing item does not stop the others). " +
  "Read results back with get_element_details (same field names).";

export function registerSlabRoofTools(server: McpServer, ctx: ToolContext): void {
  // --- create -----------------------------------------------------------------------------------------

  defineTool(server, ctx, {
    name: "create_slabs",
    title: "Create slabs",
    description:
      "Creates slabs (floors, ceilings, flat roofs, terraces) from a plan polygon with optional curved edges (arcs) and holes. " +
      "'level' is the elevation of the slab's reference plane above the home story — pass referencePlane: 'Top' to make level = top of the slab " +
      "(the tool default may differ; get_element_details shows referencePlane and offsetFromTop), " +
      "'thickness' in m, structure by 'buildingMaterial' or 'composite'. Edge trims (Vertical or CustomAngle with edgeAngle) for all edges " +
      "or per edge via 'edges', surface overrides, floor plan pens/fills. Example: {polygon: [{x:0,y:0},{x:8,y:0},{x:8,y:6},{x:0,y:6}], thickness: 0.25, level: 0}. " +
      "A self-intersecting polygon is regularized automatically when it stays one piece. " +
      COMMON_NOTES,
    input: { slabs: z.array(SlabSpec).min(1).max(500).describe("Slabs to create"), undoName: UndoName },
    annotations: CREATES,
    handler: async ({ slabs, undoName }, c) =>
      createElements(c, slabs.map((s) => ({ type: "Slab", ...s })), undoName ?? "Create slabs (Claude)"),
  });

  defineTool(server, ctx, {
    name: "create_roofs",
    title: "Create roofs",
    description:
      "Creates roofs. SinglePlane: one sloped plane — 'polygon' (roof outline in plan incl. overhang), 'pivotLine' {begin, end} (the horizontal line at " +
      "elevation 'level' the plane pivots around, usually the eaves), 'slopeAngle' (degrees); the plane rises towards the polygon unless risesToLeft is given. " +
      "Two single-plane roofs with opposite pivot lines make a gable roof. MultiPlane: a hip roof over any closed 'pivotPolygon' (usually the outer wall outline at the " +
      "wall top: level = wall height) with 'slopeAngle' or 'levels' (pitch breaks, e.g. mansard), 'eavesOverhang', and per-plane 'pivotEdges' overrides " +
      "(gable: true turns that side into a vertical gable end, angle changes its pitch). 'thickness', 'buildingMaterial'/'composite', surfaces and floor plan " +
      "attributes as for slabs. Example hip roof: {pivotPolygon: [{x:0,y:0},{x:10,y:0},{x:10,y:8},{x:0,y:8}], level: 3, slopeAngle: 30, eavesOverhang: 0.5, thickness: 0.3}. " +
      COMMON_NOTES,
    input: { roofs: z.array(RoofSpec).min(1).max(200).describe("Roofs to create"), undoName: UndoName },
    annotations: CREATES,
    handler: async ({ roofs, undoName }, c) =>
      createElements(c, roofs.map((r) => ({ type: "Roof", ...r })), undoName ?? "Create roofs (Claude)"),
  });

  defineTool(server, ctx, {
    name: "create_shells",
    title: "Create shells",
    description:
      "Creates shells (free-form roofs/vaults/domes/canopies). Extruded: an open 'profile' polyline drawn in the profile plane (x across, y up) swept from " +
      "'begin' along the 3D 'extrusion' vector — e.g. a barrel vault: {profile: {points: [{x:-3,y:0},{x:3,y:0}], arcs: [{index:0, angle:-180}]}, begin: {x:0,y:0,z:3}, " +
      "extrusion: {x:0,y:12,z:0}}. Revolved: 'profile' {x = distance from the axis, y = height} revolved by 'revolutionAngle' (default 360) around the vertical axis " +
      "through 'axisOrigin' — e.g. a dome. Ruled: surface between 'profile' on 'plane1' and 'profile2' on 'plane2'. 'closedProfile': true for closed sections (tubes). " +
      "'thickness', 'flipped' (side of the thickness), structure, surfaces, edge trim and floor plan attributes as for roofs. Check the result in 3D and read the " +
      "stored geometry with get_element_details (basePlane, profile, extrusion ...) — copy those values from an existing shell for exact placement. " +
      COMMON_NOTES,
    input: { shells: z.array(ShellSpec).min(1).max(200).describe("Shells to create"), undoName: UndoName },
    annotations: CREATES,
    handler: async ({ shells, undoName }, c) =>
      createElements(c, shells.map((s) => ({ type: "Shell", ...s })), undoName ?? "Create shells (Claude)"),
  });

  defineTool(server, ctx, {
    name: "create_meshes",
    title: "Create meshes",
    description:
      "Creates meshes (terrain / site surfaces). 'polygon' points carry heights: {x, y, z} with z relative to the mesh base 'level'; add inner 'levelLines' " +
      "(ridges / contour lines, each point with z) to shape the surface inside the outline; holes allowed. 'skirt': SolidBody | SkirtWithoutBottom | SurfaceOnly, " +
      "'skirtLevel' = depth of the body below the base plane. 'ridges' controls smooth/sharp display. Example: {polygon: [{x:0,y:0,z:0},{x:30,y:0,z:1},{x:30,y:20,z:2.5},{x:0,y:20,z:0.5}], " +
      "level: -0.1, skirt: 'SolidBody', skirtLevel: 1}. " +
      COMMON_NOTES,
    input: { meshes: z.array(MeshSpec).min(1).max(200).describe("Meshes to create"), undoName: UndoName },
    annotations: CREATES,
    handler: async ({ meshes, undoName }, c) =>
      createElements(c, meshes.map((m) => ({ type: "Mesh", ...m })), undoName ?? "Create meshes (Claude)"),
  });

  // --- modify -----------------------------------------------------------------------------------------

  const MODIFY_NOTES =
    "Only the given fields change; the others keep their values. Each item must be an element of this type (checked first; other items still run). " +
    "Returns [{guid} | {error}] in input order, one undo step.";

  defineTool(server, ctx, {
    name: "modify_slabs",
    title: "Modify slabs",
    description:
      "Changes existing slabs: polygon (outline incl. holes — replaces the shape and resets per-edge data), thickness, level, referencePlane, structure, " +
      "surfaces, edge trims (all edges or per edge via 'edges'), floor plan attributes, layer, story, renovation status, element ID. Fields as in create_slabs. " +
      MODIFY_NOTES,
    input: { slabs: z.array(SlabPatch).min(1).max(1000).describe("Patches {guid, ...fields to change}"), undoName: UndoName },
    annotations: MODIFIES,
    handler: async ({ slabs, undoName }, c) => modifyOfType(c, slabs, "Slab", undoName ?? "Modify slabs (Claude)"),
  });

  defineTool(server, ctx, {
    name: "modify_roofs",
    title: "Modify roofs",
    description:
      "Changes existing roofs (the class SinglePlane/MultiPlane cannot change). SinglePlane: polygon, pivotLine, slopeAngle, risesToLeft, edges. MultiPlane: " +
      "pivotPolygon, slopeAngle, levels, eavesOverhang, pivotEdges (per-plane pitch / gable end / overhang; edge indices of the stored pivot polygon as returned " +
      "by get_element_details). Both: level, thickness, structure, surfaces, edge trim, floor plan attributes. Fields as in create_roofs. " +
      MODIFY_NOTES,
    input: { roofs: z.array(RoofPatch).min(1).max(1000).describe("Patches {guid, ...fields to change}"), undoName: UndoName },
    annotations: MODIFIES,
    handler: async ({ roofs, undoName }, c) => modifyOfType(c, roofs, "Roof", undoName ?? "Modify roofs (Claude)"),
  });

  defineTool(server, ctx, {
    name: "modify_shells",
    title: "Modify shells",
    description:
      "Changes existing shells (the class Extruded/Revolved/Ruled cannot change): profile / profile2 / closedProfile, placement (begin, extrusion, axisOrigin, " +
      "profileRotation, basePlane, plane1/plane2), angles, thickness, flipped, structure, surfaces, floor plan attributes. Fields as in create_shells. " +
      MODIFY_NOTES,
    input: { shells: z.array(ShellPatch).min(1).max(1000).describe("Patches {guid, ...fields to change}"), undoName: UndoName },
    annotations: MODIFIES,
    handler: async ({ shells, undoName }, c) => modifyOfType(c, shells, "Shell", undoName ?? "Modify shells (Claude)"),
  });

  defineTool(server, ctx, {
    name: "modify_meshes",
    title: "Modify meshes",
    description:
      "Changes existing meshes: polygon with point heights (replaces the outline incl. holes), levelLines ([] removes them), level, skirt, skirtLevel, " +
      "ridges, building material, surfaces, floor plan attributes. Fields as in create_meshes. " +
      MODIFY_NOTES,
    input: { meshes: z.array(MeshPatch).min(1).max(1000).describe("Patches {guid, ...fields to change}"), undoName: UndoName },
    annotations: MODIFIES,
    handler: async ({ meshes, undoName }, c) => modifyOfType(c, meshes, "Mesh", undoName ?? "Modify meshes (Claude)"),
  });
}
