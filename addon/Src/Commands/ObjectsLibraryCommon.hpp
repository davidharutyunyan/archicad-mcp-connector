// *****************************************************************************
// ObjectsLibraryCommon.hpp — private helpers of the "objects-library" command
// family (objects, lamps, libraries, library parts, GDL parameters).
// Implemented in (and only used by) Commands/ObjectsLibrary.cpp.
// *****************************************************************************

#pragma once

#include "Core/Json.hpp"
#include "Core/LibParts.hpp"

#include "Location.hpp"

#include <vector>

namespace cc {
namespace objlib {

// --- Sub-family registration (called from RegisterObjectLibraryCommands) ----------

void			RegisterObjectLampAdapters ();		// Object / Lamp element adapters
void			RegisterGdlParameterCommands ();	// GetGdlParameters, SetGdlParameters, ChangeLibraryPart
void			RegisterLibraryPartCommands ();		// SearchLibraryParts, GetLibraryPartSubtypes, ...Details, ...Scripts, CreateLibraryPart
void			RegisterLibraryCommands ();			// GetLibraries, AddLibraries, RemoveLibraries, ReloadLibraries

// --- Generic helpers -----------------------------------------------------------------

GS::UniString	LocationPath (const IO::Location& loc);
// Runs fn directly; when it fails with APIERR_NEEDSUNDOSCOPE, runs it again inside Undoable (undoName).
void			RunWithUndoFallback (const GS::UniString& undoName, const std::function<void ()>& fn);
// Copies one field (any JSON type) from src[srcKey] to dst[dstKey] (dst must not contain dstKey).
void			CopyField (const OS& src, const GS::String& srcKey, OS& dst, const GS::String& dstKey);
GS::UniString	FourCC (GSType type);

struct ListKindCounts {
	UInt32 numbers = 0;
	UInt32 strings = 0;
	UInt32 objects = 0;
	UInt32 bools = 0;
};
// Counts the item kinds of the JSON array os[key].
ListKindCounts	CountListKinds (const OS& os, const char* key);
GS::UniString	JoinNames (const GS::Array<GS::UniString>& names, const char* sep = ", ");
// Case-insensitive substring test (GS::UniString::Contains has no case parameter!).
bool			ContainsNoCase (const GS::UniString& haystack, const GS::UniString& needle);

// --- Library part helpers ----------------------------------------------------------------

// ACAPI_LibPart_Get + location cleanup. Returns the API error (APIERR_MISSINGDEF still fills most fields).
GSErrCode		FetchLibPart (Int32 index, API_LibPart& lp, GS::UniString* path = nullptr, IO::Location* loc = nullptr);
// Main part of a library part unique ID ("{MAIN}-{REV}" -> "{MAIN}"), uppercase.
GS::UniString	MainGuidOf (const char* unId);
std::optional<API_LibTypeID> ParseLibType (const GS::UniString& name);
// Real kind of a library part: AC26 reports typeID = Object for windows, doors, lamps, labels, zone stamps ...;
// this resolves it from the subtype ancestry (Light -> Lamp, Door (Wall) -> Door, Window (Wall) -> Window ...).
API_LibTypeID	EffectiveLibType (const API_LibPart& lp);
GS::UniString	AllowedLibTypeNames ();
// Library part type an element of the given type expects (Object -> Object, Door -> Door, Zone -> Zone stamp ...).
std::optional<API_LibTypeID> LibTypeForElemType (API_ElemTypeID typeID);
// Element type used as parameter owner for a library part type (Object for unknown types).
API_ElemTypeID	ElemTypeForLibType (API_LibTypeID libType);

// FindLibPart restricted to a library part type; when the name exists with another type the error says which tool to use.
API_LibPart		FindLibPartForElem (const OS& os, const char* key, API_ElemTypeID elemType);

// Compact reference {index, name, guid, fileName, type} or {index, missing: true}.
OS				LibPartRefJson (Int32 libInd);
// Unique ID string of the library part of a placed element ("" when not GDL based).
GS::UniString	ElementLibPartUnId (const API_Elem_Head& head);
// Library part index of a placed element (0 when not GDL based).
Int32			ElementLibInd (const API_Element& element);

struct LibPartScanItem {
	Int32			index = 0;
	API_LibTypeID	typeID = API_ZombieLibID;
	GS::UniString	name;
	GS::UniString	fileName;
	GS::UniString	ownUnId;
	GS::UniString	parentUnId;
	GS::UniString	mainGuid;
	GS::UniString	parentMainGuid;
	bool			isTemplate = false;
	bool			isPlaceable = false;
	bool			missingDef = false;
};

// Scans every registered library part (skips parts that cannot be read at all); typeID is the EFFECTIVE type.
std::vector<LibPartScanItem> ScanAllLibParts ();

// --- GDL parameter helpers ---------------------------------------------------------------

Int32				ParamCount (API_AddParType** params);
API_AddParType*		FindParamByName (API_AddParType** params, const GS::UniString& name);
// Compact {name: value} object of the visible scalar parameters (at most maxCount entries).
OS					ParamsSummary (API_AddParType** params, UInt32 maxCount, Int32* shownCount);
// Replaces memo.params (disposes the previous handle) — takes ownership of params.
void				ReplaceMemoParams (API_ElementMemo& memo, API_AddParType** params);

API_ParamOwnerType	ElementOwner (const API_Element& element);
API_ParamOwnerType	LibPartOwner (Int32 libInd, API_ElemTypeID elemType);

// Parameter editing session (APIAny_OpenParameters ... APIAny_CloseParameters) that runs the
// parameter script of the library part for every change, like the Archicad settings dialog.
// Only ONE editor may be alive at a time (Archicad supports a single open parameter list).
class ParamEditor {
public:
	explicit ParamEditor (const API_ParamOwnerType& owner);
	~ParamEditor ();
	ParamEditor (const ParamEditor&) = delete;
	ParamEditor& operator= (const ParamEditor&) = delete;

	// Applies {name: value, ...}. Accepted values: numbers (Length m, Angle DEGREES), booleans,
	// strings (String params; attribute NAMES for Surface/BuildingMaterial/LineType/Fill/Profile params),
	// arrays (flat [..] or rows [{values: [..]}]). Unknown names / bad values throw (strict).
	void				Apply (const OS& values);
	// Same for one field; strict=false swallows errors and returns false.
	bool				ApplyOne (const OS& values, const GS::String& key, bool strict);
	void				SetNumber (const GS::UniString& name, double value, bool strict);
	// Copies simple values of same-named, same-typed parameters (not hidden, not 'unique') from source.
	// Names listed in skipUpper (uppercase) are skipped. Returns the number of parameters copied.
	UInt32				CarryOver (API_AddParType** source, const GS::Array<GS::UniString>& skipUpper);
	bool				HasParam (const GS::UniString& name) const;
	API_AddParType**	Definitions () const { return defs.Get (); }

	// Value list info of a parameter (VALUES / LOCK of the parameter script): {values?, locked?, custom?}.
	// Returns false when the parameter has no value list and is not locked.
	bool				GetValueList (const API_AddParType& param, OS& out) const;

	// Returns the resulting parameters (caller owns: dispose or put into memo.params) and A/B.
	API_AddParType**	Finish (double* a, double* b);

private:
	struct Guard {
		bool open = false;
		~Guard ();
	};

	void				ApplyValue (const OS& values, const char* key);
	void				ChangeNumber (const API_AddParType* def, const GS::UniString& name, double value, Int32 ind1, Int32 ind2);
	void				ChangeString (const API_AddParType* def, const GS::UniString& name, const GS::UniString& value, Int32 ind1, Int32 ind2);
	GS::UniString		SimilarNames (const GS::UniString& name) const;

	Guard				guard;		// must be the first member (closes the list if the constructor throws)
	ParamsHandle		defs;
};

// Adds "valueName" (the attribute's localized name) to ParamsToJson items of Surface / BuildingMaterial /
// LineType / FillPattern / Profile parameters.
GS::Array<OS>		AddAttributeNames (API_AddParType** params, GS::Array<OS> items);
// Adds "valueList" / "locked" info to ParamsToJson items (requires an open editor on the same owner).
void				AddValueLists (ParamEditor& editor, API_AddParType** params, GS::Array<OS>& items);

} // namespace objlib
} // namespace cc
