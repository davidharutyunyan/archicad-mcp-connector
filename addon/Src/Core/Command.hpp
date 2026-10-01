// *****************************************************************************
// Command.hpp — tiny command registry on top of API_AddOnCommand.
//
// Every command lives in namespace "ClaudeConnector" and is invoked through the
// official JSON API:
//   {"command": "API.ExecuteAddOnCommand",
//    "parameters": {"addOnCommandId": {"commandNamespace": "ClaudeConnector",
//                                      "commandName": "<Name>"},
//                   "addOnCommandParameters": {...}}}
//
// A handler receives the parameters object and returns the response object.
// Throwing cc::Error (see Json.hpp) produces {"error": {"code", "message"}}.
// *****************************************************************************

#pragma once

#include "Core/Json.hpp"

#include <functional>

namespace cc {

extern const char* const kCommandNamespace;		// "ClaudeConnector"
extern const char* const kAddOnVersion;

using Handler = std::function<OS (const OS& parameters)>;

struct CommandInfo {
	GS::String		name;
	GS::UniString	description;
	Handler			handler;
};

// Registers a command. Call from a Register*Commands() function (see Commands/Commands.hpp).
void							RegisterCommand (const char* name, const char* description, Handler handler);
const GS::Array<CommandInfo>&	RegisteredCommands ();
const GS::Array<GS::String>&	DuplicateCommandNames ();		// names registered more than once (only the first is kept)
GSErrCode						InstallRegisteredCommands ();

// Runs fn inside ACAPI_CallUndoableCommand (one undo step). cc::Error thrown by fn is
// propagated after the undo scope is closed (the exception never crosses the API boundary).
void							Undoable (const GS::UniString& undoName, const std::function<void ()>& fn);

// Runs fn and converts a thrown cc::Error into an {"error": ...} object. Use it for
// per-item results in batch commands so one bad item does not abort the whole batch.
OS								Try (const std::function<OS ()>& fn);

} // namespace cc
