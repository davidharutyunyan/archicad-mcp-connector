/**
 * Image helpers for the view tools (capture_view / render_view):
 * temp file paths, PNG/JPEG size sniffing, downscaling (macOS `sips`) and cleanup.
 *
 * Archicad writes the picture to a file (APIDo_SaveID / APIDo_PhotoRenderID); the MCP server reads it,
 * shrinks it when needed and returns it as MCP image content.
 */

import { execFile } from "node:child_process";
import { randomUUID } from "node:crypto";
import { promises as fs } from "node:fs";
import os from "node:os";
import path from "node:path";

export type ImageFormat = "png" | "jpeg";

/** Default longest side of returned images (px). */
export const DEFAULT_MAX_SIZE = 1600;
/** Keep returned images below this many bytes (MCP clients / the Claude API reject very large images). */
export const MAX_IMAGE_BYTES = 3_500_000;

export interface ImageSize {
  width: number;
  height: number;
  format: ImageFormat;
}

export interface LoadedImage {
  data: string;
  mimeType: string;
  format: ImageFormat;
  width?: number;
  height?: number;
  originalWidth?: number;
  originalHeight?: number;
  bytes: number;
  resized: boolean;
  note?: string;
}

/** Folder for temporary pictures written by Archicad. */
export function tempImageDir(): string {
  return path.join(os.tmpdir(), "archicad-connector");
}

/** Creates the temp folder and returns a fresh file path, e.g. /tmp/.../capture-1700000000000-ab12cd34.png */
export async function makeTempImagePath(prefix: string, format: ImageFormat): Promise<string> {
  const dir = tempImageDir();
  await fs.mkdir(dir, { recursive: true });
  const ext = format === "jpeg" ? "jpg" : "png";
  return path.join(dir, `${prefix}-${Date.now()}-${randomUUID().slice(0, 8)}.${ext}`);
}

/** Reads width/height from a PNG or JPEG header (no decoding). */
export function sniffImageSize(buf: Buffer): ImageSize | undefined {
  if (buf.length >= 24 && buf[0] === 0x89 && buf[1] === 0x50 && buf[2] === 0x4e && buf[3] === 0x47) {
    return { width: buf.readUInt32BE(16), height: buf.readUInt32BE(20), format: "png" };
  }
  if (buf.length >= 4 && buf[0] === 0xff && buf[1] === 0xd8) {
    let i = 2;
    while (i + 9 < buf.length) {
      if (buf[i] !== 0xff) {
        i++;
        continue;
      }
      const marker = buf[i + 1]!;
      if (marker === 0xd8 || marker === 0x01 || (marker >= 0xd0 && marker <= 0xd7)) {
        i += 2;
        continue;
      }
      const len = buf.readUInt16BE(i + 2);
      const isSof = marker >= 0xc0 && marker <= 0xcf && marker !== 0xc4 && marker !== 0xc8 && marker !== 0xcc;
      if (isSof) {
        return { height: buf.readUInt16BE(i + 5), width: buf.readUInt16BE(i + 7), format: "jpeg" };
      }
      if (len < 2) return undefined;
      i += 2 + len;
    }
    return undefined;
  }
  return undefined;
}

function detectFormat(buf: Buffer, fallback: ImageFormat): ImageFormat {
  return sniffImageSize(buf)?.format ?? fallback;
}

function mimeOf(format: ImageFormat): string {
  return format === "jpeg" ? "image/jpeg" : "image/png";
}

function run(cmd: string, args: string[], timeoutMs: number): Promise<void> {
  return new Promise((resolve, reject) => {
    execFile(cmd, args, { timeout: timeoutMs }, (err) => (err ? reject(err) : resolve()));
  });
}

/** Downscales (and optionally converts) an image with macOS `sips`. Throws when sips is unavailable. */
export async function resizeImage(src: string, dst: string, maxSide: number, format: ImageFormat, quality: number): Promise<void> {
  if (process.platform !== "darwin") throw new Error("image resizing needs macOS 'sips'");
  const args = ["-Z", String(Math.max(16, Math.round(maxSide)))];
  if (format === "jpeg") args.push("-s", "format", "jpeg", "-s", "formatOptions", String(Math.min(100, Math.max(1, Math.round(quality)))));
  else args.push("-s", "format", "png");
  args.push(src, "--out", dst);
  await run("sips", args, 60_000);
}

export interface LoadOptions {
  maxSize?: number;
  /** Bounding box (px) the image must fit into; combined with maxSize. */
  maxWidth?: number;
  maxHeight?: number;
  format: ImageFormat;
  quality?: number;
}

/**
 * Reads an image file and returns it base64-encoded, downscaled to fit maxSize / maxWidth / maxHeight and
 * MAX_IMAGE_BYTES (re-encoding large images as JPEG). Temporary files created here are removed.
 */
export async function loadImage(file: string, opts: LoadOptions): Promise<LoadedImage> {
  const original = await fs.readFile(file);
  const size = sniffImageSize(original);
  let format = detectFormat(original, opts.format);
  const maxSize = opts.maxSize ?? DEFAULT_MAX_SIZE;
  const quality = opts.quality ?? 85;

  // `limit` = allowed longest side in px.
  let limit = maxSize;
  if (size) {
    const longest = Math.max(size.width, size.height);
    if (opts.maxWidth && size.width > opts.maxWidth) limit = Math.min(limit, Math.floor((longest * opts.maxWidth) / size.width));
    if (opts.maxHeight && size.height > opts.maxHeight) limit = Math.min(limit, Math.floor((longest * opts.maxHeight) / size.height));
  }
  const tooLarge = size ? Math.max(size.width, size.height) > limit : false;
  const tooHeavy = original.length > MAX_IMAGE_BYTES;

  let out = original;
  let resized = false;
  let note: string | undefined;
  let width = size?.width;
  let height = size?.height;

  if (tooLarge || tooHeavy) {
    const tmp = await makeTempImagePath("resized", tooHeavy ? "jpeg" : format);
    try {
      const targetFormat: ImageFormat = tooHeavy ? "jpeg" : format;
      const side = size ? Math.min(limit, Math.max(size.width, size.height)) : limit;
      await resizeImage(file, tmp, side, targetFormat, quality);
      let buf = await fs.readFile(tmp);
      if (buf.length > MAX_IMAGE_BYTES) {
        await resizeImage(file, tmp, Math.round(side * 0.7), "jpeg", 70);
        buf = await fs.readFile(tmp);
      }
      out = buf;
      resized = true;
      format = detectFormat(buf, targetFormat);
      const s2 = sniffImageSize(buf);
      width = s2?.width;
      height = s2?.height;
    } catch (e) {
      note = `The image was not downscaled (${e instanceof Error ? e.message : String(e)}).`;
    } finally {
      await fs.rm(tmp, { force: true }).catch(() => undefined);
    }
  }

  const result: LoadedImage = {
    data: out.toString("base64"),
    mimeType: mimeOf(format),
    format,
    bytes: out.length,
    resized,
  };
  if (width !== undefined) result.width = width;
  if (height !== undefined) result.height = height;
  if (resized && size) {
    result.originalWidth = size.width;
    result.originalHeight = size.height;
  }
  if (note) result.note = note;
  return result;
}

/** Waits until a file exists and its size is stable (Archicad may finish writing slightly after returning). */
export async function waitForFile(file: string, timeoutMs: number, pollMs = 250): Promise<boolean> {
  const deadline = Date.now() + timeoutMs;
  let lastSize = -1;
  for (;;) {
    try {
      const st = await fs.stat(file);
      if (st.size > 0 && st.size === lastSize) return true;
      lastSize = st.size;
    } catch {
      /* not there yet */
    }
    if (Date.now() >= deadline) return lastSize > 0;
    await new Promise((r) => setTimeout(r, pollMs));
  }
}

export async function removeQuietly(file: string | undefined): Promise<void> {
  if (!file) return;
  await fs.rm(file, { force: true }).catch(() => undefined);
}

/** Copies a picture to a user-chosen absolute path (creating folders). */
export async function keepCopy(src: string, dest: string): Promise<string> {
  if (!path.isAbsolute(dest)) throw new Error(`saveTo must be an absolute path, got '${dest}'`);
  await fs.mkdir(path.dirname(dest), { recursive: true });
  await fs.copyFile(src, dest);
  return dest;
}
