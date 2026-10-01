// *****************************************************************************
// ViewsCommon.cpp — shared helpers of the views family (see ViewsCommon.hpp).
// *****************************************************************************

#include "Commands/ViewsCommon.hpp"

#include "Core/Command.hpp"
#include "Core/Types.hpp"

#include <cstring>

namespace cc {
namespace views {

// =============================================================================
// Window types
// =============================================================================

namespace {

struct WindowTypeEntry {
	const char*			name;
	API_WindowTypeID	type;
};

const WindowTypeEntry kWindowTypes[] = {
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

const WindowTypeEntry kWindowTypeAliases[] = {
	{ "3D",						APIWind_3DModelID },
	{ "3DWindow",				APIWind_3DModelID },
	{ "Model3D",				APIWind_3DModelID },
	{ "Plan",					APIWind_FloorPlanID },
	{ "Story",					APIWind_FloorPlanID },
	{ "3DDocument",				APIWind_DocumentFrom3DID },
	{ "Schedule",				APIWind_IESCommonDrawingID },
};

} // namespace


GS::UniString WindowTypeName (API_WindowTypeID type)
{
	for (const WindowTypeEntry& e : kWindowTypes) {
		if (e.type == type)
			return e.name;
	}
	GS::UniString s;
	s.Printf ("WindowType%d", (int) type);
	return s;
}


std::optional<API_WindowTypeID> ParseWindowType (const GS::UniString& name)
{
	for (const WindowTypeEntry& e : kWindowTypes) {
		if (EqualsIgnoreCase (name, e.name))
			return e.type;
	}
	for (const WindowTypeEntry& e : kWindowTypeAliases) {
		if (EqualsIgnoreCase (name, e.name))
			return e.type;
	}
	return std::nullopt;
}


API_WindowTypeID GetWindowType (const OS& os, const char* key)
{
	const GS::UniString name = GetString (os, key);
	if (auto t = ParseWindowType (name))
		return *t;
	Fail ("Invalid window type '" + name + "' for '" + GS::UniString (key) + "'. Allowed: FloorPlan, 3D, Section, Elevation, "
		  "InteriorElevation, Detail, Worksheet, Layout, MasterLayout, DocumentFrom3D.");
}


bool Is2DWindow (API_WindowTypeID type)
{
	switch (type) {
		case APIWind_FloorPlanID:
		case APIWind_SectionID:
		case APIWind_DetailID:
		case APIWind_LayoutID:
		case APIWind_DrawingID:
		case APIWind_MasterLayoutID:
		case APIWind_ElevationID:
		case APIWind_InteriorElevationID:
		case APIWind_WorksheetID:
		case APIWind_DocumentFrom3DID:
			return true;
		default:
			return false;
	}
}


const GS::Array<API_WindowTypeID>& ViewpointTypes ()
{
	static const GS::Array<API_WindowTypeID> types = {
		APIWind_SectionID, APIWind_ElevationID, APIWind_InteriorElevationID, APIWind_DetailID, APIWind_WorksheetID,
		APIWind_DocumentFrom3DID, APIWind_LayoutID, APIWind_MasterLayoutID
	};
	return types;
}

// =============================================================================
// Strings
// =============================================================================

GS::UniString UStrN (const GS::uchar_t* s, USize capacity)
{
	if (s == nullptr)
		return GS::UniString ();
	USize len = 0;
	while (len < capacity && s[len] != 0)
		++len;
	return GS::UniString (s, len);
}


void SetUStrN (GS::uchar_t* dst, USize capacity, const GS::UniString& value)
{
	if (dst == nullptr || capacity == 0)
		return;
	std::memset (dst, 0, capacity * sizeof (GS::uchar_t));
	const GS::UniChar::Layout* src = value.ToUStr ();
	const USize n = GS::Min (value.GetLength (), capacity - 1);
	for (USize i = 0; i < n; ++i)
		dst[i] = src[i];
}


namespace {

bool IsValidUtf8 (const unsigned char* s, USize len)
{
	USize i = 0;
	while (i < len) {
		const unsigned char c = s[i];
		USize extra = 0;
		if (c < 0x80)					extra = 0;
		else if ((c & 0xE0) == 0xC0)	extra = 1;
		else if ((c & 0xF0) == 0xE0)	extra = 2;
		else if ((c & 0xF8) == 0xF0)	extra = 3;
		else							return false;
		for (USize k = 1; k <= extra; ++k) {
			if (i + k >= len || (s[i + k] & 0xC0) != 0x80)
				return false;
		}
		i += extra + 1;
	}
	return true;
}

} // namespace


GS::UniString DecodeCStrN (const char* s, USize capacity)
{
	if (s == nullptr)
		return GS::UniString ();
	USize len = 0;
	while (len < capacity && s[len] != 0)
		++len;
	if (len == 0)
		return GS::UniString ();
	if (IsValidUtf8 (reinterpret_cast<const unsigned char*> (s), len))
		return GS::UniString (s, len, CC_UTF8);
	return GS::UniString (s, len, CC_Default);
}


void CopyCStrN (char* dst, USize capacity, const char* src)
{
	if (dst == nullptr || capacity == 0)
		return;
	std::memset (dst, 0, capacity);
	if (src == nullptr)
		return;
	USize n = 0;
	while (n + 1 < capacity && src[n] != 0)
		++n;
	std::memcpy (dst, src, n);
}

// =============================================================================
// API calls
// =============================================================================

void CallApi (const GS::UniString& what, const std::function<GSErrCode ()>& fn)
{
	const GSErrCode err = fn ();
	if (err == APIERR_NEEDSUNDOSCOPE) {
		Undoable (what + " (Claude)", [&] () {
			Check (fn (), what);
		});
		return;
	}
	Check (err, what);
}

// =============================================================================
// Current window / database
// =============================================================================

bool TryGetCurrentWindow (API_WindowInfo& info)
{
	BNZeroMemory (&info, sizeof (info));
	return ACAPI_Database (APIDb_GetCurrentWindowID, &info, nullptr) == NoError;
}


API_WindowInfo GetCurrentWindow ()
{
	API_WindowInfo info;
	if (!TryGetCurrentWindow (info))
		Fail ("There is no active Archicad window. Open a project, then use open_view to open a floor plan, 3D, section or layout window.", APIERR_BADWINDOW);
	return info;
}


OS StoryJson (short index)
{
	return OS ("index", (Int32) index, "name", StoryName (index), "level", StoryLevel (index));
}


OS WindowJson (const API_WindowInfo& info)
{
	OS out;
	out.Add ("type", WindowTypeName (info.typeID));
	GS::UniString name = UStr (info.name);
	GS::UniString ref = UStr (info.ref);
	GS::UniString title = UStr (info.title);
	if (info.databaseUnId.elemSetId != APINULLGuid) {
		out.Add ("database", GuidStr (info.databaseUnId.elemSetId));
		if (name.IsEmpty () && ref.IsEmpty () && title.IsEmpty ()) {
			API_DatabaseInfo db;
			BNZeroMemory (&db, sizeof (db));
			db.typeID = info.typeID;
			db.databaseUnId = info.databaseUnId;
			if (ACAPI_Database (APIDb_GetDatabaseInfoID, &db, nullptr) == NoError) {
				name = UStr (db.name);
				ref = UStr (db.ref);
				title = UStr (db.title);
			}
		}
	}
	if (!name.IsEmpty ())	out.Add ("name", name);
	if (!ref.IsEmpty ())	out.Add ("reference", ref);
	if (!title.IsEmpty ())	out.Add ("title", title);
	if (info.linkedElement != APINULLGuid)
		out.Add ("linkedElement", GuidStr (info.linkedElement));
	if (info.typeID == APIWind_LayoutID && info.masterLayoutUnId.elemSetId != APINULLGuid)
		out.Add ("masterLayout", GuidStr (info.masterLayoutUnId.elemSetId));
	return out;
}


OS CurrentWindowJson ()
{
	API_WindowInfo info;
	if (!TryGetCurrentWindow (info)) {
		OS out ("type", GS::UniString ("None"));
		out.Add ("hint", GS::UniString ("No window is active (is a project open?). Use open_view to open one."));
		return out;
	}
	OS out = WindowJson (info);
	if (info.typeID == APIWind_FloorPlanID)
		out.Add ("story", StoryJson (CurrentStoryIndex ()));
	if (Is2DWindow (info.typeID)) {
		double scale = 0.0;
		if (ACAPI_Database (APIDb_GetDrawingScaleID, &scale, nullptr) == NoError && scale > 0.0)
			out.Add ("drawingScale", scale);
		API_Box box = {};
		if (ACAPI_Database (APIDb_GetZoomID, &box, nullptr) == NoError && box.xMax > box.xMin)
			out.Add ("zoom", BoxObj (box));
	}
	if (info.typeID == APIWind_3DModelID) {
		API_3DProjectionInfo proj;
		BNZeroMemory (&proj, sizeof (proj));
		if (ACAPI_Environment (APIEnv_Get3DProjectionSetsID, &proj, nullptr) == NoError)
			out.Add ("projection", GS::UniString (proj.isPersp ? "perspective" : "axonometric"));
	}
	return out;
}


CurrentDatabaseSwitch::~CurrentDatabaseSwitch ()
{
	if (switched) {
		API_DatabaseInfo db = original;
		ACAPI_Database (APIDb_ChangeCurrentDatabaseID, &db, nullptr);
	}
}


void CurrentDatabaseSwitch::SwitchTo (const API_DatabaseInfo& target)
{
	if (!switched) {
		BNZeroMemory (&original, sizeof (original));
		Check (ACAPI_Database (APIDb_GetCurrentDatabaseID, &original, nullptr), "Cannot read the current database");
	}
	API_DatabaseInfo db = target;
	Check (ACAPI_Database (APIDb_ChangeCurrentDatabaseID, &db, nullptr), "Cannot switch to the " + WindowTypeName (target.typeID) + " database");
	switched = true;
}


void CurrentDatabaseSwitch::EnsureType (API_DatabaseTypeID type)
{
	API_DatabaseInfo current;
	BNZeroMemory (&current, sizeof (current));
	if (ACAPI_Database (APIDb_GetCurrentDatabaseID, &current, nullptr) == NoError && current.typeID == type)
		return;
	API_DatabaseInfo target;
	BNZeroMemory (&target, sizeof (target));
	target.typeID = type;
	SwitchTo (target);
}


bool CurrentDatabaseSwitch::EnsureContaining (const API_Guid& elemGuid)
{
	API_Elem_Head head;
	BNZeroMemory (&head, sizeof (head));
	head.guid = elemGuid;
	if (ACAPI_Element_GetHeader (&head) == NoError)
		return true;
	API_DatabaseInfo container;
	BNZeroMemory (&container, sizeof (container));
	if (ACAPI_Database (APIDb_GetContainingDatabaseID, const_cast<API_Guid*> (&elemGuid), &container) != NoError)
		return false;
	SwitchTo (container);
	return true;
}

// =============================================================================
// Viewpoint databases
// =============================================================================

namespace {

std::optional<API_DatabaseID> DatabaseListCode (API_WindowTypeID type)
{
	switch (type) {
		case APIWind_SectionID:				return APIDb_GetSectionDatabasesID;
		case APIWind_ElevationID:			return APIDb_GetElevationDatabasesID;
		case APIWind_InteriorElevationID:	return APIDb_GetInteriorElevationDatabasesID;
		case APIWind_DetailID:				return APIDb_GetDetailDatabasesID;
		case APIWind_WorksheetID:			return APIDb_GetWorksheetDatabasesID;
		case APIWind_DocumentFrom3DID:		return APIDb_GetDocumentFrom3DDatabasesID;
		case APIWind_LayoutID:				return APIDb_GetLayoutDatabasesID;
		case APIWind_MasterLayoutID:		return APIDb_GetMasterLayoutDatabasesID;
		default:							return std::nullopt;
	}
}

} // namespace


GS::Array<DbEntry> ListDatabases (API_WindowTypeID type)
{
	GS::Array<DbEntry> result;
	const auto code = DatabaseListCode (type);
	if (!code.has_value ())
		return result;
	GS::Array<API_DatabaseUnId> ids;
	if (ACAPI_Database (*code, nullptr, &ids) != NoError)
		return result;
	for (const API_DatabaseUnId& id : ids) {
		API_DatabaseInfo info;
		BNZeroMemory (&info, sizeof (info));
		info.typeID = type;
		info.databaseUnId = id;
		DbEntry entry;
		entry.type = type;
		entry.id = id;
		if (ACAPI_Database (APIDb_GetDatabaseInfoID, &info, nullptr) == NoError) {
			entry.name = UStr (info.name);
			entry.ref = UStr (info.ref);
			entry.title = UStr (info.title);
		}
		result.Push (entry);
	}
	return result;
}


std::optional<DbEntry> FindDatabaseByGuid (const API_Guid& guid)
{
	for (API_WindowTypeID type : ViewpointTypes ()) {
		for (const DbEntry& e : ListDatabases (type)) {
			if (e.id.elemSetId == guid)
				return e;
		}
	}
	return std::nullopt;
}


OS DbEntryJson (const DbEntry& entry)
{
	OS out;
	out.Add ("type", WindowTypeName (entry.type));
	out.Add ("database", GuidStr (entry.id.elemSetId));
	out.Add ("name", entry.name);
	if (!entry.ref.IsEmpty ())		out.Add ("reference", entry.ref);
	if (!entry.title.IsEmpty ())	out.Add ("title", entry.title);
	return out;
}

// =============================================================================
// Navigator
// =============================================================================

GS::UniString NavItemTypeName (API_NavigatorItemTypeID type)
{
	switch (type) {
		case API_ProjectNavItem:				return "Project";
		case API_StoryNavItem:					return "Story";
		case API_SectionNavItem:				return "Section";
		case API_DetailDrawingNavItem:			return "Detail";
		case API_PerspectiveNavItem:			return "Perspective";
		case API_AxonometryNavItem:				return "Axonometry";
		case API_ListNavItem:					return "List";
		case API_ScheduleNavItem:				return "Schedule";
		case API_TocNavItem:					return "TableOfContents";
		case API_CameraNavItem:					return "Camera";
		case API_CameraSetNavItem:				return "CameraSet";
		case API_InfoNavItem:					return "Info";
		case API_HelpNavItem:					return "Help";
		case API_LayoutNavItem:					return "Layout";
		case API_MasterLayoutNavItem:			return "MasterLayout";
		case API_BookNavItem:					return "LayoutBook";
		case API_MasterFolderNavItem:			return "MasterFolder";
		case API_SubSetNavItem:					return "Subset";
		case API_TextListNavItem:				return "TextList";
		case API_ElevationNavItem:				return "Elevation";
		case API_InteriorElevationNavItem:		return "InteriorElevation";
		case API_WorksheetDrawingNavItem:		return "Worksheet";
		case API_DocumentFrom3DNavItem:			return "DocumentFrom3D";
		case API_FolderNavItem:					return "Folder";
		case API_DrawingNavItem:				return "Drawing";
		default:								return "Undefined";
	}
}


GS::UniString NavMapName (API_NavigatorMapID map)
{
	switch (map) {
		case API_ProjectMap:		return "ProjectMap";
		case API_PublicViewMap:		return "ViewMap";
		case API_MyViewMap:			return "MyViewMap";
		case API_LayoutMap:			return "LayoutBook";
		case API_PublisherSets:		return "PublisherSets";
		default:					return "Undefined";
	}
}


bool IsNavFolder (API_NavigatorItemTypeID type)
{
	switch (type) {
		case API_UndefinedNavItem:
		case API_ProjectNavItem:
		case API_FolderNavItem:
		case API_BookNavItem:
		case API_SubSetNavItem:
		case API_MasterFolderNavItem:
			return true;
		default:
			return false;
	}
}


GS::UniString NavDisplayName (const API_NavigatorItem& item)
{
	const GS::UniString id = CStr (item.uiId);
	GS::UniString name = UStr (item.uAutoTextedName);
	if (name.IsEmpty ())
		name = UStr (item.uName);
	if (id.IsEmpty ())
		return name;
	if (name.IsEmpty ())
		return id;
	return id + " " + name;
}


bool GetNavItem (const API_Guid& guid, API_NavigatorMapID map, API_NavigatorItem& out)
{
	BNZeroMemory (&out, sizeof (out));
	out.mapId = map;
	if (ACAPI_Navigator (APINavigator_GetNavigatorItemID, const_cast<API_Guid*> (&guid), &out) != NoError)
		return false;
	out.mapId = map;
	if (out.guid == APINULLGuid)
		out.guid = guid;
	return true;
}


namespace {

void CollectChildren (const API_NavigatorItem& parent, API_NavigatorMapID map, const GS::UniString& path, Int32 depth,
					  GS::Array<NavEntry>& out, USize limit)
{
	if (depth > 40 || out.GetSize () >= limit)
		return;
	API_NavigatorItem query = parent;
	query.mapId = map;
	GS::Array<API_NavigatorItem> children;
	if (ACAPI_Navigator (APINavigator_GetNavigatorChildrenItemsID, &query, nullptr, &children) != NoError)
		return;
	for (API_NavigatorItem& child : children) {
		if (out.GetSize () >= limit)
			return;
		child.mapId = map;
		NavEntry entry;
		entry.item = child;
		entry.path = path;
		out.Push (entry);
		GS::UniString childName = NavDisplayName (child);
		const GS::UniString childPath = path.IsEmpty () ? childName : path + " / " + childName;
		CollectChildren (child, map, childPath, depth + 1, out, limit);
	}
}

} // namespace


void CollectNavItems (API_NavigatorMapID map, GS::Array<NavEntry>& out, USize limit)
{
	API_NavigatorSet set;
	BNZeroMemory (&set, sizeof (set));
	set.mapId = map;
	Int32 setIndex = 0;
	if (ACAPI_Navigator (APINavigator_GetNavigatorSetID, &set, &setIndex) != NoError)
		return;
	API_NavigatorItem root;
	BNZeroMemory (&root, sizeof (root));
	root.guid = set.rootGuid;
	root.mapId = map;
	CollectChildren (root, map, GS::UniString (), 0, out, limit);
}


OS NavEntryJson (const NavEntry& entry)
{
	const API_NavigatorItem& item = entry.item;
	OS out;
	out.Add ("guid", GuidStr (item.guid));
	out.Add ("name", UStr (item.uName));
	const GS::UniString autoTexted = UStr (item.uAutoTextedName);
	if (!autoTexted.IsEmpty () && autoTexted != UStr (item.uName))
		out.Add ("displayedName", autoTexted);
	const GS::UniString id = CStr (item.uiId);
	if (!id.IsEmpty ())
		out.Add ("id", id);
	out.Add ("fullName", NavDisplayName (item));
	out.Add ("itemType", NavItemTypeName (item.itemType));
	out.Add ("map", NavMapName (item.mapId));
	if (!entry.path.IsEmpty ())
		out.Add ("folder", entry.path);
	if (item.db.typeID != API_ZombieWindowID)
		out.Add ("windowType", WindowTypeName (item.db.typeID));
	if (item.db.databaseUnId.elemSetId != APINULLGuid)
		out.Add ("database", GuidStr (item.db.databaseUnId.elemSetId));
	if (item.itemType == API_StoryNavItem)
		out.Add ("storyIndex", (Int32) item.floorNum);
	if (item.sourceGuid != APINULLGuid)
		out.Add ("sourceItem", GuidStr (item.sourceGuid));
	return out;
}


namespace {

bool TryParseGuidString (const GS::UniString& s, API_Guid& guid)
{
	GS::UniString t = s;
	t.Trim ();
	if (t.GetLength () < 36 || t.GetLength () > 38)
		return false;
	GS::Guid g;
	if (g.ConvertFromString (t.ToCStr ().Get ()) != NoError || g.IsNull ())
		return false;
	guid = GSGuid2APIGuid (g);
	return true;
}

const API_NavigatorMapID kAllMaps[] = { API_PublicViewMap, API_MyViewMap, API_ProjectMap, API_LayoutMap };

} // namespace


NavEntry ResolveNavItem (const OS& os, const char* key, const GS::Array<API_NavigatorMapID>& searchMaps)
{
	GS::UniString name;
	API_Guid guid = APINULLGuid;
	bool byGuid = false;

	if (!os.Contains (key))
		Fail ("Missing '" + GS::UniString (key) + "': give a Navigator item GUID or name (see list_views).");
	if (os.IsString (key)) {
		const GS::UniString s = GetString (os, key);
		if (TryParseGuidString (s, guid))
			byGuid = true;
		else
			name = s;
	} else if (os.IsObject (key)) {
		OS ref = GetObject (os, key);
		if (ref.Contains ("guid") || ref.Contains ("navigatorItemId")) {
			guid = GuidFromItem (ref.Contains ("navigatorItemId") ? GetObject (ref, "navigatorItemId") : ref);
			byGuid = true;
		} else {
			name = GetString (ref, "name");
		}
	} else {
		Fail ("'" + GS::UniString (key) + "' must be a Navigator item GUID or name.");
	}

	if (byGuid) {
		for (API_NavigatorMapID map : kAllMaps) {
			NavEntry entry;
			if (GetNavItem (guid, map, entry.item))
				return entry;
		}
		Fail ("No Navigator item with GUID " + GuidStr (guid) + ". Use list_views to list the View Map, Project Map and Layout Book items.", APIERR_BADID);
	}

	name.Trim ();
	if (name.IsEmpty ())
		Fail ("The view name must not be empty.");

	GS::Array<NavEntry> candidates;
	for (API_NavigatorMapID map : searchMaps) {
		GS::Array<NavEntry> all;
		CollectNavItems (map, all);
		GS::Array<NavEntry> exact, ci, partial;
		for (const NavEntry& e : all) {
			if (IsNavFolder (e.item.itemType))
				continue;
			const GS::UniString full = NavDisplayName (e.item);
			const GS::UniString uName = UStr (e.item.uName);
			const GS::UniString auto_ = UStr (e.item.uAutoTextedName);
			const GS::UniString id = CStr (e.item.uiId);
			if (name == full || name == uName || name == auto_ || (!id.IsEmpty () && name == id))
				exact.Push (e);
			else if (EqualsIgnoreCase (name, full) || EqualsIgnoreCase (name, uName) || EqualsIgnoreCase (name, auto_) || (!id.IsEmpty () && EqualsIgnoreCase (name, id)))
				ci.Push (e);
			else if (full.ToLowerCase ().Contains (GS::UniString (name).ToLowerCase ()))
				partial.Push (e);
		}
		candidates = !exact.IsEmpty () ? exact : !ci.IsEmpty () ? ci : partial;
		if (!candidates.IsEmpty ())
			break;
	}

	if (candidates.GetSize () == 1)
		return candidates[0];
	if (candidates.IsEmpty ())
		Fail ("No view named '" + name + "' found in the Navigator. Use list_views to see the available views (names are localized).", APIERR_BADNAME);

	GS::UniString list;
	for (UIndex i = 0; i < candidates.GetSize () && i < 15; ++i) {
		if (!list.IsEmpty ()) list += "; ";
		list += NavDisplayName (candidates[i].item) + " [" + GuidStr (candidates[i].item.guid) + "]";
	}
	Fail ("The name '" + name + "' matches several views: " + list + ". Pass the GUID instead.", APIERR_BADNAME);
}

// =============================================================================
// Switching windows
// =============================================================================

void OpenWindow (const API_WindowInfo& info, const GS::UniString& label)
{
	API_WindowInfo w = info;
	const GSErrCode err = ACAPI_Automate (APIDo_ChangeWindowID, &w, nullptr);
	if (err == NoError)
		return;
	Fail ("Cannot open " + label + ": " + ErrorName (err) + ". Use list_views to see the views that can be opened.", err);
}


void GoToStory (short index)
{
	API_StoryCmdType cmd;
	BNZeroMemory (&cmd, sizeof (cmd));
	cmd.action = APIStory_GoTo;
	cmd.index = index;
	CallApi ("Go to story", [&] () {
		API_StoryCmdType c = cmd;
		return ACAPI_Environment (APIEnv_ChangeStorySettingsID, &c, nullptr);
	});
}


void Switch3DMode (bool perspective)
{
	API_3DProjectionInfo proj;
	BNZeroMemory (&proj, sizeof (proj));
	Check (ACAPI_Environment (APIEnv_Get3DProjectionSetsID, &proj, nullptr), "Cannot read the 3D projection settings");
	if (proj.isPersp == perspective)
		return;
	proj.isPersp = perspective;
	CallApi (perspective ? GS::UniString ("Switch to perspective") : GS::UniString ("Switch to axonometry"), [&] () {
		API_3DProjectionInfo p = proj;
		bool onlySwitch = true;
		return ACAPI_Environment (APIEnv_Change3DProjectionSetsID, &p, &onlySwitch);
	});
}


void OpenNavItem (const API_NavigatorItem& item)
{
	if (item.db.typeID == API_ZombieWindowID)
		Fail ("The Navigator item '" + NavDisplayName (item) + "' (" + NavItemTypeName (item.itemType) + ") cannot be opened in a window.", APIERR_BADWINDOW);
	if (item.db.typeID == APIWind_3DModelID) {
		if (item.itemType == API_PerspectiveNavItem)
			Switch3DMode (true);
		else if (item.itemType == API_AxonometryNavItem)
			Switch3DMode (false);
	}
	API_WindowInfo w = item.db;
	OpenWindow (w, "'" + NavDisplayName (item) + "'");
	if (item.db.typeID == APIWind_FloorPlanID && item.itemType == API_StoryNavItem)
		GoToStory (item.floorNum);
}

} // namespace views
} // namespace cc
