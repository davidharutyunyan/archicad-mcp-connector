/**
 * Usage guide served to Claude:
 *  - SERVER_INSTRUCTIONS: sent in the MCP initialize response (short, always in context).
 *  - CONNECTOR_GUIDE: returned by the get_connector_guide tool — the full docs/CLAUDE_GUIDE.md
 *    (falls back to the short instructions when the file is not shipped).
 */

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

export const SERVER_INSTRUCTIONS = `Archicad connector: full read/write control of a running Graphisoft Archicad 26 project.
- Call get_connector_guide once before building or editing anything non-trivial: conventions, workflows, recipes, limits.
- Units: meters and DEGREES. Project coordinates (x right, y up on the plan, z up); elevations are relative to the element's home story.
- Elements are referenced by GUID. Stories by index (0 = ground floor), name, {floorId} or {displayNumber}.
- Attribute and library part names are LOCALIZED (this Archicad may be Russian): look them up with get_attributes / search_library_parts, never guess.
- Walls: referenceLine 'Outside' = outer face, body to the RIGHT of begin→end: draw exterior walls CLOCKWISE. Slab 'level' = top surface.
- Batch: every create/modify tool takes arrays, runs as one undo step '... (Claude)' and returns [{guid} | {error}] per item.
- Verify with get_element_details / get_element_quantities and LOOK with capture_view (3D axonometric, floor plan, sections).
- Inputs are strict: an unknown field is an error naming it. If anything fails, call archicad_status.`;

function loadGuide(): string {
  const candidates = [
    new URL("../docs/CLAUDE_GUIDE.md", import.meta.url), // src/ (tsx) and dist/ (built) both sit next to docs/
  ];
  for (const url of candidates) {
    try {
      return readFileSync(fileURLToPath(url), "utf8");
    } catch {
      /* try next */
    }
  }
  return `# Archicad connector guide\n\n${SERVER_INSTRUCTIONS}\n\n(The full guide docs/CLAUDE_GUIDE.md was not found next to the server.)`;
}

export const CONNECTOR_GUIDE = loadGuide();
