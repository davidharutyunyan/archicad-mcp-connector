import { describe, expect, it } from "vitest";

import { addonOk, harness } from "../helpers.js";

describe("create_walls", () => {
  it("sends typed wall specs to CreateElements", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: "92F5E7F6-2460-FF4F-A499-0B07286D2B5F", type: "Wall" }] }));
    const res = await h.call("create_walls", {
      walls: [{ begin: { x: 0, y: 0 }, end: { x: 5, y: 0 }, height: 3, composite: "Brick 250", referenceLine: "Center" }],
    });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnCommand).toBe("CreateElements");
    expect(h.requests[0]?.addOnParameters).toMatchObject({
      elements: [{ type: "Wall", begin: { x: 0, y: 0 }, end: { x: 5, y: 0 }, height: 3, composite: "Brick 250", referenceLine: "Center" }],
    });
    expect(res.json).toEqual({ results: [{ guid: "92F5E7F6-2460-FF4F-A499-0B07286D2B5F", type: "Wall" }] });
  });

  it("rejects invalid input before calling Archicad", async () => {
    const h = await harness(() => addonOk({}));
    const res = await h.call("create_walls", { walls: [{ begin: { x: 0 } }] });
    expect(res.isError).toBe(true);
    expect(h.requests).toHaveLength(0);
  });

  it("surfaces add-on errors as tool errors", async () => {
    const h = await harness(() => addonOk({ error: { code: -2130313112, message: "Wall requires 'begin' and 'end' points." } }));
    const res = await h.call("get_element_details", { elements: ["92F5E7F6-2460-FF4F-A499-0B07286D2B5F"] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("Wall requires");
  });
});

describe("strict input validation", () => {
  it("rejects misspelled keys at any depth with the key name", async () => {
    const h = await harness(() => addonOk({ results: [] }));
    const res = await h.call("create_walls", { walls: [{ begin: { x: 0, y: 0 }, end: { x: 5, y: 0 }, heigth: 3 }] });
    expect(res.isError).toBe(true);
    expect(res.text).toContain("heigth");
    expect(h.requests).toHaveLength(0);
    const top = await h.call("create_walls", { walls: [{ begin: { x: 0, y: 0 }, end: { x: 5, y: 0 } }], extra: 1 });
    expect(top.isError).toBe(true);
    expect(top.text).toContain("extra");
  });

  it("keeps pass-through objects open (create_elements / modify_elements)", async () => {
    const h = await harness(() => addonOk({ results: [{ guid: "92F5E7F6-2460-FF4F-A499-0B07286D2B5F" }] }));
    const res = await h.call("modify_elements", { elements: [{ guid: "92F5E7F6-2460-FF4F-A499-0B07286D2B5F", height: 2.5, anyField: true }] });
    expect(res.isError).toBe(false);
    expect(h.requests[0]?.addOnParameters).toMatchObject({ elements: [{ height: 2.5, anyField: true }] });
  });
});
