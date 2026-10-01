// *****************************************************************************
// ViewsCommon.hpp — helpers shared by the "views" command family
// (Views.cpp, Views3D.cpp, ViewsMarkers.cpp): window types, current window /
// database access, viewpoint databases, Navigator items and window switching.
// Everything lives in cc::views so nothing collides with other families.
// *****************************************************************************

#pragma once

#include "Core/Json.hpp"
#include "Core/Types.hpp"

#include <functional>
#include <optional>

namespace cc {
namespace views {

// --- Window types --------------------------------------------------------------
//   "FloorPlan", "Section", "Detail", "3DModel" (alias "3D"), "Layout", "Drawing",
//   "CustomText", "CustomDraw", "MasterLayout", "Elevation", "InteriorElevation",
//   "Worksheet", "Report", "DocumentFrom3D", "External3D", "Movie3D", "MovieRendering",
//   "Rendering", "ModelCompare", "InteractiveSchedule"

GS::UniString						WindowTypeName (API_WindowTypeID type);
std::optional<API_WindowTypeID>		ParseWindowType (const GS::UniString& name);
API_WindowTypeID					GetWindowType (const OS& os, const char* key);		// throws listing the allowed values
bool								Is2DWindow (API_WindowTypeID type);					// plan, section, detail, layout, ...
const GS::Array<API_WindowTypeID>&	ViewpointTypes ();									// types that have databases (sections ... layouts)

// --- Strings ---------------------------------------------------------------------

GS::UniString	UStrN (const GS::uchar_t* s, USize capacity);
template <USize N>
GS::UniString	UStr (const GS::uchar_t (&s)[N])			{ return UStrN (s, N); }
void			SetUStrN (GS::uchar_t* dst, USize capacity, const GS::UniString& value);
template <USize N>
void			SetUStr (GS::uchar_t (&dst)[N], const GS::UniString& value)	{ SetUStrN (dst, N, value); }

// Decodes a C string of an API struct (UTF-8 when valid, otherwise the system encoding).
GS::UniString	DecodeCStrN (const char* s, USize capacity);
template <USize N>
GS::UniString	CStr (const char (&s)[N])					{ return DecodeCStrN (s, N); }
void			CopyCStrN (char* dst, USize capacity, const char* src);

// --- API call helper ---------------------------------------------------------------

// Calls fn; when Archicad answers APIERR_NEEDSUNDOSCOPE the call is repeated inside an undo step.
// Throws cc::Error ("<what>: <APIERR ...>") on failure.
void			CallApi (const GS::UniString& what, const std::function<GSErrCode ()>& fn);

// --- Current window / database ---------------------------------------------------

bool			TryGetCurrentWindow (API_WindowInfo& info);
API_WindowInfo	GetCurrentWindow ();								// throws with a hint when no window is open
OS				WindowJson (const API_WindowInfo& info);			// {type, database?, name?, reference?, title?, linkedElement?}
OS				CurrentWindowJson ();								// WindowJson + story / drawingScale / zoom / projection
OS				StoryJson (short index);

// Temporarily makes another database current (restored by the destructor).
class CurrentDatabaseSwitch {
public:
	CurrentDatabaseSwitch () = default;
	~CurrentDatabaseSwitch ();
	CurrentDatabaseSwitch (const CurrentDatabaseSwitch&) = delete;
	CurrentDatabaseSwitch& operator= (const CurrentDatabaseSwitch&) = delete;

	// Switches when the current database is not of the given type (floor plan: typeID only).
	void	EnsureType (API_DatabaseTypeID type);
	// Switches to the database that contains the element when it is not in the current one.
	bool	EnsureContaining (const API_Guid& elemGuid);
	bool	Switched () const	{ return switched; }

private:
	void	SwitchTo (const API_DatabaseInfo& target);

	bool				switched = false;
	API_DatabaseInfo	original = {};
};

// --- Viewpoint databases ---------------------------------------------------------

struct DbEntry {
	API_WindowTypeID	type = API_ZombieWindowID;
	API_DatabaseUnId	id = {};
	GS::UniString		name;
	GS::UniString		ref;
	GS::UniString		title;
};

GS::Array<DbEntry>		ListDatabases (API_WindowTypeID type);
std::optional<DbEntry>	FindDatabaseByGuid (const API_Guid& guid);
OS						DbEntryJson (const DbEntry& entry);

// --- Navigator ----------------------------------------------------------------------

struct NavEntry {
	API_NavigatorItem	item = {};
	GS::UniString		path;			// "Folder / Subfolder"
};

GS::UniString	NavItemTypeName (API_NavigatorItemTypeID type);
GS::UniString	NavMapName (API_NavigatorMapID map);
bool			IsNavFolder (API_NavigatorItemTypeID type);
GS::UniString	NavDisplayName (const API_NavigatorItem& item);	// "<id> <name>"
bool			GetNavItem (const API_Guid& guid, API_NavigatorMapID map, API_NavigatorItem& out);
void			CollectNavItems (API_NavigatorMapID map, GS::Array<NavEntry>& out, USize limit = 20000);
OS				NavEntryJson (const NavEntry& entry);

// Resolves os[key] = "guid" | "name" | {guid} | {name} | {navigatorItemId: {guid}} to a Navigator item.
// GUIDs are looked up in every map; names are searched in `searchMaps` (in order, first map with matches wins).
NavEntry		ResolveNavItem (const OS& os, const char* key, const GS::Array<API_NavigatorMapID>& searchMaps);

// --- Switching windows ------------------------------------------------------------------

void			OpenWindow (const API_WindowInfo& info, const GS::UniString& label);
void			GoToStory (short index);
void			Switch3DMode (bool perspective);				// keeps the other projection parameters
void			OpenNavItem (const API_NavigatorItem& item);	// project map / layout book / view map item without view settings

// --- Sub-registrations --------------------------------------------------------------

void			RegisterView3DCommands ();		// Views3D.cpp
void			RegisterViewMarkerAdapters ();	// ViewsMarkers.cpp

} // namespace views
} // namespace cc
