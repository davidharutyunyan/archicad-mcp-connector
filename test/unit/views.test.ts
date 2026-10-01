import { existsSync, promises as fsp, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { crc32, deflateSync } from "node:zlib";

import { afterAll, describe, expect, it } from "vitest";

import { sniffImageSize } from "../../src/tools/views-image.js";
import { addonOk, harness, type RecordedRequest } from "../helpers.js";

const G1 = "92F5E7F6-2460-FF4F-A499-0B07286D2B5F";
const G2 = "0B2C3D4E-5F60-4A1B-9C2D-3E4F5A6B7C8D";
const DB = "11111111-2222-4333-8444-555555555555";

const PLAN_WINDOW = { type: "FloorPlan", story: { index: 0, name: "1-й этаж", level: 0 }, drawingScale: 100 };
const WINDOW_3D = { type: "3DModel", projection: "perspective" };

// --- tiny image encoders (the fake Archicad writes real picture files) ------------------------

function pngChunk(type: string, data: Buffer): Buffer {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length);
  const body = Buffer.concat([Buffer.from(type, "ascii"), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(body) >>> 0);
  return Buffer.concat([len, body, crc]);
}

/** Minimal valid RGB PNG of the given size (a simple gradient). */
function makePng(width: number, height: number): Buffer {
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8; // bit depth
  ihdr[9] = 2; // RGB
  const raw = Buffer.alloc((width * 3 + 1) * height);
  for (let y = 0; y < height; y++) {
    const row = y * (width * 3 + 1);
    raw[row] = 0; // filter: none
    for (let x = 0; x < width; x++) {
      raw[row + 1 + x * 3] = (x * 255) / Math.max(1, width - 1);
      raw[row + 2 + x * 3] = (y * 255) / Math.max(1, height - 1);
      raw[row + 3 + x * 3] = 128;
    }
  }
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    pngChunk("IHDR", ihdr),
    pngChunk("IDAT", deflateSync(raw)),
    pngChunk("IEND", Buffer.alloc(0)),
  ]);
}

/** JPEG header bytes (SOI + SOF0) — enough for size sniffing, never decoded by these tests. */
function makeJpegHeader(width: number, height: number): Buffer {
  const sof = Buffer.from([0xff, 0xc0, 0x00, 0x11, 0x08, 0, 0, 0, 0, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01]);
  sof.writeUInt16BE(height, 5);
  sof.writeUInt16BE(width, 7);
  return Buffer.concat([Buffer.from([0xff, 0xd8]), Buffer.from([0xff, 0xe0, 0x00, 0x04, 0x00, 0x00]), sof, Buffer.from([0xff, 0xd9])]);
}

const scratch = path.join(os.tmpdir(), `archicad-connector-views-test-${process.pid}`);
afterAll(async () => {
  await fsp.rm(scratch, { recursive: true, force: true });
});

type Routes = Record<string, (req: RecordedRequest) => unknown>;

/** Harness that answers add-on commands by name (unknown commands answer {}). */
function route(routes: Routes) {
  return harness((req) => {
    const name = req.addOnCommand ?? req.command;
    const fn = routes[name];
    return addonOk(fn ? fn(req) : {});
  });
}

function imageOf(content: Array<{ type: string; data?: string; mimeType?: string; text?: string }>) {
  const img = content.find((c) => c.type === "image");
  return img as { type: "image"; data: string; mimeType: string } | undefined;
}

// =============================================================================

describe("tool registration", () => {
  it("registers every views tool with a description and schema", async () => {
    const h = await route({});
    const tools = await h.listTools();
    const names = new Set(tools.map((t) => t.name));
    for (const n of [
      "get_current_window",
      "list_views",
      "open_view",
      "go_to_view",
      "zoom",
      "get_3d_view",
      "set_3d_view",
      "show_in_3d",
      "capture_view",
      "render_view",
      "create_sections",
      "create_elevations",
      "create_interior_elevations",
      "create_details",
      "create_worksheets",
      "get_view_settings",
      "set_view_settings",
    ]) {
      expect(names.has(n), n).toBe(true);
      const tool = tools.find((t) => t.name === n)!;
      expect(tool.description?.length ?? 0).toBeGreaterThan(60);
      expect(tool.inputSchema.type).toBe("object");
    }
  });
});

describe("get_current_window", () => {
  it("calls GetCurrentWindow and returns its answer", async () => {
    const h = await route({ GetCurrentWindow: () => PLAN_WINDOW });
    const res = await h.call("get_current_window");
    expect(res.isError).toBe(false);
    expect(h.requests).toHaveLength(1);
    expect(h.requests[0]?.addOnCommand).toBe("GetCurrentWindow");
    expect(h.requests[0]?.addOnParameters).toEqual({});
    expect(res.json).toEqual(PLAN_WINDOW);
  });

  it("surfaces add-on errors", async () => {
    const h = await route({ GetCurrentWindow: () => ({ error: { code: -2130313111, message: "There is no active Archicad window." } }) });
    const res = await h.call("get_current_window");
    expect(res.isError).toBe(true);
    expect(res.text).toContain("no active Archicad window");
  });
});

describe("list_views", () => {
  it("forwards filters to ListViews", async () => {
    const h = await route({ ListViews: () => ({ viewpoints: [] }) });
    const res = await h.call("list_views", { include: ["viewpoints", "viewMap"], types: ["Section", "3DModel"], nameContains: "Разрез", limit: 20 });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("ListViews");
    expect(h.requests[0]?.addOnParameters).toEqual({ include: ["viewpoints", "viewMap"], types: ["Section", "3DModel"], nameContains: "Разрез", limit: 20 });
  });

  it("sends {} by default and rejects unknown parts", async () => {
    const h = await route({});
    await h.call("list_views");
    expect(h.requests[0]?.addOnParameters).toEqual({});
    const bad = await h.call("list_views", { include: ["everything"] });
    expect(bad.isError).toBe(true);
    expect(h.requests).toHaveLength(1);
  });
});

describe("open_view", () => {
  it("opens a floor plan story", async () => {
    const h = await route({ OpenView: () => ({ opened: "FloorPlan", window: PLAN_WINDOW }) });
    const res = await h.call("open_view", { window: "FloorPlan", story: "1-й этаж" });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("OpenView");
    expect(h.requests[0]?.addOnParameters).toEqual({ window: "FloorPlan", story: "1-й этаж" });
    expect(res.json).toEqual({ opened: "FloorPlan", window: PLAN_WINDOW });
  });

  it("converts a marker element reference to a GUID string", async () => {
    const h = await route({});
    await h.call("open_view", { element: { guid: G1 }, segmentIndex: 2 });
    expect(h.requests[0]?.addOnParameters).toEqual({ element: G1, segmentIndex: 2 });
  });

  it("passes database, navigator item and projection through", async () => {
    const h = await route({});
    await h.call("open_view", { database: DB, window: "Layout" });
    await h.call("open_view", { navigatorItem: { name: "01 План" } });
    await h.call("open_view", { window: "3D", projection: "axonometric" });
    expect(h.requests.map((r) => r.addOnParameters)).toEqual([
      { database: DB, window: "Layout" },
      { navigatorItem: { name: "01 План" } },
      { window: "3D", projection: "axonometric" },
    ]);
  });

  it("rejects an empty request before calling Archicad", async () => {
    const h = await route({});
    const res = await h.call("open_view", {});
    expect(res.isError).toBe(true);
    expect(res.text).toContain("tell open_view what to open");
    expect(h.requests).toHaveLength(0);
  });

  it("rejects an unknown window type and a malformed database GUID", async () => {
    const h = await route({});
    expect((await h.call("open_view", { window: "Kitchen" })).isError).toBe(true);
    expect((await h.call("open_view", { database: "not-a-guid" })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("surfaces an ambiguous-name error from the add-on", async () => {
    const h = await route({ OpenView: () => ({ error: { code: -2130312310, message: "'A' matches several views: Section 'A-A' [..]; Section 'A-B' [..]. Pass 'database' (GUID) instead." } }) });
    const res = await h.call("open_view", { window: "Section", name: "A" });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("matches several views");
  });
});

describe("go_to_view", () => {
  it("accepts a GUID, a name or an object", async () => {
    const h = await route({ GoToView: () => ({ method: "goToView" }) });
    await h.call("go_to_view", { view: G1 });
    await h.call("go_to_view", { view: "01 План 1-го этажа" });
    await h.call("go_to_view", { view: { guid: G2 } });
    expect(h.requests.map((r) => [r.addOnCommand, r.addOnParameters])).toEqual([
      ["GoToView", { view: G1 }],
      ["GoToView", { view: "01 План 1-го этажа" }],
      ["GoToView", { view: { guid: G2 } }],
    ]);
  });

  it("requires a view", async () => {
    const h = await route({});
    const res = await h.call("go_to_view", {});
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("zoom", () => {
  it("defaults to fit", async () => {
    const h = await route({ Zoom: () => ({ mode: "fit", window: PLAN_WINDOW }) });
    const res = await h.call("zoom");
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("Zoom");
    expect(h.requests[0]?.addOnParameters).toEqual({ mode: "fit" });
  });

  it("infers box / elements modes and converts element refs", async () => {
    const h = await route({});
    await h.call("zoom", { box: { xMin: 0, yMin: 0, xMax: 10, yMax: 5 }, margin: 0.1 });
    await h.call("zoom", { elements: [G1, { guid: G2 }] });
    await h.call("zoom", { mode: "in", factor: 3, center: { x: 1, y: 2 } });
    await h.call("zoom", { mode: "previous", steps: 2 });
    expect(h.requests.map((r) => r.addOnParameters)).toEqual([
      { mode: "box", box: { xMin: 0, yMin: 0, xMax: 10, yMax: 5 }, margin: 0.1 },
      { mode: "elements", elements: [G1, G2] },
      { mode: "in", factor: 3, center: { x: 1, y: 2 } },
      { mode: "previous", steps: 2 },
    ]);
  });

  it("rejects box / elements modes without their data", async () => {
    const h = await route({});
    const a = await h.call("zoom", { mode: "box" });
    const b = await h.call("zoom", { mode: "elements" });
    const c = await h.call("zoom", { mode: "in", factor: 1 });
    expect(a.isError && b.isError && c.isError).toBe(true);
    expect(a.text).toContain("needs 'box'");
    expect(b.text).toContain("needs 'elements'");
    expect(h.requests).toHaveLength(0);
  });

  it("surfaces add-on errors", async () => {
    const h = await route({ Zoom: () => ({ error: { code: -2130313087, message: "Nothing is selected. Select elements first (set_selection) or use mode 'elements'." } }) });
    const res = await h.call("zoom", { mode: "selection" });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("Nothing is selected");
  });
});

describe("get_3d_view / set_3d_view", () => {
  it("get_3d_view calls Get3DView", async () => {
    const state = { projection: { mode: "axonometric", axonometric: { projection: "Isometric", azimuth: 45 } } };
    const h = await route({ Get3DView: () => state });
    const res = await h.call("get_3d_view");
    expect(h.requests[0]?.addOnCommand).toBe("Get3DView");
    expect(res.json).toEqual(state);
  });

  it("set_3d_view forwards a perspective camera", async () => {
    const h = await route({ Set3DView: () => ({ changed: ["projection"], camera: { verified: true } }) });
    const res = await h.call("set_3d_view", {
      mode: "perspective",
      camera: { x: -10, y: -10, z: 1.7 },
      target: { x: 5, y: 5, z: 1.5 },
      viewCone: 60,
      roll: 0,
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("Set3DView");
    expect(h.requests[0]?.addOnParameters).toEqual({
      mode: "perspective",
      camera: { x: -10, y: -10, z: 1.7 },
      target: { x: 5, y: 5, z: 1.5 },
      viewCone: 60,
      roll: 0,
    });
  });

  it("set_3d_view forwards orbit, axonometry, filters and window options", async () => {
    const h = await route({});
    await h.call("set_3d_view", { mode: "perspective", azimuth: 225, altitude: 30 });
    await h.call("set_3d_view", {
      mode: "axonometric",
      projection: "Isometric",
      azimuth: 30,
      style: "Простой",
      windowSize: { width: 1200, height: 800 },
      stories: { from: 0, to: "2-й этаж", trim: true },
      elementTypes: ["Wall", "Slab"],
      cutPlanes: { enabled: false },
      sun: { azimuth: 135, altitude: 40 },
      open3D: false,
    });
    expect(h.requests[0]?.addOnParameters).toEqual({ mode: "perspective", azimuth: 225, altitude: 30 });
    expect(h.requests[1]?.addOnParameters).toEqual({
      mode: "axonometric",
      projection: "Isometric",
      azimuth: 30,
      style: "Простой",
      windowSize: { width: 1200, height: 800 },
      stories: { from: 0, to: "2-й этаж", trim: true },
      elementTypes: ["Wall", "Slab"],
      cutPlanes: { enabled: false },
      sun: { azimuth: 135, altitude: 40 },
      open3D: false,
    });
  });

  it("set_3d_view forwards 3D style settings", async () => {
    const h = await route({ Set3DView: () => ({ changed: ["style", "styleSettings"] }) });
    const res = await h.call("set_3d_view", {
      style: "Архитектурный",
      styleSettings: { model: "HiddenLine", sunShadows: "AllSurfacesContoursOn", castShadowPercent: 60, skyColor: { r: 0.8, g: 0.9, b: 1 } },
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({
      style: "Архитектурный",
      styleSettings: { model: "HiddenLine", sunShadows: "AllSurfacesContoursOn", castShadowPercent: 60, skyColor: { r: 0.8, g: 0.9, b: 1 } },
    });
    const bad = await h.call("set_3d_view", { styleSettings: { model: "Toon" } });
    expect(bad.isError).toBe(true);
    const badColor = await h.call("set_3d_view", { styleSettings: { skyColor: { r: 255, g: 0, b: 0 } } });
    expect(badColor.isError).toBe(true);
    expect(h.requests).toHaveLength(1);
  });

  it("set_3d_view validates ranges before calling Archicad", async () => {
    const h = await route({});
    expect((await h.call("set_3d_view", { viewCone: 200 })).isError).toBe(true);
    expect((await h.call("set_3d_view", { altitude: 120 })).isError).toBe(true);
    expect((await h.call("set_3d_view", { projection: "Fisheye" })).isError).toBe(true);
    expect((await h.call("set_3d_view", { tranmat: [1, 0, 0] })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("show_in_3d", () => {
  it("defaults to no mode (add-on: all) and converts elements", async () => {
    const h = await route({ ShowIn3D: () => ({ mode: "all" }) });
    await h.call("show_in_3d");
    await h.call("show_in_3d", { elements: [G1, { guid: G2 }] });
    await h.call("show_in_3d", { mode: "selection" });
    expect(h.requests.map((r) => [r.addOnCommand, r.addOnParameters])).toEqual([
      ["ShowIn3D", {}],
      ["ShowIn3D", { elements: [G1, G2] }],
      ["ShowIn3D", { mode: "selection" }],
    ]);
  });

  it("mode 'elements' needs elements", async () => {
    const h = await route({});
    const res = await h.call("show_in_3d", { mode: "elements" });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("needs 'elements'");
    expect(h.requests).toHaveLength(0);
  });
});

// capture_view / render_view also query the window type and poll the 3D model size
// (GetCurrentWindow / Get3DModelStats) before capturing; the tests look at the other calls.
const core = <T extends { addOnCommand?: string }>(reqs: T[]): T[] =>
  reqs.filter((r) => r.addOnCommand !== "GetCurrentWindow" && r.addOnCommand !== "Get3DModelStats");

describe("capture_view", () => {
  it("captures the active window into a temp file and returns it as an image", async () => {
    const png = makePng(40, 20);
    let written = "";
    const h = await route({
      CaptureView: (req) => {
        written = String(req.addOnParameters?.["path"]);
        writeFileSync(written, png);
        return { path: written, format: "png", window: PLAN_WINDOW };
      },
    });
    const res = await h.call("capture_view");
    expect(res.isError).toBe(false);
    expect(core(h.requests)).toHaveLength(1);
    expect(core(h.requests)[0]?.addOnCommand).toBe("CaptureView");
    const params = core(h.requests)[0]?.addOnParameters ?? {};
    expect(params["format"]).toBe("png");
    expect(path.isAbsolute(String(params["path"]))).toBe(true);
    expect(String(params["path"])).toMatch(/capture-.*\.png$/);

    const img = imageOf(res.content as never);
    expect(img?.mimeType).toBe("image/png");
    expect(Buffer.from(img!.data, "base64").equals(png)).toBe(true);
    expect(res.json).toMatchObject({ window: PLAN_WINDOW, image: { width: 40, height: 20, format: "png", resized: false } });
    expect(existsSync(written)).toBe(false); // temp file removed
  });

  it("runs the preparation steps in order before capturing", async () => {
    const h = await route({
      GoToView: () => ({ method: "goToView", window: PLAN_WINDOW }),
      OpenView: () => ({ opened: "FloorPlan", window: PLAN_WINDOW }),
      Set3DView: () => ({ changed: ["projection"], camera: { verified: false, warning: "not exact" }, window: WINDOW_3D }),
      Zoom: () => ({ mode: "fit", window: WINDOW_3D }),
      CaptureView: (req) => {
        writeFileSync(String(req.addOnParameters?.["path"]), makePng(8, 8));
        return { window: WINDOW_3D };
      },
    });
    const res = await h.call("capture_view", {
      goToView: "Вид 1",
      view: { window: "FloorPlan", story: 1 },
      threeD: { mode: "perspective", azimuth: 200, altitude: 20 },
      zoom: { mode: "fit" },
      width: 1024,
      height: 768,
      keepSelectionHighlight: true,
      cropToWindow: false,
      format: "png",
    });
    expect(res.isError).toBe(false);
    expect(core(h.requests).map((r) => r.addOnCommand)).toEqual(["GoToView", "OpenView", "Set3DView", "Zoom", "CaptureView"]);
    expect(core(h.requests)[0]?.addOnParameters).toEqual({ view: "Вид 1" });
    expect(core(h.requests)[1]?.addOnParameters).toEqual({ window: "FloorPlan", story: 1 });
    expect(core(h.requests)[2]?.addOnParameters).toEqual({ mode: "perspective", azimuth: 200, altitude: 20, open3D: true });
    expect(core(h.requests)[3]?.addOnParameters).toEqual({ mode: "fit" });
    expect(core(h.requests)[4]?.addOnParameters).toMatchObject({ format: "png", width: 1024, height: 768, keepSelectionHighlight: true, cropToWindow: false });
    const meta = res.json as { preparation: Record<string, Record<string, unknown>>; window: unknown };
    expect(Object.keys(meta.preparation)).toEqual(["goToView", "openView", "set3DView", "zoom"]);
    expect(meta.preparation["set3DView"]).toEqual({ changed: ["projection"], camera: { verified: false, warning: "not exact" } });
    expect(meta.preparation["openView"]).not.toHaveProperty("window");
    expect(meta.window).toEqual(WINDOW_3D);
  });

  it("stops (and does not capture) when a preparation step fails", async () => {
    const h = await route({ OpenView: () => ({ error: { code: -2130312310, message: "No view named 'X'." } }) });
    const res = await h.call("capture_view", { view: { name: "X" } });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("No view named 'X'");
    expect(core(h.requests).map((r) => r.addOnCommand)).toEqual(["OpenView"]);
  });

  it("rejects an empty view preparation and bad zoom input before calling Archicad", async () => {
    const h = await route({});
    expect((await h.call("capture_view", { view: {} })).isError).toBe(true);
    expect((await h.call("capture_view", { zoom: { mode: "box" } })).isError).toBe(true);
    expect((await h.call("capture_view", { maxSize: 10 })).isError).toBe(true);
    expect(core(h.requests)).toHaveLength(0);
  });

  it("reports a missing picture file", async () => {
    const h = await route({ CaptureView: () => ({ window: PLAN_WINDOW }) });
    const res = await h.call("capture_view");
    expect(res.isError).toBe(true);
    expect(res.text).toContain("did not write the picture file");
  });

  it("surfaces add-on errors (e.g. unsupported window)", async () => {
    const h = await route({
      CaptureView: () => ({ error: { code: -2130313111, message: "The active window (Report) cannot be saved as a picture. Open a floor plan ... first (open_view)." } }),
    });
    const res = await h.call("capture_view");
    expect(res.isError).toBe(true);
    expect(res.text).toContain("cannot be saved as a picture");
  });

  it("keeps a copy at saveTo", async () => {
    const png = makePng(12, 6);
    const dest = path.join(scratch, "sub", "plan.png");
    const h = await route({
      CaptureView: (req) => {
        writeFileSync(String(req.addOnParameters?.["path"]), png);
        return { window: PLAN_WINDOW };
      },
    });
    const res = await h.call("capture_view", { saveTo: dest });
    expect(res.isError).toBe(false);
    expect((await fsp.readFile(dest)).equals(png)).toBe(true);
    expect(res.json).toMatchObject({ savedTo: dest });
  });

  it("rejects a relative saveTo", async () => {
    const h = await route({
      CaptureView: (req) => {
        writeFileSync(String(req.addOnParameters?.["path"]), makePng(4, 4));
        return { window: PLAN_WINDOW };
      },
    });
    const res = await h.call("capture_view", { saveTo: "relative/plan.png" });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("absolute path");
  });

  it.skipIf(process.platform !== "darwin")("downscales large pictures to maxSize (macOS sips)", async () => {
    const h = await route({
      CaptureView: (req) => {
        writeFileSync(String(req.addOnParameters?.["path"]), makePng(400, 200));
        return { window: PLAN_WINDOW };
      },
    });
    const res = await h.call("capture_view", { maxSize: 100 });
    expect(res.isError).toBe(false);
    const img = imageOf(res.content as never)!;
    const size = sniffImageSize(Buffer.from(img.data, "base64"));
    expect(size).toEqual({ width: 100, height: 50, format: "png" });
    expect(res.json).toMatchObject({ image: { width: 100, height: 50, resized: true, originalWidth: 400, originalHeight: 200 } });
  });

  it.skipIf(process.platform !== "darwin")("uses width/height as the image bounds for 2D windows", async () => {
    const h = await route({
      CaptureView: (req) => {
        writeFileSync(String(req.addOnParameters?.["path"]), makePng(300, 100));
        return { window: PLAN_WINDOW };
      },
    });
    const res = await h.call("capture_view", { width: 150 });
    expect(res.isError).toBe(false);
    expect(core(h.requests)[0]?.addOnParameters).toMatchObject({ width: 150 });
    expect(res.json).toMatchObject({ image: { width: 150, height: 50, resized: true } });
  });

  it("does not shrink 3D captures to width/height (they size the 3D window instead)", async () => {
    const h = await route({
      CaptureView: (req) => {
        writeFileSync(String(req.addOnParameters?.["path"]), makePng(64, 32));
        return { window: WINDOW_3D };
      },
    });
    const res = await h.call("capture_view", { width: 32, height: 16 });
    expect(res.isError).toBe(false);
    expect(res.json).toMatchObject({ image: { width: 64, height: 32, resized: false } });
  });
});

describe("render_view", () => {
  it("renders to a temp JPEG (default) with scene and size and returns the image", async () => {
    const jpeg = makeJpegHeader(320, 200);
    let written = "";
    const h = await route({
      Set3DView: () => ({ changed: ["projection"], camera: { verified: true }, window: WINDOW_3D }),
      RenderView: (req) => {
        written = String(req.addOnParameters?.["path"]);
        writeFileSync(written, jpeg);
        return { path: written, format: "jpeg", scene: "Внешний - Быстрый", imageSize: { width: 320, height: 200 } };
      },
    });
    const res = await h.call("render_view", {
      threeD: { mode: "perspective", azimuth: 210, altitude: 15 },
      scene: "Внешний - Быстрый",
      width: 320,
    });
    expect(res.isError).toBe(false);
    expect(core(h.requests).map((r) => r.addOnCommand)).toEqual(["Set3DView", "RenderView"]);
    expect(core(h.requests)[0]?.addOnParameters).toEqual({ mode: "perspective", azimuth: 210, altitude: 15, open3D: true });
    const params = core(h.requests)[1]?.addOnParameters ?? {};
    expect(params).toMatchObject({ format: "jpeg", scene: "Внешний - Быстрый", width: 320 });
    expect(String(params["path"])).toMatch(/render-.*\.jpg$/);
    const img = imageOf(res.content as never);
    expect(img?.mimeType).toBe("image/jpeg");
    expect(res.json).toMatchObject({ scene: "Внешний - Быстрый", renderSize: { width: 320, height: 200 }, image: { width: 320, height: 200, format: "jpeg" } });
    expect((res.json as { preparation: Record<string, unknown> }).preparation).toEqual({ set3DView: { changed: ["projection"], camera: { verified: true } } });
    expect(existsSync(written)).toBe(false);
  });

  it("detects the real format of the written file", async () => {
    const h = await route({
      RenderView: (req) => {
        writeFileSync(String(req.addOnParameters?.["path"]), makePng(10, 10));
        return {};
      },
    });
    const res = await h.call("render_view", { format: "png" });
    expect(res.isError).toBe(false);
    expect(imageOf(res.content as never)?.mimeType).toBe("image/png");
    expect(core(h.requests)[0]?.addOnParameters).toMatchObject({ format: "png" });
  });

  it("surfaces a missing rendering engine", async () => {
    const h = await route({
      RenderView: () => ({ error: { code: -2130313085, message: "Photo rendering is not available in this Archicad (rendering engine missing)" } }),
    });
    const res = await h.call("render_view");
    expect(res.isError).toBe(true);
    expect(res.text).toContain("Photo rendering is not available");
  });

  it("validates sizes and timeout", async () => {
    const h = await route({});
    expect((await h.call("render_view", { width: 5 })).isError).toBe(true);
    expect((await h.call("render_view", { timeoutSeconds: 1 })).isError).toBe(true);
    expect(core(h.requests)).toHaveLength(0);
  });
});

describe("marker creation", () => {
  const created = (type: string, n = 1) => ({ results: Array.from({ length: n }, (_, i) => ({ guid: i === 0 ? G1 : G2, type })) });

  it("create_sections sends CutPlane specs and merges the viewpoint details", async () => {
    const h = await route({
      CreateElements: () => created("CutPlane"),
      GetElementDetails: () => ({
        elements: [
          {
            guid: G1,
            type: "CutPlane",
            details: {
              name: "Разрез 1-1",
              referenceId: "1",
              database: DB,
              hasViewpoint: true,
              begin: { x: 0, y: 5 },
              end: { x: 10, y: 5 },
              viewSide: "left",
              depth: 8,
              horizontalRange: "Limited",
              markedDistantArea: false,
            },
          },
        ],
      }),
    });
    const res = await h.call("create_sections", {
      sections: [
        {
          begin: { x: 0, y: 5 },
          end: { x: 10, y: 5 },
          viewSide: "left",
          depth: 8,
          verticalRange: { min: -1, max: 9 },
          name: "Разрез 1-1",
          referenceId: "1",
          layer: "Маркеры",
          storyIndex: 0,
        },
      ],
    });
    expect(res.isError).toBe(false);
    expect(h.requests.map((r) => r.addOnCommand)).toEqual(["CreateElements", "GetElementDetails"]);
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [
        {
          type: "CutPlane",
          begin: { x: 0, y: 5 },
          end: { x: 10, y: 5 },
          viewSide: "left",
          depth: 8,
          verticalRange: { min: -1, max: 9 },
          name: "Разрез 1-1",
          referenceId: "1",
          layer: "Маркеры",
          storyIndex: 0,
        },
      ],
      undoName: "Create sections (Claude)",
    });
    expect(h.requests[1]?.addOnParameters).toEqual({ elements: [G1] });
    const out = res.json as { results: Array<Record<string, unknown>>; hint: string };
    expect(out.results[0]).toEqual({
      guid: G1,
      type: "CutPlane",
      name: "Разрез 1-1",
      referenceId: "1",
      database: DB,
      hasViewpoint: true,
      begin: { x: 0, y: 5 },
      end: { x: 10, y: 5 },
      viewSide: "left",
      depth: 8,
      horizontalRange: "Limited",
    });
    expect(out.hint).toContain("open_view");
  });

  it("keeps per-item errors and skips details when nothing was created", async () => {
    const h = await route({ CreateElements: () => ({ results: [{ error: { code: -1, message: "begin and end must be at least 1 cm apart." } }] }) });
    const res = await h.call("create_elevations", { elevations: [{ begin: { x: 0, y: 0 }, end: { x: 0, y: 0 } }], undoName: "Фасады" });
    expect(res.isError).toBe(false);
    expect(h.requests.map((r) => r.addOnCommand)).toEqual(["CreateElements"]);
    expect(h.requests[0]?.addOnParameters).toEqual({ elements: [{ type: "Elevation", begin: { x: 0, y: 0 }, end: { x: 0, y: 0 } }], undoName: "Фасады" });
    expect(res.json).toEqual({ results: [{ error: { code: -1, message: "begin and end must be at least 1 cm apart." } }] });
  });

  it("still returns the created GUIDs when the details cannot be read", async () => {
    const h = await route({
      CreateElements: () => created("Elevation"),
      GetElementDetails: () => ({ error: { code: -1, message: "boom" } }),
    });
    const res = await h.call("create_elevations", { elevations: [{ begin: { x: -5, y: -5 }, end: { x: 20, y: -5 } }] });
    expect(res.isError).toBe(false);
    expect(res.json).toMatchObject({ results: [{ guid: G1, type: "Elevation" }], note: expect.stringContaining("get_element_details") });
  });

  it("create_sections requires begin and end", async () => {
    const h = await route({});
    const res = await h.call("create_sections", { sections: [{ begin: { x: 0, y: 0 } }] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("create_interior_elevations sends points and returns the segments", async () => {
    const segments = [
      { index: 0, name: "Кухня 1", database: DB, hasViewpoint: true },
      { index: 1, name: "Кухня 2", hasViewpoint: false },
    ];
    const h = await route({
      CreateElements: () => created("InteriorElevation"),
      GetElementDetails: () => ({ elements: [{ guid: G1, details: { name: "Кухня", segments, segmentCount: 2 } }] }),
    });
    const res = await h.call("create_interior_elevations", {
      interiorElevations: [
        {
          points: [
            { x: 0.1, y: 0.1 },
            { x: 3.9, y: 0.1 },
            { x: 3.9, y: 2.9 },
          ],
          closed: false,
          depth: 1.5,
          name: "Кухня",
        },
      ],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [
        {
          type: "InteriorElevation",
          points: [
            { x: 0.1, y: 0.1 },
            { x: 3.9, y: 0.1 },
            { x: 3.9, y: 2.9 },
          ],
          closed: false,
          depth: 1.5,
          name: "Кухня",
        },
      ],
      undoName: "Create interior elevations (Claude)",
    });
    expect((res.json as { results: unknown[] }).results[0]).toEqual({ guid: G1, type: "InteriorElevation", name: "Кухня", segments });
  });

  it("create_interior_elevations needs at least 2 points", async () => {
    const h = await route({});
    const res = await h.call("create_interior_elevations", { interiorElevations: [{ points: [{ x: 0, y: 0 }] }] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("create_details accepts a box or a polygon", async () => {
    const h = await route({ CreateElements: () => created("Detail", 2), GetElementDetails: () => ({ elements: [] }) });
    const res = await h.call("create_details", {
      details: [
        { box: { xMin: 0, yMin: 0, xMax: 2, yMax: 1 }, name: "Узел 1", referenceId: "У1", markerPosition: { x: 3, y: 2 } },
        {
          polygon: [
            { x: 5, y: 0 },
            { x: 7, y: 0 },
            { x: 6, y: 2 },
          ],
          markerAngle: 30,
          horizontalMarker: true,
        },
      ],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [
        { type: "Detail", box: { xMin: 0, yMin: 0, xMax: 2, yMax: 1 }, name: "Узел 1", referenceId: "У1", markerPosition: { x: 3, y: 2 } },
        {
          type: "Detail",
          polygon: [
            { x: 5, y: 0 },
            { x: 7, y: 0 },
            { x: 6, y: 2 },
          ],
          markerAngle: 30,
          horizontalMarker: true,
        },
      ],
      undoName: "Create details (Claude)",
    });
    expect(h.requests[1]?.addOnParameters).toEqual({ elements: [G1, G2] });
  });

  it("create_details / create_worksheets need exactly one of polygon or box", async () => {
    const h = await route({});
    const none = await h.call("create_details", { details: [{ name: "X" }] });
    expect(none.isError).toBe(true);
    expect(none.text).toContain("details[0]: give 'polygon' or 'box'");
    const both = await h.call("create_worksheets", {
      worksheets: [
        {
          box: { xMin: 0, yMin: 0, xMax: 1, yMax: 1 },
          polygon: [
            { x: 0, y: 0 },
            { x: 1, y: 0 },
            { x: 1, y: 1 },
          ],
        },
      ],
    });
    expect(both.isError).toBe(true);
    expect(both.text).toContain("worksheets[0]: give either 'polygon' or 'box', not both");
    expect(h.requests).toHaveLength(0);
  });

  it("create_worksheets sends Worksheet specs", async () => {
    const h = await route({ CreateElements: () => created("Worksheet"), GetElementDetails: () => ({ elements: [{ guid: G1, details: { name: "Лист", database: DB } }] }) });
    const res = await h.call("create_worksheets", { worksheets: [{ box: { xMin: 0, yMin: 0, xMax: 4, yMax: 3 }, name: "Лист" }], undoName: "WS" });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({ elements: [{ type: "Worksheet", box: { xMin: 0, yMin: 0, xMax: 4, yMax: 3 }, name: "Лист" }], undoName: "WS" });
    expect((res.json as { results: unknown[] }).results[0]).toEqual({ guid: G1, type: "Worksheet", name: "Лист", database: DB });
  });
});

describe("get_view_settings / set_view_settings", () => {
  it("reads the current window or a saved view", async () => {
    const h = await route({ GetViewSettings: () => ({ target: "currentWindow", drawingScale: 100 }) });
    const cur = await h.call("get_view_settings");
    await h.call("get_view_settings", { view: { name: "01 План" } });
    expect(cur.json).toEqual({ target: "currentWindow", drawingScale: 100 });
    expect(h.requests.map((r) => [r.addOnCommand, r.addOnParameters])).toEqual([
      ["GetViewSettings", {}],
      ["GetViewSettings", { view: { name: "01 План" } }],
    ]);
  });

  it("changes current-window settings", async () => {
    const h = await route({ SetViewSettings: () => ({ changed: ["drawingScale", "layerCombination"] }) });
    const res = await h.call("set_view_settings", { drawingScale: 50, layerCombination: "Архитектурный план", structureDisplay: "CoreOnly" });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("SetViewSettings");
    expect(h.requests[0]?.addOnParameters).toEqual({ drawingScale: 50, layerCombination: "Архитектурный план", structureDisplay: "CoreOnly" });
  });

  it("changes a saved view", async () => {
    const h = await route({});
    await h.call("set_view_settings", {
      view: G1,
      penSet: { index: 2 },
      dimensionStyle: "Метрические",
      graphicOverrides: "Нет переопределений",
      zoom: { xMin: -1, yMin: -1, xMax: 20, yMax: 15 },
      ignoreSavedZoom: false,
      renderingScene: "Внешний",
      modelViewOptions: 3,
    });
    expect(h.requests[0]?.addOnParameters).toEqual({
      view: G1,
      penSet: { index: 2 },
      dimensionStyle: "Метрические",
      graphicOverrides: "Нет переопределений",
      zoom: { xMin: -1, yMin: -1, xMax: 20, yMax: 15 },
      ignoreSavedZoom: false,
      renderingScene: "Внешний",
      modelViewOptions: 3,
    });
  });

  it("rejects requests with nothing to change", async () => {
    const h = await route({});
    const a = await h.call("set_view_settings", {});
    const b = await h.call("set_view_settings", { view: G1 });
    const c = await h.call("set_view_settings", { drawingScale: 0 });
    const d = await h.call("set_view_settings", { structureDisplay: "Everything" });
    expect(a.isError && b.isError && c.isError && d.isError).toBe(true);
    expect(a.text).toContain("nothing to change");
    expect(h.requests).toHaveLength(0);
  });

  it("surfaces add-on errors", async () => {
    const h = await route({
      SetViewSettings: () => ({ error: { code: -1, message: "'penSet' can only be stored in a saved view: pass 'view' (a View Map item from list_views)." } }),
    });
    const res = await h.call("set_view_settings", { penSet: 1 });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("can only be stored in a saved view");
  });
});

describe("3D capture waits for the model", () => {
  it("polls Get3DModelStats until stable before capturing a 3D window", async () => {
    let polls = 0;
    const png = makePng(10, 10);
    const h = await route({
      GetCurrentWindow: () => ({ type: "3DModel" }),
      Get3DModelStats: () => {
        polls++;
        return { bodies: polls < 3 ? polls * 10 : 30, polygons: 1, vertices: 1 };
      },
      CaptureView: (req) => {
        writeFileSync(String(req.addOnParameters?.["path"]), png);
        return { path: String(req.addOnParameters?.["path"]), format: "png" };
      },
    });
    const res = await h.call("capture_view");
    expect(res.isError).toBe(false);
    expect(polls).toBeGreaterThanOrEqual(4);
    const order = h.requests.map((r) => r.addOnCommand);
    expect(order.lastIndexOf("Get3DModelStats")).toBeLessThan(order.indexOf("CaptureView"));
  });
});
