import { homedir } from "node:os";
import { join } from "node:path";

import { describe, expect, it } from "vitest";

import { addonOk, harness } from "../helpers.js";

const G1 = "92F5E7F6-2460-FF4F-A499-0B07286D2B5F";
const G2 = "0B1C2D3E-4F50-6172-8394-A5B6C7D8E9F0";
const G3 = "11111111-2222-3333-4444-555555555555";

const SOLO = {
  isTeamwork: false,
  message: "This is a solo project, not a Teamwork (BIMcloud / BIMserver) project: ...",
  project: { name: "Дом", path: "/Users/me/Дом.pln", untitled: false },
};

const ERR = (message: string) => addonOk({ error: { code: -2130313112, message } });

// =============================================================================
// Registration
// =============================================================================

describe("collaboration tool registration", () => {
  it("registers every collaboration tool with a description", async () => {
    const h = await harness(() => addonOk({}));
    const tools = await h.listTools();
    const names = new Set(tools.map((t) => t.name));
    for (const name of [
      "get_teamwork_status", "teamwork_send", "teamwork_receive", "reserve_elements", "release_elements",
      "get_issues", "create_issue", "delete_issue", "add_issue_comment", "get_issue_comments", "attach_elements_to_issue",
      "detach_elements_from_issue", "get_issue_elements", "export_bcf", "import_bcf",
      "get_favorites", "apply_favorite", "create_favorite", "delete_favorite", "rename_favorite", "export_favorites", "import_favorites",
      "get_tool_defaults", "set_tool_defaults", "get_revisions", "get_revision_changes",
    ]) {
      expect(names.has(name), name).toBe(true);
      const tool = tools.find((t) => t.name === name)!;
      expect((tool.description ?? "").length, name).toBeGreaterThan(60);
    }
  });

  it("marks read-only tools as read-only", async () => {
    const h = await harness(() => addonOk({}));
    const tools = await h.listTools();
    for (const name of ["get_teamwork_status", "get_issues", "get_issue_comments", "get_issue_elements", "get_favorites", "get_tool_defaults", "get_revisions", "get_revision_changes"]) {
      expect(tools.find((t) => t.name === name)?.annotations?.readOnlyHint, name).toBe(true);
    }
    for (const name of ["delete_issue", "delete_favorite", "set_tool_defaults", "teamwork_send"]) {
      expect(tools.find((t) => t.name === name)?.annotations?.readOnlyHint, name).toBe(false);
    }
  });
});

// =============================================================================
// Teamwork
// =============================================================================

describe("get_teamwork_status", () => {
  it("calls GetTeamworkStatus with no options and returns the solo result", async () => {
    const h = await harness(() => addonOk(SOLO));
    const res = await h.call("get_teamwork_status");
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("GetTeamworkStatus");
    expect(h.requests[0]?.addOnParameters).toEqual({});
    expect(res.json).toEqual(SOLO);
  });

  it("normalizes element refs and forwards objectSets / flags", async () => {
    const h = await harness(() => addonOk({ isTeamwork: true }));
    await h.call("get_teamwork_status", {
      elements: [G1, { guid: G2 }],
      objectSets: ["Composites", "Favorites"],
      includeMembers: false,
      includeAccessRights: true,
    });
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [G1, G2],
      objectSets: ["Composites", "Favorites"],
      includeMembers: false,
      includeAccessRights: true,
    });
  });

  it("accepts objectSets: true and rejects unknown object sets", async () => {
    const h = await harness(() => addonOk({ isTeamwork: true }));
    await h.call("get_teamwork_status", { objectSets: true });
    expect(h.requests[0]?.addOnParameters).toEqual({ objectSets: true });
    const bad = await h.call("get_teamwork_status", { objectSets: ["Walls"] });
    expect(bad.isError).toBe(true);
    expect(h.requests).toHaveLength(1);
  });

  it("rejects malformed GUIDs before calling Archicad", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("get_teamwork_status", { elements: ["not-a-guid"] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("teamwork_send / teamwork_receive", () => {
  it("sends the comment with TeamworkSend", async () => {
    const h = await harness(() => addonOk({ isTeamwork: true, sent: true }));
    const res = await h.call("teamwork_send", { comment: "Стены 1 этажа" });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("TeamworkSend");
    expect(h.requests[0]?.addOnParameters).toEqual({ comment: "Стены 1 этажа" });
    expect(res.json).toEqual({ isTeamwork: true, sent: true });
  });

  it("sends an empty object without a comment", async () => {
    const h = await harness(() => addonOk({ ...SOLO, sent: false }));
    const res = await h.call("teamwork_send");
    expect(h.requests[0]?.addOnParameters).toEqual({});
    expect(res.json).toMatchObject({ isTeamwork: false, sent: false });
  });

  it("calls TeamworkReceive and surfaces offline errors", async () => {
    const h = await harness(() => ERR("The BIMcloud/BIMserver is not reachable (Teamwork offline)."));
    const res = await h.call("teamwork_receive");
    expect(h.requests[0]?.addOnCommand).toBe("TeamworkReceive");
    expect(h.requests[0]?.addOnParameters).toEqual({});
    expect(res.isError).toBe(true);
    expect(res.text).toContain("not reachable");
  });
});

describe("reserve_elements / release_elements", () => {
  it("reserves elements, object sets and hotlink cache management", async () => {
    const h = await harness(() => addonOk({ isTeamwork: true, elements: [{ guid: G1, status: "ReservedByMe", reserved: true }] }));
    const res = await h.call("reserve_elements", {
      elements: [{ guid: G1 }],
      objectSets: ["LayerSettings"],
      hotlinkCacheManagement: true,
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("ReserveElements");
    expect(h.requests[0]?.addOnParameters).toEqual({ elements: [G1], objectSets: ["LayerSettings"], hotlinkCacheManagement: true });
  });

  it("releases with enableDialogs", async () => {
    const h = await harness(() => addonOk({ isTeamwork: true }));
    await h.call("release_elements", { elements: [G1, G2], enableDialogs: true });
    expect(h.requests[0]?.addOnCommand).toBe("ReleaseElements");
    expect(h.requests[0]?.addOnParameters).toEqual({ elements: [G1, G2], enableDialogs: true });
  });

  it("returns the solo-project answer unchanged (not an error)", async () => {
    const h = await harness(() => addonOk({ ...SOLO, reserved: false }));
    const res = await h.call("reserve_elements", { elements: [G1] });
    expect(res.isError).toBe(false);
    expect(res.json).toMatchObject({ isTeamwork: false, reserved: false });
  });

  it("rejects an empty request before calling Archicad", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("reserve_elements", {});
    expect(res.isError).toBe(true);
    expect(res.text).toContain("nothing to reserve");
    const res2 = await h.call("release_elements", { hotlinkCacheManagement: false });
    expect(res2.isError).toBe(true);
    expect(res2.text).toContain("nothing to release");
    expect(h.requests).toHaveLength(0);
  });
});

// =============================================================================
// Issues
// =============================================================================

const ISSUE = {
  guid: G1,
  name: "Коллизия стены",
  created: "2026-09-30T10:00:00Z",
  modified: "2026-09-30T10:00:00Z",
  tagText: "",
  tagTextVisible: true,
  commentCount: 1,
  attachedElementCounts: { creation: 0, highlight: 2, deletion: 0, modification: 0 },
};

describe("get_issues", () => {
  it("calls GetIssues without filters", async () => {
    const h = await harness(() => addonOk({ issues: [ISSUE], count: 1 }));
    const res = await h.call("get_issues");
    expect(h.requests[0]?.addOnCommand).toBe("GetIssues");
    expect(h.requests[0]?.addOnParameters).toEqual({});
    expect(res.json).toEqual({ issues: [ISSUE], count: 1 });
  });

  it("normalizes issue and element references", async () => {
    const h = await harness(() => addonOk({ issues: [], count: 0 }));
    await h.call("get_issues", {
      issues: [G1, { guid: G2 }, { name: "Проверка" }],
      element: { guid: G3 },
      search: "стен",
      includeComments: true,
      includeElements: true,
    });
    expect(h.requests[0]?.addOnParameters).toEqual({
      issues: [G1, G2, "Проверка"],
      element: G3,
      search: "стен",
      includeComments: true,
      includeElements: true,
    });
  });

  it("paginates the issue list client-side", async () => {
    const issues = [1, 2, 3, 4, 5].map((i) => ({ ...ISSUE, name: `Issue ${i}` }));
    const h = await harness(() => addonOk({ issues, count: 5 }));
    const res = await h.call("get_issues", { offset: 1, limit: 2 });
    expect(h.requests[0]?.addOnParameters).toEqual({});
    expect(res.json).toMatchObject({ count: 2, total: 5, offset: 1, hasMore: true });
    expect((res.json as { issues: { name: string }[] }).issues.map((i) => i.name)).toEqual(["Issue 2", "Issue 3"]);
  });
});

describe("create_issue", () => {
  it("sends normalized issue specs to CreateIssues", async () => {
    const response = { results: [{ guid: G3, name: "Нет двери", comments: [G2] }] };
    const h = await harness(() => addonOk(response));
    const res = await h.call("create_issue", {
      issues: [
        {
          name: "Нет двери",
          tagText: "Д-1",
          tagTextVisible: false,
          parentIssue: { guid: G1 },
          comment: "Добавить дверь в санузел",
          comments: [{ text: "Проверить ширину", status: "Warning", author: "Claude" }],
          attach: { highlight: [G1, { guid: G2 }], deletion: [] },
        },
        { name: "Простая" },
      ],
      undoName: "Review",
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("CreateIssues");
    expect(h.requests[0]?.addOnParameters).toEqual({
      issues: [
        {
          name: "Нет двери",
          tagText: "Д-1",
          tagTextVisible: false,
          parentIssue: G1,
          comment: "Добавить дверь в санузел",
          comments: [{ text: "Проверить ширину", status: "Warning", author: "Claude" }],
          attach: { highlight: [G1, G2] },
        },
        { name: "Простая" },
      ],
      undoName: "Review",
    });
    expect(res.json).toEqual(response);
  });

  it("accepts a structured first comment", async () => {
    const h = await harness(() => addonOk({ results: [] }));
    await h.call("create_issue", { issues: [{ name: "A", comment: { text: "t", status: "Error" } }] });
    expect(h.requests[0]?.addOnParameters).toEqual({ issues: [{ name: "A", comment: { text: "t", status: "Error" } }] });
  });

  it("rejects empty names, bad statuses and empty batches", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("create_issue", { issues: [{ name: "" }] })).isError).toBe(true);
    expect((await h.call("create_issue", { issues: [{ name: "A", comments: [{ text: "x", status: "Critical" }] }] })).isError).toBe(true);
    expect((await h.call("create_issue", { issues: [] })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("keeps per-item errors in the result", async () => {
    const response = { results: [{ guid: G1, name: "A", comments: [] }, { error: { code: 1, message: "Issue 'X' not found." } }] };
    const h = await harness(() => addonOk(response));
    const res = await h.call("create_issue", { issues: [{ name: "A" }, { name: "B", parentIssue: "X" }] });
    expect(res.isError).toBe(false);
    expect(res.json).toEqual(response);
  });
});

describe("delete_issue", () => {
  it("sends DeleteIssues with normalized refs", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G1, name: "A", deleted: true }] }));
    await h.call("delete_issue", { issues: [{ guid: G1 }, "Имя"], acceptAllElements: true });
    expect(h.requests[0]?.addOnCommand).toBe("DeleteIssues");
    expect(h.requests[0]?.addOnParameters).toEqual({ issues: [G1, "Имя"], acceptAllElements: true });
  });

  it("requires at least one issue", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("delete_issue", { issues: [] })).isError).toBe(true);
    expect((await h.call("delete_issue", {})).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("add_issue_comment / get_issue_comments", () => {
  it("sends comments to AddIssueComments", async () => {
    const h = await harness(() => addonOk({ results: [] }));
    await h.call("add_issue_comment", {
      comments: [
        { issue: { name: "A" }, text: "Готово", status: "Info" },
        { issue: G1, text: "Второй", author: "Иван" },
      ],
    });
    expect(h.requests[0]?.addOnCommand).toBe("AddIssueComments");
    expect(h.requests[0]?.addOnParameters).toEqual({
      comments: [
        { issue: "A", text: "Готово", status: "Info" },
        { issue: G1, text: "Второй", author: "Иван" },
      ],
    });
  });

  it("rejects comments without text", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("add_issue_comment", { comments: [{ issue: G1, text: "" }] })).isError).toBe(true);
    expect((await h.call("add_issue_comment", { comments: [{ issue: G1 }] })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("reads comments with GetIssueComments", async () => {
    const response = { issues: [{ guid: G1, name: "A", comments: [{ guid: G2, author: "Claude", text: "x", status: "Info", created: "" }] }] };
    const h = await harness(() => addonOk(response));
    const res = await h.call("get_issue_comments", { issues: [G1] });
    expect(h.requests[0]?.addOnCommand).toBe("GetIssueComments");
    expect(h.requests[0]?.addOnParameters).toEqual({ issues: [G1] });
    expect(res.json).toEqual(response);
    await h.call("get_issue_comments");
    expect(h.requests[1]?.addOnParameters).toEqual({});
  });
});

describe("attach_elements_to_issue", () => {
  it("attaches elements with the given type", async () => {
    const h = await harness(() => addonOk({ issue: { guid: G1, name: "A" }, type: "Deletion", attached: [G2] }));
    await h.call("attach_elements_to_issue", { issue: "A", elements: [{ guid: G2 }], type: "Deletion" });
    expect(h.requests[0]?.addOnCommand).toBe("AttachElementsToIssue");
    expect(h.requests[0]?.addOnParameters).toEqual({ issue: "A", elements: [G2], type: "Deletion" });
  });

  it("implies type Modification for modificationPairs", async () => {
    const h = await harness(() => addonOk({}));
    await h.call("attach_elements_to_issue", { issue: { guid: G1 }, modificationPairs: [{ original: G2, modified: { guid: G3 } }] });
    expect(h.requests[0]?.addOnParameters).toEqual({
      issue: G1,
      type: "Modification",
      modificationPairs: [{ original: G2, modified: G3 }],
    });
  });

  it("omits the type when not given (add-on default Highlight)", async () => {
    const h = await harness(() => addonOk({}));
    await h.call("attach_elements_to_issue", { issue: "A", elements: [G1] });
    expect(h.requests[0]?.addOnParameters).toEqual({ issue: "A", elements: [G1] });
  });

  it("rejects missing elements and pairs with a non-Modification type", async () => {
    const h = await harness(() => addonOk({}));
    const none = await h.call("attach_elements_to_issue", { issue: "A" });
    expect(none.isError).toBe(true);
    expect(none.text).toContain("give elements");
    const wrong = await h.call("attach_elements_to_issue", { issue: "A", type: "Highlight", modificationPairs: [{ original: G1, modified: G2 }] });
    expect(wrong.isError).toBe(true);
    expect(wrong.text).toContain("Modification");
    expect(h.requests).toHaveLength(0);
  });
});

describe("detach_elements_from_issue / get_issue_elements", () => {
  it("detaches elements", async () => {
    const h = await harness(() => addonOk({ issue: { guid: G1, name: "A" }, detached: [G2] }));
    await h.call("detach_elements_from_issue", { issue: G1, elements: [G2, { guid: G3 }] });
    expect(h.requests[0]?.addOnCommand).toBe("DetachElementsFromIssue");
    expect(h.requests[0]?.addOnParameters).toEqual({ issue: G1, elements: [G2, G3] });
  });

  it("surfaces the 'not attached' error", async () => {
    const h = await harness(() => ERR("None of the given elements is attached to issue 'A' (see get_issue_elements)."));
    const res = await h.call("detach_elements_from_issue", { issue: "A", elements: [G2] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("get_issue_elements");
  });

  it("reads attached elements by type", async () => {
    const h = await harness(() => addonOk({ issues: [] }));
    await h.call("get_issue_elements", { issues: ["A"], types: ["Highlight", "Creation"] });
    expect(h.requests[0]?.addOnCommand).toBe("GetIssueElements");
    expect(h.requests[0]?.addOnParameters).toEqual({ issues: ["A"], types: ["Highlight", "Creation"] });
    expect((await h.call("get_issue_elements", { types: ["Moved"] })).isError).toBe(true);
    expect(h.requests).toHaveLength(1);
  });
});

describe("export_bcf / import_bcf", () => {
  it("exports with all options", async () => {
    const response = { path: "/tmp/review.bcfzip", exported: 1, issues: [{ guid: G1, name: "A" }], fileExists: true };
    const h = await harness(() => addonOk(response));
    const res = await h.call("export_bcf", {
      path: "/tmp/review.bcfzip",
      issues: [{ name: "A" }],
      useExternalId: true,
      alignBySurveyPoint: false,
      overwrite: true,
      createFolders: true,
    });
    expect(h.requests[0]?.addOnCommand).toBe("ExportIssuesToBCF");
    expect(h.requests[0]?.addOnParameters).toEqual({
      path: "/tmp/review.bcfzip",
      issues: ["A"],
      useExternalId: true,
      alignBySurveyPoint: false,
      overwrite: true,
      createFolders: true,
    });
    expect(res.json).toEqual(response);
  });

  it("expands ~ in paths", async () => {
    const h = await harness(() => addonOk({}));
    await h.call("export_bcf", { path: "~/Desktop/x" });
    expect(h.requests[0]?.addOnParameters).toEqual({ path: join(homedir(), "Desktop/x") });
  });

  it("rejects relative paths and wrong extensions", async () => {
    const h = await harness(() => addonOk({}));
    const rel = await h.call("export_bcf", { path: "review.bcfzip" });
    expect(rel.isError).toBe(true);
    expect(rel.text).toContain("absolute path");
    const ext = await h.call("export_bcf", { path: "/tmp/review.pdf" });
    expect(ext.isError).toBe(true);
    expect(ext.text).toContain(".bcfzip");
    expect((await h.call("import_bcf", { path: "in/x.bcfzip" })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("imports a BCF file", async () => {
    const h = await harness(() => addonOk({ imported: 1, issues: [ISSUE] }));
    const res = await h.call("import_bcf", { path: "/tmp/in.bcfzip", alignBySurveyPoint: true, openIssuePalette: true });
    expect(h.requests[0]?.addOnCommand).toBe("ImportIssuesFromBCF");
    expect(h.requests[0]?.addOnParameters).toEqual({ path: "/tmp/in.bcfzip", alignBySurveyPoint: true, openIssuePalette: true });
    expect(res.json).toEqual({ imported: 1, issues: [ISSUE] });
  });

  it("surfaces a missing-file error", async () => {
    const h = await harness(() => ERR("The file '/tmp/none.bcfzip' does not exist. Give the absolute path of an existing file."));
    const res = await h.call("import_bcf", { path: "/tmp/none.bcfzip" });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("does not exist");
  });
});

// =============================================================================
// Favorites
// =============================================================================

const FAVS = [
  { name: "Стена 300", type: "Wall", folder: ["Стены"], folderPath: "Стены" },
  { name: "Стена 200", type: "Wall", folder: [], folderPath: "" },
  { name: "Колонна", type: "Column", folder: [], folderPath: "" },
];

describe("get_favorites", () => {
  it("lists all favorites", async () => {
    const h = await harness(() => addonOk({ favorites: FAVS, count: 3 }));
    const res = await h.call("get_favorites");
    expect(h.requests[0]?.addOnCommand).toBe("GetFavorites");
    expect(h.requests[0]?.addOnParameters).toEqual({});
    expect(res.json).toEqual({ favorites: FAVS, count: 3 });
  });

  it("forwards filters", async () => {
    const h = await harness(() => addonOk({ favorites: [], count: 0 }));
    await h.call("get_favorites", {
      type: "Object",
      variation: "GridElement",
      search: "стен",
      folder: "Стены/Наружные",
      names: ["Стена 300"],
      includeSettings: true,
      includeGdlParameters: false,
    });
    expect(h.requests[0]?.addOnParameters).toEqual({
      type: "Object",
      variation: "GridElement",
      search: "стен",
      folder: "Стены/Наружные",
      names: ["Стена 300"],
      includeSettings: true,
      includeGdlParameters: false,
    });
  });

  it("paginates and validates", async () => {
    const h = await harness(() => addonOk({ favorites: FAVS, count: 3 }));
    const res = await h.call("get_favorites", { limit: 2 });
    expect(res.json).toMatchObject({ count: 2, total: 3, offset: 0, hasMore: true });
    expect((await h.call("get_favorites", { variation: "Door" })).isError).toBe(true);
    expect((await h.call("get_favorites", { type: "Kitchen" })).isError).toBe(true);
    expect(h.requests).toHaveLength(1);
  });
});

describe("apply_favorite", () => {
  it("applies to the tool defaults by default", async () => {
    const h = await harness(() => addonOk({ favorite: "Стена 300", type: "Wall", target: "Defaults", applied: true }));
    const res = await h.call("apply_favorite", { name: "Стена 300" });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("ApplyFavorite");
    expect(h.requests[0]?.addOnParameters).toEqual({ name: "Стена 300", target: "Defaults" });
  });

  it("infers target Elements when elements are given", async () => {
    const h = await harness(() => addonOk({}));
    await h.call("apply_favorite", { name: "Стена 300", elements: [G1, { guid: G2 }], applyProperties: false, undoName: "Fav" });
    expect(h.requests[0]?.addOnParameters).toEqual({
      name: "Стена 300",
      target: "Elements",
      elements: [G1, G2],
      applyProperties: false,
      undoName: "Fav",
    });
  });

  it("validates target/elements combinations", async () => {
    const h = await harness(() => addonOk({}));
    const noElems = await h.call("apply_favorite", { name: "X", target: "Elements" });
    expect(noElems.isError).toBe(true);
    expect(noElems.text).toContain("needs elements");
    const extra = await h.call("apply_favorite", { name: "X", target: "Defaults", elements: [G1] });
    expect(extra.isError).toBe(true);
    expect(extra.text).toContain("only used with target 'Elements'");
    expect(h.requests).toHaveLength(0);
  });

  it("surfaces unknown favorite errors", async () => {
    const h = await harness(() => ERR("Favorite 'Стена' does not exist. Did you mean 'Стена 300', 'Стена 200'?"));
    const res = await h.call("apply_favorite", { name: "Стена" });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("Did you mean");
  });
});

describe("create_favorite / delete_favorite / rename / export / import", () => {
  it("creates favorites from an element and from tool defaults", async () => {
    const h = await harness(() => addonOk({ results: [] }));
    await h.call("create_favorite", {
      favorites: [
        { name: "Моя стена", element: { guid: G1 }, folder: ["Стены"], replace: true, includeProperties: false },
        { name: "Колонна 400", toolDefaults: { type: "Column" }, folder: "Колонны/Бетон" },
      ],
    });
    expect(h.requests[0]?.addOnCommand).toBe("CreateFavorites");
    expect(h.requests[0]?.addOnParameters).toEqual({
      favorites: [
        { name: "Моя стена", element: G1, folder: ["Стены"], replace: true, includeProperties: false },
        { name: "Колонна 400", toolDefaults: { type: "Column" }, folder: "Колонны/Бетон" },
      ],
    });
  });

  it("requires exactly one source per favorite", async () => {
    const h = await harness(() => addonOk({}));
    const none = await h.call("create_favorite", { favorites: [{ name: "A" }] });
    expect(none.isError).toBe(true);
    expect(none.text).toContain("exactly one of element");
    const both = await h.call("create_favorite", { favorites: [{ name: "A", element: G1, toolDefaults: { type: "Wall" } }] });
    expect(both.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("deletes and renames favorites", async () => {
    const h = await harness(() => addonOk({ results: [] }));
    await h.call("delete_favorite", { names: ["A", "B"] });
    expect(h.requests[0]?.addOnCommand).toBe("DeleteFavorites");
    expect(h.requests[0]?.addOnParameters).toEqual({ names: ["A", "B"] });
    await h.call("rename_favorite", { name: "A", newName: "C" });
    expect(h.requests[1]?.addOnCommand).toBe("RenameFavorite");
    expect(h.requests[1]?.addOnParameters).toEqual({ name: "A", newName: "C" });
    expect((await h.call("delete_favorite", { names: [] })).isError).toBe(true);
    expect(h.requests).toHaveLength(2);
  });

  it("exports and imports favorite files", async () => {
    const h = await harness(() => addonOk({}));
    await h.call("export_favorites", { path: "/tmp/fav.prf", names: ["A"], overwrite: true });
    expect(h.requests[0]?.addOnCommand).toBe("ExportFavorites");
    expect(h.requests[0]?.addOnParameters).toEqual({ path: "/tmp/fav.prf", names: ["A"], overwrite: true });
    await h.call("import_favorites", { path: "/tmp/fav.prf", folder: ["Импорт"], importFolders: false, conflictPolicy: "Skip" });
    expect(h.requests[1]?.addOnCommand).toBe("ImportFavorites");
    expect(h.requests[1]?.addOnParameters).toEqual({ path: "/tmp/fav.prf", folder: ["Импорт"], importFolders: false, conflictPolicy: "Skip" });
    expect((await h.call("import_favorites", { path: "/tmp/fav.prf", conflictPolicy: "Merge" })).isError).toBe(true);
    expect((await h.call("export_favorites", { path: "fav.prf" })).isError).toBe(true);
    expect(h.requests).toHaveLength(2);
  });
});

// =============================================================================
// Tool defaults
// =============================================================================

describe("get_tool_defaults", () => {
  it("lists the toolbox without a type", async () => {
    const response = { tools: [{ type: "Wall" }, { type: "Object", variation: "GridElement" }], activeTool: { type: "Wall" } };
    const h = await harness(() => addonOk(response));
    const res = await h.call("get_tool_defaults");
    expect(h.requests[0]?.addOnCommand).toBe("GetToolDefaults");
    expect(h.requests[0]?.addOnParameters).toEqual({});
    expect(res.json).toEqual(response);
  });

  it("reads one type with options", async () => {
    const h = await harness(() => addonOk({ defaults: [{ type: "Wall", settings: { height: 3 } }] }));
    await h.call("get_tool_defaults", {
      type: "Wall",
      includeGdlParameters: false,
      includeClassifications: false,
      includeCategories: false,
      includeProperties: true,
    });
    expect(h.requests[0]?.addOnParameters).toEqual({
      type: "Wall",
      includeGdlParameters: false,
      includeClassifications: false,
      includeCategories: false,
      includeProperties: true,
    });
  });

  it("normalizes mixed types to objects", async () => {
    const h = await harness(() => addonOk({ defaults: [] }));
    await h.call("get_tool_defaults", {
      types: ["Wall", { type: "Object", variation: "GridElement" }, { type: "Door" }],
      gdlParameterNames: ["A", "B"],
      includeHiddenGdlParameters: true,
    });
    expect(h.requests[0]?.addOnParameters).toEqual({
      types: [{ type: "Wall" }, { type: "Object", variation: "GridElement" }, { type: "Door" }],
      gdlParameterNames: ["A", "B"],
      includeHiddenGdlParameters: true,
    });
  });

  it("rejects unknown types and variation without type", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("get_tool_defaults", { type: "wall" })).isError).toBe(true);
    const v = await h.call("get_tool_defaults", { variation: "Door" });
    expect(v.isError).toBe(true);
    expect(v.text).toContain("variation needs type");
    expect(h.requests).toHaveLength(0);
  });
});

describe("set_tool_defaults", () => {
  it("sends the fields per tool to SetToolDefaults", async () => {
    const response = {
      results: [
        { type: "Wall", applied: ["height", "thickness", "layer"], ignored: ["begin"], settings: { height: 3, thickness: 0.25 } },
        { type: "Door", applied: ["libraryPart", "width"], rejected: [{ field: "sillHeight2", error: "Unknown field" }] },
      ],
    };
    const h = await harness(() => addonOk(response));
    const res = await h.call("set_tool_defaults", {
      defaults: [
        { type: "Wall", fields: { height: 3, thickness: 0.25, layer: "Конструктив - Стены Несущие", begin: { x: 0, y: 0 } } },
        { type: "Door", fields: { libraryPart: "Дверь 26", width: 0.9, gdlParams: { gs_leaf_type: 1 } } },
        { type: "Object", variation: "Object", fields: { classifications: [G1], properties: { "Общие/Марка": "M1" } } },
      ],
      returnSettings: false,
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("SetToolDefaults");
    expect(h.requests[0]?.addOnParameters).toEqual({
      defaults: [
        { type: "Wall", fields: { height: 3, thickness: 0.25, layer: "Конструктив - Стены Несущие", begin: { x: 0, y: 0 } } },
        { type: "Door", fields: { libraryPart: "Дверь 26", width: 0.9, gdlParams: { gs_leaf_type: 1 } } },
        { type: "Object", variation: "Object", fields: { classifications: [G1], properties: { "Общие/Марка": "M1" } } },
      ],
      returnSettings: false,
    });
    expect(res.json).toEqual(response);
  });

  it("rejects empty fields, unknown types and empty batches", async () => {
    const h = await harness(() => addonOk({}));
    const empty = await h.call("set_tool_defaults", { defaults: [{ type: "Wall", fields: {} }] });
    expect(empty.isError).toBe(true);
    expect(empty.text).toContain("at least one field");
    expect((await h.call("set_tool_defaults", { defaults: [{ type: "Chair", fields: { a: 1 } }] })).isError).toBe(true);
    expect((await h.call("set_tool_defaults", { defaults: [] })).isError).toBe(true);
    expect((await h.call("set_tool_defaults", { defaults: [{ type: "Wall" }] })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("surfaces tool errors", async () => {
    const h = await harness(() => ERR("Cannot read the default settings of the Wall tool: APIERR_BADPARS"));
    const res = await h.call("set_tool_defaults", { defaults: [{ type: "Wall", fields: { height: 3 } }] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("default settings");
  });
});

// =============================================================================
// Revisions
// =============================================================================

describe("get_revisions", () => {
  it("calls GetRevisions with defaults and options", async () => {
    const h = await harness(() => addonOk({ issues: [], documentRevisions: [] }));
    const res = await h.call("get_revisions");
    expect(h.requests[0]?.addOnCommand).toBe("GetRevisions");
    expect(h.requests[0]?.addOnParameters).toEqual({});
    expect(res.json).toEqual({ issues: [], documentRevisions: [] });
    await h.call("get_revisions", { include: ["documentRevisions"], issue: "Выпуск 1" });
    expect(h.requests[1]?.addOnParameters).toEqual({ include: ["documentRevisions"], issue: "Выпуск 1" });
    expect((await h.call("get_revisions", { include: ["changes"] })).isError).toBe(true);
    expect(h.requests).toHaveLength(2);
  });
});

describe("get_revision_changes", () => {
  it("returns all changes without a mode", async () => {
    const h = await harness(() => addonOk({ changes: [], count: 0 }));
    await h.call("get_revision_changes");
    expect(h.requests[0]?.addOnCommand).toBe("GetRevisionChanges");
    expect(h.requests[0]?.addOnParameters).toEqual({});
  });

  it("forwards each mode with normalized refs", async () => {
    const h = await harness(() => addonOk({}));
    await h.call("get_revision_changes", { documentRevision: G1, includeFirstIssue: true });
    await h.call("get_revision_changes", { layouts: ["A.01 План", { guid: G2 }] });
    await h.call("get_revision_changes", { allLayouts: true });
    await h.call("get_revision_changes", { elements: [{ guid: G3 }] });
    await h.call("get_revision_changes", { changeIds: ["C-001"] });
    expect(h.requests.map((r) => r.addOnParameters)).toEqual([
      { documentRevision: G1, includeFirstIssue: true },
      { layouts: ["A.01 План", G2] },
      { allLayouts: true },
      { elements: [G3] },
      { changeIds: ["C-001"] },
    ]);
  });

  it("drops allLayouts: false and rejects several modes", async () => {
    const h = await harness(() => addonOk({}));
    await h.call("get_revision_changes", { allLayouts: false });
    expect(h.requests[0]?.addOnParameters).toEqual({});
    const res = await h.call("get_revision_changes", { elements: [G1], changeIds: ["C-1"] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("only one of");
    expect(h.requests).toHaveLength(1);
  });
});
