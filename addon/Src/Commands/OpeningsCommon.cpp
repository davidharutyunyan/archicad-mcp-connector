// *****************************************************************************
// OpeningsCommon — helpers shared by the openings family (see Openings.hpp).
// *****************************************************************************

#include "Commands/Openings.hpp"
#include "Core/Elements.hpp"
#include "Core/LibParts.hpp"

#include <cmath>
#include <cstdio>

namespace cc {
namespace openings {

namespace {

constexpr double kEps = 1e-7;

double Dist (const API_Coord& a, const API_Coord& b)
{
	return std::hypot (b.x - a.x, b.y - a.y);
}


// NOTE: GS::UniString::Printf is static (s.Printf (...) discards its result) -> snprintf.
GS::UniString Num (double v)
{
	char buf[64];
	std::snprintf (buf, sizeof (buf), "%.3f", v);
	return ToUni (buf);
}


// Counter-clockwise angle in [0, 2*pi) from vector a to vector b.
double CcwAngle (const API_Coord& a, const API_Coord& b)
{
	double ang = std::atan2 (a.x * b.y - a.y * b.x, a.x * b.x + a.y * b.y);
	if (ang < 0.0)
		ang += 2.0 * kPi;
	return ang;
}


API_Coord Sub (const API_Coord& a, const API_Coord& b)
{
	API_Coord c;
	c.x = a.x - b.x;
	c.y = a.y - b.y;
	return c;
}


API_Coord Rotate (const API_Coord& v, double angle)
{
	API_Coord r;
	r.x = v.x * std::cos (angle) - v.y * std::sin (angle);
	r.y = v.x * std::sin (angle) + v.y * std::cos (angle);
	return r;
}


[[noreturn]] void FailOutside (const WallGeometry& wall, double distance, const GS::UniString& what)
{
	Fail (what + " lies " + Num (distance) + " m along the wall reference line, but the wall is only " +
		  Num (wall.length) + " m long (valid range 0.." + Num (wall.length) + ", measured from the wall's begin point). "
		  "Check the wall with get_element_details (begin/end/length).", APIERR_BADPARS);
}

} // namespace

// --- Host walls ------------------------------------------------------------------

WallGeometry LoadHostWall (const API_Guid& guid, const char* elementNoun)
{
	API_Elem_Head head = GetHeader (guid);
	if (head.type.typeID != API_WallID) {
		Fail ("Element " + GuidStr (guid) + " is a " + ElemTypeName (head.type) + ", but a " + GS::UniString (elementNoun) +
			  " can only be placed into a Wall. Pass the GUID of a wall (curtain walls use their own panels).", APIERR_BADELEMENTTYPE);
	}
	API_Element element = GetElement (guid);
	const API_WallType& w = element.wall;
	if (w.type == APIWtyp_Poly) {
		Fail ("Wall " + GuidStr (guid) + " is a polygonal wall; placing a " + GS::UniString (elementNoun) +
			  " into a polygonal wall is not supported by this connector. Use a straight, curved or trapezoid wall "
			  "(or place it by hand in Archicad).", APIERR_NOTSUPPORTED);
	}

	WallGeometry g;
	g.guid = guid;
	g.floorInd = element.header.floorInd;
	g.begC = w.begC;
	g.endC = w.endC;
	g.arcAngle = w.angle;
	g.chord = Dist (w.begC, w.endC);
	g.curved = std::fabs (w.angle) > 1e-6;
	g.thickness = w.thickness;
	g.height = w.height;
	g.bottomOffset = w.bottomOffset;
	g.flipped = w.flipped;
	// offsetFromOutside = distance of the outside face from the reference line; the body extends
	// `thickness` from the outside face towards the inside.
	g.bodyCenterOffset = w.thickness / 2.0 - w.offsetFromOutside;
	g.length = g.chord;
	if (g.curved && g.chord > kEps) {
		const double radius = g.chord / (2.0 * std::sin (std::fabs (w.angle) / 2.0));
		g.length = radius * std::fabs (w.angle);
	}
	if (g.length < kEps)
		Fail ("Wall " + GuidStr (guid) + " has zero length.", APIERR_BADPARS);
	return g;
}


void CheckDistanceOnWall (const WallGeometry& wall, double distance, const char* what)
{
	if (distance < -1e-6 || distance > wall.length + 1e-6)
		FailOutside (wall, distance, ToUni (what));
}


WallProjection ProjectOntoWall (const WallGeometry& wall, const API_Coord& p, const char* fieldName)
{
	WallProjection result;
	const GS::UniString what = "The projection of '" + ToUni (fieldName) + "' (" + Num (p.x) + ", " + Num (p.y) + ")";

	if (!wall.curved) {
		const API_Coord d = Sub (wall.endC, wall.begC);
		const API_Coord v = Sub (p, wall.begC);
		const double t = (v.x * d.x + v.y * d.y) / wall.chord;
		if (t < -1e-6 || t > wall.length + 1e-6)
			FailOutside (wall, t, what);
		result.distance = std::max (0.0, std::min (wall.length, t));
		result.point.x = wall.begC.x + d.x / wall.chord * result.distance;
		result.point.y = wall.begC.y + d.y / wall.chord * result.distance;
		result.tangentAngle = std::atan2 (d.y, d.x);
		result.offset = (d.x * v.y - d.y * v.x) / wall.chord;
		return result;
	}

	// Curved wall: the arc through begC and endC with central angle |arcAngle|. The centre lies on
	// one of two sides of the chord; pick the one whose circle the point is closer to, so the result
	// does not depend on Archicad's sign convention for the wall angle.
	const double alpha = std::fabs (wall.arcAngle);
	const double radius = wall.chord / (2.0 * std::sin (alpha / 2.0));
	const double h = radius * std::cos (alpha / 2.0);			// signed distance chord midpoint -> centre
	API_Coord mid;
	mid.x = (wall.begC.x + wall.endC.x) / 2.0;
	mid.y = (wall.begC.y + wall.endC.y) / 2.0;
	API_Coord n;													// left normal of begC -> endC
	n.x = -(wall.endC.y - wall.begC.y) / wall.chord;
	n.y = (wall.endC.x - wall.begC.x) / wall.chord;

	API_Coord centers[2];
	centers[0].x = mid.x + n.x * h;	centers[0].y = mid.y + n.y * h;
	centers[1].x = mid.x - n.x * h;	centers[1].y = mid.y - n.y * h;

	int best = 0;
	if (std::fabs (Dist (p, centers[1]) - radius) < std::fabs (Dist (p, centers[0]) - radius))
		best = 1;
	const API_Coord c = centers[best];
	const API_Coord cb = Sub (wall.begC, c);
	const API_Coord ce = Sub (wall.endC, c);
	const API_Coord cp = Sub (p, c);
	if (std::hypot (cp.x, cp.y) < kEps)
		Fail (what + " is the centre of the curved wall; give a point near the wall.", APIERR_BADPARS);

	// Direction of travel from begC to endC along the arc (CCW if the CCW sweep equals alpha).
	const double sweepCcw = CcwAngle (cb, ce);
	bool ccw = std::fabs (sweepCcw - alpha) <= std::fabs ((2.0 * kPi - sweepCcw) - alpha);
	if (std::fabs (alpha - kPi) < 1e-6)								// half circle: take the half containing p
		ccw = CcwAngle (cb, cp) <= kPi;

	double phi = ccw ? CcwAngle (cb, cp) : CcwAngle (cp, cb);		// angle travelled from begC to p
	if (phi > alpha + 1e-9) {
		// outside the arc: report whichever end it is closer to
		const double beyondEnd = phi - alpha;
		const double beforeBegin = 2.0 * kPi - phi;
		FailOutside (wall, beyondEnd < beforeBegin ? radius * phi : -radius * beforeBegin, what);
	}
	result.distance = radius * phi;
	const API_Coord radial = Rotate (cb, ccw ? phi : -phi);
	result.point.x = c.x + radial.x;
	result.point.y = c.y + radial.y;
	// tangent = radial rotated by +-90 degrees in the travel direction
	result.tangentAngle = std::atan2 (radial.y, radial.x) + (ccw ? kPi / 2.0 : -kPi / 2.0);
	result.offset = (Dist (p, c) - radius) * (ccw ? -1.0 : 1.0);
	return result;
}


bool PointOnWall (const WallGeometry& wall, double distance, API_Coord& point, double& tangentAngle)
{
	if (wall.chord < kEps)
		return false;
	if (!wall.curved) {
		const API_Coord d = Sub (wall.endC, wall.begC);
		point.x = wall.begC.x + d.x / wall.chord * distance;
		point.y = wall.begC.y + d.y / wall.chord * distance;
		tangentAngle = std::atan2 (d.y, d.x);
		return true;
	}
	const double alpha = std::fabs (wall.arcAngle);
	const double sign = wall.arcAngle >= 0.0 ? 1.0 : -1.0;		// +1: centre on the left, counter-clockwise travel
	const double radius = wall.chord / (2.0 * std::sin (alpha / 2.0));
	const double h = radius * std::cos (alpha / 2.0);
	API_Coord n;													// left normal of begC -> endC
	n.x = -(wall.endC.y - wall.begC.y) / wall.chord;
	n.y = (wall.endC.x - wall.begC.x) / wall.chord;
	API_Coord c;
	c.x = (wall.begC.x + wall.endC.x) / 2.0 + n.x * h * sign;
	c.y = (wall.begC.y + wall.endC.y) / 2.0 + n.y * h * sign;
	const API_Coord radial = Rotate (Sub (wall.begC, c), sign * distance / radius);
	point.x = c.x + radial.x;
	point.y = c.y + radial.y;
	tangentAngle = std::atan2 (radial.y, radial.x) + sign * kPi / 2.0;
	return true;
}


bool WallBodyNormal (const WallGeometry& wall, double tangentAngle, API_Coord& normal)
{
	if (std::fabs (wall.bodyCenterOffset) < 1e-6)
		return false;
	double side = wall.flipped ? -1.0 : 1.0;						// +1 = right of the travel direction
	if (wall.bodyCenterOffset < 0.0)
		side = -side;
	normal.x = std::sin (tangentAngle) * side;						// right normal = (sin t, -cos t)
	normal.y = -std::cos (tangentAngle) * side;
	return true;
}

// --- Current database --------------------------------------------------------------

FloorPlanDatabaseScope::FloorPlanDatabaseScope ()
{
	BNZeroMemory (&previous, sizeof (previous));
	if (ACAPI_Database (APIDb_GetCurrentDatabaseID, &previous) != NoError)
		return;
	if (previous.typeID == APIWind_FloorPlanID)
		return;
	API_DatabaseInfo plan;
	BNZeroMemory (&plan, sizeof (plan));
	plan.typeID = APIWind_FloorPlanID;
	switched = ACAPI_Database (APIDb_ChangeCurrentDatabaseID, &plan) == NoError;
}


FloorPlanDatabaseScope::~FloorPlanDatabaseScope ()
{
	if (switched)
		ACAPI_Database (APIDb_ChangeCurrentDatabaseID, &previous);
}

// --- Library parts / GDL parameters -------------------------------------------------

namespace {

// The toolbox tool a library part belongs to (API_ZombieElemID when unknown).
API_ElemTypeID CreatorTool (const API_LibPart& lp)
{
	API_LibPart tmp = lp;
	tmp.location = nullptr;
	API_ToolBoxItem tb;
	BNZeroMemory (&tb, sizeof (tb));
	const GSErrCode err = ACAPI_Goodies (APIAny_GetLibPartToolVariationID, &tmp, &tb);
	if (tmp.location != nullptr) {
		delete tmp.location;
		tmp.location = nullptr;
	}
	return err == NoError ? tb.type.typeID : API_ZombieElemID;
}


const char* SearchHint (API_ElemTypeID elemType)
{
	switch (elemType) {
		case API_WindowID:		return "search_library_parts with query 'окно' (window)";
		case API_DoorID:		return "search_library_parts with query 'дверь' (door)";
		case API_SkylightID:	return "search_library_parts with query 'мансардное окно' / 'зенитный фонарь' (skylight)";
		default:				return "search_library_parts";
	}
}

} // namespace


API_LibPart FindOpeningLibPart (const OS& spec, const char* key, API_ElemTypeID elemType)
{
	const GS::UniString noun = ElemTypeName (elemType);
	API_LibPart lp;
	try {
		lp = FindLibPart (spec, key);
	} catch (const Error& e) {
		throw Error (e.message + " Find " + noun + " parts with " + ToUni (SearchHint (elemType)) +
					 "; the tool's default part is used when 'libraryPart' is omitted.", e.code);
	}
	if (!lp.isPlaceable || lp.isTemplate) {
		Fail ("Library part '" + GS::UniString (lp.docu_UName) + "' is not placeable (it is a macro or a subtype template). Choose a placeable " +
			  noun + " part with " + ToUni (SearchHint (elemType)) + ".", APIERR_BADPARS);
	}
	const API_ElemTypeID tool = CreatorTool (lp);
	if (tool != API_ZombieElemID && tool != elemType) {
		Fail ("Library part '" + GS::UniString (lp.docu_UName) + "' belongs to the " + ElemTypeName (tool) + " tool, but a " + noun +
			  " needs a " + noun + " library part. Find one with " + ToUni (SearchHint (elemType)) + ".", APIERR_NOTSUBTYPEOF);
	}
	return lp;
}


OS LibPartBrief (Int32 libInd)
{
	OS out;
	out.Add ("index", (Int32) libInd);
	API_LibPart lp;
	BNZeroMemory (&lp, sizeof (lp));
	lp.index = libInd;
	const GSErrCode err = ACAPI_LibPart_Get (&lp);
	if (lp.location != nullptr) {
		delete lp.location;
		lp.location = nullptr;
	}
	if (err != NoError) {
		out.Add ("missing", true);
		return out;
	}
	out.Add ("name", GS::UniString (lp.docu_UName));
	out.Add ("guid", GS::UniString (lp.ownUnID));
	out.Add ("fileName", GS::UniString (lp.file_UName));
	if (lp.missingDef)
		out.Add ("missing", true);
	return out;
}


namespace {

Int32 ParamCount (API_AddParType** params)
{
	if (params == nullptr || *params == nullptr)
		return 0;
	return (Int32) (BMhGetSize (reinterpret_cast<GSHandle> (params)) / sizeof (API_AddParType));
}

} // namespace


namespace {

void AddParamValue (OS& out, const GS::String& key, const API_AddParType& p)
{
	switch (p.typeID) {
		case APIParT_CString:	out.Add (key, GS::UniString (p.value.uStr)); break;
		case APIParT_Angle:		out.Add (key, RadToDeg (p.value.real)); break;
		case APIParT_Boolean:
		case APIParT_LightSw:	out.Add (key, p.value.real != 0.0); break;
		case APIParT_Integer:
		case APIParT_LineTyp:
		case APIParT_Mater:
		case APIParT_FillPat:
		case APIParT_PenCol:
		case APIParT_BuildingMaterial:
		case APIParT_Profile:	out.Add (key, (Int32) std::lround (p.value.real)); break;
		default:				out.Add (key, p.value.real); break;
	}
}


bool IsSizeParam (const GS::String& name)
{
	return name == "A" || name == "B" || name == "ZZYZX";
}


bool IsSummaryCandidate (const API_AddParType& p)
{
	if (p.typeID == APIParT_Separator || p.typeID == APIParT_Title || p.typeID == APIParT_Dictionary)
		return false;
	if (p.typeMod == API_ParArray || (p.flags & (API_ParFlg_Hidden | API_ParFlg_Disabled)) != 0)
		return false;
	const GS::String name (p.name);
	return !name.IsEmpty () && !name.BeginsWith ("ac_");
}

} // namespace


OS GdlParamsSummary (API_AddParType** params, UInt32 maxCount, UInt32* total)
{
	OS out;
	UInt32 count = 0;
	UInt32 listed = 0;
	const Int32 n = ParamCount (params);
	for (Int32 i = 0; i < n; ++i) {								// size parameters first
		const API_AddParType& p = (*params)[i];
		const GS::String key (p.name);
		if (p.typeMod != API_ParArray && IsSizeParam (key) && !out.Contains (key)) {
			AddParamValue (out, key, p);
			++listed;
		}
	}
	for (Int32 i = 0; i < n; ++i) {
		const API_AddParType& p = (*params)[i];
		const GS::String key (p.name);
		if (IsSizeParam (key) || !IsSummaryCandidate (p))
			continue;
		++count;
		if (listed >= maxCount || out.Contains (key))
			continue;
		AddParamValue (out, key, p);
		++listed;
	}
	if (total != nullptr)
		*total = count;
	return out;
}


std::optional<double> ParamReal (API_AddParType** params, const char* name)
{
	const Int32 n = ParamCount (params);
	const GS::UniString wanted (name);
	for (Int32 i = 0; i < n; ++i) {
		const API_AddParType& p = (*params)[i];
		if (p.typeMod == API_ParArray || p.typeID == APIParT_CString)
			continue;
		if (EqualsIgnoreCase (GS::UniString (p.name, CC_UTF8), wanted))
			return p.value.real;
	}
	return std::nullopt;
}


OS WithSizeParams (const OS& values, std::optional<double> a, std::optional<double> b)
{
	OS copy = values;
	bool hasA = false, hasB = false;
	for (const GS::String& field : values.GetFieldNames ()) {
		const GS::UniString u (field.ToCStr (), CC_UTF8);
		if (EqualsIgnoreCase (u, "A")) hasA = true;
		if (EqualsIgnoreCase (u, "B")) hasB = true;
	}
	if (a.has_value () && !hasA)
		copy.Add ("A", *a);
	if (b.has_value () && !hasB)
		copy.Add ("B", *b);
	return copy;
}

// --- Misc readers -------------------------------------------------------------------

std::optional<short> OptPen (const OS& os, const char* key, short minPen)
{
	auto v = OptInt (os, key);
	if (!v.has_value ())
		return std::nullopt;
	if (*v < minPen || *v > 255) {
		char buf[160];
		std::snprintf (buf, sizeof (buf), "Field '%s' must be a pen index %d..255 (got %d).", key, (int) minPen, (int) *v);
		Fail (ToUni (buf));
	}
	return (short) *v;
}


std::optional<API_Guid> OptGuidAny (const OS& os, std::initializer_list<const char*> keys)
{
	for (const char* key : keys) {
		if (os.Contains (key))
			return OptGuid (os, key);
	}
	return std::nullopt;
}

} // namespace openings
} // namespace cc
