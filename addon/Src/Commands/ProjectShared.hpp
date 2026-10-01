// *****************************************************************************
// ProjectShared.hpp — internal helpers shared by the "project" family files
// (Project.cpp and the implementation files it includes: ProjectSettings.inl.hpp,
// ProjectEditMenu.inl.hpp). Not part of Core.
// *****************************************************************************

#pragma once

#include "Core/Command.hpp"
#include "Core/Json.hpp"

#include <functional>

namespace cc {
namespace project {

// --- Optional undo scope ---------------------------------------------------------
//
// Some AC26 setters (project info autotexts, preferences, place/geo settings) are
// documented inconsistently as to whether they need an undo scope. ModifyCall runs the
// API call directly; when Archicad answers APIERR_NEEDSUNDOSCOPE (which it does before
// changing anything) it throws NeedsUndoScope, and WithOptionalUndo re-runs the whole
// body inside ONE undo step. Bodies must therefore be idempotent (all our setters are
// "set field to value" operations and re-resolve their targets).

struct NeedsUndoScope {};

// Calls fn; throws NeedsUndoScope when the API refuses to run outside an undo scope.
GSErrCode	ModifyCall (const std::function<GSErrCode ()>& fn);

// Runs body directly, or (after NeedsUndoScope) inside Undoable (undoName, ...).
OS			WithOptionalUndo (const GS::UniString& undoName, const std::function<OS ()>& body);

// --- Shared registration -----------------------------------------------------------

void		RegisterSettingsCommands ();	// ProjectSettings.inl.hpp: preferences + geo location
void		RegisterEditMenuCommands ();	// ProjectEditMenu.inl.hpp: undo / redo

} // namespace project
} // namespace cc
