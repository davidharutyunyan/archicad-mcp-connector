/**
 * Read-back verification for the project family's setters.
 *
 * Several Archicad 26 setters report success but silently keep the old value (e.g. dimension formats while a
 * Dimension Standard is selected, computed Project Info fields such as SHORTDATE). The tools therefore compare
 * what the caller asked for with what Archicad returns afterwards and report the fields that were not applied.
 */

export interface Mismatch {
  /** Dotted path of the requested field, e.g. "dimensions.linear.decimals". */
  field: string;
  requested: unknown;
  actual: unknown;
}

type Json = Record<string, unknown>;

const isObject = (x: unknown): x is Json => typeof x === "object" && x !== null && !Array.isArray(x);

/** An attribute reference as returned by the add-on: {index, name?, guid?}. */
const isAttrRefOut = (x: unknown): x is { index: number; name?: string; guid?: string } => isObject(x) && typeof x["index"] === "number";

/** A requested attribute reference: index, name, {index}, {name} or {guid}. */
const isAttrLikeRequest = (x: unknown): boolean =>
  typeof x === "number" || typeof x === "string" || (isObject(x) && ("index" in x || "name" in x || "guid" in x));

/** Does the requested attribute reference (index / name / {index} / {name} / {guid}) denote the returned {index, name, guid}? */
function sameAttr(requested: unknown, actual: { index: number; name?: string; guid?: string }): boolean {
  if (typeof requested === "number") return requested === actual.index;
  if (typeof requested === "string") return actual.name !== undefined && actual.name.toLowerCase() === requested.toLowerCase();
  if (isObject(requested)) {
    if (typeof requested["index"] === "number") return requested["index"] === actual.index;
    if (typeof requested["name"] === "string") return sameAttr(requested["name"], actual);
    if (typeof requested["guid"] === "string") return actual.guid !== undefined && actual.guid.toUpperCase() === requested["guid"].toUpperCase();
  }
  return false;
}

function sameNumber(a: number, b: number, angle: boolean): boolean {
  const tol = 1e-4 * Math.max(1, Math.abs(a), Math.abs(b));
  if (Math.abs(a - b) <= tol) return true;
  if (angle) {
    const d = (((a - b) % 360) + 360) % 360;
    return d <= 1e-4 || 360 - d <= 1e-4;
  }
  return false;
}

export interface DiffOptions {
  /** Dotted paths (relative to the diff root) compared modulo 360 degrees. */
  angles?: string[];
  /** Dotted paths that are write-only / not echoed back and must be skipped. */
  skip?: string[];
  /** Maps a requested path to the path of the returned value (e.g. surveyPointVisible -> surveyPoint.visible). */
  rename?: Record<string, string>;
}

function lookup(root: unknown, path: string): { found: boolean; value: unknown } {
  let cur: unknown = root;
  for (const part of path.split(".")) {
    if (!isObject(cur) || !(part in cur)) return { found: false, value: undefined };
    cur = cur[part];
  }
  return { found: true, value: cur };
}

/**
 * Collects the fields of `requested` whose value differs from `actual`. Only fields present in both are compared;
 * fields missing from `actual` (write-only options) are ignored.
 */
export function diffRequested(requested: unknown, actual: unknown, options: DiffOptions = {}, prefix = ""): Mismatch[] {
  const out: Mismatch[] = [];
  const walk = (req: unknown, act: unknown, path: string): void => {
    if (req === undefined) return;
    if (options.skip?.includes(path)) return;
    if (Array.isArray(req)) {
      if (!Array.isArray(act)) {
        out.push({ field: prefix + path, requested: req, actual: act });
        return;
      }
      if (req.every(isAttrLikeRequest) && act.length > 0 && act.every(isAttrRefOut)) {
        // attribute set: same size and every requested reference present (order irrelevant)
        const ok = req.length === act.length && req.every((r) => act.some((a) => sameAttr(r, a)));
        if (!ok) out.push({ field: prefix + path, requested: req, actual: act.map((a) => (a.name !== undefined ? `${a.index} ${a.name}` : a.index)) });
        return;
      }
      if (req.every((r) => !isObject(r) && !Array.isArray(r)) && act.every((a) => !isObject(a) && !Array.isArray(a))) {
        // list of scalars (e.g. element types): compare as sorted multisets
        const norm = (xs: unknown[]) => xs.map((x) => String(x).toLowerCase()).sort().join("\u0000");
        if (norm(req) !== norm(act)) out.push({ field: prefix + path, requested: req, actual: act });
        return;
      }
      if (req.length !== act.length) {
        out.push({ field: prefix + path, requested: req, actual: act });
        return;
      }
      req.forEach((r, i) => walk(r, act[i], `${path}[${i}]`));
      return;
    }
    if (isObject(req)) {
      if (isAttrRefOut(act) && isAttrLikeRequest(req)) {
        if (!sameAttr(req, act)) out.push({ field: prefix + path, requested: req, actual: act });
        return;
      }
      if (!isObject(act)) return; // not echoed back
      for (const [k, v] of Object.entries(req)) {
        const sub = path === "" ? k : `${path}.${k}`;
        if (options.skip?.includes(sub)) continue;
        if (!(k in act)) continue;
        walk(v, act[k], sub);
      }
      return;
    }
    if (isAttrRefOut(act)) {
      if (!sameAttr(req, act)) out.push({ field: prefix + path, requested: req, actual: act });
      return;
    }
    if (typeof req === "number" && typeof act === "number") {
      if (!sameNumber(req, act, options.angles?.includes(path) ?? false)) out.push({ field: prefix + path, requested: req, actual: act });
      return;
    }
    if (typeof req === "string" && typeof act === "string") {
      if (req !== act) out.push({ field: prefix + path, requested: req, actual: act });
      return;
    }
    if (req !== act) out.push({ field: prefix + path, requested: req, actual: act });
  };

  if (options.rename && isObject(requested)) {
    const rest: Json = {};
    for (const [k, v] of Object.entries(requested)) {
      const target = options.rename[k];
      if (target === undefined) {
        rest[k] = v;
        continue;
      }
      const { found, value } = lookup(actual, target);
      if (found) walk(v, value, target);
    }
    walk(rest, actual, "");
  } else {
    walk(requested, actual, "");
  }
  return out;
}
