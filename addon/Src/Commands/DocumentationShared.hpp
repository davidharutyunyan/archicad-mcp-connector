// *****************************************************************************
// DocumentationShared.hpp — helpers shared by the "documentation" family
// (Documentation.cpp, DocumentationExport.cpp, DocumentationHotlinks.cpp,
// DocumentationFiles.cpp). Everything lives in namespace cc::doc so nothing
// collides with other families.
//
// Database references (JSON) used by every command of the family:
//   "FloorPlan" | "3DModel"          the single floor plan / 3D model database
//   "<guid>"                         a database guid (databaseGuid from GetDatabases)
//                                    or a navigator item guid (layout, view, project map item)
//   "<name>"                         exact (then case-insensitive) database name / "ref name" title
// *****************************************************************************

#pragma once

#include "Core/Command.hpp"
#include "Core/Elements.hpp"
#include "Core/Enums.hpp"
#include "Core/Types.hpp"

#include "Location.hpp"

#include <functional>
#include <optional>
#include <string>

namespace cc {
namespace doc {

// --- Sub-family registration (called from RegisterDocumentationCommands) ------

void RegisterDocumentationExportCommands ();		// DocumentationExport.cpp
void RegisterDocumentationFileCommands ();			// DocumentationFiles.cpp  (DXF / 3D model writers)
void RegisterDocumentationHotlinkCommands ();		// DocumentationHotlinks.cpp

// --- Strings -----------------------------------------------------------------

// Decodes a char[] API text field (drawing names, navigator set names ...). These fields use
// the application's default code page (same as the DevKit examples: GS::UniString (char*)).
GS::UniString		ApiText (const char* s);
// Encodes into a char[] API text field (truncated, always 0-terminated).
void				CopyApiText (const GS::UniString& s, char* dst, USize dstSize);
// Copies into a GS::uchar_t[] field (count = array length in characters).
void				CopyUniText (const GS::UniString& s, GS::uchar_t* dst, USize count);
std::string			Utf8 (const GS::UniString& s);

// --- Database types ----------------------------------------------------------

GS::UniString						DbTypeName (API_DatabaseTypeID typeID);
std::optional<API_DatabaseTypeID>	ParseDbType (const GS::UniString& name);
extern const char* const			kDbTypeList;		// "FloorPlan, Section, ..." for error messages

// --- Databases ---------------------------------------------------------------

API_DatabaseInfo	CurrentDatabase ();
API_WindowInfo		CurrentWindow ();
// Fills info (title, name, ref, typeID ...) from info.databaseUnId (and typeID). False on error.
bool				FillDatabaseInfo (API_DatabaseInfo& info);
GS::Array<API_DatabaseUnId>	ListDatabaseIds (API_DatabaseID listCode);	// APIDb_Get<Type>DatabasesID
// Databases of one type (FloorPlan -> the single plan database). Info filled.
GS::Array<API_DatabaseInfo>	ListDatabases (API_DatabaseTypeID typeID);
bool				SameDatabase (const API_DatabaseInfo& a, const API_DatabaseInfo& b);
// "FloorPlan" / "3DModel" for the singleton databases, the elemSetId guid otherwise.
GS::UniString		DatabaseRef (const API_DatabaseInfo& info);
// {databaseRef, databaseGuid?, type, index, name, ref, title, linkedElement?, linkedDatabase?, masterLayout?}
OS					DatabaseJson (const API_DatabaseInfo& info);
// Resolves a database reference (see header comment). allowedTypes empty = any type.
API_DatabaseInfo	ResolveDatabase (const GS::UniString& ref, const GS::Array<API_DatabaseTypeID>& allowedTypes = {});
// Layout (and optionally master layout) reference: database guid, layout navigator item guid, or name / "ID name".
API_DatabaseInfo	ResolveLayout (const GS::UniString& ref, bool allowMaster = false);
GS::Array<API_DatabaseInfo>	AllLayouts (bool includeMaster);

// Switches the current database for the lifetime of the object (restores the previous one).
class DatabaseScope {
public:
	explicit DatabaseScope (const API_DatabaseInfo& target);
	~DatabaseScope ();
	DatabaseScope (const DatabaseScope&) = delete;
	DatabaseScope& operator= (const DatabaseScope&) = delete;
	bool Switched () const { return switched; }
private:
	API_DatabaseInfo	previous;
	bool				switched = false;
};

// --- Navigator ---------------------------------------------------------------

bool				GetNavigatorItem (const API_Guid& guid, API_NavigatorItem& item);
GS::UniString		NavItemTypeName (API_NavigatorItemTypeID typeID);
GS::UniString		NavMapName (API_NavigatorMapID mapID);
// All items (depth-first) below the root of a navigator map. For publisher sets pass the set index.
GS::Array<API_NavigatorItem>	CollectNavigatorItems (API_NavigatorMapID mapID, Int32 setIndex = 0, API_Guid* rootGuid = nullptr);
GS::Array<API_NavigatorItem>	NavigatorChildren (const API_Guid& parent, API_NavigatorMapID mapID);
// {navigatorItemGuid, name, id, itemType, map, sourceNavigatorItemGuid?, database?}
OS					NavItemJson (const API_NavigatorItem& item, bool withDatabase = true);
// Drawing scale (e.g. 100 for 1:100) saved in a view, or 0 when the view has none / is not a view.
Int32				ViewDrawingScale (const API_NavigatorItem& item);
// Views and viewpoints that can be placed as drawings (Tapir-proven list: anything else crashes Archicad).
bool				IsPlaceableAsDrawing (API_NavigatorItemTypeID itemType);

// --- Windows -----------------------------------------------------------------

// Captures the front window (and current story) and restores it on Restore() / destruction when armed.
class WindowRestorer {
public:
	WindowRestorer ();
	~WindowRestorer ();
	WindowRestorer (const WindowRestorer&) = delete;
	WindowRestorer& operator= (const WindowRestorer&) = delete;
	void	Arm (bool on) { armed = on; }
	bool	Armed () const { return armed; }
	void	Restore ();
	void	Finish () { if (armed) Restore (); }	// restores now when armed
	const API_WindowInfo& Original () const { return original; }
private:
	API_WindowInfo	original;
	short			originalStory = 0;
	bool			haveOriginal = false;
	bool			armed = false;
};

struct TargetWindow {
	bool				changed = false;
	API_WindowInfo		window {};
	GS::UniString		description;		// human readable: "Layout 'A01 Plan'"
	OS					json;				// {type, database, view?, story?}
};

// Brings the requested content to the front window. Accepted fields in target:
//   view: navigator view guid (opens it like double-clicking in the View Map: story, layers, scale, zoom)
//   layout: layout reference (see ResolveLayout), masterLayout allowed when allowMaster
//   database: database reference (see ResolveDatabase)
//   storyIndex: floor plan story (switches to the floor plan and goes to that story)
// With none of them the current front window is used unchanged.
TargetWindow		OpenTargetWindow (const OS& target, bool allowMaster = true);
bool				SwitchToWindow (const API_WindowInfo& window);
void				GoToStory (short storyIndex);
bool				IsModelWindow (API_WindowTypeID typeID);

// --- Files -------------------------------------------------------------------

GS::UniString		FileExtension (const GS::UniString& path);			// lower case, without dot
bool				IsAbsolutePath (const GS::UniString& path);
bool				PathExists (const GS::UniString& path);
bool				IsDirectory (const GS::UniString& path);
Int64				FileSize (const GS::UniString& path);				// -1 when missing
// Validates an output file path: absolute, allowed extension (adds defaultExt when missing),
// parent folder exists (or is created when createFolders), existing file only with overwrite.
// Returns the final path.
GS::UniString		PrepareOutputPath (const OS& params, const char* key, const GS::Array<GS::UniString>& extensions,
									   const GS::UniString& defaultExt);
// Same for an output folder (created when createFolders).
GS::UniString		PrepareOutputFolder (const GS::UniString& path, bool createFolders, const char* key);
// Validates an existing input file.
GS::UniString		RequireInputFile (const OS& params, const char* key);
// {path, exists, sizeBytes}
OS					FileJson (const GS::UniString& path);
IO::Location		ToLocation (const GS::UniString& path);
GS::UniString		LocationPath (const IO::Location& loc);
// Files below folder modified at or after sinceEpochSeconds (recursive, max entries).
GS::Array<OS>		RecentFiles (const GS::UniString& folder, Int64 sinceEpochSeconds, Int32 maxEntries);
Int64				NowEpochSeconds ();

// --- Misc --------------------------------------------------------------------

// Reads an array whose items are strings (or objects with one of the given string keys).
GS::Array<GS::UniString>	GetRefArray (const OS& params, const char* key, const GS::Array<const char*>& objectKeys = {});
// Runs fn; retries it inside an undo scope when Archicad answers APIERR_NEEDSUNDOSCOPE.
GSErrCode			CallMaybeUndoable (const GS::UniString& undoName, const std::function<GSErrCode ()>& fn);

} // namespace doc
} // namespace cc
