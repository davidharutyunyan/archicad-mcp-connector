/**
 * Collaboration tools: Teamwork (BIMcloud/BIMserver), issues (Issue Manager markup entries, BCF),
 * favorites, tool default settings and revision management (read-only).
 * Backed by the add-on commands in addon/Src/Commands/Collaboration.cpp; field names match 1:1:
 *   GetTeamworkStatus, TeamworkSend, TeamworkReceive, ReserveElements, ReleaseElements,
 *   GetIssues, CreateIssues, DeleteIssues, AddIssueComments, GetIssueComments, AttachElementsToIssue,
 *   DetachElementsFromIssue, GetIssueElements, ExportIssuesToBCF, ImportIssuesFromBCF,
 *   GetFavorites, ApplyFavorite, CreateFavorites, DeleteFavorites, RenameFavorite, ExportFavorites, ImportFavorites,
 *   GetToolDefaults, SetToolDefaults, GetRevisions, GetRevisionChanges.
 */

import { homedir } from "node:os";
import { isAbsolute, join } from "node:path";

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import type { ToolAnnotations } from "@modelcontextprotocol/sdk/types.js";
import { z } from "zod";

import { CREATES, defineTool, DESTRUCTIVE, MODIFIES, READ_ONLY, type ToolContext } from "./define.js";
import { ELEMENT_TYPES } from "./element-query.js";
import { ElementRef, ElementRefs, Guid, guidOf, Pagination, paginate } from "./schemas.js";

// =============================================================================
// Shared schemas and helpers
// =============================================================================

/** Teamwork object sets that can be reserved like elements (attribute types, favorites, project settings...). */
export const OBJECT_SETS = [
  "LayerSettings", "LineTypes", "FillTypes", "Composites", "PenSets", "Surfaces", "BuildingMaterials", "ZoneCategories",
  "Profiles", "MEPSystems", "OperationProfiles", "ModelViewOptions", "Favorites", "IssueTags", "ClassificationsAndProperties",
  "ProjectInfo", "ProjectPreferences", "ProjectLibraryList",
] as const;

const ObjectSet = z
  .enum(OBJECT_SETS)
  .describe(
    "Teamwork object set: LayerSettings (layers + layer combinations), LineTypes, FillTypes, Composites, PenSets, Surfaces, " +
      "BuildingMaterials, ZoneCategories, Profiles, MEPSystems, OperationProfiles, ModelViewOptions, Favorites, IssueTags, " +
      "ClassificationsAndProperties, ProjectInfo, ProjectPreferences, ProjectLibraryList",
  );

const ToolElementType = z
  .enum(ELEMENT_TYPES)
  .describe("Element tool type (case-sensitive), e.g. 'Wall', 'Column', 'Beam', 'Slab', 'Roof', 'Door', 'Window', 'Object', 'Zone', 'Line', 'Text'");

export const VARIATIONS = ["Generic", "Object", "Light", "SymbStair", "GridElement", "WallEnd", "Door", "Skylight", "CornerWindow"] as const;

const Variation = z
  .enum(VARIATIONS)
  .describe(
    "Tool variation, only for tools that share an element type (e.g. 'GridElement' vs 'Object' for Object elements). " +
      "Normally omit it; get_tool_defaults without a type lists the toolbox tools with their variations",
  );

/** Issue reference: GUID or exact name. */
export const IssueRef = z
  .union([z.string().min(1), z.object({ guid: Guid }), z.object({ name: z.string().min(1) })])
  .describe("Issue GUID or exact issue name (case-insensitive fallback) — see get_issues; use the GUID when names repeat");

type IssueRefValue = z.infer<typeof IssueRef>;

export function issueRefOf(ref: IssueRefValue): string {
  if (typeof ref === "string") return ref;
  return "guid" in ref ? ref.guid : ref.name;
}

const IssueRefs = z.array(IssueRef).min(1).max(1000);

const CommentStatus = z
  .enum(["Error", "Warning", "Info", "Unknown"])
  .describe("Comment status shown in the Issue Manager: Error, Warning, Info or Unknown (default)");

const CommentBody = z.object({
  text: z.string().min(1).max(10000).describe("Comment text"),
  author: z.string().max(255).optional().describe("Author shown in the Issue Manager (default: the Teamwork user name, or 'Claude')"),
  status: CommentStatus.optional(),
});

export const ATTACHMENT_TYPES = ["Highlight", "Creation", "Deletion", "Modification"] as const;

const AttachmentType = z
  .enum(ATTACHMENT_TYPES)
  .describe(
    "How the elements relate to the issue: Highlight (just point at them; default), Creation (proposed new elements), " +
      "Deletion (elements proposed for deletion), Modification (proposed changes: pairs original → modified)",
  );

/** Folder of the Favorites palette: ["A", "B"] or "A/B". */
const FavoriteFolder = z
  .union([z.array(z.string().min(1)).max(20), z.string()])
  .describe("Favorites palette folder: array of folder names from the root (['Walls', 'Exterior']) or a path 'Walls/Exterior'");

const ChangeRef = z.union([z.string().min(1), z.object({ guid: Guid })]);

function refString(ref: z.infer<typeof ChangeRef>): string {
  return typeof ref === "string" ? ref : ref.guid;
}

/** Expands "~" and requires an absolute path (Archicad runs on this machine). */
export function resolveLocalPath(path: string, what: string): string {
  let p = path.trim();
  if (p === "~") p = homedir();
  else if (p.startsWith("~/")) p = join(homedir(), p.slice(2));
  if (!isAbsolute(p)) {
    throw new Error(`Invalid input: ${what} must be an absolute path on the Archicad machine (e.g. /Users/me/Desktop/file), got '${path}'`);
  }
  return p;
}

function fail(problems: string[]): void {
  if (problems.length > 0) throw new Error(`Invalid input: ${problems.join("; ")}`);
}

/** Adds only the defined values of `src` to `dst`. */
function assignDefined(dst: Record<string, unknown>, src: Record<string, unknown>): Record<string, unknown> {
  for (const [k, v] of Object.entries(src)) if (v !== undefined) dst[k] = v;
  return dst;
}

/** Applies client-side offset/limit to response[key] when pagination was requested. */
function paginateField(response: unknown, key: string, offset?: number, limit?: number): unknown {
  if (offset === undefined && limit === undefined) return response;
  if (!response || typeof response !== "object") return response;
  const obj = response as Record<string, unknown>;
  const list = obj[key];
  if (!Array.isArray(list)) return response;
  const page = paginate(list, offset ?? 0, limit ?? 500);
  return { ...obj, [key]: page.items, count: page.items.length, total: page.total, offset: page.offset, hasMore: page.hasMore };
}

const WRITES_FILE: ToolAnnotations = { readOnlyHint: false, destructiveHint: false, idempotentHint: true };

const TEAMWORK_NOTE =
  "In a solo (non-Teamwork) project this does nothing and returns {isTeamwork: false, message} — that is not an error: " +
  "everything is editable without reservation.";

// =============================================================================
// Registration
// =============================================================================

export function registerCollaborationTools(server: McpServer, ctx: ToolContext): void {
  registerTeamworkTools(server, ctx);
  registerIssueTools(server, ctx);
  registerFavoriteTools(server, ctx);
  registerToolDefaultTools(server, ctx);
  registerRevisionTools(server, ctx);
}

// =============================================================================
// Teamwork
// =============================================================================

const ReservationInput = {
  elements: ElementRefs.max(10000).optional().describe("Elements to reserve/release (GUIDs)"),
  objectSets: z.array(ObjectSet).min(1).max(OBJECT_SETS.length).optional().describe(
    "Object sets to reserve/release: needed before editing attributes (e.g. 'Composites' before modifying composites, " +
      "'LayerSettings' for layers), favorites, project info or project preferences in Teamwork",
  ),
  hotlinkCacheManagement: z.boolean().optional().describe("Also reserve/release Hotlink Cache Management (needed to update/relink hotlinks)"),
  enableDialogs: z.boolean().optional().describe("Let Archicad show its own Teamwork dialogs (e.g. request/conflict messages). Default false"),
};

type ReservationArgs = {
  elements?: z.infer<typeof ElementRef>[];
  objectSets?: (typeof OBJECT_SETS)[number][];
  hotlinkCacheManagement?: boolean;
  enableDialogs?: boolean;
};

function reservationParams(args: ReservationArgs, verb: string): Record<string, unknown> {
  if ((args.elements?.length ?? 0) === 0 && (args.objectSets?.length ?? 0) === 0 && !args.hotlinkCacheManagement) {
    fail([`nothing to ${verb}: give elements (GUIDs), objectSets and/or hotlinkCacheManagement: true`]);
  }
  return assignDefined(
    {},
    {
      elements: args.elements?.map(guidOf),
      objectSets: args.objectSets,
      hotlinkCacheManagement: args.hotlinkCacheManagement,
      enableDialogs: args.enableDialogs,
    },
  );
}

function registerTeamworkTools(server: McpServer, ctx: ToolContext): void {
  defineTool(server, ctx, {
    name: "get_teamwork_status",
    title: "Teamwork status",
    description:
      "Teamwork (BIMcloud/BIMserver) status of the open project — call this before reserve_elements / release_elements / " +
      "teamwork_send / teamwork_receive. Solo project: {isTeamwork: false, message, project: {name, path, untitled}} — then nothing " +
      "needs to be reserved and the teamwork tools do nothing. Teamwork project: {isTeamwork: true, project, teamwork: {hasConnection, " +
      "online, serverUrl, teamProjectName, loginName, teamProjectLocation}, currentUser: {userId, name}, members?: [{userId, loginName, " +
      "fullName, connected}], hotlinkCacheManagementReservedBy?, warning? (server offline), elements?: [{guid, status: Free|ReservedByMe|" +
      "ReservedByOther|ServerUnavailable|NotExist, reservedBy?: [user names]} | {error}], objectSets?: [{name, status, reservedBy?, " +
      "canCreate, canDeleteModify}], accessRights?: {RightName: bool}}. Only elements with status ReservedByMe (or Free after reserving) " +
      "can be modified.",
    input: {
      elements: ElementRefs.max(10000).optional().describe("Also return the reservation status of these elements"),
      objectSets: z
        .union([z.literal(true), z.array(ObjectSet).min(1).max(OBJECT_SETS.length)])
        .optional()
        .describe("Also return the reservation status of object sets: true = all of them, or a list of names"),
      includeMembers: z.boolean().optional().describe("List the team members with their online state (default true)"),
      includeAccessRights: z
        .boolean()
        .optional()
        .describe("Return the current user's Teamwork role rights as {RightName: bool} (e.g. LayersCreate, IssuesCreateModify). Default false"),
    },
    annotations: READ_ONLY,
    handler: async ({ elements, objectSets, includeMembers, includeAccessRights }, { ac }) =>
      ac.addon(
        "GetTeamworkStatus",
        assignDefined({}, { elements: elements?.map(guidOf), objectSets, includeMembers, includeAccessRights }),
      ),
  });

  defineTool(server, ctx, {
    name: "teamwork_send",
    title: "Teamwork Send",
    description:
      "Teamwork 'Send': uploads your changes to the BIMcloud/BIMserver so team members can receive them (reserved elements stay " +
      "reserved). Output: {isTeamwork: true, sent: true}. " + TEAMWORK_NOTE,
    input: {
      comment: z.string().max(2000).optional().describe("Comment stored with the sent changes (shown in the project history)"),
    },
    annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: false, openWorldHint: true },
    handler: async ({ comment }, { ac }) => ac.addon("TeamworkSend", assignDefined({}, { comment })),
  });

  defineTool(server, ctx, {
    name: "teamwork_receive",
    title: "Teamwork Receive",
    description:
      "Teamwork 'Receive': downloads the changes that other team members have sent, updating the local model. Output: " +
      "{isTeamwork: true, received: true}. " + TEAMWORK_NOTE,
    input: {},
    annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: true, openWorldHint: true },
    handler: async (_args, { ac }) => ac.addon("TeamworkReceive", {}),
  });

  defineTool(server, ctx, {
    name: "reserve_elements",
    title: "Reserve (Teamwork)",
    description:
      "Teamwork: reserves elements and/or object sets so you can edit them (elements reserved by someone else cannot be taken; " +
      "the result names the owner). Output: {isTeamwork: true, elements?: [{guid, status, reserved, conflictWith?, reservedBy?} | {error}], " +
      "objectSets?: [{name, status, reserved, apiError?, conflictWith?}], warning?}. Check get_teamwork_status first. " + TEAMWORK_NOTE,
    input: ReservationInput,
    annotations: MODIFIES,
    handler: async (args, { ac }) => ac.addon("ReserveElements", reservationParams(args, "reserve")),
  });

  defineTool(server, ctx, {
    name: "release_elements",
    title: "Release (Teamwork)",
    description:
      "Teamwork: releases reserved elements and/or object sets so others can edit them. Archicad sends your pending changes of the " +
      "released items with the release (call teamwork_send first to send everything with a comment). Output: {isTeamwork: true, " +
      "elements?: [{guid, status, released} | {error}], objectSets?: [{name, status, released, apiError?}], warning?}. " + TEAMWORK_NOTE,
    input: ReservationInput,
    annotations: MODIFIES,
    handler: async (args, { ac }) => ac.addon("ReleaseElements", reservationParams(args, "release")),
  });
}

// =============================================================================
// Issues (Issue Manager / BCF)
// =============================================================================

/** Element list inside create_issue.attach (empty lists are allowed and ignored). */
const AttachList = z.array(ElementRef).max(10000);

const IssueSpec = z.object({
  name: z.string().min(1).max(255).describe("Issue name (title) shown in the Issue Manager"),
  tagText: z.string().max(1000).optional().describe("Tag text: short label placed as a tag next to the highlighted elements"),
  tagTextVisible: z.boolean().optional().describe("Show the tag text element in the model views (default true)"),
  parentIssue: IssueRef.optional().describe("Make this a sub-issue of an existing issue (GUID or exact name)"),
  comment: z.union([z.string().min(1).max(10000), CommentBody]).optional().describe("First comment: text, or {text, author?, status?}"),
  comments: z.array(CommentBody).max(100).optional().describe("Further comments, added in order after 'comment'"),
  attach: z
    .object({
      highlight: AttachList.optional().describe("Elements to highlight (point at) in this issue"),
      creation: AttachList.optional().describe("Elements proposed as new (Creation)"),
      deletion: AttachList.optional().describe("Elements proposed for deletion (Deletion)"),
      modification: AttachList
        .optional()
        .describe("Elements proposed for modification: Archicad creates a modified copy of each (use attach_elements_to_issue with modificationPairs to pair existing elements)"),
    })
    .optional()
    .describe("Elements to attach right away, by attachment type"),
});

function issueSpecParams(spec: z.infer<typeof IssueSpec>): Record<string, unknown> {
  const out: Record<string, unknown> = { name: spec.name };
  assignDefined(out, { tagText: spec.tagText, tagTextVisible: spec.tagTextVisible, comments: spec.comments });
  if (spec.parentIssue !== undefined) out["parentIssue"] = issueRefOf(spec.parentIssue);
  if (spec.comment !== undefined) out["comment"] = spec.comment;
  if (spec.attach !== undefined) {
    const attach: Record<string, unknown> = {};
    for (const key of ["highlight", "creation", "deletion", "modification"] as const) {
      const list = spec.attach[key];
      if (list !== undefined && list.length > 0) attach[key] = list.map(guidOf);
    }
    if (Object.keys(attach).length > 0) out["attach"] = attach;
  }
  return out;
}

function registerIssueTools(server: McpServer, ctx: ToolContext): void {
  defineTool(server, ctx, {
    name: "get_issues",
    title: "Get issues",
    description:
      "Lists the issues of the Issue Manager (Archicad markup entries = BCF topics). Output: {issues: [{guid, name, parentGuid?, " +
      "parentName?, childIssues?, created, modified (ISO 8601 UTC), tagText, tagTextVisible, tagTextElement?, commentCount, comments?, " +
      "attachedElementCounts: {creation, highlight, deletion, modification}, attachedElements?: {creation: [guid], ...}}], count, " +
      "elementAttachment? ('highlighted'|'corrected' when filtered by element), total/offset/hasMore when paginated}. " +
      "Issue GUIDs from here are accepted by every other issue tool.",
    input: {
      issues: IssueRefs.optional().describe("Only these issues (GUIDs or exact names); default: all issues"),
      element: ElementRef.optional().describe("Only the issues this element is attached to"),
      search: z.string().min(1).optional().describe("Case-insensitive substring of the issue name or tag text"),
      includeComments: z.boolean().optional().describe("Include the full comment list of each issue (default false; commentCount is always returned)"),
      includeElements: z.boolean().optional().describe("Include the attached element GUIDs by attachment type (default false; counts are always returned)"),
      ...Pagination,
    },
    annotations: READ_ONLY,
    handler: async ({ issues, element, search, includeComments, includeElements, offset, limit }, { ac }) => {
      const params = assignDefined(
        {},
        { issues: issues?.map(issueRefOf), element: element ? guidOf(element) : undefined, search, includeComments, includeElements },
      );
      return paginateField(await ac.addon("GetIssues", params), "issues", offset, limit);
    },
  });

  defineTool(server, ctx, {
    name: "create_issue",
    title: "Create issues",
    description:
      "Creates one or more issues in the Issue Manager (one undo step), optionally with comments and attached elements — use it to " +
      "report problems found in the model (clashes, missing data, review notes) so the user sees them in Archicad and can export them " +
      "to BCF. Output: {results: [{guid, name, comments: [commentGuid], attached?: {highlight?|creation?|deletion?|modification?: " +
      "{attached: [guid], missing?, modificationPairs?}}} | {error}]} in input order.",
    input: {
      issues: z.array(IssueSpec).min(1).max(200).describe("Issues to create"),
      undoName: z.string().max(255).optional().describe("Name of the undo step shown in Archicad"),
    },
    annotations: CREATES,
    handler: async ({ issues, undoName }, { ac }) =>
      ac.addon("CreateIssues", assignDefined({ issues: issues.map(issueSpecParams) }, { undoName })),
  });

  defineTool(server, ctx, {
    name: "delete_issue",
    title: "Delete issues",
    description:
      "Deletes issues (one undo step) together with their comments. By default the proposals attached as Creation/Deletion/Modification " +
      "are discarded (the model stays as it is); acceptAllElements: true accepts them first. Output: {results: [{guid, name, deleted: true} | {error}]}.",
    input: {
      issues: IssueRefs.describe("Issues to delete (GUIDs or exact names, see get_issues)"),
      acceptAllElements: z
        .boolean()
        .optional()
        .describe("Accept the attached creation/deletion/modification proposals before deleting (default false)"),
      undoName: z.string().max(255).optional(),
    },
    annotations: DESTRUCTIVE,
    handler: async ({ issues, acceptAllElements, undoName }, { ac }) =>
      ac.addon("DeleteIssues", assignDefined({ issues: issues.map(issueRefOf) }, { acceptAllElements, undoName })),
  });

  defineTool(server, ctx, {
    name: "add_issue_comment",
    title: "Add issue comments",
    description:
      "Adds comments to issues (one undo step; several comments / issues per call). Output: {results: [{issue: {guid, name}, comment: " +
      "{guid, author, text, status, created}} | {error}]} in input order.",
    input: {
      comments: z
        .array(CommentBody.extend({ issue: IssueRef.describe("Issue to comment on (GUID or exact name)") }))
        .min(1)
        .max(500)
        .describe("Comments to add"),
      undoName: z.string().max(255).optional(),
    },
    annotations: CREATES,
    handler: async ({ comments, undoName }, { ac }) =>
      ac.addon(
        "AddIssueComments",
        assignDefined(
          { comments: comments.map((c) => assignDefined({ issue: issueRefOf(c.issue), text: c.text }, { author: c.author, status: c.status })) },
          { undoName },
        ),
      ),
  });

  defineTool(server, ctx, {
    name: "get_issue_comments",
    title: "Get issue comments",
    description:
      "Returns the comments of issues, oldest first. Output: {issues: [{guid, name, comments: [{guid, author, text, status: " +
      "Error|Warning|Info|Unknown, created (ISO 8601 UTC)}]}]}.",
    input: {
      issues: IssueRefs.optional().describe("Issues to read (GUIDs or exact names); default: all issues"),
    },
    annotations: READ_ONLY,
    handler: async ({ issues }, { ac }) => ac.addon("GetIssueComments", assignDefined({}, { issues: issues?.map(issueRefOf) })),
  });

  defineTool(server, ctx, {
    name: "attach_elements_to_issue",
    title: "Attach elements to issue",
    description:
      "Attaches elements to an issue (one undo step). type Highlight (default) just points at them; Creation / Deletion mark them as " +
      "proposed new / to-be-deleted elements; Modification either pairs existing elements (modificationPairs: original → proposed " +
      "replacement) or, with plain elements, lets Archicad create a modified copy of each. Output: {issue: {guid, name}, type, " +
      "attached: [guid], missing?: [guid of non-existing elements], modificationPairs?: [{original, modified}]}.",
    input: {
      issue: IssueRef.describe("Issue (GUID or exact name, see get_issues)"),
      elements: ElementRefs.max(10000).optional().describe("Elements to attach"),
      type: AttachmentType.optional(),
      modificationPairs: z
        .array(z.object({ original: ElementRef.describe("Existing element"), modified: ElementRef.describe("Its proposed modified version") }))
        .min(1)
        .max(5000)
        .optional()
        .describe("Only with type Modification (implied when omitted): pairs of original and modified elements"),
      undoName: z.string().max(255).optional(),
    },
    annotations: MODIFIES,
    handler: async ({ issue, elements, type, modificationPairs, undoName }, { ac }) => {
      const problems: string[] = [];
      if ((elements?.length ?? 0) === 0 && (modificationPairs?.length ?? 0) === 0) problems.push("give elements and/or modificationPairs");
      if (modificationPairs !== undefined && type !== undefined && type !== "Modification") {
        problems.push(`modificationPairs require type 'Modification' (got '${type}')`);
      }
      fail(problems);
      const effectiveType = type ?? (modificationPairs !== undefined ? "Modification" : undefined);
      return ac.addon(
        "AttachElementsToIssue",
        assignDefined(
          { issue: issueRefOf(issue) },
          {
            elements: elements?.map(guidOf),
            type: effectiveType,
            modificationPairs: modificationPairs?.map((p) => ({ original: guidOf(p.original), modified: guidOf(p.modified) })),
            undoName,
          },
        ),
      );
    },
  });

  defineTool(server, ctx, {
    name: "detach_elements_from_issue",
    title: "Detach elements from issue",
    description:
      "Detaches elements from an issue whatever their attachment type (one undo step); the elements themselves are not changed. " +
      "Output: {issue: {guid, name}, detached: [guid], notAttached?: [guid]}. Fails when none of them is attached (see get_issue_elements).",
    input: {
      issue: IssueRef.describe("Issue (GUID or exact name)"),
      elements: ElementRefs.max(10000).describe("Elements to detach"),
      undoName: z.string().max(255).optional(),
    },
    annotations: MODIFIES,
    handler: async ({ issue, elements, undoName }, { ac }) =>
      ac.addon("DetachElementsFromIssue", assignDefined({ issue: issueRefOf(issue), elements: elements.map(guidOf) }, { undoName })),
  });

  defineTool(server, ctx, {
    name: "get_issue_elements",
    title: "Get issue elements",
    description:
      "Returns the elements attached to issues, by attachment type. Output: {issues: [{guid, name, tagTextElement?, elements: " +
      "{creation|highlight|deletion|modification: [{guid, type} | {guid, missing: true}]}}]}. Pass the GUIDs to get_element_details, " +
      "select_elements or zoom tools to inspect them.",
    input: {
      issues: IssueRefs.optional().describe("Issues to read (GUIDs or exact names); default: all issues"),
      types: z.array(AttachmentType).min(1).max(4).optional().describe("Only these attachment types (default: all four)"),
    },
    annotations: READ_ONLY,
    handler: async ({ issues, types }, { ac }) =>
      ac.addon("GetIssueElements", assignDefined({}, { issues: issues?.map(issueRefOf), types })),
  });

  defineTool(server, ctx, {
    name: "export_bcf",
    title: "Export issues to BCF",
    description:
      "Exports issues to a BCF file (.bcfzip, BCF 2.1) for other BIM tools (Solibri, Revit, BIMcollab, ...). Output: {path, exported, " +
      "issues: [{guid, name}], fileExists}.",
    input: {
      path: z
        .string()
        .min(2)
        .describe("Absolute target file path on the Archicad machine, e.g. '/Users/me/Desktop/review.bcfzip' ('~/' allowed; '.bcfzip' is appended when there is no extension)"),
      issues: IssueRefs.optional().describe("Issues to export (GUIDs or exact names); default: all issues"),
      useExternalId: z
        .boolean()
        .optional()
        .describe("Reference elements by the IFC GlobalIds they had in an imported IFC model (default false = Archicad's own IFC GUIDs)"),
      alignBySurveyPoint: z.boolean().optional().describe("Use the Survey Point as the coordinate origin (default true)"),
      overwrite: z.boolean().optional().describe("Replace an existing file (default false: an existing file is an error)"),
      createFolders: z.boolean().optional().describe("Create missing parent folders (default false)"),
    },
    annotations: WRITES_FILE,
    handler: async ({ path, issues, useExternalId, alignBySurveyPoint, overwrite, createFolders }, { ac }) => {
      const target = resolveLocalPath(path, "path");
      const ext = /\.([^./\\]+)$/.exec(target)?.[1]?.toLowerCase();
      if (ext !== undefined && ext !== "bcfzip" && ext !== "bcf") fail([`path: BCF files end in .bcfzip (or .bcf), got '.${ext}'`]);
      return ac.addon(
        "ExportIssuesToBCF",
        assignDefined({ path: target }, { issues: issues?.map(issueRefOf), useExternalId, alignBySurveyPoint, overwrite, createFolders }),
      );
    },
  });

  defineTool(server, ctx, {
    name: "import_bcf",
    title: "Import issues from BCF",
    description:
      "Imports the issues (topics, comments, referenced elements) of a BCF file (.bcfzip / .bcf, BCF 2.x) into the Issue Manager " +
      "without dialogs (one undo step). Elements referenced by IFC GlobalId are matched to the model. Output: {imported, issues: " +
      "[...the new issues in get_issues format]}.",
    input: {
      path: z.string().min(2).describe("Absolute path of the BCF file on the Archicad machine ('~/' allowed)"),
      alignBySurveyPoint: z.boolean().optional().describe("The BCF coordinates are relative to the Survey Point (default true)"),
      openIssuePalette: z.boolean().optional().describe("Open the Issue Manager palette after the import (default false)"),
      undoName: z.string().max(255).optional(),
    },
    annotations: CREATES,
    handler: async ({ path, alignBySurveyPoint, openIssuePalette, undoName }, { ac }) =>
      ac.addon(
        "ImportIssuesFromBCF",
        assignDefined({ path: resolveLocalPath(path, "path") }, { alignBySurveyPoint, openIssuePalette, undoName }),
      ),
  });
}

// =============================================================================
// Favorites
// =============================================================================

const FavoriteName = z.string().min(1).max(255).describe("Exact favorite name (localized, as listed by get_favorites)");

const ToolTypeRef = z.object({ type: ToolElementType, variation: Variation.optional() });

const FavoriteSpec = z.object({
  name: z.string().min(1).max(255).describe("Name of the new favorite"),
  element: ElementRef.optional().describe("Copy the settings of this placed element (like 'Save selection as Favorite')"),
  toolDefaults: ToolTypeRef.optional().describe("Copy the current default settings of this tool (see get_tool_defaults / set_tool_defaults)"),
  folder: FavoriteFolder.optional().describe("Favorites palette folder to put it in (default: root). Missing folders are created"),
  replace: z.boolean().optional().describe("Overwrite an existing favorite with the same name (default false = error)"),
  includeProperties: z.boolean().optional().describe("Store the custom property values too (default true)"),
  includeClassifications: z.boolean().optional().describe("Store the classifications too (default true)"),
  includeCategories: z.boolean().optional().describe("Store the element categories (structural function, position, renovation) too (default true)"),
});

function registerFavoriteTools(server: McpServer, ctx: ToolContext): void {
  defineTool(server, ctx, {
    name: "get_favorites",
    title: "Get favorites",
    description:
      "Lists the favorites (saved tool settings of the Favorites palette). Output: {favorites: [{name, type, variation?, folder: [..], " +
      "folderPath, settings?, classifications?, categories?, properties?, notes?} | {error}], count, total/offset/hasMore when paginated}. " +
      "With includeSettings the settings use the same field names as get_tool_defaults and the create_* tools. Favorite names are " +
      "localized (Russian templates ship Russian names) — always take them from here. Filter by type/search/folder before using includeSettings.",
    input: {
      type: ToolElementType.optional().describe("Only favorites of this element type"),
      variation: Variation.optional(),
      search: z.string().min(1).optional().describe("Case-insensitive substring of the favorite name"),
      folder: FavoriteFolder.optional().describe("Only favorites in this folder (or its subfolders)"),
      names: z.array(FavoriteName).min(1).max(500).optional().describe("Only these exact names"),
      includeSettings: z
        .boolean()
        .optional()
        .describe("Also return each favorite's settings, classifications, categories and properties (default false; slower)"),
      includeGdlParameters: z
        .boolean()
        .optional()
        .describe("With includeSettings: include the GDL parameter values of library-part based favorites (default true)"),
      ...Pagination,
    },
    annotations: READ_ONLY,
    handler: async ({ type, variation, search, folder, names, includeSettings, includeGdlParameters, offset, limit }, { ac }) => {
      if (variation !== undefined && type === undefined) fail(["variation needs type"]);
      const params = assignDefined({}, { type, variation, search, folder, names, includeSettings, includeGdlParameters });
      return paginateField(await ac.addon("GetFavorites", params), "favorites", offset, limit);
    },
  });

  defineTool(server, ctx, {
    name: "apply_favorite",
    title: "Apply favorite",
    description:
      "Applies a favorite. target 'Defaults' (default) makes it the current settings of its tool, like double-clicking it in the " +
      "Favorites palette — elements created afterwards (in Archicad or with create_* tools without explicit values) get these settings. " +
      "target 'Elements' injects its settings into existing elements of the SAME type (geometry, position and story are kept). " +
      "Output: {favorite, type, variation?, target, applied? (Defaults), results?: [{guid, applied: true} | {error}] (Elements), warnings?}.",
    input: {
      name: FavoriteName,
      target: z.enum(["Defaults", "Elements"]).optional().describe("'Defaults' (tool settings) or 'Elements' (existing elements). Default: Elements when elements are given, else Defaults"),
      elements: ElementRefs.max(10000).optional().describe("target Elements: the elements to change (must be of the favorite's type)"),
      applyProperties: z.boolean().optional().describe("Also apply the favorite's stored property values (default true)"),
      applyClassifications: z.boolean().optional().describe("Also apply the favorite's classifications (default true)"),
      applyCategories: z.boolean().optional().describe("Also apply the favorite's element categories (default true)"),
      undoName: z.string().max(255).optional(),
    },
    annotations: MODIFIES,
    handler: async ({ name, target, elements, applyProperties, applyClassifications, applyCategories, undoName }, { ac }) => {
      const effective = target ?? ((elements?.length ?? 0) > 0 ? "Elements" : "Defaults");
      const problems: string[] = [];
      if (effective === "Elements" && (elements?.length ?? 0) === 0) problems.push("target 'Elements' needs elements (GUIDs)");
      if (effective === "Defaults" && elements !== undefined) problems.push("elements are only used with target 'Elements'");
      fail(problems);
      return ac.addon(
        "ApplyFavorite",
        assignDefined(
          { name, target: effective },
          { elements: elements?.map(guidOf), applyProperties, applyClassifications, applyCategories, undoName },
        ),
      );
    },
  });

  defineTool(server, ctx, {
    name: "create_favorite",
    title: "Create favorites",
    description:
      "Creates favorites in the Favorites palette from placed elements or from the current tool defaults (set them first with " +
      "set_tool_defaults to build a favorite from scratch). Each item needs exactly one of element / toolDefaults. Output: " +
      "{results: [{name, type, variation?, folder, replaced?} | {error}]} in input order. In Teamwork, reserve the 'Favorites' object set first.",
    input: {
      favorites: z.array(FavoriteSpec).min(1).max(100).describe("Favorites to create"),
    },
    annotations: CREATES,
    handler: async ({ favorites }, { ac }) => {
      const problems: string[] = [];
      favorites.forEach((f, i) => {
        const n = (f.element !== undefined ? 1 : 0) + (f.toolDefaults !== undefined ? 1 : 0);
        if (n !== 1) problems.push(`favorites[${i}]: give exactly one of element (GUID) or toolDefaults ({type})`);
      });
      fail(problems);
      return ac.addon("CreateFavorites", {
        favorites: favorites.map((f) =>
          assignDefined(
            { name: f.name },
            {
              element: f.element !== undefined ? guidOf(f.element) : undefined,
              toolDefaults: f.toolDefaults,
              folder: f.folder,
              replace: f.replace,
              includeProperties: f.includeProperties,
              includeClassifications: f.includeClassifications,
              includeCategories: f.includeCategories,
            },
          ),
        ),
      });
    },
  });

  defineTool(server, ctx, {
    name: "delete_favorite",
    title: "Delete favorites",
    description:
      "Deletes favorites by exact name (placed elements are not affected). Output: {results: [{name, deleted: true} | {error}]}. " +
      "In Teamwork, reserve the 'Favorites' object set first.",
    input: {
      names: z.array(FavoriteName).min(1).max(500).describe("Exact names of the favorites to delete (see get_favorites)"),
    },
    annotations: DESTRUCTIVE,
    handler: async ({ names }, { ac }) => ac.addon("DeleteFavorites", { names }),
  });

  defineTool(server, ctx, {
    name: "rename_favorite",
    title: "Rename favorite",
    description: "Renames a favorite. Output: {name, newName, renamed: true}. Fails when newName is already used.",
    input: {
      name: FavoriteName,
      newName: z.string().min(1).max(255).describe("New name"),
    },
    annotations: MODIFIES,
    handler: async ({ name, newName }, { ac }) => ac.addon("RenameFavorite", { name, newName }),
  });

  defineTool(server, ctx, {
    name: "export_favorites",
    title: "Export favorites",
    description:
      "Exports favorites to a preferences file (.prf) that other projects can import with import_favorites (or Archicad's Favorites " +
      "palette). Output: {path, exported, fileExists}.",
    input: {
      path: z.string().min(2).describe("Absolute target path, e.g. '/Users/me/Desktop/favorites.prf' ('~/' allowed; '.prf' appended when there is no extension)"),
      names: z.array(FavoriteName).min(1).max(5000).optional().describe("Favorites to export (default: all)"),
      overwrite: z.boolean().optional().describe("Replace an existing file (default false)"),
      createFolders: z.boolean().optional().describe("Create missing parent folders (default false)"),
    },
    annotations: WRITES_FILE,
    handler: async ({ path, names, overwrite, createFolders }, { ac }) =>
      ac.addon("ExportFavorites", assignDefined({ path: resolveLocalPath(path, "path") }, { names, overwrite, createFolders })),
  });

  defineTool(server, ctx, {
    name: "import_favorites",
    title: "Import favorites",
    description:
      "Imports favorites from a .prf file exported by Archicad / export_favorites. Output: {imported: [new names], count, firstConflict?}. " +
      "In Teamwork, reserve the 'Favorites' object set first.",
    input: {
      path: z.string().min(2).describe("Absolute path of the .prf file ('~/' allowed)"),
      folder: FavoriteFolder.optional().describe("Target folder in the Favorites palette (default: root)"),
      importFolders: z.boolean().optional().describe("Keep the folder structure stored in the file (default true)"),
      conflictPolicy: z
        .enum(["Append", "Overwrite", "Skip", "Error"])
        .optional()
        .describe("When a name already exists: Append (default: import under a new name), Overwrite, Skip, or Error (stop)"),
    },
    annotations: CREATES,
    handler: async ({ path, folder, importFolders, conflictPolicy }, { ac }) =>
      ac.addon("ImportFavorites", assignDefined({ path: resolveLocalPath(path, "path") }, { folder, importFolders, conflictPolicy })),
  });
}

// =============================================================================
// Tool defaults
// =============================================================================

const DefaultsFields = z
  .record(z.unknown())
  .refine((r) => Object.keys(r).length > 0, "give at least one field to change")
  .describe(
    "Settings to change, with EXACTLY the field names and units of the type's create_* tool (create_walls, create_columns, " +
      "create_beams, create_slabs, create_roofs, create_doors, create_windows, create_objects, create_zones, create_lines, create_texts...; " +
      "meters, degrees). Geometry/identity fields (begin, end, polygon, position, points, storyIndex, guid) are ignored — defaults do not " +
      "store them. Also accepted for every type: layer (name/index), renovationStatus (Existing|New|Demolished|Default), drawIndex, " +
      "elementId (ID of the next placed element); library-part based tools: libraryPart (or 'stamp' for zones) + params / gdlParams / " +
      "stampParameters {name: value}, objects/lamps also sizeA, sizeB, height; classifications: [classification item GUID]; " +
      "categories: {StructuralFunction|Position|RenovationStatus|RenovationFilter: localized value name or GUID}; properties: " +
      "{property GUID | name | 'Group/Name': value} (custom properties). Examples — Wall: {height: 3, thickness: 0.25, composite: " +
      "'...', referenceLine: 'Center', layer: '...'}; Slab: {thickness: 0.2, buildingMaterial: '...'}; Door: {libraryPart: '...', " +
      "width: 0.9, height: 2.1}; Object: {libraryPart: '...', params: {...}}",
  );

function registerToolDefaultTools(server: McpServer, ctx: ToolContext): void {
  defineTool(server, ctx, {
    name: "get_tool_defaults",
    title: "Get tool default settings",
    description:
      "Returns the default settings of element tools — what the next element placed in Archicad (or created by a create_* tool without " +
      "explicit values) gets. Without type/types: lists the toolbox tools {tools: [{type, variation?}], activeTool, hint}. With types: " +
      "{defaults: [{type, variation?, settings: {layer, renovationStatus, drawIndex, elementId?, ...the same fields as the type's create_* " +
      "tool (heights, thicknesses, structure/composite/buildingMaterial, surfaces, libraryPart, params {GDL name: value}...), " +
      "gdlParameterCount?}, classifications?: [{system, systemGuid, itemGuid, itemId, itemName}], categories?: {StructuralFunction: " +
      "{value, valueGuid, category}, ...}, properties?: [{guid, name, group, value | status}], notes?} | {error}]}. " +
      "Use it before set_tool_defaults to see the current values and field names.",
    input: {
      type: ToolElementType.optional().describe("Tool to read (e.g. 'Wall')"),
      variation: Variation.optional(),
      types: z
        .array(z.union([ToolElementType, ToolTypeRef]))
        .min(1)
        .max(50)
        .optional()
        .describe("Several tools at once: type names or {type, variation}"),
      includeGdlParameters: z.boolean().optional().describe("Library-part based tools: include the GDL parameter values (default true)"),
      gdlParameterNames: z.array(z.string().min(1)).min(1).max(500).optional().describe("Only these GDL parameters (names as in get_gdl_parameters)"),
      includeHiddenGdlParameters: z.boolean().optional().describe("Also include hidden GDL parameters (default false)"),
      includeClassifications: z.boolean().optional().describe("Include the default classifications (default true)"),
      includeCategories: z.boolean().optional().describe("Include the default element categories (default true)"),
      includeProperties: z.boolean().optional().describe("Include custom properties whose default value was overridden (default false)"),
      includeAllProperties: z.boolean().optional().describe("Include ALL custom properties available for the tool, with their values (default false)"),
    },
    annotations: READ_ONLY,
    handler: async ({ types, ...rest }, { ac }) => {
      if (rest.variation !== undefined && rest.type === undefined) fail(["variation needs type (or use types: [{type, variation}])"]);
      // the add-on reads either all strings or all objects: send a uniform object list
      const typeList = types?.map((t) => (typeof t === "string" ? { type: t } : assignDefined({ type: t.type }, { variation: t.variation })));
      return ac.addon("GetToolDefaults", assignDefined({}, { ...rest, types: typeList }));
    },
  });

  defineTool(server, ctx, {
    name: "set_tool_defaults",
    title: "Set tool default settings",
    description:
      "Changes the default settings of element tools (like the tool's Default Settings dialog), e.g. set the Wall tool to 3 m high " +
      "0.25 m composite walls on a given layer before drawing many walls, or prepare settings the user will draw with. Every field of the " +
      "type's create_* tool can be set. Fields are applied independently: an invalid field is reported in 'rejected' without blocking " +
      "the others. Output: {results: [{type, variation?, applied: [field], ignored?: [geometry fields], rejected?: [{field, error}], " +
      "notes?, settings? (the resulting defaults)} | {error}]}. Save them as a favorite with create_favorite {toolDefaults: {type}}.",
    input: {
      defaults: z
        .array(
          z.object({
            type: ToolElementType,
            variation: Variation.optional(),
            fields: DefaultsFields,
          }),
        )
        .min(1)
        .max(50)
        .describe("One entry per tool"),
      returnSettings: z.boolean().optional().describe("Return the resulting default settings of each tool (default true)"),
    },
    annotations: MODIFIES,
    handler: async ({ defaults, returnSettings }, { ac }) =>
      ac.addon(
        "SetToolDefaults",
        assignDefined({ defaults: defaults.map((d) => assignDefined({ type: d.type }, { variation: d.variation, fields: d.fields })) }, { returnSettings }),
      ),
  });
}

// =============================================================================
// Revisions / Change Manager (read-only)
// =============================================================================

function registerRevisionTools(server: McpServer, ctx: ToolContext): void {
  defineTool(server, ctx, {
    name: "get_revisions",
    title: "Get revisions",
    description:
      "Read-only document revision data (Document > Issue Manager for revisions: revision issues and the layout revisions they " +
      "contain). Output: {issues?: [{guid, id, description, issued, issueTime, issuedBy, overrideRevisionId, createNewRevision, " +
      "visibleMarkersInIssues, customFields: {name: value}, documentRevisionCount}], documentRevisions?: [{guid, id, finalId, status: " +
      "Actual|Issued, owner?, issue?: {guid, id}, layout: {id, name, databaseGuid, masterLayout, width, height, drawingScales, " +
      "subsetId, subsetName, teamworkOwner?, customFields}}]}. Revision issues are NOT Issue Manager markup issues (get_issues).",
    input: {
      include: z
        .array(z.enum(["issues", "documentRevisions"]))
        .min(1)
        .max(2)
        .optional()
        .describe("What to return (default both)"),
      issue: z.string().min(1).optional().describe("Only the document revisions of this revision issue (its GUID or ID, from issues[])"),
    },
    annotations: READ_ONLY,
    handler: async ({ include, issue }, { ac }) => ac.addon("GetRevisions", assignDefined({}, { include, issue })),
  });

  defineTool(server, ctx, {
    name: "get_revision_changes",
    title: "Get revision changes",
    description:
      "Read-only Change Manager data (changes tracked for revisions, shown by change markers). Give ONE mode (default: every change " +
      "of the project): documentRevision (changes in one layout revision, GUID from get_revisions), layouts / allLayouts (current " +
      "revision changes of layouts), elements (changes an element belongs to) or changeIds. Output: {changes: [{id, description, " +
      "lastModified, modifiedBy, issued, archived, customFields, firstIssue?}], count} | {documentRevision, changes} | {layouts: [{layout: " +
      "{databaseGuid, id, name}, changes | error}]} | {elements: [{guid, changeIds, changes} | {error}]}.",
    input: {
      documentRevision: Guid.optional().describe("Document revision GUID (documentRevisions[].guid of get_revisions)"),
      layouts: z
        .array(ChangeRef)
        .min(1)
        .max(1000)
        .optional()
        .describe("Layouts: layout database GUID (get_revisions layout.databaseGuid), layout ID, name, or 'ID name'"),
      allLayouts: z.boolean().optional().describe("Current revision changes of every layout"),
      elements: ElementRefs.max(5000).optional().describe("Elements whose changes to return"),
      changeIds: z.array(z.string().min(1)).min(1).max(5000).optional().describe("Change IDs to look up"),
      includeFirstIssue: z.boolean().optional().describe("Also return the first revision issue each change was issued in (default false)"),
    },
    annotations: READ_ONLY,
    handler: async ({ documentRevision, layouts, allLayouts, elements, changeIds, includeFirstIssue }, { ac }) => {
      const modes = [
        documentRevision !== undefined && "documentRevision",
        layouts !== undefined && "layouts",
        allLayouts === true && "allLayouts",
        elements !== undefined && "elements",
        changeIds !== undefined && "changeIds",
      ].filter(Boolean);
      if (modes.length > 1) fail([`give only one of documentRevision, layouts, allLayouts, elements, changeIds (got ${modes.join(", ")})`]);
      return ac.addon(
        "GetRevisionChanges",
        assignDefined(
          {},
          {
            documentRevision,
            layouts: layouts?.map(refString),
            allLayouts: allLayouts === true ? true : undefined,
            elements: elements?.map(guidOf),
            changeIds,
            includeFirstIssue,
          },
        ),
      );
    },
  });
}
