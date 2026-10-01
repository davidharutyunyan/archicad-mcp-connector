/**
 * Registry of all tool families. Each family has a matching add-on command family
 * in addon/Src/Commands/ (except "official", which wraps the built-in JSON API).
 */

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";

import type { ToolContext } from "./define.js";
import { registerSystemTools } from "./system.js";
import { registerElementTools } from "./elements.js";
import { registerWallTools } from "./walls.js";
import { registerProjectTools } from "./project.js";
import { registerStoryTools } from "./stories.js";
import { registerAttributeTools } from "./attributes.js";
import { registerElementQueryTools } from "./element-query.js";
import { registerElementEditTools } from "./element-edit.js";
import { registerColumnBeamTools } from "./columns-beams.js";
import { registerSlabRoofTools } from "./slabs-roofs.js";
import { registerOpeningTools } from "./openings.js";
import { registerObjectLibraryTools } from "./objects-library.js";
import { registerZoneTools } from "./zones.js";
import { registerDraftingTools } from "./drafting.js";
import { registerDimensionTools } from "./dimensions.js";
import { registerComplexElementTools } from "./complex-elements.js";
import { registerViewTools } from "./views.js";
import { registerDocumentationTools } from "./documentation.js";
import { registerPropertyTools } from "./properties.js";
import { registerCollaborationTools } from "./collaboration.js";
import { registerOfficialTools } from "./official.js";

export const TOOL_FAMILIES: ReadonlyArray<[string, (server: McpServer, ctx: ToolContext) => void]> = [
  ["system", registerSystemTools],               // status, instance selection, guide, raw pass-through
  ["elements", registerElementTools],            // generic create/get/modify for any type
  ["walls", registerWallTools],                  // walls
  ["project", registerProjectTools],             // project info, save/open/close, autotexts, geo location, units, undo
  ["stories", registerStoryTools],               // stories
  ["attributes", registerAttributeTools],        // layers, composites, building materials, surfaces, fills, lines, pens, zone categories, profiles, layer combinations
  ["element-query", registerElementQueryTools],  // find/filter elements, quantities, relations, selection
  ["element-edit", registerElementEditTools],    // move/copy/rotate/mirror/delete/group/lock/trim/solid ops/draw order
  ["columns-beams", registerColumnBeamTools],    // columns, beams
  ["slabs-roofs", registerSlabRoofTools],        // slabs, roofs, shells, meshes
  ["openings", registerOpeningTools],            // doors, windows, skylights, openings
  ["objects-library", registerObjectLibraryTools], // objects, lamps, libraries, library parts, GDL parameters, custom GDL objects
  ["zones", registerZoneTools],                  // zones
  ["drafting", registerDraftingTools],           // lines, arcs, circles, polylines, splines, hatches, texts, labels, hotspots, pictures
  ["dimensions", registerDimensionTools],        // linear / level / radial / angle dimensions
  ["complex-elements", registerComplexElementTools], // morphs, curtain walls, stairs, railings
  ["views", registerViewTools],                  // windows, zoom, 3D view, capture images, render, sections/elevations
  ["documentation", registerDocumentationTools], // layouts, drawings, publishing, export/import (IFC, DWG, PDF), hotlinks
  ["properties", registerPropertyTools],         // property definitions & values, classifications, IFC data
  ["collaboration", registerCollaborationTools], // Teamwork, issues/BCF, favorites, tool defaults, revisions
  ["official", registerOfficialTools],           // remaining official JSON API commands (navigator, classification, layouts, components...)
];

/** Families always loaded, regardless of ARCHICAD_TOOLSETS. */
const ALWAYS = new Set(["system", "elements"]);

/** Named presets usable in ARCHICAD_TOOLSETS (expanded to family names). */
export const TOOLSET_PRESETS: Record<string, string[]> = {
  minimal: ["views", "element-query"],
  modeling: [
    "walls", "stories", "attributes", "element-query", "element-edit", "columns-beams", "slabs-roofs",
    "openings", "objects-library", "zones", "complex-elements", "views", "project",
  ],
  documentation: ["views", "documentation", "dimensions", "drafting", "zones", "official", "project", "element-query"],
  data: ["properties", "official", "element-query", "attributes", "project"],
};

/**
 * Registers the tool families selected by `toolsets` (default: all).
 * `toolsets` is a comma-separated list of family names (see TOOL_FAMILIES), e.g.
 * "walls,slabs-roofs,openings,views"; "all" or empty = everything. Presets: minimal, modeling,
 * documentation, data. Prefix a name with "-" to exclude it ("all,-collaboration,-official").
 */
export function registerAllTools(server: McpServer, ctx: ToolContext, toolsets = process.env["ARCHICAD_TOOLSETS"] ?? "all"): string[] {
  const tokens = toolsets
    .split(",")
    .map((t) => t.trim().toLowerCase())
    .filter(Boolean)
    .flatMap((t) => {
      const neg = t.startsWith("-");
      const preset = TOOLSET_PRESETS[t.replace(/^-/, "")];
      return preset ? preset.map((f) => (neg ? `-${f}` : f)) : [t];
    });
  const known = new Set(TOOL_FAMILIES.map(([name]) => name));
  for (const t of tokens) {
    const name = t.replace(/^-/, "");
    if (name !== "all" && !known.has(name)) {
      throw new Error(
        `Unknown toolset '${name}' in ARCHICAD_TOOLSETS. Known: all, ${Object.keys(TOOLSET_PRESETS).join(", ")}, ${[...known].join(", ")}`,
      );
    }
  }
  const includeAll = tokens.length === 0 || tokens.includes("all");
  const included = new Set(tokens.filter((t) => !t.startsWith("-") && t !== "all"));
  const excluded = new Set(tokens.filter((t) => t.startsWith("-")).map((t) => t.slice(1)));
  const loaded: string[] = [];
  for (const [name, register] of TOOL_FAMILIES) {
    const wanted = ALWAYS.has(name) || ((includeAll || included.has(name)) && !excluded.has(name));
    if (!wanted) continue;
    register(server, ctx);
    loaded.push(name);
  }
  return loaded;
}
