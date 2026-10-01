/**
 * Official JSON API wrappers for attribute folders (browse / create / delete / move / rename),
 * profile preview images and the active pen tables.
 */

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import type { ArchicadClient } from "../archicad/client.js";
import { CREATES, defineTool, DESTRUCTIVE, MODIFIES, READ_ONLY, type ToolContext } from "./define.js";
import {
  attrIdItems,
  execResults,
  folderJson,
  folderLabel,
  FolderRef,
  getAttributeDetails,
  hexToRgb,
  innerGuid,
  OfficialAttributeType,
  OfficialAttrRef,
  resolveAttributes,
  rgbToHex,
  unwrap,
  type AttributeType,
  type FolderRefT,
  type Json,
  type OfficialAttrRefT,
} from "./official-helpers.js";

interface FolderOut {
  path: string[];
  guid?: string;
  attributes?: Array<{ guid: string; name?: string }>;
  attributeCount?: number;
  subfolders?: FolderOut[];
  subfolderCount?: number;
}

async function folderContent(ac: ArchicadClient, folder: Json): Promise<{ subfolders: Json[]; attributeIds: string[] }> {
  const res = await ac.api<{ attributeFolderContent?: { subfolders?: Json[]; attributeIds?: unknown[] } }>("API.GetAttributeFolderContent", {
    attributeFolder: folder,
  });
  const c = res.attributeFolderContent ?? {};
  return {
    subfolders: c.subfolders ?? [],
    attributeIds: (c.attributeIds ?? []).map((x) => innerGuid(x)).filter((g): g is string => !!g),
  };
}

function hintFolderError(e: unknown, type: string): Error {
  const msg = e instanceof Error ? e.message : String(e);
  if (/not found/i.test(msg)) return new Error(`${msg} Browse the existing ${type} folders with get_attribute_folders {attributeType: '${type}'}.`);
  if (/cannot move/i.test(msg)) {
    return new Error(
      `${msg}. Built-in special attributes (e.g. the Archicad layer 'Слой Archicad' / 'ARCHICAD Layer') always stay in the root folder; ` +
        "move the other attributes in a separate call.",
    );
  }
  return e instanceof Error ? e : new Error(msg);
}

const FolderItem = z.union([
  FolderRef,
  z.object({ attributeType: OfficialAttributeType.optional(), path: z.union([z.array(z.string().min(1)), z.string()]) }),
]);

type FolderItemT = z.infer<typeof FolderItem>;

function folderOf(defaultType: AttributeType | undefined, item: FolderItemT): { type: AttributeType; ref: FolderRefT } {
  if (typeof item === "object" && !Array.isArray(item) && "path" in item) {
    const type = (item.attributeType ?? defaultType) as AttributeType | undefined;
    if (!type) throw new Error("Pass attributeType (for all folders or per folder).");
    return { type, ref: item.path };
  }
  if (!defaultType) throw new Error("Pass attributeType (for all folders or per folder).");
  return { type: defaultType, ref: item as FolderRefT };
}

export function registerOfficialAttributeTools(server: McpServer, ctx: ToolContext): void {
  defineTool(server, ctx, {
    name: "get_attribute_folders",
    title: "Browse attribute folders",
    description:
      "Browses the folder structure of an attribute type (Attribute Manager folders): a folder's GUID, its attributes {guid, name} and " +
      "subfolders, recursively up to `depth`. Paths are arrays of folder names from the root (localized). Output: {attributeType, path, guid, " +
      "attributes, subfolders: [{path, guid, attributes?, subfolders?, subfolderCount?}]}. Use the folder paths/GUIDs with create/delete/" +
      "rename_attribute_folder(s) and move_attributes_to_folder; attribute details come from get_attributes.",
    input: {
      attributeType: OfficialAttributeType,
      folder: FolderRef.optional().describe("Folder to open (default: root)"),
      depth: z.number().int().min(0).max(10).optional().describe("Subfolder levels to expand (default 1; 0 = only this folder)"),
      includeAttributes: z.boolean().optional().describe("List attributes {guid, name} of each expanded folder (default true); false = only counts"),
    },
    annotations: READ_ONLY,
    handler: async ({ attributeType, folder, depth, includeAttributes }, { ac }) => {
      const type = attributeType as AttributeType;
      const maxDepth = depth ?? 1;
      const withAttrs = includeAttributes !== false;
      let rootJson: Json;
      try {
        const r = await ac.api<{ attributeFolder?: Json }>("API.GetAttributeFolder", { attributeFolder: folderJson(type, (folder ?? []) as FolderRefT) });
        rootJson = r.attributeFolder ?? folderJson(type, (folder ?? []) as FolderRefT);
      } catch (e) {
        throw hintFolderError(e, type);
      }
      const allAttrIds: string[] = [];
      const expand = async (f: Json, level: number): Promise<FolderOut> => {
        const out: FolderOut = { path: (f["path"] as string[] | undefined) ?? [] };
        const g = innerGuid(f["attributeFolderId"]);
        if (g) out.guid = g;
        const content = await folderContent(ac, { attributeType: type, ...(g ? { attributeFolderId: { guid: g } } : { path: out.path }) });
        if (withAttrs) {
          out.attributes = content.attributeIds.map((id) => ({ guid: id }));
          allAttrIds.push(...content.attributeIds);
        } else out.attributeCount = content.attributeIds.length;
        if (content.subfolders.length) {
          if (level < maxDepth) out.subfolders = await Promise.all(content.subfolders.map((s) => expand(s, level + 1)));
          else out.subfolderCount = content.subfolders.length;
        }
        return out;
      };
      const tree = await expand(rootJson, 0);
      if (withAttrs && allAttrIds.length) {
        const details = await getAttributeDetails(ac, type, [...new Set(allAttrIds)]);
        const names = new Map(details.map((d) => [d.guid, d.name]));
        const fill = (f: FolderOut) => {
          for (const a of f.attributes ?? []) a.name = names.get(a.guid.toUpperCase());
          for (const s of f.subfolders ?? []) fill(s);
        };
        fill(tree);
      }
      return { attributeType: type, ...tree };
    },
  });

  defineTool(server, ctx, {
    name: "create_attribute_folders",
    title: "Create attribute folders",
    description:
      "Creates attribute folders by full path; missing parent folders are created too (e.g. ['Проект', 'Стены'] creates both). " +
      "Output: {results: [{folder, guid, ok: true} | {folder, error}]}. Move attributes in with move_attributes_to_folder.",
    input: {
      attributeType: OfficialAttributeType.optional().describe("Attribute type of all folders (or give it per folder)"),
      folders: z
        .array(FolderItem)
        .min(1)
        .max(200)
        .describe("Folder paths: ['A','B'] or 'A/B', or {attributeType, path}"),
    },
    annotations: CREATES,
    handler: async ({ attributeType, folders }, { ac }) => {
      const specs = folders.map((f) => folderOf(attributeType as AttributeType | undefined, f as FolderItemT));
      const res = await ac.api<{ executionResults?: unknown[] }>("API.CreateAttributeFolders", {
        attributeFolders: specs.map((s) => folderJson(s.type, s.ref)),
      });
      const r = execResults(res.executionResults);
      const out = await Promise.all(
        specs.map(async (s, i) => {
          const row: Json = { attributeType: s.type, folder: folderLabel(s.ref), ...(r[i] ?? { error: "no result" }) };
          if ("ok" in row) {
            try {
              const f = await ac.api<{ attributeFolder?: Json }>("API.GetAttributeFolder", { attributeFolder: folderJson(s.type, s.ref) });
              row["guid"] = innerGuid(f.attributeFolder?.["attributeFolderId"]);
            } catch {
              /* optional */
            }
          }
          return row;
        }),
      );
      return { results: out };
    },
  });

  defineTool(server, ctx, {
    name: "delete_attribute_folders",
    title: "Delete attribute folders",
    description:
      "Deletes attribute folders AND every deletable attribute inside them (attributes in use or built-in ones are kept). Irreversible " +
      "through this connector — inspect with get_attribute_folders first, and move attributes you want to keep out with " +
      "move_attributes_to_folder. Output: {results: [{folder, ok: true} | {folder, error}]}.",
    input: {
      attributeType: OfficialAttributeType.optional().describe("Attribute type of all folders (or per folder)"),
      folders: z.array(FolderItem).min(1).max(200).describe("Folders: path ['A','B'] / 'A/B', {guid}, or {attributeType, path}"),
    },
    annotations: DESTRUCTIVE,
    handler: async ({ attributeType, folders }, { ac }) => {
      const specs = folders.map((f) => folderOf(attributeType as AttributeType | undefined, f as FolderItemT));
      for (const s of specs) {
        if (typeof s.ref !== "object" || Array.isArray(s.ref)) {
          const p = Array.isArray(s.ref) ? s.ref : s.ref.split("/").filter((x) => x.trim());
          if (p.length === 0) throw new Error("Refusing to delete the root attribute folder.");
        }
      }
      const res = await ac.api<{ executionResults?: unknown[] }>("API.DeleteAttributeFolders", {
        attributeFolders: specs.map((s) => folderJson(s.type, s.ref)),
      });
      const r = execResults(res.executionResults);
      return { results: specs.map((s, i) => ({ attributeType: s.type, folder: folderLabel(s.ref), ...(r[i] ?? { error: "no result" }) })) };
    },
  });

  defineTool(server, ctx, {
    name: "move_attributes_to_folder",
    title: "Move attributes to folder",
    description:
      "Moves attributes and/or attribute folders of one type into a target folder (created beforehand with create_attribute_folders; [] = root). " +
      "Attributes are given by GUID or exact localized name. Output: {ok, target, moved: {attributes, folders}}.",
    input: {
      attributeType: OfficialAttributeType,
      target: FolderRef.describe("Target folder: path (['A','B'] / 'A/B', [] = root) or {guid}"),
      attributes: z.array(OfficialAttrRef).max(1000).optional().describe("Attributes to move"),
      folders: z.array(FolderRef).max(200).optional().describe("Folders to move (with their content)"),
    },
    annotations: MODIFIES,
    handler: async ({ attributeType, target, attributes, folders }, { ac }) => {
      const type = attributeType as AttributeType;
      if (!attributes?.length && !folders?.length) throw new Error("Pass 'attributes' and/or 'folders' to move.");
      const resolved = attributes?.length ? await resolveAttributes(ac, type, attributes as OfficialAttrRefT[]) : [];
      try {
        await ac.api("API.MoveAttributesAndFolders", {
          folders: (folders ?? []).map((f) => folderJson(type, f as FolderRefT)),
          attributeIds: attrIdItems(resolved.map((r) => r.guid)),
          targetFolder: folderJson(type, target as FolderRefT),
        });
      } catch (e) {
        throw hintFolderError(e, type);
      }
      return { ok: true, target: folderLabel(target as FolderRefT), moved: { attributes: resolved, folders: (folders ?? []).map((f) => folderLabel(f as FolderRefT)) } };
    },
  });

  defineTool(server, ctx, {
    name: "rename_attribute_folder",
    title: "Rename attribute folder",
    description: "Renames an attribute folder (path or GUID). Output: {ok, folder: {path, guid}}.",
    input: {
      attributeType: OfficialAttributeType,
      folder: FolderRef.describe("Folder to rename: path (['A','B'] / 'A/B') or {guid}"),
      newName: z.string().min(1).describe("New folder name"),
    },
    annotations: MODIFIES,
    handler: async ({ attributeType, folder, newName }, { ac }) => {
      const type = attributeType as AttributeType;
      const f = folder as FolderRefT;
      let guid: string | undefined;
      try {
        const cur = await ac.api<{ attributeFolder?: Json }>("API.GetAttributeFolder", { attributeFolder: folderJson(type, f) });
        guid = innerGuid(cur.attributeFolder?.["attributeFolderId"]);
        const p = cur.attributeFolder?.["path"];
        if (Array.isArray(p) && p.length === 0) throw new Error("The root attribute folder cannot be renamed.");
        await ac.api("API.RenameAttributeFolder", { attributeFolder: folderJson(type, f), newName });
      } catch (e) {
        throw hintFolderError(e, type);
      }
      let after: Json | undefined;
      if (guid) {
        try {
          after = (await ac.api<{ attributeFolder?: Json }>("API.GetAttributeFolder", { attributeFolder: { attributeType: type, attributeFolderId: { guid } } }))
            .attributeFolder;
        } catch {
          /* optional */
        }
      }
      return { ok: true, folder: { path: after?.["path"], guid } };
    },
  });

  defineTool(server, ctx, {
    name: "get_profile_preview",
    title: "Profile preview image",
    description:
      "Renders preview images (PNG) of Profile attributes (complex profiles used by walls, beams, columns, handrails...) so you can see their " +
      "cross-section shape, plus each profile's name, usage and nominal size. Profiles by exact localized name or GUID (see get_attributes " +
      "{type: 'Profile'} or get_attribute_folders {attributeType: 'Profile'}).",
    input: {
      profiles: z.array(OfficialAttrRef).min(1).max(8).describe("Profile attributes"),
      width: z.number().int().min(16).max(2048).optional().describe("Image width in pixels (default 400)"),
      height: z.number().int().min(16).max(2048).optional().describe("Image height in pixels (default 400)"),
      background: z
        .union([z.string(), z.object({ red: z.number().min(0).max(1), green: z.number().min(0).max(1), blue: z.number().min(0).max(1) })])
        .optional()
        .describe("Background color '#RRGGBB' or {red, green, blue} 0..1 (default white)"),
    },
    annotations: READ_ONLY,
    handler: async ({ profiles, width, height, background }, { ac }) => {
      const resolved = await resolveAttributes(ac, "Profile", profiles as OfficialAttrRefT[]);
      const guids = resolved.map((r) => r.guid);
      const params: Json = { attributeIds: attrIdItems(guids), imageWidth: width ?? 400, imageHeight: height ?? 400 };
      params["backgroundColor"] = typeof background === "string" ? hexToRgb(background) : (background ?? { red: 1, green: 1, blue: 1 });
      const [prev, details] = await Promise.all([
        ac.api<{ previewImages?: unknown[] }>("API.GetProfileAttributePreview", params),
        getAttributeDetails(ac, "Profile", guids),
      ]);
      const content: Array<{ type: "image"; data: string; mimeType: string } | { type: "text"; text: string }> = [];
      const meta = guids.map((g, i) => {
        const d = details[i];
        const raw = d?.raw ?? {};
        const info: Json = { guid: g, name: d?.name ?? resolved[i]?.name };
        for (const k of ["useWith", "width", "height", "minimumWidth", "minimumHeight", "widthStretchable", "heightStretchable", "hasCoreSkin"]) {
          if (raw[k] !== undefined) info[k] = raw[k];
        }
        const u = unwrap<Json>(prev.previewImages?.[i]);
        if (!u.ok) return { ...info, error: u.error };
        const data = String(u.value["content"] ?? "");
        if (!data) return { ...info, error: "empty image" };
        content.push({ type: "image", data, mimeType: "image/png" });
        return { ...info, image: content.filter((c) => c.type === "image").length };
      });
      content.push({ type: "text", text: JSON.stringify({ profiles: meta }) });
      return { kind: "content", result: { content, isError: content.length === 1 ? true : undefined } };
    },
  });

  defineTool(server, ctx, {
    name: "get_active_pen_tables",
    title: "Active pen tables",
    description:
      "Returns the pen tables (pen sets) currently used by model views and by the layout book: {modelView: {guid, name}, layoutBook: {guid, " +
      "name}}. With includePens, also their 255 pens {index, color '#RRGGBB', weight (mm), description}. Pen colors of elements follow " +
      "the model-view table. Change pens with modify_pens.",
    input: {
      includePens: z.boolean().optional().describe("Include the pens of both tables (default false)"),
      pens: z.array(z.number().int().min(1).max(255)).max(255).optional().describe("With includePens: only these pen indices"),
    },
    annotations: READ_ONLY,
    handler: async ({ includePens, pens }, { ac }) => {
      const res = await ac.api<{ modelViewPenTableId?: Json; layoutBookPenTableId?: Json }>("API.GetActivePenTables");
      const ids = { modelView: res.modelViewPenTableId, layoutBook: res.layoutBookPenTableId };
      const out: Json = {};
      const guids: Array<[string, string]> = [];
      for (const [k, v] of Object.entries(ids)) {
        const u = unwrap<Json>(v);
        if (!u.ok) out[k] = { error: u.error };
        else {
          const g = innerGuid(u.value) ?? innerGuid(v);
          if (g) guids.push([k, g]);
        }
      }
      const details = await getAttributeDetails(
        ac,
        "PenTable",
        guids.map(([, g]) => g),
      );
      const wanted = pens ? new Set(pens) : undefined;
      guids.forEach(([k, g], i) => {
        const d = details[i]!;
        const o: Json = { guid: g, name: d.name };
        if (d.error) o["error"] = d.error;
        if (includePens && d.raw) {
          o["pens"] = ((d.raw["pens"] as unknown[] | undefined) ?? [])
            .map((p) => ((p as Json)["pen"] ?? p) as Json)
            .filter((p) => !wanted || wanted.has(Number(p["index"])))
            .map((p) => ({ index: p["index"], color: rgbToHex(p["color"]), weight: p["weight"], ...(p["description"] ? { description: p["description"] } : {}) }));
        }
        out[k] = o;
      });
      if (guids.length === 2 && guids[0]![1] === guids[1]![1]) out["sameTable"] = true;
      return out;
    },
  });
}
