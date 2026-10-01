/**
 * Connection, discovery and raw pass-through tools.
 */

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";

import { ADDON_NAMESPACE } from "../archicad/client.js";
import { defineTool, DESTRUCTIVE, READ_ONLY, type ToolContext } from "./define.js";
import { CONNECTOR_GUIDE } from "../guide.js";

export function registerSystemTools(server: McpServer, ctx: ToolContext): void {
  defineTool(server, ctx, {
    name: "archicad_status",
    title: "Archicad status",
    description:
      "Checks the connection: lists running Archicad instances (port, version, build, language), which one is active, " +
      "and whether the Claude Connector add-on (full create/modify power) and Tapir add-on are available. Call this first if anything fails.",
    input: {},
    annotations: READ_ONLY,
    handler: async (_args, { ac }) => {
      const instances = await ac.listInstances();
      if (instances.length === 0) {
        return {
          connected: false,
          hint: "No Archicad JSON API found on ports 19723-19744. Start Archicad and open or create a project (the API only listens while a project is open).",
        };
      }
      if (instances.every((i) => i.unavailable)) {
        return { connected: false, archicadRunning: true, instances, hint: instances[0]!.unavailable };
      }
      const usable = instances.filter((i) => !i.unavailable);
      if (ac.currentPort === undefined || !usable.some((i) => i.port === ac.currentPort)) {
        ac.usePort((usable.find((i) => i.connectorAddOn) ?? usable[0]!).port);
      }
      let addOn: unknown = null;
      try {
        addOn = await ac.addon("GetAddOnInfo");
      } catch (e) {
        addOn = { available: false, error: e instanceof Error ? e.message : String(e) };
      }
      const tapir = await ac.isAddOnCommandAvailable("GetAddOnVersion", "TapirCommand");
      const dialog = await ac.modalDialog();
      return {
        connected: true,
        activePort: ac.currentPort,
        instances,
        connectorAddOn: addOn,
        tapirAddOn: tapir,
        ...(dialog ? { blockedByModalDialog: dialog, hint: "Ask the user to close this dialog in Archicad; until then commands wait or fail." } : {}),
      };
    },
  });

  defineTool(server, ctx, {
    name: "select_archicad_instance",
    title: "Select Archicad instance",
    description: "When several Archicad instances run, switches all following tool calls to the instance listening on `port` (see archicad_status).",
    input: { port: z.number().int().min(1).max(65535) },
    annotations: READ_ONLY,
    handler: async ({ port }, { ac }) => {
      ac.usePort(port);
      const info = await ac.api("API.GetProductInfo");
      return { activePort: port, productInfo: info };
    },
  });

  defineTool(server, ctx, {
    name: "get_connector_guide",
    title: "Connector usage guide",
    description:
      "Returns the full usage guide for this connector: units, coordinate system, element references, workflows (building a model, " +
      "documentation, schedules), gotchas (localized library names, top-linked walls, undo), and which tool to use for what. Read it before complex tasks.",
    input: {},
    annotations: READ_ONLY,
    handler: async () => ({ kind: "content", result: { content: [{ type: "text", text: CONNECTOR_GUIDE }] } }),
  });

  defineTool(server, ctx, {
    name: "list_addon_commands",
    title: "List add-on commands",
    description: `Lists every JSON command implemented by the Claude Connector add-on (namespace ${ADDON_NAMESPACE}) with descriptions. Useful together with execute_addon_command for features without a dedicated tool.`,
    input: {},
    annotations: READ_ONLY,
    handler: async (_args, { ac }) => ac.addon("ListCommands"),
  });

  defineTool(server, ctx, {
    name: "execute_json_api_command",
    title: "Execute official JSON API command",
    description:
      "Escape hatch: runs any official Archicad JSON API command verbatim, e.g. command 'API.GetNavigatorItemTree' with parameters " +
      "{navigatorTreeId: {type: 'ProjectMap'}}. Prefer the dedicated tools; use this for commands without one. " +
      "Reference: https://archicadapi.graphisoft.com/JSONInterfaceDocumentation/",
    input: {
      command: z.string().describe("Command name, e.g. 'API.GetElementsByType' (the 'API.' prefix is optional)"),
      parameters: z.record(z.unknown()).optional().describe("Command parameters object"),
    },
    annotations: DESTRUCTIVE,
    handler: async ({ command, parameters }, { ac }) => ac.api(command, parameters),
  });

  defineTool(server, ctx, {
    name: "execute_addon_command",
    title: "Execute add-on command",
    description:
      `Escape hatch: runs any add-on JSON command through API.ExecuteAddOnCommand. Default namespace is '${ADDON_NAMESPACE}' ` +
      "(see list_addon_commands); other installed add-ons also work, e.g. namespace 'TapirCommand'.",
    input: {
      command: z.string().describe("Add-on command name, e.g. 'GetElementDetails'"),
      parameters: z.record(z.unknown()).optional().describe("Command parameters object"),
      namespace: z.string().optional().describe(`Command namespace (default ${ADDON_NAMESPACE})`),
    },
    annotations: DESTRUCTIVE,
    handler: async ({ command, parameters, namespace }, { ac }) => ac.addon(command, parameters ?? {}, { namespace }),
  });
}
