// *****************************************************************************
// Json.hpp — helpers for reading/writing GS::ObjectState (the JSON DOM that the
// Archicad JSON API hands to add-on commands).
//
// Conventions used across ALL Claude Connector commands:
//   * lengths are in meters, angles in DEGREES (converted to radians internally)
//   * GUIDs are strings "XXXXXXXX-XXXX-..."; element references accept either
//     "guid-string" or {"guid": "guid-string"} (official JSON API ElementId)
//   * 2D points are {"x":..,"y":..}; 3D points are {"x":..,"y":..,"z":..}
//   * failures throw cc::Error, which the command wrapper turns into
//     {"error": {"code": <int>, "message": <string>}}
// *****************************************************************************

#pragma once

#include "APIEnvir.h"
#include "ACAPinc.h"
#include "ObjectState.hpp"
#include "UniString.hpp"
#include "StringConversion.hpp"

#include <exception>
#include <functional>
#include <optional>

namespace cc {

using OS = GS::ObjectState;

constexpr double kPi = 3.14159265358979323846;

// --- Errors ------------------------------------------------------------------

class Error : public std::exception {
public:
	Error (const GS::UniString& message, GSErrCode code = APIERR_BADPARS) : message (message), code (code) {}
	const char* what () const noexcept override { return "cc::Error"; }

	GS::UniString	message;
	GSErrCode		code;
};

[[noreturn]] void		Fail (const GS::UniString& message, GSErrCode code = APIERR_BADPARS);
// Throws cc::Error("<what>: <APIERR name>") when err != NoError.
void					Check (GSErrCode err, const GS::UniString& what);
GS::UniString			ErrorName (GSErrCode err);
OS						ErrorObject (GSErrCode code, const GS::UniString& message);

// --- String helpers ----------------------------------------------------------

GS::UniString			ToUni (const char* s);
GS::String				ToStr (const GS::UniString& s);
bool					EqualsIgnoreCase (const GS::UniString& a, const GS::UniString& b);

// --- Readers (required variants throw a descriptive error when missing) ------

bool					Has (const OS& os, const char* key);
bool					IsNumber (const OS& os, const char* key);

double					GetDouble (const OS& os, const char* key);
double					GetDouble (const OS& os, const char* key, double def);
std::optional<double>	OptDouble (const OS& os, const char* key);

Int32					GetInt (const OS& os, const char* key);
Int32					GetInt (const OS& os, const char* key, Int32 def);
std::optional<Int32>	OptInt (const OS& os, const char* key);

bool					GetBool (const OS& os, const char* key);
bool					GetBool (const OS& os, const char* key, bool def);
std::optional<bool>		OptBool (const OS& os, const char* key);

GS::UniString			GetString (const OS& os, const char* key);
GS::UniString			GetString (const OS& os, const char* key, const GS::UniString& def);
std::optional<GS::UniString> OptString (const OS& os, const char* key);

OS						GetObject (const OS& os, const char* key);
bool					TryGetObject (const OS& os, const char* key, OS& out);

GS::Array<OS>			GetObjectArray (const OS& os, const char* key);
GS::Array<OS>			GetObjectArray (const OS& os, const char* key, bool required);
GS::Array<GS::UniString> GetStringArray (const OS& os, const char* key, bool required = false);
GS::Array<double>		GetNumberArray (const OS& os, const char* key, bool required = false);

// Angle given in degrees in JSON, returned in radians.
double					GetAngle (const OS& os, const char* key);
double					GetAngle (const OS& os, const char* key, double defRadians);
std::optional<double>	OptAngle (const OS& os, const char* key);

// --- GUIDs / element ids -----------------------------------------------------

API_Guid				ParseGuid (const GS::UniString& str);
// Accepts "guid" or {"guid": "..."} or {"elementId": {"guid": "..."}}
API_Guid				GetGuid (const OS& os, const char* key);
std::optional<API_Guid>	OptGuid (const OS& os, const char* key);
API_Guid				GuidFromItem (const OS& item);
GS::Array<API_Guid>		GetGuidArray (const OS& os, const char* key, bool required = true);
GS::UniString			GuidStr (const API_Guid& guid);
OS						GuidObj (const API_Guid& guid);		// {"guid": "..."}

// --- Geometry ----------------------------------------------------------------

API_Coord				GetCoord (const OS& os, const char* key);
std::optional<API_Coord> OptCoord (const OS& os, const char* key);
API_Coord				CoordFrom (const OS& point);
API_Coord3D				GetCoord3D (const OS& os, const char* key);
API_Coord3D				Coord3DFrom (const OS& point);
OS						CoordObj (const API_Coord& c);
OS						CoordObj (double x, double y);
OS						Coord3DObj (const API_Coord3D& c);
OS						Coord3DObj (double x, double y, double z);
OS						BoxObj (const API_Box& box);
OS						Box3DObj (const API_Box3D& box);

double					RadToDeg (double rad);
double					DegToRad (double deg);

// --- Writers -----------------------------------------------------------------

// Adds "angle" in degrees for a radian value.
void					AddAngle (OS& os, const char* key, double radians);

} // namespace cc
