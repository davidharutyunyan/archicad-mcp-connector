/**
 * DWG support for export_dwg.
 *
 * The Archicad 26 API cannot write DWG (APIDo_SaveID has no DWG/DXF file type), so the add-on writes DXF itself
 * (ClaudeConnector.ExportDxf). A real .dwg is produced by converting that DXF with the free ODA File Converter when it
 * is installed on this machine (https://www.opendesign.com/guestfiles/oda_file_converter).
 */

import { execFile } from "node:child_process";
import { existsSync } from "node:fs";
import { copyFile, mkdir, mkdtemp, readdir, rm, stat } from "node:fs/promises";
import { tmpdir } from "node:os";
import { basename, dirname, extname, isAbsolute, join } from "node:path";

export const DWG_VERSIONS = ["ACAD2018", "ACAD2013", "ACAD2010", "ACAD2007", "ACAD2004", "ACAD2000", "ACAD14", "ACAD12"] as const;
export type DwgVersion = (typeof DWG_VERSIONS)[number];

/** Candidate locations of the ODA File Converter executable (the env variable wins). */
export function odaConverterCandidates(env: NodeJS.ProcessEnv = process.env): string[] {
  const list: string[] = [];
  if (env["ODA_FILE_CONVERTER"]) list.push(env["ODA_FILE_CONVERTER"]);
  list.push(
    "/Applications/ODAFileConverter.app/Contents/MacOS/ODAFileConverter",
    join(env["HOME"] ?? "", "Applications/ODAFileConverter.app/Contents/MacOS/ODAFileConverter"),
    "/usr/bin/ODAFileConverter",
    "/usr/local/bin/ODAFileConverter",
    "C:\\Program Files\\ODA\\ODAFileConverter\\ODAFileConverter.exe",
  );
  return list;
}

export function findOdaConverter(env: NodeJS.ProcessEnv = process.env): string | undefined {
  return odaConverterCandidates(env).find((p) => p && existsSync(p));
}

export const DWG_UNAVAILABLE_MESSAGE =
  "Writing .dwg needs the free ODA File Converter (https://www.opendesign.com/guestfiles/oda_file_converter), which was not found " +
  "(looked in ODA_FILE_CONVERTER and /Applications/ODAFileConverter.app). Options: install it (or set the ODA_FILE_CONVERTER environment " +
  "variable of the MCP server to its executable); export format 'dxf' instead (AutoCAD, BricsCAD, nanoCAD, LibreCAD and every other CAD " +
  "program open DXF); or publish a Publisher Set whose output format is DWG (publish_publisher_set) — that uses Archicad's own DWG translator.";

/** Validates the final .dwg path before anything is exported (the add-on only sees the temporary .dxf). */
export async function checkDwgTarget(path: string, overwrite: boolean, createFolders: boolean): Promise<void> {
  if (!isAbsolute(path)) throw new Error(`'path' must be an absolute file path (e.g. /Users/me/Exports/plan.dwg), got '${path}'.`);
  if (extname(path).toLowerCase() !== ".dwg") throw new Error(`For format 'dwg' the path must end with .dwg, got '${path}'.`);
  if (existsSync(path) && !overwrite) throw new Error(`The file '${path}' already exists. Pass overwrite: true to replace it.`);
  const folder = dirname(path);
  if (!existsSync(folder)) {
    if (!createFolders) throw new Error(`The folder '${folder}' does not exist. Pass createFolders: true to create it.`);
    await mkdir(folder, { recursive: true });
  }
}

export async function makeDwgWorkFolder(): Promise<{ root: string; input: string; output: string }> {
  const root = await mkdtemp(join(tmpdir(), "claude-dwg-"));
  const input = join(root, "in");
  const output = join(root, "out");
  await mkdir(input);
  await mkdir(output);
  return { root, input, output };
}

function run(file: string, args: string[], timeoutMs: number): Promise<{ stdout: string; stderr: string }> {
  return new Promise((resolve, reject) => {
    execFile(file, args, { timeout: timeoutMs, windowsHide: true }, (err, stdout, stderr) => {
      if (err) reject(Object.assign(err, { stdout: String(stdout), stderr: String(stderr) }));
      else resolve({ stdout: String(stdout), stderr: String(stderr) });
    });
  });
}

/**
 * Converts the single DXF in `inputFolder` to DWG in `outputFolder` with the ODA File Converter and copies it to `target`.
 * Returns the size of the written file.
 */
export async function convertDxfToDwg(
  converter: string,
  inputFolder: string,
  outputFolder: string,
  dxfName: string,
  target: string,
  version: DwgVersion,
  timeoutMs = 10 * 60_000,
): Promise<{ sizeBytes: number }> {
  try {
    // ODAFileConverter <in folder> <out folder> <version> <type> <recurse> <audit> [filter]
    await run(converter, [inputFolder, outputFolder, version, "DWG", "0", "1", dxfName], timeoutMs);
  } catch (e) {
    const err = e as Error & { stderr?: string };
    throw new Error(`ODA File Converter failed: ${err.message}${err.stderr ? ` — ${err.stderr.trim().slice(0, 300)}` : ""}`);
  }
  const produced = (await readdir(outputFolder)).find((f) => extname(f).toLowerCase() === ".dwg");
  if (!produced) {
    throw new Error(
      `ODA File Converter produced no .dwg (output folder ${outputFolder} contains: ${(await readdir(outputFolder)).join(", ") || "nothing"}). ` +
        "Export format 'dxf' instead.",
    );
  }
  await copyFile(join(outputFolder, produced), target);
  return { sizeBytes: (await stat(target)).size };
}

export async function removeFolderQuietly(folder: string): Promise<void> {
  try {
    await rm(folder, { recursive: true, force: true });
  } catch {
    /* ignore */
  }
}

export function dxfNameFor(target: string): string {
  const base = basename(target, extname(target)).replace(/[^\w.\- ]+/g, "_") || "export";
  return `${base}.dxf`;
}
