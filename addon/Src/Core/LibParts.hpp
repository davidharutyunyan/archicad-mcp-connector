// *****************************************************************************
// LibParts.hpp — library part lookup and GDL parameter helpers, shared by
// objects, lamps, doors, windows, skylights, openings, labels and zones.
//
// Library part reference (JSON) accepted forms:
//   "Chair 01 26"                       -> exact (then case-insensitive, then substring) name match
//   {"name": "Chair 01 26"}
//   {"guid": "{MAIN-GUID}-{REV-GUID}"}  -> library part unique ID (ownUnID)
//   {"index": 123}                      -> current library part index
// NOTE: library part names are LOCALIZED (e.g. Russian Archicad ships Russian names).
// Always call SearchLibraryParts first to discover the real names.
//
// GDL parameter values (JSON) — object {name: value}:
//   numbers (Length in meters, Angle in DEGREES, Integer/RealNum/Boolean as number or bool),
//   strings for CString / value-list strings, arrays as nested JSON arrays of numbers/strings.
// *****************************************************************************

#pragma once

#include "Core/Json.hpp"

namespace cc {

// Returns the library part (index filled) or throws with suggestions.
API_LibPart		FindLibPart (const OS& os, const char* key, std::optional<API_LibTypeID> requiredType = std::nullopt);
API_LibPart		GetLibPartByIndex (Int32 libInd);
OS				LibPartToJson (const API_LibPart& lp);
GS::UniString	LibTypeName (API_LibTypeID typeID);
bool			IsSubtypeOf (const API_LibPart& lp, API_LibTypeID typeID);	// uses type + ancestry

// Loads default parameters of a library part. Caller owns *params (BMKillHandle) — use ParamsHandle.
class ParamsHandle {
public:
	ParamsHandle () = default;
	~ParamsHandle ();
	ParamsHandle (const ParamsHandle&) = delete;
	ParamsHandle& operator= (const ParamsHandle&) = delete;

	API_AddParType**	Release ();		// caller takes ownership
	API_AddParType**	Get () const	{ return params; }
	void				Reset (API_AddParType** p);

	double				a = 0.0;
	double				b = 0.0;
private:
	API_AddParType**	params = nullptr;
};

void			LoadDefaultParams (Int32 libInd, ParamsHandle& out);

// JSON dump of parameters: [{name, type, description, value, (hidden), (arrayDims)}]
// When `names` is non-empty only those parameters are returned.
GS::Array<OS>	ParamsToJson (API_AddParType** params, bool includeHidden, const GS::Array<GS::UniString>& names = {});

// Sets parameter values in a params handle in place (no parameter script is run).
// values: {"paramName": value, ...}. Unknown names throw (with a hint).
void			ApplyParamValues (API_AddParType** params, const OS& values);

// Changes parameters of placed elements THROUGH Archicad's parameter-script machinery
// (APIAny_OpenParameters / ChangeAParameter / GetActParameters) and returns the resulting
// params handle (caller owns it; put it into memo.params and change with APIMemoMask_AddPars).
API_AddParType** ChangeParamsWithScript (const API_Guid& elemGuid, const API_ElemType& type, Int32 libInd, const OS& values);
// Same, starting from a library part's defaults (for new elements).
API_AddParType** ChangeParamsWithScriptForLibPart (Int32 libInd, const API_ElemType& type, const OS& values, double* outA = nullptr, double* outB = nullptr);

GS::UniString	ParamTypeName (API_AddParID typeID);

} // namespace cc
