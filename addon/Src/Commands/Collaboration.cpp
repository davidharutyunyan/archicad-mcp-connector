// *****************************************************************************
// Collaboration — Teamwork, issues (Issue Manager markup entries, BCF), favorites,
// tool defaults and revision management (read-only).
//
// Commands (namespace ClaudeConnector); lengths in meters, angles in degrees:
//
//  Teamwork (ACAPI_TeamworkControl_*) — solo projects answer {isTeamwork: false, message} instead of failing
//   GetTeamworkStatus   {elements?: [guid], objectSets?: true | [name], includeMembers?: true, includeAccessRights?: false}
//   TeamworkSend        {comment?}
//   TeamworkReceive     {}
//   ReserveElements     {elements?: [guid], objectSets?: [name], hotlinkCacheManagement?: bool, enableDialogs?: false}
//   ReleaseElements     {elements?: [guid], objectSets?: [name], hotlinkCacheManagement?: bool, enableDialogs?: false}
//
//  Issues (ACAPI_MarkUp_*) — issue reference = GUID string | exact issue name | {guid} | {name}
//   GetIssues               {issues?: [ref], element?: guid, search?, includeComments?: false, includeElements?: false}
//   CreateIssues            {issues: [{name, tagText?, tagTextVisible?, parentIssue?, comment?|comments?, attach?: {highlight|creation|deletion|modification: [guid]}}]}
//   DeleteIssues            {issues: [ref], acceptAllElements?: false}
//   AddIssueComments        {comments: [{issue, text, author?, status?: Error|Warning|Info|Unknown}]}
//   GetIssueComments        {issues?: [ref]}
//   AttachElementsToIssue   {issue, elements?: [guid], type?: Highlight|Creation|Deletion|Modification, modificationPairs?: [{original, modified}]}
//   DetachElementsFromIssue {issue, elements: [guid]}
//   GetIssueElements        {issues?: [ref], types?: [..]}
//   ExportIssuesToBCF       {path, issues?: [ref], useExternalId?: false, alignBySurveyPoint?: true, overwrite?: false, createFolders?: false}
//   ImportIssuesFromBCF     {path, alignBySurveyPoint?: true, openIssuePalette?: false}
//
//  Favorites (ACAPI_Favorite_*)
//   GetFavorites     {type?, variation?, search?, folder?, names?: [..], includeSettings?: false}
//   ApplyFavorite    {name, target: Defaults|Elements, elements?: [guid], applyProperties?, applyClassifications?, applyCategories?}
//   CreateFavorites  {favorites: [{name, element?: guid | toolDefaults?: {type, variation?}, folder?: [..] | "A/B", replace?: false,
//                                   includeProperties?: true, includeClassifications?: true, includeCategories?: true}]}
//   DeleteFavorites  {names: [..]}
//   RenameFavorite   {name, newName}
//   ExportFavorites  {path, names?: [..], overwrite?: false}
//   ImportFavorites  {path, folder?: [..] | "A/B", importFolders?: true, conflictPolicy?: Append|Overwrite|Skip|Error}
//
//  Tool defaults (ACAPI_Element_GetDefaults / ChangeDefaults through the element adapters)
//   GetToolDefaults  {type? | types?: [type | {type, variation}], variation?, includeGdlParameters?: true, gdlParameterNames?,
//                     includeHiddenGdlParameters?, includeClassifications?: true, includeCategories?: true,
//                     includeProperties?: false, includeAllProperties?: false}
//   SetToolDefaults  {defaults: [{type, variation?, fields: {...create_* fields, params?, libraryPart?, elementId?,
//                                   classifications?: [itemGuid], categories?: {id: value}, properties?: {guid|name: value}}}],
//                     returnSettings?: true}
//
//  Revisions (APIDb_GetRVM*) — read-only
//   GetRevisions        {include?: [issues|documentRevisions], issue?: guid|id}
//   GetRevisionChanges  {documentRevision? | layouts?: [ref] | allLayouts? | elements?: [guid] | changeIds?: [id], includeFirstIssue?: false}
// *****************************************************************************

#include "Commands/Commands.hpp"
#include "Core/Command.hpp"
#include "Core/Elements.hpp"
#include "Core/Enums.hpp"
#include "Core/LibParts.hpp"
#include "Core/Types.hpp"

#include "FileSystem.hpp"
#include "GSTime.hpp"
#include "Location.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace cc {

namespace {

// =============================================================================
// 1. Generic helpers
// =============================================================================

GS::UniString Key (const char* key)
{
	return GS::UniString (key, CC_UTF8);
}


GS::UniString Key (const GS::String& key)
{
	return GS::UniString (key.ToCStr (), CC_UTF8);
}


GS::UniString Join (const GS::Array<GS::UniString>& items, const char* sep = ", ")
{
	GS::UniString out;
	for (UIndex i = 0; i < items.GetSize (); ++i) {
		if (i > 0)
			out += sep;
		out += items[i];
	}
	return out;
}


bool ContainsIgnoreCase (const GS::UniString& haystack, const GS::UniString& needle)
{
	return haystack.ToLowerCase ().Contains (needle.ToLowerCase ());
}


// ISO 8601 UTC ("2024-05-01T12:30:00Z"); empty for 0.
GS::UniString TimeString (GSTime t)
{
	if (t == 0)
		return GS::UniString ();
	GSTimeRecord rec;
	if (TIGetTimeRecord (t, &rec, TI_UTC_TIME) != NoError)
		return GS::UniString ();
	return GS::UniString::Printf ("%04u-%02u-%02uT%02u:%02u:%02uZ",
								  (unsigned) rec.year, (unsigned) rec.month, (unsigned) rec.day,
								  (unsigned) rec.hour, (unsigned) rec.minute, (unsigned) rec.second);
}


GS::UniString LocationToPath (const IO::Location& loc)
{
	GS::UniString path;
	if (loc.ToPath (&path) != NoError || path.IsEmpty ())
		path = loc.ToDisplayText ();
	return path;
}


bool LocationExists (const IO::Location& loc)
{
	bool contains = false;
	return IO::fileSystem.Contains (loc, &contains) == NoError && contains;
}


IO::Location AbsoluteLocation (const GS::UniString& path, const char* key)
{
	if (path.IsEmpty ())
		Fail ("'" + Key (key) + "' must not be empty.");
#if defined (WINDOWS)
	const bool absolute = path.GetLength () > 2 && (path[1] == ':' || path.BeginsWith ("\\\\"));
#else
	const bool absolute = path.BeginsWith (GS::UniChar ('/'));
#endif
	if (!absolute)
		Fail ("'" + Key (key) + "' must be an absolute file path (e.g. /Users/me/Desktop/issues.bcfzip), got '" + path + "'.");
	IO::Location loc (path);
	if (loc.GetStatus () != NoError)
		Fail ("Invalid path '" + path + "'.");
	return loc;
}


GS::UniString PathExtension (const GS::UniString& path)
{
	const UIndex slash = path.FindLast (GS::UniChar ('/'));
	const UIndex dot = path.FindLast (GS::UniChar ('.'));
	if (dot == MaxUIndex || (slash != MaxUIndex && dot < slash) || dot + 1 >= path.GetLength ())
		return GS::UniString ();
	return GS::UniString (path.GetSubstring (dot + 1, path.GetLength () - dot - 1)).ToLowerCase ();
}


// Validates an output file: parent folder exists (or is created), existing file only with overwrite (then deleted).
IO::Location PrepareOutputFile (const OS& params, const GS::UniString& path)
{
	IO::Location loc = AbsoluteLocation (path, "path");
	IO::Location parent (loc);
	parent.DeleteLastLocalName ();
	if (!LocationExists (parent)) {
		if (!GetBool (params, "createFolders", false))
			Fail ("The folder '" + LocationToPath (parent) + "' does not exist. Pass createFolders: true to create it.");
		Check (IO::fileSystem.CreateFolderTree (parent), "Cannot create folder '" + LocationToPath (parent) + "'");
	}
	if (LocationExists (loc)) {
		if (!GetBool (params, "overwrite", false))
			Fail ("The file '" + path + "' already exists. Pass overwrite: true to replace it, or choose another path.");
		Check (IO::fileSystem.Delete (loc), "Cannot replace the existing file '" + path + "'");
	}
	return loc;
}


IO::Location ExistingInputFile (const GS::UniString& path)
{
	IO::Location loc = AbsoluteLocation (path, "path");
	if (!LocationExists (loc))
		Fail ("The file '" + path + "' does not exist. Give the absolute path of an existing file.", APIERR_BADPARS);
	return loc;
}


// Runs fn; when the API reports APIERR_NEEDSUNDOSCOPE it is run again inside an undoable command.
GSErrCode CallWithUndoFallback (const GS::UniString& undoName, const std::function<GSErrCode ()>& fn)
{
	const GSErrCode err = fn ();
	if (err != APIERR_NEEDSUNDOSCOPE)
		return err;
	GSErrCode inner = NoError;
	Undoable (undoName, [&] () { inner = fn (); });
	return inner;
}


// Runs fn and returns the error message of a failure (nullopt on success).
std::optional<GS::UniString> Attempt (const std::function<void ()>& fn)
{
	try {
		fn ();
		return std::nullopt;
	} catch (const Error& e) {
		return e.message;
	} catch (const GS::Exception& e) {
		return GS::UniString ("Archicad exception: ") + e.GetMessage ();
	} catch (const std::exception& e) {
		return GS::UniString ("C++ exception: ") + e.what ();
	}
}


bool ParseGuidText (const GS::UniString& text, API_Guid& out)
{
	GS::UniString s = text;
	s.Trim ();
	if (s.BeginsWith ("{") && s.EndsWith ("}") && s.GetLength () > 2)
		s = s.GetSubstring (1, s.GetLength () - 2);
	if (s.GetLength () != 36)
		return false;
	GS::Guid guid;
	if (guid.ConvertFromString (s.ToCStr ().Get ()) != NoError || guid.IsNull ())
		return false;
	out = GSGuid2APIGuid (guid);
	return true;
}


// --- ObjectState copying ------------------------------------------------------------

class ListKinds : public OS::Processor {
public:
	UInt32 numbers = 0;
	UInt32 strings = 0;
	UInt32 objects = 0;
	UInt32 bools = 0;
	void BoolFound (const GS::String&, bool) override						{ ++bools; }
	void IntFound (const GS::String&, Int64) override						{ ++numbers; }
	void UIntFound (const GS::String&, UInt64) override						{ ++numbers; }
	void RealFound (const GS::String&, double) override						{ ++numbers; }
	void StringFound (const GS::String&, const GS::UniString&) override		{ ++strings; }
	bool ObjectFound (const GS::String&, const OS&) override				{ ++objects; return false; }
};


class BoolListCollector : public OS::Processor {
public:
	GS::Array<bool> values;
	void BoolFound (const GS::String&, bool v) override { values.Push (v); }
};


// Copies src[key] (any JSON type) into dst[dstKey] (dstKey empty = same key).
void CopyField (const OS& src, const GS::String& key, OS& dst, const GS::String& dstKey = GS::String ())
{
	const GS::String& to = dstKey.IsEmpty () ? key : dstKey;
	const char* k = key.ToCStr ();
	switch (src.GetType (key)) {
		case OS::Bool:		{ bool v = false; src.Get (key, v); dst.Add (to, v); break; }
		case OS::Int:		{ Int64 v = 0; src.Get (key, v); dst.Add (to, v); break; }
		case OS::UInt:		{ UInt64 v = 0; src.Get (key, v); dst.Add (to, v); break; }
		case OS::Real:		{ double v = 0; src.Get (key, v); dst.Add (to, v); break; }
		case OS::String:	{ GS::UniString v; src.Get (key, v); dst.Add (to, v); break; }
		case OS::Object:	{ OS v; src.Get (key, v); dst.Add (to, v); break; }
		case OS::List: {
			ListKinds kinds;
			src.Enumerate (key, kinds);
			const UInt32 kindCount = (kinds.numbers > 0) + (kinds.strings > 0) + (kinds.objects > 0) + (kinds.bools > 0);
			if (kindCount > 1)
				Fail ("Field '" + Key (key) + "' mixes value types in one array; use one type per array.");
			if (kinds.objects > 0) {
				GS::Array<OS> v; src.Get (key, v); dst.Add (to, v);
			} else if (kinds.strings > 0) {
				dst.Add (to, GetStringArray (src, k));
			} else if (kinds.bools > 0) {
				BoolListCollector c; src.Enumerate (key, c); dst.Add (to, c.values);
			} else {
				dst.Add (to, GetNumberArray (src, k));
			}
			break;
		}
	}
}


bool InList (const GS::String& key, std::initializer_list<const char*> names)
{
	for (const char* n : names) {
		if (key == n)
			return true;
	}
	return false;
}


// Copy of src without the fields for which drop(key) is true; dropped names are collected.
OS CopyWithout (const OS& src, const std::function<bool (const GS::String&)>& drop, GS::Array<GS::UniString>* dropped = nullptr)
{
	OS out;
	for (const GS::String& key : src.GetFieldNames ()) {
		if (drop (key)) {
			if (dropped != nullptr)
				dropped->Push (Key (key));
			continue;
		}
		CopyField (src, key, out);
	}
	return out;
}


// Reads os[key] as a list of references: strings, or objects {guid} | {name} | {id}.
class RefCollector : public OS::Processor {
public:
	GS::Array<GS::UniString> refs;
	GS::Array<GS::UniString> errors;
	void StringFound (const GS::String&, const GS::UniString& v) override { refs.Push (v); }
	bool ObjectFound (const GS::String&, const OS& v) override
	{
		GS::UniString s;
		for (const char* k : { "guid", "name", "id" }) {
			if (v.Contains (k) && v.IsString (k)) {
				v.Get (k, s);
				refs.Push (s);
				return false;
			}
		}
		errors.Push ("Expected a string or {\"guid\": \"...\"} / {\"name\": \"...\"}.");
		return false;
	}
	void IntFound (const GS::String&, Int64) override		{ errors.Push ("Expected strings, got a number."); }
	void UIntFound (const GS::String&, UInt64) override		{ errors.Push ("Expected strings, got a number."); }
	void RealFound (const GS::String&, double) override		{ errors.Push ("Expected strings, got a number."); }
	void BoolFound (const GS::String&, bool) override		{ errors.Push ("Expected strings, got a boolean."); }
};


GS::Array<GS::UniString> GetRefArray (const OS& os, const char* key, bool required)
{
	if (!os.Contains (key)) {
		if (required)
			Fail ("Missing required array field '" + Key (key) + "'.");
		return {};
	}
	if (!os.IsList (key))
		Fail ("Field '" + Key (key) + "' must be an array.");
	RefCollector c;
	os.Enumerate (key, c);
	if (!c.errors.IsEmpty ())
		Fail ("Field '" + Key (key) + "': " + c.errors[0]);
	return c.refs;
}


// Reads os[key] as a single reference string: "text" or {guid}|{name}|{id}.
std::optional<GS::UniString> OptRef (const OS& os, const char* key)
{
	if (!os.Contains (key))
		return std::nullopt;
	if (os.IsString (key))
		return GetString (os, key);
	if (os.IsObject (key)) {
		OS o = GetObject (os, key);
		for (const char* k : { "guid", "name", "id" }) {
			if (o.Contains (k) && o.IsString (k))
				return GetString (o, k);
		}
	}
	Fail ("Field '" + Key (key) + "' must be a string (GUID or name) or {\"guid\": \"...\"} / {\"name\": \"...\"}.");
}


GS::UniString GetRef (const OS& os, const char* key)
{
	auto r = OptRef (os, key);
	if (!r.has_value ())
		Fail ("Missing required field '" + Key (key) + "'.");
	return *r;
}


// Folder hierarchy from ["A", "B"] or "A/B".
API_FavoriteFolderHierarchy GetFolder (const OS& os, const char* key)
{
	API_FavoriteFolderHierarchy folder;
	if (!os.Contains (key))
		return folder;
	if (os.IsString (key)) {
		GS::UniString s = GetString (os, key);
		GS::Array<GS::UniString> parts;
		s.Split ("/", &parts);
		for (GS::UniString p : parts) {
			p.Trim ();
			if (!p.IsEmpty ())
				folder.Push (p);
		}
		return folder;
	}
	return GetStringArray (os, key);
}


GS::UniString FolderPath (const API_FavoriteFolderHierarchy& folder)
{
	return Join (folder, "/");
}

// --- Element types / variations -------------------------------------------------------

const NamedValue kVariations[] = {
	{ "Generic",		APIVarId_Generic },
	{ "Object",			APIVarId_Object },
	{ "Light",			APIVarId_Light },
	{ "SymbStair",		APIVarId_SymbStair },
	{ "GridElement",	APIVarId_GridElement },
	{ "WallEnd",		APIVarId_WallEnd },
	{ "Door",			APIVarId_Door },
	{ "Skylight",		APIVarId_Skylight },
	{ "CornerWindow",	APIVarId_CornerWindow },
};


GS::UniString AllElemTypeNames ()
{
	GS::Array<GS::UniString> names;
	for (API_ElemTypeID t : AllElemTypes ())
		names.Push (ElemTypeName (t));
	return Join (names);
}


API_ElemTypeID ParseTypeName (const GS::UniString& name)
{
	auto t = ParseElemType (name);
	if (!t.has_value ())
		Fail ("Unknown element type '" + name + "'. Allowed: " + AllElemTypeNames () + ".");
	return *t;
}


// Reads {type, variation?} from os (type key configurable).
API_ElemType GetToolType (const OS& os, const char* typeKey = "type")
{
	API_ElemType type (ParseTypeName (GetString (os, typeKey)));
	if (Has (os, "variation"))
		type.variationID = (API_ElemVariationID) ParseNamed (kVariations, os, "variation");
	return type;
}


void AddTypeJson (OS& out, const API_ElemType& type)
{
	out.Add ("type", ElemTypeName (type.typeID));
	if (type.variationID != APIVarId_Generic)
		out.Add ("variation", NameOf (kVariations, (Int32) type.variationID));
}


OS TypeJson (const API_ElemType& type)
{
	OS out;
	AddTypeJson (out, type);
	return out;
}


GS::Array<API_ElemType> ToolboxTypes (bool includeHidden, API_ElemType* active = nullptr)
{
	GS::Array<API_ElemType> out;
	API_ToolBoxInfo info;
	BNZeroMemory (&info, sizeof (info));
	const GSErrCode err = ACAPI_Environment (APIEnv_GetToolBoxInfoID, &info, reinterpret_cast<void*> ((GS::IntPtr) includeHidden));
	if (err == NoError && info.data != nullptr) {
		const Int32 stored = (Int32) (BMGetHandleSize (reinterpret_cast<GSConstHandle> (info.data)) / sizeof (API_ToolBoxItem));
		const Int32 n = std::min<Int32> (info.nTools, stored);
		for (Int32 i = 0; i < n; ++i)
			out.Push ((*info.data)[i].type);
		if (active != nullptr)
			*active = info.activeTool.type;
	}
	BMKillHandle (reinterpret_cast<GSHandle*> (&info.data));
	return out;
}

// --- Library part based types -----------------------------------------------------------

std::optional<API_LibTypeID> LibTypeOf (API_ElemTypeID typeID)
{
	switch (typeID) {
		case API_ObjectID:		return APILib_ObjectID;
		case API_LampID:		return APILib_LampID;
		case API_WindowID:		return APILib_WindowID;
		case API_DoorID:		return APILib_DoorID;
		case API_SkylightID:	return APILib_SkylightID;
		case API_ZoneID:		return APILib_RoomID;
		default:				return std::nullopt;
	}
}


Int32* LibIndField (API_Element& e)
{
	switch (e.header.type.typeID) {
		case API_ObjectID:
		case API_LampID:		return &e.object.libInd;
		case API_WindowID:		return &e.window.openingBase.libInd;
		case API_DoorID:		return &e.door.openingBase.libInd;
		case API_SkylightID:	return &e.skylight.openingBase.libInd;
		case API_ZoneID:		return &e.zone.libInd;
		default:				return nullptr;
	}
}


// Field names used by the element families' create_* tools for the library part and its GDL values.
const char* LibPartKeyFor (API_ElemTypeID t)
{
	return t == API_ZoneID ? "stamp" : "libraryPart";
}


const char* GdlKeyFor (API_ElemTypeID t)
{
	switch (t) {
		case API_WindowID:
		case API_DoorID:
		case API_SkylightID:	return "gdlParams";
		case API_ZoneID:		return "stampParameters";
		default:				return "params";
	}
}


bool IsGdlKey (const GS::String& k)
{
	return InList (k, { "params", "gdlParams", "gdlParameters", "stampParameters" });
}


Int32 LibIndOf (const API_Element& e)
{
	Int32* p = LibIndField (const_cast<API_Element&> (e));
	return p != nullptr ? *p : 0;
}


void MaskLibInd (API_Element& mask, API_ElemTypeID typeID)
{
	switch (typeID) {
		case API_ObjectID:
		case API_LampID:		ACAPI_ELEMENT_MASK_SET (mask, API_ObjectType, libInd); break;
		case API_WindowID:		ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, openingBase.libInd); break;
		case API_DoorID:		ACAPI_ELEMENT_MASK_SET (mask, API_DoorType, openingBase.libInd); break;
		case API_SkylightID:	ACAPI_ELEMENT_MASK_SET (mask, API_SkylightType, openingBase.libInd); break;
		case API_ZoneID:		ACAPI_ELEMENT_MASK_SET (mask, API_ZoneType, libInd); break;
		default:				break;
	}
}


// Door / window / skylight opening base (nullptr for other types).
API_OpeningBaseType* OpeningBaseOf (API_Element& e)
{
	switch (e.header.type.typeID) {
		case API_WindowID:		return &e.window.openingBase;
		case API_DoorID:		return &e.door.openingBase;
		case API_SkylightID:	return &e.skylight.openingBase;
		default:				return nullptr;
	}
}


void MaskOpeningSize (API_Element& mask, API_ElemTypeID typeID, bool width, bool height)
{
	switch (typeID) {
		case API_WindowID:
			if (width)	ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, openingBase.width);
			if (height)	ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, openingBase.height);
			break;
		case API_DoorID:
			if (width)	ACAPI_ELEMENT_MASK_SET (mask, API_DoorType, openingBase.width);
			if (height)	ACAPI_ELEMENT_MASK_SET (mask, API_DoorType, openingBase.height);
			break;
		case API_SkylightID:
			if (width)	ACAPI_ELEMENT_MASK_SET (mask, API_SkylightType, openingBase.width);
			if (height)	ACAPI_ELEMENT_MASK_SET (mask, API_SkylightType, openingBase.height);
			break;
		default:
			break;
	}
}


Int32 ParamHandleCount (API_AddParType** params)
{
	if (params == nullptr || *params == nullptr)
		return 0;
	return (Int32) (BMGetHandleSize (reinterpret_cast<GSConstHandle> (params)) / sizeof (API_AddParType));
}


API_AddParType* FindParam (API_AddParType** params, const char* name)
{
	const Int32 n = ParamHandleCount (params);
	for (Int32 i = 0; i < n; ++i) {
		if (EqualsIgnoreCase (GS::UniString ((*params)[i].name, CC_UTF8), Key (name)))
			return &(*params)[i];
	}
	return nullptr;
}


// {name: value} of the parameters (visible only unless includeHidden / names given).
OS GdlParamsDict (API_AddParType** params, bool includeHidden, const GS::Array<GS::UniString>& names)
{
	OS dict;
	for (const OS& p : ParamsToJson (params, includeHidden, names)) {
		GS::UniString name;
		p.Get ("name", name);
		if (name.IsEmpty () || !p.Contains ("value"))
			continue;
		const GS::String key = ToStr (name);
		if (dict.Contains (key))
			continue;
		auto err = Attempt ([&] () { CopyField (p, "value", dict, key); });
		if (err.has_value ())
			continue;
	}
	return dict;
}

// --- Categories ------------------------------------------------------------------------

const NamedValue kCategoryIds[] = {
	{ "StructuralFunction",	API_ElemCategory_StructuralFunction },
	{ "Position",			API_ElemCategory_Position },
	{ "RenovationStatus",	API_ElemCategory_RenovationStatus },
	{ "RenovationFilter",	API_ElemCategory_RenovationFilter },
	{ "BRI",				API_ElemCategory_BRI },
};


GS::Array<API_ElemCategory> AllCategories ()
{
	GS::Array<API_ElemCategory> cats;
	if (ACAPI_Database (APIDb_GetElementCategoriesID, &cats) != NoError)
		cats.Clear ();
	return cats;
}


GS::Array<API_ElemCategoryValue> CategoryValues (const API_ElemCategory& cat)
{
	GS::Array<API_ElemCategoryValue> values;
	if (ACAPI_Database (APIDb_GetElementCategoryValuesID, const_cast<API_ElemCategory*> (&cat), &values) != NoError)
		values.Clear ();
	return values;
}


GS::UniString CategoryKey (const API_ElemCategory& cat)
{
	for (const NamedValue& nv : kCategoryIds) {
		if (nv.value == (Int32) cat.categoryID)
			return nv.name;
	}
	return GS::UniString (cat.name);
}


OS CategoryValueJson (const API_ElemCategoryValue& v)
{
	return OS ("value", GS::UniString (v.name), "valueGuid", GuidStr (v.guid), "category", GS::UniString (v.category.name));
}


// categories: {"StructuralFunction": "<localized value name or guid>", ...} -> resolved values
GS::Array<API_ElemCategoryValue> ResolveCategoryValues (const OS& categories)
{
	GS::Array<API_ElemCategoryValue> out;
	const GS::Array<API_ElemCategory> cats = AllCategories ();
	for (const GS::String& field : categories.GetFieldNames ()) {
		const GS::UniString key = Key (field);
		const API_ElemCategory* cat = nullptr;
		for (const API_ElemCategory& c : cats) {
			if (EqualsIgnoreCase (key, CategoryKey (c)) || EqualsIgnoreCase (key, GS::UniString (c.name)))
				cat = &c;
		}
		if (cat == nullptr) {
			GS::Array<GS::UniString> names;
			for (const API_ElemCategory& c : cats)
				names.Push (CategoryKey (c) + " (" + GS::UniString (c.name) + ")");
			Fail ("Unknown element category '" + key + "'. Available: " + Join (names) + ".");
		}
		const GS::UniString wanted = GetString (categories, field.ToCStr ());
		API_Guid wantedGuid = APINULLGuid;
		const bool byGuid = ParseGuidText (wanted, wantedGuid);
		const GS::Array<API_ElemCategoryValue> values = CategoryValues (*cat);
		const API_ElemCategoryValue* found = nullptr;
		for (const API_ElemCategoryValue& v : values) {
			if ((byGuid && v.guid == wantedGuid) || EqualsIgnoreCase (wanted, GS::UniString (v.name)))
				found = &v;
		}
		if (found == nullptr) {
			GS::Array<GS::UniString> names;
			for (const API_ElemCategoryValue& v : values)
				names.Push (GS::UniString (v.name));
			Fail ("Category '" + key + "' has no value '" + wanted + "'. Available (localized): " + Join (names) + ".");
		}
		out.Push (*found);
	}
	return out;
}

// --- Classifications --------------------------------------------------------------------

class ClassificationNames {
public:
	OS Item (const API_Guid& systemGuid, const API_Guid& itemGuid)
	{
		OS out;
		out.Add ("system", SystemName (systemGuid));
		out.Add ("systemGuid", GuidStr (systemGuid));
		API_ClassificationItem item;
		item.guid = itemGuid;
		out.Add ("itemGuid", GuidStr (itemGuid));
		if (ACAPI_Classification_GetClassificationItem (item) == NoError) {
			out.Add ("itemId", item.id);
			out.Add ("itemName", item.name);
		}
		return out;
	}

private:
	GS::UniString SystemName (const API_Guid& guid)
	{
		if (const GS::UniString* cached = systems.GetPtr (guid))
			return *cached;
		API_ClassificationSystem system;
		system.guid = guid;
		GS::UniString name;
		if (ACAPI_Classification_GetClassificationSystem (system) == NoError)
			name = system.name + (system.editionVersion.IsEmpty () ? GS::UniString () : " " + system.editionVersion);
		systems.Put (guid, name);
		return name;
	}

	GS::HashTable<API_Guid, GS::UniString> systems;
};


GS::Array<OS> ClassificationsJson (const GS::Array<GS::Pair<API_Guid, API_Guid>>& pairs)
{
	ClassificationNames names;
	GS::Array<OS> out;
	for (const auto& pair : pairs)
		out.Push (names.Item (pair.first, pair.second));
	return out;
}


// Classification item references: GUID strings (from get_classification... tools).
GS::Array<API_Guid> GetClassificationItemGuids (const OS& os, const char* key)
{
	GS::Array<API_Guid> out;
	for (const GS::UniString& ref : GetRefArray (os, key, false)) {
		API_Guid g = APINULLGuid;
		if (!ParseGuidText (ref, g))
			Fail ("'" + Key (key) + "' must contain classification item GUIDs, got '" + ref + "'.");
		API_ClassificationItem item;
		item.guid = g;
		if (ACAPI_Classification_GetClassificationItem (item) != NoError)
			Fail ("Classification item " + ref + " does not exist.", APIERR_BADID);
		out.Push (g);
	}
	return out;
}

// --- Properties -------------------------------------------------------------------------

class PropertyGroupNames {
public:
	GS::UniString Name (const API_Guid& groupGuid)
	{
		if (const GS::UniString* cached = groups.GetPtr (groupGuid))
			return *cached;
		API_PropertyGroup group;
		group.guid = groupGuid;
		GS::UniString name;
		if (ACAPI_Property_GetPropertyGroup (group) == NoError)
			name = group.name;
		groups.Put (groupGuid, name);
		return name;
	}

private:
	GS::HashTable<API_Guid, GS::UniString> groups;
};


GS::UniString EnumDisplay (const API_PropertyDefinition& def, const API_Variant& key)
{
	for (const API_SingleEnumerationVariant& e : def.possibleEnumValues) {
		if (e.keyVariant.type == API_PropertyGuidValueType && key.type == API_PropertyGuidValueType && e.keyVariant.guidValue == key.guidValue)
			return e.displayVariant.uniStringValue;
	}
	return GS::UniString ();
}


void AddVariant (OS& out, const char* key, const API_Variant& v, API_PropertyMeasureType measure)
{
	switch (v.type) {
		case API_PropertyIntegerValueType:	out.Add (key, (Int32) v.intValue); break;
		case API_PropertyRealValueType:		out.Add (key, measure == API_PropertyAngleMeasureType ? RadToDeg (v.doubleValue) : v.doubleValue); break;
		case API_PropertyStringValueType:	out.Add (key, v.uniStringValue); break;
		case API_PropertyBooleanValueType:	out.Add (key, v.boolValue); break;
		case API_PropertyGuidValueType:		out.Add (key, GuidStr (v.guidValue)); break;
		default:							out.Add (key, GS::UniString ()); break;
	}
}


void AddPropertyValue (OS& out, const API_Property& p)
{
	const API_PropertyDefinition& def = p.definition;
	if (p.status != API_Property_HasValue) {
		out.Add ("status", GS::UniString (p.status == API_Property_NotEvaluated ? "NotEvaluated" : "NotAvailable"));
		return;
	}
	if (p.value.variantStatus != API_VariantStatusNormal) {
		out.Add ("status", GS::UniString (p.value.variantStatus == API_VariantStatusNull ? "Null" : "Undefined"));
		return;
	}
	switch (def.collectionType) {
		case API_PropertySingleCollectionType:
			AddVariant (out, "value", p.value.singleVariant.variant, def.measureType);
			break;
		case API_PropertySingleChoiceEnumerationCollectionType:
			out.Add ("value", EnumDisplay (def, p.value.singleVariant.variant));
			break;
		case API_PropertyMultipleChoiceEnumerationCollectionType: {
			GS::Array<GS::UniString> values;
			for (const API_Variant& v : p.value.listVariant.variants)
				values.Push (EnumDisplay (def, v));
			out.Add ("value", values);
			break;
		}
		case API_PropertyListCollectionType: {
			if (def.valueType == API_PropertyStringValueType || def.valueType == API_PropertyGuidValueType) {
				GS::Array<GS::UniString> values;
				for (const API_Variant& v : p.value.listVariant.variants)
					values.Push (v.type == API_PropertyGuidValueType ? GuidStr (v.guidValue) : v.uniStringValue);
				out.Add ("value", values);
			} else if (def.valueType == API_PropertyBooleanValueType) {
				GS::Array<bool> values;
				for (const API_Variant& v : p.value.listVariant.variants)
					values.Push (v.boolValue);
				out.Add ("value", values);
			} else {
				GS::Array<double> values;
				for (const API_Variant& v : p.value.listVariant.variants)
					values.Push (v.type == API_PropertyIntegerValueType ? (double) v.intValue :
								 (def.measureType == API_PropertyAngleMeasureType ? RadToDeg (v.doubleValue) : v.doubleValue));
				out.Add ("value", values);
			}
			break;
		}
		default:
			break;
	}
}


OS PropertyJson (const API_Property& p, PropertyGroupNames& groups)
{
	OS out;
	out.Add ("guid", GuidStr (p.definition.guid));
	out.Add ("name", p.definition.name);
	out.Add ("group", groups.Name (p.definition.groupGuid));
	AddPropertyValue (out, p);
	if (p.isDefault)
		out.Add ("isDefault", true);
	return out;
}


API_Variant VariantFromJson (const OS& os, const char* key, const API_PropertyDefinition& def)
{
	API_Variant v;
	v.type = def.valueType;
	switch (def.valueType) {
		case API_PropertyIntegerValueType:	v.intValue = GetInt (os, key); break;
		case API_PropertyRealValueType:		v.doubleValue = def.measureType == API_PropertyAngleMeasureType ? DegToRad (GetDouble (os, key)) : GetDouble (os, key); break;
		case API_PropertyStringValueType:	v.uniStringValue = GetString (os, key); break;
		case API_PropertyBooleanValueType:	v.boolValue = GetBool (os, key); break;
		case API_PropertyGuidValueType:		v.guidValue = ParseGuid (GetString (os, key)); break;
		default:							Fail ("Property '" + def.name + "' has an undefined value type and cannot be set.", APIERR_NOTSUPPORTED);
	}
	return v;
}


API_Variant EnumKeyFromText (const API_PropertyDefinition& def, const GS::UniString& text)
{
	for (const API_SingleEnumerationVariant& e : def.possibleEnumValues) {
		const API_Variant& d = e.displayVariant;
		GS::UniString shown = d.type == API_PropertyStringValueType ? d.uniStringValue :
							  d.type == API_PropertyIntegerValueType ? GS::ValueToUniString (d.intValue) :
							  d.type == API_PropertyRealValueType ? GS::ValueToUniString (d.doubleValue) : GS::UniString ();
		if (EqualsIgnoreCase (shown, text) || (e.nonLocalizedValue.HasValue () && EqualsIgnoreCase (e.nonLocalizedValue.Get (), text)))
			return e.keyVariant;
	}
	GS::Array<GS::UniString> options;
	for (const API_SingleEnumerationVariant& e : def.possibleEnumValues)
		options.Push (e.displayVariant.uniStringValue);
	Fail ("Property '" + def.name + "' has no option '" + text + "'. Options: " + Join (options) + ".");
}


API_Property PropertyFromJson (const API_PropertyDefinition& def, const OS& os, const char* key)
{
	if (!def.canValueBeEditable)
		Fail ("Property '" + def.name + "' is not editable (calculated or expression-based).", APIERR_NOTEDITABLE);
	API_Property p;
	p.definition = def;
	p.isDefault = false;
	p.status = API_Property_HasValue;
	p.value.variantStatus = API_VariantStatusNormal;
	switch (def.collectionType) {
		case API_PropertySingleCollectionType:
			p.value.singleVariant.variant = VariantFromJson (os, key, def);
			break;
		case API_PropertySingleChoiceEnumerationCollectionType:
			p.value.singleVariant.variant = EnumKeyFromText (def, GetString (os, key));
			break;
		case API_PropertyMultipleChoiceEnumerationCollectionType:
			for (const GS::UniString& s : GetStringArray (os, key, true))
				p.value.listVariant.variants.Push (EnumKeyFromText (def, s));
			break;
		case API_PropertyListCollectionType:
			if (def.valueType == API_PropertyStringValueType) {
				for (const GS::UniString& s : GetStringArray (os, key, true)) {
					API_Variant v; v.type = def.valueType; v.uniStringValue = s;
					p.value.listVariant.variants.Push (v);
				}
			} else if (def.valueType == API_PropertyIntegerValueType || def.valueType == API_PropertyRealValueType) {
				for (double d : GetNumberArray (os, key, true)) {
					API_Variant v; v.type = def.valueType;
					if (def.valueType == API_PropertyIntegerValueType)
						v.intValue = (Int32) std::llround (d);
					else
						v.doubleValue = def.measureType == API_PropertyAngleMeasureType ? DegToRad (d) : d;
					p.value.listVariant.variants.Push (v);
				}
			} else {
				Fail ("List property '" + def.name + "' of this value type cannot be set here.", APIERR_NOTSUPPORTED);
			}
			break;
		default:
			Fail ("Property '" + def.name + "' has an unsupported collection type.", APIERR_NOTSUPPORTED);
	}
	return p;
}


// Finds a definition by GUID, exact name, or "Group/Name".
const API_PropertyDefinition& FindPropertyDefinition (const GS::Array<API_PropertyDefinition>& defs, const GS::UniString& ref, PropertyGroupNames& groups)
{
	API_Guid g = APINULLGuid;
	if (ParseGuidText (ref, g)) {
		for (const API_PropertyDefinition& d : defs) {
			if (d.guid == g)
				return d;
		}
		Fail ("Property definition " + ref + " is not available for this element type (check its availability / classification).", APIERR_BADID);
	}
	const API_PropertyDefinition* found = nullptr;
	UInt32 matches = 0;
	for (const API_PropertyDefinition& d : defs) {
		if (EqualsIgnoreCase (d.name, ref)) {
			found = &d;
			++matches;
		}
	}
	if (matches == 0) {
		const UIndex slash = ref.FindLast (GS::UniChar ('/'));
		if (slash != MaxUIndex && slash > 0) {
			GS::UniString groupName = ref.GetSubstring (0, slash);
			GS::UniString propName = ref.GetSubstring (slash + 1, ref.GetLength () - slash - 1);
			groupName.Trim ();
			propName.Trim ();
			for (const API_PropertyDefinition& d : defs) {
				if (EqualsIgnoreCase (d.name, propName) && EqualsIgnoreCase (groups.Name (d.groupGuid), groupName)) {
					found = &d;
					++matches;
				}
			}
		}
	}
	if (matches == 1)
		return *found;
	if (matches > 1)
		Fail ("Property name '" + ref + "' is ambiguous (it exists in several groups). Use 'Group/Name' or the property GUID.");
	Fail ("Property '" + ref + "' is not available for this element type. Only custom (user-defined) properties can be set here; "
		  "use the property GUID or 'Group/Name' (names are localized).", APIERR_BADNAME);
}

// =============================================================================
// 2. Settings serialization (tool defaults, favorites)
// =============================================================================

// Keys of per-instance data dropped from default/favorite settings (geometry, derived values, relations).
bool IsInstanceKey (const GS::String& key)
{
	return InList (key, { "begin", "end", "length", "polygon", "position", "origin", "absoluteElevation", "windows", "doors",
						  "points", "center", "area", "perimeter", "volume", "guid", "openings", "path", "holes" });
}


// Fields of set_tool_defaults that make no sense for defaults (geometry / identity) — reported as ignored.
bool IsIgnoredDefaultsField (const GS::String& key)
{
	return InList (key, { "begin", "end", "polygon", "position", "origin", "points", "center", "path", "guid", "type",
						  "storyIndex", "keepParameters", "keepSize" });
}


struct SettingsOptions {
	bool						gdl = true;
	bool						hiddenGdl = false;
	GS::Array<GS::UniString>	gdlNames;
};


// Settings of a default/favorite element: common header fields + adapter details + library part + GDL params.
OS SettingsJson (const API_Element& element, const API_ElementMemo* memo, const SettingsOptions& opt, GS::Array<GS::UniString>& notes)
{
	OS settings;
	settings.Add ("layer", AttrRef (API_LayerID, element.header.layer));
	settings.Add ("renovationStatus", RenovationStatusName (element.header.renovationStatus));
	settings.Add ("drawIndex", (Int32) element.header.drwIndex);
	if (memo != nullptr && memo->elemInfoString != nullptr)
		settings.Add ("elementId", *memo->elemInfoString);

	const ElementAdapter* adapter = FindAdapter (element.header.type.typeID);
	if (adapter != nullptr && adapter->serialize) {
		OS details;
		auto err = Attempt ([&] () { adapter->serialize (element, details); });
		if (err.has_value ()) {
			notes.Push ("Type-specific settings could not be read: " + *err);
		} else {
			for (const GS::String& key : details.GetFieldNames ()) {
				if (IsInstanceKey (key) || settings.Contains (key))
					continue;
				CopyField (details, key, settings);
			}
		}
	} else {
		notes.Push ("No type-specific adapter for " + ElemTypeName (element.header.type) +
					" yet: only layer, renovationStatus, drawIndex, elementId (and library part / GDL parameters) are shown and settable.");
	}

	const API_ElemTypeID typeID = element.header.type.typeID;
	const Int32 libInd = LibIndOf (element);
	const char* libKey = LibPartKeyFor (typeID);
	if (libInd > 0 && !settings.Contains (libKey)) {
		auto err = Attempt ([&] () {
			const API_LibPart lp = GetLibPartByIndex (libInd);
			settings.Add (libKey, OS ("index", libInd, "name", GS::UniString (lp.docu_UName), "guid", GS::UniString (lp.ownUnID)));
		});
		if (err.has_value ())
			notes.Push ("Library part " + GS::ValueToUniString (libInd) + " not found (missing library part?).");
	}
	const char* gdlKey = GdlKeyFor (typeID);
	if (opt.gdl && memo != nullptr && memo->params != nullptr) {
		// replace any partial summary of the adapter with the full {name: value} list of the default parameters
		OS withoutSummary = CopyWithout (settings, [&] (const GS::String& k) {
			return k == gdlKey || InList (k, { "paramCount", "paramsNote", "gdlParamCount", "gdlParamsNote" });
		});
		settings = withoutSummary;
		settings.Add (gdlKey, GdlParamsDict (memo->params, opt.hiddenGdl, opt.gdlNames));
		settings.Add ("gdlParameterCount", ParamHandleCount (memo->params));
	}
	return settings;
}

// =============================================================================
// 3. Teamwork
// =============================================================================

struct ProjectState {
	bool			teamwork = false;
	bool			untitled = false;
	short			userId = 0;
	Int32			workGroupMode = 0;
	GS::UniString	name;
	GS::UniString	path;
	bool			hasTeamLocation = false;
	IO::Location	teamLocation;
};


ProjectState GetProjectState ()
{
	ProjectState s;
	API_ProjectInfo info;
	Check (ACAPI_Environment (APIEnv_ProjectID, &info), "Cannot read the project info");
	s.teamwork = info.teamwork;
	s.untitled = info.untitled;
	s.userId = info.userId;
	s.workGroupMode = info.workGroupMode;
	if (info.projectName != nullptr)
		s.name = *info.projectName;
	if (info.projectPath != nullptr)
		s.path = *info.projectPath;
	if (info.location_team != nullptr) {
		s.hasTeamLocation = true;
		s.teamLocation = *info.location_team;
	}
	return s;
}


const char* const kSoloMessage =
	"This is a solo project, not a Teamwork (BIMcloud / BIMserver) project: every element and attribute is editable, "
	"nothing needs to be reserved or released, and Send/Receive do not apply. Just keep working (save with save_project).";


OS SoloResult (const ProjectState& s)
{
	OS out ("isTeamwork", false, "message", GS::UniString (kSoloMessage, CC_UTF8));
	out.Add ("project", OS ("name", s.name, "path", s.path, "untitled", s.untitled));
	return out;
}


GS::UniString UserName (short userId)
{
	GS::UniString name;
	if (userId <= 0 || ACAPI_TeamworkControl_GetUsernameFromId (userId, &name) != NoError)
		return GS::UniString ();
	return name;
}


GS::UniString UserLabel (short userId)
{
	const GS::UniString name = UserName (userId);
	return name.IsEmpty () ? "user #" + GS::ValueToUniString ((Int32) userId) : name;
}


GS::UniString LockableStatusName (API_LockableStatus st)
{
	switch (st) {
		case APILockableStatus_Free:			return "Free";
		case APILockableStatus_Editable:		return "ReservedByMe";
		case APILockableStatus_Locked:			return "ReservedByOther";
		case APILockableStatus_NotAvailable:	return "ServerUnavailable";
		case APILockableStatus_NotExist:		return "NotExist";
		default:								return "Unknown";
	}
}


OS LockStatusJson (const API_Guid& objectId)
{
	GS::PagedArray<short> conflicts;
	const API_LockableStatus st = ACAPI_TeamworkControl_GetLockableStatus (objectId, &conflicts);
	OS out ("status", LockableStatusName (st));
	if (conflicts.GetSize () > 0) {
		GS::Array<GS::UniString> names;
		for (UIndex i = 0; i < conflicts.GetSize (); ++i)
			names.Push (UserLabel (conflicts[i]));
		out.Add ("reservedBy", names);
	}
	return out;
}


struct ObjectSetName {
	const char* name;
	const char* internal;
};

const ObjectSetName kObjectSets[] = {
	{ "LayerSettings",					"LayerSettingsDialog" },
	{ "LineTypes",						"LineTypes" },
	{ "FillTypes",						"FillTypes" },
	{ "Composites",						"Composites" },
	{ "PenSets",						"PenTables" },
	{ "Surfaces",						"Surfaces" },
	{ "BuildingMaterials",				"BuildingMaterials" },
	{ "ZoneCategories",					"ZoneCategories" },
	{ "Profiles",						"Profiles" },
	{ "MEPSystems",						"MEPSystems" },
	{ "OperationProfiles",				"OperationProfiles" },
	{ "ModelViewOptions",				"ModelViewOptions" },
	{ "Favorites",						"Favorites" },
	{ "IssueTags",						"IssueTags" },
	{ "ClassificationsAndProperties",	"ClassificationsAndProperties" },
	{ "ProjectInfo",					"ProjectInfo" },
	{ "ProjectPreferences",				"PreferencesDialog" },
	{ "ProjectLibraryList",				"ProjectLibraryList" },
};

const char* const kHotlinkCache = "HotlinkCacheManagement";


GS::UniString ObjectSetNames ()
{
	GS::Array<GS::UniString> names;
	for (const ObjectSetName& s : kObjectSets)
		names.Push (s.name);
	names.Push (kHotlinkCache);
	return Join (names);
}


API_Guid FindObjectSet (const GS::UniString& name, GS::UniString& canonical)
{
	for (const ObjectSetName& s : kObjectSets) {
		if (EqualsIgnoreCase (name, s.name) || EqualsIgnoreCase (name, s.internal)) {
			canonical = s.name;
			const API_Guid g = ACAPI_TeamworkControl_FindLockableObjectSet (s.internal);
			if (g == APINULLGuid)
				Fail ("Teamwork object set '" + name + "' is not available in this project.", APIERR_BADNAME);
			return g;
		}
	}
	const API_Guid g = ACAPI_TeamworkControl_FindLockableObjectSet (name);
	if (g == APINULLGuid)
		Fail ("Unknown Teamwork object set '" + name + "'. Allowed: " + ObjectSetNames () + ".", APIERR_BADNAME);
	canonical = name;
	return g;
}


struct AccessRightName {
	API_TWAccessRights	right;
	const char*			name;
};

const AccessRightName kAccessRights[] = {
	{ APILineTypesCreate, "LineTypesCreate" }, { APILineTypesDeleteModify, "LineTypesDeleteModify" },
	{ APIFillTypesCreate, "FillTypesCreate" }, { APIFillTypesDeleteModify, "FillTypesDeleteModify" },
	{ APICompositesCreate, "CompositesCreate" }, { APICompositesDeleteModify, "CompositesDeleteModify" },
	{ APIPenSetsCreate, "PenSetsCreate" }, { APIPenSetsDeleteModify, "PenSetsDeleteModify" },
	{ APIProfilesCreate, "ProfilesCreate" }, { APIProfilesDeleteModify, "ProfilesDeleteModify" },
	{ APIZoneCategoriesCreate, "ZoneCategoriesCreate" }, { APIZoneCategoriesDeleteModify, "ZoneCategoriesDeleteModify" },
	{ APILayersCreate, "LayersCreate" }, { APILayersDeleteModify, "LayersDeleteModify" },
	{ APIMaterialsCreate, "SurfacesCreate" }, { APIMaterialsDeleteModify, "SurfacesDeleteModify" },
	{ APIFavoritesCreate, "FavoritesCreate" }, { APIFavoritesDeleteModify, "FavoritesDeleteModify" },
	{ APIMasterLayoutsCreate, "MasterLayoutsCreate" }, { APIMasterLayoutsDeleteModify, "MasterLayoutsDeleteModify" },
	{ APIModelViewOptionsCreate, "ModelViewOptionsCreate" }, { APIModelViewOptionsDeleteModify, "ModelViewOptionsDeleteModify" },
	{ APIProjectLocation, "ProjectLocation" }, { APIProjectPreferences, "ProjectPreferences" }, { APIProjectInfo, "ProjectInfo" },
	{ APIStoreyCreate, "StoriesCreate" }, { APIStoreyDeleteModify, "StoriesDeleteModify" },
	{ APIViewsAndFoldersCreate, "ViewsAndFoldersCreate" }, { APIViewsAndFoldersDeleteModify, "ViewsAndFoldersDeleteModify" },
	{ APILayoutsAndSubsetsCreate, "LayoutsAndSubsetsCreate" }, { APILayoutsAndSubsetsDeleteModify, "LayoutsAndSubsetsDeleteModify" },
	{ APIProjectNotesManage, "ProjectNotesManage" },
	{ APIDesignToolElements, "DesignToolElements" }, { APIGridToolElements, "GridToolElements" }, { APIDocumentToolElements, "DocumentToolElements" },
	{ APIExternalDrawingsManage, "ExternalDrawingsManage" }, { APIIFCExternalPropertiesManage, "IFCExternalPropertiesManage" },
	{ APISchedulesAndIndexesCreate, "SchedulesAndIndexesCreate" }, { APISchedulesAndIndexesDeleteModify, "SchedulesAndIndexesDeleteModify" },
	{ APISetUpListSchemesManage, "ListSchemesManage" },
	{ APIPublisherSetsCreate, "PublisherSetsCreate" }, { APIPublisherSetsDeleteModify, "PublisherSetsDeleteModify" }, { APIPublisherSetsPublish, "PublisherSetsPublish" },
	{ APIViewpointManage, "ViewpointManage" },
	{ APIMarkupEntryCreate, "MarkupEntryCreate" }, { APIMarkupEntryDeleteModify, "MarkupEntryDeleteModify" },
	{ APIXrefInstances, "XrefInstances" }, { APIHotlinkInstances, "HotlinkInstances" }, { APIHotlinkAndXrefManage, "HotlinkAndXrefManage" },
	{ APILibraryAddRemove, "LibraryAddRemove" }, { APILibraryPartCreate, "LibraryPartCreate" }, { APILibraryPartDeleteModify, "LibraryPartDeleteModify" },
	{ APIAddOnsSetupManage, "AddOnsSetupManage" },
	{ APIMEPSystemCreate, "MEPSystemCreate" }, { APIMEPSystemDeleteModify, "MEPSystemDeleteModify" },
	{ APIProjectPreviewManage, "ProjectPreviewManage" },
	{ APIRenovationFilterCreate, "RenovationFilterCreate" }, { APIRenovationFilterDeleteModify, "RenovationFilterDeleteModify" },
	{ APIRenovationOverrideStyleManage, "RenovationOverrideStyleManage" },
	{ APIBuildingMaterialsCreate, "BuildingMaterialsCreate" }, { APIBuildingMaterialsDeleteModify, "BuildingMaterialsDeleteModify" },
	{ APIRenderingSceneCreate, "RenderingSceneCreate" }, { APIRenderingSceneDeleteModify, "RenderingSceneDeleteModify" },
	{ APISaveAsPLNPLAMOD, "SaveAsPlnPlaMod" }, { APISaveAsDXFDWG, "SaveAsDxfDwg" }, { APISaveAsIFC, "SaveAsIfc" },
	{ APIViewTimeSheet, "ViewTimeSheet" }, { APIUpgradeToLaterArchiCADBuild, "UpgradeToLaterBuild" },
	{ APIChangeCreate, "ChangesCreate" }, { APIChangeDeleteModify, "ChangesDeleteModify" },
	{ APIIssueManage, "RevisionIssuesManage" }, { APIIssueModifyHistory, "RevisionIssueHistoryModify" },
	{ APIPropertyAndClassificationCreate, "PropertiesAndClassificationsCreate" },
	{ APIPropertyAndClassificationDeleteModify, "PropertiesAndClassificationsDeleteModify" },
	{ APIGraphicOverridesCreate, "GraphicOverridesCreate" }, { APIGraphicOverridesDeleteModify, "GraphicOverridesDeleteModify" },
	{ APIElementTransferSettingsCreate, "ElementTransferSettingsCreate" }, { APIElementTransferSettingsDeleteModify, "ElementTransferSettingsDeleteModify" },
	{ API3DStyleCreate, "3DStylesCreate" }, { API3DStyleDeleteModify, "3DStylesDeleteModify" },
	{ APIIFCPreferencesManage, "IFCPreferencesManage" },
	{ APIIssueCreateModify, "IssuesCreateModify" }, { APIIssueDelete, "IssuesDelete" }, { APIIssueTagManage, "IssueTagsManage" },
	{ APIStructuralAnalyticalToolElements, "StructuralAnalyticalToolElements" },
	{ APIStructuralAnalyticalModelGenerationRulesManage, "StructuralAnalyticalModelRulesManage" },
	{ APISAFTranslatorsManage, "SAFTranslatorsManage" }, { APISaveAsSAF, "SaveAsSaf" }, { APISaveAsRVT, "SaveAsRvt" },
};


bool HasAccessRight (API_TWAccessRights right)
{
	try {
		if (!GetProjectState ().teamwork)
			return true;	// solo project: every right is granted
	} catch (const Error&) {
		return true;
	}
	bool has = false;
	if (ACAPI_Environment (APIEnv_GetTWAccessRightID, reinterpret_cast<void*> ((GS::IntPtr) right), &has) != NoError)
		return true;		// unknown right / API failure: do not block, the actual call reports the error
	return has;
}


// Throws when the current Teamwork user lacks the right (solo projects always pass).
void RequireAccessRight (API_TWAccessRights right, const char* what)
{
	if (!HasAccessRight (right))
		Fail ("Your Teamwork role has no right to " + Key (what) + ". Ask the project administrator (BIMcloud role settings).", APIERR_NOACCESSRIGHT);
}


OS MembersJson ()
{
	API_SharingInfo si;
	BNZeroMemory (&si, sizeof (si));
	GS::Array<OS> members;
	if (ACAPI_Environment (APIEnv_ProjectSharingID, &si) == NoError && si.users != nullptr) {
		const Int32 stored = (Int32) (BMGetHandleSize (reinterpret_cast<GSConstHandle> (si.users)) / sizeof (API_UserInfo));
		const Int32 n = std::min<Int32> (si.nUsers, stored);
		for (Int32 i = 0; i < n; ++i) {
			const API_UserInfo& u = (*si.users)[i];
			members.Push (OS ("userId", (Int32) u.userId, "loginName", GS::UniString (u.loginName), "fullName", GS::UniString (u.fullName),
							  "connected", u.connected));
		}
	}
	BMKillHandle (reinterpret_cast<GSHandle*> (&si.users));
	return OS ("members", members);
}


OS GetTeamworkStatus (const OS& params)
{
	const ProjectState s = GetProjectState ();
	if (!s.teamwork)
		return SoloResult (s);

	OS out ("isTeamwork", true);
	out.Add ("project", OS ("name", s.name, "path", s.path));
	const bool online = ACAPI_TeamworkControl_IsOnline ();
	OS tw;
	tw.Add ("hasConnection", ACAPI_TeamworkControl_HasConnection ());
	tw.Add ("online", online);
	if (s.hasTeamLocation) {
		GS::UniString serverUrl, projectName, userName;
		if (ACAPI_TeamworkControl_GetTeamworkProjectDetails (s.teamLocation, &serverUrl, &projectName, &userName) == NoError) {
			tw.Add ("serverUrl", serverUrl);
			tw.Add ("teamProjectName", projectName);
			tw.Add ("loginName", userName);
		}
		tw.Add ("teamProjectLocation", LocationToPath (s.teamLocation));
	}
	out.Add ("teamwork", tw);
	out.Add ("currentUser", OS ("userId", (Int32) s.userId, "name", UserName (s.userId)));
	if (!online)
		out.Add ("warning", GS::UniString ("The BIMcloud/BIMserver is not reachable: reservation, Send and Receive are unavailable until the connection is back."));

	if (GetBool (params, "includeMembers", true)) {
		OS m = MembersJson ();
		GS::Array<OS> members;
		m.Get ("members", members);
		out.Add ("members", members);
	}

	short hotlinkOwner = 0;
	if (ACAPI_TeamworkControl_GetHotlinkCacheManagementOwner (&hotlinkOwner) == NoError && hotlinkOwner > 0)
		out.Add ("hotlinkCacheManagementReservedBy", UserLabel (hotlinkOwner));

	if (Has (params, "elements")) {
		GS::Array<OS> elements;
		for (const API_Guid& guid : GetGuidArray (params, "elements", true)) {
			elements.Push (Try ([&] () -> OS {
				if (!ElementExists (guid))
					Fail ("Element " + GuidStr (guid) + " does not exist.", APIERR_BADID);
				OS e = LockStatusJson (guid);
				e.Add ("guid", GuidStr (guid));
				return e;
			}));
		}
		out.Add ("elements", elements);
	}

	if (Has (params, "objectSets")) {
		GS::Array<GS::UniString> wanted;
		if (params.IsBool ("objectSets")) {
			if (GetBool (params, "objectSets"))
				for (const ObjectSetName& n : kObjectSets)
					wanted.Push (n.name);
		} else {
			wanted = GetStringArray (params, "objectSets");
		}
		GS::Array<OS> sets;
		for (const GS::UniString& name : wanted) {
			sets.Push (Try ([&] () -> OS {
				GS::UniString canonical;
				const API_Guid g = FindObjectSet (name, canonical);
				OS e = LockStatusJson (g);
				e.Add ("name", canonical);
				e.Add ("canCreate", ACAPI_TeamworkControl_HasCreateRight (g));
				e.Add ("canDeleteModify", ACAPI_TeamworkControl_HasDeleteModifyRight (g));
				return e;
			}));
		}
		out.Add ("objectSets", sets);
	}

	if (GetBool (params, "includeAccessRights", false)) {
		OS rights;
		for (const AccessRightName& r : kAccessRights)
			rights.Add (r.name, HasAccessRight (r.right));
		out.Add ("accessRights", rights);
	}
	return out;
}


void RequireOnline ()
{
	if (!ACAPI_TeamworkControl_IsOnline ())
		Fail ("The BIMcloud/BIMserver is not reachable (Teamwork offline). Check the network / server and retry; get_teamwork_status shows the connection state.", APIERR_GENERAL);
}


OS ReserveOrRelease (const OS& params, bool reserve)
{
	const ProjectState s = GetProjectState ();
	if (!s.teamwork) {
		OS out = SoloResult (s);
		out.Add (reserve ? "reserved" : "released", false);
		return out;
	}
	if (!Has (params, "elements") && GetStringArray (params, "objectSets").IsEmpty () && !GetBool (params, "hotlinkCacheManagement", false))
		Fail ("Nothing to " + GS::UniString (reserve ? "reserve" : "release") + ": give 'elements' (GUIDs) and/or 'objectSets' (" + ObjectSetNames () + ").");
	RequireOnline ();
	const bool dialogs = GetBool (params, "enableDialogs", false);
	OS out ("isTeamwork", true);

	if (Has (params, "elements")) {
		const GS::Array<API_Guid> guids = GetGuidArray (params, "elements", true);
		GS::Array<API_Guid> existing;
		GS::Array<OS> results;
		for (const API_Guid& g : guids) {
			if (ElementExists (g))
				existing.Push (g);
		}
		GS::HashTable<API_Guid, short> conflicts;
		GSErrCode err = NoError;
		if (!existing.IsEmpty ()) {
			err = reserve ? ACAPI_TeamworkControl_ReserveElements (existing, &conflicts, dialogs)
						  : ACAPI_TeamworkControl_ReleaseElements (existing, dialogs);
		}
		for (const API_Guid& g : guids) {
			if (!ElementExists (g)) {
				results.Push (ErrorObject (APIERR_BADID, "Element " + GuidStr (g) + " does not exist."));
				continue;
			}
			OS r = LockStatusJson (g);
			r.Add ("guid", GuidStr (g));
			GS::UniString status;
			r.Get ("status", status);
			if (reserve) {
				r.Add ("reserved", status == "ReservedByMe");
				if (const short* owner = conflicts.GetPtr (g))
					r.Add ("conflictWith", UserLabel (*owner));
			} else {
				r.Add ("released", status != "ReservedByMe");
			}
			results.Push (r);
		}
		out.Add ("elements", results);
		if (err != NoError)
			out.Add ("warning", GS::UniString ((reserve ? GS::UniString ("Reservation") : GS::UniString ("Release")) + " reported " + ErrorName (err) +
								 " - see the per-element status (elements reserved by others cannot be reserved; request them in Archicad's Teamwork palette)."));
	}

	GS::Array<GS::UniString> sets = GetStringArray (params, "objectSets");
	if (GetBool (params, "hotlinkCacheManagement", false))
		sets.Push (kHotlinkCache);
	if (!sets.IsEmpty ()) {
		GS::Array<OS> results;
		for (const GS::UniString& name : sets) {
			results.Push (Try ([&] () -> OS {
				if (EqualsIgnoreCase (name, kHotlinkCache)) {
					short conflict = 0;
					const GSErrCode e = reserve ? ACAPI_TeamworkControl_ReserveHotlinkCacheManagement (&conflict)
												: ACAPI_TeamworkControl_ReleaseHotlinkCacheManagement ();
					OS r ("name", GS::UniString (kHotlinkCache));
					r.Add (reserve ? "reserved" : "released", e == NoError);
					if (e != NoError)
						r.Add ("apiError", ErrorName (e));
					if (conflict > 0)
						r.Add ("conflictWith", UserLabel (conflict));
					return r;
				}
				GS::UniString canonical;
				const API_Guid g = FindObjectSet (name, canonical);
				GS::PagedArray<short> conflicts;
				const GSErrCode e = reserve ? ACAPI_TeamworkControl_ReserveLockable (g, &conflicts, dialogs)
											: ACAPI_TeamworkControl_ReleaseLockable (g, dialogs);
				OS r = LockStatusJson (g);
				r.Add ("name", canonical);
				r.Add (reserve ? "reserved" : "released", e == NoError);
				if (e != NoError)
					r.Add ("apiError", ErrorName (e));
				return r;
			}));
		}
		out.Add ("objectSets", results);
	}

	return out;
}

// =============================================================================
// 4. Issues (markup entries)
// =============================================================================

const NamedValue kCommentStatuses[] = {
	{ "Error",		APIComment_Error },
	{ "Warning",	APIComment_Warning },
	{ "Info",		APIComment_Info },
	{ "Unknown",	APIComment_Unknown },
};

const NamedValue kComponentTypes[] = {
	{ "Creation",		APIMarkUpComponent_Creation },
	{ "Highlight",		APIMarkUpComponent_Highlight },
	{ "Deletion",		APIMarkUpComponent_Deletion },
	{ "Modification",	APIMarkUpComponent_Modification },
};

const API_MarkUpComponentTypeID kAllComponentTypes[] = {
	APIMarkUpComponent_Creation, APIMarkUpComponent_Highlight, APIMarkUpComponent_Deletion, APIMarkUpComponent_Modification
};


GS::String ComponentKey (API_MarkUpComponentTypeID t)
{
	switch (t) {
		case APIMarkUpComponent_Creation:		return "creation";
		case APIMarkUpComponent_Highlight:		return "highlight";
		case APIMarkUpComponent_Deletion:		return "deletion";
		case APIMarkUpComponent_Modification:	return "modification";
		default:								return "other";
	}
}


GS::Array<API_MarkUpType> ListIssues ()
{
	GS::Array<API_MarkUpType> list;
	Check (ACAPI_MarkUp_GetList (APINULLGuid, &list), "Cannot list issues");
	return list;
}


GS::UniString IssueNamesHint (const GS::Array<API_MarkUpType>& all)
{
	if (all.IsEmpty ())
		return "The project has no issues (create one with create_issue).";
	GS::Array<GS::UniString> names;
	for (UIndex i = 0; i < all.GetSize () && i < 20; ++i)
		names.Push ("'" + all[i].name + "'");
	return "Existing issues: " + Join (names) + (all.GetSize () > 20 ? ", ..." : "") + " (see get_issues).";
}


API_MarkUpType FindIssue (const GS::Array<API_MarkUpType>& all, const GS::UniString& ref)
{
	API_Guid g = APINULLGuid;
	if (ParseGuidText (ref, g)) {
		for (const API_MarkUpType& m : all) {
			if (m.guid == g)
				return m;
		}
		Fail ("No issue with GUID " + ref + ". " + IssueNamesHint (all), APIERR_BADID);
	}
	GS::Array<const API_MarkUpType*> exact, loose;
	for (const API_MarkUpType& m : all) {
		if (m.name == ref)
			exact.Push (&m);
		else if (EqualsIgnoreCase (m.name, ref))
			loose.Push (&m);
	}
	const GS::Array<const API_MarkUpType*>& hits = exact.IsEmpty () ? loose : exact;
	if (hits.GetSize () == 1)
		return *hits[0];
	if (hits.GetSize () > 1)
		Fail ("Several issues are named '" + ref + "'. Use the issue GUID (see get_issues).");
	Fail ("Issue '" + ref + "' not found. " + IssueNamesHint (all), APIERR_BADNAME);
}


OS IssueRefJson (const API_MarkUpType& m)
{
	return OS ("guid", GuidStr (m.guid), "name", m.name);
}


OS CommentJson (const API_MarkUpCommentType& c)
{
	OS out;
	out.Add ("guid", GuidStr (c.guid));
	out.Add ("author", c.author);
	out.Add ("text", c.text);
	out.Add ("status", NameOf (kCommentStatuses, (Int32) c.status));
	out.Add ("created", TimeString (c.creaTime));
	return out;
}


GS::Array<OS> CommentsJson (const API_Guid& issue)
{
	GS::Array<API_MarkUpCommentType> comments;
	GS::Array<OS> out;
	if (ACAPI_MarkUp_GetComments (issue, &comments) == NoError) {
		for (const API_MarkUpCommentType& c : comments)
			out.Push (CommentJson (c));
	}
	return out;
}


GS::Array<API_Guid> AttachedElements (const API_Guid& issue, API_MarkUpComponentTypeID type)
{
	GS::Array<API_Guid> elems;
	if (ACAPI_MarkUp_GetAttachedElements (issue, type, elems) != NoError)
		elems.Clear ();
	return elems;
}


GS::Array<GS::UniString> GuidStrings (const GS::Array<API_Guid>& guids)
{
	GS::Array<GS::UniString> out;
	for (const API_Guid& g : guids)
		out.Push (GuidStr (g));
	return out;
}


OS IssueJson (const API_MarkUpType& m, const GS::Array<API_MarkUpType>& all, bool withComments, bool withElements)
{
	OS out;
	out.Add ("guid", GuidStr (m.guid));
	out.Add ("name", m.name);
	if (m.parentGuid != APINULLGuid) {
		out.Add ("parentGuid", GuidStr (m.parentGuid));
		for (const API_MarkUpType& p : all) {
			if (p.guid == m.parentGuid)
				out.Add ("parentName", p.name);
		}
	}
	GS::Array<GS::UniString> children;
	for (const API_MarkUpType& c : all) {
		if (c.parentGuid == m.guid)
			children.Push (GuidStr (c.guid));
	}
	if (!children.IsEmpty ())
		out.Add ("childIssues", children);
	out.Add ("created", TimeString (m.creaTime));
	out.Add ("modified", TimeString (m.modiTime));
	out.Add ("tagText", m.tagText);
	out.Add ("tagTextVisible", m.isTagTextElemVisible);
	if (m.tagTextElemGuid != APINULLGuid)
		out.Add ("tagTextElement", GuidStr (m.tagTextElemGuid));

	const GS::Array<OS> comments = CommentsJson (m.guid);
	out.Add ("commentCount", (Int32) comments.GetSize ());
	if (withComments)
		out.Add ("comments", comments);

	OS counts, lists;
	for (API_MarkUpComponentTypeID t : kAllComponentTypes) {
		const GS::Array<API_Guid> elems = AttachedElements (m.guid, t);
		counts.Add (ComponentKey (t), (Int32) elems.GetSize ());
		if (withElements)
			lists.Add (ComponentKey (t), GuidStrings (elems));
	}
	out.Add ("attachedElementCounts", counts);
	if (withElements)
		out.Add ("attachedElements", lists);
	return out;
}


GS::Array<API_MarkUpType> SelectIssues (const GS::Array<API_MarkUpType>& all, const OS& params, const char* key)
{
	if (!Has (params, key))
		return all;
	GS::Array<API_MarkUpType> out;
	for (const GS::UniString& ref : GetRefArray (params, key, true))
		out.Push (FindIssue (all, ref));
	return out;
}


API_MarkUpCommentType CommentFromJson (const OS& c, const GS::UniString& defaultAuthor)
{
	const GS::UniString text = GetString (c, "text");
	if (text.IsEmpty ())
		Fail ("Comment 'text' must not be empty.");
	API_MarkUpCommentStatusID status = APIComment_Unknown;
	if (Has (c, "status"))
		status = (API_MarkUpCommentStatusID) ParseNamed (kCommentStatuses, c, "status");
	return API_MarkUpCommentType (GetString (c, "author", defaultAuthor), text, status);
}


GS::UniString DefaultAuthor ()
{
	try {
		const ProjectState s = GetProjectState ();
		if (s.teamwork) {
			const GS::UniString name = UserName (s.userId);
			if (!name.IsEmpty ())
				return name;
		}
	} catch (const Error&) {
	}
	return "Claude";
}


GS::Array<API_Guid> ExistingElements (const GS::Array<API_Guid>& guids, GS::Array<GS::UniString>& missing)
{
	GS::Array<API_Guid> out;
	for (const API_Guid& g : guids) {
		if (ElementExists (g))
			out.Push (g);
		else
			missing.Push (GuidStr (g));
	}
	return out;
}


// Attaches elements; returns {attached: [...], missing?: [...], modificationPairs?: [...]}.
OS AttachElements (const API_MarkUpType& issue, API_MarkUpComponentTypeID type, const GS::Array<API_Guid>& elements,
				   const GS::Array<OS>& pairs)
{
	GS::Array<GS::UniString> missing;
	GS::HashTable<API_Guid, API_Guid> table;
	GS::Array<API_Guid> originals;
	if (!pairs.IsEmpty ()) {
		if (type != APIMarkUpComponent_Modification)
			Fail ("'modificationPairs' can only be used with type 'Modification'.");
		for (const OS& p : pairs) {
			const API_Guid orig = GetGuid (p, "original");
			const API_Guid mod = GetGuid (p, "modified");
			if (!ElementExists (orig)) { missing.Push (GuidStr (orig)); continue; }
			if (!ElementExists (mod)) { missing.Push (GuidStr (mod)); continue; }
			if (table.ContainsKey (orig))
				Fail ("Element " + GuidStr (orig) + " appears twice as 'original' in modificationPairs.");
			table.Add (orig, mod);
			originals.Push (orig);
		}
	}
	for (const API_Guid& g : ExistingElements (elements, missing)) {
		if (!table.ContainsKey (g))
			originals.Push (g);
	}
	if (originals.IsEmpty ())
		Fail ("No existing elements to attach" + (missing.IsEmpty () ? GS::UniString (".") : ": missing " + Join (missing) + "."), APIERR_BADID);

	Check (ACAPI_MarkUp_AttachElements (issue.guid, originals, type, type == APIMarkUpComponent_Modification ? &table : nullptr),
		   "Cannot attach elements to issue '" + issue.name + "'");

	OS out ("issue", IssueRefJson (issue));
	out.Add ("type", NameOf (kComponentTypes, (Int32) type));
	out.Add ("attached", GuidStrings (originals));
	if (!missing.IsEmpty ())
		out.Add ("missing", missing);
	if (type == APIMarkUpComponent_Modification) {
		GS::Array<OS> mods;
		for (const auto& pair : table)
			mods.Push (OS ("original", GuidStr (*pair.key), "modified", GuidStr (*pair.value)));
		out.Add ("modificationPairs", mods);
	}
	return out;
}


GSErrCode __ACENV_CALL CollectIfcRelationshipData (GS::HashTable<API_Guid, API_IFCRelationshipData>* table, const void* par1)
{
	if (table == nullptr || par1 == nullptr)
		return NoError;
	const API_IFCRelationshipData* data = reinterpret_cast<const API_IFCRelationshipData*> (par1);
	API_Guid ifcProjectId = APINULLGuid;
	data->containmentTable.EnumerateValues ([&] (const API_Guid& value) {
		if (!data->containmentTable.ContainsKey (value))
			ifcProjectId = value;
	});
	table->Put (ifcProjectId, *data);
	return NoError;
}


API_IFCRelationshipData CurrentIfcRelationshipData ()
{
	API_IFCRelationshipData data;
	GS::Array<API_IFCTranslatorIdentifier> translators;
	if (ACAPI_IFC_GetIFCExportTranslatorsList (translators) == NoError && !translators.IsEmpty () && translators[0].innerReference != nullptr)
		ACAPI_IFC_GetIFCRelationshipData (translators[0], data);
	return data;
}


void RegisterIssueCommands ()
{
	RegisterCommand ("GetIssues",
		"Lists the issues of the Issue Manager (markup entries, BCF topics). Input: {issues?: [guid|name], element?: guid (only issues the element is attached to), "
		"search?: substring of name/tag text, includeComments?: false, includeElements?: false}. Output: {issues: [{guid, name, parentGuid?, parentName?, "
		"childIssues?, created, modified (ISO UTC), tagText, tagTextVisible, tagTextElement?, commentCount, comments?, attachedElementCounts: "
		"{creation, highlight, deletion, modification}, attachedElements?}], count}.",
		[] (const OS& params) -> OS {
			const GS::Array<API_MarkUpType> all = ListIssues ();
			GS::Array<API_MarkUpType> issues = SelectIssues (all, params, "issues");
			GS::UniString attachment;
			if (Has (params, "element")) {
				const API_Guid elem = GetGuid (params, "element");
				if (!ElementExists (elem))
					Fail ("Element " + GuidStr (elem) + " does not exist.", APIERR_BADID);
				GS::Array<API_MarkUpType> attached;
				bool asCorrected = false;
				Check (ACAPI_MarkUp_GetList (elem, &attached, &asCorrected), "Cannot list the issues of element " + GuidStr (elem));
				GS::Array<API_MarkUpType> filtered;
				for (const API_MarkUpType& m : issues) {
					for (const API_MarkUpType& a : attached) {
						if (a.guid == m.guid) { filtered.Push (m); break; }
					}
				}
				issues = filtered;
				attachment = asCorrected ? "corrected" : "highlighted";
			}
			const GS::UniString search = GetString (params, "search", GS::UniString ());
			const bool withComments = GetBool (params, "includeComments", false);
			const bool withElements = GetBool (params, "includeElements", false);
			GS::Array<OS> out;
			for (const API_MarkUpType& m : issues) {
				if (!search.IsEmpty () && !ContainsIgnoreCase (m.name, search) &&
					!ContainsIgnoreCase (m.tagText, search))
					continue;
				out.Push (IssueJson (m, all, withComments, withElements));
			}
			OS res ("issues", out, "count", (Int32) out.GetSize ());
			if (!attachment.IsEmpty ())
				res.Add ("elementAttachment", attachment);
			return res;
		});

	RegisterCommand ("CreateIssues",
		"Creates issues (one undo step). Input: {issues: [{name*, tagText?, tagTextVisible?: true, parentIssue?: guid|name, "
		"comment?: text | {text, author?, status?}, comments?: [...], attach?: {highlight?|creation?|deletion?|modification?: [guid]}}]}. "
		"Comment status: Error|Warning|Info|Unknown. Output: {results: [{guid, name, comments: [guid], attached?} | {error}]}.",
		[] (const OS& params) -> OS {
			const GS::Array<OS> specs = GetObjectArray (params, "issues");
			RequireAccessRight (APIMarkupEntryCreate, "create issues");
			const GS::UniString author = DefaultAuthor ();
			GS::Array<OS> results;
			Undoable (GetString (params, "undoName", "Create issues (Claude)"), [&] () {
				for (const OS& spec : specs) {
					results.Push (Try ([&] () -> OS {
						const GS::UniString name = GetString (spec, "name");
						if (name.IsEmpty ())
							Fail ("Issue 'name' must not be empty.");
						const GS::Array<API_MarkUpType> all = ListIssues ();
						API_MarkUpType m (name);
						if (auto parent = OptRef (spec, "parentIssue"))
							m.parentGuid = FindIssue (all, *parent).guid;
						m.tagText = GetString (spec, "tagText", GS::UniString ());
						m.isTagTextElemVisible = GetBool (spec, "tagTextVisible", true);
						Check (ACAPI_MarkUp_Create (m), "Cannot create issue '" + name + "'");
						if (m.guid == APINULLGuid) {
							for (const API_MarkUpType& n : ListIssues ()) {
								bool known = false;
								for (const API_MarkUpType& o : all)
									if (o.guid == n.guid) { known = true; break; }
								if (!known && n.name == name)
									m.guid = n.guid;
							}
						}
						if (m.guid == APINULLGuid)
							Fail ("The issue '" + name + "' was created but could not be found again.", APIERR_GENERAL);

						OS res ("guid", GuidStr (m.guid), "name", name);
						GS::Array<OS> commentSpecs;
						if (Has (spec, "comment")) {
							if (spec.IsString ("comment"))
								commentSpecs.Push (OS ("text", GetString (spec, "comment")));
							else
								commentSpecs.Push (GetObject (spec, "comment"));
						}
						commentSpecs.Append (GetObjectArray (spec, "comments", false));
						GS::Array<GS::UniString> commentGuids;
						for (const OS& c : commentSpecs) {
							API_MarkUpCommentType comment = CommentFromJson (c, author);
							Check (ACAPI_MarkUp_AddComment (m.guid, comment), "Cannot add a comment to issue '" + name + "'");
							commentGuids.Push (GuidStr (comment.guid));
						}
						res.Add ("comments", commentGuids);

						OS attach;
						if (TryGetObject (spec, "attach", attach)) {
							OS attached;
							for (const NamedValue& nv : kComponentTypes) {
								GS::String key (ComponentKey ((API_MarkUpComponentTypeID) nv.value));
								if (!attach.Contains (key))
									continue;
								const GS::Array<API_Guid> elems = GetGuidArray (attach, key.ToCStr (), true);
								if (elems.IsEmpty ())
									continue;
								OS a = AttachElements (m, (API_MarkUpComponentTypeID) nv.value, elems, {});
								attached.Add (key, a);
							}
							res.Add ("attached", attached);
						}
						return res;
					}));
				}
			});
			return OS ("results", results);
		});

	RegisterCommand ("DeleteIssues",
		"Deletes issues (one undo step). Input: {issues: [guid|name], acceptAllElements?: false (true = accept the attached "
		"creation/deletion/modification proposals before deleting)}. Output: {results: [{guid, name, deleted: true} | {error}]}.",
		[] (const OS& params) -> OS {
			const GS::Array<GS::UniString> refs = GetRefArray (params, "issues", true);
			const bool accept = GetBool (params, "acceptAllElements", false);
			RequireAccessRight (APIMarkupEntryDeleteModify, "delete issues");
			GS::Array<OS> results;
			Undoable (GetString (params, "undoName", "Delete issues (Claude)"), [&] () {
				for (const GS::UniString& ref : refs) {
					results.Push (Try ([&] () -> OS {
						const API_MarkUpType m = FindIssue (ListIssues (), ref);
						Check (ACAPI_MarkUp_Delete (m.guid, accept), "Cannot delete issue '" + m.name + "'");
						OS r = IssueRefJson (m);
						r.Add ("deleted", true);
						return r;
					}));
				}
			});
			return OS ("results", results);
		});

	RegisterCommand ("AddIssueComments",
		"Adds comments to issues (one undo step). Input: {comments: [{issue: guid|name, text, author? (default: Teamwork user or 'Claude'), "
		"status?: Error|Warning|Info|Unknown}]}. Output: {results: [{issue: {guid, name}, comment: {guid, author, text, status, created}} | {error}]}.",
		[] (const OS& params) -> OS {
			const GS::Array<OS> items = GetObjectArray (params, "comments");
			const GS::UniString author = DefaultAuthor ();
			GS::Array<OS> results;
			Undoable (GetString (params, "undoName", "Add issue comments (Claude)"), [&] () {
				for (const OS& item : items) {
					results.Push (Try ([&] () -> OS {
						const API_MarkUpType m = FindIssue (ListIssues (), GetRef (item, "issue"));
						API_MarkUpCommentType comment = CommentFromJson (item, author);
						Check (ACAPI_MarkUp_AddComment (m.guid, comment), "Cannot add a comment to issue '" + m.name + "'");
						OS c = CommentJson (comment);
						for (const OS& stored : CommentsJson (m.guid)) {
							GS::UniString g;
							stored.Get ("guid", g);
							if (g == GuidStr (comment.guid))
								c = stored;
						}
						return OS ("issue", IssueRefJson (m), "comment", c);
					}));
				}
			});
			return OS ("results", results);
		});

	RegisterCommand ("GetIssueComments",
		"Returns the comments of issues. Input: {issues?: [guid|name] (default: all issues)}. "
		"Output: {issues: [{guid, name, comments: [{guid, author, text, status, created}]}]}.",
		[] (const OS& params) -> OS {
			const GS::Array<API_MarkUpType> all = ListIssues ();
			GS::Array<OS> out;
			for (const API_MarkUpType& m : SelectIssues (all, params, "issues")) {
				OS r = IssueRefJson (m);
				r.Add ("comments", CommentsJson (m.guid));
				out.Push (r);
			}
			return OS ("issues", out);
		});

	RegisterCommand ("AttachElementsToIssue",
		"Attaches elements to an issue (one undo step). Input: {issue: guid|name, elements?: [guid], type?: Highlight (default)|Creation|Deletion|Modification, "
		"modificationPairs?: [{original, modified}] (type Modification: pair an original element with its proposed replacement; "
		"Modification WITHOUT pairs makes Archicad create a modified copy of each element)}. Output: {issue, type, attached, missing?, modificationPairs?}.",
		[] (const OS& params) -> OS {
			const API_MarkUpType m = FindIssue (ListIssues (), GetRef (params, "issue"));
			API_MarkUpComponentTypeID type = APIMarkUpComponent_Highlight;
			if (Has (params, "type"))
				type = (API_MarkUpComponentTypeID) ParseNamed (kComponentTypes, params, "type");
			const GS::Array<OS> pairs = GetObjectArray (params, "modificationPairs", false);
			const GS::Array<API_Guid> elements = GetGuidArray (params, "elements", pairs.IsEmpty ());
			OS out;
			Undoable (GetString (params, "undoName", "Attach elements to issue (Claude)"), [&] () {
				out = AttachElements (m, type, elements, pairs);
			});
			return out;
		});

	RegisterCommand ("DetachElementsFromIssue",
		"Detaches elements from an issue, whatever their attachment type (one undo step). Input: {issue: guid|name, elements: [guid]}. "
		"Output: {issue, detached: [guid], notAttached?: [guid]}.",
		[] (const OS& params) -> OS {
			const API_MarkUpType m = FindIssue (ListIssues (), GetRef (params, "issue"));
			const GS::Array<API_Guid> elements = GetGuidArray (params, "elements", true);
			GS::HashSet<API_Guid> attached;
			for (API_MarkUpComponentTypeID t : kAllComponentTypes)
				for (const API_Guid& g : AttachedElements (m.guid, t))
					attached.Add (g);
			GS::Array<API_Guid> toDetach;
			GS::Array<GS::UniString> notAttached;
			for (const API_Guid& g : elements) {
				if (attached.Contains (g))
					toDetach.Push (g);
				else
					notAttached.Push (GuidStr (g));
			}
			if (toDetach.IsEmpty ())
				Fail ("None of the given elements is attached to issue '" + m.name + "' (see get_issue_elements).", APIERR_BADPARS);
			Undoable (GetString (params, "undoName", "Detach elements from issue (Claude)"), [&] () {
				Check (ACAPI_MarkUp_DetachElements (m.guid, toDetach), "Cannot detach elements from issue '" + m.name + "'");
			});
			OS out ("issue", IssueRefJson (m), "detached", GuidStrings (toDetach));
			if (!notAttached.IsEmpty ())
				out.Add ("notAttached", notAttached);
			return out;
		});

	RegisterCommand ("GetIssueElements",
		"Returns the elements attached to issues by attachment type. Input: {issues?: [guid|name] (default all), types?: [Creation|Highlight|Deletion|Modification]}. "
		"Output: {issues: [{guid, name, tagTextElement?, elements: {creation|highlight|deletion|modification: [{guid, type} | {guid, missing: true}]}}]}.",
		[] (const OS& params) -> OS {
			GS::Array<API_MarkUpComponentTypeID> types;
			for (const GS::UniString& t : GetStringArray (params, "types"))
				types.Push ((API_MarkUpComponentTypeID) ParseNamedString (kComponentTypes, t, "types"));
			if (types.IsEmpty ())
				for (API_MarkUpComponentTypeID t : kAllComponentTypes)
					types.Push (t);
			const GS::Array<API_MarkUpType> all = ListIssues ();
			GS::Array<OS> out;
			for (const API_MarkUpType& m : SelectIssues (all, params, "issues")) {
				OS r = IssueRefJson (m);
				if (m.tagTextElemGuid != APINULLGuid)
					r.Add ("tagTextElement", GuidStr (m.tagTextElemGuid));
				OS elements;
				for (API_MarkUpComponentTypeID t : types) {
					GS::Array<OS> list;
					for (const API_Guid& g : AttachedElements (m.guid, t)) {
						API_Elem_Head head;
						BNZeroMemory (&head, sizeof (head));
						head.guid = g;
						if (ACAPI_Element_GetHeader (&head) == NoError)
							list.Push (OS ("guid", GuidStr (g), "type", ElemTypeName (head.type)));
						else
							list.Push (OS ("guid", GuidStr (g), "missing", true));
					}
					elements.Add (ComponentKey (t), list);
				}
				r.Add ("elements", elements);
				out.Push (r);
			}
			return OS ("issues", out);
		});

	RegisterCommand ("ExportIssuesToBCF",
		"Exports issues to a BCF file (.bcfzip). Input: {path* (absolute; '.bcfzip' is appended when no extension), issues?: [guid|name] (default all), "
		"useExternalId?: false (use IFC GlobalIds of the original IFC import), alignBySurveyPoint?: true, overwrite?: false, createFolders?: false}. "
		"Output: {path, exported, issues: [{guid, name}]}.",
		[] (const OS& params) -> OS {
			GS::UniString path = GetString (params, "path");
			const GS::UniString ext = PathExtension (path);
			if (ext.IsEmpty ())
				path += ".bcfzip";
			else if (ext != "bcfzip" && ext != "bcf")
				Fail ("BCF files must end in .bcfzip (or .bcf), got '." + ext + "'.");
			const GS::Array<API_MarkUpType> all = ListIssues ();
			const GS::Array<API_MarkUpType> issues = SelectIssues (all, params, "issues");
			if (issues.IsEmpty ())
				Fail ("There are no issues to export (create some with create_issue).", APIERR_BADPARS);
			GS::Array<API_Guid> ids;
			GS::Array<OS> refs;
			for (const API_MarkUpType& m : issues) {
				ids.Push (m.guid);
				refs.Push (IssueRefJson (m));
			}
			IO::Location loc = PrepareOutputFile (params, path);
			const bool useExternalId = GetBool (params, "useExternalId", false);
			const bool align = GetBool (params, "alignBySurveyPoint", true);
			const GSErrCode err = CallWithUndoFallback ("Export issues to BCF (Claude)", [&] () {
				return ACAPI_MarkUp_ExportToBCF (loc, ids, useExternalId, align);
			});
			Check (err, "Cannot export issues to '" + path + "'");
			OS out ("path", LocationToPath (loc), "exported", (Int32) ids.GetSize (), "issues", refs);
			out.Add ("fileExists", LocationExists (loc));
			return out;
		});

	RegisterCommand ("ImportIssuesFromBCF",
		"Imports issues from a BCF file (.bcfzip/.bcf, BCF 2.x) without dialogs (one undo step). Input: {path*, alignBySurveyPoint?: true, "
		"openIssuePalette?: false}. Elements referenced by IFC GlobalId are matched to the model. Output: {imported, issues: [...new issues]}.",
		[] (const OS& params) -> OS {
			IO::Location loc = ExistingInputFile (GetString (params, "path"));
			const bool align = GetBool (params, "alignBySurveyPoint", true);
			const bool openPalette = GetBool (params, "openIssuePalette", false);
			RequireAccessRight (APIMarkupEntryCreate, "create (import) issues");
			const GS::Array<API_MarkUpType> before = ListIssues ();
			API_IFCRelationshipData relData = CurrentIfcRelationshipData ();
			Undoable (GetString (params, "undoName", "Import BCF issues (Claude)"), [&] () {
				Check (ACAPI_MarkUp_ImportFromBCF (loc, true, &CollectIfcRelationshipData, &relData, openPalette, align),
					   "Cannot import BCF file '" + LocationToPath (loc) + "' (is it a valid BCF 2.x .bcfzip?)");
			});
			const GS::Array<API_MarkUpType> after = ListIssues ();
			GS::Array<OS> added;
			for (const API_MarkUpType& m : after) {
				bool known = false;
				for (const API_MarkUpType& o : before)
					if (o.guid == m.guid) { known = true; break; }
				if (!known)
					added.Push (IssueJson (m, after, false, false));
			}
			return OS ("imported", (Int32) added.GetSize (), "issues", added);
		});
}

// =============================================================================
// 5. Favorites
// =============================================================================

class FavoriteHolder {
public:
	explicit FavoriteHolder (const GS::UniString& name) : fav (name) {}
	~FavoriteHolder ()
	{
		if (fav.memo.HasValue ())
			ACAPI_DisposeElemMemoHdls (&fav.memo.Get ());
		if (fav.memoMarker.HasValue ())
			ACAPI_DisposeElemMemoHdls (&fav.memoMarker.Get ());
		if (fav.subElements.HasValue ()) {
			for (API_SubElement& s : fav.subElements.Get ())
				ACAPI_DisposeElemMemoHdls (&s.memo);
		}
	}
	FavoriteHolder (const FavoriteHolder&) = delete;
	FavoriteHolder& operator= (const FavoriteHolder&) = delete;

	void NewMemo ()
	{
		fav.memo.New ();
		BNZeroMemory (&fav.memo.Get (), sizeof (API_ElementMemo));
	}

	API_Favorite fav;
};


struct FavoriteEntry {
	GS::UniString	name;
	API_ElemType	type;
};


// Every favorite with the tool type it was listed under (ACAPI_Favorite_GetNum per toolbox / element type).
GS::Array<FavoriteEntry> AllFavorites ()
{
	GS::Array<API_ElemType> types = ToolboxTypes (true);
	for (API_ElemTypeID t : AllElemTypes ())
		types.Push (API_ElemType (t));
	GS::HashSet<GS::UniString> seen;
	GS::Array<FavoriteEntry> entries;
	for (const API_ElemType& type : types) {
		short count = 0;
		GS::Array<GS::UniString> list;
		if (ACAPI_Favorite_GetNum (type, &count, nullptr, &list) != NoError)
			continue;
		for (const GS::UniString& n : list) {
			if (!seen.Contains (n)) {
				seen.Add (n);
				entries.Push ({ n, type });
			}
		}
	}
	return entries;
}


GS::Array<GS::UniString> AllFavoriteNames ()
{
	GS::Array<GS::UniString> names;
	for (const FavoriteEntry& e : AllFavorites ())
		names.Push (e.name);
	return names;
}


std::optional<API_ElemType> FavoriteTypeOf (const GS::UniString& name)
{
	for (const FavoriteEntry& e : AllFavorites ()) {
		if (e.name == name)
			return e.type;
	}
	return std::nullopt;
}


// Archicad 26 crashes inside ACAPI_Favorite_Get for favorites of external tools (e.g. the structural
// analytical Point Load: AnalyticalModelAPI -> AMElem::Load::GetDrawIndex null dereference), so the settings
// of such favorites are never read through the API.
bool FavoriteSettingsReadable (const API_ElemType& type)
{
	return type.typeID != API_ExternalElemID;
}


bool FavoriteExists (const GS::UniString& name)
{
	return FavoriteTypeOf (name).has_value ();
}


GS::UniString FavoriteNotFoundHint (const GS::UniString& name)
{
	const GS::Array<GS::UniString> names = AllFavoriteNames ();
	GS::Array<GS::UniString> similar;
	for (const GS::UniString& n : names) {
		if (ContainsIgnoreCase (n, name) || ContainsIgnoreCase (name, n))
			similar.Push ("'" + n + "'");
	}
	if (!similar.IsEmpty ())
		return "Did you mean " + Join (similar) + "?";
	return names.IsEmpty () ? GS::UniString ("The project has no favorites.") : GS::UniString ("List the names with get_favorites (names are localized).");
}


// Loads a favorite; withMemo also loads memo, properties, classifications and categories.
std::unique_ptr<FavoriteHolder> LoadFavorite (const GS::UniString& name, bool withMemo)
{
	const std::optional<API_ElemType> listedType = FavoriteTypeOf (name);
	if (!listedType.has_value ())
		Fail ("Favorite '" + name + "' does not exist. " + FavoriteNotFoundHint (name), APIERR_BADNAME);
	if (!FavoriteSettingsReadable (*listedType))
		Fail ("Favorite '" + name + "' belongs to an external tool (e.g. a structural analytical load). Archicad 26 crashes when such "
			  "favorites are read through the API, so they can only be listed by name and used by hand in the Favorites palette.", APIERR_NOTSUPPORTED);
	std::unique_ptr<FavoriteHolder> h (new FavoriteHolder (name));
	if (withMemo) {
		h->NewMemo ();
		h->fav.properties.New ();
		h->fav.classifications.New ();
		h->fav.elemCategoryValues.New ();
	}
	h->fav.folder.New ();
	const GSErrCode err = ACAPI_Favorite_Get (&h->fav);
	if (err != NoError) {
		// The favorite is listed (ACAPI_Favorite_GetNum), so a failure here means the API cannot read favorites of this
		// tool (seen on AC26 for Opening favorites: APIERR_BADNAME, and Drawing favorites: APIERR_REFUSEDCMD).
		Fail ("Favorite '" + name + "' (" + ElemTypeName (*listedType) + " tool) exists, but Archicad's API cannot read it (" + ErrorName (err) +
			  "): apply it by hand in the Favorites palette, or use get_tool_defaults / set_tool_defaults for this tool.", APIERR_NOTSUPPORTED);
	}
	return h;
}


OS FavoriteSummary (const API_Favorite& fav)
{
	OS out ("name", fav.name);
	AddTypeJson (out, fav.element.header.type);
	API_FavoriteFolderHierarchy folder;
	if (fav.folder.HasValue ())
		folder = fav.folder.Get ();
	out.Add ("folder", folder);
	out.Add ("folderPath", FolderPath (folder));
	return out;
}


// Mask used when a favorite (or element settings) is applied: every type-specific field plus the header settings
// (layer, draw order, renovation status/filter) — never identity, story or group/hotlink membership.
API_Element SettingsMask ()
{
	API_Element mask;
	ACAPI_ELEMENT_MASK_SETFULL (mask);
	BNZeroMemory (&mask.header, sizeof (API_Elem_Head));
	ACAPI_ELEMENT_MASK_SET (mask, API_Elem_Head, layer);
	ACAPI_ELEMENT_MASK_SET (mask, API_Elem_Head, drwIndex);
	ACAPI_ELEMENT_MASK_SET (mask, API_Elem_Head, renovationStatus);
	ACAPI_ELEMENT_MASK_SET (mask, API_Elem_Head, renovationFilterGuid);
	return mask;
}


bool IsHierarchicalType (API_ElemTypeID t)
{
	switch (t) {
		case API_StairID: case API_RiserID: case API_TreadID: case API_StairStructureID:
		case API_RailingID: case API_RailingToprailID: case API_RailingHandrailID: case API_RailingRailID: case API_RailingPostID:
		case API_RailingInnerPostID: case API_RailingBalusterID: case API_RailingPanelID: case API_RailingSegmentID: case API_RailingNodeID:
		case API_RailingBalusterSetID: case API_RailingPatternID: case API_RailingToprailEndID: case API_RailingHandrailEndID:
		case API_RailingRailEndID: case API_RailingToprailConnectionID: case API_RailingHandrailConnectionID:
		case API_RailingRailConnectionID: case API_RailingEndFinishID:
		case API_CurtainWallID: case API_CurtainWallSegmentID: case API_CurtainWallFrameID: case API_CurtainWallPanelID:
		case API_CurtainWallJunctionID: case API_CurtainWallAccessoryID:
		case API_MorphID:
			return true;
		default:
			return false;
	}
}


// A non-owning view of the settings-only parts of a favorite memo (GDL parameters, column/beam segment
// data). Geometry-carrying parts (polygons, stair base lines, morph bodies, ...) are left out so that applying
// a favorite to existing elements never moves or reshapes them.
class SettingsMemoView {
public:
	SettingsMemoView (const API_ElementMemo& src, API_ElemTypeID typeID)
	{
		BNZeroMemory (&memo, sizeof (memo));
		if (IsHierarchicalType (typeID))
			return;
		memo.params = src.params;
		if (typeID == API_ColumnID || typeID == API_BeamID) {
			memo.columnSegments = src.columnSegments;
			memo.beamSegments = src.beamSegments;
			memo.assemblySegmentCuts = src.assemblySegmentCuts;
			memo.assemblySegmentSchemes = src.assemblySegmentSchemes;
			memo.assemblySegmentProfiles = src.assemblySegmentProfiles;
		}
	}
	~SettingsMemoView () { BNZeroMemory (&memo, sizeof (memo)); }		// not owned: never dispose
	SettingsMemoView (const SettingsMemoView&) = delete;
	SettingsMemoView& operator= (const SettingsMemoView&) = delete;

	API_ElementMemo* Ptr () { return &memo; }

private:
	API_ElementMemo memo;
};


// Applies properties / classifications / categories of a favorite to one element (or to the defaults of type when guid is null).
void ApplyFavoriteExtras (const API_Favorite& fav, const API_Guid& guid, const API_ElemType& type, bool props, bool classes, bool cats,
						  GS::Array<GS::UniString>& warnings)
{
	// Inside an undo scope (target Elements) the calls succeed directly; for the defaults they may need one.
	const bool defaults = guid == APINULLGuid;
	const GS::UniString undoName ("Apply favorite (Claude)");
	if (classes && fav.classifications.HasValue ()) {
		for (const auto& pair : fav.classifications.Get ()) {
			const GSErrCode e = CallWithUndoFallback (undoName, [&] () {
				return defaults ? ACAPI_Element_AddClassificationItemDefault (type, pair.second)
								: ACAPI_Element_AddClassificationItem (guid, pair.second);
			});
			if (e != NoError)
				warnings.Push ("Classification " + GuidStr (pair.second) + " not applied: " + ErrorName (e));
		}
	}
	if (cats && fav.elemCategoryValues.HasValue ()) {
		for (const API_ElemCategoryValue& v : fav.elemCategoryValues.Get ()) {
			const GSErrCode e = CallWithUndoFallback (undoName, [&] () {
				return defaults ? ACAPI_Element_SetCategoryValueDefault (type, v.category, v)
								: ACAPI_Element_SetCategoryValue (guid, v.category, v);
			});
			if (e != NoError)
				warnings.Push ("Category '" + GS::UniString (v.category.name) + "' not applied: " + ErrorName (e));
		}
	}
	if (props && fav.properties.HasValue () && !fav.properties.Get ().IsEmpty ()) {
		const GSErrCode e = CallWithUndoFallback (undoName, [&] () {
			return defaults ? ACAPI_Element_SetPropertiesOfDefaultElem (type, fav.properties.Get ())
							: ACAPI_Element_SetProperties (guid, fav.properties.Get ());
		});
		if (e != NoError)
			warnings.Push ("Properties not applied: " + ErrorName (e) + " (a property may not be available for the element's classification).");
	}
}


// Collects the favorite payload (memo, classifications, properties, categories) from an element.
void FillFavoriteFromElement (FavoriteHolder& h, const API_Guid& guid, bool props, bool classes, bool cats)
{
	API_Favorite& fav = h.fav;
	fav.element = GetElement (guid);
	h.NewMemo ();
	Check (ACAPI_Element_GetMemo (guid, &fav.memo.Get (), APIMemoMask_All), "Cannot read the memo of element " + GuidStr (guid));
	if (classes) {
		fav.classifications.New ();
		if (ACAPI_Element_GetClassificationItems (guid, fav.classifications.Get ()) != NoError)
			fav.classifications.Get ().Clear ();
	}
	if (props) {
		GS::Array<API_PropertyDefinition> defs;
		fav.properties.New ();
		if (ACAPI_Element_GetPropertyDefinitions (guid, API_PropertyDefinitionFilter_UserDefined, defs) == NoError &&
			ACAPI_Element_GetPropertyValues (guid, defs, fav.properties.Get ()) == NoError) {
			GS::Array<API_Property>& list = fav.properties.Get ();
			for (UIndex i = list.GetSize (); i >= 1; --i) {
				const API_Property& p = list[i - 1];
				if (p.isDefault || !p.definition.canValueBeEditable || p.status != API_Property_HasValue)
					list.Delete (i - 1);
			}
		} else {
			fav.properties.Get ().Clear ();
		}
	}
	if (cats) {
		fav.elemCategoryValues.New ();
		for (const API_ElemCategory& c : AllCategories ()) {
			API_ElemCategoryValue v;
			BNZeroMemory (&v, sizeof (v));
			if (ACAPI_Element_GetCategoryValue (guid, c, &v) == NoError)
				fav.elemCategoryValues.Get ().Push (v);
		}
	}
}


void FillFavoriteFromDefaults (FavoriteHolder& h, const API_ElemType& type, bool props, bool classes, bool cats)
{
	API_Favorite& fav = h.fav;
	fav.element = {};
	fav.element.header.type = type;
	h.NewMemo ();
	Check (ACAPI_Element_GetDefaults (&fav.element, &fav.memo.Get ()), "Cannot read the tool defaults of " + ElemTypeName (type));
	if (classes) {
		fav.classifications.New ();
		if (ACAPI_Element_GetClassificationItemsDefault (type, fav.classifications.Get ()) != NoError)
			fav.classifications.Get ().Clear ();
	}
	if (props) {
		GS::Array<API_PropertyDefinition> defs;
		fav.properties.New ();
		if (ACAPI_Element_GetPropertyDefinitionsOfDefaultElem (type, API_PropertyDefinitionFilter_UserDefined, defs) == NoError &&
			ACAPI_Element_GetPropertyValuesOfDefaultElem (type, defs, fav.properties.Get ()) == NoError) {
			GS::Array<API_Property>& list = fav.properties.Get ();
			for (UIndex i = list.GetSize (); i >= 1; --i) {
				const API_Property& p = list[i - 1];
				if (p.isDefault || !p.definition.canValueBeEditable || p.status != API_Property_HasValue)
					list.Delete (i - 1);
			}
		} else {
			fav.properties.Get ().Clear ();
		}
	}
	if (cats) {
		fav.elemCategoryValues.New ();
		for (const API_ElemCategory& c : AllCategories ()) {
			API_ElemCategoryValue v;
			BNZeroMemory (&v, sizeof (v));
			if (ACAPI_Element_GetCategoryValueDefault (type, c, &v) == NoError)
				fav.elemCategoryValues.Get ().Push (v);
		}
	}
}


GS::UniString FavoriteErrorText (GSErrCode err, const GS::UniString& name)
{
	switch (err) {
		case APIERR_BADNAME:			return "Favorite name '" + name + "' is invalid or already used (pass replace: true to overwrite it, or choose another name).";
		case APIERR_NAMEALREADYUSED:	return "A favorite named '" + name + "' already exists (pass replace: true to overwrite it).";
		case APIERR_NOTMINE:			return "Favorite '" + name + "' is not editable by this add-on / user (Teamwork: reserve 'Favorites' with reserve_elements objectSets).";
		case APIERR_NOACCESSRIGHT:		return "Your Teamwork role has no right to change favorites.";
		default:						return "Favorite operation on '" + name + "' failed: " + ErrorName (err);
	}
}


const NamedValue kConflictPolicies[] = {
	{ "Append",		API_FavoriteAppend },
	{ "Overwrite",	API_FavoriteOverwrite },
	{ "Skip",		API_FavoriteSkip },
	{ "Error",		API_FavoriteError },
};


void RegisterFavoriteCommands ()
{
	RegisterCommand ("GetFavorites",
		"Lists the favorites of the project. Input: {type?: element type, variation?, search?: substring, folder?: 'A/B' (prefix), names?: [exact names], "
		"includeSettings?: false (also returns each favorite's settings like get_tool_defaults, plus classifications/categories/properties)}. "
		"Output: {favorites: [{name, type, variation?, folder: [..], folderPath, settings?, classifications?, categories?, properties?, notes?}], count}.",
		[] (const OS& params) -> OS {
			std::optional<API_ElemType> typeFilter;
			if (Has (params, "type"))
				typeFilter = GetToolType (params);
			const GS::UniString search = GetString (params, "search", GS::UniString ());
			const API_FavoriteFolderHierarchy folderFilter = GetFolder (params, "folder");
			const GS::Array<GS::UniString> wanted = GetStringArray (params, "names");
			const bool withSettings = GetBool (params, "includeSettings", false);
			SettingsOptions opt;
			opt.gdl = GetBool (params, "includeGdlParameters", true);

			GS::Array<OS> out;
			const GS::Array<FavoriteEntry> all = AllFavorites ();
			GS::Array<GS::UniString> names;
			if (wanted.IsEmpty ()) {
				for (const FavoriteEntry& e : all)
					names.Push (e.name);
			} else {
				names = wanted;
			}
			for (const GS::UniString& name : names) {
				if (!search.IsEmpty () && !ContainsIgnoreCase (name, search))
					continue;
				const FavoriteEntry* entry = nullptr;
				for (const FavoriteEntry& e : all) {
					if (e.name == name) { entry = &e; break; }
				}
				if (entry != nullptr && !FavoriteSettingsReadable (entry->type)) {
					// Listed by name only (reading it would crash Archicad 26).
					if (typeFilter.has_value () && entry->type.typeID != typeFilter->typeID)
						continue;
					if (!folderFilter.IsEmpty ())
						continue;
					OS r ("name", name);
					AddTypeJson (r, entry->type);
					r.Add ("notes", GS::Array<GS::UniString> { "External tool favorite (e.g. structural analytical load): settings cannot be read or applied through the API." });
					out.Push (r);
					continue;
				}
				OS item = Try ([&] () -> OS {
					std::unique_ptr<FavoriteHolder> h = LoadFavorite (name, withSettings);
					const API_Favorite& fav = h->fav;
					if (typeFilter.has_value ()) {
						if (fav.element.header.type.typeID != typeFilter->typeID)
							return OS ("skip", true);
						if (Has (params, "variation") && fav.element.header.type.variationID != typeFilter->variationID)
							return OS ("skip", true);
					}
					if (!folderFilter.IsEmpty ()) {
						const API_FavoriteFolderHierarchy folder = fav.folder.HasValue () ? fav.folder.Get () : API_FavoriteFolderHierarchy ();
						if (folder.GetSize () < folderFilter.GetSize ())
							return OS ("skip", true);
						for (UIndex i = 0; i < folderFilter.GetSize (); ++i) {
							if (!EqualsIgnoreCase (folder[i], folderFilter[i]))
								return OS ("skip", true);
						}
					}
					OS r = FavoriteSummary (fav);
					if (withSettings) {
						GS::Array<GS::UniString> notes;
						r.Add ("settings", SettingsJson (fav.element, fav.memo.HasValue () ? &fav.memo.Get () : nullptr, opt, notes));
						if (fav.classifications.HasValue ())
							r.Add ("classifications", ClassificationsJson (fav.classifications.Get ()));
						if (fav.elemCategoryValues.HasValue ()) {
							OS cats;
							for (const API_ElemCategoryValue& v : fav.elemCategoryValues.Get ())
								cats.Add (ToStr (CategoryKey (v.category)), CategoryValueJson (v));
							r.Add ("categories", cats);
						}
						if (fav.properties.HasValue ()) {
							PropertyGroupNames groups;
							GS::Array<OS> props;
							for (const API_Property& p : fav.properties.Get ())
								props.Push (PropertyJson (p, groups));
							r.Add ("properties", props);
						}
						if (!notes.IsEmpty ())
							r.Add ("notes", notes);
					}
					return r;
				});
				if (item.Contains ("error") && entry != nullptr) {
					// Still list it (with its type) so the caller sees every favorite; explain why it has no details.
					if (typeFilter.has_value () && entry->type.typeID != typeFilter->typeID)
						continue;
					OS err;
					GS::UniString message;
					if (item.Get ("error", err))
						err.Get ("message", message);
					OS r ("name", name);
					AddTypeJson (r, entry->type);
					r.Add ("notes", GS::Array<GS::UniString> { message });
					out.Push (r);
					continue;
				}
				if (!item.Contains ("skip"))
					out.Push (item);
			}
			return OS ("favorites", out, "count", (Int32) out.GetSize ());
		});

	RegisterCommand ("ApplyFavorite",
		"Applies a favorite. Input: {name*, target: 'Defaults' (tool defaults of the favorite's type, like double-clicking it in the Favorites palette) | "
		"'Elements' (inject its settings into existing elements of the same type, geometry kept), elements?: [guid] (target Elements), "
		"applyProperties?: true, applyClassifications?: true, applyCategories?: true}. Output: {favorite, type, target, results? , warnings?}.",
		[] (const OS& params) -> OS {
			const GS::UniString name = GetString (params, "name");
			const GS::UniString target = GetString (params, "target", "Defaults");
			const bool props = GetBool (params, "applyProperties", true);
			const bool classes = GetBool (params, "applyClassifications", true);
			const bool cats = GetBool (params, "applyCategories", true);
			std::unique_ptr<FavoriteHolder> h = LoadFavorite (name, true);
			API_Favorite& fav = h->fav;
			const API_ElemType type = fav.element.header.type;
			OS out ("favorite", name);
			AddTypeJson (out, type);
			GS::Array<GS::UniString> warnings;
			API_Element mask = SettingsMask ();

			if (EqualsIgnoreCase (target, "Defaults")) {
				out.Add ("target", GS::UniString ("Defaults"));
				API_Element element = fav.element;
				const GSErrCode err = CallWithUndoFallback ("Apply favorite to defaults (Claude)", [&] () {
					return ACAPI_Element_ChangeDefaults (&element, &fav.memo.Get (), &mask);
				});
				Check (err, "Cannot apply favorite '" + name + "' to the " + ElemTypeName (type) + " tool defaults");
				ApplyFavoriteExtras (fav, APINULLGuid, type, props, classes, cats, warnings);
				out.Add ("applied", true);
			} else if (EqualsIgnoreCase (target, "Elements")) {
				out.Add ("target", GS::UniString ("Elements"));
				const GS::Array<API_Guid> guids = GetGuidArray (params, "elements", true);
				GS::Array<API_Guid> matching;
				GS::Array<OS> results;
				for (const API_Guid& g : guids) {
					OS r = Try ([&] () -> OS {
						const API_Elem_Head head = GetHeader (g);
						if (head.type.typeID != type.typeID)
							Fail ("Element " + GuidStr (g) + " is a " + ElemTypeName (head.type) + ", but favorite '" + name + "' is for " +
								  ElemTypeName (type) + " elements.", APIERR_BADELEMENTTYPE);
						return OS ("guid", GuidStr (g));
					});
					if (!r.Contains ("error"))
						matching.Push (g);
					results.Push (r);
				}
				if (!matching.IsEmpty ()) {
					Undoable (GetString (params, "undoName", "Apply favorite (Claude)"), [&] () {
						SettingsMemoView view (fav.memo.Get (), type.typeID);
						Check (ACAPI_Element_ChangeParameters (matching, &fav.element, view.Ptr (), &mask),
							   "Cannot apply favorite '" + name + "' to the elements");
						for (const API_Guid& g : matching) {
							GS::Array<GS::UniString> w;
							ApplyFavoriteExtras (fav, g, type, props, classes, cats, w);
							for (const GS::UniString& s : w)
								warnings.Push (GuidStr (g) + ": " + s);
						}
					});
					for (OS& r : results) {
						if (!r.Contains ("error"))
							r.Add ("applied", true);
					}
				}
				out.Add ("results", results);
				if (IsHierarchicalType (type.typeID))
					warnings.Push ("Stairs, railings, curtain walls and morphs: only the main element settings are injected; their sub-element "
								   "structure (segments, panels, geometry) is kept unchanged.");
			} else {
				Fail ("'target' must be 'Defaults' or 'Elements'.");
			}
			if (!warnings.IsEmpty ())
				out.Add ("warnings", warnings);
			return out;
		});

	RegisterCommand ("CreateFavorites",
		"Creates favorites from placed elements or from the current tool defaults. Input: {favorites: [{name*, element?: guid | "
		"toolDefaults?: {type, variation?}, folder?: ['A','B'] | 'A/B', replace?: false, includeProperties?: true, includeClassifications?: true, "
		"includeCategories?: true}]}. Output: {results: [{name, type, folder} | {error}]}.",
		[] (const OS& params) -> OS {
			const GS::Array<OS> specs = GetObjectArray (params, "favorites");
			GS::Array<OS> results;
			for (const OS& spec : specs) {
				results.Push (Try ([&] () -> OS {
					const GS::UniString name = GetString (spec, "name");
					if (name.IsEmpty ())
						Fail ("Favorite 'name' must not be empty.");
					const bool props = GetBool (spec, "includeProperties", true);
					const bool classes = GetBool (spec, "includeClassifications", true);
					const bool cats = GetBool (spec, "includeCategories", true);
					FavoriteHolder h (name);
					OS tdef;
					if (Has (spec, "element")) {
						FillFavoriteFromElement (h, GetGuid (spec, "element"), props, classes, cats);
					} else if (TryGetObject (spec, "toolDefaults", tdef)) {
						FillFavoriteFromDefaults (h, GetToolType (tdef), props, classes, cats);
					} else {
						Fail ("Give 'element' (GUID of a placed element to copy the settings from) or 'toolDefaults': {type} (current tool settings).");
					}
					const API_FavoriteFolderHierarchy folder = GetFolder (spec, "folder");
					if (!folder.IsEmpty ())
						h.fav.folder = folder;
					const bool exists = FavoriteExists (name);
					if (exists && !GetBool (spec, "replace", false))
						Fail ("A favorite named '" + name + "' already exists. Pass replace: true to overwrite it, or choose another name.", APIERR_NAMEALREADYUSED);
					if (exists) {
						// replace safely: create under a temporary name, then delete the old one and rename
						GS::UniString tmp = name + " (new)";
						for (Int32 i = 2; FavoriteExists (tmp); ++i)
							tmp = name + " (new " + GS::ValueToUniString (i) + ")";
						h.fav.name = tmp;
						const GSErrCode err = CallWithUndoFallback ("Create favorite (Claude)", [&] () { return ACAPI_Favorite_Create (h.fav); });
						if (err != NoError)
							Fail (FavoriteErrorText (err, name), err);
						const GSErrCode del = CallWithUndoFallback ("Replace favorite (Claude)", [&] () { return ACAPI_Favorite_Delete (name); });
						if (del != NoError) {
							CallWithUndoFallback ("Replace favorite (Claude)", [&] () { return ACAPI_Favorite_Delete (tmp); });
							Fail (FavoriteErrorText (del, name), del);
						}
						const GSErrCode ren = CallWithUndoFallback ("Replace favorite (Claude)", [&] () { return ACAPI_Favorite_Rename (tmp, name); });
						if (ren != NoError)
							Fail ("The new favorite was created as '" + tmp + "' but could not be renamed to '" + name + "': " + ErrorName (ren) +
								  ". Rename it with rename_favorite.", ren);
						h.fav.name = name;
					} else {
						const GSErrCode err = CallWithUndoFallback ("Create favorite (Claude)", [&] () { return ACAPI_Favorite_Create (h.fav); });
						if (err != NoError)
							Fail (FavoriteErrorText (err, name), err);
					}
					const bool replaced = exists;
					OS r ("name", name);
					AddTypeJson (r, h.fav.element.header.type);
					r.Add ("folder", folder);
					if (replaced)
						r.Add ("replaced", true);
					return r;
				}));
			}
			return OS ("results", results);
		});

	RegisterCommand ("DeleteFavorites",
		"Deletes favorites by exact name. Input: {names: [..]}. Output: {results: [{name, deleted: true} | {error}]}.",
		[] (const OS& params) -> OS {
			GS::Array<OS> results;
			for (const GS::UniString& name : GetStringArray (params, "names", true)) {
				results.Push (Try ([&] () -> OS {
					if (!FavoriteExists (name))
						Fail ("Favorite '" + name + "' does not exist. " + FavoriteNotFoundHint (name), APIERR_BADNAME);
					const GSErrCode err = CallWithUndoFallback ("Delete favorite (Claude)", [&] () { return ACAPI_Favorite_Delete (name); });
					if (err != NoError)
						Fail (FavoriteErrorText (err, name), err);
					return OS ("name", name, "deleted", true);
				}));
			}
			return OS ("results", results);
		});

	RegisterCommand ("RenameFavorite",
		"Renames a favorite. Input: {name, newName}. Output: {name, newName, renamed: true}.",
		[] (const OS& params) -> OS {
			const GS::UniString name = GetString (params, "name");
			const GS::UniString newName = GetString (params, "newName");
			if (newName.IsEmpty ())
				Fail ("'newName' must not be empty.");
			if (!FavoriteExists (name))
				Fail ("Favorite '" + name + "' does not exist. " + FavoriteNotFoundHint (name), APIERR_BADNAME);
			if (FavoriteExists (newName))
				Fail ("A favorite named '" + newName + "' already exists.", APIERR_NAMEALREADYUSED);
			const GSErrCode err = CallWithUndoFallback ("Rename favorite (Claude)", [&] () { return ACAPI_Favorite_Rename (name, newName); });
			if (err != NoError)
				Fail (FavoriteErrorText (err, name), err);
			return OS ("name", name, "newName", newName, "renamed", true);
		});

	RegisterCommand ("ExportFavorites",
		"Exports favorites to a preferences file (.prf) that other projects can import. Input: {path* (absolute, .prf appended when missing), "
		"names?: [..] (default all), overwrite?: false, createFolders?: false}. Output: {path, exported}.",
		[] (const OS& params) -> OS {
			GS::UniString path = GetString (params, "path");
			if (PathExtension (path).IsEmpty ())
				path += ".prf";
			const GS::Array<GS::UniString> names = GetStringArray (params, "names");
			for (const GS::UniString& n : names) {
				if (!FavoriteExists (n))
					Fail ("Favorite '" + n + "' does not exist. " + FavoriteNotFoundHint (n), APIERR_BADNAME);
			}
			IO::Location loc = PrepareOutputFile (params, path);
			const GSErrCode err = ACAPI_Favorite_Export (loc, names.IsEmpty () ? nullptr : &names);
			Check (err, "Cannot export favorites to '" + path + "'");
			return OS ("path", LocationToPath (loc), "exported", names.IsEmpty () ? (Int32) AllFavoriteNames ().GetSize () : (Int32) names.GetSize (),
					   "fileExists", LocationExists (loc));
		});

	RegisterCommand ("ImportFavorites",
		"Imports favorites from a .prf file exported from Archicad. Input: {path*, folder?: target folder ['A'] | 'A/B' (default root), "
		"importFolders?: true (keep the file's folder structure), conflictPolicy?: Append (default, renames duplicates)|Overwrite|Skip|Error}. "
		"Output: {imported: [names], count, conflict?}.",
		[] (const OS& params) -> OS {
			IO::Location loc = ExistingInputFile (GetString (params, "path"));
			const API_FavoriteFolderHierarchy folder = GetFolder (params, "folder");
			const bool importFolders = GetBool (params, "importFolders", true);
			API_FavoriteNameConflictResolutionPolicy policy = API_FavoriteAppend;
			if (Has (params, "conflictPolicy"))
				policy = (API_FavoriteNameConflictResolutionPolicy) ParseNamed (kConflictPolicies, params, "conflictPolicy");
			GS::HashSet<GS::UniString> before;
			for (const GS::UniString& n : AllFavoriteNames ())
				before.Add (n);
			GS::UniString conflict;
			const GSErrCode err = CallWithUndoFallback ("Import favorites (Claude)", [&] () {
				return ACAPI_Favorite_Import (loc, folder, importFolders, policy, &conflict);
			});
			if (err != NoError) {
				if (!conflict.IsEmpty ())
					Fail ("Import stopped: favorite '" + conflict + "' already exists (conflictPolicy Error). Use Append, Overwrite or Skip.", err);
				Check (err, "Cannot import favorites from '" + LocationToPath (loc) + "' (is it a favorites .prf file?)");
			}
			GS::Array<GS::UniString> added;
			for (const GS::UniString& n : AllFavoriteNames ()) {
				if (!before.Contains (n))
					added.Push (n);
			}
			OS out ("imported", added, "count", (Int32) added.GetSize ());
			if (!conflict.IsEmpty ())
				out.Add ("firstConflict", conflict);
			return out;
		});
}

// =============================================================================
// 6. Tool defaults
// =============================================================================

OS ToolDefaultsJson (const API_ElemType& type, const OS& params)
{
	API_Element element = NewElement (type.typeID);
	element.header.type = type;
	Memo memo;
	Check (ACAPI_Element_GetDefaults (&element, memo.Ptr ()),
		   "Cannot read the default settings of the " + ElemTypeName (type) + " tool (not every element type has a tool)");
	element.header.guid = APINULLGuid;

	SettingsOptions opt;
	opt.gdl = GetBool (params, "includeGdlParameters", true);
	opt.hiddenGdl = GetBool (params, "includeHiddenGdlParameters", false);
	opt.gdlNames = GetStringArray (params, "gdlParameterNames");

	GS::Array<GS::UniString> notes;
	OS out;
	AddTypeJson (out, element.header.type);
	out.Add ("settings", SettingsJson (element, memo.Ptr (), opt, notes));

	if (GetBool (params, "includeClassifications", true)) {
		GS::Array<GS::Pair<API_Guid, API_Guid>> pairs;
		if (ACAPI_Element_GetClassificationItemsDefault (type, pairs) == NoError)
			out.Add ("classifications", ClassificationsJson (pairs));
	}
	if (GetBool (params, "includeCategories", true)) {
		OS cats;
		for (const API_ElemCategory& c : AllCategories ()) {
			API_ElemCategoryValue v;
			BNZeroMemory (&v, sizeof (v));
			if (ACAPI_Element_GetCategoryValueDefault (type, c, &v) == NoError)
				cats.Add (ToStr (CategoryKey (c)), CategoryValueJson (v));
		}
		out.Add ("categories", cats);
	}
	if (GetBool (params, "includeProperties", false) || GetBool (params, "includeAllProperties", false)) {
		const bool all = GetBool (params, "includeAllProperties", false);
		GS::Array<API_PropertyDefinition> defs;
		GS::Array<API_Property> props;
		GS::Array<OS> list;
		if (ACAPI_Element_GetPropertyDefinitionsOfDefaultElem (type, API_PropertyDefinitionFilter_UserDefined, defs) == NoError &&
			ACAPI_Element_GetPropertyValuesOfDefaultElem (type, defs, props) == NoError) {
			PropertyGroupNames groups;
			for (const API_Property& p : props) {
				if (!all && p.isDefault)
					continue;
				list.Push (PropertyJson (p, groups));
			}
		}
		out.Add ("properties", list);
	}
	if (!notes.IsEmpty ())
		out.Add ("notes", notes);
	return out;
}


GS::Array<API_ElemType> RequestedTypes (const OS& params)
{
	GS::Array<API_ElemType> types;
	if (Has (params, "type"))
		types.Push (GetToolType (params));
	if (Has (params, "types")) {
		if (!params.IsList ("types"))
			Fail ("'types' must be an array of type names or {type, variation} objects.");
		ListKinds kinds;
		params.Enumerate ("types", kinds);
		if (kinds.objects > 0) {
			for (const OS& t : GetObjectArray (params, "types"))
				types.Push (GetToolType (t));
		} else {
			for (const GS::UniString& t : GetStringArray (params, "types"))
				types.Push (API_ElemType (ParseTypeName (t)));
		}
	}
	return types;
}


OS ToolboxJson ()
{
	API_ElemType active (API_ZombieElemID);
	GS::Array<OS> tools;
	for (const API_ElemType& t : ToolboxTypes (false, &active))
		tools.Push (TypeJson (t));
	OS out ("tools", tools);
	if (active.typeID != API_ZombieElemID)
		out.Add ("activeTool", TypeJson (active));
	out.Add ("hint", GS::UniString ("Pass type (e.g. 'Wall') to get_tool_defaults to read a tool's default settings; variation is only needed for "
									"tools that share an element type (e.g. Object vs. GridElement)."));
	return out;
}


// Applies fields through the element adapter; returns the adapter memo's GDL params (caller owns) if it produced any.
void ApplyAdapterFields (const ElementAdapter* adapter, API_Element& element, API_Element& mask, const OS& patch,
						 API_AddParType**& paramsOut, GS::Array<GS::UniString>& memoNotes)
{
	Memo am;
	UInt64 amMask = 0;
	adapter->modify (element, mask, *am, amMask, patch);
	if ((amMask & APIMemoMask_AddPars) != 0 && am->params != nullptr) {
		if (paramsOut != nullptr)
			ACAPI_DisposeAddParHdl (&paramsOut);
		paramsOut = am->params;
		am->params = nullptr;
	}
	if ((amMask & ~APIMemoMask_AddPars) != 0)
		memoNotes.Push ("Some fields change per-element data (polygon, segments, edges) that tool defaults cannot store; they were ignored.");
}


OS SetOneToolDefaults (const OS& item, bool returnSettings)
{
	const API_ElemType type = GetToolType (item);
	OS fields;
	if (Has (item, "fields"))
		fields = GetObject (item, "fields");
	else
		fields = CopyWithout (item, [] (const GS::String& k) { return k == "type" || k == "variation"; });

	API_Element element = NewElement (type.typeID);
	element.header.type = type;
	Memo memo;
	Check (ACAPI_Element_GetDefaults (&element, memo.Ptr ()), "Cannot read the default settings of the " + ElemTypeName (type) + " tool");
	element.header.guid = APINULLGuid;
	API_Element mask;
	ACAPI_ELEMENT_MASK_CLEAR (mask);
	bool memoChanged = false;

	GS::Array<GS::UniString> applied, ignored, notes;
	GS::Array<OS> rejected;

	// 1. common header fields
	{
		OS common = CopyWithout (fields, [] (const GS::String& k) { return !InList (k, { "layer", "renovationStatus", "drawIndex" }); });
		ApplyCommonFields (element, &mask, common);
		for (const GS::String& k : common.GetFieldNames ())
			applied.Push (Key (k));
	}
	// 2. element ID of the next placed element
	if (auto id = OptString (fields, "elementId")) {
		delete memo->elemInfoString;
		memo->elemInfoString = new GS::UniString (*id);
		memoChanged = true;
		applied.Push ("elementId");
	}
	// 3. library part + GDL parameters (handled here: the adapters work on placed elements)
	const std::optional<API_LibTypeID> libType = LibTypeOf (type.typeID);
	const bool isObject = type.typeID == API_ObjectID || type.typeID == API_LampID;
	OS gdlValues;
	GS::String gdlKeyGiven;
	for (const char* k : { "params", "gdlParams", "gdlParameters", "stampParameters" }) {
		if (Has (fields, k)) {
			if (!gdlKeyGiven.IsEmpty ())
				Fail ("Give the GDL parameter values in one field only ('" + Key (gdlKeyGiven) + "' and '" + Key (k) + "' were both given).");
			gdlValues = GetObject (fields, k);
			gdlKeyGiven = k;
		}
	}
	const bool hasParams = !gdlKeyGiven.IsEmpty ();
	GS::String libKeyGiven;
	if (Has (fields, "libraryPart"))
		libKeyGiven = "libraryPart";
	else if (type.typeID == API_ZoneID && Has (fields, "stamp"))
		libKeyGiven = "stamp";
	if (libType.has_value ()) {
		if (!libKeyGiven.IsEmpty ()) {
			const API_LibPart lp = FindLibPart (fields, libKeyGiven.ToCStr (), *libType);
			double a = 0.0, b = 0.0;
			API_AddParType** newParams = ChangeParamsWithScriptForLibPart (lp.index, type, hasParams ? gdlValues : OS (), &a, &b);
			if (memo->params != nullptr)
				ACAPI_DisposeAddParHdl (&memo->params);
			memo->params = newParams;
			*LibIndField (element) = lp.index;
			MaskLibInd (mask, type.typeID);
			if (isObject) {
				element.object.xRatio = a;
				element.object.yRatio = b;
				ACAPI_ELEMENT_MASK_SET (mask, API_ObjectType, xRatio);
				ACAPI_ELEMENT_MASK_SET (mask, API_ObjectType, yRatio);
			}
			memoChanged = true;
			applied.Push (Key (libKeyGiven));
			if (hasParams)
				applied.Push (Key (gdlKeyGiven));
		} else if (hasParams) {
			if (memo->params == nullptr)
				Fail ("The " + ElemTypeName (type) + " tool defaults have no GDL parameters (no default library part): set '" +
					  Key (LibPartKeyFor (type.typeID)) + "' first.", APIERR_BADPARS);
			ApplyParamValues (memo->params, gdlValues);
			if (isObject) {
				if (const API_AddParType* pa = FindParam (memo->params, "A")) { element.object.xRatio = pa->value.real; ACAPI_ELEMENT_MASK_SET (mask, API_ObjectType, xRatio); }
				if (const API_AddParType* pb = FindParam (memo->params, "B")) { element.object.yRatio = pb->value.real; ACAPI_ELEMENT_MASK_SET (mask, API_ObjectType, yRatio); }
			}
			memoChanged = true;
			applied.Push (Key (gdlKeyGiven));
		}
		if (isObject) {
			struct SizeField { const char* key; const char* param; };
			for (const SizeField& f : { SizeField { "sizeA", "A" }, SizeField { "sizeB", "B" }, SizeField { "height", "ZZYZX" } }) {
				auto v = OptDouble (fields, f.key);
				if (!v.has_value ())
					continue;
				if (*v <= 0.0)
					Fail ("'" + Key (f.key) + "' must be positive (meters).");
				const GS::String key (f.key);
				if (key == "sizeA") { element.object.xRatio = *v; ACAPI_ELEMENT_MASK_SET (mask, API_ObjectType, xRatio); }
				if (key == "sizeB") { element.object.yRatio = *v; ACAPI_ELEMENT_MASK_SET (mask, API_ObjectType, yRatio); }
				API_AddParType* p = FindParam (memo->params, f.param);
				if (p != nullptr && p->typeMod != API_ParArray && p->typeID != APIParT_CString) {
					p->value.real = *v;
					memoChanged = true;
				} else if (key == "height") {
					Fail ("The default library part has no ZZYZX (height) parameter; set its height parameter through 'params'.", APIERR_BADPARS);
				}
				applied.Push (Key (f.key));
			}
		}
	} else if (Has (fields, "libraryPart") || hasParams) {
		Fail (ElemTypeName (type) + " is not a library-part based type: 'libraryPart' / GDL parameter values do not apply.", APIERR_BADPARS);
	}

	// 4. type-specific fields through the adapter (per-field fallback so one unsupported field does not block the rest)
	const OS adapterPatch = CopyWithout (fields, [&] (const GS::String& k) {
		if (InList (k, { "layer", "renovationStatus", "drawIndex", "elementId", "classifications", "categories", "properties" }))
			return true;
		if (libType.has_value () && (IsGdlKey (k) || k == libKeyGiven))
			return true;
		if (isObject && InList (k, { "sizeA", "sizeB", "height" }))
			return true;
		if (IsIgnoredDefaultsField (k)) {
			ignored.Push (Key (k));
			return true;
		}
		return false;
	});
	if (!adapterPatch.IsEmpty ()) {
		const ElementAdapter* adapter = FindAdapter (type.typeID);
		if (adapter == nullptr || !adapter->modify) {
			for (const GS::String& k : adapterPatch.GetFieldNames ())
				rejected.Push (OS ("field", Key (k), "error", GS::UniString ("No type-specific adapter for " + ElemTypeName (type) +
								   " yet: only layer, renovationStatus, drawIndex, elementId, params, libraryPart, classifications, categories and properties can be set.")));
		} else {
			API_AddParType** adapterParams = nullptr;
			const API_Element baseElement = element;
			const API_Element baseMask = mask;
			auto err = Attempt ([&] () { ApplyAdapterFields (adapter, element, mask, adapterPatch, adapterParams, notes); });
			if (!err.has_value ()) {
				for (const GS::String& k : adapterPatch.GetFieldNames ())
					applied.Push (Key (k));
			} else {
				// find the fields that work on their own, then apply those together
				element = baseElement;
				mask = baseMask;
				if (adapterParams != nullptr)
					ACAPI_DisposeAddParHdl (&adapterParams);
				GS::Array<GS::String> good;
				for (const GS::String& k : adapterPatch.GetFieldNames ()) {
					OS single;
					CopyField (adapterPatch, k, single);
					API_Element e = baseElement;
					API_Element m = baseMask;
					API_AddParType** p = nullptr;
					GS::Array<GS::UniString> n;
					auto e1 = Attempt ([&] () { ApplyAdapterFields (adapter, e, m, single, p, n); });
					if (p != nullptr)
						ACAPI_DisposeAddParHdl (&p);
					if (e1.has_value ())
						rejected.Push (OS ("field", Key (k), "error", *e1));
					else
						good.Push (k);
				}
				if (!good.IsEmpty ()) {
					OS goodPatch;
					for (const GS::String& k : good)
						CopyField (adapterPatch, k, goodPatch);
					auto e2 = Attempt ([&] () { ApplyAdapterFields (adapter, element, mask, goodPatch, adapterParams, notes); });
					if (e2.has_value ()) {
						element = baseElement;
						mask = baseMask;
						for (const GS::String& k : good)
							rejected.Push (OS ("field", Key (k), "error", *e2));
					} else {
						for (const GS::String& k : good)
							applied.Push (Key (k));
					}
				}
			}
			if (adapterParams != nullptr) {
				if (memo->params != nullptr)
					ACAPI_DisposeAddParHdl (&memo->params);
				memo->params = adapterParams;
				memoChanged = true;
			}
		}
	}

	// 4b. doors / windows / skylights: keep the opening size (struct) and the GDL A/B parameters in step.
	//     Explicit GDL A/B values win; otherwise the (new or kept) width/height is written into A/B.
	if (API_OpeningBaseType* base = OpeningBaseOf (element); base != nullptr && memo->params != nullptr &&
		(!libKeyGiven.IsEmpty () || hasParams || Has (fields, "width") || Has (fields, "height"))) {
		struct SizeParam { const char* name; double* value; bool isWidth; };
		for (const SizeParam& sp : { SizeParam { "A", &base->width, true }, SizeParam { "B", &base->height, false } }) {
			API_AddParType* p = FindParam (memo->params, sp.name);
			if (p == nullptr || p->typeMod == API_ParArray || p->typeID == APIParT_CString)
				continue;
			const bool explicitParam = hasParams && (gdlValues.Contains (sp.name) || gdlValues.Contains (sp.isWidth ? "a" : "b"));
			if (explicitParam) {
				if (p->value.real > 0.0) {
					*sp.value = p->value.real;
					MaskOpeningSize (mask, type.typeID, sp.isWidth, !sp.isWidth);
				}
			} else if (*sp.value > 0.0 && std::fabs (p->value.real - *sp.value) > 1e-9) {
				p->value.real = *sp.value;
				memoChanged = true;
			}
		}
	}

	// 5. write the defaults
	bool anyMasked = false;
	{
		const char* bytes = reinterpret_cast<const char*> (&mask);
		for (size_t i = 0; i < sizeof (mask) && !anyMasked; ++i)
			anyMasked = bytes[i] != 0;
	}
	if (anyMasked || memoChanged) {
		const GSErrCode err = CallWithUndoFallback ("Set tool defaults (Claude)", [&] () {
			return ACAPI_Element_ChangeDefaults (&element, memoChanged ? memo.Ptr () : nullptr, &mask);
		});
		Check (err, "Cannot change the " + ElemTypeName (type) + " tool defaults");
	}

	// 6. classifications / categories / properties of the default element
	if (Has (fields, "classifications")) {
		for (const API_Guid& item : GetClassificationItemGuids (fields, "classifications")) {
			const GSErrCode e = CallWithUndoFallback ("Set default classification (Claude)", [&] () {
				return ACAPI_Element_AddClassificationItemDefault (type, item);
			});
			if (e != NoError)
				rejected.Push (OS ("field", GS::UniString ("classifications"), "error", GS::UniString ("Item " + GuidStr (item) + ": " + ErrorName (e))));
		}
		applied.Push ("classifications");
	}
	OS categories;
	if (TryGetObject (fields, "categories", categories)) {
		for (const API_ElemCategoryValue& v : ResolveCategoryValues (categories)) {
			const GSErrCode e = CallWithUndoFallback ("Set default category (Claude)", [&] () {
				return ACAPI_Element_SetCategoryValueDefault (type, v.category, v);
			});
			if (e != NoError)
				rejected.Push (OS ("field", GS::UniString ("categories"), "error", GS::UniString (GS::UniString (v.category.name) + ": " + ErrorName (e))));
		}
		applied.Push ("categories");
	}
	OS properties;
	if (TryGetObject (fields, "properties", properties) && !properties.IsEmpty ()) {
		GS::Array<API_PropertyDefinition> defs;
		Check (ACAPI_Element_GetPropertyDefinitionsOfDefaultElem (type, API_PropertyDefinitionFilter_UserDefined, defs),
			   "Cannot read the property definitions of the " + ElemTypeName (type) + " defaults");
		PropertyGroupNames groups;
		GS::Array<API_Property> props;
		for (const GS::String& k : properties.GetFieldNames ()) {
			auto e = Attempt ([&] () {
				const API_PropertyDefinition& def = FindPropertyDefinition (defs, Key (k), groups);
				props.Push (PropertyFromJson (def, properties, k.ToCStr ()));
			});
			if (e.has_value ())
				rejected.Push (OS ("field", GS::UniString ("properties." + Key (k)), "error", *e));
		}
		if (!props.IsEmpty ()) {
			const GSErrCode e = CallWithUndoFallback ("Set default properties (Claude)", [&] () {
				return ACAPI_Element_SetPropertiesOfDefaultElem (type, props);
			});
			if (e != NoError)
				rejected.Push (OS ("field", GS::UniString ("properties"), "error", GS::UniString ("Cannot set the default property values: " + ErrorName (e))));
			else
				applied.Push ("properties");
		}
	}

	OS out;
	AddTypeJson (out, type);
	out.Add ("applied", applied);
	if (!ignored.IsEmpty ()) {
		out.Add ("ignored", ignored);
		notes.Push ("Ignored fields are per-element geometry/identity (coordinates, polygon, story, GUID) that tool defaults do not store.");
	}
	if (!rejected.IsEmpty ())
		out.Add ("rejected", rejected);
	if (!notes.IsEmpty ())
		out.Add ("notes", notes);
	if (returnSettings) {
		OS opts ("includeGdlParameters", false, "includeClassifications", false, "includeCategories", false);
		OS now = ToolDefaultsJson (type, opts);
		OS settings;
		if (now.Get ("settings", settings))
			out.Add ("settings", settings);
	}
	return out;
}


void RegisterToolDefaultCommands ()
{
	RegisterCommand ("GetToolDefaults",
		"Returns the default settings of element tools (what the next placed element gets). Input: {type? | types?: [type | {type, variation}], "
		"variation?, includeGdlParameters?: true, gdlParameterNames?: [..], includeHiddenGdlParameters?: false, includeClassifications?: true, "
		"includeCategories?: true, includeProperties?: false (custom properties with a non-default value), includeAllProperties?: false}. "
		"Without type: lists the toolbox tools {tools: [{type, variation?}], activeTool}. Output: {defaults: [{type, variation?, settings: "
		"{layer, renovationStatus, drawIndex, elementId?, ...same fields as the create_* tools, libraryPart?, params?}, classifications?, "
		"categories?, properties?, notes?} | {error}]}.",
		[] (const OS& params) -> OS {
			const GS::Array<API_ElemType> types = RequestedTypes (params);
			if (types.IsEmpty ())
				return ToolboxJson ();
			GS::Array<OS> out;
			for (const API_ElemType& t : types)
				out.Push (Try ([&] () { return ToolDefaultsJson (t, params); }));
			return OS ("defaults", out);
		});

	RegisterCommand ("SetToolDefaults",
		"Changes tool default settings (like the tool's Default Settings dialog) so the next elements drawn in Archicad or created without "
		"explicit values get them. Input: {defaults: [{type, variation?, fields: {...same fields as the type's create_* tool (geometry is ignored), "
		"layer, renovationStatus, drawIndex, elementId (ID of the next element), libraryPart + params (library part based tools), "
		"classifications: [itemGuid], categories: {StructuralFunction|Position|...: localized value}, properties: {guid|name|'Group/Name': value}}}], "
		"returnSettings?: true}. Output: {results: [{type, applied: [field], ignored?, rejected?: [{field, error}], notes?, settings?} | {error}]}.",
		[] (const OS& params) -> OS {
			const GS::Array<OS> items = GetObjectArray (params, "defaults");
			const bool returnSettings = GetBool (params, "returnSettings", true);
			GS::Array<OS> results;
			for (const OS& item : items)
				results.Push (Try ([&] () { return SetOneToolDefaults (item, returnSettings); }));
			return OS ("results", results);
		});
}

// =============================================================================
// 7. Revisions (document revisions, revision issues, changes) — read-only
// =============================================================================

OS CustomDataJson (const GS::HashTable<API_Guid, GS::UniString>& data, const GS::HashTable<API_Guid, GS::UniString>& scheme)
{
	OS out;
	for (const auto& pair : data) {
		GS::UniString name;
		if (!scheme.Get (*pair.key, &name) || name.IsEmpty ())
			name = GuidStr (*pair.key);
		const GS::String key = ToStr (name);
		if (!out.Contains (key))
			out.Add (key, *pair.value);
	}
	return out;
}


GS::HashTable<API_Guid, GS::UniString> Scheme (API_DatabaseID id)
{
	GS::HashTable<API_Guid, GS::UniString> scheme;
	if (ACAPI_Database (id, &scheme) != NoError)
		scheme.Clear ();
	return scheme;
}


OS RvmIssueJson (const API_RVMIssue& issue, const GS::HashTable<API_Guid, GS::UniString>& scheme)
{
	OS out;
	out.Add ("guid", GuidStr (issue.guid));
	out.Add ("id", issue.id);
	out.Add ("description", issue.description);
	out.Add ("issued", issue.issued);
	out.Add ("issueTime", TimeString (issue.issueTime));
	out.Add ("issuedBy", issue.issuedByUser);
	out.Add ("overrideRevisionId", issue.isOverrideRevisionId);
	out.Add ("createNewRevision", issue.isCreateNewRevision);
	out.Add ("visibleMarkersInIssues", (Int32) issue.visibleMarkersInIssues);
	out.Add ("customFields", CustomDataJson (issue.customData, scheme));
	return out;
}


OS RvmChangeJson (const API_RVMChange& change, const GS::HashTable<API_Guid, GS::UniString>& scheme, bool firstIssue,
				  const GS::HashTable<API_Guid, GS::UniString>& issueScheme)
{
	OS out;
	out.Add ("id", change.id);
	out.Add ("description", change.description);
	out.Add ("lastModified", TimeString (change.lastModifiedTime));
	out.Add ("modifiedBy", change.modifiedByUser);
	out.Add ("issued", change.issued);
	out.Add ("archived", change.archived);
	out.Add ("customFields", CustomDataJson (change.customData, scheme));
	if (firstIssue) {
		API_RVMIssue issue;
		issue.guid = APINULLGuid;
		GS::UniString id = change.id;
		if (ACAPI_Database (APIDb_GetRVMChangeFirstIssueID, &id, &issue) == NoError && issue.guid != APINULLGuid)
			out.Add ("firstIssue", RvmIssueJson (issue, issueScheme));
	}
	return out;
}


OS LayoutInfoJson (const API_RVMLayoutInfo& l)
{
	OS out;
	out.Add ("id", l.id);
	out.Add ("name", l.name);
	out.Add ("databaseGuid", GuidStr (l.dbId.elemSetId));
	out.Add ("masterLayout", l.masterLayoutName);
	out.Add ("width", l.width);
	out.Add ("height", l.height);
	out.Add ("drawingScales", l.drawingScales);
	out.Add ("subsetId", l.subsetId);
	out.Add ("subsetName", l.subsetName);
	if (l.teamworkOwner > 0)
		out.Add ("teamworkOwner", UserLabel (l.teamworkOwner));
	out.Add ("customFields", CustomDataJson (l.customData, GS::HashTable<API_Guid, GS::UniString> ()));
	return out;
}


GS::Array<API_RVMIssue> RvmIssues ()
{
	GS::Array<API_RVMIssue> issues;
	Check (ACAPI_Database (APIDb_GetRVMIssuesID, &issues), "Cannot read the revision issues");
	return issues;
}


const API_RVMIssue& FindRvmIssue (const GS::Array<API_RVMIssue>& issues, const GS::UniString& ref)
{
	API_Guid g = APINULLGuid;
	const bool byGuid = ParseGuidText (ref, g);
	for (const API_RVMIssue& i : issues) {
		if ((byGuid && i.guid == g) || (!byGuid && EqualsIgnoreCase (i.id, ref)))
			return i;
	}
	GS::Array<GS::UniString> ids;
	for (const API_RVMIssue& i : issues)
		ids.Push ("'" + i.id + "'");
	Fail ("Revision issue '" + ref + "' not found. " + (ids.IsEmpty () ? GS::UniString ("The project has no revision issues.") : "Existing issue IDs: " + Join (ids) + "."), APIERR_BADNAME);
}


struct LayoutRef {
	API_DatabaseUnId	dbId;
	GS::UniString		name;
	GS::UniString		id;
};


GS::Array<LayoutRef> AllLayouts ()
{
	GS::Array<API_DatabaseUnId> dbs;
	GS::Array<LayoutRef> out;
	if (ACAPI_Database (APIDb_GetLayoutDatabasesID, nullptr, &dbs) != NoError)
		return out;
	for (const API_DatabaseUnId& db : dbs) {
		API_DatabaseInfo info;
		BNZeroMemory (&info, sizeof (info));
		info.typeID = APIWind_LayoutID;
		info.databaseUnId = db;
		LayoutRef r;
		r.dbId = db;
		if (ACAPI_Database (APIDb_GetDatabaseInfoID, &info) == NoError) {
			r.name = GS::UniString (info.name);
			r.id = GS::UniString (info.ref);
		}
		out.Push (r);
	}
	return out;
}


LayoutRef FindLayout (const GS::Array<LayoutRef>& layouts, const GS::UniString& ref)
{
	API_Guid g = APINULLGuid;
	const bool byGuid = ParseGuidText (ref, g);
	for (const LayoutRef& l : layouts) {
		if (byGuid && l.dbId.elemSetId == g)
			return l;
		if (!byGuid && (EqualsIgnoreCase (l.name, ref) || EqualsIgnoreCase (l.id, ref) || EqualsIgnoreCase (l.id + " " + l.name, ref)))
			return l;
	}
	GS::Array<GS::UniString> names;
	for (UIndex i = 0; i < layouts.GetSize () && i < 30; ++i)
		names.Push ("'" + layouts[i].id + " " + layouts[i].name + "'");
	Fail ("Layout '" + ref + "' not found. " + (names.IsEmpty () ? GS::UniString ("The project has no layouts.") : "Layouts: " + Join (names) + "."), APIERR_BADNAME);
}


void RegisterRevisionCommands ()
{
	RegisterCommand ("GetRevisions",
		"Read-only revision management data (Document > Issue Manager / Change Manager). Input: {include?: ['issues','documentRevisions'] "
		"(default both), issue?: revision issue guid or ID (only the document revisions of that issue)}. Output: {issues: [{guid, id, description, "
		"issued, issueTime, issuedBy, overrideRevisionId, createNewRevision, visibleMarkersInIssues, customFields, documentRevisionCount}], "
		"documentRevisions: [{guid, id, finalId, status: Actual|Issued, issue?: {guid, id}, layout: {id, name, databaseGuid, masterLayout, width, "
		"height, drawingScales, subsetId, subsetName, customFields}}]}.",
		[] (const OS& params) -> OS {
			GS::Array<GS::UniString> include = GetStringArray (params, "include");
			auto wants = [&] (const char* what) {
				if (include.IsEmpty ())
					return true;
				for (const GS::UniString& s : include)
					if (EqualsIgnoreCase (s, what)) return true;
				return false;
			};
			for (const GS::UniString& s : include) {
				if (!EqualsIgnoreCase (s, "issues") && !EqualsIgnoreCase (s, "documentRevisions"))
					Fail ("'include' items must be 'issues' or 'documentRevisions', got '" + s + "'.");
			}
			const GS::Array<API_RVMIssue> issues = RvmIssues ();
			const GS::HashTable<API_Guid, GS::UniString> issueScheme = Scheme (APIDb_GetRVMIssueCustomSchemeID);

			// revision -> issue
			GS::HashTable<API_Guid, API_Guid> revisionIssue;
			GS::HashTable<API_Guid, Int32> revisionCounts;
			for (const API_RVMIssue& i : issues) {
				GS::Array<API_RVMDocumentRevision> revs;
				API_Guid guid = i.guid;
				Int32 n = 0;
				if (ACAPI_Database (APIDb_GetRVMIssueDocumentRevisionsID, &guid, &revs) == NoError) {
					for (const API_RVMDocumentRevision& r : revs) {
						revisionIssue.Put (r.guid, i.guid);
						++n;
					}
				}
				revisionCounts.Put (i.guid, n);
			}

			OS out;
			if (wants ("issues")) {
				GS::Array<OS> list;
				for (const API_RVMIssue& i : issues) {
					OS j = RvmIssueJson (i, issueScheme);
					Int32 n = 0;
					revisionCounts.Get (i.guid, &n);
					j.Add ("documentRevisionCount", n);
					list.Push (j);
				}
				out.Add ("issues", list);
			}
			if (wants ("documentRevisions")) {
				GS::Array<API_RVMDocumentRevision> revs;
				if (auto ref = OptRef (params, "issue")) {
					const API_RVMIssue& issue = FindRvmIssue (issues, *ref);
					API_Guid guid = issue.guid;
					Check (ACAPI_Database (APIDb_GetRVMIssueDocumentRevisionsID, &guid, &revs), "Cannot read the document revisions of issue " + issue.id);
				} else {
					Check (ACAPI_Database (APIDb_GetRVMDocumentRevisionsID, &revs), "Cannot read the document revisions");
				}
				GS::Array<OS> list;
				for (const API_RVMDocumentRevision& r : revs) {
					OS j;
					j.Add ("guid", GuidStr (r.guid));
					j.Add ("id", r.id);
					j.Add ("finalId", r.finalId);
					j.Add ("status", GS::UniString (r.status == API_RVMDocumentRevisionStatusIssued ? "Issued" : "Actual"));
					if (r.userId > 0)
						j.Add ("owner", UserLabel (r.userId));
					API_Guid issueGuid = APINULLGuid;
					if (revisionIssue.Get (r.guid, &issueGuid)) {
						for (const API_RVMIssue& i : issues) {
							if (i.guid == issueGuid)
								j.Add ("issue", OS ("guid", GuidStr (i.guid), "id", i.id));
						}
					}
					j.Add ("layout", LayoutInfoJson (r.layoutInfo));
					list.Push (j);
				}
				out.Add ("documentRevisions", list);
			}
			return out;
		});

	RegisterCommand ("GetRevisionChanges",
		"Read-only Change Manager data. Input (one mode; default = all changes): {documentRevision?: guid | layouts?: [layout database guid | layout ID | "
		"name] | allLayouts?: true (current revision changes of every layout) | elements?: [guid] | changeIds?: [id], includeFirstIssue?: false}. "
		"Output: {changes: [{id, description, lastModified, modifiedBy, issued, archived, customFields, firstIssue?}]} or "
		"{layouts: [{layout: {databaseGuid, id, name}, changes}]} or {elements: [{guid, changeIds, changes}]}.",
		[] (const OS& params) -> OS {
			const GS::HashTable<API_Guid, GS::UniString> scheme = Scheme (APIDb_GetRVMChangeCustomSchemeID);
			const GS::HashTable<API_Guid, GS::UniString> issueScheme = Scheme (APIDb_GetRVMIssueCustomSchemeID);
			const bool firstIssue = GetBool (params, "includeFirstIssue", false);
			auto changesJson = [&] (const GS::Array<API_RVMChange>& changes) {
				GS::Array<OS> list;
				for (const API_RVMChange& c : changes)
					list.Push (RvmChangeJson (c, scheme, firstIssue, issueScheme));
				return list;
			};
			UInt32 modes = 0;
			for (const char* k : { "documentRevision", "layouts", "allLayouts", "elements", "changeIds" })
				modes += Has (params, k) ? 1 : 0;
			if (modes > 1)
				Fail ("Give only one of documentRevision, layouts, allLayouts, elements, changeIds.");

			if (Has (params, "documentRevision")) {
				API_Guid rev = GetGuid (params, "documentRevision");
				GS::Array<API_RVMChange> changes;
				Check (ACAPI_Database (APIDb_GetRVMDocumentRevisionChangesID, &rev, &changes),
					   "Cannot read the changes of document revision " + GuidStr (rev) + " (get the GUID from get_revisions)");
				return OS ("documentRevision", GuidStr (rev), "changes", changesJson (changes));
			}
			if (Has (params, "layouts") || GetBool (params, "allLayouts", false)) {
				const GS::Array<LayoutRef> all = AllLayouts ();
				GS::Array<LayoutRef> selected;
				if (Has (params, "layouts")) {
					for (const GS::UniString& ref : GetRefArray (params, "layouts", true))
						selected.Push (FindLayout (all, ref));
				} else {
					selected = all;
				}
				GS::Array<OS> list;
				for (const LayoutRef& l : selected) {
					OS item ("layout", OS ("databaseGuid", GuidStr (l.dbId.elemSetId), "id", l.id, "name", l.name));
					GS::Array<API_RVMChange> changes;
					API_DatabaseUnId db = l.dbId;
					const GSErrCode err = ACAPI_Database (APIDb_GetRVMLayoutCurrentRevisionChangesID, &db, &changes);
					if (err != NoError)
						item.Add ("error", OS ("code", (Int32) err, "message", GS::UniString ("Cannot read the current revision changes: " + ErrorName (err))));
					else
						item.Add ("changes", changesJson (changes));
					list.Push (item);
				}
				return OS ("layouts", list);
			}
			if (Has (params, "elements")) {
				GS::Array<OS> list;
				for (const API_Guid& g : GetGuidArray (params, "elements", true)) {
					list.Push (Try ([&] () -> OS {
						if (!ElementExists (g))
							Fail ("Element " + GuidStr (g) + " does not exist.", APIERR_BADID);
						GS::Array<GS::UniString> ids;
						API_Guid guid = g;
						Check (ACAPI_Database (APIDb_GetRVMElemChangeIdsID, &guid, &ids), "Cannot read the change IDs of element " + GuidStr (g));
						GS::Array<API_RVMChange> changes;
						if (!ids.IsEmpty ())
							Check (ACAPI_Database (APIDb_GetRVMChangesFromChangeIdsID, &ids, &changes), "Cannot read changes");
						return OS ("guid", GuidStr (g), "changeIds", ids, "changes", changesJson (changes));
					}));
				}
				return OS ("elements", list);
			}
			GS::Array<API_RVMChange> changes;
			if (Has (params, "changeIds")) {
				GS::Array<GS::UniString> ids = GetStringArray (params, "changeIds", true);
				Check (ACAPI_Database (APIDb_GetRVMChangesFromChangeIdsID, &ids, &changes), "Cannot read the given changes");
			} else {
				Check (ACAPI_Database (APIDb_GetRVMChangesID, &changes), "Cannot read the changes");
			}
			return OS ("changes", changesJson (changes), "count", (Int32) changes.GetSize ());
		});
}

// =============================================================================
// 8. Teamwork command registration
// =============================================================================

void RegisterTeamworkCommands ()
{
	RegisterCommand ("GetTeamworkStatus",
		"Teamwork status. Input: {elements?: [guid] (reservation status per element), objectSets?: true (all) | [LayerSettings|LineTypes|FillTypes|"
		"Composites|PenSets|Surfaces|BuildingMaterials|ZoneCategories|Profiles|MEPSystems|OperationProfiles|ModelViewOptions|Favorites|IssueTags|"
		"ClassificationsAndProperties|ProjectInfo|ProjectPreferences|ProjectLibraryList], includeMembers?: true, includeAccessRights?: false}. "
		"Solo project: {isTeamwork: false, message, project}. Teamwork: {isTeamwork: true, project, teamwork: {online, hasConnection, serverUrl, "
		"teamProjectName, loginName}, currentUser, members, elements?: [{guid, status: Free|ReservedByMe|ReservedByOther|ServerUnavailable|NotExist, "
		"reservedBy?}], objectSets?, accessRights?}.",
		[] (const OS& params) -> OS { return GetTeamworkStatus (params); });

	RegisterCommand ("TeamworkSend",
		"Teamwork: sends your changes to the BIMcloud/BIMserver (Send). Input: {comment?}. Solo projects return {isTeamwork: false, message}.",
		[] (const OS& params) -> OS {
			const ProjectState s = GetProjectState ();
			if (!s.teamwork) {
				OS out = SoloResult (s);
				out.Add ("sent", false);
				return out;
			}
			RequireOnline ();
			Check (ACAPI_TeamworkControl_SendChanges (GetString (params, "comment", GS::UniString ())), "Teamwork Send failed");
			return OS ("isTeamwork", true, "sent", true);
		});

	RegisterCommand ("TeamworkReceive",
		"Teamwork: receives the changes of other team members (Receive). Input: {}. Solo projects return {isTeamwork: false, message}.",
		[] (const OS&) -> OS {
			const ProjectState s = GetProjectState ();
			if (!s.teamwork) {
				OS out = SoloResult (s);
				out.Add ("received", false);
				return out;
			}
			RequireOnline ();
			Check (ACAPI_TeamworkControl_ReceiveChanges (), "Teamwork Receive failed");
			return OS ("isTeamwork", true, "received", true);
		});

	RegisterCommand ("ReserveElements",
		"Teamwork: reserves elements and/or object sets (attribute types, favorites, project info...). Input: {elements?: [guid], "
		"objectSets?: [name], hotlinkCacheManagement?: false, enableDialogs?: false}. Output: {elements: [{guid, status, reserved, "
		"conflictWith?, reservedBy?}], objectSets: [{name, status, reserved}]}. Solo projects return {isTeamwork: false, message}.",
		[] (const OS& params) -> OS { return ReserveOrRelease (params, true); });

	RegisterCommand ("ReleaseElements",
		"Teamwork: releases reserved elements and/or object sets (your changes should be sent first, or they are sent with the release). "
		"Input: {elements?: [guid], objectSets?: [name], hotlinkCacheManagement?: false, enableDialogs?: false}. Output like ReserveElements "
		"with 'released'. Solo projects return {isTeamwork: false, message}.",
		[] (const OS& params) -> OS { return ReserveOrRelease (params, false); });
}

} // namespace


void RegisterCollaborationCommands ()
{
	RegisterTeamworkCommands ();
	RegisterIssueCommands ();
	RegisterFavoriteCommands ();
	RegisterToolDefaultCommands ();
	RegisterRevisionCommands ();
}

} // namespace cc
