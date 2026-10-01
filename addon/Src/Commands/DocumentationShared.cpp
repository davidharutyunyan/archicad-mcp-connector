// *****************************************************************************
// DocumentationShared.cpp — implementation of the helpers declared in
// DocumentationShared.hpp (databases, navigator, windows, files).
// *****************************************************************************

#include "Commands/DocumentationShared.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace cc {
namespace doc {

namespace fs = std::filesystem;

// =============================================================================
// Strings
// =============================================================================

GS::UniString ApiText (const char* s)
{
	if (s == nullptr)
		return GS::UniString ();
	return GS::UniString (s, CC_Default);
}


void CopyApiText (const GS::UniString& s, char* dst, USize dstSize)
{
	if (dst == nullptr || dstSize == 0)
		return;
	std::memset (dst, 0, dstSize);
	const GS::String converted (s.ToCStr (0, MaxUSize, CC_Default).Get ());
	const char* src = converted.ToCStr ();
	std::strncpy (dst, src, dstSize - 1);
	dst[dstSize - 1] = 0;
}


void CopyUniText (const GS::UniString& s, GS::uchar_t* dst, USize count)
{
	if (dst == nullptr || count == 0)
		return;
	std::memset (dst, 0, count * sizeof (GS::uchar_t));
	GS::ucsncpy (dst, s.ToUStr ().Get (), count - 1);
	dst[count - 1] = 0;
}


std::string Utf8 (const GS::UniString& s)
{
	return std::string (s.ToCStr (0, MaxUSize, CC_UTF8).Get ());
}

// =============================================================================
// Database types
// =============================================================================

namespace {

const NamedValue kDbTypes[] = {
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


struct ListableType {
	API_DatabaseTypeID	type;
	API_DatabaseID		listCode;		// 0 for the floor plan
};

const ListableType kListableTypes[] = {
	{ APIWind_FloorPlanID,			(API_DatabaseID) 0 },
	{ APIWind_SectionID,			APIDb_GetSectionDatabasesID },
	{ APIWind_ElevationID,			APIDb_GetElevationDatabasesID },
	{ APIWind_InteriorElevationID,	APIDb_GetInteriorElevationDatabasesID },
	{ APIWind_DetailID,				APIDb_GetDetailDatabasesID },
	{ APIWind_WorksheetID,			APIDb_GetWorksheetDatabasesID },
	{ APIWind_DocumentFrom3DID,		APIDb_GetDocumentFrom3DDatabasesID },
	{ APIWind_LayoutID,				APIDb_GetLayoutDatabasesID },
	{ APIWind_MasterLayoutID,		APIDb_GetMasterLayoutDatabasesID },
};


bool IsNullGuid (const API_Guid& g)
{
	return g == APINULLGuid;
}


bool LooksLikeGuid (const GS::UniString& s)
{
	GS::UniString t = s;
	t.Trim ();
	if (t.BeginsWith ("{") && t.EndsWith ("}"))
		t = t.GetSubstring (1, t.GetLength () - 2);
	if (t.GetLength () != 36)
		return false;
	for (UIndex i = 0; i < t.GetLength (); ++i) {
		const GS::UniChar::Layout c = GS::UniChar (t[i]);
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (c != '-')
				return false;
		} else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
			return false;
		}
	}
	return true;
}


API_Guid GuidOf (const GS::UniString& s)
{
	GS::UniString t = s;
	t.Trim ();
	if (t.BeginsWith ("{") && t.EndsWith ("}"))
		t = t.GetSubstring (1, t.GetLength () - 2);
	return ParseGuid (t);
}


GS::UniString DbLabel (const API_DatabaseInfo& info)
{
	GS::UniString title (info.title);
	if (title.IsEmpty ())
		title = GS::UniString (info.name);
	return DbTypeName (info.typeID) + (title.IsEmpty () ? GS::UniString () : " '" + title + "'");
}


GS::Array<API_DatabaseInfo> AllListableDatabases ()
{
	GS::Array<API_DatabaseInfo> all;
	for (const ListableType& t : kListableTypes) {
		for (const API_DatabaseInfo& info : ListDatabases (t.type))
			all.Push (info);
	}
	return all;
}


bool TypeAllowed (API_DatabaseTypeID type, const GS::Array<API_DatabaseTypeID>& allowed)
{
	return allowed.IsEmpty () || allowed.Contains (type);
}


GS::UniString AllowedTypesText (const GS::Array<API_DatabaseTypeID>& allowed)
{
	GS::UniString s;
	for (API_DatabaseTypeID t : allowed) {
		if (!s.IsEmpty ())
			s += ", ";
		s += DbTypeName (t);
	}
	return s;
}

} // namespace

const char* const kDbTypeList =
	"FloorPlan, Section, Elevation, InteriorElevation, Detail, Worksheet, DocumentFrom3D, Layout, MasterLayout";


GS::UniString DbTypeName (API_DatabaseTypeID typeID)
{
	return NameOf (kDbTypes, (Int32) typeID);
}


std::optional<API_DatabaseTypeID> ParseDbType (const GS::UniString& name)
{
	for (const NamedValue& nv : kDbTypes) {
		if (EqualsIgnoreCase (name, nv.name))
			return (API_DatabaseTypeID) nv.value;
	}
	return std::nullopt;
}

// =============================================================================
// Databases
// =============================================================================

API_DatabaseInfo CurrentDatabase ()
{
	API_DatabaseInfo info;
	BNZeroMemory (&info, sizeof (info));
	Check (ACAPI_Database (APIDb_GetCurrentDatabaseID, &info, nullptr), "Cannot read the current database");
	return info;
}


API_WindowInfo CurrentWindow ()
{
	API_WindowInfo info;
	BNZeroMemory (&info, sizeof (info));
	Check (ACAPI_Database (APIDb_GetCurrentWindowID, &info, nullptr), "Cannot read the current window");
	return info;
}


bool FillDatabaseInfo (API_DatabaseInfo& info)
{
	return ACAPI_Database (APIDb_GetDatabaseInfoID, &info, nullptr) == NoError;
}


GS::Array<API_DatabaseUnId> ListDatabaseIds (API_DatabaseID listCode)
{
	GS::Array<API_DatabaseUnId> ids;
	ACAPI_Database (listCode, nullptr, &ids);
	return ids;
}


GS::Array<API_DatabaseInfo> ListDatabases (API_DatabaseTypeID typeID)
{
	GS::Array<API_DatabaseInfo> out;
	if (typeID == APIWind_FloorPlanID || typeID == APIWind_3DModelID) {
		API_DatabaseInfo info;
		BNZeroMemory (&info, sizeof (info));
		info.typeID = typeID;
		FillDatabaseInfo (info);
		info.typeID = typeID;
		out.Push (info);
		return out;
	}
	for (const ListableType& t : kListableTypes) {
		if (t.type != typeID || t.listCode == 0)
			continue;
		for (const API_DatabaseUnId& id : ListDatabaseIds (t.listCode)) {
			API_DatabaseInfo info;
			BNZeroMemory (&info, sizeof (info));
			info.typeID = typeID;
			info.databaseUnId = id;
			FillDatabaseInfo (info);
			info.typeID = typeID;
			info.databaseUnId = id;
			out.Push (info);
		}
	}
	return out;
}


bool SameDatabase (const API_DatabaseInfo& a, const API_DatabaseInfo& b)
{
	if (a.typeID != b.typeID)
		return false;
	if (a.typeID == APIWind_FloorPlanID || a.typeID == APIWind_3DModelID)
		return true;
	return a.databaseUnId.elemSetId == b.databaseUnId.elemSetId;
}


GS::UniString DatabaseRef (const API_DatabaseInfo& info)
{
	if (info.typeID == APIWind_FloorPlanID)
		return "FloorPlan";
	if (info.typeID == APIWind_3DModelID)
		return "3DModel";
	return GuidStr (info.databaseUnId.elemSetId);
}


OS DatabaseJson (const API_DatabaseInfo& info)
{
	OS out;
	out.Add ("databaseRef", DatabaseRef (info));
	if (!IsNullGuid (info.databaseUnId.elemSetId))
		out.Add ("databaseGuid", GuidStr (info.databaseUnId.elemSetId));
	out.Add ("type", DbTypeName (info.typeID));
	out.Add ("index", (Int32) info.index);
	out.Add ("name", GS::UniString (info.name));
	out.Add ("ref", GS::UniString (info.ref));
	out.Add ("title", GS::UniString (info.title));
	if (!IsNullGuid (info.linkedElement))
		out.Add ("linkedElement", GuidStr (info.linkedElement));
	if (!IsNullGuid (info.linkedDatabaseUnId.elemSetId))
		out.Add ("linkedDatabaseGuid", GuidStr (info.linkedDatabaseUnId.elemSetId));
	if (info.typeID == APIWind_LayoutID && !IsNullGuid (info.masterLayoutUnId.elemSetId)) {
		API_DatabaseInfo master;
		BNZeroMemory (&master, sizeof (master));
		master.typeID = APIWind_MasterLayoutID;
		master.databaseUnId = info.masterLayoutUnId;
		OS m ("databaseGuid", GuidStr (info.masterLayoutUnId.elemSetId));
		if (FillDatabaseInfo (master))
			m.Add ("name", GS::UniString (master.name));
		out.Add ("masterLayout", m);
	}
	return out;
}


API_DatabaseInfo ResolveDatabase (const GS::UniString& refIn, const GS::Array<API_DatabaseTypeID>& allowedTypes)
{
	GS::UniString ref = refIn;
	ref.Trim ();
	if (ref.IsEmpty ())
		Fail ("Empty database reference. Pass \"FloorPlan\", a databaseGuid / navigatorItemGuid from get_databases, or a database name.");

	auto finish = [&] (API_DatabaseInfo info) -> API_DatabaseInfo {
		if (!TypeAllowed (info.typeID, allowedTypes))
			Fail ("'" + ref + "' is a " + DbLabel (info) + ", but a " + AllowedTypesText (allowedTypes) + " database is required here.");
		return info;
	};

	// Singleton databases by type name.
	if (auto t = ParseDbType (ref)) {
		if (*t == APIWind_FloorPlanID || *t == APIWind_3DModelID)
			return finish (ListDatabases (*t)[0]);
		Fail ("'" + ref + "' is a database type, not a database. Pass the databaseGuid or name of one " + ref +
			  " database (list them with get_databases {types: [\"" + ref + "\"]}).");
	}

	const GS::Array<API_DatabaseInfo> all = AllListableDatabases ();

	if (LooksLikeGuid (ref)) {
		const API_Guid guid = GuidOf (ref);
		for (const API_DatabaseInfo& info : all) {
			if (info.databaseUnId.elemSetId == guid && info.typeID != APIWind_FloorPlanID)
				return finish (info);
		}
		API_NavigatorItem item;
		if (GetNavigatorItem (guid, item)) {
			API_DatabaseInfo info = item.db;
			if (info.typeID == API_ZombieWindowID)
				Fail ("Navigator item '" + GS::UniString (item.uName) + "' (" + NavItemTypeName (item.itemType) + ") has no database of its own (folders, subsets, schedules, lists and 3D views have none).");
			FillDatabaseInfo (info);
			info.typeID = item.db.typeID;
			return finish (info);
		}
		Fail ("No database or navigator item with guid " + ref + ". Call get_databases to list databases (databaseRef) and the navigator item guids of layouts and views.", APIERR_BADID);
	}

	// By name: exact first, then case-insensitive, on name, title ("ref name") and ref.
	for (int pass = 0; pass < 2; ++pass) {
		GS::Array<API_DatabaseInfo> hits;
		for (const API_DatabaseInfo& info : all) {
			if (!TypeAllowed (info.typeID, allowedTypes))
				continue;
			const GS::UniString name (info.name), title (info.title), dbRef (info.ref);
			const bool match = pass == 0 ? (name == ref || title == ref || (!dbRef.IsEmpty () && dbRef == ref))
										 : (EqualsIgnoreCase (name, ref) || EqualsIgnoreCase (title, ref) || (!dbRef.IsEmpty () && EqualsIgnoreCase (dbRef, ref)));
			if (match)
				hits.Push (info);
		}
		if (hits.GetSize () == 1)
			return finish (hits[0]);
		if (hits.GetSize () > 1) {
			GS::UniString list;
			for (const API_DatabaseInfo& h : hits) {
				if (!list.IsEmpty ())
					list += "; ";
				list += DbLabel (h) + " = " + DatabaseRef (h);
			}
			Fail ("'" + ref + "' matches several databases (" + list + "). Pass the databaseRef guid instead.");
		}
	}
	Fail ("No database named '" + ref + "'" + (allowedTypes.IsEmpty () ? GS::UniString () : " of type " + AllowedTypesText (allowedTypes)) +
		  ". Names are localized; call get_databases to list them.", APIERR_BADNAME);
}


GS::Array<API_DatabaseInfo> AllLayouts (bool includeMaster)
{
	GS::Array<API_DatabaseInfo> out = ListDatabases (APIWind_LayoutID);
	if (includeMaster) {
		for (const API_DatabaseInfo& m : ListDatabases (APIWind_MasterLayoutID))
			out.Push (m);
	}
	return out;
}


API_DatabaseInfo ResolveLayout (const GS::UniString& ref, bool allowMaster)
{
	GS::Array<API_DatabaseTypeID> allowed;
	allowed.Push (APIWind_LayoutID);
	if (allowMaster)
		allowed.Push (APIWind_MasterLayoutID);
	try {
		return ResolveDatabase (ref, allowed);
	} catch (const Error& e) {
		if (e.code != APIERR_BADNAME)
			throw;
		// Names as shown in the Layout Book: "<ID> <name>", the name, or the autotext-resolved name.
		GS::UniString want = ref;
		want.Trim ();
		for (int pass = 0; pass < 2; ++pass) {
			GS::Array<API_NavigatorItem> hits;
			for (const API_NavigatorItem& item : CollectNavigatorItems (API_LayoutMap)) {
				const bool isLayout = item.itemType == API_LayoutNavItem || (allowMaster && item.itemType == API_MasterLayoutNavItem);
				if (!isLayout)
					continue;
				const GS::UniString name (item.uName), autoName (item.uAutoTextedName), id = ApiText (item.uiId);
				const GS::UniString idName = id.IsEmpty () ? name : id + " " + name;
				const bool match = pass == 0 ? (name == want || autoName == want || idName == want)
											 : (EqualsIgnoreCase (name, want) || EqualsIgnoreCase (autoName, want) || EqualsIgnoreCase (idName, want));
				if (match)
					hits.Push (item);
			}
			if (hits.GetSize () == 1) {
				API_DatabaseInfo info = hits[0].db;
				FillDatabaseInfo (info);
				info.typeID = hits[0].db.typeID;
				return info;
			}
			if (hits.GetSize () > 1)
				Fail ("'" + ref + "' matches several layouts; pass the layout's databaseGuid or navigatorItemGuid (get_databases {types: [\"Layout\"]}).");
		}
		throw;
	}
}


DatabaseScope::DatabaseScope (const API_DatabaseInfo& target)
{
	previous = CurrentDatabase ();
	if (SameDatabase (previous, target))
		return;
	API_DatabaseInfo t = target;
	const GSErrCode err = ACAPI_Database (APIDb_ChangeCurrentDatabaseID, &t, nullptr);
	if (err != NoError) {
		Fail ("Cannot switch to " + DbLabel (target) + ": " + ErrorName (err) +
			  ". Section/elevation/detail databases that were not opened since the project was opened are not generated yet; "
			  "open that view once (e.g. export_pdf with this database, or the views tools) and retry.", err);
	}
	switched = true;
}


DatabaseScope::~DatabaseScope ()
{
	if (switched)
		ACAPI_Database (APIDb_ChangeCurrentDatabaseID, &previous, nullptr);
}

// =============================================================================
// Navigator
// =============================================================================

namespace {

const NamedValue kNavItemTypes[] = {
	{ "Undefined",				API_UndefinedNavItem },
	{ "Project",				API_ProjectNavItem },
	{ "Story",					API_StoryNavItem },
	{ "Section",				API_SectionNavItem },
	{ "Detail",					API_DetailDrawingNavItem },
	{ "Perspective",			API_PerspectiveNavItem },
	{ "Axonometry",				API_AxonometryNavItem },
	{ "List",					API_ListNavItem },
	{ "Schedule",				API_ScheduleNavItem },
	{ "Toc",					API_TocNavItem },
	{ "Camera",					API_CameraNavItem },
	{ "CameraSet",				API_CameraSetNavItem },
	{ "Info",					API_InfoNavItem },
	{ "Help",					API_HelpNavItem },
	{ "Layout",					API_LayoutNavItem },
	{ "MasterLayout",			API_MasterLayoutNavItem },
	{ "Book",					API_BookNavItem },
	{ "MasterFolder",			API_MasterFolderNavItem },
	{ "Subset",					API_SubSetNavItem },
	{ "TextList",				API_TextListNavItem },
	{ "Elevation",				API_ElevationNavItem },
	{ "InteriorElevation",		API_InteriorElevationNavItem },
	{ "Worksheet",				API_WorksheetDrawingNavItem },
	{ "DocumentFrom3D",			API_DocumentFrom3DNavItem },
	{ "Folder",					API_FolderNavItem },
	{ "Drawing",				API_DrawingNavItem },
};

const NamedValue kNavMaps[] = {
	{ "Undefined",		API_UndefinedMap },
	{ "ProjectMap",		API_ProjectMap },
	{ "ViewMap",		API_PublicViewMap },
	{ "MyViewMap",		API_MyViewMap },
	{ "LayoutBook",		API_LayoutMap },
	{ "PublisherSets",	API_PublisherSets },
};


void FreeNavigatorSet (API_NavigatorSet& set)
{
	delete set.host;				set.host = nullptr;
	delete set.dirName;				set.dirName = nullptr;
	delete set.path;				set.path = nullptr;
	delete set.twFileServerPath;	set.twFileServerPath = nullptr;
}


void CollectRecursive (const API_Guid& parent, API_NavigatorMapID mapID, GS::Array<API_NavigatorItem>& out, Int32 depth)
{
	if (depth > 64 || out.GetSize () > 50000)
		return;
	for (const API_NavigatorItem& child : NavigatorChildren (parent, mapID)) {
		out.Push (child);
		CollectRecursive (child.guid, mapID, out, depth + 1);
	}
}

} // namespace


bool GetNavigatorItem (const API_Guid& guid, API_NavigatorItem& item)
{
	// The lookup is per navigator map (mapId must be set, and Archicad does not always fill it in): try each map.
	static const API_NavigatorMapID kMaps[] = { API_PublicViewMap, API_MyViewMap, API_ProjectMap, API_LayoutMap, API_PublisherSets };
	for (API_NavigatorMapID map : kMaps) {
		BNZeroMemory (&item, sizeof (item));
		item.mapId = map;
		API_Guid g = guid;
		if (ACAPI_Navigator (APINavigator_GetNavigatorItemID, &g, &item) == NoError) {
			item.mapId = map;
			if (item.guid == APINULLGuid)
				item.guid = guid;
			return true;
		}
	}
	BNZeroMemory (&item, sizeof (item));
	return false;
}


GS::UniString NavItemTypeName (API_NavigatorItemTypeID typeID)
{
	return NameOf (kNavItemTypes, (Int32) typeID);
}


GS::UniString NavMapName (API_NavigatorMapID mapID)
{
	return NameOf (kNavMaps, (Int32) mapID);
}


GS::Array<API_NavigatorItem> NavigatorChildren (const API_Guid& parent, API_NavigatorMapID mapID)
{
	API_NavigatorItem p;
	BNZeroMemory (&p, sizeof (p));
	p.guid = parent;
	p.mapId = mapID;
	GS::Array<API_NavigatorItem> items;
	ACAPI_Navigator (APINavigator_GetNavigatorChildrenItemsID, &p, nullptr, &items);
	for (API_NavigatorItem& child : items)
		child.mapId = mapID;		// not always filled in by Archicad
	return items;
}


GS::Array<API_NavigatorItem> CollectNavigatorItems (API_NavigatorMapID mapID, Int32 setIndex, API_Guid* rootGuid)
{
	GS::Array<API_NavigatorItem> out;
	API_NavigatorSet set;
	BNZeroMemory (&set, sizeof (set));
	set.mapId = mapID;
	Int32 idx = setIndex;
	const GSErrCode err = ACAPI_Navigator (APINavigator_GetNavigatorSetID, &set, &idx);
	FreeNavigatorSet (set);
	if (err != NoError)
		return out;
	if (rootGuid != nullptr)
		*rootGuid = set.rootGuid;
	CollectRecursive (set.rootGuid, mapID, out, 0);
	return out;
}


OS NavItemJson (const API_NavigatorItem& item, bool withDatabase)
{
	OS out;
	out.Add ("navigatorItemGuid", GuidStr (item.guid));
	GS::UniString name (item.uName);
	out.Add ("name", name);
	const GS::UniString autoName (item.uAutoTextedName);
	if (!autoName.IsEmpty () && autoName != name)
		out.Add ("displayName", autoName);
	out.Add ("id", ApiText (item.uiId));
	out.Add ("itemType", NavItemTypeName (item.itemType));
	out.Add ("map", NavMapName (item.mapId));
	if (item.sourceGuid != APINULLGuid)
		out.Add ("sourceNavigatorItemGuid", GuidStr (item.sourceGuid));
	if (item.itemType == API_StoryNavItem)
		out.Add ("storyIndex", (Int32) item.floorNum);
	if (withDatabase && item.db.typeID != API_ZombieWindowID) {
		API_DatabaseInfo db = item.db;
		FillDatabaseInfo (db);
		db.typeID = item.db.typeID;
		OS d ("databaseRef", DatabaseRef (db), "type", DbTypeName (db.typeID));
		out.Add ("database", d);
	}
	return out;
}


Int32 ViewDrawingScale (const API_NavigatorItem& itemIn)
{
	API_NavigatorItem item = itemIn;
	API_NavigatorView view;
	BNZeroMemory (&view, sizeof (view));
	const GSErrCode err = ACAPI_Navigator (APINavigator_GetNavigatorViewID, &item, &view);
	if (view.modelViewOpt != nullptr) {
		ACAPI_FreeGDLModelViewOptionsPtr (&view.modelViewOpt->gdlOptions);
		BMKillPtr (reinterpret_cast<GSPtr*> (&view.modelViewOpt));
	}
	if (view.layerStats != nullptr)
		BMKillHandle (reinterpret_cast<GSHandle*> (&view.layerStats));
	if (view.dimPrefs != nullptr)
		BMKillPtr (reinterpret_cast<GSPtr*> (&view.dimPrefs));
	if (view.pens != nullptr)
		BMKillPtr (reinterpret_cast<GSPtr*> (&view.pens));
	if (err != NoError || !view.saveDScale || view.drawingScale <= 0)
		return 0;
	return view.drawingScale;
}


bool IsPlaceableAsDrawing (API_NavigatorItemTypeID itemType)
{
	switch (itemType) {
		case API_StoryNavItem:
		case API_SectionNavItem:
		case API_ElevationNavItem:
		case API_InteriorElevationNavItem:
		case API_DetailDrawingNavItem:
		case API_WorksheetDrawingNavItem:
		case API_DocumentFrom3DNavItem:
		case API_PerspectiveNavItem:
		case API_AxonometryNavItem:
		case API_ScheduleNavItem:
		case API_ListNavItem:
		case API_TextListNavItem:
		case API_TocNavItem:
			return true;
		default:
			return false;
	}
}

// =============================================================================
// Windows
// =============================================================================

WindowRestorer::WindowRestorer ()
{
	BNZeroMemory (&original, sizeof (original));
	haveOriginal = ACAPI_Database (APIDb_GetCurrentWindowID, &original, nullptr) == NoError;
	originalStory = CurrentStoryIndex ();
}


WindowRestorer::~WindowRestorer ()
{
	if (armed) {
		try {
			Restore ();
		} catch (...) {
		}
	}
}


void WindowRestorer::Restore ()
{
	armed = false;
	if (!haveOriginal || original.typeID == API_ZombieWindowID)
		return;
	API_WindowInfo now;
	BNZeroMemory (&now, sizeof (now));
	ACAPI_Database (APIDb_GetCurrentWindowID, &now, nullptr);
	if (!SameDatabase (now, original))
		SwitchToWindow (original);
	if (original.typeID == APIWind_FloorPlanID && CurrentStoryIndex () != originalStory) {
		try {
			GoToStory (originalStory);
		} catch (...) {
		}
	}
}


bool SwitchToWindow (const API_WindowInfo& window)
{
	API_WindowInfo w = window;
	return ACAPI_Automate (APIDo_ChangeWindowID, &w, nullptr) == NoError;
}


void GoToStory (short storyIndex)
{
	API_StoryCmdType cmd;
	BNZeroMemory (&cmd, sizeof (cmd));
	cmd.action = APIStory_GoTo;
	cmd.index = storyIndex;
	GSErrCode err = CallMaybeUndoable ("Go to story (Claude)", [&] () -> GSErrCode {
		API_StoryCmdType c = cmd;
		return ACAPI_Environment (APIEnv_ChangeStorySettingsID, &c, nullptr);
	});
	if (err != NoError)
		Fail (GS::UniString::Printf ("Cannot go to story %d: ", (int) storyIndex) + ErrorName (err) + ". List stories with get_stories.", err);
}


bool IsModelWindow (API_WindowTypeID typeID)
{
	switch (typeID) {
		case APIWind_FloorPlanID:
		case APIWind_SectionID:
		case APIWind_DetailID:
		case APIWind_3DModelID:
		case APIWind_ElevationID:
		case APIWind_InteriorElevationID:
		case APIWind_WorksheetID:
		case APIWind_DocumentFrom3DID:
			return true;
		default:
			return false;
	}
}


TargetWindow OpenTargetWindow (const OS& target, bool allowMaster)
{
	TargetWindow result;
	const int given = (Has (target, "view") ? 1 : 0) + (Has (target, "layout") ? 1 : 0) + (Has (target, "database") ? 1 : 0) +
					  (Has (target, "storyIndex") ? 1 : 0);
	if (given > 1)
		Fail ("Pass only one of view, layout, database, storyIndex.");

	API_WindowInfo before;
	BNZeroMemory (&before, sizeof (before));
	ACAPI_Database (APIDb_GetCurrentWindowID, &before, nullptr);

	if (Has (target, "view")) {
		const API_Guid viewGuid = GetGuid (target, "view");
		API_NavigatorItem item;
		if (!GetNavigatorItem (viewGuid, item))
			Fail ("No navigator item with guid " + GuidStr (viewGuid) + ". Use get_databases {includeViews: true} to list views.", APIERR_BADID);
		if (item.mapId == API_PublicViewMap || item.mapId == API_MyViewMap) {
			const GS::String guidStr (APIGuidToString (viewGuid).ToCStr ().Get ());
			const GSErrCode err = CallMaybeUndoable ("Go to view (Claude)", [&] () -> GSErrCode {
				return ACAPI_Automate (APIDo_GoToViewID, const_cast<char*> (guidStr.ToCStr ()), nullptr);
			});
			if (err != NoError)
				Fail ("Cannot open view '" + GS::UniString (item.uName) + "': " + ErrorName (err) + ".", err);
		} else if (item.itemType == API_StoryNavItem) {
			API_WindowInfo plan;
			BNZeroMemory (&plan, sizeof (plan));
			plan.typeID = APIWind_FloorPlanID;
			if (!SwitchToWindow (plan))
				Fail ("Cannot switch to the floor plan window.");
			GoToStory (item.floorNum);
		} else if (item.db.typeID != API_ZombieWindowID) {
			API_WindowInfo w = item.db;
			FillDatabaseInfo (w);
			w.typeID = item.db.typeID;
			if (!SwitchToWindow (w))
				Fail ("Cannot open '" + GS::UniString (item.uName) + "' (" + NavItemTypeName (item.itemType) + ") in a window.");
		} else {
			Fail ("Navigator item '" + GS::UniString (item.uName) + "' (" + NavItemTypeName (item.itemType) + ") cannot be opened as a 2D/3D window.");
		}
		result.json.Add ("view", NavItemJson (item, false));
		result.description = "view '" + GS::UniString (item.uName) + "'";
	} else if (Has (target, "layout")) {
		const API_DatabaseInfo layout = ResolveLayout (GetString (target, "layout"), allowMaster);
		if (!SwitchToWindow (layout))
			Fail ("Cannot open " + DbLabel (layout) + " in a window.");
		result.description = DbLabel (layout);
	} else if (Has (target, "database")) {
		const API_DatabaseInfo db = ResolveDatabase (GetString (target, "database"));
		if (db.typeID == APIWind_FloorPlanID && Has (target, "storyIndex") == false) {
			API_WindowInfo plan;
			BNZeroMemory (&plan, sizeof (plan));
			plan.typeID = APIWind_FloorPlanID;
			if (!SwitchToWindow (plan))
				Fail ("Cannot switch to the floor plan window.");
		} else if (!SwitchToWindow (db)) {
			Fail ("Cannot open " + DbLabel (db) + " in a window.");
		}
		result.description = DbLabel (db);
	} else if (Has (target, "storyIndex")) {
		auto story = OptStory (target, "storyIndex");
		if (!story.has_value ())
			Fail ("Invalid storyIndex.");
		API_WindowInfo plan;
		BNZeroMemory (&plan, sizeof (plan));
		plan.typeID = APIWind_FloorPlanID;
		if (!SwitchToWindow (plan))
			Fail ("Cannot switch to the floor plan window.");
		GoToStory (*story);
		result.description = GS::UniString::Printf ("floor plan, story %d", (int) *story);
	}

	result.window = CurrentWindow ();
	result.changed = !SameDatabase (before, result.window);
	API_DatabaseInfo w = result.window;
	FillDatabaseInfo (w);
	w.typeID = result.window.typeID;
	if (result.description.IsEmpty ())
		result.description = DbLabel (w);
	result.json.Add ("window", DatabaseJson (w));
	if (w.typeID == APIWind_FloorPlanID)
		result.json.Add ("storyIndex", (Int32) CurrentStoryIndex ());
	return result;
}

// =============================================================================
// Files
// =============================================================================

namespace {

fs::path FsPath (const GS::UniString& path)
{
	return fs::u8path (Utf8 (path));
}


GS::UniString FromFsPath (const fs::path& p)
{
	return GS::UniString (p.u8string ().c_str (), CC_UTF8);
}


GS::UniString ExpandHome (const GS::UniString& path)
{
	if (path == "~" || path.BeginsWith ("~/")) {
		const char* home = std::getenv ("HOME");
		if (home != nullptr)
			return GS::UniString (home, CC_UTF8) + path.GetSubstring (1, path.GetLength () - 1);
	}
	return path;
}


GS::UniString ParentOf (const GS::UniString& path)
{
	return FromFsPath (FsPath (path).parent_path ());
}

} // namespace


GS::UniString FileExtension (const GS::UniString& path)
{
	UIndex start = 0;
	for (UIndex i = 0; i < path.GetLength (); ++i) {
		if (path[i] == '/' || path[i] == '\\')
			start = i + 1;
	}
	const UIndex dot = path.FindLast (GS::UniChar ('.'));
	if (dot == MaxUIndex || dot < start || dot + 1 >= path.GetLength ())
		return GS::UniString ();
	GS::UniString ext = path.GetSubstring (dot + 1, path.GetLength () - dot - 1);
	ext.SetToLowerCase ();
	return ext;
}


bool IsAbsolutePath (const GS::UniString& path)
{
#if defined (WINDOWS)
	return path.GetLength () > 2 && (path[1] == ':' || path.BeginsWith ("\\\\"));
#else
	return path.BeginsWith (GS::UniChar ('/'));
#endif
}


bool PathExists (const GS::UniString& path)
{
	std::error_code ec;
	return fs::exists (FsPath (path), ec);
}


bool IsDirectory (const GS::UniString& path)
{
	std::error_code ec;
	return fs::is_directory (FsPath (path), ec);
}


Int64 FileSize (const GS::UniString& path)
{
	std::error_code ec;
	const fs::path p = FsPath (path);
	if (!fs::is_regular_file (p, ec))
		return -1;
	const auto size = fs::file_size (p, ec);
	return ec ? -1 : (Int64) size;
}


GS::UniString PrepareOutputPath (const OS& params, const char* key, const GS::Array<GS::UniString>& extensions, const GS::UniString& defaultExt)
{
	GS::UniString path = ExpandHome (GetString (params, key));
	path.Trim ();
	if (path.IsEmpty ())
		Fail ("'" + GS::UniString (key) + "' must not be empty.");
	if (!IsAbsolutePath (path))
		Fail ("'" + GS::UniString (key) + "' must be an absolute file path (e.g. /Users/me/Exports/model." + defaultExt + "), got '" + path + "'.");
	if (path.EndsWith ("/") || IsDirectory (path))
		Fail ("'" + path + "' is a folder; pass a full file path including the file name.");

	GS::UniString ext = FileExtension (path);
	if (ext.IsEmpty ()) {
		path += "." + defaultExt;
		ext = defaultExt;
	} else if (!extensions.IsEmpty () && !extensions.Contains (ext)) {
		GS::UniString allowed;
		for (const GS::UniString& e : extensions) {
			if (!allowed.IsEmpty ())
				allowed += ", ";
			allowed += "." + e;
		}
		Fail ("Unsupported file extension '." + ext + "' for '" + GS::UniString (key) + "'. Allowed: " + allowed + ".");
	}

	const GS::UniString parent = ParentOf (path);
	if (!IsDirectory (parent)) {
		if (!GetBool (params, "createFolders", false))
			Fail ("The folder '" + parent + "' does not exist. Pass createFolders: true to create it.");
		std::error_code ec;
		fs::create_directories (FsPath (parent), ec);
		if (ec || !IsDirectory (parent))
			Fail ("Cannot create the folder '" + parent + "': " + GS::UniString (ec.message ().c_str (), CC_UTF8));
	}
	if (PathExists (path)) {
		if (!GetBool (params, "overwrite", false))
			Fail ("The file '" + path + "' already exists. Pass overwrite: true to replace it.");
		std::error_code ec;
		fs::remove (FsPath (path), ec);
		if (ec)
			Fail ("Cannot replace '" + path + "': " + GS::UniString (ec.message ().c_str (), CC_UTF8));
	}
	return path;
}


GS::UniString PrepareOutputFolder (const GS::UniString& pathIn, bool createFolders, const char* key)
{
	GS::UniString path = ExpandHome (pathIn);
	path.Trim ();
	if (!IsAbsolutePath (path))
		Fail ("'" + GS::UniString (key) + "' must be an absolute folder path, got '" + path + "'.");
	if (!IsDirectory (path)) {
		if (PathExists (path))
			Fail ("'" + path + "' is a file, not a folder.");
		if (!createFolders)
			Fail ("The folder '" + path + "' does not exist. Pass createFolders: true to create it.");
		std::error_code ec;
		fs::create_directories (FsPath (path), ec);
		if (ec || !IsDirectory (path))
			Fail ("Cannot create the folder '" + path + "': " + GS::UniString (ec.message ().c_str (), CC_UTF8));
	}
	return path;
}


GS::UniString RequireInputFile (const OS& params, const char* key)
{
	GS::UniString path = ExpandHome (GetString (params, key));
	path.Trim ();
	if (!IsAbsolutePath (path))
		Fail ("'" + GS::UniString (key) + "' must be an absolute file path, got '" + path + "'.");
	if (!PathExists (path))
		Fail ("The file '" + path + "' does not exist.", APIERR_BADPARS);
	if (IsDirectory (path))
		Fail ("'" + path + "' is a folder, not a file.");
	return path;
}


OS FileJson (const GS::UniString& path)
{
	OS out;
	out.Add ("path", path);
	const Int64 size = FileSize (path);
	out.Add ("exists", size >= 0);
	if (size >= 0)
		out.Add ("sizeBytes", (double) size);
	return out;
}


IO::Location ToLocation (const GS::UniString& path)
{
	IO::Location loc (path);
	if (loc.GetStatus () != NoError)
		Fail ("Invalid path '" + path + "'.");
	return loc;
}


GS::UniString LocationPath (const IO::Location& loc)
{
	GS::UniString path;
	if (loc.ToPath (&path) != NoError || path.IsEmpty ())
		path = loc.ToDisplayText ();
	return path;
}


Int64 NowEpochSeconds ()
{
	return (Int64) std::chrono::duration_cast<std::chrono::seconds> (std::chrono::system_clock::now ().time_since_epoch ()).count ();
}


GS::Array<OS> RecentFiles (const GS::UniString& folder, Int64 sinceEpochSeconds, Int32 maxEntries)
{
	GS::Array<OS> out;
	std::error_code ec;
	const fs::path root = FsPath (folder);
	if (!fs::is_directory (root, ec))
		return out;
	// file_time_type has an unspecified epoch in C++17: compare against "now" on the same clock.
	const auto fileNow = fs::file_time_type::clock::now ();
	const Int64 ageLimit = NowEpochSeconds () - sinceEpochSeconds;
	fs::recursive_directory_iterator it (root, fs::directory_options::skip_permission_denied, ec), end;
	for (; !ec && it != end; it.increment (ec)) {
		std::error_code ec2;
		if (!it->is_regular_file (ec2))
			continue;
		const auto mtime = fs::last_write_time (it->path (), ec2);
		if (ec2)
			continue;
		const Int64 age = (Int64) std::chrono::duration_cast<std::chrono::seconds> (fileNow - mtime).count ();
		if (age > ageLimit)
			continue;
		OS f ("path", FromFsPath (it->path ()));
		const auto size = fs::file_size (it->path (), ec2);
		if (!ec2)
			f.Add ("sizeBytes", (double) size);
		out.Push (f);
		if ((Int32) out.GetSize () >= maxEntries)
			break;
	}
	return out;
}

// =============================================================================
// Misc
// =============================================================================

namespace {

class RefCollector : public OS::Processor {
public:
	explicit RefCollector (const GS::Array<const char*>& keys) : keys (keys) {}
	GS::Array<GS::UniString>	values;
	GS::Array<GS::UniString>	errors;

	void StringFound (const GS::String&, const GS::UniString& value) override { values.Push (value); }
	void IntFound (const GS::String&, Int64 value) override { values.Push (GS::UniString (std::to_string (value).c_str ())); }
	void UIntFound (const GS::String&, UInt64 value) override { values.Push (GS::UniString (std::to_string (value).c_str ())); }
	bool ObjectFound (const GS::String&, const OS& value) override
	{
		for (const char* k : keys) {
			if (value.Contains (k) && value.IsString (k)) {
				GS::UniString s;
				value.Get (k, s);
				values.Push (s);
				return false;
			}
		}
		GS::UniString allowed;
		for (const char* k : keys) {
			if (!allowed.IsEmpty ())
				allowed += ", ";
			allowed += k;
		}
		errors.Push ("Array items must be strings" + (allowed.IsEmpty () ? GS::UniString () : " or objects with one of: " + allowed) + ".");
		return false;
	}

private:
	const GS::Array<const char*>& keys;
};

} // namespace


GS::Array<GS::UniString> GetRefArray (const OS& params, const char* key, const GS::Array<const char*>& objectKeys)
{
	if (!params.Contains (key))
		return {};
	if (params.IsString (key))
		return { GetString (params, key) };
	if (!params.IsList (key))
		Fail ("Field '" + GS::UniString (key) + "' must be an array.");
	RefCollector collector (objectKeys);
	params.Enumerate (key, collector);
	if (!collector.errors.IsEmpty ())
		Fail ("Field '" + GS::UniString (key) + "': " + collector.errors[0]);
	return collector.values;
}


GSErrCode CallMaybeUndoable (const GS::UniString& undoName, const std::function<GSErrCode ()>& fn)
{
	GSErrCode err = fn ();
	if (err == APIERR_NEEDSUNDOSCOPE) {
		GSErrCode inner = NoError;
		const GSErrCode undoErr = ACAPI_CallUndoableCommand (undoName, [&] () -> GSErrCode {
			inner = fn ();
			return inner;
		});
		err = inner != NoError ? inner : undoErr;
	}
	return err;
}

} // namespace doc
} // namespace cc
