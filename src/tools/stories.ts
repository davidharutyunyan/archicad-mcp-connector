/**
 * Story tools: list, insert, modify (rename / level / height / show on sections), delete, go to story.
 * Backed by the add-on commands GetStories / CreateStories / ModifyStories / DeleteStories / SetCurrentStory
 * (addon/Src/Commands/Stories.cpp); field names match 1:1.
 *
 * Archicad's story API is not undoable (story settings are "non-undoable modifiers", deleting a story is a
 * "complete operation"), so the add-on applies changes immediately and reverts a failed item itself.
 */

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import { CREATES, defineTool, DESTRUCTIVE, MODIFIES, READ_ONLY, type ToolContext } from "./define.js";
import { StoryRef } from "./schemas.js";

/** Story index, name, {index}, {name}, {floorId} (stable across index shifts) or {displayNumber}. */
export const StorySelector = z
  .union([...StoryRef.options])
  .describe(
    "Story: index (integer from get_stories — NOT the displayed story number; negative = below the index-0 story), exact story name " +
      "(localized, e.g. Russian — see get_stories), {index}, {name}, {floorId} (stable id that does not change when stories are " +
      "inserted/deleted), or {displayNumber} (the number shown in the Navigator)",
  );

type StorySelectorValue = z.infer<typeof StorySelector>;

/** Normalizes a story selector to the object form understood by the add-on everywhere. */
export function storyRefObject(ref: StorySelectorValue): Record<string, unknown> {
  if (typeof ref === "number") return { index: ref };
  if (typeof ref === "string") return { name: ref };
  return ref;
}

const StoryName = z.string().max(255).describe("Story name as shown in the Story Settings dialog / Navigator");

const ShowOnSections = z
  .boolean()
  .describe("Show the story level line on sections/elevations ('Show on Sections' column of Story Settings)");

const Meters = z.number().finite();

export const StoryCreateSpec = z.object({
  name: StoryName.optional().describe("Name of the new story (default: empty, Archicad then shows only the number)"),
  height: Meters.positive()
    .max(1000)
    .optional()
    .describe(
      "Height of the NEW story (distance to the story above it), in m. Default: the height of the reference story " +
        "(the top story for 'Top', the bottom story for 'Bottom', relativeTo for 'Above'/'Below'), or 3 m. " +
        "Giving height makes room for the new story: the stories above move up (below the ground floor: the stories below move down).",
    ),
  level: Meters.optional().describe(
    "Elevation of the new story in m above Project Zero. 'Top': must be above the current top story (default: top level + top " +
      "height; the old top story's height becomes level - its level). 'Above'/'Below' with level but WITHOUT height: the new " +
      "story fills the gap between its neighbours and nothing moves (level must lie between them). 'Bottom': the new story ends " +
      "at the old lowest story, so give level OR height.",
  ),
  position: z
    .enum(["Top", "Bottom", "Above", "Below"])
    .optional()
    .describe(
      "Where to insert: 'Top' (default) = above the highest story, 'Bottom' = below the lowest story, " +
        "'Above'/'Below' = directly above/below the story given in relativeTo",
    ),
  relativeTo: StorySelector.optional().describe("Reference story for position 'Above'/'Below' (required for those, not allowed with 'Top'/'Bottom')"),
  showOnSections: ShowOnSections.optional().describe(
    "Show the story level line on sections/elevations ('Show on Sections' column of Story Settings; default true)",
  ),
});

export const StoryPatch = z.object({
  story: StorySelector.describe("Story to change (index, current name, or {floorId})"),
  name: StoryName.optional().describe("New name"),
  level: Meters.optional().describe(
    "New elevation in m above Project Zero. By default ONLY this story moves: the story below and this story change height, " +
      "the stories above stay (so it must stay between its neighbours). With moveStoriesAbove: true the stories above move by " +
      "the same amount (this story keeps its height). Elements move with their home story.",
  ),
  moveStoriesAbove: z
    .boolean()
    .optional()
    .describe("Only with level: true = move all stories above together with this story (default false = only this story moves)"),
  height: Meters.positive()
    .max(1000)
    .optional()
    .describe(
      "New height to the next story, in m (like 'Height to next' in Story Settings): the stories above move up/down. " +
        "On the top story it only changes its height value.",
    ),
  showOnSections: ShowOnSections.optional(),
});

function checkCreateSpecs(stories: z.infer<typeof StoryCreateSpec>[]): void {
  const problems: string[] = [];
  stories.forEach((s, i) => {
    const pos = s.position ?? "Top";
    if ((pos === "Above" || pos === "Below") && s.relativeTo === undefined) {
      problems.push(`stories[${i}]: position '${pos}' needs relativeTo (the story to insert next to)`);
    }
    if ((pos === "Top" || pos === "Bottom") && s.relativeTo !== undefined) {
      problems.push(`stories[${i}]: relativeTo is only used with position 'Above' or 'Below' (got position '${pos}')`);
    }
  });
  if (problems.length > 0) throw new Error(`Invalid input: ${problems.join("; ")}`);
}

function checkPatches(stories: z.infer<typeof StoryPatch>[]): void {
  const problems: string[] = [];
  stories.forEach((s, i) => {
    if (s.name === undefined && s.level === undefined && s.height === undefined && s.showOnSections === undefined) {
      problems.push(`stories[${i}]: nothing to change — give at least one of name, level, height, showOnSections`);
    }
    if (s.moveStoriesAbove === true && s.level === undefined) {
      problems.push(`stories[${i}]: moveStoriesAbove only applies together with level`);
    }
  });
  if (problems.length > 0) throw new Error(`Invalid input: ${problems.join("; ")}`);
}

export function registerStoryTools(server: McpServer, ctx: ToolContext): void {
  defineTool(server, ctx, {
    name: "get_stories",
    title: "Get stories",
    description:
      "Lists all stories (floors) of the project, bottom to top: {stories: [{index, displayNumber, name, level (m above Project Zero), " +
      "height (m to the next story; may be missing for the top story), floorId (stable id), showOnSections, isCurrent, " +
      "reservedByOtherUser (Teamwork, only when true)}], firstIndex, lastIndex, currentIndex, count, skipNullFloor, ghostStory}. " +
      "Call this first for any story-related work: element tools place elements by storyIndex (elevations are relative to the " +
      "home story), and story names are localized. displayNumber is the number shown in the Navigator: when skipNullFloor is true " +
      "(e.g. the Russian template) index 0 is shown as '1.', so 'the 3rd floor' is displayNumber 3 = index 2 — always pass the index " +
      "(or name) to other tools. Indexes change when stories are inserted/deleted; floorId does not. " +
      "Use atLevels to convert absolute elevations to {storyIndex, offsetFromStory}, and includeElementCounts to see what is on each story.",
    input: {
      includeElementCounts: z
        .boolean()
        .optional()
        .describe("Also return elementCount and elementsByType ({Wall: 12, ...}) per story, counted by home story (slower on big projects)"),
      atLevels: z
        .array(Meters)
        .max(500)
        .optional()
        .describe("Absolute elevations (m above Project Zero) to look up: returns levelLookup [{level, storyIndex, storyName, storyLevel, offsetFromStory}]"),
    },
    annotations: READ_ONLY,
    handler: async ({ includeElementCounts, atLevels }, { ac }) => {
      const params: Record<string, unknown> = {};
      if (includeElementCounts !== undefined) params["includeElementCounts"] = includeElementCounts;
      if (atLevels !== undefined && atLevels.length > 0) params["atLevels"] = atLevels;
      return ac.addon("GetStories", params);
    },
  });

  defineTool(server, ctx, {
    name: "create_stories",
    title: "Create stories",
    description:
      "Inserts new stories (Story Settings > Insert Above/Below). Items are applied in order, each one seeing the stories created " +
      "by the previous items, so several 'Top' items stack upwards. Default: a new top story with the height of the current top story. " +
      "height is the height of the NEW story; level only (Above/Below) fills the gap between two stories without moving anything. " +
      "Inserting shifts the indexes of the stories above (floorId stays stable) — re-read indexes from the returned 'stories' before " +
      "using them. Returns {results: [{story, movedStories?: [{floorId, index, name, levelBefore, level}]} | {error}] in input order, " +
      "stories: [all stories after the change]}. Story changes are applied immediately (not via Archicad's undo); a failed item is " +
      "reverted by the add-on. Example: {stories: [{name: 'Этаж 4', height: 3}, {name: 'Кровля', height: 2.5}]}.",
    input: {
      stories: z.array(StoryCreateSpec).min(1).max(200).describe("Stories to insert, applied in order"),
    },
    annotations: CREATES,
    handler: async ({ stories }, { ac }) => {
      checkCreateSpecs(stories);
      const items = stories.map((s) => (s.relativeTo === undefined ? s : { ...s, relativeTo: storyRefObject(s.relativeTo) }));
      return ac.addon("CreateStories", { stories: items });
    },
  });

  defineTool(server, ctx, {
    name: "modify_stories",
    title: "Modify stories",
    description:
      "Changes existing stories, patches applied in order: rename, change level (elevation above Project Zero; only this story " +
      "moves unless moveStoriesAbove), change height to the next story (the stories above move), toggle 'show on sections'. " +
      "Only the given fields change. Elements keep their position relative to their home story, so changing levels/heights moves " +
      "them vertically (and changes the height of walls/columns linked to a moved story). Returns {results: [{story, movedStories?} | " +
      "{error}], stories: [all stories after the change]}; a failed patch is reverted.",
    input: {
      stories: z.array(StoryPatch).min(1).max(200).describe("Patches: {story, ...fields to change}"),
    },
    annotations: MODIFIES,
    handler: async ({ stories }, { ac }) => {
      checkPatches(stories);
      const items = stories.map((s) => ({ ...s, story: storyRefObject(s.story) }));
      return ac.addon("ModifyStories", { stories: items });
    },
  });

  defineTool(server, ctx, {
    name: "delete_stories",
    title: "Delete stories",
    description:
      "Deletes stories AND EVERY ELEMENT whose home story they are. Use ONLY when the user explicitly asks to delete stories. " +
      "Run with dryRun: true first to see how many elements each story holds. All references are resolved before anything is " +
      "deleted (so [2, 3] means the current stories 2 and 3); at least one story must remain. Indexes of the remaining stories shift. " +
      "By default Archicad closes the gap (the stories above a deleted story move down); keepLevels: true keeps every remaining " +
      "story at its elevation instead (the story below a deleted one gets taller). Returns {results: [{deleted: story+elementCount} | " +
      "{error}], movedStories?, stories: [remaining], warning?} or, with dryRun, {dryRun, wouldDelete, remainingCount}.",
    input: {
      stories: z.array(StorySelector).min(1).max(200).describe("Stories to delete"),
      dryRun: z.boolean().optional().describe("true = change nothing, only report the stories and element counts that would be deleted"),
      keepLevels: z
        .boolean()
        .optional()
        .describe("true = the remaining stories keep their elevations (default false: Archicad moves the stories above down to close the gap)"),
    },
    annotations: DESTRUCTIVE,
    handler: async ({ stories, dryRun, keepLevels }, { ac }) => {
      const params: Record<string, unknown> = { stories: stories.map(storyRefObject) };
      if (dryRun !== undefined) params["dryRun"] = dryRun;
      if (keepLevels !== undefined) params["keepLevels"] = keepLevels;
      return ac.addon("DeleteStories", params);
    },
  });

  defineTool(server, ctx, {
    name: "set_current_story",
    title: "Set current story",
    description:
      "Makes a story the current one and (by default) shows its floor plan, like double-clicking the story in the Navigator. " +
      "New elements are placed on the current story when no storyIndex is given, and floor plan screenshots/zooms show it. " +
      "Returns {currentStory, windowChanged}.",
    input: {
      story: StorySelector.describe("Story to go to (index, name, or {floorId})"),
      openFloorPlan: z
        .boolean()
        .optional()
        .describe("Switch to the floor plan window first (default true). false = only change the current story, keep the active window"),
    },
    annotations: { readOnlyHint: false, destructiveHint: false, idempotentHint: true },
    handler: async ({ story, openFloorPlan }, { ac }) => {
      const params: Record<string, unknown> = { story: storyRefObject(story) };
      if (openFloorPlan !== undefined) params["openFloorPlan"] = openFloorPlan;
      return ac.addon("SetCurrentStory", params);
    },
  });
}
