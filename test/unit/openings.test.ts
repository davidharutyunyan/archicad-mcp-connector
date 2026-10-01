import { describe, expect, it } from "vitest";

import { addonOk, harness } from "../helpers.js";

const WALL = "92F5E7F6-2460-FF4F-A499-0B07286D2B5F";
const ROOF = "0A1B2C3D-4E5F-4A6B-8C7D-9E0F1A2B3C4D";
const WIN = "11111111-2222-4333-8444-555555555555";

describe("openings tools registration", () => {
  it("registers all opening tools with described input schemas", async () => {
    const h = await harness(() => addonOk({}));
    const tools = await h.listTools();
    const names = tools.map((t) => t.name);
    for (const n of ["create_windows", "create_doors", "create_skylights", "create_openings", "modify_openings", "get_host_openings"]) {
      expect(names).toContain(n);
    }
    const win = tools.find((t) => t.name === "create_windows")!;
    const schema = win.inputSchema as { properties: Record<string, { items?: { required?: string[]; properties?: Record<string, { description?: string }> } }> };
    const item = schema.properties["windows"]!.items!;
    expect(item.required).toContain("wall");
    expect(item.properties!["position"]!.description).toMatch(/BEGIN point/);
    expect(item.properties!["libraryPart"]!.description).toMatch(/search_library_parts/);
    // windows always live on the wall's story: no storyIndex field
    expect(item.properties!["storyIndex"]).toBeUndefined();
  });
});

describe("create_windows", () => {
  it("sends typed window specs to CreateElements", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: WIN, type: "Window" }] }));
    const res = await h.call("create_windows", {
      windows: [
        {
          wall: WALL,
          position: 1.5,
          sillHeight: 0.9,
          width: 1.2,
          height: 1.4,
          libraryPart: "Окно 26",
          gdlParams: { gs_frame_thk: 0.07 },
          flipped: true,
          anchor: "Center",
          verticalAnchor: "SillToWallBottom",
        },
      ],
    });
    expect(res.isError).toBe(false);
    expect(h.requests).toHaveLength(1);
    expect(h.requests[0]?.addOnCommand).toBe("CreateElements");
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [
        {
          type: "Window",
          wall: WALL,
          position: 1.5,
          sillHeight: 0.9,
          width: 1.2,
          height: 1.4,
          libraryPart: "Окно 26",
          gdlParams: { gs_frame_thk: 0.07 },
          flipped: true,
          anchor: "Center",
          verticalAnchor: "SillToWallBottom",
        },
      ],
      undoName: "Create windows (Claude)",
    });
    expect(res.json).toEqual({ results: [{ guid: WIN, type: "Window" }] });
  });

  it("accepts a plan point instead of a position and {guid} wall references", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: WIN, type: "Window" }] }));
    const res = await h.call("create_windows", {
      windows: [{ wall: { guid: WALL }, point: { x: 2, y: 0.1 }, positionReference: "Begin" }],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toMatchObject({
      elements: [{ type: "Window", wall: { guid: WALL }, point: { x: 2, y: 0.1 }, positionReference: "Begin" }],
    });
  });

  it("requires position or point before calling Archicad", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("create_windows", { windows: [{ wall: WALL, width: 1 }] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("windows[0]");
    expect(res.text).toContain("position");
    expect(h.requests).toHaveLength(0);
  });

  it("rejects position together with point", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("create_windows", { windows: [{ wall: WALL, position: 1, point: { x: 1, y: 0 } }] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("not both");
    expect(h.requests).toHaveLength(0);
  });

  it("rejects invalid enum values and missing wall", async () => {
    const h = await harness(() => addonOk({}));
    const bad = await h.call("create_windows", { windows: [{ wall: WALL, position: 1, anchor: "Middle" }] });
    expect(bad.isError).toBe(true);
    const noWall = await h.call("create_windows", { windows: [{ position: 1 }] });
    expect(noWall.isError).toBe(true);
    const negative = await h.call("create_windows", { windows: [{ wall: WALL, position: 1, width: -1 }] });
    expect(negative.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("surfaces add-on errors as tool errors", async () => {
    const h = await harness(() => addonOk({ error: { code: -2130313112, message: "Missing required array field 'elements'." } }));
    const res = await h.call("create_windows", { windows: [{ wall: WALL, position: 1 }] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("Missing required array field");
  });

  it("passes per-item errors through unchanged", async () => {
    const itemError = { error: { code: -2130313112, message: "The projection of 'point' lies 7.000 m along the wall reference line, but the wall is only 5.000 m long" } };
    const h = await harness(() => addonOk({ results: [itemError] }));
    const res = await h.call("create_windows", { windows: [{ wall: WALL, point: { x: 7, y: 0 } }] });
    expect(res.isError).toBe(false);
    expect(res.json).toEqual({ results: [itemError] });
  });
});

describe("create_doors", () => {
  it("sends Door specs", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: WIN, type: "Door" }] }));
    const res = await h.call("create_doors", {
      doors: [{ wall: WALL, position: 0.6, positionReference: "Begin", mirrored: true, sillHeight: 0, layer: "Двери" }],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("CreateElements");
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [{ type: "Door", wall: WALL, position: 0.6, positionReference: "Begin", mirrored: true, sillHeight: 0, layer: "Двери" }],
      undoName: "Create doors (Claude)",
    });
  });

  it("requires a placement", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("create_doors", { doors: [{ wall: WALL }] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("doors[0]");
    expect(h.requests).toHaveLength(0);
  });
});

describe("create_skylights", () => {
  it("sends Skylight specs", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: WIN, type: "Skylight" }] }));
    const res = await h.call("create_skylights", {
      skylights: [{ owner: ROOF, point: { x: 3, y: 4 }, anchor: "BottomCenter", azimuthAngle: 90, width: 0.78, height: 1.18 }],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [{ type: "Skylight", owner: ROOF, point: { x: 3, y: 4 }, anchor: "BottomCenter", azimuthAngle: 90, width: 0.78, height: 1.18 }],
      undoName: "Create skylights (Claude)",
    });
  });

  it("requires owner and point", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("create_skylights", { skylights: [{ owner: ROOF }] })).isError).toBe(true);
    expect((await h.call("create_skylights", { skylights: [{ point: { x: 0, y: 0 } }] })).isError).toBe(true);
    expect((await h.call("create_skylights", { skylights: [{ owner: ROOF, point: { x: 0, y: 0 }, verticalAnchor: "SillToStory" }] })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("create_openings", () => {
  it("sends Opening specs for a wall opening", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: WIN, type: "Opening" }] }));
    const res = await h.call("create_openings", {
      openings: [{ owner: WALL, position: 2, bottomElevation: 0.3, width: 0.4, height: 0.3, limit: "Infinite" }],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [{ type: "Opening", owner: WALL, position: 2, bottomElevation: 0.3, width: 0.4, height: 0.3, limit: "Infinite" }],
      undoName: "Create openings (Claude)",
    });
  });

  it("allows circular openings with only a width and 3D points", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: WIN, type: "Opening" }] }));
    const res = await h.call("create_openings", {
      openings: [{ owner: ROOF, shape: "Circular", width: 0.2, point: { x: 1, y: 1, z: 2.5 }, constraint: "Vertical", rotation: 45 }],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toMatchObject({
      elements: [{ type: "Opening", shape: "Circular", width: 0.2, point: { x: 1, y: 1, z: 2.5 }, constraint: "Vertical", rotation: 45 }],
    });
  });

  it("validates size and placement before calling Archicad", async () => {
    const h = await harness(() => addonOk({}));
    const noHeight = await h.call("create_openings", { openings: [{ owner: WALL, position: 1, width: 0.5 }] });
    expect(noHeight.isError).toBe(true);
    expect(noHeight.text).toContain("height");
    const noPlace = await h.call("create_openings", { openings: [{ owner: WALL, width: 0.5, height: 0.5 }] });
    expect(noPlace.isError).toBe(true);
    expect(noPlace.text).toContain("point");
    const custom = await h.call("create_openings", { openings: [{ owner: WALL, position: 1, width: 0.5, height: 0.5, shape: "Custom" }] });
    expect(custom.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("modify_openings", () => {
  it("sends patches to ModifyElements in one undo step", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: WIN }] }));
    const res = await h.call("modify_openings", {
      openings: [
        { guid: WIN, position: 2.4, width: 1.5, gdlParams: { gs_sash_thk: 0.05 } },
        { guid: WALL, bottomElevation: 1.0, rotation: 30 },
      ],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("ModifyElements");
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [
        { guid: WIN, position: 2.4, width: 1.5, gdlParams: { gs_sash_thk: 0.05 } },
        { guid: WALL, bottomElevation: 1.0, rotation: 30 },
      ],
      undoName: "Modify openings (Claude)",
    });
  });

  it("requires a guid per patch", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("modify_openings", { openings: [{ width: 1 }] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("get_host_openings", () => {
  it("normalizes host references and forwards the details flag", async () => {
    const response = { hosts: [{ guid: WALL, type: "Wall", windows: [WIN], doors: [], skylights: [], openings: [] }] };
    const h = await harness(() => addonOk(response));
    const res = await h.call("get_host_openings", { hosts: [{ guid: WALL }, ROOF], details: true });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("GetHostOpenings");
    expect(h.requests[0]?.addOnParameters).toEqual({ hosts: [WALL, ROOF], details: true });
    expect(res.json).toEqual(response);
  });

  it("omits details when not given", async () => {
    const h = await harness(() => addonOk({ hosts: [] }));
    await h.call("get_host_openings", { hosts: [WALL] });
    expect(h.requests[0]?.addOnParameters).toEqual({ hosts: [WALL] });
  });
});
