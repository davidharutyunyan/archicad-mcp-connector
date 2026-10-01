/**
 * Element editing tools for ANY element type: move / copy / rotate / mirror / elevate / resize,
 * delete, copy to stories, groups, locking, drawing order, trimming to roofs/shells, merging and
 * solid element operations. Backed by addon/Src/Commands/ElementEdit.cpp (field names match 1:1).
 *
 * Every call is ONE undo step in Archicad and returns per-element results ({guid, ...} | {guid, error}),
 * so one bad element never aborts the others.
 */

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import { CREATES, defineTool, DESTRUCTIVE, MODIFIES, READ_ONLY, type ToolContext } from "./define.js";
import { ElementRef, ElementRefs, Guid, guidOf, Point2D, StoryRef } from "./schemas.js";

// ---------------------------------------------------------------------------------------------
// Shared schemas
// ---------------------------------------------------------------------------------------------

const Elements = ElementRefs.max(5000).describe(
  "Elements to edit: GUID strings (or {guid}) of any element type, e.g. from get_element_details / element search tools. " +
    "A group GUID stands for all elements of that group.",
);

const IncludeGroupMembers = z
  .boolean()
  .optional()
  .describe(
    "Default false: ONLY the listed elements are affected even when they belong to groups (Suspend Groups is switched on for the " +
      "duration of the call and restored afterwards). true: all members of their (top-level) groups are affected as well.",
  );

const Vector = z
  .object({
    x: z.number().describe("Displacement along X in meters (east is +)"),
    y: z.number().describe("Displacement along Y in meters (north is +)"),
    z: z.number().optional().describe("Vertical displacement in meters (default 0; ignored by 2D elements)"),
  })
  .describe("Displacement vector in meters");

const CopyCount = z
  .number()
  .int()
  .min(1)
  .max(100)
  .optional()
  .describe("Number of copies (default 1). With count N, copy k is placed at k × the transformation (linear/polar array, like Archicad's Multiply)");

const AsCopy = z.boolean().optional().describe("true = keep the originals and transform copies (default false = transform the originals)");

const TRIM_TYPES = ["KeepInside", "KeepOutside", "KeepAll"] as const;
const SOLID_OPERATIONS = ["Subtract", "SubtractUpwards", "SubtractDownwards", "Intersect", "Add"] as const;
const DRAW_ORDER_ACTIONS = ["BringToFront", "BringForward", "SendBackward", "SendToBack", "Reset"] as const;

const ids = (refs: z.infer<typeof ElementRef>[]): string[] => refs.map(guidOf);

const EDIT_RESULT_NOTE =
  "Returns {results: [{guid, newGuid?, warning?} | {guid, error}], warnings?} in input order (a 'warning' on an item means Archicad " +
  "may have ignored it); with copy=true the copy-style result {results: [{guid, copies: [new GUIDs]}], createdCount, additionalCreated?}. " +
  "Locked elements, elements on locked/hidden layers, hotlinked elements and elements outside your Teamwork workspace are reported " +
  "with an actionable error instead of being edited.";

const COPY_RESULT_NOTE =
  "Returns {results: [{guid (original), copies: [new GUIDs]} | {guid, error}], createdCount, createdGroups? (GUIDs of the new groups " +
  "holding the copies when whole groups were copied), additionalCreated?: [{guid, type, storyIndex}] " +
  "(dependent elements created with the copies: openings of copied walls, copied group members, associative dimensions/labels), " +
  "subElementsCreated? (curtain wall / stair / railing parts), warnings?}.";

// ---------------------------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------------------------

export function registerElementEditTools(server: McpServer, ctx: ToolContext): void {
  // --- Transformations ---------------------------------------------------------------------

  defineTool(server, ctx, {
    name: "move_elements",
    title: "Move elements",
    description:
      "Moves elements of ANY type (walls, slabs, objects, zones, lines, dimensions, ...) by a displacement vector, like Edit > Move > Drag. " +
      "Pass `elements` + `vector` to move them all by the same offset, or `moves` to move different sets by different vectors " +
      "(all in one undo step). A z component moves model elements vertically (use elevate_elements for pure vertical moves). " +
      "Windows/doors move along their host wall; connected dimensions and labels follow automatically. " +
      EDIT_RESULT_NOTE +
      " With `moves` the result is {operations: [<result per move>]}.",
    input: {
      elements: Elements.optional().describe("Elements to move (together with `vector`)"),
      vector: Vector.optional().describe("Displacement applied to `elements`, in meters"),
      moves: z
        .array(z.object({ elements: Elements, vector: Vector }))
        .min(1)
        .max(200)
        .optional()
        .describe("Alternative to elements+vector: several independent moves [{elements, vector}] in one undo step"),
      includeGroupMembers: IncludeGroupMembers,
    },
    annotations: DESTRUCTIVE,
    handler: async ({ elements, vector, moves, includeGroupMembers }, { ac }) => {
      if (moves) {
        if (elements || vector) throw new Error("Give either `moves` or `elements` + `vector`, not both.");
        return ac.addon("MoveElements", {
          operations: moves.map((m) => ({ elements: ids(m.elements), vector: m.vector, includeGroupMembers })),
        });
      }
      if (!elements || !vector) throw new Error("Give `elements` and `vector` (meters), or `moves: [{elements, vector}]`.");
      return ac.addon("MoveElements", { elements: ids(elements), vector, includeGroupMembers });
    },
  });

  defineTool(server, ctx, {
    name: "copy_elements",
    title: "Copy elements",
    description:
      "Duplicates elements of ANY type displaced by `vector` (Edit > Move > Drag a Copy); the originals stay. `count` > 1 makes a linear array " +
      "(copy k at k × vector, e.g. 10 columns every 6 m: vector {x: 6, y: 0}, count 10). Copies keep all settings, story, layer and " +
      "properties of their original; copying a wall also copies its windows/doors. To duplicate onto other stories use copy_elements_to_stories. " +
      COPY_RESULT_NOTE,
    input: {
      elements: Elements,
      vector: Vector.describe("Offset of the (first) copy from the original, in meters"),
      count: CopyCount,
      includeGroupMembers: IncludeGroupMembers,
    },
    annotations: CREATES,
    handler: async ({ elements, vector, count, includeGroupMembers }, { ac }) =>
      ac.addon("CopyElements", { elements: ids(elements), vector, count, includeGroupMembers }),
  });

  defineTool(server, ctx, {
    name: "rotate_elements",
    title: "Rotate elements",
    description:
      "Rotates elements of ANY type in plan around a centre point (Edit > Move > Rotate). Angle in DEGREES, positive = counter-clockwise. " +
      "Without `center` the centre of the elements' combined bounding box is used (reported in warnings). With copy=true the originals stay " +
      "and `count` copies are made at k × angle (polar array, e.g. 6 chairs around a table: angle 60, count 5, copy true). " +
      "Returns move-style results, or copy-style results ({results: [{guid, copies}], createdCount, additionalCreated?}) when copy=true.",
    input: {
      elements: Elements,
      angle: z.number().describe("Rotation angle in degrees; positive = counter-clockwise, negative = clockwise"),
      center: Point2D.optional().describe("Pivot point in meters (default: centre of the elements' bounding box)"),
      copy: AsCopy,
      count: CopyCount.describe("Only with copy=true: number of rotated copies, copy k rotated by k × angle (default 1)"),
      includeGroupMembers: IncludeGroupMembers,
    },
    annotations: DESTRUCTIVE,
    handler: async ({ elements, angle, center, copy, count, includeGroupMembers }, { ac }) =>
      ac.addon("RotateElements", { elements: ids(elements), angle, center, copy, count, includeGroupMembers }),
  });

  defineTool(server, ctx, {
    name: "mirror_elements",
    title: "Mirror elements",
    description:
      "Mirrors elements of ANY type across a line in plan (Edit > Move > Mirror). Give the mirror line either by two points " +
      "(`axisStart`, `axisEnd`) or as `axis`: 'Vertical' | 'Horizontal' through `through` (default: centre of the elements' bounding box). " +
      "copy=true keeps the originals and creates mirrored copies (e.g. the other half of a symmetric plan). Mirroring flips wall/door " +
      "opening sides and object handedness like Archicad does. " +
      EDIT_RESULT_NOTE,
    input: {
      elements: Elements,
      axisStart: Point2D.optional().describe("First point of the mirror line (with axisEnd)"),
      axisEnd: Point2D.optional().describe("Second point of the mirror line (with axisStart)"),
      axis: z.enum(["Vertical", "Horizontal"]).optional().describe("Alternative to axisStart/axisEnd: a vertical (parallel to Y) or horizontal (parallel to X) mirror line"),
      through: Point2D.optional().describe("Point the vertical/horizontal `axis` passes through (default: bounding-box centre)"),
      copy: AsCopy,
      includeGroupMembers: IncludeGroupMembers,
    },
    annotations: DESTRUCTIVE,
    handler: async ({ elements, axisStart, axisEnd, axis, through, copy, includeGroupMembers }, { ac }) => {
      const byPoints = axisStart !== undefined || axisEnd !== undefined;
      if (byPoints && (axisStart === undefined || axisEnd === undefined)) throw new Error("Give both `axisStart` and `axisEnd`.");
      if (byPoints && axis) throw new Error("Give either `axisStart` + `axisEnd` or `axis` (+ `through`), not both.");
      if (!byPoints && !axis) throw new Error("Give the mirror line: `axisStart` + `axisEnd`, or `axis`: 'Vertical' | 'Horizontal' (+ optional `through`).");
      return ac.addon("MirrorElements", { elements: ids(elements), axisStart, axisEnd, axis, through, copy, includeGroupMembers });
    },
  });

  defineTool(server, ctx, {
    name: "elevate_elements",
    title: "Elevate elements",
    description:
      "Moves model elements vertically by `deltaZ` meters (Edit > Move > Elevate); positive = up. Heights/offsets relative to the home story " +
      "change accordingly (e.g. a wall's bottom offset, an object's elevation); the home story does not change (use modify_elements storyIndex " +
      "or copy_elements_to_stories for that). 2D elements (lines, fills, texts, labels, dimensions, drawings...) have no elevation and are " +
      "reported as per-item errors. copy=true with `count` stacks copies at k × deltaZ. " +
      EDIT_RESULT_NOTE,
    input: {
      elements: Elements,
      deltaZ: z.number().describe("Vertical displacement in meters (positive = up)"),
      copy: AsCopy,
      count: CopyCount.describe("Only with copy=true: number of stacked copies (default 1)"),
      includeGroupMembers: IncludeGroupMembers,
    },
    annotations: DESTRUCTIVE,
    handler: async ({ elements, deltaZ, copy, count, includeGroupMembers }, { ac }) =>
      ac.addon("ElevateElements", { elements: ids(elements), deltaZ, copy, count, includeGroupMembers }),
  });

  defineTool(server, ctx, {
    name: "resize_elements",
    title: "Resize (scale) elements",
    description:
      "Scales elements in plan by `ratio` around a centre point (Edit > Reshape > Resize): 2 = double size, 0.5 = half. " +
      "Default centre: the elements' bounding-box centre. Model element heights are not scaled; text/labels scale according to Archicad rules. " +
      EDIT_RESULT_NOTE,
    input: {
      elements: Elements,
      ratio: z.number().positive().describe("Scale factor (> 0, not 1)"),
      center: Point2D.optional().describe("Fixed point of the scaling in meters (default: bounding-box centre)"),
      copy: AsCopy,
      includeGroupMembers: IncludeGroupMembers,
    },
    annotations: DESTRUCTIVE,
    handler: async ({ elements, ratio, center, copy, includeGroupMembers }, { ac }) =>
      ac.addon("ResizeElements", { elements: ids(elements), ratio, center, copy, includeGroupMembers }),
  });

  // --- Delete / copy to stories --------------------------------------------------------------

  defineTool(server, ctx, {
    name: "delete_elements",
    title: "Delete elements",
    description:
      "Deletes elements of ANY type (one undo step: undo restores them). Dependent elements are removed by Archicad too and are reported: " +
      "windows/doors of deleted walls, associative dimensions and labels, etc. -> `alsoDeleted: [{guid, type}]`, `subElementsDeleted` " +
      "(curtain wall / stair / railing parts). Only delete elements you created or were asked to delete. Group GUIDs delete the whole group. " +
      "Returns {results: [{guid, deleted: true} | {guid, error}], deletedCount, alsoDeleted?, subElementsDeleted?, warnings?}.",
    input: {
      elements: Elements,
      includeGroupMembers: IncludeGroupMembers,
    },
    annotations: DESTRUCTIVE,
    handler: async ({ elements, includeGroupMembers }, { ac }) =>
      ac.addon("DeleteElements", { elements: ids(elements), includeGroupMembers }),
  });

  defineTool(server, ctx, {
    name: "copy_elements_to_stories",
    title: "Copy elements to other stories",
    description:
      "Duplicates elements onto other stories at the same plan position, keeping their elevation RELATIVE to the home story " +
      "(like copy + Paste to stories: a wall with bottom offset 0 on story 0 becomes a wall with bottom offset 0 on story 2). " +
      "Top-linked walls/columns stay linked relative to their new home story. Copy walls/roofs/slabs rather than their windows/doors: " +
      "hosted openings travel with their host. List stories with get_stories. " +
      "Returns {results: [{guid (original), copies: [{storyIndex, guid} | {storyIndex, error}]}], additionalCreated?, subElementsCreated?, warnings?}.",
    input: {
      elements: Elements,
      stories: z.array(StoryRef).min(1).max(200).describe("Target stories: story indices (0 = ground floor) or story names"),
      includeGroupMembers: IncludeGroupMembers,
    },
    annotations: CREATES,
    handler: async ({ elements, stories, includeGroupMembers }, { ac }) =>
      ac.addon("CopyElementsToStories", { elements: ids(elements), stories, includeGroupMembers }),
  });

  // --- Groups ---------------------------------------------------------------------------------

  const GroupSpec = z.object({
    elements: ElementRefs.min(2).max(5000).describe("At least two elements and/or group GUIDs (all on the same story)"),
    parentGroup: Guid.optional().describe("Optional existing group GUID to nest the new group into"),
  });

  defineTool(server, ctx, {
    name: "group_elements",
    title: "Group elements",
    description:
      "Groups elements (Edit > Grouping > Group) so they move/select together. Elements that already belong to a group bring their whole " +
      "top-level group, which becomes a nested sub-group (same as Archicad). All elements must be on the same story. " +
      "Pass `elements` for one group, or `groups: [{elements, parentGroup?}]` for several in one undo step. " +
      "Returns {groupGuid, members, elementCount} (with `groups`: {results: [...]}). The group GUID can be passed to move_elements, " +
      "delete_elements, ungroup_elements, ... to address the whole group.",
    input: {
      elements: GroupSpec.shape.elements.optional(),
      parentGroup: GroupSpec.shape.parentGroup,
      groups: z.array(GroupSpec).min(1).max(200).optional().describe("Several groups to create in one undo step"),
    },
    annotations: CREATES,
    handler: async ({ elements, parentGroup, groups }, { ac }) => {
      if (groups) {
        if (elements) throw new Error("Give either `elements` or `groups`, not both.");
        return ac.addon("GroupElements", { groups: groups.map((g) => ({ elements: ids(g.elements), parentGroup: g.parentGroup })) });
      }
      if (!elements) throw new Error("Give `elements` (at least two) or `groups: [{elements}]`.");
      return ac.addon("GroupElements", { elements: ids(elements), parentGroup });
    },
  });

  defineTool(server, ctx, {
    name: "ungroup_elements",
    title: "Ungroup elements",
    description:
      "Dissolves groups (Edit > Grouping > Ungroup). Pass group GUIDs, or any member element (its top-level group is dissolved). " +
      "One level per call like Archicad: nested sub-groups survive unless completely=true. " +
      "Returns {results: [{guid, dissolvedGroups: [guid], elementCount, remainingGroup?} | {guid, error}]}.",
    input: {
      elements: ElementRefs.max(1000).describe("Group GUIDs or member element GUIDs"),
      completely: z.boolean().optional().describe("true = also dissolve all nested sub-groups (default false: one level)"),
    },
    annotations: MODIFIES,
    handler: async ({ elements, completely }, { ac }) => ac.addon("UngroupElements", { elements: ids(elements), completely }),
  });

  defineTool(server, ctx, {
    name: "set_suspend_groups",
    title: "Suspend groups",
    description:
      "Reads or switches Archicad's 'Suspend Groups' mode (Edit > Grouping). When ON, members of groups can be selected and edited individually. " +
      "The edit tools of this connector already handle groups via includeGroupMembers, so this is only needed for manual workflows. " +
      "Omit `suspend` to just read the state. Returns {suspendGroups, changed}.",
    input: { suspend: z.boolean().optional().describe("true = suspend groups (ON), false = groups active (OFF); omit to read") },
    annotations: MODIFIES,
    handler: async ({ suspend }, { ac }) => ac.addon("SetSuspendGroups", { suspend }),
  });

  // --- Lock / drawing order -----------------------------------------------------------------

  defineTool(server, ctx, {
    name: "lock_elements",
    title: "Lock elements",
    description:
      "Locks elements (Edit > Locking > Lock): locked elements cannot be edited, moved or deleted until unlocked (unlock_elements). " +
      "Returns {results: [{guid, locked: true} | {guid, error}], warnings?}.",
    input: { elements: Elements, includeGroupMembers: IncludeGroupMembers },
    annotations: MODIFIES,
    handler: async ({ elements, includeGroupMembers }, { ac }) => ac.addon("LockElements", { elements: ids(elements), includeGroupMembers }),
  });

  defineTool(server, ctx, {
    name: "unlock_elements",
    title: "Unlock elements",
    description:
      "Unlocks locked elements (Edit > Locking > Unlock). Call this when another tool reports 'is locked'. Elements on a LOCKED LAYER " +
      "need the layer unlocked instead (layer attribute tools). Returns {results: [{guid, locked: false} | {guid, error}]}.",
    input: { elements: Elements, includeGroupMembers: IncludeGroupMembers },
    annotations: MODIFIES,
    handler: async ({ elements, includeGroupMembers }, { ac }) => ac.addon("UnlockElements", { elements: ids(elements), includeGroupMembers }),
  });

  defineTool(server, ctx, {
    name: "set_draw_order",
    title: "Set drawing order",
    description:
      "Changes the 2D stacking (display) order of elements on the plan (Edit > Display Order): which element covers which (e.g. a fill " +
      "behind lines). Give `action` ('BringToFront' | 'BringForward' | 'SendBackward' | 'SendToBack' | 'Reset' = default order of the type; " +
      "`steps` repeats BringForward/SendBackward) OR a target `level` 1 (bottom) .. 14 (top). Type defaults: drawings 5, fills/zones 6, " +
      "walls/slabs/roofs/columns/doors/windows 7, objects/lamps 8, lines/arcs/polylines/beams 9, texts/labels/dimensions 10. " +
      "Windows/doors cannot go below their host. Returns {results: [{guid, drawIndexBefore, drawIndex, warning?}]}.",
    input: {
      elements: Elements,
      action: z.enum(DRAW_ORDER_ACTIONS).optional().describe("Relative change (give this OR level)"),
      steps: z.number().int().min(1).max(14).optional().describe("Repeat BringForward/SendBackward this many levels (default 1)"),
      level: z.number().int().min(1).max(14).optional().describe("Absolute target drawing order level 1..14 (give this OR action)"),
      includeGroupMembers: IncludeGroupMembers,
    },
    annotations: DESTRUCTIVE,
    handler: async ({ elements, action, steps, level, includeGroupMembers }, { ac }) => {
      if ((action === undefined) === (level === undefined)) throw new Error("Give exactly one of `action` or `level`.");
      if (steps !== undefined && action !== "BringForward" && action !== "SendBackward") {
        throw new Error("`steps` only applies to action 'BringForward' or 'SendBackward'.");
      }
      return ac.addon("SetDrawOrder", { elements: ids(elements), action, steps, level, includeGroupMembers });
    },
  });

  // --- Trim / merge ---------------------------------------------------------------------------

  defineTool(server, ctx, {
    name: "trim_elements",
    title: "Trim elements to roof/shell",
    description:
      "Trims construction elements (walls, columns, beams, slabs, roofs, shells, curtain walls, windows/doors/skylights) to a Roof or Shell " +
      "(Design > Connect > Trim Elements to Roof/Shell), e.g. cut gable walls to the roof slope. With `trimWith` (a roof/shell GUID) " +
      "every element in `elements` is trimmed by it using `trimType`; without it the roofs/shells contained in `elements` trim the other " +
      "elements (default: keep the part below/inside). Returns {results: [{guid, trimmedBy: [{guid, trimType}]} | {guid, error}]}; " +
      "undo with remove_trim.",
    input: {
      elements: Elements.describe("Elements to trim (and, without trimWith, the trimming roofs/shells)"),
      trimWith: ElementRef.optional().describe("The Roof or Shell that trims the elements"),
      trimType: z
        .enum(TRIM_TYPES)
        .optional()
        .describe(
          "Needs trimWith. KeepInside (default) = keep the part inside/below the roof or shell (typical: walls under a roof); " +
            "KeepOutside = keep the part outside/above; KeepAll = keep all parts",
        ),
    },
    annotations: MODIFIES,
    handler: async ({ elements, trimWith, trimType }, { ac }) => {
      if (trimType && !trimWith) throw new Error("`trimType` needs `trimWith` (the roof/shell GUID).");
      return ac.addon("TrimElements", { elements: ids(elements), trimWith: trimWith ? guidOf(trimWith) : undefined, trimType });
    },
  });

  defineTool(server, ctx, {
    name: "remove_trim",
    title: "Remove trim",
    description:
      "Removes trim-to-roof/shell connections, restoring the untrimmed elements. Give explicit `pairs` [{element, trimmingElement}] and/or " +
      "`elements` (all trim connections of these elements are removed, whether they are trimmed or trimming). Inspect trims with " +
      "get_element_edit_relations. Returns {results: [{element, trimmingElement, removed: true} | {element, trimmingElement, error}], removedCount}.",
    input: {
      pairs: z
        .array(z.object({ element: ElementRef.describe("Trimmed element"), trimmingElement: ElementRef.describe("Roof/shell that trims it") }))
        .min(1)
        .max(1000)
        .optional(),
      elements: Elements.optional().describe("Remove every trim connection of these elements"),
    },
    annotations: MODIFIES,
    handler: async ({ pairs, elements }, { ac }) => {
      if (!pairs && !elements) throw new Error("Give `pairs` and/or `elements`.");
      return ac.addon("RemoveTrims", {
        pairs: pairs?.map((p) => ({ element: guidOf(p.element), trimmingElement: guidOf(p.trimmingElement) })),
        elements: elements ? ids(elements) : undefined,
      });
    },
  });

  defineTool(server, ctx, {
    name: "merge_elements",
    title: "Merge elements",
    description:
      "Merges construction elements (Design > Connect > Merge) so their 3D bodies and sections are displayed as one without contour " +
      "lines between them (e.g. slabs of the same material). Needs at least two elements. Undo with unmerge_elements. " +
      "Returns {merged, results: [{guid, mergedWith: [guid]}]} (merged=false with per-element errors when some element is not editable).",
    input: { elements: ElementRefs.min(2).max(5000).describe("At least two construction elements to merge") },
    annotations: MODIFIES,
    handler: async ({ elements }, { ac }) => ac.addon("MergeElements", { elements: ids(elements) }),
  });

  defineTool(server, ctx, {
    name: "unmerge_elements",
    title: "Unmerge elements",
    description:
      "Removes merge connections created by merge_elements. Give `pairs` [{element, otherElement}] and/or `elements` (all merges of these). " +
      "Returns {results: [{element, otherElement, removed: true} | {..., error}], removedCount}.",
    input: {
      pairs: z.array(z.object({ element: ElementRef, otherElement: ElementRef })).min(1).max(1000).optional(),
      elements: Elements.optional().describe("Remove every merge connection of these elements"),
    },
    annotations: MODIFIES,
    handler: async ({ pairs, elements }, { ac }) => {
      if (!pairs && !elements) throw new Error("Give `pairs` and/or `elements`.");
      return ac.addon("UnmergeElements", {
        pairs: pairs?.map((p) => ({ element: guidOf(p.element), otherElement: guidOf(p.otherElement) })),
        elements: elements ? ids(elements) : undefined,
      });
    },
  });

  // --- Solid element operations ---------------------------------------------------------------

  const SolidOperationSpec = z.object({
    target: ElementRef.describe("Target element that is cut/added to (keeps its identity)"),
    operators: ElementRefs.max(500).describe("Operator element(s) that cut/add to the target"),
    operation: z
      .enum(SOLID_OPERATIONS)
      .optional()
      .describe(
        "Subtract (default: remove the operator's volume from the target), SubtractUpwards / SubtractDownwards (remove the operator's " +
          "volume extruded upwards/downwards, e.g. cut a wall under a roof), Intersect (keep only the common part), Add (union)",
      ),
    inheritOperatorAttributes: z.boolean().optional().describe("New cut surfaces take the operator's surface/attributes (default false)"),
    skipOperatorHoles: z.boolean().optional().describe("Ignore holes of slab/roof operators (default false)"),
  });

  defineTool(server, ctx, {
    name: "solid_operation",
    title: "Solid element operation",
    description:
      "Solid Element Operations (Design > Solid Element Operations): cut or add 3D volumes between construction elements - e.g. subtract a " +
      "morph/slab/object from walls to make a niche, cut walls with SubtractUpwards/SubtractDownwards against a roof or slab. By default " +
      "(permanent=false) a LIVE link is created: it updates when elements move, operators are usually put on a hidden layer afterwards, " +
      "and it can be removed with remove_solid_operation. permanent=true performs a destructive Boolean on MORPHS only: target and operators are replaced by " +
      "the result morph(s). Give target/operators/operation for one operation or `operations` for several (one undo step). " +
      "Returns {results: [{target, links: [{operator, operation} | {operator, error}]}]} or, when permanent, [{target, resultMorphs: [guid]}].",
    input: {
      target: SolidOperationSpec.shape.target.optional(),
      operators: SolidOperationSpec.shape.operators.optional(),
      operation: SolidOperationSpec.shape.operation,
      inheritOperatorAttributes: SolidOperationSpec.shape.inheritOperatorAttributes,
      skipOperatorHoles: SolidOperationSpec.shape.skipOperatorHoles,
      operations: z.array(SolidOperationSpec).min(1).max(500).optional().describe("Several operations [{target, operators, operation?, ...}]"),
      permanent: z.boolean().optional().describe("true = destructive morph Boolean (morphs only, inputs replaced); default false = live link"),
    },
    annotations: MODIFIES,
    handler: async ({ target, operators, operation, inheritOperatorAttributes, skipOperatorHoles, operations, permanent }, { ac }) => {
      const toSpec = (s: z.infer<typeof SolidOperationSpec>) => ({
        target: guidOf(s.target),
        operators: ids(s.operators),
        operation: s.operation,
        inheritOperatorAttributes: s.inheritOperatorAttributes,
        skipOperatorHoles: s.skipOperatorHoles,
      });
      if (operations) {
        if (target || operators) throw new Error("Give either `operations` or `target` + `operators`, not both.");
        return ac.addon("CreateSolidOperations", { operations: operations.map(toSpec), permanent });
      }
      if (!target || !operators) throw new Error("Give `target` and `operators` (or `operations: [{target, operators}]`).");
      return ac.addon("CreateSolidOperations", {
        operations: [toSpec({ target, operators, operation, inheritOperatorAttributes, skipOperatorHoles })],
        permanent,
      });
    },
  });

  defineTool(server, ctx, {
    name: "remove_solid_operation",
    title: "Remove solid operation",
    description:
      "Removes live solid element operation links (created by solid_operation or in Archicad). Give `links` [{target, operator}] and/or " +
      "`elements` (every link where the element is target or operator). Inspect links with get_element_edit_relations. " +
      "Returns {results: [{target, operator, removed: true} | {target, operator, error}], removedCount}.",
    input: {
      links: z.array(z.object({ target: ElementRef, operator: ElementRef })).min(1).max(1000).optional(),
      elements: Elements.optional().describe("Remove every solid operation link of these elements"),
    },
    annotations: MODIFIES,
    handler: async ({ links, elements }, { ac }) => {
      if (!links && !elements) throw new Error("Give `links` and/or `elements`.");
      return ac.addon("RemoveSolidOperations", {
        links: links?.map((l) => ({ target: guidOf(l.target), operator: guidOf(l.operator) })),
        elements: elements ? ids(elements) : undefined,
      });
    },
  });

  // --- Inspection -------------------------------------------------------------------------------

  defineTool(server, ctx, {
    name: "get_element_edit_relations",
    title: "Element edit relations",
    description:
      "Read-only. For each element returns what affects editing it: type, storyIndex, locked, editable, drawIndex, group membership " +
      "(groupGuid, rootGroupGuid, groupElements), hotlinkGuid, trimmedBy / trims (trim-to-roof connections with trimType), mergedWith, " +
      "solidOperators (elements cutting/adding to it) and solidTargets (elements it cuts). Use it to verify group/trim/merge/solid " +
      "operations or to find out why an edit was refused.",
    input: { elements: ElementRefs.max(1000) },
    annotations: READ_ONLY,
    handler: async ({ elements }, { ac }) => ac.addon("GetElementEditRelations", { elements: ids(elements) }),
  });
}
