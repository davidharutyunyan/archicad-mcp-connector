import { chmod, mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import { existsSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { afterAll, beforeAll, beforeEach, describe, expect, it, vi } from "vitest";

import { addonOk, harness, type RecordedRequest } from "../helpers.js";

// The DWG converter lookup is mocked so that the tests do not depend on what is installed on this machine.
const dwgMocks = vi.hoisted(() => ({ converter: undefined as string | undefined }));
vi.mock("../../src/tools/documentation-dwg.js", async (importOriginal) => {
  const orig = await importOriginal<typeof import("../../src/tools/documentation-dwg.js")>();
  return { ...orig, findOdaConverter: () => dwgMocks.converter };
});

const dwg = await import("../../src/tools/documentation-dwg.js");

const G1 = "92F5E7F6-2460-FF4F-A499-0B07286D2B5F";
const G2 = "0A1B2C3D-4E5F-6071-8293-A4B5C6D7E8F9";
const LAYOUT = "B167A39F-7537-4CE1-893C-ED75701103CF";

const addonCalls = (requests: RecordedRequest[]) => requests.map((r) => r.addOnCommand);

let scratch = "";
beforeAll(async () => {
  scratch = await mkdtemp(join(tmpdir(), "doc-test-"));
});
afterAll(async () => {
  await rm(scratch, { recursive: true, force: true });
});
beforeEach(() => {
  dwgMocks.converter = undefined;
});

describe("documentation tool registration", () => {
  it("registers every documentation tool with a description and input schema", async () => {
    const h = await harness(() => addonOk({}));
    const tools = await h.listTools();
    const names = tools.map((t) => t.name);
    for (const name of [
      "get_databases",
      "get_layout_drawings",
      "place_drawing",
      "modify_drawings",
      "delete_drawings",
      "update_drawings",
      "publish_publisher_set",
      "get_ifc_translators",
      "export_ifc",
      "export_dwg",
      "export_pdf",
      "export_3d_model",
      "export_module",
      "merge_file",
      "get_hotlinks",
      "place_hotlink",
      "update_hotlinks",
      "delete_hotlinks",
    ]) {
      expect(names).toContain(name);
      const tool = tools.find((t) => t.name === name)!;
      expect(tool.description?.length ?? 0).toBeGreaterThan(80);
    }
    // read-only annotations on the listing tools
    expect(tools.find((t) => t.name === "get_databases")?.annotations?.readOnlyHint).toBe(true);
    expect(tools.find((t) => t.name === "delete_hotlinks")?.annotations?.destructiveHint).toBe(true);
  });
});

describe("get_databases / get_layout_drawings", () => {
  it("forwards the filters to GetDatabases", async () => {
    const h = await harness(() => addonOk({ databases: [], count: 0 }));
    const res = await h.call("get_databases", { types: ["Layout", "Section"], search: "План", includeViews: true });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("GetDatabases");
    expect(h.requests[0]?.addOnParameters).toEqual({ types: ["Layout", "Section"], search: "План", includeViews: true });
  });

  it("rejects unknown database types before calling Archicad", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("get_databases", { types: ["Model"] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("forwards layouts to GetLayoutDrawings", async () => {
    const h = await harness(() => addonOk({ layouts: [], layoutCount: 0, drawingCount: 0 }));
    await h.call("get_layout_drawings", { layouts: [LAYOUT, "План на отм. ±0.000"], includeFrame: false });
    expect(h.requests[0]?.addOnCommand).toBe("GetLayoutDrawings");
    expect(h.requests[0]?.addOnParameters).toEqual({ layouts: [LAYOUT, "План на отм. ±0.000"], includeFrame: false });
  });
});

describe("place_drawing / modify_drawings / delete_drawings", () => {
  it("sends the drawing specs to PlaceDrawings", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G1 }] }));
    const res = await h.call("place_drawing", {
      drawings: [
        {
          layout: LAYOUT,
          view: G2,
          position: { x: 0.21, y: 0.1485 },
          anchor: "LeftBottom",
          scale: 50,
          name: "План 1 этажа",
          title: false,
          frame: [{ x: 0.02, y: 0.02 }, { x: 0.3, y: 0.02 }, { x: 0.3, y: 0.2 }],
          border: { show: true, pen: 3 },
        },
        { layout: "Фасады", database: "FloorPlan", storyIndex: 1, ratio: 2 },
      ],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("PlaceDrawings");
    expect(h.requests[0]?.addOnParameters).toEqual({
      drawings: [
        {
          layout: LAYOUT,
          view: G2,
          position: { x: 0.21, y: 0.1485 },
          anchor: "LeftBottom",
          scale: 50,
          name: "План 1 этажа",
          title: false,
          frame: [{ x: 0.02, y: 0.02 }, { x: 0.3, y: 0.02 }, { x: 0.3, y: 0.2 }],
          border: { show: true, pen: 3 },
        },
        { layout: "Фасады", database: "FloorPlan", storyIndex: 1, ratio: 2 },
      ],
    });
    expect(res.json).toEqual({ results: [{ guid: G1 }] });
  });

  it("requires exactly one source per drawing", async () => {
    const h = await harness(() => addonOk({}));
    const none = await h.call("place_drawing", { drawings: [{ layout: LAYOUT }] });
    expect(none.isError).toBe(true);
    expect(none.text).toContain("exactly one source");
    const both = await h.call("place_drawing", { drawings: [{ layout: LAYOUT, view: G2, database: "FloorPlan" }] });
    expect(both.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("rejects scale together with ratio", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("place_drawing", { drawings: [{ layout: LAYOUT, view: G2, scale: 100, ratio: 1 }] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("scale or ratio");
    expect(h.requests).toHaveLength(0);
  });

  it("rejects an invalid anchor value", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("place_drawing", { drawings: [{ layout: LAYOUT, view: G2, anchor: "Middle" }] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("modify_drawings forwards the changes", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G1 }] }));
    await h.call("modify_drawings", { drawings: [{ guid: G1, position: { x: 0.1, y: 0.1 }, angle: 90, frame: false, penTable: "Model" }] });
    expect(h.requests[0]?.addOnCommand).toBe("ModifyDrawings");
    expect(h.requests[0]?.addOnParameters).toEqual({
      drawings: [{ guid: G1, position: { x: 0.1, y: 0.1 }, angle: 90, frame: false, penTable: "Model" }],
    });
  });

  it("delete_drawings normalizes {guid} references", async () => {
    const h = await harness(() => addonOk({ results: [] }));
    await h.call("delete_drawings", { drawings: [G1, { guid: G2 }] });
    expect(h.requests[0]?.addOnCommand).toBe("DeleteDrawings");
    expect(h.requests[0]?.addOnParameters).toEqual({ drawings: [G1, G2] });
  });
});

describe("update_drawings", () => {
  it("sends drawing guids", async () => {
    const h = await harness(() => addonOk({ results: [], drawingCount: 1, updatedCount: 1 }));
    await h.call("update_drawings", { drawings: [{ guid: G1 }], includeManual: false });
    expect(h.requests[0]?.addOnCommand).toBe("UpdateDrawings");
    expect(h.requests[0]?.addOnParameters).toEqual({ drawings: [G1], includeManual: false });
  });

  it("supports all: true", async () => {
    const h = await harness(() => addonOk({ results: [] }));
    await h.call("update_drawings", { all: true });
    expect(h.requests[0]?.addOnParameters).toEqual({ all: true });
  });

  it("requires exactly one selection mode", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("update_drawings", {})).isError).toBe(true);
    expect((await h.call("update_drawings", { all: true, layouts: [LAYOUT] })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("publish_publisher_set", () => {
  it("publishes by name with an output folder", async () => {
    const h = await harness(() => addonOk({ published: true, files: [], fileCount: 0 }));
    const res = await h.call("publish_publisher_set", {
      name: "2 - Макеты",
      outputPath: "/private/tmp/claude-connector-tests/publish",
      createFolders: true,
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("PublishPublisherSet");
    expect(h.requests[0]?.addOnParameters).toEqual({
      name: "2 - Макеты",
      outputPath: "/private/tmp/claude-connector-tests/publish",
      createFolders: true,
    });
  });

  it("needs name or index, not both", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("publish_publisher_set", {})).isError).toBe(true);
    expect((await h.call("publish_publisher_set", { name: "x", index: 0 })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("surfaces add-on errors (e.g. unknown set) as tool errors", async () => {
    const h = await harness(() => addonOk({ error: { code: -2130313111, message: "No publisher set named 'X'. Available: '1 - Виды'" } }));
    const res = await h.call("publish_publisher_set", { name: "X" });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("No publisher set named 'X'");
  });
});

describe("IFC", () => {
  it("get_ifc_translators calls ListIfcTranslators", async () => {
    const h = await harness(() => addonOk({ translators: [{ name: "Общий перевод", index: 0, isDefault: true }] }));
    const res = await h.call("get_ifc_translators");
    expect(h.requests[0]?.addOnCommand).toBe("ListIfcTranslators");
    expect(res.json).toEqual({ translators: [{ name: "Общий перевод", index: 0, isDefault: true }] });
  });

  it("export_ifc forwards options and normalizes element refs", async () => {
    const h = await harness(() => addonOk({ file: { path: "/private/tmp/claude-connector-tests/m.ifc", exists: true, sizeBytes: 10 } }));
    await h.call("export_ifc", {
      path: "/private/tmp/claude-connector-tests/m.ifc",
      translator: "Coordination",
      elements: [G1, { guid: G2 }],
      overwrite: true,
    });
    expect(h.requests[0]?.addOnCommand).toBe("ExportIfc");
    expect(h.requests[0]?.addOnParameters).toEqual({
      path: "/private/tmp/claude-connector-tests/m.ifc",
      translator: "Coordination",
      elements: [G1, G2],
      overwrite: true,
    });
  });

  it("rejects an invalid scope", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("export_ifc", { path: "/tmp/a.ifc", scope: "Everything" });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("export_dwg", () => {
  it("writes DXF through ExportDxf", async () => {
    const h = await harness(() => addonOk({ file: { path: "/private/tmp/claude-connector-tests/plan.dxf", exists: true, sizeBytes: 1000 } }));
    const res = await h.call("export_dwg", {
      path: "/private/tmp/claude-connector-tests/plan.dxf",
      storyIndex: 0,
      units: "mm",
      fillPatterns: false,
      createFolders: true,
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("ExportDxf");
    expect(h.requests[0]?.addOnParameters).toEqual({
      path: "/private/tmp/claude-connector-tests/plan.dxf",
      storyIndex: 0,
      units: "mm",
      fillPatterns: false,
      createFolders: true,
    });
  });

  it("rejects several targets", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("export_dwg", { path: "/tmp/a.dxf", layout: LAYOUT, storyIndex: 1 });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("only one of");
    expect(h.requests).toHaveLength(0);
  });

  it("rejects a .dwg path with format dxf", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("export_dwg", { path: "/tmp/a.dwg", format: "dxf" });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("explains how to get DWG when no converter is installed", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("export_dwg", { path: join(scratch, "plan.dwg") });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("ODA File Converter");
    expect(res.text).toContain("publish_publisher_set");
    expect(h.requests).toHaveLength(0);
  });

  it("converts the DXF to DWG with the converter and cleans up", async () => {
    // Fake ODA File Converter: <in> <out> <version> <type> <recurse> <audit> <filter> -> copies <in>/<filter> to <out>/<name>.dwg
    const fake = join(scratch, "fake-oda.sh");
    await writeFile(fake, '#!/bin/sh\nname=$(basename "$7" .dxf)\ncp "$1/$7" "$2/$name.dwg"\n');
    await chmod(fake, 0o755);
    dwgMocks.converter = fake;

    let dxfPath = "";
    const h = await harness((req) => {
      if (req.addOnCommand === "ExportDxf") {
        dxfPath = String(req.addOnParameters?.["path"]);
        writeFileSync(dxfPath, "0\nEOF\n"); // emulate the add-on writing the DXF
        return addonOk({ file: { path: dxfPath, exists: true, sizeBytes: 6 }, entityCount: 0, layerCount: 1 });
      }
      return addonOk({});
    });
    const target = join(scratch, "out", "plan.dwg");
    const res = await h.call("export_dwg", { path: target, layout: LAYOUT, createFolders: true, dwgVersion: "ACAD2013" });
    expect(res.isError).toBe(false);
    expect(addonCalls(h.requests)).toEqual(["ExportDxf"]);
    expect(h.requests[0]?.addOnParameters).toMatchObject({ layout: LAYOUT, overwrite: true, createFolders: false });
    expect(dxfPath.endsWith("/in/plan.dxf")).toBe(true);
    expect(await readFile(target, "utf8")).toBe("0\nEOF\n");
    expect(res.json).toMatchObject({ file: { path: target, exists: true, sizeBytes: 6 }, entityCount: 0, layerCount: 1 });
    expect((res.json as { format: string }).format).toContain("ACAD2013");
    // the temporary work folder is removed
    expect(existsSync(dxfPath)).toBe(false);
  });

  it("refuses to overwrite an existing .dwg before exporting", async () => {
    dwgMocks.converter = "/bin/true";
    const existing = join(scratch, "existing-target.dwg");
    await writeFile(existing, "old");
    const h = await harness(() => addonOk({}));
    const res = await h.call("export_dwg", { path: existing });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("overwrite");
    expect(h.requests).toHaveLength(0);
  });
});

describe("documentation-dwg helpers", () => {
  it("dxfNameFor sanitizes the base name", () => {
    expect(dwg.dxfNameFor("/a/b/План этажа.dwg")).toMatch(/\.dxf$/);
    expect(dwg.dxfNameFor("/a/b/plan-01.dwg")).toBe("plan-01.dxf");
  });

  it("odaConverterCandidates puts ODA_FILE_CONVERTER first", () => {
    const list = dwg.odaConverterCandidates({ ODA_FILE_CONVERTER: "/opt/oda/ODAFileConverter", HOME: "/Users/x" });
    expect(list[0]).toBe("/opt/oda/ODAFileConverter");
    expect(list).toContain("/Applications/ODAFileConverter.app/Contents/MacOS/ODAFileConverter");
  });

  it("checkDwgTarget validates path, extension, overwrite and folders", async () => {
    await expect(dwg.checkDwgTarget("relative.dwg", false, false)).rejects.toThrow("absolute");
    await expect(dwg.checkDwgTarget(join(scratch, "x.dxf"), false, false)).rejects.toThrow(".dwg");
    const existing = join(scratch, "exists.dwg");
    await writeFile(existing, "x");
    await expect(dwg.checkDwgTarget(existing, false, false)).rejects.toThrow("overwrite");
    await expect(dwg.checkDwgTarget(existing, true, false)).resolves.toBeUndefined();
    const nested = join(scratch, "new-folder", "deep", "a.dwg");
    await expect(dwg.checkDwgTarget(nested, false, false)).rejects.toThrow("createFolders");
    await dwg.checkDwgTarget(nested, false, true);
    expect(existsSync(join(scratch, "new-folder", "deep"))).toBe(true);
  });

  it("convertDxfToDwg runs the converter and copies the result", async () => {
    const fake = join(scratch, "fake-oda2.sh");
    await writeFile(fake, '#!/bin/sh\n[ "$3" = "ACAD2013" ] || exit 3\nname=$(basename "$7" .dxf)\ncp "$1/$7" "$2/$name.dwg"\n');
    await chmod(fake, 0o755);
    const work = await dwg.makeDwgWorkFolder();
    await writeFile(join(work.input, "plan.dxf"), "0\nEOF\n");
    const target = join(scratch, "converted.dwg");
    const { sizeBytes } = await dwg.convertDxfToDwg(fake, work.input, work.output, "plan.dxf", target, "ACAD2013");
    expect(sizeBytes).toBe(6);
    expect(await readFile(target, "utf8")).toBe("0\nEOF\n");
    await dwg.removeFolderQuietly(work.root);
    expect(existsSync(work.root)).toBe(false);
  });

  it("convertDxfToDwg reports converter failures and missing output", async () => {
    const failing = join(scratch, "fail-oda.sh");
    await writeFile(failing, "#!/bin/sh\necho boom >&2\nexit 2\n");
    await chmod(failing, 0o755);
    const noop = join(scratch, "noop-oda.sh");
    await writeFile(noop, "#!/bin/sh\nexit 0\n");
    await chmod(noop, 0o755);
    const work = await dwg.makeDwgWorkFolder();
    await mkdir(work.output, { recursive: true });
    await expect(dwg.convertDxfToDwg(failing, work.input, work.output, "a.dxf", join(scratch, "f.dwg"), "ACAD2018")).rejects.toThrow("boom");
    await expect(dwg.convertDxfToDwg(noop, work.input, work.output, "a.dxf", join(scratch, "f.dwg"), "ACAD2018")).rejects.toThrow("no .dwg");
    await dwg.removeFolderQuietly(work.root);
  });
});

describe("export_pdf", () => {
  it("exports one layout", async () => {
    const h = await harness(() => addonOk({ file: { path: "/private/tmp/claude-connector-tests/l.pdf", exists: true, sizeBytes: 5 } }));
    await h.call("export_pdf", { path: "/private/tmp/claude-connector-tests/l.pdf", layout: LAYOUT, overwrite: true });
    expect(h.requests[0]?.addOnCommand).toBe("ExportPdf");
    expect(h.requests[0]?.addOnParameters).toEqual({ path: "/private/tmp/claude-connector-tests/l.pdf", layout: LAYOUT, overwrite: true });
  });

  it("supports batches with common options", async () => {
    const h = await harness(() => addonOk({ results: [] }));
    await h.call("export_pdf", {
      exports: [
        { path: "/tmp/a.pdf", storyIndex: 0 },
        { path: "/tmp/b.pdf", view: G2, paper: { width: 0.297, height: 0.21, margin: 0.005 } },
      ],
      overwrite: true,
    });
    expect(h.requests[0]?.addOnParameters).toEqual({
      exports: [
        { path: "/tmp/a.pdf", storyIndex: 0 },
        { path: "/tmp/b.pdf", view: G2, paper: { width: 0.297, height: 0.21, margin: 0.005 } },
      ],
      overwrite: true,
    });
  });

  it("needs path xor exports and one target per item", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("export_pdf", {})).isError).toBe(true);
    expect((await h.call("export_pdf", { path: "/tmp/a.pdf", exports: [{ path: "/tmp/b.pdf" }] })).isError).toBe(true);
    expect((await h.call("export_pdf", { exports: [{ path: "/tmp/b.pdf", layout: LAYOUT, view: G2 }] })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("export_3d_model / export_module", () => {
  it("forwards 3D export options", async () => {
    const h = await harness(() => addonOk({ file: { path: "/tmp/m.obj", exists: true, sizeBytes: 1 }, bodyCount: 3 }));
    await h.call("export_3d_model", { path: "/tmp/m.obj", elements: [{ guid: G1 }], upAxis: "Z", units: "mm" });
    expect(h.requests[0]?.addOnCommand).toBe("Export3DModel");
    expect(h.requests[0]?.addOnParameters).toEqual({ path: "/tmp/m.obj", elements: [G1], upAxis: "Z", units: "mm" });
  });

  it("rejects conflicting 3D sources", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("export_3d_model", { path: "/tmp/m.stl", elements: [G1], useSelection: true })).isError).toBe(true);
    expect((await h.call("export_3d_model", { path: "/tmp/m.stl", useSelection: true, source: "AllElements" })).isError).toBe(true);
    expect((await h.call("export_3d_model", { path: "/tmp/m.fbx", format: "fbx" })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("export_module needs elements xor useSelection", async () => {
    const h = await harness(() => addonOk({ file: { path: "/tmp/a.mod" } }));
    expect((await h.call("export_module", { path: "/tmp/a.mod" })).isError).toBe(true);
    await h.call("export_module", { path: "/tmp/a.mod", useSelection: true });
    expect(addonCalls(h.requests)).toEqual(["ExportModule"]);
    expect(h.requests[0]?.addOnParameters).toEqual({ path: "/tmp/a.mod", useSelection: true });
  });
});

describe("merge_file and hotlinks", () => {
  it("merge_file forwards placement options", async () => {
    const h = await harness(() => addonOk({ merged: true, elementCount: 12 }));
    await h.call("merge_file", {
      path: "/private/tmp/claude-connector-tests/core.mod",
      position: { x: 10, y: 5 },
      angle: 90,
      storyIndex: 1,
      storyRange: "SingleStory",
      sourceStory: 0,
    });
    expect(h.requests[0]?.addOnCommand).toBe("MergeFile");
    expect(h.requests[0]?.addOnParameters).toEqual({
      path: "/private/tmp/claude-connector-tests/core.mod",
      position: { x: 10, y: 5 },
      angle: 90,
      storyIndex: 1,
      storyRange: "SingleStory",
      sourceStory: 0,
    });
  });

  it("get_hotlinks forwards filters", async () => {
    const h = await harness(() => addonOk({ nodes: [], nodeCount: 0, instanceCount: 0 }));
    await h.call("get_hotlinks", { types: ["Module"], includeElementCounts: true });
    expect(h.requests[0]?.addOnCommand).toBe("GetHotlinks");
    expect(h.requests[0]?.addOnParameters).toEqual({ types: ["Module"], includeElementCounts: true });
  });

  it("place_hotlink sends the batch", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: G1, nodeGuid: G2, nodeCreated: true }] }));
    await h.call("place_hotlink", {
      hotlinks: [
        { source: "/private/tmp/claude-connector-tests/core.mod", position: { x: 1, y: 2, z: 0 }, angle: 45, mirrored: true },
        { node: G2, storyIndex: 2 },
      ],
    });
    expect(h.requests[0]?.addOnCommand).toBe("PlaceHotlinks");
    expect(h.requests[0]?.addOnParameters).toEqual({
      hotlinks: [
        { source: "/private/tmp/claude-connector-tests/core.mod", position: { x: 1, y: 2, z: 0 }, angle: 45, mirrored: true },
        { node: G2, storyIndex: 2 },
      ],
    });
  });

  it("place_hotlink needs exactly one of source / node", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("place_hotlink", { hotlinks: [{}] })).isError).toBe(true);
    expect((await h.call("place_hotlink", { hotlinks: [{ source: "/a.pln", node: G2 }] })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("update_hotlinks supports all and relink", async () => {
    const h = await harness(() => addonOk({ results: [], updatedCount: 0 }));
    expect((await h.call("update_hotlinks", {})).isError).toBe(true);
    await h.call("update_hotlinks", { all: true, relink: [{ node: G2, source: "/new/core.mod" }] });
    expect(h.requests[0]?.addOnCommand).toBe("UpdateHotlinks");
    expect(h.requests[0]?.addOnParameters).toEqual({ all: true, relink: [{ node: G2, source: "/new/core.mod" }] });
  });

  it("delete_hotlinks forwards keepElements", async () => {
    const h = await harness(() => addonOk({ results: [{ nodeGuid: G2, broken: true }] }));
    const res = await h.call("delete_hotlinks", { nodes: [G2], keepElements: true });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("DeleteHotlinks");
    expect(h.requests[0]?.addOnParameters).toEqual({ nodes: [G2], keepElements: true });
  });

  it("reports a missing add-on command with an actionable message", async () => {
    const h = await harness(() => ({
      succeeded: false,
      error: { code: 4010, message: "Archicad does not have the registered Add-On command with the name : ClaudeConnector.GetHotlinks" },
    }));
    const res = await h.call("get_hotlinks");
    expect(res.isError).toBe(true);
    expect(res.text).toContain("GetHotlinks");
  });
});
