// *****************************************************************************
// Project commands — project / application information, Project Info fields
// (autotexts), Save / Save As / Open / New / Close, Rebuild / Redraw.
//
// Preferences and geo location live in ProjectSettings.inl.hpp, undo / redo in
// ProjectEditMenu.inl.hpp; both are implementation files included at the end of this
// file (one translation unit, so no CMake re-configure is needed for this family).
//
// Commands:
//   GetProjectInfo          {includeTemplates?}
//   GetProjectInfoFields    {category?, search?, nonEmptyOnly?}
//   SetProjectInfoFields    {fields: [{key|name, value}], createIfMissing?}
//   DeleteProjectInfoFields {fields: [key|name]}             (custom fields only)
//   SaveProject             {}
//   SaveProjectAs           {path, format?, overwrite?, createFolders?, archive?}
//   OpenProject             {path, saveFirst?|discardChanges?, archiveLibraryFolder?}
//   NewProject              {template?, reset?, saveFirst?|discardChanges?}
//   CloseProject            {confirm: true, saveFirst?|discardChanges?}
//   RebuildModel            {mode?: "Rebuild"|"Regenerate"|"Redraw"}
// *****************************************************************************

#include "Commands/Commands.hpp"
#include "Commands/ProjectShared.hpp"
#include "Core/Command.hpp"
#include "Core/Enums.hpp"
#include "Core/Types.hpp"

#include "FileSystem.hpp"
#include "Folder.hpp"
#include "GSGuid.hpp"
#include "Location.hpp"
#include "Name.hpp"

#include <cstdio>
#include <string>

#if !defined (WINDOWS)
#include <unistd.h>
#endif

namespace cc {

// =============================================================================
// Optional undo scope (see ProjectShared.hpp)
// =============================================================================

namespace project {

namespace {
	bool gInUndoScope = false;
}


GSErrCode ModifyCall (const std::function<GSErrCode ()>& fn)
{
	const GSErrCode err = fn ();
	if (err == APIERR_NEEDSUNDOSCOPE && !gInUndoScope)
		throw NeedsUndoScope {};
	return err;
}


OS WithOptionalUndo (const GS::UniString& undoName, const std::function<OS ()>& body)
{
	if (gInUndoScope)
		return body ();

	try {
		return body ();
	} catch (const NeedsUndoScope&) {
		// The first modifying call was refused before changing anything: run everything in one undo step.
	}

	OS result;
	Undoable (undoName, [&] () {
		struct ScopeFlag {
			ScopeFlag ()	{ gInUndoScope = true; }
			~ScopeFlag ()	{ gInUndoScope = false; }
		} flag;
		result = body ();
	});
	return result;
}

} // namespace project


namespace {

// =============================================================================
// Files & locations
// =============================================================================

GS::UniString LocationToPath (const IO::Location& loc)
{
	GS::UniString path;
	if (loc.ToPath (&path) != NoError || path.IsEmpty ())
		path = loc.ToDisplayText ();
	return path;
}


GS::UniString FileExtension (const GS::UniString& path)
{
	const UIndex slash = path.FindLast (GS::UniChar ('/'));
	const UIndex dot = path.FindLast (GS::UniChar ('.'));
	if (dot == MaxUIndex || (slash != MaxUIndex && dot < slash) || dot + 1 >= path.GetLength ())
		return GS::UniString ();
	return GS::UniString (path.GetSubstring (dot + 1, path.GetLength () - dot - 1)).ToLowerCase ();
}


GS::UniString FileTypeName (const GS::UniString& ext)
{
	if (ext == "pln")	return "SoloProject";
	if (ext == "pla")	return "Archive";
	if (ext == "tpl")	return "Template";
	if (ext == "bpn")	return "Backup";
	if (ext == "plp")	return "LegacyTeamworkProject";
	if (ext.IsEmpty ())	return "None";
	return "Other";
}


bool LocationExists (const IO::Location& loc)
{
	bool contains = false;
	return IO::fileSystem.Contains (loc, &contains) == NoError && contains;
}


bool IsAbsolutePath (const GS::UniString& path)
{
#if defined (WINDOWS)
	return path.GetLength () > 2 && (path[1] == ':' || path.BeginsWith ("\\\\"));
#else
	return path.BeginsWith (GS::UniChar ('/'));
#endif
}


bool IsWritable (const GS::UniString& path)
{
#if defined (WINDOWS)
	return true;
#else
	const GS::String p = ToStr (path);
	return ::access (p.ToCStr (), W_OK) == 0;
#endif
}


IO::Location LocationFromPath (const GS::UniString& path, const char* key)
{
	if (path.IsEmpty ())
		Fail ("'" + GS::UniString (key) + "' must not be empty.");
	if (!IsAbsolutePath (path))
		Fail ("'" + GS::UniString (key) + "' must be an absolute path (e.g. /Users/me/Projects/House.pln), got '" + path + "'.");
	IO::Location loc (path);
	if (loc.GetStatus () != NoError)
		Fail ("Invalid path '" + path + "'.");
	return loc;
}


OS FileJson (const IO::Location& loc)
{
	OS out;
	const GS::UniString path = LocationToPath (loc);
	out.Add ("path", path);
	IO::Name name;
	if (loc.GetLastLocalName (&name) == NoError)
		out.Add ("fileName", name.ToString ());
	IO::Location folder (loc);
	if (folder.DeleteLastLocalName () == NoError)
		out.Add ("folder", LocationToPath (folder));
	const GS::UniString ext = FileExtension (path);
	out.Add ("extension", ext);
	out.Add ("fileType", FileTypeName (ext));
	const bool exists = LocationExists (loc);
	out.Add ("exists", exists);
	if (exists)
		out.Add ("writable", IsWritable (path));
	return out;
}

// =============================================================================
// Project / application info
// =============================================================================

const NamedValue kWindowTypes[] = {
	{ "None",					API_ZombieWindowID },
	{ "FloorPlan",				APIWind_FloorPlanID },
	{ "Section",				APIWind_SectionID },
	{ "Detail",					APIWind_DetailID },
	{ "3DModel",				APIWind_3DModelID },
	{ "Layout",					APIWind_LayoutID },
	{ "Drawing",				APIWind_DrawingID },
	{ "CustomText",				APIWind_MyTextID },
	{ "CustomDraw",				APIWind_MyDrawID },
	{ "MasterLayout",			APIWind_MasterLayoutID },
	{ "Elevation",				APIWind_ElevationID },
	{ "InteriorElevation",		APIWind_InteriorElevationID },
	{ "Worksheet",				APIWind_WorksheetID },
	{ "Report",					APIWind_ReportID },
	{ "DocumentFrom3D",			APIWind_DocumentFrom3DID },
	{ "External3D",				APIWind_External3DID },
	{ "Movie3D",				APIWind_Movie3DID },
	{ "MovieRendering",			APIWind_MovieRenderingID },
	{ "Rendering",				APIWind_RenderingID },
	{ "ModelCompare",			APIWind_ModelCompareID },
	{ "InteractiveSchedule",	APIWind_IESCommonDrawingID },
};


OS WindowInfoJson (const API_WindowInfo& info)
{
	OS out;
	out.Add ("type", NameOf (kWindowTypes, info.typeID));
	out.Add ("index", (Int32) info.index);
	out.Add ("databaseGuid", GuidStr (info.databaseUnId.elemSetId));
	out.Add ("title", GS::UniString (info.title));
	out.Add ("name", GS::UniString (info.name));
	out.Add ("reference", GS::UniString (info.ref));
	if (info.linkedElement != APINULLGuid)
		out.Add ("linkedElement", GuidStr (info.linkedElement));
	return out;
}


// Basic project facts (also returned after save / open / new).
OS ProjectSummary ()
{
	OS out;
	API_ProjectInfo info;
	const GSErrCode err = ACAPI_Environment (APIEnv_ProjectID, &info);
	if (err == APIERR_NOPLAN) {
		out.Add ("projectOpen", false);
		return out;
	}
	Check (err, "Cannot read project info");

	out.Add ("projectOpen", true);
	out.Add ("untitled", info.untitled);
	out.Add ("teamwork", info.teamwork);
	if (info.projectName != nullptr)
		out.Add ("projectName", *info.projectName);
	if (info.projectPath != nullptr && !info.projectPath->IsEmpty ())
		out.Add ("projectPath", *info.projectPath);
	if (!info.untitled && info.location != nullptr)
		out.Add ("file", FileJson (*info.location));
	if (info.teamwork) {
		OS tw;
		tw.Add ("userId", (Int32) info.userId);
		tw.Add ("workGroupMode", (Int32) info.workGroupMode);
		if (info.location_team != nullptr)
			tw.Add ("serverLocation", info.location_team->ToLogText ());
		out.Add ("teamworkInfo", tw);
	}
	out.Add ("modificationStamp", GS::UniString (std::to_string ((unsigned long long) info.modiStamp).c_str ()));
	return out;
}


struct SpecFolder {
	const char*			name;
	API_SpecFolderID	id;
};

const SpecFolder kSpecFolders[] = {
	{ "application",				API_ApplicationFolderID },
	{ "defaults",					API_DefaultsFolderID },
	{ "templates",					API_TemplatesFolderID },
	{ "userDocuments",				API_UserDocumentsFolderID },
	{ "temporary",					API_TemporaryFolderID },
	{ "graphisoftHome",				API_GraphisoftHomeFolderID },
	{ "applicationPreferences",		API_ApplicationPrefsFolderID },
	{ "graphisoftPreferences",		API_GraphisoftPrefsFolderID },
	{ "cache",						API_CacheFolderID },
	{ "data",						API_DataFolderID },
	{ "webObjects",					API_WebObjectsFolderID },
	{ "help",						API_HelpFolderID },
	{ "embeddedProjectLibrary",		API_EmbeddedProjectLibraryFolderID },
};


bool GetSpecFolder (API_SpecFolderID id, IO::Location& loc)
{
	API_SpecFolderID folderId = id;
	return ACAPI_Environment (APIEnv_GetSpecFolderID, &folderId, &loc) == NoError && !loc.IsEmpty ();
}


// Collects *.tpl files below root: breadth-first, depth-limited, with a budget of enumerated
// folders so that scanning an installation folder never walks the whole library.
void CollectTemplates (const IO::Location& root, Int32 maxDepth, GS::Array<GS::UniString>& out)
{
	struct Pending {
		IO::Location	loc;
		Int32			depth;
	};
	GS::Array<Pending> queue;
	queue.Push ({ root, 0 });
	UIndex next = 0;
	Int32 budget = 400;
	while (next < queue.GetSize () && budget-- > 0 && out.GetSize () < 100) {
		const Pending current = queue[next++];
		IO::Folder folder (current.loc);
		if (folder.GetStatus () != NoError)
			continue;
		folder.Enumerate ([&] (const IO::Name& name, bool isFolder) {
			const GS::UniString ext = FileExtension (name.ToString ());
			if (isFolder) {
				// never descend into bundles (.app, .bundle, .framework ...)
				const bool bundle = ext == "app" || ext == "bundle" || ext == "framework" || ext == "plugin" || ext == "lcf";
				if (!bundle && current.depth < maxDepth)
					queue.Push ({ IO::Location (current.loc, name), current.depth + 1 });
			} else if (ext == "tpl") {
				out.Push (LocationToPath (IO::Location (current.loc, name)));
			}
		});
	}
}


OS ApplicationJson ()
{
	OS app;
	API_ServerApplicationInfo serverInfo;
	ACAPI_GetReleaseNumber (&serverInfo);
	app.Add ("mainVersion", (Int32) serverInfo.mainVersion);
	app.Add ("releaseVersion", (Int32) serverInfo.releaseVersion);
	app.Add ("buildNumber", (Int32) serverInfo.buildNum);
	app.Add ("language", serverInfo.language);
	if (!serverInfo.partnerID.IsEmpty ())
		app.Add ("partnerId", serverInfo.partnerID);
	app.Add ("runningInBackground", serverInfo.runningInBackground);

	API_MiscAppInfo misc = {};
	if (ACAPI_Environment (APIEnv_GetMiscAppInfoID, &misc) == NoError) {
		misc.version[sizeof (misc.version) - 1] = '\0';
		misc.caption[sizeof (misc.caption) - 1] = '\0';
		app.Add ("versionString", GS::UniString (misc.version));
		app.Add ("windowCaption", GS::UniString (misc.caption));
	}

	UShort port = 0;
	if (ACAPI_Goodies (APIAny_GetHttpConnectionPortID, &port) == NoError && port != 0)
		app.Add ("jsonApiPort", (Int32) port);
	return app;
}


OS GetProjectInfo (const OS& params)
{
	OS out = ProjectSummary ();
	if (!GetBool (out, "projectOpen", false)) {
		out.Add ("application", ApplicationJson ());
		return out;
	}

	API_WindowInfo window = {};
	if (ACAPI_Database (APIDb_GetCurrentWindowID, &window) == NoError)
		out.Add ("currentWindow", WindowInfoJson (window));
	API_DatabaseInfo database = {};
	if (ACAPI_Database (APIDb_GetCurrentDatabaseID, &database) == NoError)
		out.Add ("currentDatabase", WindowInfoJson (database));

	const short story = CurrentStoryIndex ();
	out.Add ("currentStory", OS ("index", (Int32) story, "name", StoryName (story), "level", StoryLevel (story)));

	out.Add ("application", ApplicationJson ());

	OS folders;
	for (const SpecFolder& sf : kSpecFolders) {
		IO::Location loc;
		if (GetSpecFolder (sf.id, loc))
			folders.Add (sf.name, LocationToPath (loc));
	}
	API_SafetyPrefs safety = {};
	if (ACAPI_Environment (APIEnv_GetPreferencesID, &safety, reinterpret_cast<void*> (APIPrefs_DataSafetyID)) == NoError && safety.tempFolder != nullptr) {
		folders.Add ("projectTemporary", LocationToPath (*safety.tempFolder));
		delete safety.tempFolder;
		safety.tempFolder = nullptr;
	}
	out.Add ("folders", folders);

	if (GetBool (params, "includeTemplates", false)) {
		GS::Array<GS::UniString> templates;
		IO::Location loc;
		if (GetSpecFolder (API_TemplatesFolderID, loc))
			CollectTemplates (loc, 3, templates);
		if (GetSpecFolder (API_DefaultsFolderID, loc))
			CollectTemplates (loc, 3, templates);
		if (GetSpecFolder (API_ApplicationFolderID, loc)) {
			// Installed templates live next to the application bundle, in the (localized) Defaults folder,
			// e.g. ".../Archicad 26/Значения по умолчанию/Archicad/AC26-Шаблон.tpl".
			IO::Location installRoot (loc);
			IO::Name last;
			if (installRoot.GetLastLocalName (&last) == NoError && FileExtension (last.ToString ()) == "app")
				installRoot.DeleteLastLocalName ();
			CollectTemplates (installRoot, 2, templates);
		}
		GS::Array<GS::UniString> unique;
		for (const GS::UniString& t : templates) {
			if (!unique.Contains (t))
				unique.Push (t);
		}
		out.Add ("templates", unique);
	}
	return out;
}

// =============================================================================
// Project Info fields (autotexts)
// =============================================================================

struct AutoTextEntry {
	GS::UniString	name;		// user interface name (localized)
	GS::UniString	key;		// database key, e.g. "PROJECTNAME", "autotext-<GUID>"
	GS::UniString	value;
	GS::UniString	category;	// Fixed | Custom | Other
};

struct AutoTextKind {
	API_AutotextType	type;
	const char*			name;
};

const AutoTextKind kAutoTextKinds[] = {
	{ APIAutoText_Fixed,	"Fixed" },
	{ APIAutoText_Custom,	"Custom" },
	{ APIAutoText_Other,	"Other" },
};


GS::Array<AutoTextEntry> LoadAutoTexts (const GS::UniString& category = "All")
{
	GS::Array<AutoTextEntry> result;
	for (const AutoTextKind& kind : kAutoTextKinds) {
		if (category != "All" && !EqualsIgnoreCase (category, kind.name))
			continue;
		GS::Array<GS::ArrayFB<GS::UniString, 3>> texts;
		const GSErrCode err = ACAPI_Goodies (APIAny_GetAutoTextsID, &texts, reinterpret_cast<void*> ((GS::IntPtr) kind.type));
		Check (err, GS::UniString ("Cannot read Project Info (") + kind.name + " autotexts)");
		for (const auto& t : texts) {
			if (t.GetSize () < 3)
				continue;
			result.Push ({ t[0], t[1], t[2], kind.name });
		}
	}
	return result;
}


OS AutoTextJson (const AutoTextEntry& e)
{
	return OS ("name", e.name, "key", e.key, "value", e.value, "category", e.category);
}


const AutoTextEntry* FindByKey (const GS::Array<AutoTextEntry>& all, const GS::UniString& key)
{
	for (const AutoTextEntry& e : all) {
		if (e.key == key)
			return &e;
	}
	for (const AutoTextEntry& e : all) {
		if (EqualsIgnoreCase (e.key, key))
			return &e;
	}
	return nullptr;
}


// Finds a field by its UI name. Throws when the name is ambiguous.
const AutoTextEntry* FindByName (const GS::Array<AutoTextEntry>& all, const GS::UniString& name)
{
	for (int pass = 0; pass < 2; ++pass) {
		const AutoTextEntry* found = nullptr;
		GS::UniString keys;
		Int32 count = 0;
		for (const AutoTextEntry& e : all) {
			const bool match = pass == 0 ? (e.name == name) : EqualsIgnoreCase (e.name, name);
			if (!match)
				continue;
			if (found == nullptr)
				found = &e;
			if (!keys.IsEmpty ())
				keys += ", ";
			keys += e.key;
			++count;
		}
		if (count > 1)
			Fail ("Several Project Info fields are named '" + name + "' (keys: " + keys + "). Pass 'key' instead of 'name'.", APIERR_BADNAME);
		if (found != nullptr)
			return found;
	}
	return nullptr;
}


GS::UniString FieldValueString (const OS& item)
{
	if (!item.Contains ("value"))
		Fail ("Each field needs a 'value' (string; use \"\" to clear it).");
	if (item.IsString ("value"))
		return GetString (item, "value");
	if (item.IsBool ("value"))
		return GetBool (item, "value") ? "true" : "false";
	if (IsNumber (item, "value")) {
		// NOTE: GS::UniString::Printf is static; format with snprintf.
		char buf[64];
		std::snprintf (buf, sizeof (buf), "%.10g", GetDouble (item, "value"));
		return GS::UniString (buf);
	}
	Fail ("Field 'value' must be a string.");
}


// Creates a custom Project Info field and returns its database key.
GS::UniString CreateCustomField (const GS::UniString& name)
{
	GS::Guid guid;
	guid.Generate ();
	API_Guid apiGuid = GSGuid2APIGuid (guid);
	const GS::String uiKey (name.ToCStr ().Get ());
	const GSErrCode err = project::ModifyCall ([&] () {
		return ACAPI_Goodies (APIAny_CreateAnAutoTextID, &apiGuid, const_cast<char*> (uiKey.ToCStr ()));
	});
	Check (err, "Cannot create custom Project Info field '" + name + "'");

	const GS::UniString expectedKey = GS::UniString ("autotext-") + guid.ToUniString ();
	const GS::Array<AutoTextEntry> custom = LoadAutoTexts ("Custom");
	if (const AutoTextEntry* e = FindByKey (custom, expectedKey))
		return e->key;
	// Fallback: the newest custom field with this name.
	for (UIndex i = custom.GetSize (); i > 0; --i) {
		if (EqualsIgnoreCase (custom[i - 1].name, name))
			return custom[i - 1].key;
	}
	return expectedKey;
}


OS SetOneField (const OS& item, bool createIfMissing)
{
	const auto key = OptString (item, "key");
	const auto name = OptString (item, "name");
	if (!key.has_value () && !name.has_value ())
		Fail ("Each field needs 'key' (database key from get_project_info_fields) or 'name' (the localized field name).");
	GS::UniString value = FieldValueString (item);

	const GS::Array<AutoTextEntry> all = LoadAutoTexts ();
	const AutoTextEntry* entry = nullptr;
	if (key.has_value ()) {
		entry = FindByKey (all, *key);
		if (entry == nullptr)
			Fail ("Unknown Project Info key '" + *key + "'. Call get_project_info_fields to list the keys.", APIERR_BADNAME);
	} else {
		entry = FindByName (all, *name);
	}

	GS::UniString dbKey;
	bool created = false;
	if (entry != nullptr) {
		dbKey = entry->key;
	} else if (createIfMissing) {
		dbKey = CreateCustomField (*name);
		created = true;
	} else {
		Fail ("No Project Info field named '" + *name + "'. Names are localized: call get_project_info_fields, or pass createIfMissing: true to add a custom field.", APIERR_BADNAME);
	}

	const GSErrCode err = project::ModifyCall ([&] () {
		return ACAPI_Goodies (APIAny_SetAnAutoTextID, &dbKey, &value);
	});
	if (err != NoError) {
		GS::UniString hint;
		if (entry != nullptr && entry->category == "Other")
			hint = " Fields of category 'Other' are computed by Archicad (dates, layout/drawing data) and usually cannot be set.";
		if (err == APIERR_NOTEDITABLE || err == APIERR_NOACCESSRIGHT)
			hint += " In Teamwork, reserve Project Info first.";
		Check (err, "Cannot set Project Info field '" + dbKey + "'." + hint);
	}

	OS result;
	const GS::Array<AutoTextEntry> after = LoadAutoTexts (entry != nullptr ? entry->category : GS::UniString ("Custom"));
	if (const AutoTextEntry* e = FindByKey (after, dbKey)) {
		if (e->value != value) {
			// APIAny_SetAnAutoTextID reports success for computed fields but keeps their value (verified live: SHORTDATE).
			if (e->category == "Other")
				Fail ("Archicad ignored the new value of '" + e->name + "' (" + e->key + "): fields of category 'Other' are computed by Archicad " +
					  "(dates, file name/path, layout/drawing/revision data) and cannot be set. Current value: '" + e->value + "'.", APIERR_REFUSEDPAR);
			result = AutoTextJson (*e);
			result.Add ("warning", GS::UniString ("Archicad stored '" + e->value + "' instead of the requested '" + value + "'."));
		} else {
			result = AutoTextJson (*e);
		}
	} else {
		result = OS ("key", dbKey, "value", value);
	}
	if (created)
		result.Add ("created", true);
	return result;
}

// =============================================================================
// Save / open / new / close
// =============================================================================

API_ProjectInfo* ReadProjectInfo (API_ProjectInfo& info)
{
	const GSErrCode err = ACAPI_Environment (APIEnv_ProjectID, &info);
	if (err == APIERR_NOPLAN)
		Fail ("No project is open in Archicad.", err);
	Check (err, "Cannot read project info");
	return &info;
}


void SaveCurrentProject ()
{
	API_ProjectInfo info;
	ReadProjectInfo (info);
	if (info.untitled)
		Fail ("The current project has never been saved (untitled), so it cannot be saved in place. Use save_project_as with a full .pln path.", APIERR_READONLY);
	const GSErrCode err = ACAPI_Automate (APIDo_SaveID);
	if (err == APIERR_READONLY)
		Fail ("Archicad refused to save: the project is read-only (opened read-only, locked by another user/instance, or write-protected). "
			  "If no other Archicad has it open (e.g. a stale lock after Archicad was restarted), save_project_as to the SAME path with "
			  "overwrite: true clears it (verified live); otherwise use save_project_as with another path.", err);
	if (err == APIERR_REFUSEDCMD)
		Fail ("Archicad refused to save right now (a dialog or another operation may be active in Archicad). Close open dialogs and retry.", err);
	Check (err, "Save failed");
}


// Handles the saveFirst / discardChanges options of operations that close the current project.
void HandleUnsavedChanges (const OS& params, const char* operation)
{
	const bool saveFirst = GetBool (params, "saveFirst", false);
	const bool discard = GetBool (params, "discardChanges", false);
	if (saveFirst && discard)
		Fail ("Pass either saveFirst: true or discardChanges: true, not both.");
	if (!saveFirst && !discard)
		Fail (GS::UniString (operation) + " closes the current project. Decide what happens to its unsaved changes: pass saveFirst: true (save the current project first; it must have been saved before) or discardChanges: true (unsaved changes are lost).");
	if (saveFirst)
		SaveCurrentProject ();
}


struct SaveFormat {
	const char*		name;
	API_FTypeID		type;
	const char*		extension;
	bool			archive;
	const char*		description;
};

const SaveFormat kSaveFormats[] = {
	{ "pln",	APIFType_PlanFile,			"pln",	false,	"Archicad solo project" },
	{ "pla",	APIFType_A_PlanFile,		"pla",	true,	"Archicad archive (project + embedded library parts)" },
	{ "tpl",	APIFType_PlanFile,			"tpl",	false,	"Archicad template (written in project format with a .tpl extension)" },
	{ "pln25",	APIFType_PlanFile2500,		"pln",	false,	"Archicad 25 project (save as previous version)" },
	{ "pla25",	APIFType_A_PlanFile2500,	"pla",	true,	"Archicad 25 archive (save as previous version)" },
};


const SaveFormat& ResolveSaveFormat (const std::optional<GS::UniString>& format, const GS::UniString& ext)
{
	if (format.has_value ()) {
		for (const SaveFormat& f : kSaveFormats) {
			if (EqualsIgnoreCase (*format, f.name))
				return f;
		}
		Fail ("Unknown format '" + *format + "'. Allowed: pln, pla, tpl, pln25, pla25 (IFC/DWG/PDF/images: use the export tools).");
	}
	if (ext.IsEmpty () || ext == "pln")
		return kSaveFormats[0];
	if (ext == "pla")
		return kSaveFormats[1];
	if (ext == "tpl")
		return kSaveFormats[2];
	Fail ("Cannot infer the format from the extension '." + ext + "'. Use a .pln / .pla / .tpl path or pass 'format'.");
}


OS SaveProjectAs (const OS& params)
{
	API_ProjectInfo info;
	ReadProjectInfo (info);

	GS::UniString path = GetString (params, "path");
	GS::UniString ext = FileExtension (path);
	const SaveFormat& format = ResolveSaveFormat (OptString (params, "format"), ext);
	if (ext.IsEmpty ()) {
		path += GS::UniString (".") + format.extension;
		ext = format.extension;
	} else if (ext != format.extension) {
		Fail ("The path extension '." + ext + "' does not match format '" + format.name + "' (expected ." + format.extension + ").");
	}

	IO::Location loc = LocationFromPath (path, "path");
	IO::Location parent (loc);
	parent.DeleteLastLocalName ();
	if (!LocationExists (parent)) {
		if (!GetBool (params, "createFolders", false))
			Fail ("The folder '" + LocationToPath (parent) + "' does not exist. Pass createFolders: true to create it.", APIERR_BADPARS);
		Check (IO::fileSystem.CreateFolderTree (parent), "Cannot create folder '" + LocationToPath (parent) + "'");
	}
	if (LocationExists (loc) && !GetBool (params, "overwrite", false))
		Fail ("The file '" + path + "' already exists. Pass overwrite: true to replace it.", APIERR_BADPARS);

	API_FileSavePars fsp = {};
	fsp.fileTypeID = format.type;
	fsp.file = &loc;

	GSErrCode err = NoError;
	if (format.archive) {
		OS archive;
		TryGetObject (params, "archive", archive);
		API_SavePars_Archive pars = {};
		pars.texturesOn			= GetBool (archive, "includeTextures", true);
		pars.backgroundPictOn	= GetBool (archive, "includeBackgroundPictures", true);
		pars.propertiesOn		= GetBool (archive, "includePropertyObjects", true);
		pars.libraryPartsOn		= GetBool (archive, "includeAllLibraryParts", false);
		pars.picturesInTIFF		= GetBool (archive, "picturesInTIFF", false);
		err = ACAPI_Automate (APIDo_SaveID, &fsp, &pars);
	} else {
		err = ACAPI_Automate (APIDo_SaveID, &fsp);
	}
	if (err == APIERR_READONLY)
		Fail ("Archicad refused to write '" + path + "' (read-only location or file locked).", err);
	if (err == APIERR_REFUSEDCMD)
		Fail ("Archicad refused to save right now (demo/educational restrictions, an open dialog, or another running operation).", err);
	Check (err, "Save As '" + path + "' failed");

	OS out;
	out.Add ("saved", true);
	out.Add ("format", GS::UniString (format.name));
	out.Add ("formatDescription", GS::UniString (format.description));
	out.Add ("file", FileJson (loc));
	out.Add ("project", ProjectSummary ());
	return out;
}


struct OpenType {
	const char*		extension;
	API_FTypeID		type;
};

const OpenType kOpenTypes[] = {
	{ "pln",	APIFType_PlanFile },
	{ "pla",	APIFType_A_PlanFile },
	{ "tpl",	APIFType_PlanFile },
	{ "bpn",	APIFType_Bak_PlanFile },
};


API_FTypeID OpenFileType (const GS::UniString& ext, const GS::UniString& path)
{
	for (const OpenType& t : kOpenTypes) {
		if (ext == t.extension)
			return t.type;
	}
	Fail ("Cannot open '" + path + "': only .pln, .pla, .tpl and .bpn files can be opened as projects (import IFC/DWG with the import tools; BIMcloud Teamwork projects cannot be opened by path).");
}


GSErrCode OpenProjectFile (IO::Location& loc, API_FTypeID type, IO::Location* archiveLib)
{
	API_FileOpenPars fop = {};
	fop.fileTypeID = type;
	fop.useStoredLib = true;
	fop.enableSaveAlert = false;
	fop.file = &loc;
	if (archiveLib != nullptr) {
		fop.libGiven = true;
		fop.archiveLib = archiveLib;
	}
	return ACAPI_Automate (APIDo_OpenID, &fop);
}


OS OpenProject (const OS& params)
{
	const GS::UniString path = GetString (params, "path");
	IO::Location loc = LocationFromPath (path, "path");
	if (!LocationExists (loc))
		Fail ("File not found: '" + path + "'.", APIERR_BADPARS);
	const GS::UniString ext = FileExtension (path);
	const API_FTypeID type = OpenFileType (ext, path);

	IO::Location libLoc;
	IO::Location* libPtr = nullptr;
	if (auto lib = OptString (params, "archiveLibraryFolder")) {
		if (ext != "pla")
			Fail ("'archiveLibraryFolder' is only used when opening a .pla archive.");
		libLoc = LocationFromPath (*lib, "archiveLibraryFolder");
		libPtr = &libLoc;
	}

	HandleUnsavedChanges (params, "Opening a project");

	const GSErrCode err = OpenProjectFile (loc, type, libPtr);
	if (err == APIERR_REFUSEDCMD)
		Fail ("Archicad refused to open the project right now (another operation or a dialog is active).", err);
	Check (err, "Cannot open '" + path + "'");
	return OS ("opened", true, "project", ProjectSummary ());
}


OS NewProject (const OS& params)
{
	std::optional<IO::Location> templateLoc;
	if (auto tpl = OptString (params, "template")) {
		IO::Location loc = LocationFromPath (*tpl, "template");
		if (!LocationExists (loc))
			Fail ("Template not found: '" + *tpl + "'. Call get_project_info {includeTemplates: true} to list installed templates.", APIERR_BADPARS);
		if (FileExtension (*tpl) != "tpl")
			Fail ("'template' must be a .tpl file (to open an existing project use open_project).");
		templateLoc = loc;
	}

	HandleUnsavedChanges (params, "Creating a new project");

	GSErrCode err = NoError;
	if (templateLoc.has_value ()) {
		err = OpenProjectFile (*templateLoc, APIFType_PlanFile, nullptr);
	} else {
		API_NewProjectPars npp = {};
		npp.newAndReset = GetBool (params, "reset", false);
		npp.enableSaveAlert = false;
		err = ACAPI_Automate (APIDo_NewProjectID, &npp);
	}
	if (err == APIERR_REFUSEDCMD)
		Fail ("Archicad refused to create a new project right now (another operation or a dialog is active).", err);
	Check (err, "Cannot create a new project");
	return OS ("created", true, "project", ProjectSummary ());
}


OS CloseProject (const OS& params)
{
	if (!GetBool (params, "confirm", false))
		Fail ("Closing the project stops the Archicad JSON API until a project is opened again by hand. Pass confirm: true to proceed.");
	API_ProjectInfo info;
	ReadProjectInfo (info);
	HandleUnsavedChanges (params, "Closing the project");
	const GSErrCode err = ACAPI_Automate (APIDo_CloseID, reinterpret_cast<void*> ((GS::IntPtr) 1234));
	if (err == APIERR_REFUSEDCMD)
		Fail ("Archicad refused to close the project right now (another operation or a dialog is active).", err);
	Check (err, "Cannot close the project");
	return OS ("closed", true);
}

// =============================================================================
// Rebuild / redraw
// =============================================================================

enum RebuildMode { Rebuild_Rebuild, Rebuild_Regenerate, Rebuild_Redraw };

const NamedValue kRebuildModes[] = {
	{ "Rebuild",	Rebuild_Rebuild },
	{ "Regenerate",	Rebuild_Regenerate },
	{ "Redraw",		Rebuild_Redraw },
};


OS RebuildModel (const OS& params)
{
	const Int32 mode = OptNamed (kRebuildModes, params, "mode").value_or (Rebuild_Rebuild);
	GSErrCode err = NoError;
	if (mode == Rebuild_Redraw) {
		err = ACAPI_Automate (APIDo_RedrawID);
	} else {
		bool regenerate = (mode == Rebuild_Regenerate);
		err = ACAPI_Automate (APIDo_RebuildID, &regenerate);
	}
	if (err == APIERR_BADDATABASE || err == APIERR_BADWINDOW)
		Fail ("The current window cannot be rebuilt (" + ErrorName (err) + "). Switch to a model/drawing window and retry.", err);
	Check (err, GS::UniString (NameOf (kRebuildModes, mode)) + " failed");

	OS out ("ok", true, "mode", NameOf (kRebuildModes, mode));
	API_WindowInfo window = {};
	if (ACAPI_Database (APIDb_GetCurrentWindowID, &window) == NoError)
		out.Add ("window", WindowInfoJson (window));
	return out;
}

} // namespace


void RegisterProjectCommands ()
{
	RegisterCommand ("GetProjectInfo",
		"Project facts: name, file (path, type, exists, writable), untitled, Teamwork info, current window/database/story, "
		"Archicad version/build/language/JSON API port, special folders (temp, templates, documents...). "
		"Input: {includeTemplates?: bool} also lists installed *.tpl templates.",
		GetProjectInfo);

	RegisterCommand ("GetProjectInfoFields",
		"Lists Project Info fields (autotexts): [{name (localized UI name), key (database key), value, category: Fixed|Custom|Other}]. "
		"Input: {category?: 'Fixed'|'Custom'|'Other'|'All', search?: substring of name/key/value, nonEmptyOnly?: bool}.",
		[] (const OS& params) -> OS {
			const GS::UniString category = GetString (params, "category", "All");
			if (category != "All" && !EqualsIgnoreCase (category, "Fixed") && !EqualsIgnoreCase (category, "Custom") && !EqualsIgnoreCase (category, "Other") && !EqualsIgnoreCase (category, "All"))
				Fail ("Invalid category '" + category + "'. Allowed: Fixed, Custom, Other, All.");
			const GS::UniString search = GetString (params, "search", "").ToLowerCase ();
			const bool nonEmptyOnly = GetBool (params, "nonEmptyOnly", false);
			GS::Array<OS> fields;
			for (const AutoTextEntry& e : LoadAutoTexts (EqualsIgnoreCase (category, "All") ? GS::UniString ("All") : category)) {
				if (nonEmptyOnly && e.value.IsEmpty ())
					continue;
				if (!search.IsEmpty () && !e.name.ToLowerCase ().Contains (search) && !e.key.ToLowerCase ().Contains (search) && !e.value.ToLowerCase ().Contains (search))
					continue;
				fields.Push (AutoTextJson (e));
			}
			return OS ("fields", fields, "count", (Int32) fields.GetSize ());
		});

	RegisterCommand ("SetProjectInfoFields",
		"Sets Project Info field values. Input: {fields: [{key | name, value}], createIfMissing?: bool (create custom fields for unknown names)}. "
		"Output: {results: [{name, key, value, category, created?} | {error}]}.",
		[] (const OS& params) -> OS {
			const GS::Array<OS> items = GetObjectArray (params, "fields");
			if (items.IsEmpty ())
				Fail ("'fields' must contain at least one {key|name, value} item.");
			const bool createIfMissing = GetBool (params, "createIfMissing", false);
			return project::WithOptionalUndo ("Set Project Info (Claude)", [&] () -> OS {
				GS::Array<OS> results;
				for (const OS& item : items)
					results.Push (Try ([&] () -> OS { return SetOneField (item, createIfMissing); }));
				return OS ("results", results);
			});
		});

	RegisterCommand ("DeleteProjectInfoFields",
		"Deletes CUSTOM Project Info fields. Input: {fields: [key | name]}. Built-in (Fixed/Other) fields cannot be deleted. Output: {results: [{key, deleted} | {error}]}.",
		[] (const OS& params) -> OS {
			const GS::Array<GS::UniString> refs = GetStringArray (params, "fields", true);
			if (refs.IsEmpty ())
				Fail ("'fields' must contain at least one key or name.");
			return project::WithOptionalUndo ("Delete Project Info fields (Claude)", [&] () -> OS {
				GS::Array<OS> results;
				for (const GS::UniString& ref : refs) {
					results.Push (Try ([&] () -> OS {
						const GS::Array<AutoTextEntry> all = LoadAutoTexts ();
						const AutoTextEntry* entry = FindByKey (all, ref);
						if (entry == nullptr)
							entry = FindByName (all, ref);
						if (entry == nullptr)
							Fail ("No Project Info field with key or name '" + ref + "'. Call get_project_info_fields.", APIERR_BADNAME);
						if (entry->category != "Custom")
							Fail ("'" + entry->name + "' (" + entry->key + ") is a built-in " + entry->category + " field; only custom fields can be deleted. Set its value to \"\" to clear it.", APIERR_REFUSEDPAR);
						const GS::UniString key = entry->key;
						const GS::String keyStr = ToStr (key);
						const GSErrCode err = project::ModifyCall ([&] () {
							return ACAPI_Goodies (APIAny_DeleteAnAutoTextID, const_cast<char*> (keyStr.ToCStr ()));
						});
						Check (err, "Cannot delete Project Info field '" + key + "'");
						return OS ("key", key, "deleted", true);
					}));
				}
				return OS ("results", results);
			});
		});

	RegisterCommand ("SaveProject",
		"Saves the current project to its own file (File > Save). Fails for untitled projects (use SaveProjectAs). Output: {saved, project}.",
		[] (const OS&) -> OS {
			SaveCurrentProject ();
			return OS ("saved", true, "project", ProjectSummary ());
		});

	RegisterCommand ("SaveProjectAs",
		"Saves the project under a new path (File > Save As; the open project then refers to the new file). "
		"Input: {path (absolute), format?: pln|pla|tpl|pln25|pla25 (default from extension), overwrite?, createFolders?, "
		"archive?: {includeTextures, includeBackgroundPictures, includePropertyObjects, includeAllLibraryParts, picturesInTIFF}}.",
		SaveProjectAs);

	RegisterCommand ("OpenProject",
		"Opens a .pln/.pla/.tpl/.bpn file, closing the current project. Input: {path, saveFirst?: bool | discardChanges?: bool (one is required), "
		"archiveLibraryFolder?: folder for the embedded library of a .pla}.",
		OpenProject);

	RegisterCommand ("NewProject",
		"Creates a new untitled project, closing the current one. Input: {template?: .tpl path, reset?: bool (New & Reset: default settings), "
		"saveFirst?: bool | discardChanges?: bool (one is required)}.",
		NewProject);

	RegisterCommand ("CloseProject",
		"Closes the current project. The JSON API stops answering until a project is opened again by hand. "
		"Input: {confirm: true, saveFirst?: bool | discardChanges?: bool (one is required)}.",
		CloseProject);

	RegisterCommand ("RebuildModel",
		"Rebuilds or redraws the current (front) window. Input: {mode?: 'Rebuild' (default) | 'Regenerate' (Rebuild & Regenerate) | 'Redraw'}.",
		RebuildModel);

	project::RegisterSettingsCommands ();
	project::RegisterEditMenuCommands ();
}

} // namespace cc


// =============================================================================
// Implementation parts of the project family (single translation unit)
// =============================================================================

#include "Commands/ProjectSettings.inl.hpp"
#include "Commands/ProjectEditMenu.inl.hpp"
