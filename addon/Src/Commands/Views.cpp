// *****************************************************************************
// Views — windows, navigation, zoom, "show in 3D" and view settings.
// (3D projection / capture / render: Views3D.cpp; cut plane & detail markers:
//  ViewsMarkers.cpp; shared helpers: ViewsCommon.hpp)
//
// Commands:
//   GetCurrentWindow   {}
//   ListViews          {include?: ["stories","viewpoints","layouts","viewMap","projectMap"], types?, nameContains?, limit?}
//   OpenView           {window?, story?, element?, segmentIndex?, database?, name?, navigatorItem?, projection?}
//   GoToView           {view: guid | name | {guid} | {name}}
//   Zoom               {mode: fit|box|elements|selection|in|out|previous|redraw, box?, elements?, factor?, margin?, steps?}
//   ShowIn3D           {mode?: all|selection|elements, elements?}
//   GetViewSettings    {view?}
//   SetViewSettings    {view?, drawingScale?, layerCombination?, modelViewOptions?, penSet?, dimensionStyle?,
//                       graphicOverrides?, renovationFilter?, structureDisplay?, style3D?, renderingScene?, zoom?, ignoreSavedZoom?}
// *****************************************************************************

#include "Commands/Commands.hpp"
#include "Commands/DocumentationShared.hpp"
#include "Commands/ViewsCommon.hpp"
#include "Core/Command.hpp"
#include "Core/Elements.hpp"
#include "Core/Enums.hpp"
#include "Core/Types.hpp"

#include <cmath>
#include <cstddef>
#include <cstring>

namespace cc {

using namespace views;

namespace {

// --- Small utilities ------------------------------------------------------------------

template <typename F>
class ScopeExit {
public:
	explicit ScopeExit (F f) : fn (f) {}
	~ScopeExit () { fn (); }
	ScopeExit (const ScopeExit&) = delete;
	ScopeExit& operator= (const ScopeExit&) = delete;
private:
	F fn;
};

template <typename F>
ScopeExit<F> MakeScopeExit (F f) { return ScopeExit<F> (f); }


bool Matches (const GS::UniString& filter, const GS::Array<GS::UniString>& values)
{
	if (filter.IsEmpty ())
		return true;
	const GS::UniString f = GS::UniString (filter).ToLowerCase ();
	for (const GS::UniString& v : values) {
		if (GS::UniString (v).ToLowerCase ().Contains (f))
			return true;
	}
	return false;
}


const NamedValue kStructureDisplay[] = {
	{ "EntireStructure",	API_EntireStructure },
	{ "CoreOnly",			API_CoreOnly },
	{ "WithoutFinishes",	API_WithoutFinishes },
	{ "StructureOnly",		API_StructureOnly },
};

// --- Attribute names in raw (char) form, as used by API_NavigatorView -------------------

struct AttrNames {
	GS::UniString	uni;
	char			raw[API_AttrNameLen] = {};
	API_Guid		guid = APINULLGuid;
};


bool LoadAttrNames (API_AttrTypeID type, API_AttributeIndex index, AttrNames& out)
{
	API_Attribute attr;
	BNZeroMemory (&attr, sizeof (attr));
	attr.header.typeID = type;
	attr.header.index = index;
	GS::UniString uni;
	attr.header.uniStringNamePtr = &uni;
	if (ACAPI_Attribute_Get (&attr) != NoError)
		return false;
	if (type == API_ModelViewOptionsID)
		ACAPI_FreeGDLModelViewOptionsPtr (&attr.modelViewOpt.modelViewOpt.gdlOptions);
	else if (type == API_MaterialID && attr.material.texture.fileLoc != nullptr)
		delete attr.material.texture.fileLoc;
	out.uni = uni.IsEmpty () ? DecodeCStrN (attr.header.name, API_AttrNameLen) : uni;
	CopyCStrN (out.raw, API_AttrNameLen, attr.header.name);
	out.guid = attr.header.guid;
	return true;
}


// {index, name, guid} of the attribute whose raw name equals `raw`; {name, found: false} otherwise; null-like "" -> not stored.
OS AttrRefFromRawName (API_AttrTypeID type, const char* raw, USize rawCapacity)
{
	const GS::UniString decoded = DecodeCStrN (raw, rawCapacity);
	API_AttributeIndex count = 0;
	if (ACAPI_Attribute_GetNum (type, &count) == NoError) {
		for (API_AttributeIndex i = 1; i <= count; ++i) {
			AttrNames names;
			if (!LoadAttrNames (type, i, names))
				continue;
			if (std::strncmp (names.raw, raw, rawCapacity) == 0 || EqualsIgnoreCase (names.uni, decoded))
				return OS ("index", (Int32) i, "name", names.uni, "guid", GuidStr (names.guid));
		}
	}
	return OS ("name", decoded, "found", false);
}


// Resolves an attribute reference and copies its raw (char) name into dst.
API_AttributeIndex ResolveAttrToRaw (API_AttrTypeID type, const OS& params, const char* key, char* dst, USize capacity)
{
	const API_AttributeIndex index = GetAttr (type, params, key);
	AttrNames names;
	if (!LoadAttrNames (type, index, names))
		Fail ("Cannot read the attribute given in '" + GS::UniString (key) + "'.", APIERR_BADINDEX);
	CopyCStrN (dst, capacity, names.raw);
	return index;
}

// --- Renovation filters, override combinations, 3D styles, rendering scenes -------------

GS::UniString RenovationFilterName (const API_Guid& guid)
{
	GS::UniString name;
	ACAPI_Goodies (APIAny_GetRenovationFilterNameID, const_cast<API_Guid*> (&guid), &name);
	return name;
}


GS::Array<API_Guid> RenovationFilters ()
{
	GS::Array<API_Guid> list;
	ACAPI_Database (APIDb_GetRenovationFiltersID, &list, nullptr);
	return list;
}


OS RenovationFilterJson (const API_Guid& guid)
{
	return OS ("guid", GuidStr (guid), "name", RenovationFilterName (guid));
}


API_Guid ResolveRenovationFilter (const OS& params, const char* key)
{
	const GS::Array<API_Guid> filters = RenovationFilters ();
	GS::UniString text;
	if (params.IsObject (key)) {
		OS ref = GetObject (params, key);
		text = ref.Contains ("guid") ? GetString (ref, "guid") : GetString (ref, "name");
	} else {
		text = GetString (params, key);
	}
	for (const API_Guid& g : filters) {
		if (EqualsIgnoreCase (GuidStr (g), text) || RenovationFilterName (g) == text)
			return g;
	}
	for (const API_Guid& g : filters) {
		if (EqualsIgnoreCase (RenovationFilterName (g), text))
			return g;
	}
	GS::UniString list;
	for (const API_Guid& g : filters) {
		if (!list.IsEmpty ()) list += ", ";
		list += "'" + RenovationFilterName (g) + "'";
	}
	Fail ("Renovation filter '" + text + "' not found. Available: " + list + ".", APIERR_BADNAME);
}


GS::Array<GS::UniString> OverrideCombinationNames ()
{
	GS::Array<GS::UniString> names;
	GS::Array<API_Guid> list;
	if (ACAPI_Override_GetOverrideCombinationList (list) != NoError)
		return names;
	for (const API_Guid& g : list) {
		API_OverrideCombination combination;
		combination.guid = g;
		if (ACAPI_Override_GetOverrideCombination (combination) == NoError)
			names.Push (combination.name);
	}
	return names;
}


GS::Array<GS::UniString> Style3DNames (GS::UniString* current = nullptr)
{
	GS::Array<GS::UniString> names;
	GS::UniString cur;
	ACAPI_Environment (APIEnv_Get3DStyleListID, &names, &cur);
	if (current != nullptr)
		*current = cur;
	return names;
}


GS::Array<GS::UniString> RenderingSceneNames ()
{
	GS::Array<GS::UniString> names;
	ACAPI_Environment (APIEnv_GetRenderingSceneNamesID, &names);
	return names;
}


// Finds `name` in `names` (exact, then case-insensitive); fails listing the allowed values.
GS::UniString PickName (const GS::Array<GS::UniString>& names, const GS::UniString& name, const char* what)
{
	for (const GS::UniString& n : names) {
		if (n == name)
			return n;
	}
	for (const GS::UniString& n : names) {
		if (EqualsIgnoreCase (n, name))
			return n;
	}
	GS::UniString list;
	for (const GS::UniString& n : names) {
		if (!list.IsEmpty ()) list += ", ";
		list += "'" + n + "'";
	}
	Fail (GS::UniString (what) + " '" + name + "' not found. Available: " + (list.IsEmpty () ? GS::UniString ("(none)") : list) + ".", APIERR_BADNAME);
}

// --- Stories ------------------------------------------------------------------------------

GS::Array<OS> StoryList ()
{
	GS::Array<OS> result;
	API_StoryInfo info;
	BNZeroMemory (&info, sizeof (info));
	if (ACAPI_Environment (APIEnv_GetStorySettingsID, &info, nullptr) != NoError)
		return result;
	if (info.data != nullptr) {
		const Int32 n = info.lastStory - info.firstStory + 1;
		const Int32 available = (Int32) (BMGetHandleSize ((GSConstHandle) info.data) / sizeof (API_StoryType));
		for (Int32 i = 0; i < n && i < available; ++i) {
			const API_StoryType& s = (*info.data)[i];
			OS story ("index", (Int32) s.index, "name", GS::UniString (s.uName), "level", s.level);
			story.Add ("isCurrent", s.index == info.actStory);
			result.Push (story);
		}
	}
	BMKillHandle (reinterpret_cast<GSHandle*> (&info.data));
	return result;
}

// --- Markers -> viewpoint database -------------------------------------------------------

struct MarkerTarget {
	API_WindowTypeID	type = API_ZombieWindowID;
	API_DatabaseUnId	db = {};
	GS::UniString		label;
};


MarkerTarget MarkerDatabase (const API_Guid& guid, Int32 segmentIndex)
{
	API_Element element;
	BNZeroMemory (&element, sizeof (element));
	element.header.guid = guid;
	GSErrCode err = ACAPI_Element_Get (&element);
	if (err != NoError) {
		BNZeroMemory (&element, sizeof (element));
		element.header.guid = guid;
		err = ACAPI_Database (APIDb_GetElementFromAnywhereID, const_cast<API_Guid*> (&guid), &element);
	}
	if (err != NoError)
		Fail ("Element " + GuidStr (guid) + " not found: " + ErrorName (err) + ". Use find_elements with type CutPlane/Elevation/InteriorElevation/Detail/Worksheet.", err);

	MarkerTarget target;
	const API_ElemTypeID typeID = element.header.type.typeID;
	switch (typeID) {
		case API_CutPlaneID:
			target.type = APIWind_SectionID;
			target.db = element.cutPlane.segment.databaseID;
			target.label = "section '" + UStr (element.cutPlane.segment.cutPlName) + "'";
			break;
		case API_ElevationID:
			target.type = APIWind_ElevationID;
			target.db = element.elevation.segment.databaseID;
			target.label = "elevation '" + UStr (element.elevation.segment.cutPlName) + "'";
			break;
		case API_DetailID:
			target.type = APIWind_DetailID;
			target.db = element.detail.databaseID;
			target.label = "detail '" + UStr (element.detail.detailName) + "'";
			break;
		case API_WorksheetID:
			target.type = APIWind_WorksheetID;
			target.db = element.worksheet.databaseID;
			target.label = "worksheet '" + UStr (element.worksheet.detailName) + "'";
			break;
		case API_InteriorElevationID: {
			CurrentDatabaseSwitch dbSwitch;
			dbSwitch.EnsureContaining (guid);
			Memo memo;
			Check (ACAPI_Element_GetMemo (guid, memo.Ptr (), APIMemoMask_SectionSegments), "Cannot read the interior elevation segments");
			const USize n = memo->intElevSegments != nullptr ? (USize) (BMGetPtrSize (reinterpret_cast<GSConstPtr> (memo->intElevSegments)) / sizeof (API_SectionSegment)) : 0;
			if (n == 0)
				Fail ("The interior elevation has no segments.", APIERR_GENERAL);
			if (segmentIndex < 0 || (USize) segmentIndex >= n) {
				GS::UniString msg;
				msg.Printf ("segmentIndex %d is out of range: this interior elevation has %u segments (0..%u).", (int) segmentIndex, (unsigned) n, (unsigned) (n - 1));
				Fail (msg, APIERR_BADINDEX);
			}
			target.type = APIWind_InteriorElevationID;
			target.db = memo->intElevSegments[segmentIndex].databaseID;
			target.label = "interior elevation '" + UStr (memo->intElevSegments[segmentIndex].cutPlName) + "'";
			break;
		}
		default:
			Fail ("Element " + GuidStr (guid) + " is a " + ElemTypeName (typeID) + ". open_view {element} needs a Section (CutPlane), Elevation, "
				  "InteriorElevation, Detail or Worksheet marker.", APIERR_BADELEMENTTYPE);
	}
	if (target.db.elemSetId == APINULLGuid)
		Fail ("The " + target.label + " marker has no viewpoint of its own (it is a linked or unlinked marker, or not generated yet). "
			  "Open the referenced view with go_to_view / list_views instead.", APIERR_BADDATABASE);
	return target;
}

// --- Viewpoint lookup by name --------------------------------------------------------------

DbEntry FindDatabaseByName (const GS::Array<API_WindowTypeID>& types, const GS::UniString& name)
{
	GS::Array<DbEntry> exact, ci, partial, all;
	for (API_WindowTypeID t : types) {
		for (const DbEntry& e : ListDatabases (t)) {
			all.Push (e);
			const GS::UniString refName = e.ref.IsEmpty () ? e.name : e.ref + " " + e.name;
			if (name == e.name || name == e.ref || name == e.title || name == refName)
				exact.Push (e);
			else if (EqualsIgnoreCase (name, e.name) || EqualsIgnoreCase (name, e.ref) || EqualsIgnoreCase (name, e.title) || EqualsIgnoreCase (name, refName))
				ci.Push (e);
			else if (Matches (name, { e.name, e.ref, e.title }))
				partial.Push (e);
		}
	}
	const GS::Array<DbEntry>& found = !exact.IsEmpty () ? exact : !ci.IsEmpty () ? ci : partial;
	if (found.GetSize () == 1)
		return found[0];

	auto listOf = [] (const GS::Array<DbEntry>& entries) {
		GS::UniString list;
		for (UIndex i = 0; i < entries.GetSize () && i < 20; ++i) {
			if (!list.IsEmpty ()) list += "; ";
			list += WindowTypeName (entries[i].type) + " '" + (entries[i].title.IsEmpty () ? entries[i].name : entries[i].title) + "' [" + GuidStr (entries[i].id.elemSetId) + "]";
		}
		if (entries.GetSize () > 20)
			list += "; ...";
		return list;
	};
	if (found.IsEmpty ())
		Fail ("No view named '" + name + "'. Available: " + (all.IsEmpty () ? GS::UniString ("(none)") : listOf (all)) + ". Names are localized; see list_views.", APIERR_BADNAME);
	Fail ("'" + name + "' matches several views: " + listOf (found) + ". Pass 'database' (GUID) instead.", APIERR_BADNAME);
}

// =============================================================================
// Commands
// =============================================================================

OS GetCurrentWindowCmd (const OS&)
{
	return CurrentWindowJson ();
}


OS ListViewsCmd (const OS& params)
{
	GS::Array<GS::UniString> include = GetStringArray (params, "include", false);
	if (include.IsEmpty ())
		include = { "stories", "viewpoints", "layouts", "viewMap" };
	auto wants = [&] (const char* part) {
		for (const GS::UniString& s : include) {
			if (EqualsIgnoreCase (s, part) || EqualsIgnoreCase (s, "all"))
				return true;
		}
		return false;
	};
	for (const GS::UniString& s : include) {
		if (!EqualsIgnoreCase (s, "stories") && !EqualsIgnoreCase (s, "viewpoints") && !EqualsIgnoreCase (s, "layouts") &&
			!EqualsIgnoreCase (s, "viewMap") && !EqualsIgnoreCase (s, "projectMap") && !EqualsIgnoreCase (s, "all"))
			Fail ("Invalid 'include' value '" + s + "'. Allowed: stories, viewpoints, layouts, viewMap, projectMap, all.");
	}

	GS::Array<API_WindowTypeID> typeFilter;
	for (const GS::UniString& t : GetStringArray (params, "types", false)) {
		auto parsed = ParseWindowType (t);
		if (!parsed.has_value ())
			Fail ("Invalid view type '" + t + "' in 'types'. Allowed: Section, Elevation, InteriorElevation, Detail, Worksheet, DocumentFrom3D, Layout, MasterLayout.");
		typeFilter.Push (*parsed);
	}
	const GS::UniString nameFilter = GetString (params, "nameContains", GS::UniString ());
	const Int32 limit = GS::Max (1, GetInt (params, "limit", 1000));

	OS out;
	out.Add ("current", CurrentWindowJson ());

	if (wants ("stories")) {
		GS::Array<OS> stories;
		for (const OS& s : StoryList ()) {
			GS::UniString n;
			s.Get ("name", n);
			if (Matches (nameFilter, { n }))
				stories.Push (s);
		}
		out.Add ("stories", stories);
	}

	auto addDatabases = [&] (const char* key, const GS::Array<API_WindowTypeID>& types) {
		GS::Array<OS> list;
		bool truncated = false;
		for (API_WindowTypeID t : types) {
			if (!typeFilter.IsEmpty () && !typeFilter.Contains (t))
				continue;
			for (const DbEntry& e : ListDatabases (t)) {
				if (!Matches (nameFilter, { e.name, e.ref, e.title }))
					continue;
				if ((Int32) list.GetSize () >= limit) { truncated = true; break; }
				list.Push (DbEntryJson (e));
			}
		}
		out.Add (key, list);
		if (truncated)
			out.Add (GS::String (key) + "Truncated", true);
	};
	if (wants ("viewpoints"))
		addDatabases ("viewpoints", { APIWind_SectionID, APIWind_ElevationID, APIWind_InteriorElevationID, APIWind_DetailID, APIWind_WorksheetID, APIWind_DocumentFrom3DID });
	if (wants ("layouts"))
		addDatabases ("layouts", { APIWind_LayoutID, APIWind_MasterLayoutID });

	auto addMap = [&] (const char* key, const GS::Array<API_NavigatorMapID>& maps) {
		GS::Array<OS> list;
		bool truncated = false;
		for (API_NavigatorMapID map : maps) {
			GS::Array<NavEntry> entries;
			CollectNavItems (map, entries);
			for (const NavEntry& e : entries) {
				if (IsNavFolder (e.item.itemType))
					continue;
				if (!typeFilter.IsEmpty () && !typeFilter.Contains (e.item.db.typeID))
					continue;
				if (!Matches (nameFilter, { NavDisplayName (e.item), e.path }))
					continue;
				if ((Int32) list.GetSize () >= limit) { truncated = true; break; }
				list.Push (NavEntryJson (e));
			}
		}
		out.Add (key, list);
		if (truncated)
			out.Add (GS::String (key) + "Truncated", true);
	};
	if (wants ("viewMap"))
		addMap ("viewMap", { API_PublicViewMap, API_MyViewMap });
	if (wants ("projectMap"))
		addMap ("projectMap", { API_ProjectMap });
	return out;
}


OS OpenViewCmd (const OS& params)
{
	GS::UniString opened;

	if (params.Contains ("navigatorItem")) {
		const NavEntry entry = ResolveNavItem (params, "navigatorItem", { API_ProjectMap, API_LayoutMap, API_PublicViewMap, API_MyViewMap });
		OpenNavItem (entry.item);
		opened = NavDisplayName (entry.item);
	} else if (params.Contains ("element")) {
		const API_Guid guid = GetGuid (params, "element");
		const MarkerTarget target = MarkerDatabase (guid, GetInt (params, "segmentIndex", 0));
		API_WindowInfo w;
		BNZeroMemory (&w, sizeof (w));
		w.typeID = target.type;
		w.databaseUnId = target.db;
		OpenWindow (w, "the " + target.label);
		opened = target.label;
	} else if (params.Contains ("database")) {
		const API_Guid guid = GetGuid (params, "database");
		API_WindowInfo w;
		BNZeroMemory (&w, sizeof (w));
		w.databaseUnId.elemSetId = guid;
		if (params.Contains ("window")) {
			w.typeID = GetWindowType (params, "window");
		} else {
			auto entry = FindDatabaseByGuid (guid);
			if (!entry.has_value ())
				Fail ("No section/elevation/detail/worksheet/3D document/layout database with GUID " + GuidStr (guid) + ". Use list_views.", APIERR_BADID);
			w.typeID = entry->type;
		}
		OpenWindow (w, WindowTypeName (w.typeID) + " " + GuidStr (guid));
		opened = WindowTypeName (w.typeID);
	} else if (params.Contains ("window") || params.Contains ("story") || params.Contains ("name")) {
		API_WindowTypeID type = params.Contains ("window") ? GetWindowType (params, "window")
							  : params.Contains ("story") ? APIWind_FloorPlanID : API_ZombieWindowID;
		if (params.Contains ("story") && type != APIWind_FloorPlanID)
			Fail ("'story' can only be used with window 'FloorPlan'.");
		if (type == APIWind_FloorPlanID) {
			const std::optional<short> story = OptStory (params, "story");
			API_WindowInfo w;
			BNZeroMemory (&w, sizeof (w));
			w.typeID = APIWind_FloorPlanID;
			OpenWindow (w, "the floor plan");
			if (story.has_value ())
				GoToStory (*story);
			opened = "FloorPlan";
		} else if (type == APIWind_3DModelID) {
			if (auto proj = OptString (params, "projection")) {
				if (EqualsIgnoreCase (*proj, "perspective"))		Switch3DMode (true);
				else if (EqualsIgnoreCase (*proj, "axonometric") || EqualsIgnoreCase (*proj, "parallel"))	Switch3DMode (false);
				else Fail ("projection must be 'perspective' or 'axonometric'.");
			}
			API_WindowInfo w;
			BNZeroMemory (&w, sizeof (w));
			w.typeID = APIWind_3DModelID;
			OpenWindow (w, "the 3D window");
			opened = "3D";
		} else {
			GS::Array<API_WindowTypeID> types;
			if (type == API_ZombieWindowID) {
				types = ViewpointTypes ();
			} else {
				if (!ViewpointTypes ().Contains (type))
					Fail ("Window type '" + WindowTypeName (type) + "' cannot be opened with open_view. Allowed: FloorPlan, 3D, Section, Elevation, "
						  "InteriorElevation, Detail, Worksheet, Layout, MasterLayout, DocumentFrom3D (schedules/lists: go_to_view).");
				types.Push (type);
			}
			DbEntry entry;
			if (params.Contains ("name")) {
				entry = FindDatabaseByName (types, GetString (params, "name"));
			} else {
				GS::Array<DbEntry> all = ListDatabases (type);
				if (all.GetSize () != 1) {
					GS::UniString list;
					for (UIndex i = 0; i < all.GetSize () && i < 20; ++i) {
						if (!list.IsEmpty ()) list += "; ";
						list += "'" + (all[i].title.IsEmpty () ? all[i].name : all[i].title) + "'";
					}
					Fail (all.IsEmpty ()
						  ? "The project has no " + WindowTypeName (type) + " views. Create one first (e.g. create_sections) or pick another window."
						  : "Several " + WindowTypeName (type) + " views exist: " + list + ". Pass 'name', 'database' or 'element'.", APIERR_BADPARS);
				}
				entry = all[0];
			}
			API_WindowInfo w;
			BNZeroMemory (&w, sizeof (w));
			w.typeID = entry.type;
			w.databaseUnId = entry.id;
			OpenWindow (w, WindowTypeName (entry.type) + " '" + entry.name + "'");
			opened = WindowTypeName (entry.type) + " " + entry.name;
		}
	} else {
		Fail ("Tell open_view what to open: window ('FloorPlan' + story, '3D', 'Section'/'Layout'/... + name), element (marker GUID), "
			  "database (GUID from list_views) or navigatorItem.");
	}

	OS out ("opened", opened);
	out.Add ("window", CurrentWindowJson ());
	return out;
}


// Opens a View Map view with its stored settings (APIDo_GoToView).
GSErrCode GoToSavedView (const API_NavigatorItem& item)
{
	const GS::String guidStr (APIGuidToString (item.guid).ToCStr ().Get ());
	auto goTo = [&] () -> GSErrCode {
		return ACAPI_Automate (APIDo_GoToViewID, const_cast<char*> (guidStr.ToCStr ()), nullptr);
	};
	GSErrCode err = goTo ();
	if (err == APIERR_NEEDSUNDOSCOPE) {
		Undoable ("Go to view (Claude)", [&] () { err = goTo (); });
	}
	return err;
}


// Opening a saved view applies its layer combination, scale, structure display, renovation filter, model view options and
// pen set to the project. Snapshot of that display state, put back after an internal view refresh (see SetViewSettingsCmd);
// without it the refresh left layers hidden (verified live: the next suite could not edit walls on a hidden layer).
class DisplayStateKeeper {
public:
	DisplayStateKeeper ()
	{
		API_AttributeIndex count = 0;
		if (ACAPI_Attribute_GetNum (API_LayerID, &count) == NoError) {
			for (API_AttributeIndex i = 1; i <= count; ++i) {
				API_Attribute a;
				BNZeroMemory (&a, sizeof (a));
				a.header.typeID = API_LayerID;
				a.header.index = i;
				if (ACAPI_Attribute_Get (&a) == NoError)
					layers.Push ({ i, (short) (a.header.flags & kLayerBits), a.layer.conClassId });
			}
		}
		ACAPI_Environment (APIEnv_GetCurrLayerCombID, &layerComb, nullptr);
		ACAPI_Environment (APIEnv_GetCurrPenSetID, &penSet, nullptr);
		hasScale = ACAPI_Database (APIDb_GetDrawingScaleID, &scale, nullptr) == NoError && scale > 0.0;
		BNZeroMemory (&structure, sizeof (structure));
		hasStructure = ACAPI_Environment (APIEnv_GetStructureDisplayID, &structure, nullptr) == NoError;
		hasRenovation = ACAPI_Database (APIDb_GetActualRenovationFilterID, &renovation, nullptr) == NoError && renovation != APINULLGuid;
		BNZeroMemory (&viewOptions, sizeof (viewOptions));
		hasViewOptions = ACAPI_Environment (APIEnv_GetViewOptionsID, &viewOptions, nullptr) == NoError;
	}

	~DisplayStateKeeper ()
	{
		if (hasViewOptions)
			ACAPI_FreeGDLModelViewOptionsPtr (&viewOptions.modelViewOpt.gdlOptions);
	}

	DisplayStateKeeper (const DisplayStateKeeper&) = delete;
	DisplayStateKeeper& operator= (const DisplayStateKeeper&) = delete;

	// Call with the original window in front again. Returns what could not be put back.
	GS::Array<GS::UniString> Restore ()
	{
		GS::Array<GS::UniString> problems;
		auto note = [&] (const char* what, GSErrCode err) {
			if (err != NoError)
				problems.Push (GS::UniString (what) + " (" + ErrorName (err) + ")");
		};
		Undoable ("Restore display settings (Claude)", [&] () {
			if (layerComb > 0) {
				API_AttributeIndex i = layerComb;
				note ("layer combination", ACAPI_Environment (APIEnv_ChangeCurrLayerCombID, &i, nullptr));
			}
			for (const LayerState& st : layers) {		// custom layer states (and anything the combination did not cover)
				API_Attribute a;
				BNZeroMemory (&a, sizeof (a));
				a.header.typeID = API_LayerID;
				a.header.index = st.index;
				GS::UniString name;
				a.header.uniStringNamePtr = &name;
				if (ACAPI_Attribute_Get (&a) != NoError)
					continue;
				if ((a.header.flags & kLayerBits) == st.flags && a.layer.conClassId == st.group)
					continue;
				a.header.flags = (short) ((a.header.flags & ~kLayerBits) | st.flags);
				a.layer.conClassId = st.group;
				note ("layer states", ACAPI_Attribute_Modify (&a, nullptr));
			}
			if (hasScale) {
				double s = scale;
				note ("drawing scale", ACAPI_Database (APIDb_ChangeDrawingScaleID, &s, nullptr));
			}
			if (hasStructure) {
				API_UIStructureDisplay sd = structure;
				note ("structure display", ACAPI_Environment (APIEnv_ChangeStructureDisplayID, &sd, nullptr));
			}
			if (hasRenovation) {
				API_Guid g = renovation;
				note ("renovation filter", ACAPI_Database (APIDb_SetActualRenovationFilterID, &g, nullptr));
			}
			if (hasViewOptions) {
				API_ViewOptions vo = viewOptions;
				note ("model view options", ACAPI_Environment (APIEnv_ChangeViewOptionsID, &vo, nullptr));
			}
		});
		API_AttributeIndex penNow = 0;
		if (penSet > 0 && ACAPI_Environment (APIEnv_GetCurrPenSetID, &penNow, nullptr) == NoError && penNow != penSet)
			problems.Push ("pen set (the Archicad 26 API cannot switch it back; the view's pen set is now active)");
		return problems;
	}

private:
	static constexpr short kLayerBits = APILay_Hidden | APILay_Locked | APILay_ForceToWire;
	struct LayerState { API_AttributeIndex index; short flags; Int32 group; };
	GS::Array<LayerState>	layers;
	API_AttributeIndex		layerComb = 0;
	API_AttributeIndex		penSet = 0;
	double					scale = 0.0;
	bool					hasScale = false;
	API_UIStructureDisplay	structure;
	bool					hasStructure = false;
	API_Guid				renovation = APINULLGuid;
	bool					hasRenovation = false;
	API_ViewOptions			viewOptions;
	bool					hasViewOptions = false;
};


OS GoToViewCmd (const OS& params)
{
	const NavEntry entry = ResolveNavItem (params, "view", { API_PublicViewMap, API_MyViewMap, API_ProjectMap, API_LayoutMap });
	GS::UniString method;
	GS::UniString note;

	if (entry.item.mapId == API_PublicViewMap || entry.item.mapId == API_MyViewMap) {
		const GSErrCode err = GoToSavedView (entry.item);
		if (err == NoError) {
			method = "goToView";
		} else {
			OpenNavItem (entry.item);
			method = "openWindow";
			note = "APIDo_GoToView failed (" + ErrorName (err) + "); the view's window was opened without applying its stored view settings.";
		}
	} else {
		OpenNavItem (entry.item);
		method = "openWindow";
	}

	OS out;
	out.Add ("view", NavEntryJson (entry));
	out.Add ("method", method);
	if (!note.IsEmpty ())
		out.Add ("note", note);
	out.Add ("window", CurrentWindowJson ());
	return out;
}


API_Box ReadBox (const OS& params, const char* key)
{
	OS b = GetObject (params, key);
	API_Box box = {};
	box.xMin = GetDouble (b, "xMin");
	box.yMin = GetDouble (b, "yMin");
	box.xMax = GetDouble (b, "xMax");
	box.yMax = GetDouble (b, "yMax");
	if (box.xMax < box.xMin) std::swap (box.xMin, box.xMax);
	if (box.yMax < box.yMin) std::swap (box.yMin, box.yMax);
	if (box.xMax - box.xMin < 1e-6 || box.yMax - box.yMin < 1e-6)
		Fail ("'" + GS::UniString (key) + "' must have a non-zero width and height (xMin < xMax, yMin < yMax, meters).");
	return box;
}


void ZoomToBox (const API_Box& box)
{
	CallApi ("Zoom", [&] () {
		API_Box b = box;
		GSErrCode err = ACAPI_Automate (APIDo_ZoomID, &b, nullptr);
		if (err != NoError) {
			API_Box b2 = box;
			err = ACAPI_Database (APIDb_SetZoomID, &b2, nullptr);
		}
		return err;
	});
}


void ApplyZoomMargin (double margin)
{
	if (margin <= 0.0)
		return;
	API_Box box = {};
	if (ACAPI_Database (APIDb_GetZoomID, &box, nullptr) != NoError || box.xMax <= box.xMin)
		return;
	const double dx = (box.xMax - box.xMin) * margin;
	const double dy = (box.yMax - box.yMin) * margin;
	box.xMin -= dx; box.xMax += dx;
	box.yMin -= dy; box.yMax += dy;
	ZoomToBox (box);
}


OS ZoomCmd (const OS& params)
{
	const GS::UniString mode = GetString (params, "mode", "fit");
	const API_WindowInfo window = GetCurrentWindow ();
	const bool is3D = window.typeID == APIWind_3DModelID;
	const double margin = GetDouble (params, "margin", 0.0);
	if (margin < 0.0 || margin > 5.0)
		Fail ("'margin' must be between 0 and 5 (fraction of the zoomed area added on each side, e.g. 0.1).");

	if (EqualsIgnoreCase (mode, "fit") || EqualsIgnoreCase (mode, "extents") || EqualsIgnoreCase (mode, "reset")) {
		CallApi ("Fit in window", [] () { return ACAPI_Automate (APIDo_ZoomID, nullptr, nullptr); });
		if (!is3D) ApplyZoomMargin (margin);
	} else if (EqualsIgnoreCase (mode, "box")) {
		if (!Is2DWindow (window.typeID))
			Fail ("Zoom to box works in 2D windows (floor plan, section, layout, ...). The current window is " + WindowTypeName (window.typeID) + ".", APIERR_BADWINDOW);
		API_Box box = ReadBox (params, "box");
		if (margin > 0.0) {
			const double dx = (box.xMax - box.xMin) * margin, dy = (box.yMax - box.yMin) * margin;
			box.xMin -= dx; box.xMax += dx; box.yMin -= dy; box.yMax += dy;
		}
		ZoomToBox (box);
	} else if (EqualsIgnoreCase (mode, "elements")) {
		GS::Array<API_Guid> guids = GetGuidArray (params, "elements", true);
		if (guids.IsEmpty ())
			Fail ("'elements' must contain at least one element GUID.");
		CallApi ("Zoom to elements", [&] () { return ACAPI_Automate (APIDo_ZoomToElementsID, &guids, nullptr); });
		if (!is3D) ApplyZoomMargin (margin);
	} else if (EqualsIgnoreCase (mode, "selection")) {
		const GSErrCode err = ACAPI_Automate (APIDo_ZoomToSelectedID, nullptr, nullptr);
		if (err == APIERR_NOSEL)
			Fail ("Nothing is selected. Select elements first (set_selection) or use mode 'elements'.", err);
		if (err == APIERR_NEEDSUNDOSCOPE)
			Undoable ("Zoom to selection (Claude)", [] () { Check (ACAPI_Automate (APIDo_ZoomToSelectedID, nullptr, nullptr), "Zoom to selection"); });
		else
			Check (err, "Zoom to selection");
		if (!is3D) ApplyZoomMargin (margin);
	} else if (EqualsIgnoreCase (mode, "in") || EqualsIgnoreCase (mode, "out")) {
		const bool zoomIn = EqualsIgnoreCase (mode, "in");
		const double factor = GetDouble (params, "factor", 2.0);
		if (factor <= 1.0 || factor > 100.0)
			Fail ("'factor' must be > 1 and <= 100 (2 = twice as close / far).");
		if (is3D) {
			API_3DWindowInfo info = {};
			Check (ACAPI_Environment (APIEnv_Get3DWindowSetsID, &info, nullptr), "Cannot read the 3D window size");
			API_Rect rect = {};
			rect.left = 0; rect.top = 0; rect.right = info.hSize; rect.bottom = info.vSize;
			if (zoomIn) {
				const short insetX = (short) (info.hSize * (1.0 - 1.0 / factor) / 2.0);
				const short insetY = (short) (info.vSize * (1.0 - 1.0 / factor) / 2.0);
				rect.left += insetX; rect.right -= insetX; rect.top += insetY; rect.bottom -= insetY;
			} else {
				const short growX = (short) GS::Min (8000.0, info.hSize * (factor - 1.0) / 2.0);
				const short growY = (short) GS::Min (8000.0, info.vSize * (factor - 1.0) / 2.0);
				rect.left -= growX; rect.right += growX; rect.top -= growY; rect.bottom += growY;
			}
			CallApi ("Zoom", [&] () { API_Rect r = rect; return ACAPI_Automate (APIDo_ZoomID, nullptr, &r); });
		} else {
			API_Box box = {};
			Check (ACAPI_Database (APIDb_GetZoomID, &box, nullptr), "Cannot read the current zoom");
			double cx = (box.xMin + box.xMax) / 2.0, cy = (box.yMin + box.yMax) / 2.0;
			if (auto c = OptCoord (params, "center")) { cx = c->x; cy = c->y; }
			const double s = zoomIn ? 1.0 / factor : factor;
			const double hw = (box.xMax - box.xMin) / 2.0 * s, hh = (box.yMax - box.yMin) / 2.0 * s;
			API_Box nb = { cx - hw, cy - hh, cx + hw, cy + hh };
			ZoomToBox (nb);
		}
	} else if (EqualsIgnoreCase (mode, "previous")) {
		const Int32 steps = GetInt (params, "steps", 1);
		if (steps < 1 || steps > 50)
			Fail ("'steps' must be 1..50.");
		short s = (short) steps;
		Check (ACAPI_Database (APIDb_ReSetZoomID, &s, nullptr), "Cannot go back to the previous zoom");
	} else if (EqualsIgnoreCase (mode, "redraw")) {
		CallApi ("Redraw", [] () { return ACAPI_Automate (APIDo_RedrawID, nullptr, nullptr); });
	} else {
		Fail ("Invalid mode '" + mode + "'. Allowed: fit, box, elements, selection, in, out, previous, redraw.");
	}

	OS out ("mode", mode);
	out.Add ("window", CurrentWindowJson ());
	return out;
}


OS ShowIn3DCmd (const OS& params)
{
	GS::UniString mode = GetString (params, "mode", params.Contains ("elements") ? "elements" : "all");
	Int32 count = 0;
	if (EqualsIgnoreCase (mode, "elements")) {
		GS::Array<API_Guid> guids = GetGuidArray (params, "elements", true);
		if (guids.IsEmpty ())
			Fail ("'elements' must contain at least one element GUID.");
		GS::Array<API_Neig> neigs;
		GS::Array<GS::UniString> missing;
		for (const API_Guid& g : guids) {
			if (ElementExists (g))
				neigs.PushNew (g);
			else
				missing.Push (GuidStr (g));
		}
		if (neigs.IsEmpty ())
			Fail ("None of the elements exist in the current database. Open the floor plan (open_view) and use GUIDs from find_elements.", APIERR_BADID);
		const GSErrCode desel = ACAPI_Element_DeselectAll ();
		if (desel != NoError && desel != APIERR_NOSEL)
			Check (desel, "Cannot clear the selection");
		Check (ACAPI_Element_Select (neigs, true), "Cannot select the elements");
		count = (Int32) neigs.GetSize ();
		CallApi ("Show selection in 3D", [] () { return ACAPI_Automate (APIDo_ShowSelectionIn3DID, nullptr, nullptr); });
		OS out ("mode", GS::UniString ("elements"));
		out.Add ("shownElements", count);
		if (!missing.IsEmpty ())
			out.Add ("notFound", missing);
		out.Add ("note", GS::UniString ("The elements are now selected; the 3D window shows only them until show_in_3d {mode: 'all'}."));
		out.Add ("window", CurrentWindowJson ());
		return out;
	}
	if (EqualsIgnoreCase (mode, "selection")) {
		const GSErrCode err = ACAPI_Automate (APIDo_ShowSelectionIn3DID, nullptr, nullptr);
		if (err == APIERR_NOSEL)
			Fail ("Nothing is selected. Pass 'elements' or select elements first (set_selection).", err);
		if (err == APIERR_NEEDSUNDOSCOPE)
			Undoable ("Show selection in 3D (Claude)", [] () { Check (ACAPI_Automate (APIDo_ShowSelectionIn3DID, nullptr, nullptr), "Show selection in 3D"); });
		else
			Check (err, "Show selection in 3D");
	} else if (EqualsIgnoreCase (mode, "all")) {
		CallApi ("Show all in 3D", [] () { return ACAPI_Automate (APIDo_ShowAllIn3DID, nullptr, nullptr); });
	} else {
		Fail ("Invalid mode '" + mode + "'. Allowed: all, selection, elements.");
	}
	OS out ("mode", mode);
	out.Add ("window", CurrentWindowJson ());
	return out;
}

// --- View settings: current window --------------------------------------------------------

OS CurrentViewSettingsJson ()
{
	OS out;
	API_WindowInfo window;
	const bool hasWindow = TryGetCurrentWindow (window);
	out.Add ("target", GS::UniString ("currentWindow"));
	out.Add ("window", CurrentWindowJson ());

	if (hasWindow && Is2DWindow (window.typeID)) {
		double scale = 0.0;
		if (ACAPI_Database (APIDb_GetDrawingScaleID, &scale, nullptr) == NoError && scale > 0.0)
			out.Add ("drawingScale", scale);
	}

	API_AttributeIndex layerComb = 0;
	if (ACAPI_Environment (APIEnv_GetCurrLayerCombID, &layerComb, nullptr) == NoError && layerComb > 0)
		out.Add ("layerCombination", AttrRef (API_LayerCombID, layerComb));
	else
		out.Add ("layerCombination", OS ("custom", true, "note", GS::UniString ("The layer settings do not match a saved layer combination.")));

	API_AttributeIndex penSet = 0;
	if (ACAPI_Environment (APIEnv_GetCurrPenSetID, &penSet, nullptr) == NoError && penSet > 0)
		out.Add ("penSet", AttrRef (API_PenTableID, penSet));

	API_UIStructureDisplay sd;
	BNZeroMemory (&sd, sizeof (sd));
	if (ACAPI_Environment (APIEnv_GetStructureDisplayID, &sd, nullptr) == NoError)
		out.Add ("structureDisplay", NameOf (kStructureDisplay, sd.structureDisplay));

	API_Guid renoFilter = APINULLGuid;
	if (ACAPI_Database (APIDb_GetActualRenovationFilterID, &renoFilter, nullptr) == NoError && renoFilter != APINULLGuid)
		out.Add ("renovationFilter", RenovationFilterJson (renoFilter));

	// Model view options: the API has no "current combination" getter; report the combinations whose options match (best effort).
	API_ViewOptions vo;
	BNZeroMemory (&vo, sizeof (vo));
	if (ACAPI_Environment (APIEnv_GetViewOptionsID, &vo, nullptr) == NoError) {
		GS::Array<OS> matching;
		API_AttributeIndex count = 0;
		if (ACAPI_Attribute_GetNum (API_ModelViewOptionsID, &count) == NoError) {
			for (API_AttributeIndex i = 1; i <= count; ++i) {
				API_Attribute attr;
				BNZeroMemory (&attr, sizeof (attr));
				attr.header.typeID = API_ModelViewOptionsID;
				attr.header.index = i;
				GS::UniString name;
				attr.header.uniStringNamePtr = &name;
				if (ACAPI_Attribute_Get (&attr) != NoError)
					continue;
				if (std::memcmp (&attr.modelViewOpt.modelViewOpt, &vo.modelViewOpt, offsetof (API_ModelViewOptions, gdlOptions)) == 0)
					matching.Push (OS ("index", (Int32) i, "name", name, "guid", GuidStr (attr.header.guid)));
				ACAPI_FreeGDLModelViewOptionsPtr (&attr.modelViewOpt.modelViewOpt.gdlOptions);
			}
		}
		ACAPI_FreeGDLModelViewOptionsPtr (&vo.modelViewOpt.gdlOptions);
		out.Add ("modelViewOptions", OS ("matchingCombinations", matching, "note",
			GS::UniString ("Best effort: combinations whose (non-GDL) options equal the current ones.")));
	}

	GS::Array<OS> filters;
	for (const API_Guid& g : RenovationFilters ())
		filters.Push (RenovationFilterJson (g));
	out.Add ("availableRenovationFilters", filters);
	return out;
}


void ApplyModelViewOptionsToCurrent (API_AttributeIndex index)
{
	API_Attribute attr;
	BNZeroMemory (&attr, sizeof (attr));
	attr.header.typeID = API_ModelViewOptionsID;
	attr.header.index = index;
	Check (ACAPI_Attribute_Get (&attr), "Cannot read the model view options combination");
	auto freeAttr = MakeScopeExit ([&] () { ACAPI_FreeGDLModelViewOptionsPtr (&attr.modelViewOpt.modelViewOpt.gdlOptions); });

	API_ViewOptions vo;
	BNZeroMemory (&vo, sizeof (vo));
	Check (ACAPI_Environment (APIEnv_GetViewOptionsID, &vo, nullptr), "Cannot read the current view options");
	ACAPI_FreeGDLModelViewOptionsPtr (&vo.modelViewOpt.gdlOptions);
	vo.modelViewOpt = attr.modelViewOpt.modelViewOpt;		// gdlOptions stays owned by attr
	CallApi ("Apply model view options", [&] () {
		API_ViewOptions v = vo;
		return ACAPI_Environment (APIEnv_ChangeViewOptionsID, &v, nullptr);
	});
}


GS::Array<GS::UniString> SetCurrentViewSettings (const OS& params)
{
	static const char* const kViewOnly[] = { "penSet", "dimensionStyle", "graphicOverrides", "renderingScene", "zoom", "ignoreSavedZoom" };
	for (const char* key : kViewOnly) {
		if (params.Contains (key))
			Fail ("'" + GS::UniString (key) + "' can only be stored in a saved view: pass 'view' (a View Map item from list_views). "
				  "For the current window use zoom / set_3d_view / render_view.");
	}
	GS::Array<GS::UniString> changed;
	if (auto scale = OptDouble (params, "drawingScale")) {
		if (*scale < 1.0 || *scale > 100000.0)
			Fail ("'drawingScale' is the N of 1:N and must be 1..100000 (e.g. 100 for 1:100).");
		CallApi ("Change drawing scale", [&] () {
			double s = *scale;
			return ACAPI_Database (APIDb_ChangeDrawingScaleID, &s, nullptr);
		});
		changed.Push ("drawingScale");
	}
	if (params.Contains ("layerCombination")) {
		const API_AttributeIndex idx = GetAttr (API_LayerCombID, params, "layerCombination");
		CallApi ("Apply layer combination", [&] () {
			API_AttributeIndex i = idx;
			return ACAPI_Environment (APIEnv_ChangeCurrLayerCombID, &i, nullptr);
		});
		changed.Push ("layerCombination");
	}
	if (params.Contains ("modelViewOptions")) {
		ApplyModelViewOptionsToCurrent (GetAttr (API_ModelViewOptionsID, params, "modelViewOptions"));
		changed.Push ("modelViewOptions");
	}
	if (params.Contains ("structureDisplay")) {
		const API_StructureDisplay value = (API_StructureDisplay) ParseNamed (kStructureDisplay, params, "structureDisplay");
		API_UIStructureDisplay sd;
		BNZeroMemory (&sd, sizeof (sd));
		Check (ACAPI_Environment (APIEnv_GetStructureDisplayID, &sd, nullptr), "Cannot read the structure display");
		sd.structureDisplay = value;
		CallApi ("Change structure display", [&] () {
			API_UIStructureDisplay s = sd;
			return ACAPI_Environment (APIEnv_ChangeStructureDisplayID, &s, nullptr);
		});
		changed.Push ("structureDisplay");
	}
	if (params.Contains ("renovationFilter")) {
		const API_Guid g = ResolveRenovationFilter (params, "renovationFilter");
		CallApi ("Change renovation filter", [&] () {
			API_Guid gg = g;
			return ACAPI_Database (APIDb_SetActualRenovationFilterID, &gg, nullptr);
		});
		changed.Push ("renovationFilter");
	}
	if (params.Contains ("style3D")) {
		const GS::UniString style = PickName (Style3DNames (), GetString (params, "style3D"), "3D style");
		CallApi ("Change 3D style", [&] () {
			GS::UniString s = style;
			return ACAPI_Environment (APIEnv_SetCurrent3DStyleID, &s, nullptr);
		});
		changed.Push ("style3D");
	}
	return changed;
}

// --- View settings: saved views (Navigator) ---------------------------------------------------

struct NavViewHolder {
	API_NavigatorView view;
	NavViewHolder () { BNZeroMemory (&view, sizeof (view)); }
	~NavViewHolder () { Release (); }
	NavViewHolder (const NavViewHolder&) = delete;
	NavViewHolder& operator= (const NavViewHolder&) = delete;

	void ReleaseModelViewOpt ()
	{
		if (view.modelViewOpt != nullptr) {
			ACAPI_FreeGDLModelViewOptionsPtr (&view.modelViewOpt->gdlOptions);
			BMKillPtr (reinterpret_cast<GSPtr*> (&view.modelViewOpt));
		}
	}
	void ReleaseLayerStats ()	{ BMKillHandle (reinterpret_cast<GSHandle*> (&view.layerStats)); }
	void ReleasePens ()			{ BMKillPtr (reinterpret_cast<GSPtr*> (&view.pens)); }
	void ReleaseDimPrefs ()		{ BMKillPtr (reinterpret_cast<GSPtr*> (&view.dimPrefs)); }
	void Release ()
	{
		ReleaseModelViewOpt ();
		ReleaseLayerStats ();
		ReleasePens ();
		ReleaseDimPrefs ();
	}
};


void LoadNavView (const API_NavigatorItem& item, NavViewHolder& holder)
{
	API_NavigatorItem query = item;
	const GSErrCode err = ACAPI_Navigator (APINavigator_GetNavigatorViewID, &query, &holder.view);
	if (err != NoError)
		Fail ("'" + NavDisplayName (item) + "' has no view settings (" + ErrorName (err) + "). Pass a View Map item (list_views include viewMap).", err);
}


OS NavViewSettingsJson (const NavEntry& entry)
{
	NavViewHolder holder;
	LoadNavView (entry.item, holder);
	const API_NavigatorView& v = holder.view;

	OS out;
	out.Add ("target", GS::UniString ("savedView"));
	out.Add ("view", NavEntryJson (entry));

	auto stored = [] (bool isSaved, OS value) {
		value.Add ("storedInView", isSaved);
		return value;
	};
	OS layer = v.layerCombination[0] != 0 ? AttrRefFromRawName (API_LayerCombID, v.layerCombination, API_AttrNameLen)
										 : OS ("custom", v.layerStats != nullptr);
	out.Add ("layerCombination", stored (v.saveLaySet, layer));
	OS mvo = v.modelViewOptName[0] != 0 ? AttrRefFromRawName (API_ModelViewOptionsID, v.modelViewOptName, API_LongNameLen)
									   : OS ("custom", v.modelViewOpt != nullptr);
	out.Add ("modelViewOptions", stored (v.saveDispOpt, mvo));
	OS pen = v.penSetName[0] != 0 ? AttrRefFromRawName (API_PenTableID, v.penSetName, API_AttrNameLen) : OS ("custom", v.pens != nullptr);
	out.Add ("penSet", stored (v.savePenSet, pen));
	OS dim = v.dimName[0] != 0 ? AttrRefFromRawName (API_DimStandID, v.dimName, API_LongNameLen) : OS ("custom", v.dimPrefs != nullptr);
	out.Add ("dimensionStyle", stored (v.saveDim, dim));
	out.Add ("drawingScale", stored (v.saveDScale, OS ("value", (Int32) v.drawingScale)));
	out.Add ("structureDisplay", stored (v.saveStructureDisplay, OS ("value", NameOf (kStructureDisplay, v.structureDisplay))));
	OS zoom = BoxObj (v.zoom);
	zoom.Add ("ignoreSavedZoom", v.ignoreSavedZoom);
	out.Add ("zoom", stored (v.saveZoom, zoom));
	if (v.renovationFilterGuid != APINULLGuid)
		out.Add ("renovationFilter", RenovationFilterJson (v.renovationFilterGuid));
	out.Add ("graphicOverrides", UStr (v.overrideCombination));
	out.Add ("style3D", UStr (v.d3styleName));
	out.Add ("renderingScene", UStr (v.renderingSceneName));
	out.Add ("usePhotoRendering", v.usePhotoRendering);
	return out;
}


GS::Array<GS::UniString> SetNavViewSettings (const NavEntry& entry, const OS& params)
{
	NavViewHolder holder;
	LoadNavView (entry.item, holder);
	API_NavigatorView& v = holder.view;
	GS::Array<GS::UniString> changed;

	if (params.Contains ("layerCombination")) {
		ResolveAttrToRaw (API_LayerCombID, params, "layerCombination", v.layerCombination, API_AttrNameLen);
		holder.ReleaseLayerStats ();
		v.saveLaySet = true;
		changed.Push ("layerCombination");
	}
	if (params.Contains ("modelViewOptions")) {
		ResolveAttrToRaw (API_ModelViewOptionsID, params, "modelViewOptions", v.modelViewOptName, API_LongNameLen);
		holder.ReleaseModelViewOpt ();
		v.saveDispOpt = true;
		changed.Push ("modelViewOptions");
	}
	if (params.Contains ("penSet")) {
		ResolveAttrToRaw (API_PenTableID, params, "penSet", v.penSetName, API_AttrNameLen);
		holder.ReleasePens ();
		v.savePenSet = true;
		changed.Push ("penSet");
	}
	if (params.Contains ("dimensionStyle")) {
		ResolveAttrToRaw (API_DimStandID, params, "dimensionStyle", v.dimName, API_LongNameLen);
		holder.ReleaseDimPrefs ();
		v.saveDim = true;
		changed.Push ("dimensionStyle");
	}
	if (auto scale = OptDouble (params, "drawingScale")) {
		if (*scale < 1.0 || *scale > 100000.0)
			Fail ("'drawingScale' is the N of 1:N and must be 1..100000 (e.g. 100 for 1:100).");
		v.drawingScale = (Int32) std::lround (*scale);
		v.saveDScale = true;
		changed.Push ("drawingScale");
	}
	if (params.Contains ("structureDisplay")) {
		v.structureDisplay = (API_StructureDisplay) ParseNamed (kStructureDisplay, params, "structureDisplay");
		v.saveStructureDisplay = true;
		changed.Push ("structureDisplay");
	}
	if (params.Contains ("zoom")) {
		v.zoom = ReadBox (params, "zoom");
		v.saveZoom = true;
		changed.Push ("zoom");
	}
	if (auto ignore = OptBool (params, "ignoreSavedZoom")) {
		v.ignoreSavedZoom = *ignore;
		changed.Push ("ignoreSavedZoom");
	}
	if (params.Contains ("renovationFilter")) {
		v.renovationFilterGuid = ResolveRenovationFilter (params, "renovationFilter");
		changed.Push ("renovationFilter");
	}
	if (params.Contains ("graphicOverrides")) {
		SetUStr (v.overrideCombination, PickName (OverrideCombinationNames (), GetString (params, "graphicOverrides"), "Graphic override combination"));
		changed.Push ("graphicOverrides");
	}
	if (params.Contains ("style3D")) {
		SetUStr (v.d3styleName, PickName (Style3DNames (), GetString (params, "style3D"), "3D style"));
		changed.Push ("style3D");
	}
	if (params.Contains ("renderingScene")) {
		SetUStr (v.renderingSceneName, PickName (RenderingSceneNames (), GetString (params, "renderingScene"), "Rendering scene"));
		changed.Push ("renderingScene");
	}
	if (changed.IsEmpty ())
		Fail ("Nothing to change. Give at least one of layerCombination, modelViewOptions, penSet, dimensionStyle, drawingScale, "
			  "structureDisplay, zoom, ignoreSavedZoom, renovationFilter, graphicOverrides, style3D, renderingScene.");

	CallApi ("Change view settings of '" + NavDisplayName (entry.item) + "'", [&] () {
		API_NavigatorItem item = entry.item;
		return ACAPI_Navigator (APINavigator_ChangeNavigatorViewID, &item, &v);
	});
	return changed;
}


OS GetViewSettingsCmd (const OS& params)
{
	if (params.Contains ("view"))
		return NavViewSettingsJson (ResolveNavItem (params, "view", { API_PublicViewMap, API_MyViewMap }));
	return CurrentViewSettingsJson ();
}


OS SetViewSettingsCmd (const OS& params)
{
	if (params.Contains ("view")) {
		const NavEntry entry = ResolveNavItem (params, "view", { API_PublicViewMap, API_MyViewMap });
		GS::Array<GS::UniString> changed = SetNavViewSettings (entry, params);
		OS out ("changed", changed);
		// Drawings are built from a view's settings as they were when it was last opened: a stored change (scale, zoom...)
		// reaches newly placed drawings only after the view has been opened once (verified live: a view set to 1:200 kept
		// placing at 1:500, and a new zoom left the crop frame and the content apart, until the view was opened).
		{
			doc::WindowRestorer restorer;		// the front window
			restorer.Arm (true);
			const short storyBefore = CurrentStoryIndex ();
			// Layer states, scale and display options belong to each window type (verified live: with a layout in front the
			// layout kept its layers while the floor plan behind it took the view's). Open the view's window without its
			// settings first, so the snapshot and the restore act on the window that GoToView changes.
			try {
				OpenNavItem (entry.item);
			} catch (...) {
			}
			GSErrCode err = NoError;
			GS::Array<GS::UniString> notRestored;
			{
				DisplayStateKeeper display;
				err = GoToSavedView (entry.item);
				notRestored = display.Restore ();
			}
			if (CurrentStoryIndex () != storyBefore) {
				try {
					doc::GoToStory (storyBefore);
				} catch (...) {
				}
			}
			restorer.Finish ();
			if (!notRestored.IsEmpty ()) {
				GS::UniString list;
				for (const GS::UniString& p : notRestored)
					list += (list.IsEmpty () ? "" : "; ") + p;
				out.Add ("displayNote", GS::UniString ("Opening the view changed display settings that could not all be put back: " + list + "."));
			}
			if (err == NoError)
				out.Add ("refreshed", "The view was opened once (so drawings placed from now on use the new settings); the previous window "
									  "and its layers / scale / display options were put back.");
			else
				out.Add ("warning", GS::UniString ("Could not open the view (" + ErrorName (err) + "): open it once with go_to_view before "
													 "placing drawings from it, otherwise they may still use its old scale/zoom."));
		}
		out.Add ("settings", NavViewSettingsJson (entry));
		return out;
	}
	GS::Array<GS::UniString> changed = SetCurrentViewSettings (params);
	if (changed.IsEmpty ())
		Fail ("Nothing to change. For the current window give drawingScale, layerCombination, modelViewOptions, structureDisplay, "
			  "renovationFilter or style3D (or pass 'view' to change a saved view).");
	OS out ("changed", changed);
	out.Add ("settings", CurrentViewSettingsJson ());
	return out;
}

} // namespace


void RegisterViewCommands ()
{
	RegisterCommand ("GetCurrentWindow",
		"Returns the active window: {type (FloorPlan|3DModel|Section|Elevation|InteriorElevation|Detail|Worksheet|Layout|MasterLayout|DocumentFrom3D|...), "
		"database (GUID), name, reference, title, linkedElement, story (floor plan), drawingScale (1:N), zoom box (2D, m), projection (3D)}.",
		GetCurrentWindowCmd);

	RegisterCommand ("ListViews",
		"Lists what can be opened: {current, stories, viewpoints (sections, elevations, interior elevations, details, worksheets, 3D documents: "
		"{type, database, name, reference, title}), layouts (layouts and master layouts), viewMap (saved views: {guid, name, id, fullName, itemType, "
		"folder, windowType, database}), projectMap (optional)}. Input: {include?: [stories|viewpoints|layouts|viewMap|projectMap|all], "
		"types?: [window types], nameContains?, limit?}.",
		ListViewsCmd);

	RegisterCommand ("OpenView",
		"Opens (brings to front) a window. Input (one of): {window: 'FloorPlan', story?} | {window: '3D', projection?: perspective|axonometric} | "
		"{window: 'Section'|'Elevation'|'InteriorElevation'|'Detail'|'Worksheet'|'Layout'|'MasterLayout'|'DocumentFrom3D', name?} | "
		"{element: marker GUID, segmentIndex?} | {database: GUID, window?} | {name} | {navigatorItem: GUID or name}. Output: {opened, window}.",
		OpenViewCmd);

	RegisterCommand ("GoToView",
		"Opens a saved view of the View Map with all its stored settings (layer combination, scale, model view options, zoom...), like "
		"double-clicking it in the Navigator. Project Map / Layout Book items are opened without settings. Input: {view: GUID | name | {guid} | {name}}.",
		GoToViewCmd);

	RegisterCommand ("Zoom",
		"Zooms the active window. Input: {mode: 'fit' (fit in window) | 'box' (2D, box {xMin,yMin,xMax,yMax} m) | 'elements' (elements: [GUID]) | "
		"'selection' | 'in' | 'out' (factor?, center? for 2D) | 'previous' (steps?) | 'redraw', margin?: fraction added around 2D zooms}. Output: {mode, window}.",
		ZoomCmd);

	RegisterCommand ("ShowIn3D",
		"Shows elements in the 3D window. Input: {mode?: 'all' (Show All in 3D) | 'selection' | 'elements', elements?: [GUID] (selects them and shows only them)}.",
		ShowIn3DCmd);

	RegisterCommand ("GetViewSettings",
		"Returns view settings. Without 'view': the current window's drawing scale, layer combination, pen set, structure display, renovation filter "
		"and matching model view options. With view (View Map item GUID or name): the settings stored in that saved view.",
		GetViewSettingsCmd);

	RegisterCommand ("SetViewSettings",
		"Changes view settings. Current window: {drawingScale?, layerCombination?, modelViewOptions?, structureDisplay?, renovationFilter?, style3D?}. "
		"Saved view ({view: GUID|name}): additionally penSet, dimensionStyle, graphicOverrides, zoom {xMin..}, ignoreSavedZoom, renderingScene. Output: {changed, settings}.",
		SetViewSettingsCmd);

	RegisterView3DCommands ();
	RegisterViewMarkerAdapters ();
}

} // namespace cc
