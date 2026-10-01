// *****************************************************************************
// ObjectsLibrary — Object / Lamp element adapters and GDL parameter commands.
//
// Object / Lamp create & modify fields (lengths in meters, angles in degrees):
//   libraryPart* (name | index | {guid} | {name} | {index}; * required on create unless the tool
//     default should be used), position* {x,y}, elevation (from the home story), angle, mirrored,
//   sizeA (GDL A = X size), sizeB (GDL B = Y size), height (GDL ZZYZX, when the part has it),
//   params {gdlName: value},
//   pen, useObjectPens, lineType, useObjectLineTypes, overrideSurface (surface | false),
//   sectionFill, sectionFillPen, sectionBackgroundPen, sectionContourPen, useObjectSectionAttributes,
//   showOnStories ("HomeOnly" | "AllRelevant" | "AllStories" | {home, allAbove, allBelow, above, below}),
//   lamps only: lightOn, lightColor {red, green, blue} (0..1), lightIntensity,
//   modify only: keepParameters (default true), keepSize (default false) when libraryPart changes,
//   + common: layer, storyIndex, renovationStatus, elementId
//
// Commands: GetGdlParameters, SetGdlParameters, ChangeLibraryPart,
//   SearchLibraryParts, GetLibraryPartSubtypes, GetLibraryPartDetails, GetLibraryPartScripts, CreateLibraryPart,
//   GetLibraries, AddLibraries, RemoveLibraries, ReloadLibraries.
// Everything lives in this one file on purpose: the add-on build globs sources only at CMake
// configure time, so extra .cpp files would need a re-configure of every build directory.
// Sections: 1. shared helpers + ParamEditor  2. library parts  3. libraries  4. objects/lamps + GDL parameters.
// *****************************************************************************

#include "Commands/Commands.hpp"
#include "Commands/ObjectsLibraryCommon.hpp"
#include "Core/Command.hpp"
#include "Core/Elements.hpp"
#include "Core/Enums.hpp"
#include "Core/LibParts.hpp"
#include "Core/Types.hpp"

#include "BuiltInLibrary.hpp"
#include "FileSystem.hpp"
#include "GSUnID.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>


// =============================================================================
// 1. Shared helpers and the GDL parameter editor
// =============================================================================

namespace cc {
namespace objlib {

// --- Generic helpers -----------------------------------------------------------------

GS::UniString LocationPath (const IO::Location& loc)
{
	GS::UniString path;
	if (loc.ToPath (&path) != NoError || path.IsEmpty ())
		path = loc.ToDisplayText ();
	return path;
}


void RunWithUndoFallback (const GS::UniString& undoName, const std::function<void ()>& fn)
{
	try {
		fn ();
	} catch (const Error& e) {
		if (e.code != APIERR_NEEDSUNDOSCOPE)
			throw;
		Undoable (undoName, fn);
	}
}


namespace {
	class ListKindCollector : public OS::Processor {
	public:
		UInt32 numbers = 0;
		UInt32 strings = 0;
		UInt32 objects = 0;
		UInt32 bools = 0;
		void BoolFound (const GS::String&, bool) override				{ ++bools; }
		void IntFound (const GS::String&, Int64) override				{ ++numbers; }
		void UIntFound (const GS::String&, UInt64) override				{ ++numbers; }
		void RealFound (const GS::String&, double) override				{ ++numbers; }
		void StringFound (const GS::String&, const GS::UniString&) override { ++strings; }
		bool ObjectFound (const GS::String&, const OS&) override		{ ++objects; return false; }
	};

	ListKindCollector ListKinds (const OS& os, const char* key)
	{
		ListKindCollector c;
		os.Enumerate (key, c);
		return c;
	}
}


ListKindCounts CountListKinds (const OS& os, const char* key)
{
	const ListKindCollector c = ListKinds (os, key);
	ListKindCounts out;
	out.numbers = c.numbers;
	out.strings = c.strings;
	out.objects = c.objects;
	out.bools = c.bools;
	return out;
}


void CopyField (const OS& src, const GS::String& srcKey, OS& dst, const GS::String& dstKey)
{
	const char* key = srcKey.ToCStr ();
	switch (src.GetType (srcKey)) {
		case OS::Bool:		{ bool v = false; src.Get (srcKey, v); dst.Add (dstKey, v); break; }
		case OS::Int:
		case OS::UInt:
		case OS::Real:		dst.Add (dstKey, GetDouble (src, key)); break;
		case OS::String:	{ GS::UniString v; src.Get (srcKey, v); dst.Add (dstKey, v); break; }
		case OS::Object:	{ OS v; src.Get (srcKey, v); dst.Add (dstKey, v); break; }
		case OS::List: {
			ListKindCollector kinds = ListKinds (src, key);
			if (kinds.objects > 0 && kinds.numbers == 0 && kinds.strings == 0 && kinds.bools == 0) {
				GS::Array<OS> v; src.Get (srcKey, v); dst.Add (dstKey, v);
			} else if (kinds.strings > 0 && kinds.numbers == 0 && kinds.objects == 0) {
				dst.Add (dstKey, GetStringArray (src, key));
			} else if (kinds.objects == 0 && kinds.strings == 0) {
				dst.Add (dstKey, GetNumberArray (src, key));
			} else {
				Fail ("Field '" + GS::UniString (key, CC_UTF8) + "' mixes value types in one array.");
			}
			break;
		}
	}
}


GS::UniString FourCC (GSType type)
{
	const char s[5] = { (char) ((type >> 24) & 0xFF), (char) ((type >> 16) & 0xFF), (char) ((type >> 8) & 0xFF), (char) (type & 0xFF), 0 };
	return GS::UniString (s);
}


GS::UniString JoinNames (const GS::Array<GS::UniString>& names, const char* sep)
{
	GS::UniString out;
	for (UIndex i = 0; i < names.GetSize (); ++i) {
		if (i > 0)
			out += sep;
		out += names[i];
	}
	return out;
}


bool ContainsNoCase (const GS::UniString& haystack, const GS::UniString& needle)
{
	// NOTE: GS::UniString::Contains (str, from, range) has NO case-comparison parameter: passing
	// GS::UniString::CaseInsensitive there means from = 1, which reads out of bounds on empty strings.
	if (needle.IsEmpty ())
		return true;
	if (haystack.IsEmpty ())
		return false;
	return haystack.ToLowerCase ().Contains (needle.ToLowerCase ());
}

// --- Library parts ---------------------------------------------------------------------

GSErrCode FetchLibPart (Int32 index, API_LibPart& lp, GS::UniString* path, IO::Location* loc)
{
	BNZeroMemory (&lp, sizeof (lp));
	lp.index = index;
	GSErrCode err = ACAPI_LibPart_Get (&lp);
	if (lp.location != nullptr) {
		if (path != nullptr)
			*path = LocationPath (*lp.location);
		if (loc != nullptr)
			*loc = *lp.location;
		delete lp.location;
		lp.location = nullptr;
	}
	return err;
}


GS::UniString MainGuidOf (const char* unId)
{
	GS::UniString s (unId);
	s.Trim ();
	const UIndex close = s.FindFirst ('}');
	if (close != MaxUIndex)
		s = s.GetSubstring (0, close + 1);
	s.SetToUpperCase ();
	return s;
}


namespace {
	const NamedValue kLibTypes[] = {
		{ "Object",			APILib_ObjectID },
		{ "Door",			APILib_DoorID },
		{ "Window",			APILib_WindowID },
		{ "Lamp",			APILib_LampID },
		{ "Zone",			APILib_RoomID },
		{ "Label",			APILib_LabelID },
		{ "Skylight",		APILib_SkylightID },
		{ "Macro",			APILib_MacroID },
		{ "Picture",		APILib_PictID },
		{ "PlanSign",		APILib_PlanSignID },
		{ "ListScheme",		APILib_ListSchemeID },
		{ "Property",		APILib_PropertyID },
		{ "OpeningSymbol",	APILib_OpeningSymbolID },
		{ "Spec",			APILib_SpecID },
		// aliases
		{ "Room",			APILib_RoomID },
		{ "Light",			APILib_LampID },
		{ "ZoneStamp",		APILib_RoomID },
	};
}


std::optional<API_LibTypeID> ParseLibType (const GS::UniString& name)
{
	for (const NamedValue& nv : kLibTypes) {
		if (EqualsIgnoreCase (name, nv.name))
			return (API_LibTypeID) nv.value;
	}
	return std::nullopt;
}


GS::UniString AllowedLibTypeNames ()
{
	return "Object, Door, Window, Lamp, Zone, Label, Skylight, Macro, Picture, PlanSign, ListScheme, Property, OpeningSymbol, Spec";
}


// --- Effective library part type ---------------------------------------------------------
// Archicad 26 reports API_LibPart::typeID = APILib_ObjectID for (almost) every GDL part, including
// windows, doors, lamps, labels and zone stamps. The real kind comes from the subtype ancestry:
// a part is a Lamp when it descends from the built-in "Light" subtype, a Door from "Door (Wall)" ...

namespace {
	struct LibTypeRoot {
		BL::BuiltInLibPartID	id;
		API_LibTypeID			type;
	};

	// Specific roots before generic ones (a door is also a wall opening).
	const LibTypeRoot kLibTypeRoots[] = {
		{ BL::BuiltInLibPartID::DoorWallLibPartID,			APILib_DoorID },
		{ BL::BuiltInLibPartID::WindowWallLibPartID,		APILib_WindowID },
		{ BL::BuiltInLibPartID::CornerWindowLibPartID,		APILib_WindowID },
		{ BL::BuiltInLibPartID::WallEndLibPartID,			APILib_WindowID },
		{ BL::BuiltInLibPartID::SkylightLibPartID,			APILib_SkylightID },
		{ BL::BuiltInLibPartID::LightLibPartID,				APILib_LampID },
		{ BL::BuiltInLibPartID::LabelLibPartID,				APILib_LabelID },
		{ BL::BuiltInLibPartID::ZoneStampLibPartID,			APILib_RoomID },
		{ BL::BuiltInLibPartID::OpeningSymbolLibPartID,		APILib_OpeningSymbolID },
		{ BL::BuiltInLibPartID::WallOpeningLibPartID,		APILib_WindowID },
		{ BL::BuiltInLibPartID::RoofOpeningLibPartID,		APILib_SkylightID },
	};

	struct ResolvedRoot {
		GS::UniString	unId;		// "{MAIN}-{00000000-...}"
		GS::UniString	mainGuid;	// "{MAIN}" uppercase
		API_LibTypeID	type;
	};

	const GS::Array<ResolvedRoot>& ResolvedLibTypeRoots ()
	{
		static GS::Array<ResolvedRoot> roots;
		static bool resolved = false;
		if (!resolved) {
			const BL::BuiltInLibraryMainGuidContainer& container = BL::BuiltInLibraryMainGuidContainer::GetInstance ();
			if (!container.IsXMLLoaded ())
				return roots;		// try again next time
			resolved = true;
			for (const LibTypeRoot& r : kLibTypeRoots) {
				const GS::UniString unId = container.GetUnIDWithNullRevGuid (r.id).ToUniString ();
				const GS::UniString mainGuid = MainGuidOf (ToStr (unId).ToCStr ());
				if (mainGuid.IsEmpty () || mainGuid == "{00000000-0000-0000-0000-000000000000}")
					continue;
				roots.Push ({ unId, mainGuid, r.type });
			}
		}
		return roots;
	}

	bool IsSubtypeByMainId (const GS::UniString& ownUnId, const GS::UniString& ancestorUnId)
	{
		char successor[256] = {};
		char predecessor[256] = {};
		CHTruncate (ToStr (ownUnId).ToCStr (), successor, sizeof (successor));
		CHTruncate (ToStr (ancestorUnId).ToCStr (), predecessor, sizeof (predecessor));
		return successor[0] != 0 && ACAPI_Goodies (APIAny_CheckLibPartSubtypeOfbyMainID, successor, predecessor) == NoError;
	}

	// Ancestry check through the API (works when some ancestors are not loaded as parts).
	API_LibTypeID EffectiveLibTypeOf (const GS::UniString& ownUnId, API_LibTypeID reported)
	{
		if (reported != APILib_ObjectID || ownUnId.IsEmpty ())
			return reported;
		for (const ResolvedRoot& r : ResolvedLibTypeRoots ()) {
			if (IsSubtypeByMainId (ownUnId, r.unId))
				return r.type;
		}
		return reported;
	}

	// Bulk version over a scan: walks the parent chain in memory (nearest root wins).
	void ResolveEffectiveTypes (std::vector<LibPartScanItem>& items)
	{
		const GS::Array<ResolvedRoot>& roots = ResolvedLibTypeRoots ();
		if (roots.IsEmpty ())
			return;
		GS::HashTable<GS::UniString, API_LibTypeID> rootType;
		for (const ResolvedRoot& r : roots) {
			if (!rootType.ContainsKey (r.mainGuid))
				rootType.Put (r.mainGuid, r.type);
		}
		GS::HashTable<GS::UniString, GS::UniString> parentOf;
		for (const LibPartScanItem& it : items) {
			if (!it.mainGuid.IsEmpty () && !parentOf.ContainsKey (it.mainGuid))
				parentOf.Put (it.mainGuid, it.parentMainGuid);
		}
		GS::HashTable<GS::UniString, Int32> cache;		// mainGuid -> API_LibTypeID, -1 = unresolved (broken chain)
		for (LibPartScanItem& it : items) {
			if (it.typeID != APILib_ObjectID || it.mainGuid.IsEmpty ())
				continue;
			if (const Int32* c = cache.GetPtr (it.mainGuid)) {
				if (*c >= 0)
					it.typeID = (API_LibTypeID) *c;
				else
					it.typeID = EffectiveLibTypeOf (it.ownUnId, it.typeID);
				continue;
			}
			Int32 found = (Int32) APILib_ObjectID;
			bool broken = false;
			GS::UniString current = it.mainGuid;
			for (int guard = 0; guard < 40 && !current.IsEmpty (); ++guard) {
				if (const API_LibTypeID* t = rootType.GetPtr (current)) {
					found = (Int32) *t;
					break;
				}
				const GS::UniString* parent = parentOf.GetPtr (current);
				if (parent == nullptr) {
					broken = true;		// ancestor not loaded as a library part
					break;
				}
				current = *parent;
			}
			if (broken) {
				cache.Put (it.mainGuid, -1);
				it.typeID = EffectiveLibTypeOf (it.ownUnId, it.typeID);
			} else {
				cache.Put (it.mainGuid, found);
				it.typeID = (API_LibTypeID) found;
			}
		}
	}
}


API_LibTypeID EffectiveLibType (const API_LibPart& lp)
{
	return EffectiveLibTypeOf (GS::UniString (lp.ownUnID), lp.typeID);
}


std::optional<API_LibTypeID> LibTypeForElemType (API_ElemTypeID typeID)
{
	switch (typeID) {
		case API_ObjectID:		return APILib_ObjectID;
		case API_LampID:		return APILib_LampID;
		case API_WindowID:		return APILib_WindowID;
		case API_DoorID:		return APILib_DoorID;
		case API_SkylightID:	return APILib_SkylightID;
		case API_ZoneID:		return APILib_RoomID;
		case API_LabelID:		return APILib_LabelID;
		default:				return std::nullopt;
	}
}


API_ElemTypeID ElemTypeForLibType (API_LibTypeID libType)
{
	switch (libType) {
		case APILib_LampID:		return API_LampID;
		case APILib_WindowID:	return API_WindowID;
		case APILib_DoorID:		return API_DoorID;
		case APILib_SkylightID:	return API_SkylightID;
		case APILib_RoomID:		return API_ZoneID;
		case APILib_LabelID:	return API_LabelID;
		default:				return API_ObjectID;
	}
}


static GS::UniString ToolHint (API_LibTypeID libType)
{
	switch (libType) {
		case APILib_ObjectID:	return "Place it with create_objects.";
		case APILib_LampID:		return "Place it with create_lamps.";
		case APILib_DoorID:		return "Place it with the door creation tool (element type 'Door', inside a wall).";
		case APILib_WindowID:	return "Place it with the window creation tool (element type 'Window', inside a wall).";
		case APILib_SkylightID:	return "Place it with the skylight creation tool (element type 'Skylight', in a roof).";
		case APILib_RoomID:		return "It is a zone stamp: use it as the zone's library part.";
		case APILib_LabelID:	return "It is a label: use it as a symbol label's library part.";
		case APILib_MacroID:	return "Macros cannot be placed; they are CALLed from other GDL scripts.";
		default:				return "This kind of library part cannot be placed as an element.";
	}
}


API_LibPart FindLibPartForElem (const OS& os, const char* key, API_ElemTypeID elemType)
{
	const std::optional<API_LibTypeID> libType = LibTypeForElemType (elemType);
	// No type filter in the Core lookup: AC26 reports typeID Object for nearly every part, the
	// effective type (from the ancestry) is checked here instead.
	API_LibPart lp = FindLibPart (os, key);
	if (!libType.has_value ())
		return lp;
	const API_LibTypeID actual = EffectiveLibType (lp);
	if (actual != *libType) {
		Fail ("Library part '" + GS::UniString (lp.docu_UName) + "' is a " + LibTypeName (actual) + ", but a " +
			  ElemTypeName (elemType) + " needs a " + LibTypeName (*libType) + " library part. " + ToolHint (actual) +
			  " To find " + LibTypeName (*libType) + " parts use search_library_parts with type '" + LibTypeName (*libType) + "'.",
			  APIERR_NOTSUBTYPEOF);
	}
	if (!lp.isPlaceable && actual != APILib_RoomID && actual != APILib_LabelID)
		Fail ("Library part '" + GS::UniString (lp.docu_UName) + "' is not placeable (it is a macro or a subtype template). "
			  "Use search_library_parts (placeableOnly: true) to find a placeable one.", APIERR_BADPARS);
	return lp;
}


OS LibPartRefJson (Int32 libInd)
{
	OS out ("index", libInd);
	if (libInd <= 0) {
		out.Add ("missing", true);
		return out;
	}
	API_LibPart lp;
	const GSErrCode err = FetchLibPart (libInd, lp);
	if (err == NoError || err == APIERR_MISSINGDEF) {
		out.Add ("name", GS::UniString (lp.docu_UName));
		out.Add ("guid", GS::UniString (lp.ownUnID));
		out.Add ("fileName", GS::UniString (lp.file_UName));
		out.Add ("type", LibTypeName (EffectiveLibType (lp)));
		if (err == APIERR_MISSINGDEF || lp.missingDef)
			out.Add ("missing", true);
	} else {
		out.Add ("missing", true);
	}
	return out;
}


GS::UniString ElementLibPartUnId (const API_Elem_Head& head)
{
	char unId[256] = {};
	API_Elem_Head h = head;
	if (ACAPI_Goodies (APIAny_GetElemLibPartUnIdID, &h, unId) != NoError)
		return GS::UniString ();
	unId[sizeof (unId) - 1] = 0;
	return GS::UniString (unId);
}


Int32 ElementLibInd (const API_Element& element)
{
	switch (element.header.type.typeID) {
		case API_ObjectID:
		case API_LampID:		return element.object.libInd;
		case API_WindowID:		return element.window.openingBase.libInd;
		case API_DoorID:		return element.door.openingBase.libInd;
		case API_SkylightID:	return element.skylight.openingBase.libInd;
		case API_ZoneID:		return element.zone.libInd;
		case API_LabelID:		return element.label.labelClass == APILblClass_Symbol ? element.label.u.symbol.libInd : 0;
		default:				break;
	}
	const GS::UniString unId = ElementLibPartUnId (element.header);
	if (unId.IsEmpty ())
		return 0;
	API_LibPart lp;
	BNZeroMemory (&lp, sizeof (lp));
	CHTruncate (unId.ToCStr ().Get (), lp.ownUnID, sizeof (lp.ownUnID));
	const GSErrCode err = ACAPI_LibPart_Search (&lp, false);
	if (lp.location != nullptr) {
		delete lp.location;
		lp.location = nullptr;
	}
	return err == NoError ? lp.index : 0;
}


std::vector<LibPartScanItem> ScanAllLibParts ()
{
	std::vector<LibPartScanItem> out;
	Int32 count = 0;
	if (ACAPI_LibPart_GetNum (&count) != NoError || count <= 0)
		return out;
	out.reserve ((size_t) count);
	for (Int32 i = 1; i <= count; ++i) {
		API_LibPart lp;
		const GSErrCode err = FetchLibPart (i, lp);
		if (err != NoError && err != APIERR_MISSINGDEF)
			continue;
		LibPartScanItem item;
		item.index = i;
		item.typeID = lp.typeID;
		item.name = GS::UniString (lp.docu_UName);
		item.fileName = GS::UniString (lp.file_UName);
		item.ownUnId = GS::UniString (lp.ownUnID);
		item.parentUnId = GS::UniString (lp.parentUnID);
		item.mainGuid = MainGuidOf (lp.ownUnID);
		item.parentMainGuid = MainGuidOf (lp.parentUnID);
		item.isTemplate = lp.isTemplate;
		item.isPlaceable = lp.isPlaceable;
		item.missingDef = lp.missingDef || err == APIERR_MISSINGDEF;
		out.push_back (item);
	}
	ResolveEffectiveTypes (out);
	return out;
}

// --- GDL parameters --------------------------------------------------------------------

Int32 ParamCount (API_AddParType** params)
{
	if (params == nullptr || *params == nullptr)
		return 0;
	return (Int32) (BMGetHandleSize (reinterpret_cast<GSConstHandle> (params)) / sizeof (API_AddParType));
}


API_AddParType* FindParamByName (API_AddParType** params, const GS::UniString& name)
{
	const Int32 n = ParamCount (params);
	for (Int32 i = 0; i < n; ++i) {
		if (EqualsIgnoreCase (GS::UniString ((*params)[i].name, CC_UTF8), name))
			return &(*params)[i];
	}
	return nullptr;
}


static bool IsValueless (API_AddParID t)
{
	return t == APIParT_Separator || t == APIParT_Title;
}


static void AddScalar (OS& out, const GS::String& key, const API_AddParType& p)
{
	switch (p.typeID) {
		case APIParT_CString:			out.Add (key, GS::UniString (p.value.uStr)); break;
		case APIParT_Angle:				out.Add (key, RadToDeg (p.value.real)); break;
		case APIParT_Boolean:
		case APIParT_LightSw:			out.Add (key, p.value.real != 0.0); break;
		case APIParT_Integer:
		case APIParT_LineTyp:
		case APIParT_Mater:
		case APIParT_FillPat:
		case APIParT_PenCol:
		case APIParT_BuildingMaterial:
		case APIParT_Profile:			out.Add (key, (Int32) std::lround (p.value.real)); break;
		default:						out.Add (key, p.value.real); break;
	}
}


OS ParamsSummary (API_AddParType** params, UInt32 maxCount, Int32* shownCount)
{
	OS out;
	UInt32 shown = 0;
	const Int32 n = ParamCount (params);
	for (Int32 i = 0; i < n && shown < maxCount; ++i) {
		const API_AddParType& p = (*params)[i];
		if (IsValueless (p.typeID) || p.typeID == APIParT_Dictionary || (p.flags & API_ParFlg_Hidden) != 0)
			continue;
		const GS::String key (p.name);
		if (key.IsEmpty () || out.Contains (key))
			continue;
		if (p.typeMod == API_ParArray) {
			out.Add (key, GS::UniString ("<array " + GS::ValueToUniString (p.dim1) + "x" + GS::ValueToUniString (p.dim2) + ">"));
		} else {
			AddScalar (out, key, p);
		}
		++shown;
	}
	if (shownCount != nullptr)
		*shownCount = (Int32) shown;
	return out;
}


void ReplaceMemoParams (API_ElementMemo& memo, API_AddParType** params)
{
	if (memo.params != nullptr)
		ACAPI_DisposeAddParHdl (&memo.params);
	memo.params = params;
}


API_ParamOwnerType ElementOwner (const API_Element& element)
{
	API_ParamOwnerType owner;
	BNZeroMemory (&owner, sizeof (owner));
	owner.guid = element.header.guid;
	owner.libInd = 0;
	owner.type = element.header.type;
	return owner;
}


API_ParamOwnerType LibPartOwner (Int32 libInd, API_ElemTypeID elemType)
{
	API_ParamOwnerType owner;
	BNZeroMemory (&owner, sizeof (owner));
	owner.guid = APINULLGuid;
	owner.libInd = libInd;
	owner.type = API_ElemType (elemType);
	return owner;
}


// --- ParamEditor -------------------------------------------------------------------------

ParamEditor::Guard::~Guard ()
{
	if (open)
		ACAPI_Goodies (APIAny_CloseParametersID);
}


ParamEditor::ParamEditor (const API_ParamOwnerType& owner)
{
	API_ParamOwnerType o = owner;
	Check (ACAPI_Goodies (APIAny_OpenParametersID, &o), "Cannot open the GDL parameter list of the library part");
	guard.open = true;

	API_GetParamsType getParams;
	BNZeroMemory (&getParams, sizeof (getParams));
	Check (ACAPI_Goodies (APIAny_GetActParametersID, &getParams), "Cannot read the GDL parameters");
	defs.Reset (getParams.params);
}


ParamEditor::~ParamEditor () = default;


bool ParamEditor::HasParam (const GS::UniString& name) const
{
	return FindParamByName (defs.Get (), name) != nullptr;
}


GS::UniString ParamEditor::SimilarNames (const GS::UniString& name) const
{
	GS::Array<GS::UniString> similar;
	const Int32 n = ParamCount (defs.Get ());
	for (Int32 i = 0; i < n && similar.GetSize () < 10; ++i) {
		const API_AddParType& p = (*defs.Get ())[i];
		if (IsValueless (p.typeID))
			continue;
		GS::UniString pname (p.name, CC_UTF8);
		GS::UniString desc (p.uDescname);
		if (pname.IsEmpty () || name.IsEmpty ())
			continue;
		if (ContainsNoCase (pname, name) || ContainsNoCase (name, pname) || ContainsNoCase (desc, name))
			similar.Push (pname);
	}
	if (similar.IsEmpty ())
		return GS::UniString ();
	return " Similar parameter names: " + JoinNames (similar) + ".";
}


void ParamEditor::ChangeNumber (const API_AddParType* def, const GS::UniString& name, double value, Int32 ind1, Int32 ind2)
{
	API_ChangeParamType chg;
	BNZeroMemory (&chg, sizeof (chg));
	CHTruncate (ToStr (name).ToCStr (), chg.name, sizeof (chg.name));
	chg.ind1 = ind1;
	chg.ind2 = ind2;
	chg.realValue = value;
	GSErrCode err = ACAPI_Goodies (APIAny_ChangeAParameterID, &chg);
	if (err != NoError && def != nullptr && def->typeMod == API_ParArray && def->dim2 == 0 && ind2 == 0) {
		chg.ind2 = 1;		// 1-dimensional array addressed as [i][1]
		err = ACAPI_Goodies (APIAny_ChangeAParameterID, &chg);
	}
	if (err != NoError) {
		GS::UniString where = ind1 > 0 ? "[" + GS::ValueToUniString (ind1) + "," + GS::ValueToUniString (ind2) + "]" : GS::UniString ();
		Fail ("Cannot set GDL parameter '" + name + where + "' (" + (def != nullptr ? ParamTypeName (def->typeID) : GS::UniString ("A/B")) +
			  "): " + ErrorName (err) + ". The parameter may be locked by the parameter script, or the value is out of its allowed range (see get_gdl_parameters with includeValueLists).", err);
	}
}


void ParamEditor::ChangeString (const API_AddParType* def, const GS::UniString& name, const GS::UniString& value, Int32 ind1, Int32 ind2)
{
	if (value.GetLength () >= API_UAddParStrLen)
		Fail ("Value of GDL string parameter '" + name + "' is too long (max " + GS::ValueToUniString (API_UAddParStrLen - 1) + " characters).");
	GS::uchar_t buffer[API_UAddParStrLen] = {};
	GS::ucsncpy (buffer, value.ToUStr ().Get (), API_UAddParStrLen - 1);

	API_ChangeParamType chg;
	BNZeroMemory (&chg, sizeof (chg));
	CHTruncate (ToStr (name).ToCStr (), chg.name, sizeof (chg.name));
	chg.ind1 = ind1;
	chg.ind2 = ind2;
	chg.uStrValue = buffer;
	GSErrCode err = ACAPI_Goodies (APIAny_ChangeAParameterID, &chg);
	if (err != NoError && def != nullptr && def->typeMod == API_ParArray && def->dim2 == 0 && ind2 == 0) {
		chg.ind2 = 1;
		err = ACAPI_Goodies (APIAny_ChangeAParameterID, &chg);
	}
	if (err != NoError)
		Fail ("Cannot set GDL string parameter '" + name + "': " + ErrorName (err) + ". The parameter may be locked, or the value is not in its value list (see get_gdl_parameters with includeValueLists).", err);
}


static std::optional<API_AttrTypeID> AttrTypeOfParam (API_AddParID t)
{
	switch (t) {
		case APIParT_Mater:				return API_MaterialID;
		case APIParT_LineTyp:			return API_LinetypeID;
		case APIParT_FillPat:			return API_FilltypeID;
		case APIParT_BuildingMaterial:	return API_BuildingMaterialID;
		case APIParT_Profile:			return API_ProfileID;
		default:						return std::nullopt;
	}
}


void ParamEditor::ApplyValue (const OS& values, const char* key)
{
	const GS::UniString name (key, CC_UTF8);
	const API_AddParType* def = FindParamByName (defs.Get (), name);
	const bool isAB = EqualsIgnoreCase (name, "A") || EqualsIgnoreCase (name, "B");
	if (def == nullptr && !isAB) {
		Fail ("GDL parameter '" + name + "' does not exist in this library part." + SimilarNames (name) +
			  " Use get_gdl_parameters (placed elements) or get_library_part_details (library parts) to list parameter names.", APIERR_BADNAME);
	}
	const GS::UniString canonical = def != nullptr ? GS::UniString (def->name, CC_UTF8) : GS::UniString (EqualsIgnoreCase (name, "A") ? "A" : "B");

	if (def != nullptr) {
		if (IsValueless (def->typeID))
			Fail ("GDL parameter '" + canonical + "' is a " + ParamTypeName (def->typeID) + " and has no value.");
		if (def->typeID == APIParT_Dictionary)
			Fail ("GDL dictionary parameters (like '" + canonical + "') cannot be set through the API.", APIERR_NOTSUPPORTED);
	}

	// --- arrays ---
	if (def != nullptr && def->typeMod == API_ParArray) {
		if (!values.IsList (key))
			Fail ("GDL parameter '" + canonical + "' is an array; pass an array of values ([..] or rows [{values: [..]}]).");
		const Int32 dim1 = def->dim1;
		const Int32 cols = std::max<Int32> (def->dim2, 1);
		const bool isString = def->typeID == APIParT_CString;
		ListKindCollector kinds = ListKinds (values, key);

		auto cellInd2 = [&] (Int32 c) -> Int32 { return def->dim2 == 0 ? 0 : c + 1; };
		auto checkBounds = [&] (Int32 r, Int32 c) {
			if (r >= dim1 || c >= cols)
				Fail ("GDL array parameter '" + canonical + "' has " + GS::ValueToUniString (dim1) + " x " + GS::ValueToUniString (def->dim2) +
					  " cells; value [" + GS::ValueToUniString (r + 1) + "," + GS::ValueToUniString (c + 1) +
					  "] is out of range (the parameter script controls the array size).");
		};
		auto setNumberCell = [&] (Int32 r, Int32 c, double number) {
			checkBounds (r, c);
			ChangeNumber (def, canonical, def->typeID == APIParT_Angle ? DegToRad (number) : number, r + 1, cellInd2 (c));
		};
		auto setStringCell = [&] (Int32 r, Int32 c, const GS::UniString& str) {
			checkBounds (r, c);
			ChangeString (def, canonical, str, r + 1, cellInd2 (c));
		};

		if (kinds.objects > 0) {
			GS::Array<OS> rows = GetObjectArray (values, key, true);
			for (UIndex r = 0; r < rows.GetSize (); ++r) {
				if (isString) {
					GS::Array<GS::UniString> cells = GetStringArray (rows[r], "values", true);
					for (UIndex c = 0; c < cells.GetSize (); ++c)
						setStringCell ((Int32) r, (Int32) c, cells[c]);
				} else {
					GS::Array<double> cells = GetNumberArray (rows[r], "values", true);
					for (UIndex c = 0; c < cells.GetSize (); ++c)
						setNumberCell ((Int32) r, (Int32) c, cells[c]);
				}
			}
		} else if (isString) {
			GS::Array<GS::UniString> flat = GetStringArray (values, key, true);
			for (UIndex i = 0; i < flat.GetSize (); ++i)
				setStringCell ((Int32) (i / cols), (Int32) (i % cols), flat[i]);
		} else {
			GS::Array<double> flat = GetNumberArray (values, key, true);
			for (UIndex i = 0; i < flat.GetSize (); ++i)
				setNumberCell ((Int32) (i / cols), (Int32) (i % cols), flat[i]);
		}
		return;
	}

	// --- strings ---
	if (def != nullptr && def->typeID == APIParT_CString) {
		if (!values.IsString (key))
			Fail ("GDL parameter '" + canonical + "' is a String parameter; pass a string value.");
		ChangeString (def, canonical, GetString (values, key), 0, 0);
		return;
	}

	// --- numbers (Length, Angle, RealNum, Integer, Boolean, attributes, pens, light params, A/B) ---
	double value = 0.0;
	if (values.IsBool (key)) {
		value = GetBool (values, key) ? 1.0 : 0.0;
	} else if (IsNumber (values, key)) {
		value = GetDouble (values, key);
		if (def != nullptr && def->typeID == APIParT_Angle)
			value = DegToRad (value);
	} else if (values.IsString (key)) {
		const GS::UniString text = GetString (values, key);
		const std::optional<API_AttrTypeID> attrType = def != nullptr ? AttrTypeOfParam (def->typeID) : std::nullopt;
		if (attrType.has_value ()) {
			const std::optional<API_AttributeIndex> idx = FindAttrByName (*attrType, text);
			if (!idx.has_value ())
				Fail (AttrTypeName (*attrType) + " attribute named '" + text + "' (for GDL parameter '" + canonical + "') not found. Use get_attributes to list the localized names, or pass the attribute index.", APIERR_BADNAME);
			value = (double) *idx;
		} else {
			// Numeric parameter with a VALUES list with descriptions: accept the description text.
			bool matched = false;
			if (def != nullptr) {
				OS vl;
				GS::Array<OS> options;
				if (GetValueList (*def, vl) && vl.Contains ("values") && vl.Get ("values", options)) {
					for (const OS& opt : options) {
						GS::UniString d;
						if (opt.Contains ("description") && opt.Get ("description", d) && EqualsIgnoreCase (d, text)) {
							value = GetDouble (opt, "value");
							if (def->typeID == APIParT_Angle)
								value = DegToRad (value);
							matched = true;
							break;
						}
					}
				}
			}
			if (!matched)
				Fail ("GDL parameter '" + canonical + "' is numeric (" + (def != nullptr ? ParamTypeName (def->typeID) : GS::UniString ("Length")) +
					  "); pass a number" + (def != nullptr && def->typeID == APIParT_Angle ? " in degrees" : "") +
					  " (lengths in meters, booleans as true/false).");
		}
	} else {
		Fail ("Invalid value for GDL parameter '" + canonical + "': expected a number, boolean or string.");
	}
	ChangeNumber (def, canonical, value, 0, 0);
}


void ParamEditor::Apply (const OS& values)
{
	const GS::HashSet<GS::String> fieldNames = values.GetFieldNames ();
	for (const GS::String& field : fieldNames)
		ApplyValue (values, field.ToCStr ());
}


bool ParamEditor::ApplyOne (const OS& values, const GS::String& key, bool strict)
{
	try {
		ApplyValue (values, key.ToCStr ());
		return true;
	} catch (const Error&) {
		if (strict)
			throw;
		return false;
	}
}


void ParamEditor::SetNumber (const GS::UniString& name, double value, bool strict)
{
	try {
		ChangeNumber (FindParamByName (defs.Get (), name), name, value, 0, 0);
	} catch (const Error&) {
		if (strict)
			throw;
	}
}


UInt32 ParamEditor::CarryOver (API_AddParType** source, const GS::Array<GS::UniString>& skipUpper)
{
	UInt32 copied = 0;
	const Int32 n = ParamCount (source);
	for (Int32 i = 0; i < n; ++i) {
		const API_AddParType& src = (*source)[i];
		if (IsValueless (src.typeID) || src.typeID == APIParT_Dictionary)
			continue;
		if ((src.flags & (API_ParFlg_Hidden | API_ParFlg_Unique)) != 0)
			continue;
		const GS::UniString name (src.name, CC_UTF8);
		GS::UniString upper = name;
		upper.SetToUpperCase ();
		if (skipUpper.Contains (upper))
			continue;
		const API_AddParType* def = FindParamByName (defs.Get (), name);
		if (def == nullptr || def->typeID != src.typeID || def->typeMod != src.typeMod || (def->flags & API_ParFlg_Hidden) != 0)
			continue;
		try {
			if (src.typeMod == API_ParArray) {
				if (def->dim1 != src.dim1 || def->dim2 != src.dim2 || src.value.array == nullptr || src.typeID == APIParT_CString)
					continue;
				const Int32 cols = std::max<Int32> (src.dim2, 1);
				const Int32 cells = src.dim1 * cols;
				if (cells <= 0 || cells > 256)
					continue;
				if (BMGetHandleSize (src.value.array) < (GSSize) (cells * sizeof (double)))
					continue;
				const double* vals = reinterpret_cast<const double*> (*src.value.array);
				for (Int32 k = 0; k < cells; ++k)
					ChangeNumber (def, name, vals[k], k / cols + 1, src.dim2 == 0 ? 0 : k % cols + 1);
			} else if (src.typeID == APIParT_CString) {
				const GS::UniString v (src.value.uStr);
				if (v == GS::UniString (def->value.uStr))
					continue;
				ChangeString (def, name, v, 0, 0);
			} else {
				if (std::fabs (src.value.real - def->value.real) < 1e-12)
					continue;
				ChangeNumber (def, name, src.value.real, 0, 0);
			}
			++copied;
		} catch (const Error&) {
			// best effort: parameters the new library part refuses are left at their defaults
		}
	}
	return copied;
}


bool ParamEditor::GetValueList (const API_AddParType& param, OS& out) const
{
	API_GetParamValuesType pv;
	BNZeroMemory (&pv, sizeof (pv));
	CHTruncate (param.name, pv.name, sizeof (pv.name));
	const GSErrCode err = ACAPI_Goodies (APIAny_GetParamValuesID, &pv);
	bool any = false;
	if (err == NoError) {
		if (pv.locked) {
			out.Add ("locked", true);
			any = true;
		}
		if (pv.custom)
			out.Add ("custom", true);

		GS::Array<GS::UniString> strings;
		if (pv.uStrValues != nullptr && *pv.uStrValues != nullptr) {
			const GSSize bytes = BMGetHandleSize (reinterpret_cast<GSConstHandle> (pv.uStrValues));
			const GS::uchar_t* s = *pv.uStrValues;
			const GS::uchar_t* end = s + bytes / (GSSize) sizeof (GS::uchar_t);
			for (Int32 i = 0; i < pv.nVals && s < end; ++i) {
				const GS::uchar_t* e = s;
				while (e < end && *e != 0)
					++e;
				strings.Push (GS::UniString (s, (USize) (e - s)));
				s = e + 1;
			}
		}

		GS::Array<OS> numbers;
		if (pv.realValues != nullptr && *pv.realValues != nullptr) {
			const GSSize bytes = BMGetHandleSize (reinterpret_cast<GSConstHandle> (pv.realValues));
			const Int32 count = std::min<Int32> (pv.nVals, (Int32) (bytes / (GSSize) sizeof (API_VLNumType)));
			const bool angle = param.typeID == APIParT_Angle;
			auto conv = [angle] (double v) { return angle ? RadToDeg (v) : v; };
			for (Int32 i = 0; i < count; ++i) {
				const API_VLNumType& v = (*pv.realValues)[i];
				OS item;
				if ((v.flags & (APIVLVal_LowerLimit | APIVLVal_UpperLimit | APIVLVal_Step)) != 0) {
					if ((v.flags & APIVLVal_LowerLimit) != 0) {
						item.Add ("min", conv (v.lowerLimit));
						item.Add ("minInclusive", (v.flags & APIVLVal_LowerEqual) != 0);
					}
					if ((v.flags & APIVLVal_UpperLimit) != 0) {
						item.Add ("max", conv (v.upperLimit));
						item.Add ("maxInclusive", (v.flags & APIVLVal_UpperEqual) != 0);
					}
					if ((v.flags & APIVLVal_Step) != 0) {
						item.Add ("stepStart", conv (v.stepBeg));
						item.Add ("step", conv (v.stepVal));
					}
				} else {
					item.Add ("value", conv (v.value));
				}
				const GS::UniString desc (v.valueDescription);
				if (!desc.IsEmpty ())
					item.Add ("description", desc);
				numbers.Push (item);
			}
		}

		if (!strings.IsEmpty ()) {
			out.Add ("values", strings);
			any = true;
		} else if (!numbers.IsEmpty ()) {
			out.Add ("values", numbers);
			any = true;
		}
	}
	if (pv.uStrValues != nullptr)
		BMKillHandle (reinterpret_cast<GSHandle*> (&pv.uStrValues));
	if (pv.realValues != nullptr)
		BMKillHandle (reinterpret_cast<GSHandle*> (&pv.realValues));
	return any;
}


API_AddParType** ParamEditor::Finish (double* a, double* b)
{
	API_GetParamsType getParams;
	BNZeroMemory (&getParams, sizeof (getParams));
	Check (ACAPI_Goodies (APIAny_GetActParametersID, &getParams), "Cannot read back the changed GDL parameters");
	if (a != nullptr) *a = getParams.a;
	if (b != nullptr) *b = getParams.b;
	return getParams.params;
}


GS::Array<OS> AddAttributeNames (API_AddParType** params, GS::Array<OS> items)
{
	for (UIndex i = 0; i < items.GetSize (); ++i) {
		GS::UniString name;
		if (!items[i].Get ("name", name))
			continue;
		const API_AddParType* p = FindParamByName (params, name);
		if (p == nullptr || p->typeMod == API_ParArray)
			continue;
		const std::optional<API_AttrTypeID> attrType = AttrTypeOfParam (p->typeID);
		if (!attrType.has_value ())
			continue;
		const GS::UniString attrName = AttrName (*attrType, (API_AttributeIndex) std::lround (p->value.real));
		if (!attrName.IsEmpty ())
			items[i].Add ("valueName", attrName);
	}
	return items;
}


void AddValueLists (ParamEditor& editor, API_AddParType** params, GS::Array<OS>& items)
{
	for (UIndex i = 0; i < items.GetSize (); ++i) {
		GS::UniString name;
		if (!items[i].Get ("name", name))
			continue;
		const API_AddParType* p = FindParamByName (params, name);
		if (p == nullptr)
			continue;
		OS vl;
		if (editor.GetValueList (*p, vl))
			items[i].Add ("valueList", vl);
	}
}

} // namespace objlib
} // namespace cc


// =============================================================================
// 2. Library parts: search, subtypes, details, scripts, creation
// =============================================================================

namespace cc {
namespace objlib {

namespace {

// --- Shared ------------------------------------------------------------------------------

GS::Array<API_LibraryInfo> LibraryList (Int32* embeddedIndex)
{
	GS::Array<API_LibraryInfo> libs;
	Int32 embedded = -1;
	Check (ACAPI_Environment (APIEnv_GetLibrariesID, &libs, &embedded), "Cannot read the loaded libraries");
	if (embeddedIndex != nullptr)
		*embeddedIndex = embedded;
	return libs;
}


bool EmbeddedLibraryLocation (IO::Location& loc)
{
	Int32 embedded = -1;
	GS::Array<API_LibraryInfo> libs;
	if (ACAPI_Environment (APIEnv_GetLibrariesID, &libs, &embedded) == NoError && embedded >= 0 && (UIndex) embedded < libs.GetSize ()) {
		loc = libs[(UIndex) embedded].location;
		return true;
	}
	API_SpecFolderID folderId = API_EmbeddedProjectLibraryFolderID;
	return ACAPI_Environment (APIEnv_GetSpecFolderID, &folderId, &loc) == NoError;
}


GS::Array<API_LibTypeID> ParseLibTypes (const OS& params, const char* key)
{
	GS::Array<API_LibTypeID> types;
	if (!params.Contains (key))
		return types;
	GS::Array<GS::UniString> names;
	if (params.IsString (key))
		names.Push (GetString (params, key));
	else
		names = GetStringArray (params, key, true);
	for (const GS::UniString& n : names) {
		const std::optional<API_LibTypeID> t = ParseLibType (n);
		if (!t.has_value ())
			Fail ("Unknown library part type '" + n + "'. Allowed: " + AllowedLibTypeNames () + ".");
		types.Push (*t);
	}
	return types;
}


GS::Array<GS::UniString> Tokenize (const GS::UniString& text)
{
	GS::Array<GS::UniString> tokens;
	text.Split (" \t", GS::UniString::SkipEmptyParts, &tokens);
	return tokens;
}


// Parent-chain lookup over all library parts (built lazily once per command).
class Ancestry {
public:
	void Build ()
	{
		if (built)
			return;
		built = true;
		items = ScanAllLibParts ();
		for (UIndex i = 0; i < (UIndex) items.size (); ++i) {
			if (!items[i].mainGuid.IsEmpty () && !byMain.ContainsKey (items[i].mainGuid))
				byMain.Put (items[i].mainGuid, i);
		}
	}

	const LibPartScanItem* Find (const GS::UniString& mainGuid) const
	{
		const UIndex* idx = byMain.GetPtr (mainGuid);
		return idx != nullptr ? &items[*idx] : nullptr;
	}

	// Parents from the direct parent up to the root.
	GS::Array<const LibPartScanItem*> Chain (const GS::UniString& parentMainGuid) const
	{
		GS::Array<const LibPartScanItem*> chain;
		GS::UniString current = parentMainGuid;
		for (int guard = 0; guard < 32 && !current.IsEmpty (); ++guard) {
			const LibPartScanItem* p = Find (current);
			if (p == nullptr || chain.Contains (p))
				break;
			chain.Push (p);
			current = p->parentMainGuid;
		}
		return chain;
	}

	std::vector<LibPartScanItem>				items;

private:
	bool										built = false;
	GS::HashTable<GS::UniString, UIndex>		byMain;
};


GSErrCode ReadTextSection (Int32 libInd, GSType sectType, GS::UniString& text)
{
	API_LibPartSection section;
	BNZeroMemory (&section, sizeof (section));
	section.sectType = sectType;
	GSHandle hdl = nullptr;
	GS::UniString str;
	GSErrCode err = ACAPI_LibPart_GetSection (libInd, &section, &hdl, &str);
	if (err != NoError) {
		// Some text sections (e.g. keywords) are only returned as raw bytes (sectionStr == nullptr).
		if (hdl != nullptr)
			BMKillHandle (&hdl);
		str.Clear ();
		BNZeroMemory (&section, sizeof (section));
		section.sectType = sectType;
		err = ACAPI_LibPart_GetSection (libInd, &section, &hdl, nullptr);
	}
	if (err == NoError) {
		if (!str.IsEmpty () || hdl == nullptr) {
			text = str;
		} else {
			const GSSize size = BMGetHandleSize (hdl);
			if (size > 0) {
				// Heuristic: UTF-16 text contains zero high bytes; otherwise treat as UTF-8.
				const char* bytes = *hdl;
				GSSize zeros = 0;
				for (GSSize i = 1; i < size; i += 2)
					if (bytes[i] == 0) ++zeros;
				if (size % 2 == 0 && zeros > size / 4)
					text = GS::UniString (reinterpret_cast<const GS::uchar_t*> (bytes), (USize) (size / 2));
				else
					text = GS::UniString (bytes, (USize) size, CC_UTF8);
				while (!text.IsEmpty () && text[text.GetLength () - 1] == '\0')
					text.DeleteLast ();
			}
		}
	}
	if (hdl != nullptr)
		BMKillHandle (&hdl);
	return err;
}

// Built-in subtype keywords (defined with CreateLibraryPart below).
struct SubtypeKeyword;
const SubtypeKeyword*	FindSubtypeKeyword (const GS::UniString& name);
GS::String				BuiltInUnId (const SubtypeKeyword& k);

// --- SearchLibraryParts --------------------------------------------------------------------

OS SearchLibraryParts (const OS& params)
{
	GS::UniString query = GetString (params, "query", GS::UniString ());
	query.Trim ();
	const GS::UniString queryLower = query.ToLowerCase ();
	const GS::Array<GS::UniString> tokens = Tokenize (queryLower);
	const GS::Array<API_LibTypeID> types = ParseLibTypes (params, "type");
	const bool placeableOnly = GetBool (params, "placeableOnly", true);
	const bool embeddedOnly = GetBool (params, "embeddedOnly", false);
	const Int32 offset = std::max<Int32> (0, GetInt (params, "offset", 0));
	const Int32 limit = std::min<Int32> (2000, std::max<Int32> (1, GetInt (params, "limit", 100)));

	std::optional<GS::String> ancestorUnId;
	GS::UniString ancestorName;
	if (params.Contains ("subtypeOf")) {
		const SubtypeKeyword* keyword = params.IsString ("subtypeOf") ? FindSubtypeKeyword (GetString (params, "subtypeOf")) : nullptr;
		if (keyword != nullptr) {
			ancestorUnId = BuiltInUnId (*keyword);
			ancestorName = GetString (params, "subtypeOf");
		} else {
			const API_LibPart anc = FindLibPart (params, "subtypeOf");
			ancestorUnId = GS::String (anc.ownUnID);
			ancestorName = GS::UniString (anc.docu_UName);
		}
	}
	IO::Location embeddedLoc;
	if (embeddedOnly && !EmbeddedLibraryLocation (embeddedLoc))
		Fail ("The embedded library of this project is not available.", APIERR_NOLIB);

	const std::vector<LibPartScanItem> all = ScanAllLibParts ();
	GS::HashTable<GS::UniString, GS::UniString> nameByMain;
	for (const LibPartScanItem& it : all) {
		if (!it.mainGuid.IsEmpty () && !nameByMain.ContainsKey (it.mainGuid))
			nameByMain.Put (it.mainGuid, it.name);
	}

	struct Hit {
		const LibPartScanItem*	item;
		int						rank;
	};
	std::vector<Hit> hits;
	for (const LibPartScanItem& it : all) {
		if (it.missingDef)
			continue;
		if (!types.IsEmpty () && !types.Contains (it.typeID))
			continue;
		if (placeableOnly && !it.isPlaceable)
			continue;

		int rank = 5;
		if (!query.IsEmpty ()) {
			const GS::UniString nameLower = it.name.ToLowerCase ();
			const GS::UniString fileLower = it.fileName.ToLowerCase ();
			if (nameLower == queryLower || fileLower == queryLower || fileLower == queryLower + ".gsm")
				rank = 0;
			else if (nameLower.BeginsWith (queryLower))
				rank = 1;
			else if (nameLower.Contains (queryLower))
				rank = 2;
			else {
				bool inName = true, inAny = true;
				for (const GS::UniString& t : tokens) {
					const bool n = nameLower.Contains (t);
					inName = inName && n;
					inAny = inAny && (n || fileLower.Contains (t));
				}
				if (inName)
					rank = 3;
				else if (inAny)
					rank = 4;
				else
					continue;
			}
		}
		if (ancestorUnId.has_value ()) {
			char successor[256] = {};
			char predecessor[256] = {};
			CHTruncate (ToStr (it.ownUnId).ToCStr (), successor, sizeof (successor));
			CHTruncate (ancestorUnId->ToCStr (), predecessor, sizeof (predecessor));
			if (ACAPI_Goodies (APIAny_CheckLibPartSubtypeOfbyMainID, successor, predecessor) != NoError)
				continue;
		}
		if (embeddedOnly) {
			API_LibPart lp;
			IO::Location loc;
			if (FetchLibPart (it.index, lp, nullptr, &loc) != NoError || !embeddedLoc.IsAncestorOf (loc))
				continue;
		}
		hits.push_back ({ &it, rank });
	}

	std::stable_sort (hits.begin (), hits.end (), [] (const Hit& x, const Hit& y) {
		if (x.rank != y.rank)
			return x.rank < y.rank;
		return x.item->name.IsLess (y.item->name, GS::UniString::CaseInsensitive);
	});

	GS::Array<OS> list;
	for (Int32 i = offset; i < (Int32) hits.size () && (Int32) list.GetSize () < limit; ++i) {
		const LibPartScanItem& it = *hits[(size_t) i].item;
		OS o;
		o.Add ("index", it.index);
		o.Add ("name", it.name);
		o.Add ("guid", it.ownUnId);
		o.Add ("type", LibTypeName (it.typeID));
		o.Add ("fileName", it.fileName);
		if (!it.isPlaceable)
			o.Add ("isPlaceable", false);
		if (it.isTemplate)
			o.Add ("isTemplate", true);
		const GS::UniString* parentName = nameByMain.GetPtr (it.parentMainGuid);
		if (parentName != nullptr)
			o.Add ("subtype", *parentName);
		list.Push (o);
	}

	OS out;
	out.Add ("libraryParts", list);
	out.Add ("total", (Int32) hits.size ());
	out.Add ("offset", offset);
	out.Add ("hasMore", offset + (Int32) list.GetSize () < (Int32) hits.size ());
	out.Add ("scanned", (Int32) all.size ());
	if (ancestorUnId.has_value ())
		out.Add ("subtypeOf", ancestorName);
	if (hits.empty ()) {
		out.Add ("hint", GS::UniString ("No match. Library part names are LOCALIZED (this Archicad may use Russian names): try a shorter substring, "
										"a word in the Archicad language, drop the type filter, set placeableOnly false, or browse categories with get_library_part_subtypes."));
	}
	return out;
}

// --- GetLibraryPartSubtypes -------------------------------------------------------------------

OS GetLibraryPartSubtypes (const OS& params)
{
	GS::UniString query = GetString (params, "query", GS::UniString ());
	query.Trim ();
	const GS::Array<API_LibTypeID> types = ParseLibTypes (params, "type");
	const Int32 offset = std::max<Int32> (0, GetInt (params, "offset", 0));
	const Int32 limit = std::min<Int32> (2000, std::max<Int32> (1, GetInt (params, "limit", 300)));

	Ancestry anc;
	anc.Build ();

	GS::HashSet<GS::UniString> referenced;
	GS::HashTable<GS::UniString, Int32> placeableChildren;
	GS::HashTable<GS::UniString, Int32> subtypeChildren;
	for (const LibPartScanItem& it : anc.items) {
		if (it.parentMainGuid.IsEmpty ())
			continue;
		referenced.Add (it.parentMainGuid);
		if (it.isPlaceable && !it.missingDef) {
			Int32* c = placeableChildren.GetPtr (it.parentMainGuid);
			if (c != nullptr) ++(*c); else placeableChildren.Put (it.parentMainGuid, 1);
		}
		if (it.isTemplate) {
			Int32* c = subtypeChildren.GetPtr (it.parentMainGuid);
			if (c != nullptr) ++(*c); else subtypeChildren.Put (it.parentMainGuid, 1);
		}
	}

	struct Entry {
		const LibPartScanItem*	item;
		GS::UniString			path;
		Int32					depth;
	};
	std::vector<Entry> entries;
	GS::HashSet<GS::UniString> seen;
	for (const LibPartScanItem& it : anc.items) {
		if (it.mainGuid.IsEmpty () || seen.Contains (it.mainGuid))
			continue;
		if (!it.isTemplate && !referenced.Contains (it.mainGuid))
			continue;
		seen.Add (it.mainGuid);
		if (!types.IsEmpty () && !types.Contains (it.typeID))
			continue;
		GS::Array<const LibPartScanItem*> chain = anc.Chain (it.parentMainGuid);
		GS::UniString path;
		for (Int32 i = (Int32) chain.GetSize () - 1; i >= 0; --i)
			path += chain[(UIndex) i]->name + " > ";
		path += it.name;
		if (!query.IsEmpty () && !ContainsNoCase (path, query))
			continue;
		entries.push_back ({ &it, path, (Int32) chain.GetSize () });
	}
	std::sort (entries.begin (), entries.end (), [] (const Entry& x, const Entry& y) {
		return x.path.IsLess (y.path, GS::UniString::CaseInsensitive);
	});

	GS::Array<OS> list;
	for (Int32 i = offset; i < (Int32) entries.size () && (Int32) list.GetSize () < limit; ++i) {
		const Entry& e = entries[(size_t) i];
		OS o;
		o.Add ("name", e.item->name);
		o.Add ("guid", e.item->ownUnId);
		o.Add ("index", e.item->index);
		o.Add ("type", LibTypeName (e.item->typeID));
		o.Add ("path", e.path);
		o.Add ("depth", e.depth);
		const LibPartScanItem* parent = anc.Find (e.item->parentMainGuid);
		if (parent != nullptr)
			o.Add ("parent", OS ("name", parent->name, "guid", parent->ownUnId));
		const Int32* pc = placeableChildren.GetPtr (e.item->mainGuid);
		const Int32* sc = subtypeChildren.GetPtr (e.item->mainGuid);
		o.Add ("placeablePartCount", pc != nullptr ? *pc : 0);
		o.Add ("subtypeCount", sc != nullptr ? *sc : 0);
		list.Push (o);
	}
	OS out;
	out.Add ("subtypes", list);
	out.Add ("total", (Int32) entries.size ());
	out.Add ("offset", offset);
	out.Add ("hasMore", offset + (Int32) list.GetSize () < (Int32) entries.size ());
	return out;
}

// --- GetLibraryPartDetails -----------------------------------------------------------------------

const char* SectionKey (GSType t)
{
	switch (t) {
		case API_Sect1DScript:		return "masterScript";
		case API_Sect2DScript:		return "script2D";
		case API_Sect3DScript:		return "script3D";
		case API_SectVLScript:		return "parameterScript";
		case API_SectUIScript:		return "interfaceScript";
		case API_SectPRScript:		return "propertiesScript";
		case API_SectFWMScript:		return "forwardMigrationScript";
		case API_SectBWMScript:		return "backwardMigrationScript";
		case API_SectComText:		return "comment";
		case API_SectKeywords:		return "keywords";
		case API_SectParamDef:		return "parameters";
		case API_Sect2DDraw:		return "2dDrawing";
		case API_Sect3DBinData:		return "3dBinary";
		case API_SectInfoGIF:		return "previewPicture";
		case API_SectInfoPict:		return "previewPicture";
		case API_SectGDLPict:		return "gdlPicture";
		case API_SectCalledMacros:	return "calledMacros";
		case API_SectAncestors:		return "ancestors";
		case API_SectMigrTable:		return "migrationTable";
		case API_SectPropData:		return "properties";
		case API_SectCopyright:		return "copyright";
		case API_SectLibPartURL:	return "url";
		default:					return nullptr;
	}
}


OS DetailsJson (const API_LibPart& lp, API_LibTypeID type)
{
	API_LibPartDetails d;
	BNZeroMemory (&d, sizeof (d));
	OS out;
	if (ACAPI_LibPart_GetDetails (lp.index, &d) != NoError)
		return out;
	if (type == APILib_WindowID || type == APILib_DoorID || type == APILib_SkylightID) {
		out.Add ("wallInset", (double) d.wind.wallInset);
		out.Add ("mirrorThickness", (double) d.wind.mirThick);
		out.Add ("leftFrame", (double) d.wind.leftFram);
		out.Add ("rightFrame", (double) d.wind.righFram);
		out.Add ("topFrame", (double) d.wind.topFram);
		out.Add ("bottomFrame", (double) d.wind.botFram);
		auto addExpr = [&] (const char* key, const char (&expr)[512]) {
			char buf[513] = {};
			std::memcpy (buf, expr, sizeof (expr));		// the API field is not guaranteed to be terminated
			if (buf[0] != 0)
				out.Add (key, GS::UniString (buf, CC_UTF8));
		};
		addExpr ("wallInsetExpression", d.wind.wallInsetExpr);
		addExpr ("mirrorThicknessExpression", d.wind.mirThickExpr);
		out.Add ("contours3D", d.wind.contours_3D);
		out.Add ("runParameterScriptOnlyOnce", d.wind.runVLScriptOnlyOnce);
	} else if (type == APILib_ObjectID || type == APILib_LampID || type == APILib_LabelID || type == APILib_RoomID) {
		out.Add ("fixSize", d.object.fixSize);
		out.Add ("autoHotspots", d.object.autoHotspot);
		out.Add ("sizeTo2D", d.object.sizeTo2D);
		out.Add ("runParameterScriptOnlyOnce", d.object.runVLScriptOnlyOnce);
		out.Add ("enable2DScriptDrawingOrder", d.object.enable2DScriptDrawingOrder);
	}
	return out;
}


OS LibraryPartDetails (const OS& ref, bool includeParameters, bool includeHidden, bool includeValueLists,
					   const GS::Array<GS::UniString>& names, Ancestry& anc, const GS::Array<API_LibraryInfo>& libs)
{
	OS holder;
	holder.Add ("libraryPart", ref);
	const API_LibPart found = FindLibPart (holder, "libraryPart");

	API_LibPart lp;
	GS::UniString path;
	IO::Location loc;
	FetchLibPart (found.index, lp, &path, &loc);
	const API_LibTypeID type = EffectiveLibType (lp);
	API_LibPart shown = lp;
	shown.typeID = type;
	OS out = LibPartToJson (shown);
	out.Add ("location", path);
	for (const API_LibraryInfo& lib : libs) {
		if (lib.location == loc || lib.location.IsAncestorOf (loc)) {
			OS l;
			l.Add ("name", lib.name);
			l.Add ("path", LocationPath (lib.location));
			l.Add ("embedded", lib.libraryType == API_EmbeddedLibrary);
			out.Add ("library", l);
			break;
		}
	}

	anc.Build ();
	GS::Array<OS> ancestry;
	for (const LibPartScanItem* p : anc.Chain (MainGuidOf (lp.parentUnID)))
		ancestry.Push (OS ("name", p->name, "guid", p->ownUnId));
	if (!ancestry.IsEmpty ())
		out.Add ("subtype", ancestry[0]);
	out.Add ("ancestry", ancestry);

	{
		API_LibPart tmp = lp;
		tmp.location = nullptr;
		API_ToolBoxItem tb;
		BNZeroMemory (&tb, sizeof (tb));
		if (ACAPI_Goodies (APIAny_GetLibPartToolVariationID, &tmp, &tb) == NoError && tb.type.typeID != API_ZombieElemID)
			out.Add ("creatorTool", ElemTypeName (tb.type));
		if (tmp.location != nullptr)
			delete tmp.location;
	}

	OS details = DetailsJson (lp, type);
	if (!details.IsEmpty ())
		out.Add ("details", details);

	{
		Int32 nSections = 0;
		API_LibPartSection** sections = nullptr;
		if (ACAPI_LibPart_GetSectionList (lp.index, &nSections, &sections) == NoError && sections != nullptr) {
			GS::Array<GS::UniString> present;
			const Int32 count = std::min<Int32> (nSections, (Int32) (BMGetHandleSize (reinterpret_cast<GSConstHandle> (sections)) / sizeof (API_LibPartSection)));
			for (Int32 i = 0; i < count; ++i) {
				const char* key = SectionKey ((*sections)[i].sectType);
				const GS::UniString name = key != nullptr ? GS::UniString (key) : FourCC ((*sections)[i].sectType);
				if (!present.Contains (name))
					present.Push (name);
			}
			out.Add ("sections", present);
		}
		if (sections != nullptr)
			BMKillHandle (reinterpret_cast<GSHandle*> (&sections));
	}

	GS::UniString text;
	if (ReadTextSection (lp.index, API_SectComText, text) == NoError && !text.IsEmpty ())
		out.Add ("comment", text);
	text.Clear ();
	if (ReadTextSection (lp.index, API_SectKeywords, text) == NoError && !text.IsEmpty ())
		out.Add ("keywords", text);

	ParamsHandle defaults;
	try {
		LoadDefaultParams (lp.index, defaults);
	} catch (const Error& e) {
		out.Add ("parametersError", e.message);
		return out;
	}
	out.Add ("sizeA", defaults.a);
	out.Add ("sizeB", defaults.b);
	if (const API_AddParType* z = FindParamByName (defaults.Get (), "ZZYZX")) {
		if (z->typeMod != API_ParArray && z->typeID != APIParT_CString)
			out.Add ("height", z->value.real);
	}
	if (includeParameters) {
		GS::Array<OS> items = AddAttributeNames (defaults.Get (), ParamsToJson (defaults.Get (), includeHidden, names));
		if (includeValueLists && !items.IsEmpty ()) {
			try {
				ParamEditor ed (LibPartOwner (lp.index, ElemTypeForLibType (type)));
				AddValueLists (ed, defaults.Get (), items);
			} catch (const Error& e) {
				out.Add ("valueListsError", e.message);
			}
		}
		out.Add ("parameterCount", (Int32) items.GetSize ());
		out.Add ("parameters", items);
	}
	return out;
}

// --- GetLibraryPartScripts -----------------------------------------------------------------------

struct ScriptSection {
	const char*	key;
	GSType		type;
	bool		byDefault;
};

const ScriptSection kScriptSections[] = {
	{ "masterScript",				API_Sect1DScript,	true },
	{ "script2D",					API_Sect2DScript,	true },
	{ "script3D",					API_Sect3DScript,	true },
	{ "parameterScript",			API_SectVLScript,	true },
	{ "interfaceScript",			API_SectUIScript,	true },
	{ "propertiesScript",			API_SectPRScript,	true },
	{ "forwardMigrationScript",		API_SectFWMScript,	false },
	{ "backwardMigrationScript",	API_SectBWMScript,	false },
	{ "comment",					API_SectComText,	false },
	{ "keywords",					API_SectKeywords,	false },
};


GS::UniString ScriptKeyList ()
{
	GS::UniString s;
	for (const ScriptSection& sec : kScriptSections) {
		if (!s.IsEmpty ()) s += ", ";
		s += sec.key;
	}
	return s;
}


OS GetLibraryPartScripts (const OS& params)
{
	const API_LibPart lp = FindLibPart (params, "libraryPart");
	const Int32 maxLength = std::max<Int32> (100, GetInt (params, "maxLength", 60000));
	GS::Array<GS::UniString> wanted = GetStringArray (params, "scripts");
	for (const GS::UniString& w : wanted) {
		bool known = false;
		for (const ScriptSection& sec : kScriptSections)
			known = known || EqualsIgnoreCase (w, sec.key);
		if (!known)
			Fail ("Unknown script '" + w + "'. Allowed: " + ScriptKeyList () + ".");
	}

	OS scripts, lengths, errors;
	GS::Array<GS::UniString> missing, truncated;
	for (const ScriptSection& sec : kScriptSections) {
		bool take = wanted.IsEmpty () ? sec.byDefault : false;
		for (const GS::UniString& w : wanted)
			take = take || EqualsIgnoreCase (w, sec.key);
		if (!take)
			continue;
		GS::UniString text;
		const GSErrCode err = ReadTextSection (lp.index, sec.type, text);
		if (err == NoError) {
			lengths.Add (sec.key, (Int32) text.GetLength ());
			if ((Int32) text.GetLength () > maxLength) {
				text = text.GetSubstring (0, (USize) maxLength);
				truncated.Push (sec.key);
			}
			scripts.Add (sec.key, text);
		} else if (err == APIERR_NOLIBSECT || err == APIERR_BADINDEX || err == APIERR_BADPARS) {
			missing.Push (sec.key);
		} else {
			errors.Add (sec.key, ErrorName (err));
		}
	}

	OS out;
	out.Add ("libraryPart", LibPartRefJson (lp.index));
	out.Add ("scripts", scripts);
	out.Add ("lengths", lengths);
	if (!missing.IsEmpty ())
		out.Add ("missing", missing);
	if (!truncated.IsEmpty ())
		out.Add ("truncated", truncated);
	if (!errors.IsEmpty ()) {
		out.Add ("errors", errors);
		out.Add ("errorsNote", GS::UniString ("Scripts of protected (encrypted) library parts cannot be read."));
	}
	return out;
}

// --- CreateLibraryPart ------------------------------------------------------------------------------

struct SubtypeKeyword {
	const char*				name;
	BL::BuiltInLibPartID	id;
	API_LibTypeID			libType;	// library part type implied by the subtype
};

const SubtypeKeyword kSubtypes[] = {
	{ "GeneralGDLObject",		BL::BuiltInLibPartID::GeneralGDLObjectLibPartID,		APILib_ObjectID },
	{ "ModelElement",			BL::BuiltInLibPartID::ModelElementLibPartID,			APILib_ObjectID },
	{ "BuildingElement",		BL::BuiltInLibPartID::BuildingElementLibPartID,			APILib_ObjectID },
	{ "Furnishing",				BL::BuiltInLibPartID::FurnishingLibPartID,				APILib_ObjectID },
	{ "Beds",					BL::BuiltInLibPartID::BedsLibPartID,					APILib_ObjectID },
	{ "Structure",				BL::BuiltInLibPartID::StructureLibPartID,				APILib_ObjectID },
	{ "Column",					BL::BuiltInLibPartID::ColumnLibPartID,					APILib_ObjectID },
	{ "Beam",					BL::BuiltInLibPartID::BeamLibPartID,					APILib_ObjectID },
	{ "Slab",					BL::BuiltInLibPartID::SlabLibPartID,					APILib_ObjectID },
	{ "Wall",					BL::BuiltInLibPartID::WallLibPartID,					APILib_ObjectID },
	{ "Roof",					BL::BuiltInLibPartID::RoofLibPartID,					APILib_ObjectID },
	{ "Stair",					BL::BuiltInLibPartID::StairLibPartID,					APILib_ObjectID },
	{ "Railing",				BL::BuiltInLibPartID::RailingLibPartID,					APILib_ObjectID },
	{ "Ramp",					BL::BuiltInLibPartID::RampLibPartID,					APILib_ObjectID },
	{ "Covering",				BL::BuiltInLibPartID::CoveringLibPartID,				APILib_ObjectID },
	{ "Footing",				BL::BuiltInLibPartID::FootingLibPartID,					APILib_ObjectID },
	{ "Plant",					BL::BuiltInLibPartID::PlantLibPartID,					APILib_ObjectID },
	{ "People",					BL::BuiltInLibPartID::PeopleLibPartID,					APILib_ObjectID },
	{ "Animal",					BL::BuiltInLibPartID::AnimalLibPartID,					APILib_ObjectID },
	{ "Traffic",				BL::BuiltInLibPartID::TrafficLibPartID,					APILib_ObjectID },
	{ "TransportElement",		BL::BuiltInLibPartID::TransportElementLibPartID,		APILib_ObjectID },
	{ "StreetFurniture",		BL::BuiltInLibPartID::StreetFurnitureLibPartID,			APILib_ObjectID },
	{ "SportField",				BL::BuiltInLibPartID::SportFieldLibPartID,				APILib_ObjectID },
	{ "DistributionElement",	BL::BuiltInLibPartID::DistributionElementLibPartID,		APILib_ObjectID },
	{ "ElectricalElement",		BL::BuiltInLibPartID::ElectricalElementLibPartID,		APILib_ObjectID },
	{ "FlowTerminal",			BL::BuiltInLibPartID::FlowTerminalLibPartID,			APILib_ObjectID },
	{ "FlowEquipment",			BL::BuiltInLibPartID::FlowEquipmentLibPartID,			APILib_ObjectID },
	{ "SolarPVPanels",			BL::BuiltInLibPartID::SolarPVPanelsLibPartID,			APILib_ObjectID },
	{ "DrawingSymbol",			BL::BuiltInLibPartID::DrawingSymbolLibPartID,			APILib_ObjectID },
	{ "DocumentationElement",	BL::BuiltInLibPartID::DocumentationElementLibPartID,	APILib_ObjectID },
	{ "Marker",					BL::BuiltInLibPartID::MarkerLibPartID,					APILib_ObjectID },
	{ "PropertyObjects",		BL::BuiltInLibPartID::PropertyObjectsLibPartID,			APILib_ObjectID },
	{ "Light",					BL::BuiltInLibPartID::LightLibPartID,					APILib_LampID },
	{ "WindowWall",				BL::BuiltInLibPartID::WindowWallLibPartID,				APILib_WindowID },
	{ "CornerWindow",			BL::BuiltInLibPartID::CornerWindowLibPartID,			APILib_WindowID },
	{ "DoorWall",				BL::BuiltInLibPartID::DoorWallLibPartID,				APILib_DoorID },
	{ "WallOpening",			BL::BuiltInLibPartID::WallOpeningLibPartID,				APILib_WindowID },
	{ "WallEnd",				BL::BuiltInLibPartID::WallEndLibPartID,					APILib_WindowID },
	{ "Skylight",				BL::BuiltInLibPartID::SkylightLibPartID,				APILib_SkylightID },
	{ "Label",					BL::BuiltInLibPartID::LabelLibPartID,					APILib_LabelID },
	{ "ZoneStamp",				BL::BuiltInLibPartID::ZoneStampLibPartID,				APILib_RoomID },
};


GS::UniString SubtypeKeywordList ()
{
	GS::UniString s;
	for (const SubtypeKeyword& k : kSubtypes) {
		if (!s.IsEmpty ()) s += ", ";
		s += k.name;
	}
	return s;
}


const SubtypeKeyword* FindSubtypeKeyword (const GS::UniString& name)
{
	for (const SubtypeKeyword& k : kSubtypes) {
		if (EqualsIgnoreCase (name, k.name))
			return &k;
	}
	return nullptr;
}


const SubtypeKeyword& DefaultSubtype (API_LibTypeID libType)
{
	const char* key = "ModelElement";
	switch (libType) {
		case APILib_LampID:		key = "Light"; break;
		case APILib_WindowID:	key = "WindowWall"; break;
		case APILib_DoorID:		key = "DoorWall"; break;
		case APILib_SkylightID:	key = "Skylight"; break;
		case APILib_LabelID:	key = "Label"; break;
		case APILib_RoomID:		key = "ZoneStamp"; break;
		default:				break;
	}
	return *FindSubtypeKeyword (key);
}


GS::String BuiltInUnId (const SubtypeKeyword& k)
{
	const BL::BuiltInLibraryMainGuidContainer& container = BL::BuiltInLibraryMainGuidContainer::GetInstance ();
	if (!container.IsXMLLoaded ())
		Fail ("Archicad's built-in subtype table is not available; pass 'subtype' as a library part GUID or name (see get_library_part_subtypes).", APIERR_GENERAL);
	const GS::UnID unId = container.GetUnIDWithNullRevGuid (k.id);
	return ToStr (unId.ToUniString ());
}


// Resolves the parent (subtype) unique ID and the library part type.
GS::String ResolveParent (const OS& p, API_LibTypeID& libType, bool typeGiven, GS::UniString& parentName)
{
	if (!p.Contains ("subtype")) {
		const SubtypeKeyword& k = DefaultSubtype (libType);
		parentName = k.name;
		return BuiltInUnId (k);
	}
	if (p.IsString ("subtype")) {
		const GS::UniString s = GetString (p, "subtype");
		if (const SubtypeKeyword* k = FindSubtypeKeyword (s)) {
			if (typeGiven && k->libType != libType)
				Fail ("Subtype '" + s + "' is for " + LibTypeName (k->libType) + " library parts, but type is " + LibTypeName (libType) + ".");
			libType = k->libType;
			parentName = k->name;
			return BuiltInUnId (*k);
		}
	}
	// A library part (template) given by name / guid / index.
	const API_LibPart parent = FindLibPart (p, "subtype");
	if (!typeGiven) {
		const API_LibTypeID parentType = EffectiveLibType (parent);
		libType = parentType == APILib_MacroID ? APILib_ObjectID : parentType;
	}
	parentName = GS::UniString (parent.docu_UName);
	return GS::String (parent.ownUnID);
}


// Parameter handle built by us (BMAllocateHandle) — frees array value handles too.
class ManualParams {
public:
	~ManualParams ()
	{
		if (handle == nullptr)
			return;
		const Int32 n = ParamCount (handle);
		for (Int32 i = 0; i < n; ++i) {
			API_AddParType& p = (*handle)[i];
			if (p.typeMod == API_ParArray && p.value.array != nullptr)
				BMKillHandle (&p.value.array);
		}
		BMKillHandle (reinterpret_cast<GSHandle*> (&handle));
	}
	API_AddParType** handle = nullptr;
};


const NamedValue kParamTypes[] = {
	{ "Length",				APIParT_Length },
	{ "Angle",				APIParT_Angle },
	{ "RealNum",			APIParT_RealNum },
	{ "Real",				APIParT_RealNum },
	{ "Number",				APIParT_RealNum },
	{ "Integer",			APIParT_Integer },
	{ "Boolean",			APIParT_Boolean },
	{ "String",				APIParT_CString },
	{ "Surface",			APIParT_Mater },
	{ "Material",			APIParT_Mater },
	{ "Pen",				APIParT_PenCol },
	{ "LineType",			APIParT_LineTyp },
	{ "Fill",				APIParT_FillPat },
	{ "FillPattern",		APIParT_FillPat },
	{ "BuildingMaterial",	APIParT_BuildingMaterial },
	{ "Profile",			APIParT_Profile },
	{ "LightSwitch",		APIParT_LightSw },
	{ "ColorRGB",			APIParT_ColRGB },
	{ "Intensity",			APIParT_Intens },
	{ "Separator",			APIParT_Separator },
	{ "Title",				APIParT_Title },
};


std::optional<API_AttrTypeID> AttrTypeOfParamType (API_AddParID t)
{
	switch (t) {
		case APIParT_Mater:				return API_MaterialID;
		case APIParT_LineTyp:			return API_LinetypeID;
		case APIParT_FillPat:			return API_FilltypeID;
		case APIParT_BuildingMaterial:	return API_BuildingMaterialID;
		case APIParT_Profile:			return API_ProfileID;
		default:						return std::nullopt;
	}
}


bool IsValidGdlName (const GS::String& name)
{
	if (name.IsEmpty () || name.GetLength () >= API_NameLen)
		return false;
	for (UIndex i = 0; i < name.GetLength (); ++i) {
		const char c = name[i];
		const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '~';
		const bool digit = c >= '0' && c <= '9';
		if (!(letter || (digit && i > 0)))
			return false;
	}
	return true;
}


// Numeric value of a JSON scalar for a parameter type (angles in degrees -> radians, attribute names -> index).
double ScalarToParamReal (API_AddParID type, const OS& holder, const char* key, const GS::UniString& pname)
{
	if (holder.IsBool (key))
		return GetBool (holder, key) ? 1.0 : 0.0;
	if (IsNumber (holder, key)) {
		const double v = GetDouble (holder, key);
		return type == APIParT_Angle ? DegToRad (v) : v;
	}
	if (holder.IsString (key)) {
		const std::optional<API_AttrTypeID> attrType = AttrTypeOfParamType (type);
		if (attrType.has_value ()) {
			const GS::UniString name = GetString (holder, key);
			const std::optional<API_AttributeIndex> idx = FindAttrByName (*attrType, name);
			if (!idx.has_value ())
				Fail (AttrTypeName (*attrType) + " attribute named '" + name + "' (default of parameter '" + pname + "') not found. Use get_attributes, or pass an index.", APIERR_BADNAME);
			return (double) *idx;
		}
	}
	Fail ("Default value of parameter '" + pname + "' (" + ParamTypeName (type) + ") must be a number" +
		  (type == APIParT_Boolean || type == APIParT_LightSw ? GS::UniString (" or boolean") : GS::UniString ()) + ".");
}


double DefaultRealFor (API_AddParID t)
{
	switch (t) {
		case APIParT_PenCol:
		case APIParT_Mater:
		case APIParT_LineTyp:
		case APIParT_FillPat:
		case APIParT_BuildingMaterial:
		case APIParT_Profile:	return 1.0;
		case APIParT_Intens:	return 100.0;
		default:				return 0.0;
	}
}


void FillArrayValue (API_AddParType& par, const OS& spec, const GS::UniString& pname)
{
	const bool isString = par.typeID == APIParT_CString;
	GS::Array<GS::Array<double>> numRows;
	GS::Array<GS::Array<GS::UniString>> strRows;
	bool twoDim = false;

	if (spec.Contains ("value")) {
		if (!spec.IsList ("value"))
			Fail ("Array parameter '" + pname + "': 'value' must be an array ([..] for 1 dimension, [{values: [..]}, ...] rows for 2 dimensions).");
		const ListKindCounts kinds = CountListKinds (spec, "value");
		if (kinds.objects > 0 && (kinds.numbers > 0 || kinds.strings > 0 || kinds.bools > 0))
			Fail ("Array parameter '" + pname + "': 'value' mixes rows and plain values.");
		if (kinds.objects > 0) {
			twoDim = true;
			const GS::Array<OS> rows = GetObjectArray (spec, "value", true);
			for (const OS& row : rows) {
				if (isString) strRows.Push (GetStringArray (row, "values", true));
				else {
					GS::Array<double> r = GetNumberArray (row, "values", true);
					numRows.Push (r);
				}
			}
		} else if (isString) {
			strRows.Push (GetStringArray (spec, "value", true));
		} else {
			numRows.Push (GetNumberArray (spec, "value", true));
		}
	} else {
		OS dims;
		if (!TryGetObject (spec, "arrayDims", dims))
			Fail ("Array parameter '" + pname + "' needs a 'value' array or 'arrayDims' {dim1, dim2}.");
		const Int32 d1 = GetInt (dims, "dim1");
		const Int32 d2 = GetInt (dims, "dim2", 0);
		if (d1 < 1 || d2 < 0 || d1 * std::max<Int32> (d2, 1) > 100000)
			Fail ("Array parameter '" + pname + "': invalid arrayDims.");
		twoDim = d2 > 0;
		for (Int32 r = 0; r < (twoDim ? d1 : 1); ++r) {
			if (isString) {
				GS::Array<GS::UniString> row;
				for (Int32 c = 0; c < (twoDim ? d2 : d1); ++c) row.Push (GS::UniString ());
				strRows.Push (row);
			} else {
				GS::Array<double> row;
				for (Int32 c = 0; c < (twoDim ? d2 : d1); ++c) row.Push (DefaultRealFor (par.typeID));
				numRows.Push (row);
			}
		}
	}

	const UIndex nRows = isString ? strRows.GetSize () : numRows.GetSize ();
	UIndex nCols = 0;
	for (UIndex r = 0; r < nRows; ++r)
		nCols = std::max<UIndex> (nCols, isString ? strRows[r].GetSize () : numRows[r].GetSize ());
	if (nRows == 0 || nCols == 0)
		Fail ("Array parameter '" + pname + "' must have at least one value.");

	if (twoDim) {
		par.dim1 = (Int32) nRows;
		par.dim2 = (Int32) nCols;
	} else {
		par.dim1 = (Int32) nCols;		// 1-dimensional: dim2 = 0
		par.dim2 = 0;
	}
	const UIndex cells = (UIndex) par.dim1 * (UIndex) std::max<Int32> (par.dim2, 1);

	if (isString) {
		// Layout: dim1 * max(dim2,1) null-terminated UTF-16 strings, row-major.
		GS::Array<GS::UniString> cellsText;
		USize totalChars = 0;
		for (UIndex r = 0; r < nRows; ++r) {
			for (UIndex c = 0; c < nCols; ++c) {
				const GS::UniString s = c < strRows[r].GetSize () ? strRows[r][c] : GS::UniString ();
				if (s.GetLength () >= API_UAddParStrLen)
					Fail ("A value of string array parameter '" + pname + "' is too long (max 255 characters).");
				totalChars += s.GetLength () + 1;
				cellsText.Push (s);
			}
		}
		par.value.array = BMAllocateHandle ((GSSize) (totalChars * sizeof (GS::uchar_t)), ALLOCATE_CLEAR, 0);
		if (par.value.array == nullptr)
			Fail ("Out of memory.", APIERR_MEMFULL);
		GS::uchar_t* dst = reinterpret_cast<GS::uchar_t*> (*par.value.array);
		USize pos = 0;
		for (const GS::UniString& s : cellsText) {
			if (s.GetLength () > 0)
				GS::ucsncpy (dst + pos, s.ToUStr ().Get (), s.GetLength ());
			pos += s.GetLength ();
			dst[pos++] = 0;
		}
	} else {
		par.value.array = BMAllocateHandle ((GSSize) (cells * sizeof (double)), ALLOCATE_CLEAR, 0);
		if (par.value.array == nullptr)
			Fail ("Out of memory.", APIERR_MEMFULL);
		double* dst = reinterpret_cast<double*> (*par.value.array);
		for (UIndex r = 0; r < nRows; ++r) {
			for (UIndex c = 0; c < nCols; ++c) {
				double v = c < numRows[r].GetSize () ? numRows[r][c] : DefaultRealFor (par.typeID);
				if (par.typeID == APIParT_Angle && spec.Contains ("value"))
					v = DegToRad (v);
				dst[r * nCols + c] = v;
			}
		}
	}
}


// Builds the additional parameter handle from the JSON parameter list (validated before ACAPI_LibPart_Create).
void BuildParams (const GS::Array<OS>& specs, bool addZZYZX, double height, ManualParams& out)
{
	GS::Array<GS::UniString> seen;
	bool hasZZYZX = false;
	for (const OS& s : specs) {
		if (s.Contains ("name") && EqualsIgnoreCase (GetString (s, "name"), "ZZYZX"))
			hasZZYZX = true;
	}
	const bool synthZ = addZZYZX && !hasZZYZX;
	const UIndex n = specs.GetSize () + (synthZ ? 1 : 0);
	if (n == 0)
		return;

	out.handle = reinterpret_cast<API_AddParType**> (BMAllocateHandle ((GSSize) (n * sizeof (API_AddParType)), ALLOCATE_CLEAR, 0));
	if (out.handle == nullptr)
		Fail ("Out of memory.", APIERR_MEMFULL);

	UIndex idx = 0;
	if (synthZ) {
		API_AddParType& z = (*out.handle)[idx++];
		z.typeID = APIParT_Length;
		z.typeMod = API_ParSimple;
		CHTruncate ("ZZYZX", z.name, sizeof (z.name));
		GS::ucsncpy (z.uDescname, GS::UniString ("Height").ToUStr ().Get (), API_UAddParDescLen - 1);
		z.value.real = height;
		seen.Push ("ZZYZX");
	}

	Int32 sepCounter = 0;
	for (const OS& s : specs) {
		API_AddParType& par = (*out.handle)[idx++];
		const API_AddParID type = (API_AddParID) ParseNamed (kParamTypes, s, "type");
		par.typeID = type;

		GS::UniString uname = GetString (s, "name", GS::UniString ());
		if (uname.IsEmpty () && (type == APIParT_Separator || type == APIParT_Title))
			uname = GS::UniString (type == APIParT_Separator ? "sep_" : "title_") + GS::ValueToUniString (++sepCounter);
		const GS::String name = ToStr (uname);
		if (!IsValidGdlName (name))
			Fail ("Invalid GDL parameter name '" + uname + "': use 1-31 ASCII letters, digits or '_' (not starting with a digit).");
		if (EqualsIgnoreCase (uname, "A") || EqualsIgnoreCase (uname, "B"))
			Fail ("Do not declare 'A' or 'B' as parameters: they always exist; set their defaults with the top-level 'a' and 'b' fields.");
		for (const GS::UniString& prev : seen) {
			if (EqualsIgnoreCase (prev, uname))
				Fail ("Duplicate GDL parameter name '" + uname + "'.");
		}
		seen.Push (uname);
		CHTruncate (name.ToCStr (), par.name, sizeof (par.name));

		const GS::UniString desc = GetString (s, "description", uname);
		GS::ucsncpy (par.uDescname, desc.ToUStr ().Get (), API_UAddParDescLen - 1);

		unsigned short flags = 0;
		if (GetBool (s, "hidden", false))	flags |= API_ParFlg_Hidden;
		if (GetBool (s, "bold", false))		flags |= API_ParFlg_BoldName;
		if (GetBool (s, "child", false))	flags |= API_ParFlg_Child;
		if (GetBool (s, "unique", false))	flags |= API_ParFlg_Unique;
		par.flags = flags;

		if (type == APIParT_Separator || type == APIParT_Title) {
			par.typeMod = API_ParSimple;
			continue;
		}
		const bool isArray = GetBool (s, "array", false) || (s.Contains ("value") && s.IsList ("value")) || s.Contains ("arrayDims");
		if (isArray) {
			par.typeMod = API_ParArray;
			FillArrayValue (par, s, uname);
			continue;
		}
		par.typeMod = API_ParSimple;
		if (type == APIParT_CString) {
			const GS::UniString v = GetString (s, "value", GS::UniString ());
			if (v.GetLength () >= API_UAddParStrLen)
				Fail ("Default value of string parameter '" + uname + "' is too long (max 255 characters).");
			GS::ucsncpy (par.value.uStr, v.ToUStr ().Get (), API_UAddParStrLen - 1);
		} else if (s.Contains ("value")) {
			par.value.real = ScalarToParamReal (type, s, "value", uname);
		} else {
			par.value.real = DefaultRealFor (type);
		}
	}
}


GS::Array<GS::UniString> OneDimArrayNames (API_AddParType** params)
{
	GS::Array<GS::UniString> names;
	for (Int32 i = 0; i < ParamCount (params); ++i) {
		const API_AddParType& par = (*params)[i];
		if (par.typeMod == API_ParArray && par.dim2 == 0)
			names.Push (GS::UniString (par.name, CC_UTF8));
	}
	return names;
}


const NamedValue kCreateTypes[] = {
	{ "Object",		APILib_ObjectID },
	{ "Lamp",		APILib_LampID },
	{ "Window",		APILib_WindowID },
	{ "Door",		APILib_DoorID },
	{ "Skylight",	APILib_SkylightID },
	{ "Label",		APILib_LabelID },
	{ "Zone",		APILib_RoomID },
};


struct TextSection {
	GSType			type;
	const char*		key;
	GS::UniString	text;
};


GSErrCode AddTextSection (GSType type, const GS::UniString& text)
{
	API_LibPartSection section;
	BNZeroMemory (&section, sizeof (section));
	section.sectType = type;
	GS::UniString str = text;
	GSErrCode err = ACAPI_LibPart_AddSection (&section, nullptr, &str);
	if (err == NoError)
		return NoError;

	// Fallback: stream the text as UTF-8 bytes.
	err = ACAPI_LibPart_NewSection (&section);
	if (err != NoError)
		return err;
	const GS::String utf8 = ToStr (text);
	if (utf8.GetLength () > 0)
		err = ACAPI_LibPart_WriteSection ((Int32) utf8.GetLength (), utf8.ToCStr ());
	const GSErrCode endErr = ACAPI_LibPart_EndSection ();
	return err != NoError ? err : endErr;
}


std::optional<API_LibPart> SearchExactName (const GS::UniString& name, GS::UniString* path, IO::Location* loc)
{
	API_LibPart lp;
	BNZeroMemory (&lp, sizeof (lp));
	GS::ucsncpy (lp.docu_UName, name.ToUStr ().Get (), API_UniLongNameLen - 1);
	const GSErrCode err = ACAPI_LibPart_Search (&lp, false, false);
	if (lp.location != nullptr) {
		delete lp.location;
		lp.location = nullptr;
	}
	Int32 index = 0;
	if (err == NoError && lp.index > 0 && EqualsIgnoreCase (GS::UniString (lp.docu_UName), name)) {
		index = lp.index;
	} else {
		// Fallback: case-insensitive scan (the API search may be restricted by type / placeability).
		for (const LibPartScanItem& it : ScanAllLibParts ()) {
			if (EqualsIgnoreCase (it.name, name)) {
				index = it.index;
				break;
			}
		}
	}
	if (index <= 0)
		return std::nullopt;
	API_LibPart full;
	FetchLibPart (index, full, path, loc);
	return full;
}


OS CreateLibraryPartImpl (const OS& p)
{
	// ---- validate everything before ACAPI_LibPart_Create ----
	GS::UniString name = GetString (p, "name");
	name.Trim ();
	if (name.IsEmpty () || name.GetLength () >= 200)
		Fail ("'name' must be 1-199 characters.");
	if (name.FindFirstIn ("/\\:*?\"<>|") != MaxUIndex)
		Fail ("'name' must not contain any of / \\ : * ? \" < > | (it becomes the file name).");

	const bool typeGiven = p.Contains ("type");
	API_LibTypeID libType = typeGiven ? (API_LibTypeID) ParseNamed (kCreateTypes, p, "type") : APILib_ObjectID;
	GS::UniString parentName;
	const GS::String parentUnId = ResolveParent (p, libType, typeGiven, parentName);

	const bool placeable = GetBool (p, "placeable", true);
	const bool isTemplate = GetBool (p, "template", false);
	const bool overwrite = GetBool (p, "overwrite", false);
	const double a = GetDouble (p, "a", 1.0);
	const double b = GetDouble (p, "b", 1.0);
	if (a <= 0.0 || b <= 0.0)
		Fail ("'a' and 'b' (default sizes in meters) must be positive.");
	const double height = GetDouble (p, "height", 1.0);
	const bool isModelObject = libType == APILib_ObjectID || libType == APILib_LampID;
	const bool addZZYZX = GetBool (p, "addHeightParameter", isModelObject);
	const bool autoHotspots = GetBool (p, "autoHotspots", true);
	GS::UniString folder = GetString (p, "folder", "Claude Objects");
	folder.Trim ();
	if (folder.FindFirstIn ("/\\:*?\"<>|") != MaxUIndex)
		Fail ("'folder' is a single folder name inside the embedded library (no path separators).");

	// Scripts
	GS::Array<TextSection> sections;
	OS scripts;
	TryGetObject (p, "scripts", scripts);
	for (const GS::String& f : scripts.GetFieldNames ()) {
		bool known = false;
		for (const ScriptSection& sec : kScriptSections)
			known = known || f == sec.key;
		if (!known || f == "comment" || f == "keywords")
			Fail ("Unknown script key '" + GS::UniString (f.ToCStr (), CC_UTF8) + "' in 'scripts'. Allowed: masterScript, script2D, script3D, parameterScript, interfaceScript, propertiesScript, forwardMigrationScript, backwardMigrationScript.");
	}
	auto scriptText = [&] (const char* key, const char* def) -> GS::UniString {
		return scripts.Contains (key) ? GetString (scripts, key) : GS::UniString (def);
	};
	if (auto c = OptString (p, "comment"))		sections.Push ({ API_SectComText, "comment", *c });
	if (auto k = OptString (p, "keywords"))		sections.Push ({ API_SectKeywords, "keywords", *k });
	if (auto au = OptString (p, "author"))		sections.Push ({ API_SectCopyright, "author", *au });
	sections.Push ({ API_Sect1DScript, "masterScript", scriptText ("masterScript", "") });
	sections.Push ({ API_Sect3DScript, "script3D", scriptText ("script3D", "") });
	sections.Push ({ API_Sect2DScript, "script2D", scriptText ("script2D", isModelObject ? "PROJECT2 3, 270, 2\n" : "") });
	sections.Push ({ API_SectVLScript, "parameterScript", scriptText ("parameterScript", "") });
	if (scripts.Contains ("interfaceScript"))			sections.Push ({ API_SectUIScript, "interfaceScript", GetString (scripts, "interfaceScript") });
	if (scripts.Contains ("propertiesScript"))			sections.Push ({ API_SectPRScript, "propertiesScript", GetString (scripts, "propertiesScript") });
	if (scripts.Contains ("forwardMigrationScript"))	sections.Push ({ API_SectFWMScript, "forwardMigrationScript", GetString (scripts, "forwardMigrationScript") });
	if (scripts.Contains ("backwardMigrationScript"))	sections.Push ({ API_SectBWMScript, "backwardMigrationScript", GetString (scripts, "backwardMigrationScript") });

	ManualParams params;
	BuildParams (GetObjectArray (p, "parameters", false), addZZYZX, height, params);

	// ---- target location ----
	IO::Location embeddedLoc;
	if (!EmbeddedLibraryLocation (embeddedLoc))
		Fail ("The embedded library of this project is not available (is a project open?).", APIERR_NOLIB);

	GS::UniString existingPath;
	IO::Location existingLoc;
	const std::optional<API_LibPart> existing = SearchExactName (name, &existingPath, &existingLoc);
	IO::Location targetLoc = embeddedLoc;
	GS::String keepUnId;
	if (existing.has_value ()) {
		if (!overwrite)
			Fail ("A library part named '" + name + "' already exists (" + LibTypeName (EffectiveLibType (*existing)) + ", guid " + GS::UniString (existing->ownUnID) +
				  ", " + existingPath + "). Pass overwrite: true to replace it (only parts in the embedded library), or choose another name.", APIERR_NAMEALREADYUSED);
		if (!embeddedLoc.IsAncestorOf (existingLoc))
			Fail ("The existing library part '" + name + "' is in a loaded library outside the embedded library (" + existingPath +
				  ") and cannot be overwritten; choose another name.", APIERR_READONLY);
		targetLoc = existingLoc;
		targetLoc.DeleteLastLocalName ();
		// Keep the main GUID (placed instances stay linked), new revision GUID (content changed).
		GS::UnID unId (existing->ownUnID);
		if (unId.GenerateVariation () == NoError)
			keepUnId = ToStr (unId.ToUniString ());
	} else if (!folder.IsEmpty ()) {
		IO::Location folderLoc = embeddedLoc;
		folderLoc.AppendToLocal (IO::Name (folder));
		const GSErrCode ferr = IO::fileSystem.CreateFolder (folderLoc);
		if (ferr == NoError || ferr == IO::FileSystem::TargetExists)
			targetLoc = folderLoc;
	}

	// ---- create ----
	API_LibPart lp;
	auto prepare = [&] (bool withUnId) {
		BNZeroMemory (&lp, sizeof (lp));
		lp.typeID = libType;
		lp.isTemplate = isTemplate;
		lp.isPlaceable = placeable;
		GS::ucsncpy (lp.docu_UName, name.ToUStr ().Get (), API_UniLongNameLen - 1);
		CHTruncate (parentUnId.ToCStr (), lp.parentUnID, sizeof (lp.parentUnID));
		if (withUnId && !keepUnId.IsEmpty ())
			CHTruncate (keepUnId.ToCStr (), lp.ownUnID, sizeof (lp.ownUnID));
		lp.location = new IO::Location (targetLoc);
	};
	auto releaseLocation = [&] () {
		if (lp.location != nullptr) {
			delete lp.location;
			lp.location = nullptr;
		}
	};
	auto create = [&] () -> GSErrCode {
		if (overwrite)
			ACAPI_Environment (APIEnv_OverwriteLibPartID, reinterpret_cast<void*> (static_cast<GS::IntPtr> (1)));
		const GSErrCode e = ACAPI_LibPart_Create (&lp);
		if (overwrite)
			ACAPI_Environment (APIEnv_OverwriteLibPartID, reinterpret_cast<void*> (static_cast<GS::IntPtr> (0)));
		return e;
	};

	prepare (true);
	GSErrCode err = create ();
	if (err != NoError && err != APIERR_NEEDSUNDOSCOPE && !keepUnId.IsEmpty ()) {
		releaseLocation ();
		prepare (false);
		err = create ();
	}
	if (err != NoError) {
		releaseLocation ();
		Check (err, "ACAPI_LibPart_Create failed for '" + name + "'");
	}

	// From here on ACAPI_LibPart_Save must be called even on errors.
	GS::Array<GS::UniString> oneDimConverted;
	GSErrCode sectErr = NoError;
	GS::UniString sectWhat;
	for (const TextSection& s : sections) {
		if (sectErr != NoError)
			break;
		const GSErrCode e = AddTextSection (s.type, s.text);
		if (e != NoError && s.type != API_SectCopyright) {
			sectErr = e;
			sectWhat = s.key;
		}
	}
	if (sectErr == NoError) {
		API_LibPartSection section;
		BNZeroMemory (&section, sizeof (section));
		section.sectType = API_SectParamDef;
		GSHandle sectionHdl = nullptr;
		double aa = a, bb = b;
		GSErrCode e = ACAPI_LibPart_GetSect_ParamDef (&lp, params.handle, &aa, &bb, nullptr, &sectionHdl);
		if (e != NoError && !OneDimArrayNames (params.handle).IsEmpty ()) {
			// AC26 refuses 1-dimensional array parameters (dim2 = 0) here: store them as N x 1 arrays (same data layout).
			if (sectionHdl != nullptr)
				BMKillHandle (&sectionHdl);
			oneDimConverted = OneDimArrayNames (params.handle);
			for (Int32 i = 0; i < ParamCount (params.handle); ++i) {
				API_AddParType& par = (*params.handle)[i];
				if (par.typeMod == API_ParArray && par.dim2 == 0)
					par.dim2 = 1;
			}
			aa = a;
			bb = b;
			e = ACAPI_LibPart_GetSect_ParamDef (&lp, params.handle, &aa, &bb, nullptr, &sectionHdl);
			if (e != NoError)
				oneDimConverted.Clear ();
		}
		if (e == NoError) {
			API_LibPartDetails details;
			BNZeroMemory (&details, sizeof (details));
			if (libType != APILib_WindowID && libType != APILib_DoorID && libType != APILib_SkylightID) {
				details.object.autoHotspot = autoHotspots;
				details.object.fixSize = GetBool (p, "fixSize", false);
			}
			e = ACAPI_LibPart_SetDetails_ParamDef (&lp, sectionHdl, &details);
			if (e == NoError)
				e = ACAPI_LibPart_AddSection (&section, sectionHdl, nullptr);
		}
		if (sectionHdl != nullptr)
			BMKillHandle (&sectionHdl);
		if (e != NoError) {
			sectErr = e;
			sectWhat = "parameters";
		}
	}

	releaseLocation ();
	const GSErrCode saveErr = ACAPI_LibPart_Save (&lp);
	releaseLocation ();
	if (sectErr != NoError)
		Fail ("Library part '" + name + "' was created but writing its " + sectWhat + " section failed: " + ErrorName (sectErr) +
			  ". Fix the input and call create_library_part again with overwrite: true.", sectErr);
	Check (saveErr, "ACAPI_LibPart_Save failed for '" + name + "'");

	// ---- read back ----
	GS::UniString path;
	IO::Location loc;
	OS out;
	const std::optional<API_LibPart> created = SearchExactName (name, &path, &loc);
	if (created.has_value ()) {
		API_LibPart shown = *created;
		shown.typeID = EffectiveLibType (shown);
		if (shown.typeID == APILib_ObjectID && libType != APILib_ObjectID)
			shown.typeID = libType;
		out.Add ("libraryPart", LibPartToJson (shown));
		out.Add ("location", path);
	} else {
		OS basic;
		basic.Add ("index", (Int32) lp.index);
		basic.Add ("name", name);
		basic.Add ("guid", GS::UniString (lp.ownUnID));
		basic.Add ("type", LibTypeName (libType));
		out.Add ("libraryPart", basic);
		out.Add ("warning", GS::UniString ("The library part was saved but could not be found by name yet; call reload_libraries, then search_library_parts."));
	}
	out.Add ("subtype", parentName);
	out.Add ("overwritten", existing.has_value ());
	if (!oneDimConverted.IsEmpty ()) {
		out.Add ("arrayWarning", GS::UniString ("1-dimensional array parameters (" + JoinNames (oneDimConverted) +
				 ") were stored as N x 1 arrays: address their cells as name[i][1] in the scripts."));
	}
	out.Add ("parameterCount", ParamCount (params.handle));
	out.Add ("next", GS::UniString (isModelObject
		? "Place it with create_objects / create_lamps using libraryPart {guid} (or its name); check it with get_library_part_scripts / get_library_part_details."
		: "Use it as the library part of the matching element type; check it with get_library_part_scripts / get_library_part_details."));
	return out;
}

} // namespace


void RegisterLibraryPartCommands ()
{
	RegisterCommand ("SearchLibraryParts",
		"Finds library parts in the loaded libraries. Input: {query?: case-insensitive substring (all words must match), "
		"type?: 'Object'|'Door'|'Window'|'Lamp'|'Zone'|'Label'|'Skylight'|'Macro'|... or an array, subtypeOf?: library part ref of a subtype, "
		"placeableOnly?: true, embeddedOnly?: false, offset?: 0, limit?: 100}. Output: {libraryParts: [{index, name, guid, type, fileName, subtype, isPlaceable?, isTemplate?}], total, offset, hasMore}. "
		"Names are localized.",
		[] (const OS& params) -> OS { return SearchLibraryParts (params); });

	RegisterCommand ("GetLibraryPartSubtypes",
		"Lists library part subtypes (categories / templates, i.e. parents in the ancestry tree) with their path, parent and number of placeable parts. "
		"Input: {query?, type?, offset?, limit?: 300}. Use a subtype's guid as subtypeOf in SearchLibraryParts or as subtype in CreateLibraryPart.",
		[] (const OS& params) -> OS { return GetLibraryPartSubtypes (params); });

	RegisterCommand ("GetLibraryPartDetails",
		"Details of library parts: identity, file location, library, ancestry/subtype, creator tool, section list, comment, keywords, "
		"default A/B/ZZYZX and default GDL parameters. Input: {libraryParts: [{name}|{guid}|{index}], includeParameters?: true, "
		"includeHidden?: false, includeValueLists?: false, parameterNames?: [..]}. Output: {libraryParts: [.. | {error}]}.",
		[] (const OS& params) -> OS {
			const GS::Array<OS> refs = GetObjectArray (params, "libraryParts");
			const bool includeParameters = GetBool (params, "includeParameters", true);
			const bool includeHidden = GetBool (params, "includeHidden", false);
			const bool includeValueLists = GetBool (params, "includeValueLists", false);
			const GS::Array<GS::UniString> names = GetStringArray (params, "parameterNames");
			Ancestry anc;
			GS::Array<API_LibraryInfo> libs;
			ACAPI_Environment (APIEnv_GetLibrariesID, &libs, nullptr);
			GS::Array<OS> out;
			for (const OS& ref : refs)
				out.Push (Try ([&] () { return LibraryPartDetails (ref, includeParameters, includeHidden, includeValueLists, names, anc, libs); }));
			return OS ("libraryParts", out);
		});

	RegisterCommand ("GetLibraryPartScripts",
		"Returns the GDL script texts of a library part. Input: {libraryPart, scripts?: ['masterScript','script2D','script3D','parameterScript',"
		"'interfaceScript','propertiesScript','forwardMigrationScript','backwardMigrationScript','comment','keywords'] (default: the 6 main scripts), "
		"maxLength?: 60000 characters per script}. Output: {libraryPart, scripts: {key: text}, lengths, missing?, truncated?, errors?}.",
		[] (const OS& params) -> OS { return GetLibraryPartScripts (params); });

	RegisterCommand ("CreateLibraryPart",
		"Creates (or with overwrite: true replaces) a GDL library part in the project's embedded library from scripts and a parameter list. "
		"Input: {name, type?: 'Object'|'Lamp'|'Window'|'Door'|'Skylight'|'Label'|'Zone', subtype?: keyword|library part ref, "
		"scripts?: {masterScript, script2D, script3D, parameterScript, interfaceScript, propertiesScript}, "
		"parameters?: [{name, type, description?, value?, hidden?, bold?, child?, unique?, array?, arrayDims?}], a?: 1, b?: 1, height?: 1, "
		"addHeightParameter?, autoHotspots?: true, fixSize?: false, placeable?: true, template?: false, comment?, keywords?, author?, "
		"folder?: 'Claude Objects', overwrite?: false}. Output: {libraryPart, location, subtype, overwritten}.",
		[] (const OS& params) -> OS {
			OS out;
			RunWithUndoFallback ("Create library part (Claude)", [&] () { out = CreateLibraryPartImpl (params); });
			return out;
		});
}

} // namespace objlib
} // namespace cc


// =============================================================================
// 3. Libraries (Library Manager)
// =============================================================================

namespace cc {
namespace objlib {

namespace {

GS::UniString LibraryTypeName (API_LibraryTypeID t)
{
	switch (t) {
		case API_LocalLibrary:		return "Local";
		case API_UrlLibrary:		return "Url";
		case API_BuiltInLibrary:	return "BuiltIn";
		case API_EmbeddedLibrary:	return "Embedded";
		case API_OtherObject:		return "OtherObject";
		case API_UrlOtherObject:	return "UrlOtherObject";
		case API_ServerLibrary:		return "Server";
		default:					return "Undefined";
	}
}


GS::Array<API_LibraryInfo> CurrentLibraries (Int32* embeddedIndex)
{
	GS::Array<API_LibraryInfo> libs;
	Int32 embedded = -1;
	Check (ACAPI_Environment (APIEnv_GetLibrariesID, &libs, &embedded), "Cannot read the library list");
	if (embeddedIndex != nullptr)
		*embeddedIndex = embedded;
	return libs;
}


Int32 LibPartCount ()
{
	Int32 count = 0;
	ACAPI_LibPart_GetNum (&count);
	return count;
}


GS::UniString NormalizePath (GS::UniString path)
{
	path.Trim ();
	if (path == "~" || path.BeginsWith ("~/")) {
		const char* home = std::getenv ("HOME");
		if (home != nullptr)
			path = GS::UniString (home, CC_UTF8) + path.GetSubstring (1, path.GetLength () - 1);
	}
	while (path.GetLength () > 1 && path.EndsWith ('/'))
		path.DeleteLast ();
	return path;
}


bool SamePath (const IO::Location& loc, const GS::UniString& normalizedPath)
{
	return EqualsIgnoreCase (NormalizePath (LocationPath (loc)), normalizedPath);
}


GS::Array<OS> LibrariesJson ()
{
	Int32 embedded = -1;
	const GS::Array<API_LibraryInfo> libs = CurrentLibraries (&embedded);
	GS::Array<OS> list;
	for (UIndex i = 0; i < libs.GetSize (); ++i) {
		const API_LibraryInfo& lib = libs[i];
		OS o;
		o.Add ("name", lib.name);
		o.Add ("path", LocationPath (lib.location));
		o.Add ("type", LibraryTypeName (lib.libraryType));
		o.Add ("available", lib.available);
		o.Add ("readOnly", lib.readOnly);
		if ((Int32) i == embedded || lib.libraryType == API_EmbeddedLibrary)
			o.Add ("embedded", true);
		if (!lib.twServerUrl.IsEmpty ())
			o.Add ("serverUrl", lib.twServerUrl);
		if (!lib.urlWebLibrary.IsEmpty ())
			o.Add ("url", lib.urlWebLibrary);
		list.Push (o);
	}
	return list;
}


void SetLibraries (const GS::Array<API_LibraryInfo>& libs, const GS::UniString& undoName)
{
	RunWithUndoFallback (undoName, [&] () {
		Check (ACAPI_Environment (APIEnv_SetLibrariesID, const_cast<GS::Array<API_LibraryInfo>*> (&libs)),
			   "Cannot change the library list (in Teamwork projects the libraries must be managed by the server/owner)");
	});
}


OS AddLibraries (const OS& params)
{
	const GS::Array<GS::UniString> paths = GetStringArray (params, "paths", true);
	if (paths.IsEmpty ())
		Fail ("'paths' must contain at least one absolute folder (or .lcf file) path.");
	GS::Array<API_LibraryInfo> libs = CurrentLibraries (nullptr);
	GS::Array<OS> results;
	bool changed = false;
	for (const GS::UniString& raw : paths) {
		results.Push (Try ([&] () -> OS {
			const GS::UniString path = NormalizePath (raw);
			if (!path.BeginsWith ("/"))
				Fail ("Library path must be absolute (e.g. /Users/me/Library Objects): '" + raw + "'.");
			IO::Location loc (path);
			if (loc.GetStatus () != NoError)
				Fail ("Invalid path '" + path + "'.");
			bool exists = false;
			if (IO::fileSystem.Contains (loc, &exists) != NoError || !exists)
				Fail ("Folder or file not found: '" + path + "'.", APIERR_BADNAME);
			IO::FileSystem::EntryType entryType = IO::FileSystem::Folder;
			IO::fileSystem.GetType (loc, &entryType);
			if (entryType == IO::FileSystem::File && !path.ToLowerCase ().EndsWith (".lcf"))
				Fail ("'" + path + "' is a file: a library must be a folder or a .lcf library container.");
			for (const API_LibraryInfo& lib : libs) {
				if (lib.location == loc || SamePath (lib.location, path))
					return OS ("path", path, "status", GS::UniString ("alreadyLoaded"));
			}
			API_LibraryInfo info;
			info.location = loc;
			IO::Name last;
			if (loc.GetLastLocalName (&last) == NoError)
				info.name = last.ToString ();
			info.libraryType = API_LocalLibrary;
			info.available = true;
			info.readOnly = false;
			info.filler[0] = info.filler[1] = false;
			libs.Push (info);
			changed = true;
			return OS ("path", path, "status", GS::UniString ("added"));
		}));
	}
	if (changed)
		SetLibraries (libs, "Add libraries (Claude)");
	OS out;
	out.Add ("results", results);
	out.Add ("libraries", LibrariesJson ());
	out.Add ("libraryPartCount", LibPartCount ());
	return out;
}


OS RemoveLibraries (const OS& params)
{
	const GS::Array<GS::UniString> refs = GetStringArray (params, "libraries", true);
	if (refs.IsEmpty ())
		Fail ("'libraries' must contain at least one library path or name (see get_libraries).");
	const GS::Array<API_LibraryInfo> libs = CurrentLibraries (nullptr);
	GS::Array<bool> remove;
	for (UIndex i = 0; i < libs.GetSize (); ++i)
		remove.Push (false);

	GS::Array<OS> results;
	bool changed = false;
	for (const GS::UniString& ref : refs) {
		results.Push (Try ([&] () -> OS {
			const GS::UniString path = NormalizePath (ref);
			GS::Array<UIndex> byPath, byName;
			for (UIndex i = 0; i < libs.GetSize (); ++i) {
				if (SamePath (libs[i].location, path))
					byPath.Push (i);
				else if (EqualsIgnoreCase (libs[i].name, ref))
					byName.Push (i);
			}
			const GS::Array<UIndex>& matches = byPath.IsEmpty () ? byName : byPath;
			if (matches.IsEmpty ())
				Fail ("No loaded library matches '" + ref + "'. Use get_libraries to list the names and paths.", APIERR_BADNAME);
			if (matches.GetSize () > 1)
				Fail ("Several libraries are named '" + ref + "'; pass the full path instead.", APIERR_BADNAME);
			const API_LibraryInfo& lib = libs[matches[0]];
			if (lib.libraryType == API_EmbeddedLibrary || lib.libraryType == API_BuiltInLibrary)
				Fail ("The " + LibraryTypeName (lib.libraryType) + " library cannot be removed.", APIERR_REFUSEDPAR);
			remove[matches[0]] = true;
			changed = true;
			return OS ("library", ref, "path", LocationPath (lib.location), "status", GS::UniString ("removed"));
		}));
	}
	if (changed) {
		GS::Array<API_LibraryInfo> kept;
		for (UIndex i = 0; i < libs.GetSize (); ++i) {
			if (!remove[i])
				kept.Push (libs[i]);
		}
		SetLibraries (kept, "Remove libraries (Claude)");
	}
	OS out;
	out.Add ("results", results);
	out.Add ("libraries", LibrariesJson ());
	out.Add ("libraryPartCount", LibPartCount ());
	return out;
}

} // namespace


void RegisterLibraryCommands ()
{
	RegisterCommand ("GetLibraries",
		"Lists the libraries loaded in the project (Library Manager): {libraries: [{name, path, type: Local|Embedded|BuiltIn|Server|Url|..., "
		"available, readOnly, embedded?}], libraryPartCount}.",
		[] (const OS&) -> OS {
			OS out;
			out.Add ("libraries", LibrariesJson ());
			out.Add ("libraryPartCount", LibPartCount ());
			return out;
		});

	RegisterCommand ("AddLibraries",
		"Adds local libraries (absolute folder paths or .lcf container files; '~/' is expanded) to the project and loads them. "
		"Input: {paths: [..]}. Output: {results: [{path, status: added|alreadyLoaded} | {error}], libraries, libraryPartCount}. Not undoable.",
		[] (const OS& params) -> OS { return AddLibraries (params); });

	RegisterCommand ("RemoveLibraries",
		"Removes libraries from the project (by path or name, see GetLibraries). The embedded and built-in libraries cannot be removed. "
		"Placed elements of removed parts become 'missing'. Input: {libraries: [..]}. Output: {results, libraries, libraryPartCount}. Not undoable.",
		[] (const OS& params) -> OS { return RemoveLibraries (params); });

	RegisterCommand ("ReloadLibraries",
		"Reloads all libraries (picks up library part files changed on disk). Output: {ok, libraryPartCount}.",
		[] (const OS&) -> OS {
			RunWithUndoFallback ("Reload libraries (Claude)", [] () {
				Check (ACAPI_Automate (APIDo_ReloadLibrariesID), "Reloading the libraries failed");
			});
			return OS ("ok", true, "libraryPartCount", LibPartCount ());
		});
}

} // namespace objlib
} // namespace cc


// =============================================================================
// 4. Object / Lamp adapters, GDL parameters of placed elements, registration
// =============================================================================

namespace cc {
namespace objlib {

namespace {

constexpr double kSizeEps = 1e-9;

// --- GDL-backed fields (sizes, height, lamp light, params) -----------------------------

bool HasGdlFields (const OS& spec, bool isLamp)
{
	if (spec.Contains ("sizeA") || spec.Contains ("sizeB") || spec.Contains ("height") || spec.Contains ("params"))
		return true;
	return isLamp && (spec.Contains ("lightOn") || spec.Contains ("lightColor") || spec.Contains ("lightIntensity"));
}


GS::Array<GS::UniString> ParamNamesOfType (API_AddParType** defs, API_AddParID type)
{
	GS::Array<GS::UniString> names;
	const Int32 n = ParamCount (defs);
	for (Int32 i = 0; i < n; ++i) {
		const API_AddParType& p = (*defs)[i];
		if (p.typeID == type && p.typeMod != API_ParArray)
			names.Push (GS::UniString (p.name, CC_UTF8));
	}
	return names;
}


double Clamp01 (double v)
{
	return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}


void ApplyLightFields (ParamEditor& ed, const OS& spec)
{
	if (auto on = OptBool (spec, "lightOn")) {
		GS::Array<GS::UniString> names = ParamNamesOfType (ed.Definitions (), APIParT_LightSw);
		if (names.IsEmpty ())
			Fail ("This lamp library part has no Light Switch parameter; switch it with its own GDL parameter (see get_gdl_parameters).", APIERR_BADNAME);
		ed.SetNumber (names[0], *on ? 1.0 : 0.0, true);
	}
	OS color;
	if (TryGetObject (spec, "lightColor", color)) {
		GS::Array<GS::UniString> names = ParamNamesOfType (ed.Definitions (), APIParT_ColRGB);
		if (names.GetSize () < 3)
			Fail ("This lamp library part has no RGB light color parameters; set its color with params (see get_gdl_parameters).", APIERR_BADNAME);
		ed.SetNumber (names[0], Clamp01 (GetDouble (color, "red")), true);
		ed.SetNumber (names[1], Clamp01 (GetDouble (color, "green")), true);
		ed.SetNumber (names[2], Clamp01 (GetDouble (color, "blue")), true);
	}
	if (auto v = OptDouble (spec, "lightIntensity")) {
		GS::Array<GS::UniString> names = ParamNamesOfType (ed.Definitions (), APIParT_Intens);
		if (names.IsEmpty ())
			Fail ("This lamp library part has no Intensity parameter; set its brightness with params (see get_gdl_parameters).", APIERR_BADNAME);
		ed.SetNumber (names[0], *v, true);
	}
}


// Applies sizeA / sizeB / height / lamp fields / params (in this order; explicit params win).
void ApplyGdlFields (ParamEditor& ed, const OS& spec, bool isLamp)
{
	if (auto v = OptDouble (spec, "sizeA")) {
		if (*v <= 0.0)
			Fail ("sizeA must be positive (meters).");
		ed.SetNumber ("A", *v, true);
	}
	if (auto v = OptDouble (spec, "sizeB")) {
		if (*v <= 0.0)
			Fail ("sizeB must be positive (meters).");
		ed.SetNumber ("B", *v, true);
	}
	if (auto v = OptDouble (spec, "height")) {
		if (!ed.HasParam ("ZZYZX"))
			Fail ("This library part has no ZZYZX (height) parameter; its height is controlled by other GDL parameters (see get_gdl_parameters).", APIERR_BADNAME);
		ed.SetNumber ("ZZYZX", *v, true);
	}
	if (isLamp)
		ApplyLightFields (ed, spec);
	OS params;
	if (TryGetObject (spec, "params", params))
		ed.Apply (params);
}

// --- Plain struct fields ------------------------------------------------------------------

short GetPen (const OS& spec, const char* key, Int32 minPen)
{
	const Int32 pen = GetInt (spec, key);
	if (pen < minPen || pen > 255)
		Fail ("Field '" + GS::UniString (key) + "' must be a pen index " + GS::ValueToUniString (minPen) + "-255.");
	return (short) pen;
}


void ApplyStoryVisibility (API_ObjectType& obj, API_Element* mask, const OS& spec)
{
	if (!spec.Contains ("showOnStories"))
		return;
	API_StoryVisibility& vis = obj.visibility;
	if (spec.IsString ("showOnStories")) {
		const GS::UniString mode = GetString (spec, "showOnStories");
		if (EqualsIgnoreCase (mode, "AllRelevant")) {
			obj.isAutoOnStoryVisibility = true;
		} else if (EqualsIgnoreCase (mode, "HomeOnly")) {
			obj.isAutoOnStoryVisibility = false;
			vis.showOnHome = true;
			vis.showAllAbove = false;
			vis.showAllBelow = false;
			vis.showRelAbove = 0;
			vis.showRelBelow = 0;
		} else if (EqualsIgnoreCase (mode, "AllStories")) {
			obj.isAutoOnStoryVisibility = false;
			vis.showOnHome = true;
			vis.showAllAbove = true;
			vis.showAllBelow = true;
		} else {
			Fail ("showOnStories must be 'HomeOnly', 'AllRelevant', 'AllStories' or {home, allAbove, allBelow, above, below}.");
		}
	} else {
		OS v = GetObject (spec, "showOnStories");
		obj.isAutoOnStoryVisibility = false;
		if (auto b = OptBool (v, "home"))		vis.showOnHome = *b;
		if (auto b = OptBool (v, "allAbove"))	vis.showAllAbove = *b;
		if (auto b = OptBool (v, "allBelow"))	vis.showAllBelow = *b;
		if (auto n = OptInt (v, "above"))		vis.showRelAbove = (short) std::max<Int32> (0, *n);
		if (auto n = OptInt (v, "below"))		vis.showRelBelow = (short) std::max<Int32> (0, *n);
	}
	if (mask != nullptr) {
		ACAPI_ELEMENT_MASK_SET (*mask, API_ObjectType, isAutoOnStoryVisibility);
		ACAPI_ELEMENT_MASK_SET (*mask, API_ObjectType, visibility.showOnHome);
		ACAPI_ELEMENT_MASK_SET (*mask, API_ObjectType, visibility.showAllAbove);
		ACAPI_ELEMENT_MASK_SET (*mask, API_ObjectType, visibility.showAllBelow);
		ACAPI_ELEMENT_MASK_SET (*mask, API_ObjectType, visibility.showRelAbove);
		ACAPI_ELEMENT_MASK_SET (*mask, API_ObjectType, visibility.showRelBelow);
	}
}


// Struct fields of objects and lamps; mask == nullptr on create.
void ApplyObjectFields (API_ObjectType& obj, API_Element* mask, const OS& spec, bool isLamp)
{
#define OBJ_SET(field) if (mask) ACAPI_ELEMENT_MASK_SET (*mask, API_ObjectType, field)
	if (auto c = OptCoord (spec, "position"))		{ obj.pos = *c; OBJ_SET (pos); }
	if (auto v = OptDouble (spec, "elevation"))	{ obj.level = *v; OBJ_SET (level); }
	if (auto v = OptAngle (spec, "angle"))			{ obj.angle = *v; OBJ_SET (angle); }
	if (auto v = OptBool (spec, "mirrored"))		{ obj.reflected = *v; OBJ_SET (reflected); }

	if (spec.Contains ("pen")) {
		obj.pen = GetPen (spec, "pen", 1); OBJ_SET (pen);
		if (!spec.Contains ("useObjectPens")) { obj.useObjPens = false; OBJ_SET (useObjPens); }
	}
	if (auto v = OptBool (spec, "useObjectPens"))	{ obj.useObjPens = *v; OBJ_SET (useObjPens); }

	if (auto lt = OptAttr (API_LinetypeID, spec, "lineType")) {
		obj.ltypeInd = *lt; OBJ_SET (ltypeInd);
		if (!spec.Contains ("useObjectLineTypes")) { obj.useObjLtypes = false; OBJ_SET (useObjLtypes); }
	}
	if (auto v = OptBool (spec, "useObjectLineTypes"))	{ obj.useObjLtypes = *v; OBJ_SET (useObjLtypes); }

	if (spec.Contains ("overrideSurface")) {
		if (spec.IsBool ("overrideSurface")) {
			if (GetBool (spec, "overrideSurface"))
				Fail ("overrideSurface must be a surface name/index (override all surfaces), or false (use the object's own surfaces).");
			obj.useObjMaterials = true;
		} else {
			obj.mat = GetAttr (API_MaterialID, spec, "overrideSurface"); OBJ_SET (mat);
			obj.useObjMaterials = false;
		}
		OBJ_SET (useObjMaterials);
	}

	bool sectionGiven = false;
	if (auto f = OptAttr (API_FilltypeID, spec, "sectionFill"))	{ obj.sectFill = *f; OBJ_SET (sectFill); sectionGiven = true; }
	if (spec.Contains ("sectionFillPen"))		{ obj.sectFillPen = GetPen (spec, "sectionFillPen", 1); OBJ_SET (sectFillPen); sectionGiven = true; }
	if (spec.Contains ("sectionBackgroundPen"))	{ obj.sectBGPen = GetPen (spec, "sectionBackgroundPen", 0); OBJ_SET (sectBGPen); sectionGiven = true; }
	if (spec.Contains ("sectionContourPen"))	{ obj.sectContPen = GetPen (spec, "sectionContourPen", 1); OBJ_SET (sectContPen); sectionGiven = true; }
	if (auto v = OptBool (spec, "useObjectSectionAttributes")) {
		obj.useObjSectAttrs = *v; OBJ_SET (useObjSectAttrs);
	} else if (sectionGiven) {
		obj.useObjSectAttrs = false; OBJ_SET (useObjSectAttrs);
	}

	ApplyStoryVisibility (obj, mask, spec);

	if (isLamp) {
		if (auto v = OptBool (spec, "lightOn"))	{ obj.lightIsOn = *v; OBJ_SET (lightIsOn); }
		OS color;
		if (TryGetObject (spec, "lightColor", color)) {
			obj.lightColor.f_red = Clamp01 (GetDouble (color, "red"));
			obj.lightColor.f_green = Clamp01 (GetDouble (color, "green"));
			obj.lightColor.f_blue = Clamp01 (GetDouble (color, "blue"));
			OBJ_SET (lightColor.f_red); OBJ_SET (lightColor.f_green); OBJ_SET (lightColor.f_blue);
		}
	}
#undef OBJ_SET
}

// --- Library part swap (shared by the adapters and ChangeLibraryPart) ------------------------

struct SwapResult {
	API_AddParType**	params = nullptr;		// caller owns
	double				a = 0.0;
	double				b = 0.0;
	UInt32				carried = 0;
};


SwapResult BuildSwappedParams (const API_Element& element, Int32 newLibInd, bool keepParameters, bool keepSize,
							   double curA, double curB, const std::function<void (ParamEditor&)>& applyExtra)
{
	ParamsHandle oldParams;
	if (keepParameters) {
		Memo m;
		if (ACAPI_Element_GetMemo (element.header.guid, m.Ptr (), APIMemoMask_AddPars) == NoError && m->params != nullptr) {
			oldParams.Reset (m->params);
			m->params = nullptr;
		}
	}

	SwapResult r;
	ParamEditor ed (LibPartOwner (newLibInd, element.header.type.typeID));
	if (keepParameters && oldParams.Get () != nullptr) {
		GS::Array<GS::UniString> skip;
		skip.Push ("A");
		skip.Push ("B");
		r.carried = ed.CarryOver (oldParams.Get (), skip);
	}
	if (keepSize) {
		if (curA > 0.0) ed.SetNumber ("A", curA, false);
		if (curB > 0.0) ed.SetNumber ("B", curB, false);
	}
	if (applyExtra)
		applyExtra (ed);
	r.params = ed.Finish (&r.a, &r.b);
	return r;
}


void SyncSize (API_Element& element, API_Element& mask, double a, double b)
{
	switch (element.header.type.typeID) {
		case API_ObjectID:
		case API_LampID:
			if (a > 0.0 && std::fabs (a - element.object.xRatio) > kSizeEps) { element.object.xRatio = a; ACAPI_ELEMENT_MASK_SET (mask, API_ObjectType, xRatio); }
			if (b > 0.0 && std::fabs (b - element.object.yRatio) > kSizeEps) { element.object.yRatio = b; ACAPI_ELEMENT_MASK_SET (mask, API_ObjectType, yRatio); }
			break;
		case API_WindowID:
			if (a > 0.0 && std::fabs (a - element.window.openingBase.width) > kSizeEps) { element.window.openingBase.width = a; ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, openingBase.width); }
			if (b > 0.0 && std::fabs (b - element.window.openingBase.height) > kSizeEps) { element.window.openingBase.height = b; ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, openingBase.height); }
			break;
		case API_DoorID:
			if (a > 0.0 && std::fabs (a - element.door.openingBase.width) > kSizeEps) { element.door.openingBase.width = a; ACAPI_ELEMENT_MASK_SET (mask, API_DoorType, openingBase.width); }
			if (b > 0.0 && std::fabs (b - element.door.openingBase.height) > kSizeEps) { element.door.openingBase.height = b; ACAPI_ELEMENT_MASK_SET (mask, API_DoorType, openingBase.height); }
			break;
		case API_SkylightID:
			if (a > 0.0 && std::fabs (a - element.skylight.openingBase.width) > kSizeEps) { element.skylight.openingBase.width = a; ACAPI_ELEMENT_MASK_SET (mask, API_SkylightType, openingBase.width); }
			if (b > 0.0 && std::fabs (b - element.skylight.openingBase.height) > kSizeEps) { element.skylight.openingBase.height = b; ACAPI_ELEMENT_MASK_SET (mask, API_SkylightType, openingBase.height); }
			break;
		default:
			break;
	}
}

// --- Object / Lamp adapter ---------------------------------------------------------------------

API_Guid CreateObjectLike (const OS& spec, API_ElemTypeID typeID)
{
	const bool isLamp = typeID == API_LampID;
	if (!spec.Contains ("position"))
		Fail (ElemTypeName (typeID) + " requires 'position' {x, y} (meters, project coordinates).");

	API_Element element = NewElement (typeID);
	Memo memo;
	GetDefaults (element, memo.Ptr ());
	ApplyCommonFields (element, nullptr, spec);

	Int32 libInd = element.object.libInd;
	const bool explicitLib = spec.Contains ("libraryPart");
	if (explicitLib)
		libInd = FindLibPartForElem (spec, "libraryPart", typeID).index;
	if (libInd <= 0)
		Fail ("No library part given and the " + ElemTypeName (typeID) + " tool has no default library part. Pass 'libraryPart' (find one with search_library_parts).");

	if (explicitLib || HasGdlFields (spec, isLamp)) {
		double a = 0.0, b = 0.0;
		API_AddParType** newParams = nullptr;
		{
			ParamEditor ed (LibPartOwner (libInd, typeID));
			ApplyGdlFields (ed, spec, isLamp);
			newParams = ed.Finish (&a, &b);
		}
		ReplaceMemoParams (*memo, newParams);
		if (a > 0.0) element.object.xRatio = a;
		if (b > 0.0) element.object.yRatio = b;
	}
	element.object.libInd = libInd;
	ApplyObjectFields (element.object, nullptr, spec, isLamp);

	Check (ACAPI_Element_Create (&element, memo.Ptr ()), "Cannot create " + ElemTypeName (typeID));
	return element.header.guid;
}


API_Guid CreateObject (const OS& spec)	{ return CreateObjectLike (spec, API_ObjectID); }
API_Guid CreateLamp (const OS& spec)	{ return CreateObjectLike (spec, API_LampID); }


void SerializeObjectLike (const API_Element& element, OS& out)
{
	const API_ObjectType& o = element.object;
	const bool isLamp = element.header.type.typeID == API_LampID;

	out.Add ("libraryPart", LibPartRefJson (o.libInd));
	out.Add ("position", CoordObj (o.pos));
	out.Add ("elevation", o.level);
	out.Add ("absoluteElevation", StoryLevel (element.header.floorInd) + o.level);
	AddAngle (out, "angle", o.angle);
	out.Add ("mirrored", o.reflected);
	out.Add ("sizeA", o.xRatio);
	out.Add ("sizeB", o.yRatio);

	Memo memo;
	if (ACAPI_Element_GetMemo (element.header.guid, memo.Ptr (), APIMemoMask_AddPars) == NoError && memo->params != nullptr) {
		const API_AddParType* zzyzx = FindParamByName (memo->params, "ZZYZX");
		if (zzyzx != nullptr && zzyzx->typeMod != API_ParArray && zzyzx->typeID != APIParT_CString)
			out.Add ("height", zzyzx->value.real);
		if (isLamp) {
			GS::Array<GS::UniString> sw = ParamNamesOfType (memo->params, APIParT_LightSw);
			if (!sw.IsEmpty ()) {
				const API_AddParType* p = FindParamByName (memo->params, sw[0]);
				if (p != nullptr)
					out.Add ("lightSwitchParam", OS ("name", sw[0], "on", p->value.real != 0.0));
			}
		}
		const UInt32 kSummaryMax = 30;
		Int32 shown = 0;
		out.Add ("params", ParamsSummary (memo->params, kSummaryMax, &shown));
		out.Add ("paramCount", ParamCount (memo->params));
		if (shown >= (Int32) kSummaryMax)
			out.Add ("paramsNote", GS::UniString ("First 30 visible parameters only; use get_gdl_parameters for all (with types, descriptions and value lists)."));
	}

	out.Add ("pen", (Int32) o.pen);
	out.Add ("useObjectPens", o.useObjPens);
	out.Add ("lineType", AttrRef (API_LinetypeID, o.ltypeInd));
	out.Add ("useObjectLineTypes", o.useObjLtypes);
	if (!o.useObjMaterials)
		out.Add ("overrideSurface", AttrRef (API_MaterialID, o.mat));
	out.Add ("useObjectSectionAttributes", o.useObjSectAttrs);
	if (!o.useObjSectAttrs) {
		out.Add ("sectionFill", AttrRef (API_FilltypeID, o.sectFill));
		out.Add ("sectionFillPen", (Int32) o.sectFillPen);
		out.Add ("sectionBackgroundPen", (Int32) o.sectBGPen);
		out.Add ("sectionContourPen", (Int32) o.sectContPen);
	}

	if (o.isAutoOnStoryVisibility) {
		out.Add ("showOnStories", GS::UniString ("AllRelevant"));
	} else if (o.visibility.showOnHome && !o.visibility.showAllAbove && !o.visibility.showAllBelow &&
			   o.visibility.showRelAbove == 0 && o.visibility.showRelBelow == 0) {
		out.Add ("showOnStories", GS::UniString ("HomeOnly"));
	} else {
		out.Add ("showOnStories", OS ("home", o.visibility.showOnHome, "allAbove", o.visibility.showAllAbove,
									  "allBelow", o.visibility.showAllBelow,
									  "above", (Int32) o.visibility.showRelAbove, "below", (Int32) o.visibility.showRelBelow));
	}

	if (o.owner != APINULLGuid)
		out.Add ("owner", OS ("guid", GuidStr (o.owner), "type", ElemTypeName (o.ownerType)));
	if (std::fabs (o.offset.x) > kSizeEps || std::fabs (o.offset.y) > kSizeEps)
		out.Add ("originOffset", CoordObj (o.offset));

	if (isLamp) {
		out.Add ("lightOn", o.lightIsOn);
		out.Add ("lightColor", OS ("red", o.lightColor.f_red, "green", o.lightColor.f_green, "blue", o.lightColor.f_blue));
	}
}


void ModifyObjectLike (API_Element& element, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask, const OS& patch)
{
	const API_ElemTypeID typeID = element.header.type.typeID;
	const bool isLamp = typeID == API_LampID;

	if (patch.Contains ("libraryPart")) {
		const API_LibPart lp = FindLibPartForElem (patch, "libraryPart", typeID);
		const bool keepParameters = GetBool (patch, "keepParameters", true);
		const bool keepSize = GetBool (patch, "keepSize", false);
		SwapResult r = BuildSwappedParams (element, lp.index, keepParameters, keepSize, element.object.xRatio, element.object.yRatio,
										   [&] (ParamEditor& ed) { ApplyGdlFields (ed, patch, isLamp); });
		element.object.libInd = lp.index;
		ACAPI_ELEMENT_MASK_SET (mask, API_ObjectType, libInd);
		ReplaceMemoParams (memo, r.params);
		memoMask |= APIMemoMask_AddPars;
		SyncSize (element, mask, r.a, r.b);
	} else if (HasGdlFields (patch, isLamp)) {
		double a = 0.0, b = 0.0;
		API_AddParType** newParams = nullptr;
		{
			ParamEditor ed (ElementOwner (element));
			ApplyGdlFields (ed, patch, isLamp);
			newParams = ed.Finish (&a, &b);
		}
		ReplaceMemoParams (memo, newParams);
		memoMask |= APIMemoMask_AddPars;
		SyncSize (element, mask, a, b);
	}
	ApplyObjectFields (element.object, &mask, patch, isLamp);
}

// --- GDL parameter commands ---------------------------------------------------------------------

GS::Array<GS::UniString> FieldNamesOf (const OS& os)
{
	GS::Array<GS::UniString> names;
	for (const GS::String& f : os.GetFieldNames ())
		names.Push (GS::UniString (f.ToCStr (), CC_UTF8));
	return names;
}


// Placed elements' memo parameters carry no descriptions (uDescname is empty): copy them from the library part defaults.
void FillMissingDescriptions (API_AddParType** params, Int32 libInd)
{
	const Int32 n = ParamCount (params);
	bool missing = false;
	for (Int32 i = 0; i < n && !missing; ++i)
		missing = (*params)[i].uDescname[0] == 0 && !IsValueless ((*params)[i].typeID);
	if (!missing || libInd <= 0)
		return;
	ParamsHandle defaults;
	try {
		LoadDefaultParams (libInd, defaults);
	} catch (const Error&) {
		return;
	}
	for (Int32 i = 0; i < n; ++i) {
		API_AddParType& p = (*params)[i];
		if (p.uDescname[0] != 0)
			continue;
		const API_AddParType* d = FindParamByName (defaults.Get (), GS::UniString (p.name, CC_UTF8));
		if (d != nullptr && d->uDescname[0] != 0)
			GS::ucsncpy (p.uDescname, d->uDescname, API_UAddParDescLen - 1);
	}
}


OS GetElementParams (const API_Guid& guid, const GS::Array<GS::UniString>& names, const GS::UniString& search,
					 bool includeHidden, bool includeValueLists)
{
	API_Element element = GetElement (guid);
	Memo memo;
	LoadMemo (guid, *memo, APIMemoMask_AddPars);
	if (memo->params == nullptr)
		Fail (ElemTypeName (element.header.type) + " element " + GuidStr (guid) + " has no GDL parameters (it is not based on a library part).", APIERR_BADELEMENTTYPE);

	OS res ("guid", GuidStr (guid), "type", ElemTypeName (element.header.type));
	const Int32 libInd = ElementLibInd (element);
	if (libInd > 0) {
		res.Add ("libraryPart", LibPartRefJson (libInd));
		FillMissingDescriptions (memo->params, libInd);
	}

	GS::Array<OS> all = AddAttributeNames (memo->params, ParamsToJson (memo->params, includeHidden, names));
	GS::Array<OS> items;
	for (const OS& item : all) {
		if (!search.IsEmpty ()) {
			GS::UniString n, d;
			item.Get ("name", n);
			item.Get ("description", d);
			if (!ContainsNoCase (n, search) && !ContainsNoCase (d, search))
				continue;
		}
		items.Push (item);
	}

	if (!names.IsEmpty ()) {
		GS::Array<GS::UniString> notFound;
		for (const GS::UniString& wanted : names) {
			if (FindParamByName (memo->params, wanted) == nullptr)
				notFound.Push (wanted);
		}
		if (!notFound.IsEmpty ())
			res.Add ("notFound", notFound);
	}

	if (includeValueLists && !items.IsEmpty ()) {
		try {
			ParamEditor ed (ElementOwner (element));
			AddValueLists (ed, memo->params, items);
		} catch (const Error& e) {
			res.Add ("valueListsError", e.message);
		}
	}
	res.Add ("parameterCount", (Int32) items.GetSize ());
	res.Add ("parameters", items);
	return res;
}


OS SetElementParams (const API_Guid& guid, const OS& values)
{
	API_Element element = GetElement (guid);
	if (ElementLibInd (element) <= 0)
		Fail (ElemTypeName (element.header.type) + " element " + GuidStr (guid) + " is not based on a library part (no GDL parameters).", APIERR_BADELEMENTTYPE);
	if (values.IsEmpty ())
		Fail ("'params' is empty: pass {gdlParameterName: value, ...}.");

	double a = 0.0, b = 0.0;
	ParamsHandle result;
	{
		ParamEditor ed (ElementOwner (element));
		ed.Apply (values);
		result.Reset (ed.Finish (&a, &b));
	}

	API_Element mask;
	ACAPI_ELEMENT_MASK_CLEAR (mask);
	SyncSize (element, mask, a, b);

	GS::Array<OS> after = ParamsToJson (result.Get (), true, FieldNamesOf (values));

	Memo memo;
	memo->params = result.Release ();
	Check (ACAPI_Element_Change (&element, &mask, memo.Ptr (), APIMemoMask_AddPars, true),
		   "Cannot change the GDL parameters of element " + GuidStr (guid));
	return OS ("guid", GuidStr (guid), "parameters", after);
}


OS ChangeLibraryPartOf (const OS& item)
{
	const API_Guid guid = GetGuid (item, "guid");
	API_Element element = GetElement (guid);
	const API_ElemTypeID typeID = element.header.type.typeID;
	if (!LibTypeForElemType (typeID).has_value ())
		Fail ("Element " + GuidStr (guid) + " is a " + ElemTypeName (typeID) +
			  "; change_library_part supports Object, Lamp, Window, Door, Skylight, Zone and symbol Label elements.", APIERR_BADELEMENTTYPE);
	if (typeID == API_LabelID && element.label.labelClass != APILblClass_Symbol)
		Fail ("Label " + GuidStr (guid) + " is a text label; only symbol labels have a library part.", APIERR_BADELEMENTTYPE);

	const API_LibPart lp = FindLibPartForElem (item, "libraryPart", typeID);
	const bool isObject = typeID == API_ObjectID || typeID == API_LampID;
	const bool isOpening = typeID == API_WindowID || typeID == API_DoorID || typeID == API_SkylightID;
	const bool keepParameters = GetBool (item, "keepParameters", true);
	const bool keepSize = GetBool (item, "keepSize", isOpening);

	double curA = 0.0, curB = 0.0;
	if (isObject) {
		curA = element.object.xRatio;
		curB = element.object.yRatio;
	} else if (typeID == API_WindowID || typeID == API_DoorID) {
		curA = element.window.openingBase.width;
		curB = element.window.openingBase.height;
	} else if (typeID == API_SkylightID) {
		curA = element.skylight.openingBase.width;
		curB = element.skylight.openingBase.height;
	}

	SwapResult r = BuildSwappedParams (element, lp.index, keepParameters, keepSize, curA, curB, [&] (ParamEditor& ed) {
		if (isObject) {
			ApplyGdlFields (ed, item, typeID == API_LampID);
		} else {
			OS extra;
			if (TryGetObject (item, "params", extra))
				ed.Apply (extra);
		}
	});
	ParamsHandle hold;
	hold.Reset (r.params);

	API_Element mask;
	ACAPI_ELEMENT_MASK_CLEAR (mask);
	switch (typeID) {
		case API_ObjectID:
		case API_LampID:
			element.object.libInd = lp.index;
			ACAPI_ELEMENT_MASK_SET (mask, API_ObjectType, libInd);
			break;
		case API_WindowID:
			element.window.openingBase.libInd = lp.index;
			ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, openingBase.libInd);
			break;
		case API_DoorID:
			element.door.openingBase.libInd = lp.index;
			ACAPI_ELEMENT_MASK_SET (mask, API_DoorType, openingBase.libInd);
			break;
		case API_SkylightID:
			element.skylight.openingBase.libInd = lp.index;
			ACAPI_ELEMENT_MASK_SET (mask, API_SkylightType, openingBase.libInd);
			break;
		case API_ZoneID:
			element.zone.libInd = lp.index;
			ACAPI_ELEMENT_MASK_SET (mask, API_ZoneType, libInd);
			break;
		case API_LabelID:
			element.label.u.symbol.libInd = lp.index;
			ACAPI_ELEMENT_MASK_SET (mask, API_LabelType, u.symbol.libInd);
			break;
		default:
			break;
	}
	SyncSize (element, mask, r.a, r.b);

	Memo memo;
	memo->params = hold.Release ();
	Check (ACAPI_Element_Change (&element, &mask, memo.Ptr (), APIMemoMask_AddPars, true),
		   "Cannot change the library part of element " + GuidStr (guid));

	OS out ("guid", GuidStr (guid), "type", ElemTypeName (typeID));
	out.Add ("libraryPart", LibPartRefJson (lp.index));
	out.Add ("carriedOverParameters", (Int32) r.carried);
	return out;
}

} // namespace


void RegisterObjectLampAdapters ()
{
	RegisterAdapter ({ API_ObjectID, CreateObject, SerializeObjectLike, ModifyObjectLike });
	RegisterAdapter ({ API_LampID, CreateLamp, SerializeObjectLike, ModifyObjectLike });
}


void RegisterGdlParameterCommands ()
{
	RegisterCommand ("GetGdlParameters",
		"Returns the GDL parameters of placed library-part based elements (Object, Lamp, Door, Window, Skylight, Zone, symbol Label, ...). "
		"Input: {elements: [guid], names?: [exact names], search?: substring of name/description, includeHidden?: false, includeValueLists?: false}. "
		"Output: {elements: [{guid, type, libraryPart, parameters: [{name, type, description, value, valueDescription?, hidden?, disabled?, arrayDims?, valueList?}]} | {error}]}. "
		"Lengths in meters, angles in degrees.",
		[] (const OS& params) -> OS {
			const GS::Array<API_Guid> guids = GetGuidArray (params, "elements");
			const GS::Array<GS::UniString> names = GetStringArray (params, "names");
			const GS::UniString search = GetString (params, "search", GS::UniString ());
			const bool includeHidden = GetBool (params, "includeHidden", false);
			const bool includeValueLists = GetBool (params, "includeValueLists", false);
			GS::Array<OS> out;
			for (const API_Guid& guid : guids)
				out.Push (Try ([&] () { return GetElementParams (guid, names, search, includeHidden, includeValueLists); }));
			return OS ("elements", out);
		});

	RegisterCommand ("SetGdlParameters",
		"Changes GDL parameters of placed elements through the library part's parameter script (like the settings dialog), in one undo step. "
		"Input: {elements: [{guid, params: {name: value}}]}. Values: Length in meters, Angle in degrees, booleans, strings, attribute names "
		"for Surface/BuildingMaterial/LineType/Fill/Profile parameters, value-list descriptions for numeric parameters, arrays as [..] or [{values: [..]}]. "
		"Changing A/B also resizes objects/openings. Output: {results: [{guid, parameters (resulting values)} | {error}]}.",
		[] (const OS& params) -> OS {
			const GS::Array<OS> items = GetObjectArray (params, "elements");
			GS::Array<OS> results;
			Undoable (GetString (params, "undoName", "Set GDL parameters (Claude)"), [&] () {
				for (const OS& item : items) {
					results.Push (Try ([&] () -> OS {
						const API_Guid guid = GetGuid (item, "guid");
						return SetElementParams (guid, GetObject (item, "params"));
					}));
				}
			});
			return OS ("results", results);
		});

	RegisterCommand ("ChangeLibraryPart",
		"Replaces the library part of placed Object / Lamp / Window / Door / Skylight / Zone / symbol Label elements, in one undo step. "
		"Input: {elements: [{guid, libraryPart, keepParameters?: true (copy same-named, same-typed, non-hidden, non-unique values), "
		"keepSize?: true for openings / false otherwise, params?: {name: value}, (objects/lamps also sizeA, sizeB, height)}]}. "
		"Output: {results: [{guid, type, libraryPart, carriedOverParameters} | {error}]}.",
		[] (const OS& params) -> OS {
			const GS::Array<OS> items = GetObjectArray (params, "elements");
			GS::Array<OS> results;
			Undoable (GetString (params, "undoName", "Change library part (Claude)"), [&] () {
				for (const OS& item : items)
					results.Push (Try ([&] () { return ChangeLibraryPartOf (item); }));
			});
			return OS ("results", results);
		});
}

} // namespace objlib


void RegisterObjectLibraryCommands ()
{
	objlib::RegisterObjectLampAdapters ();
	objlib::RegisterGdlParameterCommands ();
	objlib::RegisterLibraryPartCommands ();
	objlib::RegisterLibraryCommands ();
}

} // namespace cc
