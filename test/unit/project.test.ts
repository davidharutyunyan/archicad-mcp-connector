import { beforeAll, describe, expect, it } from "vitest";

import { projectTiming } from "../../src/tools/project.js";
import { addonOk, harness, type RecordedRequest } from "../helpers.js";

const PROJECT = { projectOpen: true, untitled: false, teamwork: false, projectName: "House" };

beforeAll(() => {
  projectTiming.undoSettleMs = 1;
  projectTiming.undoSettlePerStepMs = 0;
  projectTiming.undoTitleWaitMs = 0;
  projectTiming.pollIntervalMs = 1;
  projectTiming.switchTimeoutMs = 2000;
});

const addonCalls = (requests: RecordedRequest[]) => requests.map((r) => r.addOnCommand);

describe("project tool registration", () => {
  it("registers every project tool", async () => {
    const h = await harness(() => addonOk({}));
    const names = (await h.listTools()).map((t) => t.name);
    for (const name of [
      "get_project_info",
      "get_project_info_fields",
      "set_project_info_fields",
      "delete_project_info_fields",
      "save_project",
      "save_project_as",
      "open_project",
      "new_project",
      "close_project",
      "quit_archicad",
      "get_preferences",
      "set_preferences",
      "get_geo_location",
      "set_geo_location",
      "rebuild_model",
      "undo",
      "redo",
    ]) {
      expect(names).toContain(name);
    }
  });
});

describe("get_project_info", () => {
  it("calls GetProjectInfo and forwards includeTemplates", async () => {
    const h = await harness(() => addonOk(PROJECT));
    const res = await h.call("get_project_info", { includeTemplates: true });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("GetProjectInfo");
    expect(h.requests[0]?.addOnParameters).toEqual({ includeTemplates: true });
    expect(res.json).toEqual(PROJECT);
  });

  it("sends no optional fields when none are given", async () => {
    const h = await harness(() => addonOk(PROJECT));
    await h.call("get_project_info");
    expect(h.requests[0]?.addOnParameters).toEqual({});
  });
});

describe("project info fields", () => {
  it("get_project_info_fields forwards the filters", async () => {
    const h = await harness(() => addonOk({ fields: [], count: 0 }));
    await h.call("get_project_info_fields", { category: "Custom", search: "client", nonEmptyOnly: true });
    expect(h.requests[0]?.addOnCommand).toBe("GetProjectInfoFields");
    expect(h.requests[0]?.addOnParameters).toEqual({ category: "Custom", search: "client", nonEmptyOnly: true });
  });

  it("set_project_info_fields converts numbers to text and keeps key/name", async () => {
    const h = await harness(() => addonOk({ results: [{ key: "PROJECTNUMBER", value: "42" }] }));
    const res = await h.call("set_project_info_fields", {
      fields: [
        { key: "PROJECTNUMBER", value: 42 },
        { name: "Заказчик", value: "ООО Ромашка" },
      ],
      createIfMissing: true,
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("SetProjectInfoFields");
    expect(h.requests[0]?.addOnParameters).toEqual({
      fields: [
        { key: "PROJECTNUMBER", value: "42" },
        { name: "Заказчик", value: "ООО Ромашка" },
      ],
      createIfMissing: true,
    });
  });

  it("set_project_info_fields rejects a field without key and name", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("set_project_info_fields", { fields: [{ value: "x" }] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("delete_project_info_fields forwards keys", async () => {
    const h = await harness(() => addonOk({ results: [{ key: "autotext-1", deleted: true }] }));
    await h.call("delete_project_info_fields", { fields: ["autotext-1"] });
    expect(h.requests[0]?.addOnCommand).toBe("DeleteProjectInfoFields");
    expect(h.requests[0]?.addOnParameters).toEqual({ fields: ["autotext-1"] });
  });

  it("surfaces add-on errors", async () => {
    const h = await harness(() => addonOk({ error: { code: -2130313111, message: "Unknown Project Info key 'NOPE'." } }));
    const res = await h.call("set_project_info_fields", { fields: [{ key: "NOPE", value: "x" }] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("Unknown Project Info key");
  });
});

describe("save", () => {
  it("save_project calls SaveProject", async () => {
    const h = await harness(() => addonOk({ saved: true, project: PROJECT }));
    const res = await h.call("save_project");
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("SaveProject");
  });

  it("save_project reports untitled projects as errors", async () => {
    const h = await harness(() => addonOk({ error: { code: -2130313088, message: "The current project has never been saved (untitled)" } }));
    const res = await h.call("save_project");
    expect(res.isError).toBe(true);
    expect(res.text).toContain("untitled");
  });

  it("save_project_as forwards path, format and archive options", async () => {
    const h = await harness(() => addonOk({ saved: true }));
    await h.call("save_project_as", {
      path: "/private/tmp/cc-test/House.pla",
      format: "pla",
      overwrite: true,
      createFolders: true,
      archive: { includeAllLibraryParts: true },
    });
    expect(h.requests[0]?.addOnCommand).toBe("SaveProjectAs");
    expect(h.requests[0]?.addOnParameters).toEqual({
      path: "/private/tmp/cc-test/House.pla",
      format: "pla",
      overwrite: true,
      createFolders: true,
      archive: { includeAllLibraryParts: true },
    });
  });

  it("save_project_as rejects unknown formats", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("save_project_as", { path: "/tmp/a.ifc", format: "ifc" });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("open / new / close / quit", () => {
  it("open_project requires a decision about unsaved changes", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("open_project", { path: "/Users/me/House.pln" });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("discardChanges");
    expect(h.requests).toHaveLength(0);
  });

  it("open_project rejects saveFirst together with discardChanges", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("open_project", { path: "/Users/me/House.pln", saveFirst: true, discardChanges: true });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("open_project sends OpenProject", async () => {
    const h = await harness(() => addonOk({ opened: true, project: PROJECT }));
    const res = await h.call("open_project", { path: "/Users/me/House.pln", discardChanges: true });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("OpenProject");
    expect(h.requests[0]?.addOnParameters).toEqual({ path: "/Users/me/House.pln", discardChanges: true });
  });

  it("open_project survives a dropped connection and reports the project that is open", async () => {
    const h = await harness((req) => {
      if (req.addOnCommand === "OpenProject") throw new Error("socket hang up");
      return addonOk(PROJECT);
    });
    const res = await h.call("open_project", { path: "/Users/me/House.pln", saveFirst: true });
    expect(res.isError).toBe(false);
    expect(addonCalls(h.requests)).toEqual(["OpenProject", "GetProjectInfo"]);
    expect(res.json).toMatchObject({ status: "connectionDroppedDuringSwitch", project: PROJECT });
  });

  it("new_project forwards template / reset", async () => {
    const h = await harness(() => addonOk({ created: true }));
    await h.call("new_project", { template: "/Applications/Graphisoft/Archicad 26/x.tpl", discardChanges: true });
    expect(h.requests[0]?.addOnCommand).toBe("NewProject");
    expect(h.requests[0]?.addOnParameters).toEqual({ template: "/Applications/Graphisoft/Archicad 26/x.tpl", discardChanges: true });

    const h2 = await harness(() => addonOk({ created: true }));
    await h2.call("new_project", { reset: true, saveFirst: true });
    expect(h2.requests[0]?.addOnParameters).toEqual({ reset: true, saveFirst: true });
  });

  it("new_project rejects reset together with template", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("new_project", { template: "/a.tpl", reset: true, discardChanges: true });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("close_project requires confirm: true", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("close_project", { discardChanges: true });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("close_project treats a dropped connection as success", async () => {
    const h = await harness(() => {
      throw new Error("ECONNRESET");
    });
    const res = await h.call("close_project", { confirm: true, discardChanges: true });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("CloseProject");
    expect(h.requests[0]?.addOnParameters).toEqual({ confirm: true, discardChanges: true });
    expect(res.json).toMatchObject({ closed: true });
  });

  it("quit_archicad saves first when asked, then quits", async () => {
    const h = await harness((req) => {
      if (req.addOnCommand === "QuitArchicad") throw new Error("socket hang up");
      return addonOk({ saved: true });
    });
    const res = await h.call("quit_archicad", { confirm: true, saveFirst: true });
    expect(res.isError).toBe(false);
    expect(addonCalls(h.requests)).toEqual(["SaveProject", "QuitArchicad"]);
    expect(h.requests[1]?.addOnParameters).toEqual({ confirm: true });
    expect(res.json).toMatchObject({ quit: true, saved: true });
  });

  it("quit_archicad does not quit when saving fails", async () => {
    const h = await harness(() => addonOk({ error: { code: -2130313088, message: "read-only" } }));
    const res = await h.call("quit_archicad", { confirm: true, saveFirst: true });
    expect(res.isError).toBe(true);
    expect(addonCalls(h.requests)).toEqual(["SaveProject"]);
  });

  it("quit_archicad requires confirm: true", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("quit_archicad", { confirm: false });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("preferences", () => {
  it("get_preferences forwards sections", async () => {
    const h = await harness(() => addonOk({ workingUnits: { lengthUnit: "Millimeter" } }));
    const res = await h.call("get_preferences", { sections: ["workingUnits", "environment"] });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("GetPreferences");
    expect(h.requests[0]?.addOnParameters).toEqual({ sections: ["workingUnits", "environment"] });
  });

  it("get_preferences rejects unknown sections", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("get_preferences", { sections: ["colors"] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("set_preferences sends only the given sections", async () => {
    const h = await harness(() => addonOk({ results: { workingUnits: { ok: true } } }));
    const res = await h.call("set_preferences", {
      workingUnits: { lengthUnit: "Millimeter", lengthDecimals: 0 },
      dimensions: { linear: { unit: "Centimeter", decimals: 1, extraAccuracy: "Off" } },
      zones: { lowHeightReductions: [{ heightLimit: 1.5, reductionPercent: 50 }] },
      environment: { autoIntersect: false },
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("SetPreferences");
    expect(h.requests[0]?.addOnParameters).toEqual({
      workingUnits: { lengthUnit: "Millimeter", lengthDecimals: 0 },
      dimensions: { linear: { unit: "Centimeter", decimals: 1, extraAccuracy: "Off" } },
      zones: { lowHeightReductions: [{ heightLimit: 1.5, reductionPercent: 50 }] },
      environment: { autoIntersect: false },
    });
  });

  it("set_preferences needs at least one section", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("set_preferences", {});
    expect(res.isError).toBe(true);
    expect(res.text).toContain("workingUnits");
    expect(h.requests).toHaveLength(0);
  });

  it("set_preferences validates units, ranges and unknown fields", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("set_preferences", { workingUnits: { lengthUnit: "Parsec" } })).isError).toBe(true);
    expect((await h.call("set_preferences", { workingUnits: { lengthDecimals: 7 } })).isError).toBe(true);
    expect((await h.call("set_preferences", { workingUnits: { lenghtUnit: "Meter" } })).isError).toBe(true);
    expect((await h.call("set_preferences", { dataSafety: {} })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("geo location", () => {
  it("get_geo_location calls GetGeoLocation", async () => {
    const h = await harness(() => addonOk({ latitude: 40.18, longitude: 44.51 }));
    const res = await h.call("get_geo_location");
    expect(h.requests[0]?.addOnCommand).toBe("GetGeoLocation");
    expect(res.json).toEqual({ latitude: 40.18, longitude: 44.51 });
  });

  it("set_geo_location forwards the given fields", async () => {
    const h = await harness(() => addonOk({ latitude: 40.18 }));
    await h.call("set_geo_location", {
      latitude: 40.18,
      longitude: 44.51,
      northDirection: 90,
      timeZoneMinutes: 240,
      dateTime: { month: 6, day: 21, hour: 12 },
      surveyPoint: { x: 1, y: 2 },
      unlockSurveyPoint: true,
      geoReference: { name: "EPSG:32638", eastings: 458000, northings: 4448000 },
    });
    expect(h.requests[0]?.addOnCommand).toBe("SetGeoLocation");
    expect(h.requests[0]?.addOnParameters).toEqual({
      latitude: 40.18,
      longitude: 44.51,
      northDirection: 90,
      timeZoneMinutes: 240,
      dateTime: { month: 6, day: 21, hour: 12 },
      surveyPoint: { x: 1, y: 2 },
      unlockSurveyPoint: true,
      geoReference: { name: "EPSG:32638", eastings: 458000, northings: 4448000 },
    });
  });

  it("set_geo_location validates ranges and requires a field", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("set_geo_location", { latitude: 91 })).isError).toBe(true);
    expect((await h.call("set_geo_location", { dateTime: { month: 13 } })).isError).toBe(true);
    const empty = await h.call("set_geo_location", {});
    expect(empty.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("rebuild_model", () => {
  it("forwards the mode", async () => {
    const h = await harness(() => addonOk({ ok: true, mode: "Regenerate" }));
    await h.call("rebuild_model", { mode: "Regenerate" });
    expect(h.requests[0]?.addOnCommand).toBe("RebuildModel");
    expect(h.requests[0]?.addOnParameters).toEqual({ mode: "Regenerate" });
  });

  it("rejects unknown modes", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("rebuild_model", { mode: "Refresh" })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("undo / redo", () => {
  it("schedules the undo, then reports what was undone", async () => {
    const h = await harness((req) => {
      if (req.addOnParameters?.["dryRun"]) {
        return addonOk({
          found: true,
          menuItem: { title: "Отменить Создать стены", enabled: true },
          lastRun: { runId: 7, action: "Undo", requested: 2, performed: 2, titles: ["Отменить Create walls (Claude)", "Отменить Create slabs (Claude)"], finished: true },
        });
      }
      return addonOk({ scheduled: true, runId: 7, steps: 2 });
    });
    const res = await h.call("undo", { steps: 2 });
    expect(res.isError).toBe(false);
    expect(addonCalls(h.requests)).toEqual(["Undo", "Undo"]);
    expect(h.requests[0]?.addOnParameters).toEqual({ steps: 2 });
    expect(h.requests[1]?.addOnParameters).toEqual({ dryRun: true });
    expect(res.json).toEqual({
      performed: 2,
      requested: 2,
      undone: ["Отменить Create walls (Claude)", "Отменить Create slabs (Claude)"],
      next: { title: "Отменить Создать стены", enabled: true },
    });
  });

  it("reports a pending run when Archicad has not executed it yet", async () => {
    const h = await harness((req) =>
      req.addOnParameters?.["dryRun"]
        ? addonOk({ found: true, lastRun: { runId: 3, finished: true, performed: 1 } })
        : addonOk({ scheduled: true, runId: 4, steps: 1 }),
    );
    const res = await h.call("undo");
    expect(res.isError).toBe(false);
    expect(res.json).toMatchObject({ status: "pending", scheduled: { runId: 4 } });
  });

  it("dryRun only inspects", async () => {
    const h = await harness(() => addonOk({ found: true, menuItem: { title: "Redo", enabled: false } }));
    await h.call("redo", { dryRun: true });
    expect(addonCalls(h.requests)).toEqual(["Redo"]);
    expect(h.requests[0]?.addOnParameters).toEqual({ dryRun: true });
  });

  it("redo forwards force and reports stoppedReason", async () => {
    const h = await harness((req) =>
      req.addOnParameters?.["dryRun"]
        ? addonOk({ found: true, lastRun: { runId: 1, finished: true, performed: 0, titles: [], stoppedReason: "The Redo menu item is disabled" } })
        : addonOk({ scheduled: true, runId: 1, steps: 1 }),
    );
    const res = await h.call("redo", { force: true });
    expect(h.requests[0]?.addOnParameters).toEqual({ force: true });
    expect(res.json).toMatchObject({ performed: 0, redone: [], stoppedReason: "The Redo menu item is disabled" });
  });

  it("surfaces 'not available' errors", async () => {
    const h = await harness(() => addonOk({ error: { code: -2130312312, message: "Could not find the Edit > Undo menu item" } }));
    const res = await h.call("undo");
    expect(res.isError).toBe(true);
    expect(res.text).toContain("Edit > Undo");
  });

  it("rejects out-of-range steps", async () => {
    const h = await harness(() => addonOk({}));
    expect((await h.call("undo", { steps: 0 })).isError).toBe(true);
    expect((await h.call("undo", { steps: 51 })).isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});
