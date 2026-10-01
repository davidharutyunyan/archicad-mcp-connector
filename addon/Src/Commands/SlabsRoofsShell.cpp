// *****************************************************************************
// SlabsRoofsShell — Shell adapter (extruded, revolved and ruled shells).
//
// Coordinates follow the Archicad API (see Element_Test Do_Create*Shell):
//   * basePlane: the shell's local coordinate system (frame). Extruded shells default to the
//     identity (world axes at the home story), revolved shells default to a vertical frame at
//     axisOrigin rotated by profileRotation.
//   * Extruded: profile points {x, y} are in the profile plane (x = across, y = up), extruded from
//     'begin' (3D) along 'extrusion' (3D vector).
//   * Revolved: profile points {x = distance from the axis, y = height along the axis}; the profile is
//     revolved around the vertical axis through axisOrigin by revolutionAngle.
//   * Ruled: two profiles (profile, profile2) placed on plane1 / plane2 are connected by ruled surfaces.
// Fields (meters, degrees): shellClass, profile*, closedProfile, flipped, basePlane, defaultEdgeType,
//   extruded: begin, extrusion, profileDirection, slantAngle, profilePlaneTilt, beginPlaneTilt, endPlaneTilt
//   revolved: axisOrigin, profileRotation, axisBase, revolutionAngle, beginAngle, slantAngle,
//             distortionAngle, segmentedSurfaces, arcSegments | circleSegments
//   ruled:    profile2*, plane1*, plane2*, morphingRule
//   + shell base (level, thickness, structure, surfaces, edge trim, floor plan fields) + common fields
// *****************************************************************************

#include "Commands/Commands.hpp"
#include "Commands/SlabsRoofsCommon.hpp"
#include "Core/Command.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace cc {

namespace {

using namespace slabroof;

const NamedValue kShellClasses[] = {
	{ "Extruded",	API_ExtrudedShellID },
	{ "Revolved",	API_RevolvedShellID },
	{ "Ruled",		API_RuledShellID },
};

const NamedValue kMorphingRules[] = {
	{ "Paired",	APIMorphingRule_Paired },
	{ "Smooth",	APIMorphingRule_Smooth },
};


// --- Profiles (API_ShellShapeData) -------------------------------------------------------------

struct Profile {
	Contour	line;
	bool	closed = false;
};


Profile ProfileFrom (const OS& spec, const char* key, bool closed)
{
	Profile p;
	if (closed) {
		// Closed profile: parsed like a polygon outline so the closing edge may be an arc too.
		const PolygonData poly = GetPolygon (spec, key);
		if (!poly.holes.IsEmpty ())
			Fail ("Shell profile '" + GS::UniString (key) + "' cannot have holes.");
		p.line = poly.outline;
		p.line.z.Clear ();
		p.closed = true;
		return p;
	}
	p.line = GetPolyline (spec, key);
	p.closed = false;
	// Drop a repeated first point at the end (closed profiles are given without it).
	const Int32 n = (Int32) p.line.points.GetSize ();
	if (n >= 3) {
		const API_Coord& a = p.line.points[0];
		const API_Coord& b = p.line.points[n - 1];
		if (std::fabs (a.x - b.x) < 1e-9 && std::fabs (a.y - b.y) < 1e-9) {
			p.line.points.Pop ();
			p.closed = true;
		}
	}
	if (p.closed && p.line.points.GetSize () < 3)
		Fail ("A closed shell profile needs at least 3 points.");
	return p;
}


// Moves shape k's handles into a scratch memo and disposes them with Archicad's disposer.
void DisposeShape (API_ElementMemo& memo, Int32 k)
{
	API_ElementMemo scratch;
	BNZeroMemory (&scratch, sizeof (scratch));
	std::swap (scratch.shellShapes[0], memo.shellShapes[k]);
	ACAPI_DisposeElemMemoHdls (&scratch);
}


// Writes the profile as a closed memo polygon whose closing edge is flagged "not body" for open profiles.
void WriteShape (const Profile& profile, API_ElementMemo& memo, Int32 k, API_Polygon& poly)
{
	DisposeShape (memo, k);
	API_ShellShapeData& shape = memo.shellShapes[k];
	const Int32 n = (Int32) profile.line.points.GetSize ();
	const Int32 nCoords = n + 1;
	const Int32 nArcs = (Int32) profile.line.arcs.GetSize ();

	ResizeHandle (shape.coords, nCoords + 1);
	ResizeHandle (shape.pends, 2);
	if (nArcs > 0)
		ResizeHandle (shape.parcs, nArcs);
	ResizeHandle (shape.bodyFlags, nCoords + 1);

	for (Int32 i = 0; i < n; ++i)
		(*shape.coords)[i + 1] = profile.line.points[i];
	(*shape.coords)[nCoords] = profile.line.points[0];
	(*shape.pends)[0] = 0;
	(*shape.pends)[1] = nCoords;
	for (Int32 a = 0; a < nArcs; ++a) {
		(*shape.parcs)[a].begIndex = profile.line.arcs[a].first + 1;
		(*shape.parcs)[a].endIndex = profile.line.arcs[a].first + 2;
		(*shape.parcs)[a].arcAngle = profile.line.arcs[a].second;
	}
	for (Int32 i = 1; i < n; ++i)
		(*shape.bodyFlags)[i] = true;
	(*shape.bodyFlags)[n] = profile.closed;			// edge from the last point back to the first
	(*shape.bodyFlags)[nCoords] = (*shape.bodyFlags)[1];

	BNZeroMemory (&poly, sizeof (poly));
	poly.nCoords = nCoords;
	poly.nSubPolys = 1;
	poly.nArcs = nArcs;
}


Profile ShapeToProfile (const API_ShellShapeData& shape)
{
	Profile p;
	const API_Polygon counts = PolygonCounts (shape.coords, shape.pends, shape.parcs);
	if (counts.nSubPolys < 1 || counts.nCoords < 3)
		return p;
	const Int32 end = std::min ((*shape.pends)[1], counts.nCoords);		// closing vertex of the first contour
	for (Int32 i = 1; i < end; ++i)
		p.line.points.Push ((*shape.coords)[i]);
	const Int32 nFlags = HandleCount (reinterpret_cast<GSConstHandle> (shape.bodyFlags), sizeof (bool));
	p.closed = (end - 1) < nFlags ? (*shape.bodyFlags)[end - 1] : false;
	for (Int32 a = 0; a < counts.nArcs; ++a) {
		const API_PolyArc& arc = (*shape.parcs)[a];
		if (arc.begIndex >= 1 && arc.begIndex < end)
			p.line.arcs.Push ({ arc.begIndex - 1, arc.arcAngle });
	}
	return p;
}


OS ProfileJson (const Profile& p)
{
	GS::Array<OS> pts;
	for (const API_Coord& c : p.line.points)
		pts.Push (CoordObj (c));
	OS out ("points", pts);
	GS::Array<OS> arcs;
	for (const auto& a : p.line.arcs)
		arcs.Push (OS ("index", a.first, "angle", RadToDeg (a.second)));
	if (!arcs.IsEmpty ())
		out.Add ("arcs", arcs);
	return out;
}


API_Polygon& ShapePoly (API_ShellType& shell, Int32 k)
{
	switch (shell.shellClass) {
		case API_RevolvedShellID:	return shell.u.revolvedShell.shellShape;
		case API_RuledShellID:		return k == 0 ? shell.u.ruledShell.shellShape1 : shell.u.ruledShell.shellShape2;
		default:					return shell.u.extrudedShell.shellShape;
	}
}

// --- Struct fields -----------------------------------------------------------------------------------

API_Vector3D VectorFrom (const OS& spec, const char* key, const GS::UniString& what)
{
	const API_Coord3D v = Point3DFrom (spec, key, 0.0);
	if (std::sqrt (v.x * v.x + v.y * v.y + v.z * v.z) < 1e-9)
		Fail (what + " must not be a zero vector.");
	return v;
}


void ApplyShellFields (API_Element& element, API_Element* mask, const OS& spec)
{
	API_ShellType& shell = element.shell;
	const FieldMask m (element, mask);
	if (Has (spec, "profile") && !spec.IsList ("profile") && !spec.IsObject ("profile"))
		Fail ("Shell 'profile' must be a polyline (array of points or {points, arcs}), not an attribute; shells have no complex profiles.");
	ApplyShellBaseFields (shell.shellBase, m, spec);
	CheckCompositeUsage (spec, shell.shellBase.composite, APICWall_ForShell, "shells");

	if (ApplyFlag (spec, "flipped", shell.isFlipped))		m.Set (&shell.isFlipped);
	if (Has (spec, "basePlane")) {
		shell.basePlane = FrameFrom (spec, "basePlane");
		m.Set (&shell.basePlane);
	}
	if (Has (spec, "defaultEdgeType")) {
		shell.defEdgeType = (API_ShellBaseContourEdgeTypeID) ParseNamed (kEdgeTypes, spec, "defaultEdgeType");
		m.Set (&shell.defEdgeType);
	}

	auto angle = [&] (const char* key, double& field) {
		if (ApplyAngleDeg (spec, key, field))
			m.Set (&field);
	};

	switch (shell.shellClass) {
		case API_ExtrudedShellID: {
			API_ExtrudedShellData& d = shell.u.extrudedShell;
			if (auto b = OptPoint3D (spec, "begin", d.begC.z))	{ d.begC = *b; m.Set (&d.begC); }
			if (Has (spec, "extrusion"))	{ d.extrusionVector = VectorFrom (spec, "extrusion", "'extrusion'"); m.Set (&d.extrusionVector); }
			if (auto v = OptCoord (spec, "profileDirection")) {
				if (std::hypot (v->x, v->y) < 1e-9) Fail ("'profileDirection' must not be a zero vector.");
				d.shapeDirection = *v;
				m.Set (&d.shapeDirection);
			}
			angle ("slantAngle", d.slantAngle);
			angle ("profilePlaneTilt", d.shapePlaneTilt);
			angle ("beginPlaneTilt", d.begPlaneTilt);
			angle ("endPlaneTilt", d.endPlaneTilt);
			break;
		}
		case API_RevolvedShellID: {
			API_RevolvedShellData& d = shell.u.revolvedShell;
			if (!Has (spec, "basePlane") && (Has (spec, "axisOrigin") || Has (spec, "profileRotation"))) {
				const double* t = shell.basePlane.tmx;
				const API_Coord3D origin = OptPoint3D (spec, "axisOrigin", t[11]).value_or (API_Coord3D { t[3], t[7], t[11] });
				const double rotation = OptAngle (spec, "profileRotation").value_or (std::atan2 (t[4], t[0]));
				shell.basePlane = VerticalFrame (origin, rotation);
				m.Set (&shell.basePlane);
			}
			if (Has (spec, "axisBase")) {
				d.axisBase = FrameFrom (spec, "axisBase");
				m.Set (&d.axisBase);
			}
			if (auto a = OptAngle (spec, "revolutionAngle")) {
				if (*a <= 0.0 || *a > 2.0 * kPi + 1e-9) Fail ("'revolutionAngle' must be above 0 and at most 360 degrees.");
				d.revolutionAngle = *a;
				m.Set (&d.revolutionAngle);
			}
			angle ("beginAngle", d.begAngle);
			angle ("slantAngle", d.slantAngle);
			angle ("distortionAngle", d.distortionAngle);
			if (ApplyFlag (spec, "segmentedSurfaces", d.segmentedSurfaces))	m.Set (&d.segmentedSurfaces);
			if (auto n = OptInt (spec, "arcSegments")) {
				if (*n < 1 || *n > 360) Fail ("'arcSegments' must be 1..360.");
				d.segmentType = APIShellBase_SegmentsByArc;
				d.segmentsByArc = *n;
				m.Set (&d.segmentType);
				m.Set (&d.segmentsByArc);
			}
			if (auto n = OptInt (spec, "circleSegments")) {
				if (*n < 3 || *n > 360) Fail ("'circleSegments' must be 3..360.");
				d.segmentType = APIShellBase_SegmentsByCircle;
				d.segmentsByCircle = *n;
				m.Set (&d.segmentType);
				m.Set (&d.segmentsByCircle);
			}
			break;
		}
		case API_RuledShellID: {
			API_RuledShellData& d = shell.u.ruledShell;
			if (Has (spec, "plane1"))	{ d.plane1 = FrameFrom (spec, "plane1"); m.Set (&d.plane1); }
			if (Has (spec, "plane2"))	{ d.plane2 = FrameFrom (spec, "plane2"); m.Set (&d.plane2); }
			if (Has (spec, "morphingRule")) {
				d.morphingRule = (API_MorphingRuleID) ParseNamed (kMorphingRules, spec, "morphingRule");
				m.Set (&d.morphingRule);
			}
			break;
		}
	}
}


void CheckClassFields (API_ShellClassID cls, const OS& spec)
{
	struct Rule { const char* key; API_ShellClassID cls; };
	static const Rule rules[] = {
		{ "begin", API_ExtrudedShellID }, { "extrusion", API_ExtrudedShellID }, { "profileDirection", API_ExtrudedShellID },
		{ "profilePlaneTilt", API_ExtrudedShellID }, { "beginPlaneTilt", API_ExtrudedShellID }, { "endPlaneTilt", API_ExtrudedShellID },
		{ "axisOrigin", API_RevolvedShellID }, { "profileRotation", API_RevolvedShellID }, { "axisBase", API_RevolvedShellID },
		{ "revolutionAngle", API_RevolvedShellID }, { "beginAngle", API_RevolvedShellID }, { "distortionAngle", API_RevolvedShellID },
		{ "segmentedSurfaces", API_RevolvedShellID }, { "arcSegments", API_RevolvedShellID }, { "circleSegments", API_RevolvedShellID },
		{ "profile2", API_RuledShellID }, { "plane1", API_RuledShellID }, { "plane2", API_RuledShellID }, { "morphingRule", API_RuledShellID },
	};
	for (const Rule& r : rules) {
		if (Has (spec, r.key) && r.cls != cls)
			Fail ("Field '" + GS::UniString (r.key) + "' is only valid for " + NameOf (kShellClasses, r.cls) +
				  " shells (this shell is " + NameOf (kShellClasses, cls) + ").");
	}
	if (Has (spec, "slantAngle") && cls == API_RuledShellID)
		Fail ("Field 'slantAngle' is only valid for Extruded and Revolved shells.");
}


API_Guid CreateShell (const OS& spec)
{
	API_ShellClassID cls = API_ExtrudedShellID;
	if (Has (spec, "shellClass"))
		cls = (API_ShellClassID) ParseNamed (kShellClasses, spec, "shellClass");
	CheckClassFields (cls, spec);
	if (!Has (spec, "profile"))
		Fail ("Shell requires 'profile' (polyline of at least 2 points in the profile plane, meters).");
	if (cls == API_ExtrudedShellID && !Has (spec, "extrusion"))
		Fail ("Extruded shell requires 'extrusion' (3D vector: direction and length of the extrusion, e.g. {x:10, y:0, z:0}).");
	if (cls == API_RuledShellID && (!Has (spec, "profile2") || !Has (spec, "plane1") || !Has (spec, "plane2")))
		Fail ("Ruled shell requires 'profile', 'profile2', 'plane1' and 'plane2'.");

	API_Element element = NewElement (API_ShellID);
	element.shell.shellClass = cls;
	GetDefaults (element, nullptr);
	element.shell.shellClass = cls;

	// Predictable placement independent of the last shell drawn in the UI.
	element.shell.hasContour = false;
	element.shell.numHoles = 0;
	if (cls == API_RevolvedShellID) {
		const API_Coord3D origin = OptPoint3D (spec, "axisOrigin", 0.0).value_or (API_Coord3D { 0.0, 0.0, 0.0 });
		element.shell.basePlane = VerticalFrame (origin, OptAngle (spec, "profileRotation").value_or (0.0));
		API_RevolvedShellData& d = element.shell.u.revolvedShell;
		BNZeroMemory (&d.axisBase, sizeof (d.axisBase));
		d.axisBase.tmx[0] = 1.0;		// axis X = base X
		d.axisBase.tmx[6] = 1.0;		// axis Z = base Y (the vertical revolution axis)
		d.axisBase.tmx[9] = -1.0;		// axis Y = -base Z
		d.revolutionAngle = 2.0 * kPi;
		d.begAngle = 0.0;
		d.slantAngle = 0.0;
		d.distortionAngle = kPi / 2.0;
		d.distortionVector.x = 0.0;
		d.distortionVector.y = 0.0;
		if (d.segmentsByCircle < 3)
			d.segmentsByCircle = 36;
		if (d.segmentsByArc < 1)
			d.segmentsByArc = 12;
	} else {
		element.shell.basePlane = IdentityTranmat ();
	}
	if (cls == API_ExtrudedShellID) {
		API_ExtrudedShellData& d = element.shell.u.extrudedShell;
		BNZeroMemory (&d.begC, sizeof (d.begC));
	}

	ApplyCommonFields (element, nullptr, spec);
	ApplyShellFields (element, nullptr, spec);

	Memo memo;
	const Profile profile = ProfileFrom (spec, "profile", GetBool (spec, "closedProfile", false));
	WriteShape (profile, *memo, 0, ShapePoly (element.shell, 0));
	if (cls == API_RuledShellID) {
		const Profile profile2 = ProfileFrom (spec, "profile2", GetBool (spec, "closedProfile", false));
		WriteShape (profile2, *memo, 1, ShapePoly (element.shell, 1));
	}

	GSErrCode err = ACAPI_Element_Create (&element, memo.Ptr ());
	Check (err, "Cannot create shell" + GS::UniString (err == APIERR_IRREGULARPOLY || err == APIERR_BADPOLY
		? " (the profile is irregular: self-intersecting, repeated points or zero-length segments — fix the profile points)" : ""));
	return element.header.guid;
}


void SerializeShell (const API_Element& element, OS& out)
{
	const API_ShellType& shell = element.shell;
	out.Add ("shellClass", NameOf (kShellClasses, shell.shellClass));
	AddShellBaseJson (out, shell.shellBase);
	out.Add ("flipped", shell.isFlipped);
	out.Add ("basePlane", FrameJson (shell.basePlane));
	out.Add ("defaultEdgeType", NameOf (kEdgeTypes, shell.defEdgeType));
	out.Add ("hasClippingContour", shell.hasContour);
	out.Add ("holeCount", (Int32) shell.numHoles);

	switch (shell.shellClass) {
		case API_ExtrudedShellID: {
			const API_ExtrudedShellData& d = shell.u.extrudedShell;
			out.Add ("begin", Coord3DObj (d.begC));
			out.Add ("extrusion", Coord3DObj (d.extrusionVector));
			out.Add ("extrusionLength", std::sqrt (d.extrusionVector.x * d.extrusionVector.x + d.extrusionVector.y * d.extrusionVector.y + d.extrusionVector.z * d.extrusionVector.z));
			out.Add ("profileDirection", CoordObj (d.shapeDirection));
			AddAngle (out, "slantAngle", d.slantAngle);
			AddAngle (out, "profilePlaneTilt", d.shapePlaneTilt);
			AddAngle (out, "beginPlaneTilt", d.begPlaneTilt);
			AddAngle (out, "endPlaneTilt", d.endPlaneTilt);
			break;
		}
		case API_RevolvedShellID: {
			const API_RevolvedShellData& d = shell.u.revolvedShell;
			out.Add ("axisBase", FrameJson (d.axisBase));
			AddAngle (out, "revolutionAngle", d.revolutionAngle);
			AddAngle (out, "beginAngle", d.begAngle);
			AddAngle (out, "slantAngle", d.slantAngle);
			AddAngle (out, "distortionAngle", d.distortionAngle);
			out.Add ("distortionVector", CoordObj (d.distortionVector));
			out.Add ("segmentedSurfaces", d.segmentedSurfaces);
			out.Add ("curveSegmentation", NameOf (kSegmentTypes, d.segmentType));
			out.Add ("arcSegments", d.segmentsByArc);
			out.Add ("circleSegments", d.segmentsByCircle);
			break;
		}
		case API_RuledShellID: {
			const API_RuledShellData& d = shell.u.ruledShell;
			out.Add ("plane1", FrameJson (d.plane1));
			out.Add ("plane2", FrameJson (d.plane2));
			out.Add ("morphingRule", NameOf (kMorphingRules, d.morphingRule));
			break;
		}
	}

	Memo memo;
	if (ACAPI_Element_GetMemo (element.header.guid, memo.Ptr (), APIMemoMask_Polygon) == NoError) {
		const Profile p = ShapeToProfile (memo->shellShapes[0]);
		out.Add ("profile", ProfileJson (p));
		out.Add ("closedProfile", p.closed);
		if (shell.shellClass == API_RuledShellID) {
			const Profile p2 = ShapeToProfile (memo->shellShapes[1]);
			out.Add ("profile2", ProfileJson (p2));
			out.Add ("closedProfile2", p2.closed);
		}
	}
}


void ModifyShell (API_Element& element, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask, const OS& patch)
{
	API_ShellType& shell = element.shell;
	if (Has (patch, "shellClass")) {
		const API_ShellClassID cls = (API_ShellClassID) ParseNamed (kShellClasses, patch, "shellClass");
		if (cls != shell.shellClass)
			Fail ("Cannot change shellClass of an existing shell (it is " + NameOf (kShellClasses, shell.shellClass) +
				  "). Create a new shell with create_shells and delete this one.", APIERR_NOTSUPPORTED);
	}
	CheckClassFields (shell.shellClass, patch);
	ApplyShellFields (element, &mask, patch);

	const bool profileChange = Has (patch, "profile") || Has (patch, "closedProfile");
	const bool profile2Change = shell.shellClass == API_RuledShellID && Has (patch, "profile2");
	if (!profileChange && !profile2Change)
		return;

	// Load the current shapes + contours (the clipping / hole contours are sent back unchanged).
	LoadMemo (element.header.guid, memo, APIMemoMask_Polygon);
	const auto closed = OptBool (patch, "closedProfile");
	auto rewrite = [&] (Int32 k, const char* key) {
		Profile p = Has (patch, key) ? ProfileFrom (patch, key, closed.value_or (ShapeToProfile (memo.shellShapes[k]).closed))
									 : ShapeToProfile (memo.shellShapes[k]);
		if (!Has (patch, key)) {
			if (p.line.points.GetSize () < 2)
				Fail ("The shell has no readable profile; pass 'profile' explicitly.");
			if (closed.has_value ())
				p.closed = *closed;
		}
		API_Polygon& poly = ShapePoly (shell, k);
		WriteShape (p, memo, k, poly);
		const FieldMask m (element, &mask);
		m.Set (&poly);
	};
	if (profileChange)
		rewrite (0, "profile");
	if (profile2Change || (shell.shellClass == API_RuledShellID && Has (patch, "closedProfile")))
		rewrite (1, "profile2");
	memoMask |= APIMemoMask_Polygon | APIMemoMask_EdgeTrims;
}

} // namespace


void RegisterShellAdapter ()
{
	RegisterAdapter ({ API_ShellID, CreateShell, SerializeShell, ModifyShell });
}

} // namespace cc
