// *****************************************************************************
// Claude Connector — Archicad 26 add-on entry points.
// *****************************************************************************

#include "APIEnvir.h"
#include "ACAPinc.h"

#include "Core/Command.hpp"
#include "Core/Elements.hpp"
#include "Commands/Commands.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>

#define CLAUDE_CONNECTOR_NAME_RES_ID	32000

// Startup diagnostics: ~/Library/Logs/ClaudeConnector.log
static void StartupLog (const char* message, GSErrCode err = NoError)
{
	const char* home = std::getenv ("HOME");
	if (home == nullptr)
		return;
	char path[1024];
	std::snprintf (path, sizeof (path), "%s/Library/Logs/ClaudeConnector.log", home);
	FILE* f = std::fopen (path, "a");
	if (f == nullptr)
		return;
	std::time_t now = std::time (nullptr);
	char stamp[32];
	std::strftime (stamp, sizeof (stamp), "%Y-%m-%d %H:%M:%S", std::localtime (&now));
	std::fprintf (f, "%s  %s (err=%d)\n", stamp, message, (int) err);
	std::fclose (f);
}

API_AddonType __ACDLL_CALL CheckEnvironment (API_EnvirParams* envir)
{
	RSGetIndString (&envir->addOnInfo.name, CLAUDE_CONNECTOR_NAME_RES_ID, 1, ACAPI_GetOwnResModule ());
	RSGetIndString (&envir->addOnInfo.description, CLAUDE_CONNECTOR_NAME_RES_ID, 2, ACAPI_GetOwnResModule ());
	StartupLog ("CheckEnvironment");
	return APIAddon_Preload;
}


GSErrCode __ACDLL_CALL RegisterInterface (void)
{
	StartupLog ("RegisterInterface");
	return NoError;
}


GSErrCode __ACENV_CALL Initialize (void)
{
	cc::RegisterSystemCommands ();
	cc::RegisterGenericElementCommands ();
	cc::RegisterProjectCommands ();
	cc::RegisterStoryCommands ();
	cc::RegisterAttributeCommands ();
	cc::RegisterElementQueryCommands ();
	cc::RegisterElementEditCommands ();
	cc::RegisterWallCommands ();
	cc::RegisterColumnBeamCommands ();
	cc::RegisterSlabRoofCommands ();
	cc::RegisterOpeningCommands ();
	cc::RegisterObjectLibraryCommands ();
	cc::RegisterZoneCommands ();
	cc::RegisterDraftingCommands ();
	cc::RegisterDimensionCommands ();
	cc::RegisterComplexElementCommands ();
	cc::RegisterViewCommands ();
	cc::RegisterDocumentationCommands ();
	cc::RegisterPropertyCommands ();
	cc::RegisterCollaborationCommands ();

	// Command handlers live in this binary: the add-on must stay loaded for them to remain registered.
	ACAPI_KeepInMemory (true);

	GSErrCode err = cc::InstallRegisteredCommands ();
	char msg[128];
	std::snprintf (msg, sizeof (msg), "Initialize: %u commands installed", (unsigned) cc::RegisteredCommands ().GetSize ());
	StartupLog (msg, err);
	for (const GS::String& name : cc::DuplicateCommandNames ()) {
		std::snprintf (msg, sizeof (msg), "Duplicate command name ignored: %s", name.ToCStr ());
		StartupLog (msg);
	}
	return err;
}


GSErrCode __ACENV_CALL FreeData (void)
{
	StartupLog ("FreeData");
	return NoError;
}
