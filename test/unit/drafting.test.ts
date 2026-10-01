import { describe, expect, it } from "vitest";

import { addonOk, harness } from "../helpers.js";

const GUID = "92F5E7F6-2460-FF4F-A499-0B07286D2B5F";
const GUID2 = "0A1B2C3D-4E5F-6071-8293-A4B5C6D7E8F9";

const DRAFTING_TOOLS = [
  "create_lines",
  "create_arcs",
  "create_circles",
  "create_polylines",
  "create_splines",
  "create_hatches",
  "create_texts",
  "create_labels",
  "create_hotspots",
  "create_pictures",
];

function okResults(type: string, n = 1) {
  return addonOk({ results: Array.from({ length: n }, () => ({ guid: GUID, type })) });
}

describe("drafting tool registration", () => {
  it("registers every drafting tool with a description and an input schema", async () => {
    const h = await harness(() => addonOk({}));
    const tools = await h.listTools();
    for (const name of DRAFTING_TOOLS) {
      const tool = tools.find((t) => t.name === name);
      expect(tool, name).toBeDefined();
      expect(tool?.description?.length ?? 0).toBeGreaterThan(80);
      expect(tool?.inputSchema.type).toBe("object");
    }
  });
});

describe("elementId on 2D elements", () => {
  it("is rejected where Archicad has no Element ID and accepted on hatches", async () => {
    const h = await harness(() => okResults("Hatch"));
    const poly = await h.call("create_polylines", { polylines: [{ points: [{ x: 0, y: 0 }, { x: 1, y: 0 }], elementId: "P-1" }] });
    expect(poly.isError).toBe(true);
    expect(poly.text).toContain("elementId");
    const dim = await h.call("create_dimensions", { dimensions: [{ points: [{ x: 0, y: 0 }, { x: 1, y: 0 }], elementId: "D-1" }] });
    expect(dim.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
    const hatch = await h.call("create_hatches", {
      hatches: [{ polygon: [{ x: 0, y: 0 }, { x: 1, y: 0 }, { x: 1, y: 1 }], elementId: "H-1" }],
    });
    expect(hatch.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toMatchObject({ elements: [{ type: "Hatch", elementId: "H-1" }] });
  });
});

describe("create_lines", () => {
  it("sends typed Line specs to CreateElements with style fields and arrows", async () => {
    const h = await harness(() => okResults("Line", 2));
    const res = await h.call("create_lines", {
      lines: [
        { begin: { x: 0, y: 0 }, end: { x: 5, y: 0 }, pen: 3, lineType: "Сплошная линия", lineWeight: 0.35, category: "Cut" },
        {
          begin: { x: 0, y: 1 },
          end: { x: 5, y: 1 },
          lineWeight: -1,
          zoneBoundary: true,
          arrows: { end: true, type: "ClosedArrow30", size: 2.5, pen: 1 },
          layer: 12,
          storyIndex: 1,
        },
      ],
    });
    expect(res.isError).toBe(false);
    expect(h.requests).toHaveLength(1);
    expect(h.requests[0]?.addOnCommand).toBe("CreateElements");
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [
        { type: "Line", begin: { x: 0, y: 0 }, end: { x: 5, y: 0 }, pen: 3, lineType: "Сплошная линия", lineWeight: 0.35, category: "Cut" },
        {
          type: "Line",
          begin: { x: 0, y: 1 },
          end: { x: 5, y: 1 },
          lineWeight: -1,
          zoneBoundary: true,
          arrows: { end: true, type: "ClosedArrow30", size: 2.5, pen: 1 },
          layer: 12,
          storyIndex: 1,
        },
      ],
      undoName: "Create lines (Claude)",
    });
    expect(res.json).toEqual({ results: [{ guid: GUID, type: "Line" }, { guid: GUID, type: "Line" }] });
  });

  it("rejects missing points, bad pens, unknown arrow types and invalid line weights before calling Archicad", async () => {
    const h = await harness(() => addonOk({}));
    for (const bad of [
      { begin: { x: 0, y: 0 } },
      { begin: { x: 0, y: 0 }, end: { x: 1, y: 0 }, pen: 0 },
      { begin: { x: 0, y: 0 }, end: { x: 1, y: 0 }, arrows: { type: "Triangle" } },
      { begin: { x: 0, y: 0 }, end: { x: 1, y: 0 }, lineWeight: 0 },
      { begin: { x: 0, y: 0 }, end: { x: 1, y: 0 }, category: "Hidden" },
    ]) {
      const res = await h.call("create_lines", { lines: [bad] });
      expect(res.isError, JSON.stringify(bad)).toBe(true);
    }
    expect(h.requests).toHaveLength(0);
  });

  it("passes per-item errors through without failing the tool", async () => {
    const results = [{ guid: GUID, type: "Line" }, { error: { code: -2130313112, message: "Line 'begin' and 'end' must be different points." } }];
    const h = await harness(() => addonOk({ results }));
    const res = await h.call("create_lines", {
      lines: [
        { begin: { x: 0, y: 0 }, end: { x: 1, y: 0 } },
        { begin: { x: 1, y: 1 }, end: { x: 1, y: 1 } },
      ],
    });
    expect(res.isError).toBe(false);
    expect(res.json).toEqual({ results });
  });

  it("surfaces a whole-command add-on error as a tool error", async () => {
    const h = await harness(() =>
      addonOk({ error: { code: -2130312312, message: "Cannot create line: APIERR_BADDATABASE. Drafting elements are placed into the database of the ACTIVE window" } }),
    );
    const res = await h.call("create_lines", { lines: [{ begin: { x: 0, y: 0 }, end: { x: 1, y: 0 } }] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("ACTIVE window");
  });
});

describe("create_arcs", () => {
  it("accepts the three arc forms", async () => {
    const h = await harness(() => okResults("Arc", 3));
    const arcs = [
      { center: { x: 0, y: 0 }, radius: 2, beginAngle: 0, endAngle: 90, minorRadius: 1, axisAngle: 30 },
      { begin: { x: 0, y: 0 }, end: { x: 4, y: 0 }, arcAngle: -180, pen: 5 },
      { begin: { x: 0, y: 0 }, through: { x: 1, y: 1 }, end: { x: 2, y: 0 }, arrows: { begin: true } },
    ];
    const res = await h.call("create_arcs", { arcs });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: arcs.map((a) => ({ type: "Arc", ...a })),
      undoName: "Create arcs (Claude)",
    });
  });

  it("rejects mixed, incomplete or ambiguous arc definitions", async () => {
    const h = await harness(() => addonOk({}));
    const bad = [
      { center: { x: 0, y: 0 }, radius: 1 }, // no angles
      { center: { x: 0, y: 0 }, radius: 1, beginAngle: 0, endAngle: 90, begin: { x: 1, y: 0 } }, // mixed forms
      { begin: { x: 0, y: 0 }, end: { x: 1, y: 0 } }, // neither arcAngle nor through
      { begin: { x: 0, y: 0 }, end: { x: 1, y: 0 }, arcAngle: 90, through: { x: 0.5, y: 0.5 } }, // both
      { begin: { x: 0, y: 0 }, end: { x: 1, y: 0 }, arcAngle: 90, minorRadius: 0.5 }, // ellipse needs form A
      { center: { x: 0, y: 0 }, radius: 1, beginAngle: 0, endAngle: 90, minorRadius: 0.5, ratio: 2 },
      {},
    ];
    for (const arc of bad) {
      const res = await h.call("create_arcs", { arcs: [arc] });
      expect(res.isError, JSON.stringify(arc)).toBe(true);
    }
    expect(h.requests).toHaveLength(0);
  });
});

describe("create_circles", () => {
  it("sends circles and ellipses as type Circle", async () => {
    const h = await harness(() => okResults("Circle", 2));
    const res = await h.call("create_circles", {
      circles: [
        { center: { x: 1, y: 2 }, radius: 0.5 },
        { center: { x: 3, y: 2 }, radius: 1, minorRadius: 0.4, axisAngle: 45, lineType: 1 },
      ],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [
        { type: "Circle", center: { x: 1, y: 2 }, radius: 0.5 },
        { type: "Circle", center: { x: 3, y: 2 }, radius: 1, minorRadius: 0.4, axisAngle: 45, lineType: 1 },
      ],
      undoName: "Create circles (Claude)",
    });
  });

  it("rejects non-positive radii, arrows and minorRadius together with ratio", async () => {
    const h = await harness(() => addonOk({}));
    for (const c of [
      { center: { x: 0, y: 0 }, radius: 0 },
      { center: { x: 0, y: 0 }, radius: 1, minorRadius: 0.5, ratio: 2 },
    ]) {
      expect((await h.call("create_circles", { circles: [c] })).isError).toBe(true);
    }
    expect(h.requests).toHaveLength(0);
  });
});

describe("create_polylines", () => {
  it("sends points, arcs, closed and continuousPattern", async () => {
    const h = await harness(() => okResults("PolyLine"));
    const polyline = {
      points: [
        { x: 0, y: 0 },
        { x: 4, y: 0 },
        { x: 4, y: 3 },
      ],
      arcs: [{ index: 1, angle: 90 }],
      closed: true,
      continuousPattern: true,
      zoneBoundary: true,
    };
    const res = await h.call("create_polylines", { polylines: [polyline] });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [{ type: "PolyLine", ...polyline }],
      undoName: "Create polylines (Claude)",
    });
  });

  it("requires at least two points", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("create_polylines", { polylines: [{ points: [{ x: 0, y: 0 }] }] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });
});

describe("create_splines", () => {
  it("sends automatic and explicit-handle splines", async () => {
    const h = await harness(() => okResults("Spline", 2));
    const auto = { points: [{ x: 0, y: 0 }, { x: 1, y: 1 }, { x: 2, y: 0 }], closed: true };
    const explicit = {
      points: [{ x: 0, y: 0 }, { x: 3, y: 0 }],
      directions: [
        { angle: 45, lengthNext: 1 },
        { angle: -45, lengthPrev: 1 },
      ],
      arrows: { end: true },
    };
    const res = await h.call("create_splines", { splines: [auto, explicit] });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [
        { type: "Spline", ...auto },
        { type: "Spline", ...explicit },
      ],
      undoName: "Create splines (Claude)",
    });
  });

  it("rejects direction count mismatches and closed splines with two points", async () => {
    const h = await harness(() => addonOk({}));
    const bad = [
      { points: [{ x: 0, y: 0 }, { x: 1, y: 0 }], directions: [{ angle: 0 }] },
      { points: [{ x: 0, y: 0 }, { x: 1, y: 0 }], closed: true },
      { points: [{ x: 0, y: 0 }, { x: 1, y: 0 }], directions: [{ angle: 0, lengthPrev: -1 }, { angle: 0 }] },
    ];
    for (const s of bad) {
      expect((await h.call("create_splines", { splines: [s] })).isError, JSON.stringify(s)).toBe(true);
    }
    expect(h.requests).toHaveLength(0);
  });
});

describe("create_hatches", () => {
  it("sends polygon, fill, pens, colours, contour, orientation and area text", async () => {
    const h = await harness(() => okResults("Hatch"));
    const hatch = {
      polygon: {
        points: [
          { x: 0, y: 0 },
          { x: 6, y: 0 },
          { x: 6, y: 4 },
          { x: 0, y: 4 },
        ],
        holes: [{ points: [{ x: 1, y: 1 }, { x: 2, y: 1 }, { x: 2, y: 2 }] }],
      },
      fillType: "Штриховка 45°",
      fillPen: 3,
      backgroundPen: 0,
      foregroundColor: "#FF8800",
      backgroundColor: { r: 255, g: 255, b: 200 },
      fillCategory: "Cover",
      contour: false,
      orientation: { type: "Rotated", angle: 30, origin: { x: 0, y: 0 } },
      showArea: true,
      areaText: { size: 2.5, pen: 1 },
      layer: "Отделка",
    };
    const res = await h.call("create_hatches", { hatches: [hatch] });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({ elements: [{ type: "Hatch", ...hatch }], undoName: "Create fills (Claude)" });
  });

  it("accepts building-material hatches and colour removal", async () => {
    const h = await harness(() => okResults("Hatch"));
    const hatch = {
      polygon: [
        { x: 0, y: 0 },
        { x: 1, y: 0 },
        { x: 1, y: 1 },
      ],
      buildingMaterial: { index: 5 },
      overrideBuildingMaterialPens: false,
      foregroundColor: false,
      contourPen: 2,
      contourLineWeight: 0.5,
    };
    const res = await h.call("create_hatches", { hatches: [hatch] });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters?.["elements"]).toEqual([{ type: "Hatch", ...hatch }]);
  });

  it("rejects bad colours, too few points and unknown orientation types", async () => {
    const h = await harness(() => addonOk({}));
    const square = [
      { x: 0, y: 0 },
      { x: 1, y: 0 },
      { x: 1, y: 1 },
    ];
    const bad = [
      { polygon: square, foregroundColor: "orange" },
      { polygon: square, backgroundColor: { r: 300, g: 0, b: 0 } },
      { polygon: [{ x: 0, y: 0 }, { x: 1, y: 0 }] },
      { polygon: square, orientation: { type: "Spiral" } },
      { polygon: square, backgroundPen: 256 },
    ];
    for (const hatch of bad) {
      expect((await h.call("create_hatches", { hatches: [hatch] })).isError, JSON.stringify(hatch)).toBe(true);
    }
    expect(h.requests).toHaveLength(0);
  });
});

describe("create_texts", () => {
  it("sends multi-line Unicode text with style and layout fields", async () => {
    const h = await harness(() => okResults("Text"));
    const text = {
      position: { x: 1, y: 2 },
      text: "Кухня\nKitchen 12.5 m²",
      size: 3.5,
      font: "Arial",
      bold: true,
      justification: "Center",
      anchor: "Center",
      angle: 15,
      wrapWidth: 0,
      frame: true,
      frameOffset: 1,
      background: true,
      backgroundPen: 19,
    };
    const res = await h.call("create_texts", { texts: [text] });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({ elements: [{ type: "Text", ...text }], undoName: "Create texts (Claude)" });
  });

  it("sends rich-text runs", async () => {
    const h = await harness(() => okResults("Text"));
    const text = {
      position: { x: 0, y: 0 },
      runs: [
        { text: "Title\n", bold: true, size: 5 },
        { text: "body text", italic: true, pen: 4 },
      ],
    };
    const res = await h.call("create_texts", { texts: [text] });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters?.["elements"]).toEqual([{ type: "Text", ...text }]);
  });

  it("requires exactly one of text / runs and a position", async () => {
    const h = await harness(() => addonOk({}));
    const bad = [
      { position: { x: 0, y: 0 } },
      { position: { x: 0, y: 0 }, text: "a", runs: [{ text: "b" }] },
      { text: "no position" },
      { position: { x: 0, y: 0 }, text: "" },
      { position: { x: 0, y: 0 }, text: "x", anchor: "Middle" },
    ];
    for (const t of bad) {
      expect((await h.call("create_texts", { texts: [t] })).isError, JSON.stringify(t)).toBe(true);
    }
    expect(h.requests).toHaveLength(0);
  });
});

describe("create_labels", () => {
  it("normalizes the parent reference and sends leader settings", async () => {
    const h = await harness(() => okResults("Label", 2));
    const res = await h.call("create_labels", {
      labels: [
        { parent: { guid: GUID2 }, text: "Стена 250", leader: { shape: "Spline", arrows: { begin: true, type: "FullCircle" } } },
        { begin: { x: 0, y: 0 }, end: { x: 2, y: 1 }, middle: { x: 1, y: 1 }, text: "Note", textOrientation: "Horizontal", frame: true },
      ],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [
        { type: "Label", parent: GUID2, text: "Стена 250", leader: { shape: "Spline", arrows: { begin: true, type: "FullCircle" } } },
        {
          type: "Label",
          begin: { x: 0, y: 0 },
          end: { x: 2, y: 1 },
          middle: { x: 1, y: 1 },
          text: "Note",
          textOrientation: "Horizontal",
          frame: true,
        },
      ],
      undoName: "Create labels (Claude)",
    });
  });

  it("sends symbol labels with a library part and GDL parameters", async () => {
    const h = await harness(() => okResults("Label"));
    const label = { parent: GUID2, labelClass: "Symbol", libraryPart: "Метка Стены", gdlParameters: { iShowId: 1 } };
    const res = await h.call("create_labels", { labels: [label] });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters?.["elements"]).toEqual([{ type: "Label", ...label }]);
  });

  it("rejects independent labels without begin, middle without end and text with runs", async () => {
    const h = await harness(() => addonOk({}));
    const bad = [
      { text: "floating" },
      { begin: { x: 0, y: 0 }, middle: { x: 1, y: 1 }, text: "x" },
      { begin: { x: 0, y: 0 }, text: "a", runs: [{ text: "b" }] },
      { parent: "not-a-guid", text: "x" },
    ];
    for (const l of bad) {
      expect((await h.call("create_labels", { labels: [l] })).isError, JSON.stringify(l)).toBe(true);
    }
    expect(h.requests).toHaveLength(0);
  });
});

describe("create_hotspots", () => {
  it("sends hotspots", async () => {
    const h = await harness(() => okResults("Hotspot", 2));
    const res = await h.call("create_hotspots", {
      hotspots: [{ position: { x: 1, y: 1 } }, { position: { x: 2, y: 1 }, height: 0.9, pen: 5, storyIndex: "1-й этаж" }],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({
      elements: [
        { type: "Hotspot", position: { x: 1, y: 1 } },
        { type: "Hotspot", position: { x: 2, y: 1 }, height: 0.9, pen: 5, storyIndex: "1-й этаж" },
      ],
      undoName: "Create hotspots (Claude)",
    });
  });
});

describe("create_pictures", () => {
  it("sends picture placements", async () => {
    const h = await harness(() => okResults("Picture"));
    const picture = { file: "/Users/me/logo.png", position: { x: 0.2, y: 0.1 }, width: 0.05, anchor: "TopLeft", transparent: true };
    const res = await h.call("create_pictures", { pictures: [picture] });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toEqual({ elements: [{ type: "Picture", ...picture }], undoName: "Place pictures (Claude)" });
  });

  it("rejects a missing file or non-positive size", async () => {
    const h = await harness(() => addonOk({}));
    for (const p of [{ position: { x: 0, y: 0 } }, { file: "/tmp/a.png", position: { x: 0, y: 0 }, width: -1 }]) {
      expect((await h.call("create_pictures", { pictures: [p] })).isError).toBe(true);
    }
    expect(h.requests).toHaveLength(0);
  });
});
