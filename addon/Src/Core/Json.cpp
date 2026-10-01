#include "Core/Json.hpp"

#include <cmath>

namespace cc {

// --- Errors ------------------------------------------------------------------

void Fail (const GS::UniString& message, GSErrCode code)
{
	throw Error (message, code);
}


GS::UniString ErrorName (GSErrCode err)
{
	switch (err) {
		case NoError:							return "NoError";
		case APIERR_GENERAL:		return "APIERR_GENERAL (general error)";
		case APIERR_MEMFULL:		return "APIERR_MEMFULL (out of memory)";
		case APIERR_CANCEL:		return "APIERR_CANCEL (operation cancelled)";
		case APIERR_BADID:		return "APIERR_BADID (invalid identifier / element not found)";
		case APIERR_BADINDEX:		return "APIERR_BADINDEX (invalid index)";
		case APIERR_BADNAME:		return "APIERR_BADNAME (invalid name)";
		case APIERR_BADPARS:		return "APIERR_BADPARS (invalid parameters)";
		case APIERR_BADPOLY:		return "APIERR_BADPOLY (invalid polygon)";
		case APIERR_BADDATABASE:		return "APIERR_BADDATABASE (command not applicable in the current database/window)";
		case APIERR_BADWINDOW:		return "APIERR_BADWINDOW (command not applicable in the current window)";
		case APIERR_BADKEYCODE:		return "APIERR_BADKEYCODE";
		case APIERR_BADPLATFORMSIGN:		return "APIERR_BADPLATFORMSIGN";
		case APIERR_BADPLANE:		return "APIERR_BADPLANE (invalid plane)";
		case APIERR_BADUSERID:		return "APIERR_BADUSERID";
		case APIERR_BADVALUE:		return "APIERR_BADVALUE (invalid value)";
		case APIERR_BADELEMENTTYPE:		return "APIERR_BADELEMENTTYPE (invalid element type)";
		case APIERR_IRREGULARPOLY:		return "APIERR_IRREGULARPOLY (irregular polygon (self-intersecting or bad orientation))";
		case APIERR_BADEXPRESSION:		return "APIERR_BADEXPRESSION (invalid expression)";
		case APIERR_BADGUID:		return "APIERR_BADGUID (invalid GUID)";
		case APIERR_BADTOKEN:		return "APIERR_BADTOKEN";
		case APIERR_NO3D:		return "APIERR_NO3D (no 3D representation)";
		case APIERR_NOMORE:		return "APIERR_NOMORE (no more items)";
		case APIERR_NOPLAN:		return "APIERR_NOPLAN (no project open)";
		case APIERR_NOLIB:		return "APIERR_NOLIB (library not loaded)";
		case APIERR_NOLIBSECT:		return "APIERR_NOLIBSECT (library part section missing)";
		case APIERR_NOSEL:		return "APIERR_NOSEL (nothing selected)";
		case APIERR_NOTEDITABLE:		return "APIERR_NOTEDITABLE (element not editable (locked, on a locked/hidden layer, or not reserved))";
		case APIERR_NOTSUBTYPEOF:		return "APIERR_NOTSUBTYPEOF (library part is not a subtype of the required type)";
		case APIERR_NOTEQUALMAIN:		return "APIERR_NOTEQUALMAIN";
		case APIERR_NOTEQUALREVISION:		return "APIERR_NOTEQUALREVISION";
		case APIERR_NOTEAMWORKPROJECT:		return "APIERR_NOTEAMWORKPROJECT (not a Teamwork project)";
		case APIERR_NOUSERDATA:		return "APIERR_NOUSERDATA (no user data)";
		case APIERR_MOREUSER:		return "APIERR_MOREUSER";
		case APIERR_LINKEXIST:		return "APIERR_LINKEXIST (link already exists)";
		case APIERR_LINKNOTEXIST:		return "APIERR_LINKNOTEXIST (link does not exist)";
		case APIERR_WINDEXIST:		return "APIERR_WINDEXIST (window already open)";
		case APIERR_WINDNOTEXIST:		return "APIERR_WINDNOTEXIST (window does not exist)";
		case APIERR_UNDOEMPTY:		return "APIERR_UNDOEMPTY (nothing to undo)";
		case APIERR_REFERENCEEXIST:		return "APIERR_REFERENCEEXIST (reference already exists)";
		case APIERR_NAMEALREADYUSED:		return "APIERR_NAMEALREADYUSED (name already used)";
		case APIERR_ATTREXIST:		return "APIERR_ATTREXIST (attribute with this name already exists)";
		case APIERR_DELETED:		return "APIERR_DELETED (element was deleted)";
		case APIERR_LOCKEDLAY:		return "APIERR_LOCKEDLAY (layer is locked)";
		case APIERR_HIDDENLAY:		return "APIERR_HIDDENLAY (layer is hidden)";
		case APIERR_INVALFLOOR:		return "APIERR_INVALFLOOR (invalid story index)";
		case APIERR_NOTMINE:		return "APIERR_NOTMINE (Teamwork: not reserved by you)";
		case APIERR_NOACCESSRIGHT:		return "APIERR_NOACCESSRIGHT (no access right)";
		case APIERR_BADPROPERTY:		return "APIERR_BADPROPERTY (invalid property)";
		case APIERR_BADCLASSIFICATION:		return "APIERR_BADCLASSIFICATION (invalid classification)";
		case APIERR_NOTEXISTINGLAYER:		return "APIERR_NOTEXISTINGLAYER (layer does not exist)";
		case APIERR_MODULNOTINSTALLED:		return "APIERR_MODULNOTINSTALLED (module not installed)";
		case APIERR_MODULCMDMINE:		return "APIERR_MODULCMDMINE";
		case APIERR_MODULCMDNOTSUPPORTED:		return "APIERR_MODULCMDNOTSUPPORTED (module command not supported)";
		case APIERR_MODULCMDVERSNOTSUPPORTED:		return "APIERR_MODULCMDVERSNOTSUPPORTED";
		case APIERR_NOMODULEDATA:		return "APIERR_NOMODULEDATA";
		case APIERR_PAROVERLAP:		return "APIERR_PAROVERLAP";
		case APIERR_PARMISSING:		return "APIERR_PARMISSING";
		case APIERR_PAROVERFLOW:		return "APIERR_PAROVERFLOW";
		case APIERR_PARIMPLICIT:		return "APIERR_PARIMPLICIT";
		case APIERR_RUNOVERLAP:		return "APIERR_RUNOVERLAP";
		case APIERR_RUNMISSING:		return "APIERR_RUNMISSING";
		case APIERR_RUNOVERFLOW:		return "APIERR_RUNOVERFLOW";
		case APIERR_RUNIMPLICIT:		return "APIERR_RUNIMPLICIT";
		case APIERR_RUNPROTECTED:		return "APIERR_RUNPROTECTED";
		case APIERR_EOLOVERLAP:		return "APIERR_EOLOVERLAP";
		case APIERR_TABOVERLAP:		return "APIERR_TABOVERLAP";
		case APIERR_NOTINIT:		return "APIERR_NOTINIT (not initialized)";
		case APIERR_NESTING:		return "APIERR_NESTING (nesting error)";
		case APIERR_NOTSUPPORTED:		return "APIERR_NOTSUPPORTED (not supported)";
		case APIERR_REFUSEDCMD:		return "APIERR_REFUSEDCMD (command refused in the current context)";
		case APIERR_REFUSEDPAR:		return "APIERR_REFUSEDPAR (parameter refused)";
		case APIERR_READONLY:		return "APIERR_READONLY (read-only)";
		case APIERR_SERVICEFAILED:		return "APIERR_SERVICEFAILED (service failed)";
		case APIERR_COMMANDFAILED:		return "APIERR_COMMANDFAILED (command failed)";
		case APIERR_NEEDSUNDOSCOPE:		return "APIERR_NEEDSUNDOSCOPE (must run inside an undoable command)";
		case APIERR_MISSINGCODE:		return "APIERR_MISSINGCODE (function not implemented in this Archicad)";
		case APIERR_MISSINGDEF:		return "APIERR_MISSINGDEF (missing definition)";
		default: {
			GS::UniString s;
			s.Printf ("error code %d", (int) err);
			return s;
		}
	}
}


void Check (GSErrCode err, const GS::UniString& what)
{
	if (err != NoError) {
		throw Error (what + ": " + ErrorName (err), err);
	}
}


OS ErrorObject (GSErrCode code, const GS::UniString& message)
{
	return OS ("error", OS ("code", (Int32) code, "message", message));
}

// --- Strings -----------------------------------------------------------------

GS::UniString ToUni (const char* s)
{
	return GS::UniString (s, CC_UTF8);
}


GS::String ToStr (const GS::UniString& s)
{
	return GS::String (s.ToCStr (0, MaxUSize, CC_UTF8).Get ());
}


bool EqualsIgnoreCase (const GS::UniString& a, const GS::UniString& b)
{
	return a.IsEqual (b, GS::UniString::CaseInsensitive);
}

// --- Readers -----------------------------------------------------------------

static GS::UniString KeyStr (const char* key)
{
	return GS::UniString (key);
}


bool Has (const OS& os, const char* key)
{
	return os.Contains (key);
}


bool IsNumber (const OS& os, const char* key)
{
	return os.Contains (key) && (os.IsReal (key) || os.IsInt (key) || os.IsUInt (key));
}


std::optional<double> OptDouble (const OS& os, const char* key)
{
	if (!os.Contains (key))
		return std::nullopt;
	if (os.IsReal (key)) {
		double v = 0; os.Get (key, v); return v;
	}
	if (os.IsInt (key)) {
		Int64 v = 0; os.Get (key, v); return (double) v;
	}
	if (os.IsUInt (key)) {
		UInt64 v = 0; os.Get (key, v); return (double) v;
	}
	Fail ("Field '" + KeyStr (key) + "' must be a number.");
}


double GetDouble (const OS& os, const char* key)
{
	auto v = OptDouble (os, key);
	if (!v.has_value ())
		Fail ("Missing required number field '" + KeyStr (key) + "'.");
	return *v;
}


double GetDouble (const OS& os, const char* key, double def)
{
	return OptDouble (os, key).value_or (def);
}


std::optional<Int32> OptInt (const OS& os, const char* key)
{
	auto v = OptDouble (os, key);
	if (!v.has_value ())
		return std::nullopt;
	return (Int32) std::llround (*v);
}


Int32 GetInt (const OS& os, const char* key)
{
	auto v = OptInt (os, key);
	if (!v.has_value ())
		Fail ("Missing required integer field '" + KeyStr (key) + "'.");
	return *v;
}


Int32 GetInt (const OS& os, const char* key, Int32 def)
{
	return OptInt (os, key).value_or (def);
}


std::optional<bool> OptBool (const OS& os, const char* key)
{
	if (!os.Contains (key))
		return std::nullopt;
	if (!os.IsBool (key))
		Fail ("Field '" + KeyStr (key) + "' must be a boolean.");
	bool v = false;
	os.Get (key, v);
	return v;
}


bool GetBool (const OS& os, const char* key)
{
	auto v = OptBool (os, key);
	if (!v.has_value ())
		Fail ("Missing required boolean field '" + KeyStr (key) + "'.");
	return *v;
}


bool GetBool (const OS& os, const char* key, bool def)
{
	return OptBool (os, key).value_or (def);
}


std::optional<GS::UniString> OptString (const OS& os, const char* key)
{
	if (!os.Contains (key))
		return std::nullopt;
	if (!os.IsString (key))
		Fail ("Field '" + KeyStr (key) + "' must be a string.");
	GS::UniString v;
	os.Get (key, v);
	return v;
}


GS::UniString GetString (const OS& os, const char* key)
{
	auto v = OptString (os, key);
	if (!v.has_value ())
		Fail ("Missing required string field '" + KeyStr (key) + "'.");
	return *v;
}


GS::UniString GetString (const OS& os, const char* key, const GS::UniString& def)
{
	return OptString (os, key).value_or (def);
}


bool TryGetObject (const OS& os, const char* key, OS& out)
{
	if (!os.Contains (key))
		return false;
	if (!os.IsObject (key))
		Fail ("Field '" + KeyStr (key) + "' must be an object.");
	return os.Get (key, out);
}


OS GetObject (const OS& os, const char* key)
{
	OS out;
	if (!TryGetObject (os, key, out))
		Fail ("Missing required object field '" + KeyStr (key) + "'.");
	return out;
}


GS::Array<OS> GetObjectArray (const OS& os, const char* key, bool required)
{
	GS::Array<OS> out;
	if (!os.Contains (key)) {
		if (required)
			Fail ("Missing required array field '" + KeyStr (key) + "'.");
		return out;
	}
	if (!os.IsList (key))
		Fail ("Field '" + KeyStr (key) + "' must be an array.");
	os.Get (key, out);
	return out;
}


GS::Array<OS> GetObjectArray (const OS& os, const char* key)
{
	return GetObjectArray (os, key, true);
}


GS::Array<GS::UniString> GetStringArray (const OS& os, const char* key, bool required)
{
	GS::Array<GS::UniString> out;
	if (!os.Contains (key)) {
		if (required)
			Fail ("Missing required array field '" + KeyStr (key) + "'.");
		return out;
	}
	if (!os.IsList (key))
		Fail ("Field '" + KeyStr (key) + "' must be an array of strings.");
	os.Get (key, out);
	return out;
}


namespace {
	class NumberListCollector : public OS::Processor {
	public:
		GS::Array<double> values;
		bool inList = false;
		void IntFound (const GS::String&, Int64 value) override { values.Push ((double) value); }
		void UIntFound (const GS::String&, UInt64 value) override { values.Push ((double) value); }
		void RealFound (const GS::String&, double value) override { values.Push (value); }
	};
}


GS::Array<double> GetNumberArray (const OS& os, const char* key, bool required)
{
	if (!os.Contains (key)) {
		if (required)
			Fail ("Missing required array field '" + KeyStr (key) + "'.");
		return {};
	}
	if (!os.IsList (key))
		Fail ("Field '" + KeyStr (key) + "' must be an array of numbers.");
	NumberListCollector collector;
	os.Enumerate (key, collector);
	return collector.values;
}


double RadToDeg (double rad) { return rad * 180.0 / kPi; }
double DegToRad (double deg) { return deg * kPi / 180.0; }


std::optional<double> OptAngle (const OS& os, const char* key)
{
	auto v = OptDouble (os, key);
	if (!v.has_value ())
		return std::nullopt;
	return DegToRad (*v);
}


double GetAngle (const OS& os, const char* key)
{
	return DegToRad (GetDouble (os, key));
}


double GetAngle (const OS& os, const char* key, double defRadians)
{
	auto v = OptAngle (os, key);
	return v.has_value () ? *v : defRadians;
}

// --- GUIDs -------------------------------------------------------------------

API_Guid ParseGuid (const GS::UniString& str)
{
	GS::Guid guid;
	if (guid.ConvertFromString (str.ToCStr ().Get ()) != NoError || guid.IsNull ()) {
		Fail ("Invalid GUID string '" + str + "'.");
	}
	return GSGuid2APIGuid (guid);
}


API_Guid GuidFromItem (const OS& item)
{
	if (item.Contains ("elementId")) {
		OS inner;
		item.Get ("elementId", inner);
		return GuidFromItem (inner);
	}
	if (item.Contains ("guid") && item.IsString ("guid")) {
		GS::UniString s;
		item.Get ("guid", s);
		return ParseGuid (s);
	}
	Fail ("Expected an element reference like {\"guid\": \"...\"}.");
}


std::optional<API_Guid> OptGuid (const OS& os, const char* key)
{
	if (!os.Contains (key))
		return std::nullopt;
	if (os.IsString (key)) {
		GS::UniString s;
		os.Get (key, s);
		return ParseGuid (s);
	}
	if (os.IsObject (key)) {
		OS inner;
		os.Get (key, inner);
		return GuidFromItem (inner);
	}
	Fail ("Field '" + KeyStr (key) + "' must be a GUID string or {\"guid\": \"...\"}.");
}


API_Guid GetGuid (const OS& os, const char* key)
{
	auto g = OptGuid (os, key);
	if (!g.has_value ())
		Fail ("Missing required GUID field '" + KeyStr (key) + "'.");
	return *g;
}


namespace {
	class GuidListCollector : public OS::Processor {
	public:
		GS::Array<API_Guid> values;
		GS::Array<GS::UniString> errors;
		void StringFound (const GS::String&, const GS::UniString& value) override
		{
			try { values.Push (ParseGuid (value)); } catch (const Error& e) { errors.Push (e.message); }
		}
		bool ObjectFound (const GS::String&, const OS& value) override
		{
			try { values.Push (GuidFromItem (value)); } catch (const Error& e) { errors.Push (e.message); }
			return false;	// do not descend
		}
	};
}


GS::Array<API_Guid> GetGuidArray (const OS& os, const char* key, bool required)
{
	if (!os.Contains (key)) {
		if (required)
			Fail ("Missing required array field '" + KeyStr (key) + "'.");
		return {};
	}
	if (!os.IsList (key))
		Fail ("Field '" + KeyStr (key) + "' must be an array of GUIDs.");
	GuidListCollector collector;
	os.Enumerate (key, collector);
	if (!collector.errors.IsEmpty ())
		Fail (collector.errors[0]);
	return collector.values;
}


GS::UniString GuidStr (const API_Guid& guid)
{
	return APIGuidToString (guid);
}


OS GuidObj (const API_Guid& guid)
{
	return OS ("guid", APIGuidToString (guid));
}

// --- Geometry ----------------------------------------------------------------

API_Coord CoordFrom (const OS& point)
{
	API_Coord c;
	c.x = GetDouble (point, "x");
	c.y = GetDouble (point, "y");
	return c;
}


API_Coord GetCoord (const OS& os, const char* key)
{
	return CoordFrom (GetObject (os, key));
}


std::optional<API_Coord> OptCoord (const OS& os, const char* key)
{
	OS p;
	if (!TryGetObject (os, key, p))
		return std::nullopt;
	return CoordFrom (p);
}


API_Coord3D Coord3DFrom (const OS& point)
{
	API_Coord3D c;
	c.x = GetDouble (point, "x");
	c.y = GetDouble (point, "y");
	c.z = GetDouble (point, "z", 0.0);
	return c;
}


API_Coord3D GetCoord3D (const OS& os, const char* key)
{
	return Coord3DFrom (GetObject (os, key));
}


OS CoordObj (double x, double y)
{
	return OS ("x", x, "y", y);
}


OS CoordObj (const API_Coord& c)
{
	return CoordObj (c.x, c.y);
}


OS Coord3DObj (double x, double y, double z)
{
	return OS ("x", x, "y", y, "z", z);
}


OS Coord3DObj (const API_Coord3D& c)
{
	return Coord3DObj (c.x, c.y, c.z);
}


OS BoxObj (const API_Box& box)
{
	return OS ("xMin", box.xMin, "yMin", box.yMin, "xMax", box.xMax, "yMax", box.yMax);
}


OS Box3DObj (const API_Box3D& box)
{
	return OS ("xMin", box.xMin, "yMin", box.yMin, "zMin", box.zMin, "xMax", box.xMax, "yMax", box.yMax, "zMax", box.zMax);
}


void AddAngle (OS& os, const char* key, double radians)
{
	os.Add (key, RadToDeg (radians));
}

} // namespace cc
