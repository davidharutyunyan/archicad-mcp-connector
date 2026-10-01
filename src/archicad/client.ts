/**
 * HTTP client for the Archicad JSON API.
 *
 * Archicad listens on http://127.0.0.1:<port> (first free port in 19723..19744).
 * Official commands:   {"command": "API.<Name>", "parameters": {...}}
 * Add-on commands:     API.ExecuteAddOnCommand with {addOnCommandId: {commandNamespace, commandName}, addOnCommandParameters}
 */

export const DEFAULT_PORT = 19723;
export const PORT_RANGE: readonly [number, number] = [19723, 19744];
export const ADDON_NAMESPACE = "ClaudeConnector";

export class ArchicadConnectionError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "ArchicadConnectionError";
  }
}

export class ArchicadCommandError extends Error {
  constructor(
    message: string,
    readonly command: string,
    readonly code?: number,
  ) {
    super(message);
    this.name = "ArchicadCommandError";
  }
}

export interface ArchicadInstance {
  port: number;
  /** Set when Archicad answers but cannot run commands (no open project, or a modal dialog). */
  unavailable?: string;
  version?: number;
  buildNumber?: number;
  languageCode?: string;
  connectorAddOn: boolean;
}

export interface ClientOptions {
  host?: string;
  /** Fixed port. When omitted the client auto-discovers the first live Archicad. */
  port?: number;
  /** Per-request timeout in ms (default 120 s; heavy exports/renders can take long). */
  timeoutMs?: number;
  fetchImpl?: typeof fetch;
}

type Json = Record<string, unknown>;

interface ApiResponse {
  succeeded: boolean;
  result?: Json;
  error?: { code?: number; message?: string };
}

export class ArchicadClient {
  private readonly host: string;
  private port: number | undefined;
  private readonly fixedPort: boolean;
  private readonly timeoutMs: number;
  private readonly fetchImpl: typeof fetch;

  constructor(options: ClientOptions = {}) {
    this.host = options.host ?? "127.0.0.1";
    this.port = options.port;
    this.fixedPort = options.port !== undefined;
    this.timeoutMs = options.timeoutMs ?? 120_000;
    this.fetchImpl = options.fetchImpl ?? fetch;
  }

  get currentPort(): number | undefined {
    return this.port;
  }

  /** Switch to another Archicad instance (see listInstances). */
  usePort(port: number): void {
    this.port = port;
  }

  private async rawPost(port: number, body: Json, timeoutMs: number): Promise<ApiResponse> {
    let response: Response;
    try {
      response = await this.fetchImpl(`http://${this.host}:${port}`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
        signal: AbortSignal.timeout(timeoutMs),
      });
    } catch (err) {
      const reason = err instanceof Error ? err.message : String(err);
      if (err instanceof Error && (err.name === "TimeoutError" || err.name === "AbortError")) {
        throw new ArchicadConnectionError(
          `Archicad did not answer within ${Math.round(timeoutMs / 1000)} s (port ${port}). It may be busy, or a modal dialog may be open in Archicad.`,
        );
      }
      throw new ArchicadConnectionError(`Cannot reach Archicad on port ${port}: ${reason}`);
    }
    const text = await response.text();
    try {
      return JSON.parse(text) as ApiResponse;
    } catch {
      throw new ArchicadConnectionError(`Archicad returned a non-JSON response (HTTP ${response.status}): ${text.slice(0, 200)}`);
    }
  }

  /** Scans the Archicad port range and returns every live instance. */
  async listInstances(): Promise<ArchicadInstance[]> {
    const ports: number[] = [];
    for (let p = PORT_RANGE[0]; p <= PORT_RANGE[1]; p++) ports.push(p);
    const found = await Promise.all(
      ports.map(async (port): Promise<ArchicadInstance | null> => {
        try {
          const info = await this.rawPost(port, { command: "API.GetProductInfo" }, 1500);
          if (!info.succeeded) {
            // 4001 = "Invalid program status": Archicad is running but has no open project or shows a modal dialog.
            if (info.error?.code === 4001) {
              const msg = info.error.message ?? "";
              const reason = /no open project/i.test(msg)
                ? "Archicad is running but no project is open — open or create a project in Archicad (the API needs an open project)."
                : /modal dialog/i.test(msg)
                  ? `Archicad is blocked by a modal dialog (${msg.replace(/^.*modal dialog:?\s*/i, "").replace(/\)$/, "")}) — close it in Archicad.`
                  : `Archicad cannot run commands right now: ${msg}`;
              return { port, unavailable: reason, connectorAddOn: false };
            }
            return null;
          }
          const res = info.result ?? {};
          let connectorAddOn = false;
          try {
            const avail = await this.rawPost(
              port,
              {
                command: "API.IsAddOnCommandAvailable",
                parameters: { addOnCommandId: { commandNamespace: ADDON_NAMESPACE, commandName: "Ping" } },
              },
              1500,
            );
            connectorAddOn = Boolean(avail.result?.["available"]);
          } catch {
            /* ignore */
          }
          return {
            port,
            version: res["version"] as number | undefined,
            buildNumber: res["buildNumber"] as number | undefined,
            languageCode: res["languageCode"] as string | undefined,
            connectorAddOn,
          };
        } catch {
          return null;
        }
      }),
    );
    return found.filter((x): x is ArchicadInstance => x !== null);
  }

  private async resolvePort(): Promise<number> {
    if (this.port !== undefined) return this.port;
    const instances = await this.listInstances();
    const usable = instances.filter((i) => !i.unavailable);
    if (usable.length === 0) {
      const blocked = instances.find((i) => i.unavailable);
      throw new ArchicadConnectionError(
        blocked
          ? `${blocked.unavailable} (port ${blocked.port})`
          : `No running Archicad found on ports ${PORT_RANGE[0]}-${PORT_RANGE[1]}. Start Archicad and open (or create) a project — the JSON API only listens while a project is open.`,
      );
    }
    const preferred = usable.find((i) => i.connectorAddOn) ?? usable[0]!;
    this.port = preferred.port;
    return preferred.port;
  }

  /**
   * Archicad answers official commands with error 4001 ("there is an open modal dialog: <title>") while a
   * modal dialog is open, but add-on commands simply wait. Returns the dialog title when one blocks the API.
   */
  async modalDialog(port?: number): Promise<string | undefined> {
    const p = port ?? this.port;
    if (p === undefined) return undefined;
    try {
      const r = await this.rawPost(p, { command: "API.IsAlive" }, 3000);
      if (!r.succeeded && r.error?.code === 4001) {
        const msg = r.error.message ?? "";
        if (!/modal dialog/i.test(msg)) return undefined; // e.g. "no open project"
        const m = /modal dialog:?\s*(.*?)\)?$/i.exec(msg);
        return (m?.[1] ?? msg).trim();
      }
    } catch {
      /* not reachable */
    }
    return undefined;
  }

  private async post(body: Json, timeoutMs?: number): Promise<ApiResponse> {
    const port = await this.resolvePort();
    try {
      return await this.rawPost(port, body, timeoutMs ?? this.timeoutMs);
    } catch (err) {
      if (err instanceof ArchicadConnectionError && err.message.includes("did not answer")) {
        const dialog = await this.modalDialog(port);
        if (dialog) {
          throw new ArchicadConnectionError(
            `Archicad is blocked by a modal dialog ("${dialog}"). Ask the user to read and close it in Archicad, then retry. ` +
              "(The command may still run once the dialog is closed — check the result with a read tool before repeating it.)",
          );
        }
      }
      // The instance may have been restarted on another port: rediscover once.
      if (err instanceof ArchicadConnectionError && !this.fixedPort && !(err.message.includes("did not answer"))) {
        this.port = undefined;
        const newPort = await this.resolvePort();
        if (newPort !== port) return this.rawPost(newPort, body, timeoutMs ?? this.timeoutMs);
      }
      throw err;
    }
  }

  /**
   * Executes an official JSON API command ("API.GetAllElements" or just "GetAllElements").
   * Returns the `result` object; throws ArchicadCommandError on failure.
   */
  async api<T = Json>(command: string, parameters?: Json, timeoutMs?: number): Promise<T> {
    const name = command.includes(".") ? command : `API.${command}`;
    const body: Json = { command: name };
    if (parameters !== undefined) body["parameters"] = parameters;
    const res = await this.post(body, timeoutMs);
    if (!res.succeeded) {
      const code = res.error?.code;
      throw new ArchicadCommandError(`${name} failed${code !== undefined ? ` (code ${code})` : ""}: ${res.error?.message ?? "unknown error"}`, name, code);
    }
    return (res.result ?? {}) as T;
  }

  /**
   * Executes an add-on command (default namespace: ClaudeConnector) and returns its response object.
   * A response of the form {"error": {code, message}} is converted to an ArchicadCommandError.
   */
  async addon<T = Json>(commandName: string, parameters: Json = {}, options: { namespace?: string; timeoutMs?: number } = {}): Promise<T> {
    const namespace = options.namespace ?? ADDON_NAMESPACE;
    const label = `${namespace}.${commandName}`;
    let result: Json;
    try {
      result = await this.api<Json>(
        "API.ExecuteAddOnCommand",
        { addOnCommandId: { commandNamespace: namespace, commandName }, addOnCommandParameters: parameters },
        options.timeoutMs,
      );
    } catch (err) {
      if (err instanceof ArchicadCommandError && namespace === ADDON_NAMESPACE && /not (found|available)|unknown/i.test(err.message)) {
        throw new ArchicadCommandError(
          `${label} is not available. The Claude Connector add-on is not loaded in this Archicad (install it with scripts/install-addon.sh and restart Archicad), or it is an older build without this command.`,
          label,
          err.code,
        );
      }
      throw err;
    }
    const response = (result["addOnCommandResponse"] ?? result) as Json;
    const error = response["error"] as { code?: number; message?: string } | undefined;
    if (error && typeof error === "object" && Object.keys(response).length === 1) {
      throw new ArchicadCommandError(`${label} failed${error.code !== undefined ? ` (code ${error.code})` : ""}: ${error.message ?? "unknown error"}`, label, error.code);
    }
    return response as T;
  }

  async isAddOnCommandAvailable(commandName: string, namespace = ADDON_NAMESPACE): Promise<boolean> {
    try {
      const r = await this.api<{ available?: boolean }>("API.IsAddOnCommandAvailable", {
        addOnCommandId: { commandNamespace: namespace, commandName },
      });
      return Boolean(r.available);
    } catch {
      return false;
    }
  }
}
