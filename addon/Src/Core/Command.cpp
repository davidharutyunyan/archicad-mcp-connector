#include "Core/Command.hpp"

#include "APIdefs_Registration.h"

namespace cc {

const char* const kCommandNamespace = "ClaudeConnector";
const char* const kAddOnVersion = "1.0.0";

namespace {

GS::Array<CommandInfo>& Registry ()
{
	static GS::Array<CommandInfo> registry;
	return registry;
}


class JsonCommand : public API_AddOnCommand {
public:
	explicit JsonCommand (const CommandInfo& info) : info (info) {}

	GS::String						GetName () const override				{ return info.name; }
	GS::String						GetNamespace () const override			{ return kCommandNamespace; }
	GS::Optional<GS::UniString>		GetSchemaDefinitions () const override	{ return GS::NoValue; }
	GS::Optional<GS::UniString>		GetInputParametersSchema () const override { return GS::NoValue; }
	GS::Optional<GS::UniString>		GetResponseSchema () const override		{ return GS::NoValue; }
	API_AddOnCommandExecutionPolicy	GetExecutionPolicy () const override	{ return API_AddOnCommandExecutionPolicy::ScheduleForExecutionOnMainThread; }
	bool							IsProcessWindowVisible () const override { return false; }
	void							OnResponseValidationFailed (const GS::ObjectState&) const override {}

	GS::ObjectState Execute (const GS::ObjectState& parameters, GS::ProcessControl&) const override
	{
		try {
			return info.handler (parameters);
		} catch (const Error& e) {
			return ErrorObject (e.code, e.message);
		} catch (const GS::Exception& e) {
			return ErrorObject (APIERR_GENERAL, GS::UniString ("Archicad exception: ") + e.GetMessage ());
		} catch (const std::exception& e) {
			return ErrorObject (APIERR_GENERAL, GS::UniString ("C++ exception: ") + e.what ());
		} catch (...) {
			return ErrorObject (APIERR_GENERAL, "Unknown exception.");
		}
	}

private:
	CommandInfo info;
};

} // namespace


static GS::Array<GS::String>& Duplicates ()
{
	static GS::Array<GS::String> duplicates;
	return duplicates;
}


void RegisterCommand (const char* name, const char* description, Handler handler)
{
	const GS::String commandName (name);
	for (const CommandInfo& info : Registry ()) {
		if (info.name == commandName) {
			// Installing two handlers with the same name makes ACAPI_Install_AddOnCommandHandler
			// fail (and Initialize with it): keep the first registration only.
			Duplicates ().Push (commandName);
			return;
		}
	}
	Registry ().Push ({ commandName, GS::UniString (description, CC_UTF8), std::move (handler) });
}


const GS::Array<GS::String>& DuplicateCommandNames ()
{
	return Duplicates ();
}


const GS::Array<CommandInfo>& RegisteredCommands ()
{
	return Registry ();
}


GSErrCode InstallRegisteredCommands ()
{
	GSErrCode result = NoError;
	for (const CommandInfo& info : Registry ()) {
		GSErrCode err = ACAPI_Install_AddOnCommandHandler (GS::NewOwned<JsonCommand> (info));
		if (err != NoError)
			result = err;
	}
	return result;
}


void Undoable (const GS::UniString& undoName, const std::function<void ()>& fn)
{
	bool			failed = false;
	GS::UniString	failMessage;
	GSErrCode		failCode = NoError;

	GSErrCode err = ACAPI_CallUndoableCommand (undoName, [&] () -> GSErrCode {
		try {
			fn ();
		} catch (const Error& e) {
			failed = true; failMessage = e.message; failCode = e.code;
		} catch (const GS::Exception& e) {
			failed = true; failMessage = GS::UniString ("Archicad exception: ") + e.GetMessage (); failCode = APIERR_GENERAL;
		} catch (const std::exception& e) {
			failed = true; failMessage = GS::UniString ("C++ exception: ") + e.what (); failCode = APIERR_GENERAL;
		} catch (...) {
			failed = true; failMessage = "Unknown exception."; failCode = APIERR_GENERAL;
		}
		// A non-NoError result makes Archicad cancel the undo step, so partial changes made
		// before the failure are rolled back instead of being committed.
		return failed ? (failCode != NoError ? failCode : APIERR_GENERAL) : NoError;
	});

	if (failed)
		throw Error (failMessage, failCode);
	Check (err, "Undoable command '" + undoName + "' failed");
}


OS Try (const std::function<OS ()>& fn)
{
	try {
		return fn ();
	} catch (const Error& e) {
		return ErrorObject (e.code, e.message);
	} catch (const GS::Exception& e) {
		return ErrorObject (APIERR_GENERAL, GS::UniString ("Archicad exception: ") + e.GetMessage ());
	} catch (const std::exception& e) {
		return ErrorObject (APIERR_GENERAL, GS::UniString ("C++ exception: ") + e.what ());
	}
}

} // namespace cc
