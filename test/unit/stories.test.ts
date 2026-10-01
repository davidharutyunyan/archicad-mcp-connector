import { describe, expect, it } from "vitest";

import { addonOk, harness } from "../helpers.js";

const STORY_0 = { index: 0, name: "1-й этаж", level: 0, height: 3, floorId: 1, showOnSections: true, isCurrent: true };
const STORY_1 = { index: 1, name: "2-й этаж", level: 3, height: 3, floorId: 2, showOnSections: true, isCurrent: false };

describe("get_stories", () => {
  it("calls GetStories without options by default", async () => {
    const response = { stories: [STORY_0, STORY_1], firstIndex: 0, lastIndex: 1, currentIndex: 0, count: 2 };
    const h = await harness(() => addonOk(response));
    const res = await h.call("get_stories");
    expect(res.isError).toBe(false);
    expect(h.requests).toHaveLength(1);
    expect(h.requests[0]?.addOnCommand).toBe("GetStories");
    expect(h.requests[0]?.addOnParameters).toEqual({});
    expect(res.json).toEqual(response);
  });

  it("forwards includeElementCounts and atLevels", async () => {
    const h = await harness(() => addonOk({ stories: [] }));
    await h.call("get_stories", { includeElementCounts: true, atLevels: [1.5, 4.2] });
    expect(h.requests[0]?.addOnParameters).toEqual({ includeElementCounts: true, atLevels: [1.5, 4.2] });
  });

  it("omits an empty atLevels array", async () => {
    const h = await harness(() => addonOk({ stories: [] }));
    await h.call("get_stories", { atLevels: [] });
    expect(h.requests[0]?.addOnParameters).toEqual({});
  });

  it("surfaces add-on errors", async () => {
    const h = await harness(() => addonOk({ error: { code: -2130313112, message: "Cannot read the story settings: APIERR_NOPLAN" } }));
    const res = await h.call("get_stories");
    expect(res.isError).toBe(true);
    expect(res.text).toContain("Cannot read the story settings");
  });
});

describe("create_stories", () => {
  it("sends story specs to CreateStories and normalizes relativeTo", async () => {
    const h = await harness(() => addonOk({ results: [{ story: STORY_1 }], stories: [STORY_0, STORY_1] }));
    const res = await h.call("create_stories", {
      stories: [
        { name: "Кровля", height: 2.5, showOnSections: false },
        { name: "Техэтаж", position: "Above", relativeTo: 1, level: 7.2 },
        { name: "Подвал", position: "Below", relativeTo: "1-й этаж" },
        { position: "Bottom", level: -3 },
        { position: "Above", relativeTo: { floorId: 7 } },
      ],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("CreateStories");
    expect(h.requests[0]?.addOnParameters).toEqual({
      stories: [
        { name: "Кровля", height: 2.5, showOnSections: false },
        { name: "Техэтаж", position: "Above", relativeTo: { index: 1 }, level: 7.2 },
        { name: "Подвал", position: "Below", relativeTo: { name: "1-й этаж" } },
        { position: "Bottom", level: -3 },
        { position: "Above", relativeTo: { floorId: 7 } },
      ],
    });
    expect(res.json).toEqual({ results: [{ story: STORY_1 }], stories: [STORY_0, STORY_1] });
  });

  it("rejects Above/Below without relativeTo before calling Archicad", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("create_stories", { stories: [{ name: "X", position: "Above" }] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("needs relativeTo");
    expect(h.requests).toHaveLength(0);
  });

  it("rejects relativeTo with position Top", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("create_stories", { stories: [{ name: "X", relativeTo: 0 }] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("relativeTo is only used");
    expect(h.requests).toHaveLength(0);
  });

  it("rejects non-positive heights, unknown positions and empty batches", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("create_stories", { stories: [{ height: 0 }] })).isError).toBe(true);
    expect((await h.call("create_stories", { stories: [{ height: -3 }] })).isError).toBe(true);
    expect((await h.call("create_stories", { stories: [{ position: "Middle" }] })).isError).toBe(true);
    expect((await h.call("create_stories", { stories: [] })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("returns per-item errors from the add-on unchanged", async () => {
    const response = {
      results: [{ story: STORY_1 }, { error: { code: -2130313112, message: "level 1.000 m must be above the level of the current top story" } }],
      stories: [STORY_0, STORY_1],
    };
    const h = await harness(() => addonOk(response));
    const res = await h.call("create_stories", { stories: [{ name: "A" }, { name: "B", level: 1 }] });
    expect(res.isError).toBe(false);
    expect(res.json).toEqual(response);
  });
});

describe("modify_stories", () => {
  it("sends patches to ModifyStories with normalized story references", async () => {
    const h = await harness(() => addonOk({ results: [{ story: STORY_1 }], stories: [STORY_0, STORY_1] }));
    const res = await h.call("modify_stories", {
      stories: [
        { story: 1, name: "Второй этаж", height: 3.3 },
        { story: "1-й этаж", showOnSections: false },
        { story: { floorId: 2 }, level: 3.2 },
      ],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("ModifyStories");
    expect(h.requests[0]?.addOnParameters).toEqual({
      stories: [
        { story: { index: 1 }, name: "Второй этаж", height: 3.3 },
        { story: { name: "1-й этаж" }, showOnSections: false },
        { story: { floorId: 2 }, level: 3.2 },
      ],
    });
  });

  it("rejects patches without any change", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("modify_stories", { stories: [{ story: 1 }] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("nothing to change");
    expect(h.requests).toHaveLength(0);
  });

  it("rejects a missing story reference", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("modify_stories", { stories: [{ name: "X" }] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("surfaces add-on errors", async () => {
    const h = await harness(() =>
      addonOk({ error: { code: -2130313112, message: "'stories' must contain at least one patch" } }),
    );
    const res = await h.call("modify_stories", { stories: [{ story: 0, name: "A" }] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("at least one patch");
  });
});

describe("delete_stories", () => {
  it("normalizes references to objects and forwards dryRun", async () => {
    const response = { dryRun: true, wouldDelete: [{ ...STORY_1, elementCount: 4, elementsByType: { Wall: 4 } }], remainingCount: 1 };
    const h = await harness(() => addonOk(response));
    const res = await h.call("delete_stories", { stories: [1, "Кровля", { floorId: 9 }, { index: 2 }], dryRun: true });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("DeleteStories");
    expect(h.requests[0]?.addOnParameters).toEqual({
      stories: [{ index: 1 }, { name: "Кровля" }, { floorId: 9 }, { index: 2 }],
      dryRun: true,
    });
    expect(res.json).toEqual(response);
  });

  it("sends no dryRun by default", async () => {
    const h = await harness(() => addonOk({ results: [{ deleted: STORY_1 }], stories: [STORY_0] }));
    await h.call("delete_stories", { stories: [1] });
    expect(h.requests[0]?.addOnParameters).toEqual({ stories: [{ index: 1 }] });
  });

  it("rejects an empty list", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("delete_stories", { stories: [] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("surfaces the 'at least one story must remain' error", async () => {
    const h = await harness(() =>
      addonOk({ error: { code: -2130313112, message: "Cannot delete every story: a project needs at least one story." } }),
    );
    const res = await h.call("delete_stories", { stories: [0, 1] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("at least one story");
  });

  it("is annotated as destructive", async () => {
    const h = await harness(() => addonOk({}));
    const tool = (await h.listTools()).find((t) => t.name === "delete_stories");
    expect(tool?.annotations?.destructiveHint).toBe(true);
    expect(tool?.annotations?.readOnlyHint).toBe(false);
  });
});

describe("set_current_story", () => {
  it("sends the story reference and openFloorPlan", async () => {
    const h = await harness(() => addonOk({ currentStory: { ...STORY_1, isCurrent: true }, windowChanged: true }));
    const res = await h.call("set_current_story", { story: "2-й этаж", openFloorPlan: false });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("SetCurrentStory");
    expect(h.requests[0]?.addOnParameters).toEqual({ story: { name: "2-й этаж" }, openFloorPlan: false });
  });

  it("accepts an index, a floorId and a displayNumber", async () => {
    const h = await harness(() => addonOk({ currentStory: STORY_0, windowChanged: false }));
    await h.call("set_current_story", { story: -1 });
    await h.call("set_current_story", { story: { floorId: 5 } });
    await h.call("set_current_story", { story: { displayNumber: 3 } });
    expect(h.requests[0]?.addOnParameters).toEqual({ story: { index: -1 } });
    expect(h.requests[1]?.addOnParameters).toEqual({ story: { floorId: 5 } });
    expect(h.requests[2]?.addOnParameters).toEqual({ story: { displayNumber: 3 } });
  });

  it("rejects unknown reference objects", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("set_current_story", { story: { guid: "x" } });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("rejects a missing story", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("set_current_story", {});
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("surfaces unknown story errors", async () => {
    const h = await harness(() =>
      addonOk({ error: { code: -2130313111, message: "Story named 'Nope' not found. Existing stories (index: 'name' @ level): 0: '1-й этаж' @ 0.000 m." } }),
    );
    const res = await h.call("set_current_story", { story: "Nope" });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("Existing stories");
  });
});

describe("story tool registration", () => {
  it("registers all story tools with descriptions", async () => {
    const h = await harness(() => addonOk({}));
    const tools = await h.listTools();
    for (const name of ["get_stories", "create_stories", "modify_stories", "delete_stories", "set_current_story"]) {
      const tool = tools.find((t) => t.name === name);
      expect(tool, name).toBeDefined();
      expect(tool?.description?.length ?? 0).toBeGreaterThan(80);
    }
    expect(tools.find((t) => t.name === "get_stories")?.annotations?.readOnlyHint).toBe(true);
  });
});
