import { describe, expect, it } from "vitest";

import { addonOk, harness } from "../helpers.js";

const G1 = "92F5E7F6-2460-FF4F-A499-0B07286D2B5F";
const G2 = "1A2B3C4D-1111-2222-3333-444455556666";
const G3 = "ABCDEF01-2345-6789-ABCD-EF0123456789";

const OK = () => addonOk({ results: [] });

async function call(name: string, args: Record<string, unknown>, responder: Parameters<typeof harness>[0] = OK) {
  const h = await harness(responder);
  const res = await h.call(name, args);
  return { h, res, req: h.requests[0] };
}

describe("element-edit tool registration", () => {
  it("registers every element-edit tool", async () => {
    const h = await harness(OK);
    const names = (await h.listTools()).map((t) => t.name);
    for (const n of [
      "move_elements",
      "copy_elements",
      "rotate_elements",
      "mirror_elements",
      "elevate_elements",
      "delete_elements",
      "copy_elements_to_stories",
      "group_elements",
      "ungroup_elements",
      "lock_elements",
      "unlock_elements",
      "set_draw_order",
      "trim_elements",
      "remove_trim",
      "merge_elements",
      "solid_operation",
      "remove_solid_operation",
      "resize_elements",
      "unmerge_elements",
      "set_suspend_groups",
      "get_element_edit_relations",
    ]) {
      expect(names).toContain(n);
    }
  });

  it("marks delete_elements destructive and get_element_edit_relations read-only", async () => {
    const h = await harness(OK);
    const tools = await h.listTools();
    expect(tools.find((t) => t.name === "delete_elements")?.annotations?.destructiveHint).toBe(true);
    expect(tools.find((t) => t.name === "get_element_edit_relations")?.annotations?.readOnlyHint).toBe(true);
  });
});

describe("move_elements", () => {
  it("sends elements + vector as one MoveElements operation, normalizing {guid} refs", async () => {
    const reply = { results: [{ guid: G1 }, { guid: G2 }] };
    const { res, req } = await call("move_elements", { elements: [G1, { guid: G2 }], vector: { x: 1.5, y: -2 } }, () => addonOk(reply));
    expect(res.isError).toBe(false);
    expect(req?.command).toBe("API.ExecuteAddOnCommand");
    expect(req?.addOnCommand).toBe("MoveElements");
    expect(req?.addOnParameters).toEqual({ elements: [G1, G2], vector: { x: 1.5, y: -2 } });
    expect(res.json).toEqual(reply);
  });

  it("sends several moves as operations in one call", async () => {
    const { req } = await call("move_elements", {
      moves: [
        { elements: [G1], vector: { x: 1, y: 0 } },
        { elements: [G2], vector: { x: 0, y: 2, z: 0.5 } },
      ],
      includeGroupMembers: true,
    });
    expect(req?.addOnParameters).toEqual({
      operations: [
        { elements: [G1], vector: { x: 1, y: 0 }, includeGroupMembers: true },
        { elements: [G2], vector: { x: 0, y: 2, z: 0.5 }, includeGroupMembers: true },
      ],
    });
  });

  it("rejects a call without vector before contacting Archicad", async () => {
    const { h, res } = await call("move_elements", { elements: [G1] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("vector");
    expect(h.requests).toHaveLength(0);
  });

  it("rejects mixing moves with elements", async () => {
    const { h, res } = await call("move_elements", { elements: [G1], vector: { x: 1, y: 0 }, moves: [{ elements: [G2], vector: { x: 1, y: 0 } }] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("rejects malformed GUIDs", async () => {
    const { h, res } = await call("move_elements", { elements: ["not-a-guid"], vector: { x: 1, y: 0 } });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("surfaces add-on errors as tool errors", async () => {
    const { res } = await call("move_elements", { elements: [G1], vector: { x: 0, y: 0 } }, () =>
      addonOk({ error: { code: -2130313112, message: "'vector' is zero: give a displacement in meters" } }),
    );
    expect(res.isError).toBe(true);
    expect(res.text).toContain("'vector' is zero");
  });
});

describe("copy / rotate / mirror / elevate / resize", () => {
  it("copy_elements passes vector and count", async () => {
    const reply = { results: [{ guid: G1, copies: [G2, G3] }], createdCount: 2 };
    const { res, req } = await call("copy_elements", { elements: [G1], vector: { x: 6, y: 0 }, count: 2 }, () => addonOk(reply));
    expect(req?.addOnCommand).toBe("CopyElements");
    expect(req?.addOnParameters).toEqual({ elements: [G1], vector: { x: 6, y: 0 }, count: 2 });
    expect(res.json).toEqual(reply);
  });

  it("copy_elements validates count", async () => {
    const { h, res } = await call("copy_elements", { elements: [G1], vector: { x: 1, y: 0 }, count: 0 });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("rotate_elements passes angle in degrees, centre and copy options", async () => {
    const { req } = await call("rotate_elements", { elements: [G1], angle: 90, center: { x: 1, y: 2 }, copy: true, count: 3 });
    expect(req?.addOnCommand).toBe("RotateElements");
    expect(req?.addOnParameters).toEqual({ elements: [G1], angle: 90, center: { x: 1, y: 2 }, copy: true, count: 3 });
  });

  it("rotate_elements works without centre (bounding-box pivot)", async () => {
    const { req } = await call("rotate_elements", { elements: [G1], angle: -45 });
    expect(req?.addOnParameters).toEqual({ elements: [G1], angle: -45 });
  });

  it("mirror_elements by two points", async () => {
    const { req } = await call("mirror_elements", { elements: [G1], axisStart: { x: 0, y: 0 }, axisEnd: { x: 0, y: 1 }, copy: true });
    expect(req?.addOnCommand).toBe("MirrorElements");
    expect(req?.addOnParameters).toEqual({ elements: [G1], axisStart: { x: 0, y: 0 }, axisEnd: { x: 0, y: 1 }, copy: true });
  });

  it("mirror_elements by axis name", async () => {
    const { req } = await call("mirror_elements", { elements: [G1], axis: "Vertical", through: { x: 5, y: 0 } });
    expect(req?.addOnParameters).toEqual({ elements: [G1], axis: "Vertical", through: { x: 5, y: 0 } });
  });

  it("mirror_elements requires a complete mirror line", async () => {
    for (const args of [{ elements: [G1] }, { elements: [G1], axisStart: { x: 0, y: 0 } }, { elements: [G1], axis: "Diagonal" }]) {
      const { h, res } = await call("mirror_elements", args);
      expect(res.isError).toBe(true);
      expect(h.requests).toHaveLength(0);
    }
  });

  it("elevate_elements passes deltaZ", async () => {
    const { req } = await call("elevate_elements", { elements: [G1, G2], deltaZ: 0.3 });
    expect(req?.addOnCommand).toBe("ElevateElements");
    expect(req?.addOnParameters).toEqual({ elements: [G1, G2], deltaZ: 0.3 });
  });

  it("resize_elements requires a positive ratio", async () => {
    const bad = await call("resize_elements", { elements: [G1], ratio: -2 });
    expect(bad.res.isError).toBe(true);
    expect(bad.h.requests).toHaveLength(0);
    const { req } = await call("resize_elements", { elements: [G1], ratio: 2, center: { x: 0, y: 0 } });
    expect(req?.addOnCommand).toBe("ResizeElements");
    expect(req?.addOnParameters).toEqual({ elements: [G1], ratio: 2, center: { x: 0, y: 0 } });
  });
});

describe("delete / copy to stories", () => {
  it("delete_elements forwards guids and returns the per-item result", async () => {
    const reply = { results: [{ guid: G1, deleted: true }], deletedCount: 1, alsoDeleted: [{ guid: G2, type: "Window" }] };
    const { res, req } = await call("delete_elements", { elements: [{ guid: G1 }] }, () => addonOk(reply));
    expect(req?.addOnCommand).toBe("DeleteElements");
    expect(req?.addOnParameters).toEqual({ elements: [G1] });
    expect(res.json).toEqual(reply);
  });

  it("delete_elements requires at least one element", async () => {
    const { h, res } = await call("delete_elements", { elements: [] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("copy_elements_to_stories accepts story indices and names", async () => {
    const { req } = await call("copy_elements_to_stories", { elements: [G1], stories: [1, "Этаж 2", { index: 3 }], includeGroupMembers: false });
    expect(req?.addOnCommand).toBe("CopyElementsToStories");
    expect(req?.addOnParameters).toEqual({ elements: [G1], stories: [1, "Этаж 2", { index: 3 }], includeGroupMembers: false });
  });

  it("copy_elements_to_stories requires stories", async () => {
    const { h, res } = await call("copy_elements_to_stories", { elements: [G1], stories: [] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("groups, locking, drawing order", () => {
  it("group_elements with one group", async () => {
    const reply = { groupGuid: G3, members: [G1, G2], elementCount: 2 };
    const { res, req } = await call("group_elements", { elements: [G1, G2] }, () => addonOk(reply));
    expect(req?.addOnCommand).toBe("GroupElements");
    expect(req?.addOnParameters).toEqual({ elements: [G1, G2] });
    expect(res.json).toEqual(reply);
  });

  it("group_elements with several groups and a parent group", async () => {
    const { req } = await call("group_elements", { groups: [{ elements: [G1, G2], parentGroup: G3 }] });
    expect(req?.addOnParameters).toEqual({ groups: [{ elements: [G1, G2], parentGroup: G3 }] });
  });

  it("group_elements needs at least two elements", async () => {
    const one = await call("group_elements", { elements: [G1] });
    expect(one.res.isError).toBe(true);
    const none = await call("group_elements", {});
    expect(none.res.isError).toBe(true);
    expect(none.h.requests).toHaveLength(0);
  });

  it("ungroup_elements passes completely", async () => {
    const { req } = await call("ungroup_elements", { elements: [G3], completely: true });
    expect(req?.addOnCommand).toBe("UngroupElements");
    expect(req?.addOnParameters).toEqual({ elements: [G3], completely: true });
  });

  it("set_suspend_groups reads when suspend is omitted", async () => {
    const { req, res } = await call("set_suspend_groups", {}, () => addonOk({ suspendGroups: false, changed: false }));
    expect(req?.addOnCommand).toBe("SetSuspendGroups");
    expect(req?.addOnParameters).toEqual({});
    expect(res.json).toEqual({ suspendGroups: false, changed: false });
  });

  it("lock_elements / unlock_elements", async () => {
    const lock = await call("lock_elements", { elements: [G1], includeGroupMembers: true });
    expect(lock.req?.addOnCommand).toBe("LockElements");
    expect(lock.req?.addOnParameters).toEqual({ elements: [G1], includeGroupMembers: true });
    const unlock = await call("unlock_elements", { elements: [G1] });
    expect(unlock.req?.addOnCommand).toBe("UnlockElements");
    expect(unlock.req?.addOnParameters).toEqual({ elements: [G1] });
  });

  it("set_draw_order with an action and steps", async () => {
    const { req } = await call("set_draw_order", { elements: [G1], action: "BringForward", steps: 2 });
    expect(req?.addOnCommand).toBe("SetDrawOrder");
    expect(req?.addOnParameters).toEqual({ elements: [G1], action: "BringForward", steps: 2 });
  });

  it("set_draw_order with a level", async () => {
    const { req } = await call("set_draw_order", { elements: [G1], level: 3 });
    expect(req?.addOnParameters).toEqual({ elements: [G1], level: 3 });
  });

  it("set_draw_order rejects ambiguous or invalid input", async () => {
    for (const args of [
      { elements: [G1] },
      { elements: [G1], action: "Reset", level: 2 },
      { elements: [G1], action: "BringToFront", steps: 2 },
      { elements: [G1], level: 15 },
      { elements: [G1], action: "Top" },
    ]) {
      const { h, res } = await call("set_draw_order", args);
      expect(res.isError).toBe(true);
      expect(h.requests).toHaveLength(0);
    }
  });
});

describe("trim / merge", () => {
  it("trim_elements with a trimming roof and trim type", async () => {
    const { req } = await call("trim_elements", { elements: [G1, G2], trimWith: { guid: G3 }, trimType: "KeepOutside" });
    expect(req?.addOnCommand).toBe("TrimElements");
    expect(req?.addOnParameters).toEqual({ elements: [G1, G2], trimWith: G3, trimType: "KeepOutside" });
  });

  it("trim_elements without trimWith (roofs in the list trim the rest)", async () => {
    const { req } = await call("trim_elements", { elements: [G1, G3] });
    expect(req?.addOnParameters).toEqual({ elements: [G1, G3] });
  });

  it("trim_elements rejects trimType without trimWith", async () => {
    const { h, res } = await call("trim_elements", { elements: [G1], trimType: "KeepInside" });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("trimWith");
    expect(h.requests).toHaveLength(0);
  });

  it("remove_trim maps pairs and elements", async () => {
    const { req } = await call("remove_trim", { pairs: [{ element: G1, trimmingElement: { guid: G3 } }], elements: [G2] });
    expect(req?.addOnCommand).toBe("RemoveTrims");
    expect(req?.addOnParameters).toEqual({ pairs: [{ element: G1, trimmingElement: G3 }], elements: [G2] });
  });

  it("remove_trim needs pairs or elements", async () => {
    const { h, res } = await call("remove_trim", {});
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("merge_elements needs two elements", async () => {
    const bad = await call("merge_elements", { elements: [G1] });
    expect(bad.res.isError).toBe(true);
    expect(bad.h.requests).toHaveLength(0);
    const { req } = await call("merge_elements", { elements: [G1, G2] });
    expect(req?.addOnCommand).toBe("MergeElements");
    expect(req?.addOnParameters).toEqual({ elements: [G1, G2] });
  });

  it("unmerge_elements maps pairs", async () => {
    const { req } = await call("unmerge_elements", { pairs: [{ element: G1, otherElement: G2 }] });
    expect(req?.addOnCommand).toBe("UnmergeElements");
    expect(req?.addOnParameters).toEqual({ pairs: [{ element: G1, otherElement: G2 }] });
  });
});

describe("solid operations", () => {
  it("solid_operation single form becomes one operation", async () => {
    const reply = { results: [{ target: G1, links: [{ operator: G2, operation: "SubtractUpwards" }] }] };
    const { req, res } = await call(
      "solid_operation",
      { target: G1, operators: [{ guid: G2 }], operation: "SubtractUpwards", inheritOperatorAttributes: true },
      () => addonOk(reply),
    );
    expect(req?.addOnCommand).toBe("CreateSolidOperations");
    expect(req?.addOnParameters).toEqual({
      operations: [{ target: G1, operators: [G2], operation: "SubtractUpwards", inheritOperatorAttributes: true }],
    });
    expect(res.json).toEqual(reply);
  });

  it("solid_operation batch form with permanent morph boolean", async () => {
    const { req } = await call("solid_operation", {
      operations: [
        { target: G1, operators: [G2], operation: "Add" },
        { target: G3, operators: [G2, G1] },
      ],
      permanent: true,
    });
    expect(req?.addOnParameters).toEqual({
      operations: [
        { target: G1, operators: [G2], operation: "Add" },
        { target: G3, operators: [G2, G1] },
      ],
      permanent: true,
    });
  });

  it("solid_operation validates the operation and required fields", async () => {
    for (const args of [{ target: G1, operators: [G2], operation: "Explode" }, { target: G1 }, {}]) {
      const { h, res } = await call("solid_operation", args);
      expect(res.isError).toBe(true);
      expect(h.requests).toHaveLength(0);
    }
  });

  it("remove_solid_operation maps links", async () => {
    const { req } = await call("remove_solid_operation", { links: [{ target: G1, operator: { guid: G2 } }] });
    expect(req?.addOnCommand).toBe("RemoveSolidOperations");
    expect(req?.addOnParameters).toEqual({ links: [{ target: G1, operator: G2 }] });
  });

  it("get_element_edit_relations is a plain read", async () => {
    const reply = { elements: [{ guid: G1, type: "Wall", locked: false, solidOperators: [{ operator: G2, operation: "Subtract" }] }] };
    const { req, res } = await call("get_element_edit_relations", { elements: [G1] }, () => addonOk(reply));
    expect(req?.addOnCommand).toBe("GetElementEditRelations");
    expect(req?.addOnParameters).toEqual({ elements: [G1] });
    expect(res.json).toEqual(reply);
  });
});
