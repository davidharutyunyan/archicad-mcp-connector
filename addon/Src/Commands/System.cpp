// System commands: connectivity check, add-on info and the command catalogue.

#include "Commands/Commands.hpp"
#include "Core/Command.hpp"

namespace cc {

void RegisterSystemCommands ()
{
	RegisterCommand ("Ping",
		"Health check. Returns {ok: true, version}.",
		[] (const OS&) -> OS {
			return OS ("ok", true, "version", GS::UniString (kAddOnVersion));
		});

	RegisterCommand ("GetAddOnInfo",
		"Returns add-on version, Archicad version/build/language and the number of registered commands.",
		[] (const OS&) -> OS {
			API_ServerApplicationInfo info;		// has UniString members: do not memset
			ACAPI_GetReleaseNumber (&info);
			OS out;
			out.Add ("addOnVersion", GS::UniString (kAddOnVersion));
			out.Add ("commandNamespace", GS::UniString (kCommandNamespace));
			out.Add ("archicadMainVersion", (Int32) info.mainVersion);
			out.Add ("archicadBuild", (Int32) info.buildNum);
			out.Add ("archicadLanguage", info.language);
			out.Add ("commandCount", (Int32) RegisteredCommands ().GetSize ());
			return out;
		});

	RegisterCommand ("QuitArchicad",
		"Quits Archicad WITHOUT saving (discarding unsaved changes). Input: {confirm: true}. The HTTP connection may drop before a response arrives.",
		[] (const OS& params) -> OS {
			if (!GetBool (params, "confirm", false))
				Fail ("Pass {\"confirm\": true} to quit Archicad without saving.");
			GSErrCode err = ACAPI_Automate (APIDo_QuitID, reinterpret_cast<void*> ((GS::IntPtr) 1234));
			Check (err, "Quit failed");
			return OS ("ok", true);
		});

	RegisterCommand ("ListCommands",
		"Lists every command registered by this add-on with its description.",
		[] (const OS&) -> OS {
			GS::Array<OS> list;
			for (const CommandInfo& info : RegisteredCommands ())
				list.Push (OS ("name", GS::UniString (info.name.ToCStr ()), "description", info.description));
			return OS ("commands", list);
		});
}

} // namespace cc
