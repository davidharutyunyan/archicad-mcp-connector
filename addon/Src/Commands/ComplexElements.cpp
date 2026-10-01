// *****************************************************************************
// ComplexElements — family "complex-elements": Morph / CurtainWall / Stair /
// Railing adapters (+ serializers for their sub-elements) and GetMorphGeometry.
//
//   ComplexElementsMorph.cpp         Morph adapter (box / extrusion / BREP mesh bodies), GetMorphGeometry
//   ComplexElementsCurtainWall.cpp   CurtainWall adapter + Segment/Frame/Panel/Junction/Accessory adapters
//   ComplexElementsStairRailing.cpp  Stair + Railing adapters + Riser/Tread/StairStructure/Railing part adapters
//
// This file holds the shared helpers (ComplexElementsCommon.hpp) and the family registration.
// *****************************************************************************

#include "Commands/Commands.hpp"
#include "Commands/ComplexElementsCommon.hpp"

#include <cmath>

namespace cc {
namespace complex {

// --- Field appliers ------------------------------------------------------------------

bool ApplyLength (const OS& spec, const char* key, double& value, bool positive, bool nonNegative)
{
	auto v = OptDouble (spec, key);
	if (!v.has_value ())
		return false;
	if (!std::isfinite (*v))
		Fail ("Field '" + GS::UniString (key) + "' must be a finite number (meters).");
	if (positive && *v <= 0.0)
		Fail ("Field '" + GS::UniString (key) + "' must be greater than 0 (meters).");
	if (nonNegative && *v < 0.0)
		Fail ("Field '" + GS::UniString (key) + "' must not be negative (meters).");
	value = *v;
	return true;
}


bool ApplyAngleField (const OS& spec, const char* key, double& radians)
{
	auto v = OptAngle (spec, key);
	if (!v.has_value ())
		return false;
	radians = *v;
	return true;
}


bool ApplyFlag (const OS& spec, const char* key, bool& value)
{
	auto v = OptBool (spec, key);
	if (!v.has_value ())
		return false;
	value = *v;
	return true;
}


bool ApplyPenField (const OS& spec, const char* key, short& pen)
{
	auto v = OptInt (spec, key);
	if (!v.has_value ())
		return false;
	if (*v < 1 || *v > 255)
		Fail ("Field '" + GS::UniString (key) + "' must be a pen index 1-255.");
	pen = (short) *v;
	return true;
}


bool ApplyAttrField (const OS& spec, const char* key, API_AttrTypeID type, API_AttributeIndex& index)
{
	auto v = OptAttr (type, spec, key);
	if (!v.has_value ())
		return false;
	index = *v;
	return true;
}


static bool ApplyOnePenOverride (const OS& spec, const char* key, short& pen, bool& overridden)
{
	if (!spec.Contains (key))
		return false;
	if (spec.IsBool (key)) {
		if (GetBool (spec, key))
			Fail ("Field '" + GS::UniString (key) + "' must be a pen index 1-255, or false to remove the override.");
		overridden = false;
		return true;
	}
	ApplyPenField (spec, key, pen);
	overridden = true;
	return true;
}


bool ApplyCutFillPenOverride (const OS& spec, API_PenOverrideType& penOverride)
{
	bool changed = ApplyOnePenOverride (spec, "cutFillPen", penOverride.cutFillPen, penOverride.overrideCutFillPen);
	changed = ApplyOnePenOverride (spec, "cutFillBackgroundPen", penOverride.cutFillBackgroundPen, penOverride.overrideCutFillBackgroundPen) || changed;
	return changed;
}


void AddCutFillPenOverrideJson (OS& out, const API_PenOverrideType& penOverride)
{
	if (penOverride.overrideCutFillPen)
		out.Add ("cutFillPen", (Int32) penOverride.cutFillPen);
	if (penOverride.overrideCutFillBackgroundPen)
		out.Add ("cutFillBackgroundPen", (Int32) penOverride.cutFillBackgroundPen);
}


void ApplyStoryCreationMode (API_LinkToSettings& link, const OS& spec)
{
	if (spec.Contains ("storyIndex")) {
		link.newCreationMode = false;
		link.homeStoryDifference = 0;
	}
}

// --- Geometry ------------------------------------------------------------------------

Contour GetPath (const OS& spec, const char* key, bool allowBeginEnd, Int32 minPoints)
{
	Contour path;
	if (spec.Contains (key)) {
		path = GetPolyline (spec, key);
	} else if (allowBeginEnd && spec.Contains ("begin") && spec.Contains ("end")) {
		const OS b = GetObject (spec, "begin");
		const OS e = GetObject (spec, "end");
		path.points.Push (CoordFrom (b));
		path.points.Push (CoordFrom (e));
		if (b.Contains ("z") || e.Contains ("z")) {
			path.z.Push (GetDouble (b, "z", 0.0));
			path.z.Push (GetDouble (e, "z", 0.0));
		}
	} else {
		Fail (GS::UniString ("Missing '") + key + "'" + (allowBeginEnd ? " (or 'begin' + 'end')" : "") +
			  ": give the path as an array of points {x, y} in meters.");
	}

	if ((Int32) path.points.GetSize () < minPoints) {
		GS::UniString msg;
		msg.Printf ("'%s' needs at least %d points.", key, (int) minPoints);
		Fail (msg);
	}
	for (UIndex i = 1; i < path.points.GetSize (); ++i) {
		const API_Coord& a = path.points[i - 1];
		const API_Coord& b = path.points[i];
		if (std::hypot (b.x - a.x, b.y - a.y) < 1e-6) {
			GS::UniString msg;
			msg.Printf ("'%s': points %d and %d coincide (zero-length segment); remove the duplicate point.", key, (int) i - 1, (int) i);
			Fail (msg);
		}
	}
	return path;
}


OS HandlePolylineJson (API_Coord** coords, API_PolyArc** parcs, Int32 nCoords, Int32 nArcs, double** z)
{
	GS::Array<OS> pts;
	GS::Array<OS> arcs;
	const Int32 available = (Int32) HdlCount (coords) - 1;
	if (nCoords <= 0 || nCoords > available)
		nCoords = available;
	const Int32 zCount = (Int32) HdlCount (z);
	for (Int32 i = 1; i <= nCoords; ++i) {
		const API_Coord& c = (*coords)[i];
		if (z != nullptr && i < zCount)
			pts.Push (Coord3DObj (c.x, c.y, (*z)[i]));
		else
			pts.Push (CoordObj (c));
	}
	const Int32 arcsAvailable = (Int32) HdlCount (parcs);
	if (nArcs <= 0 || nArcs > arcsAvailable)
		nArcs = arcsAvailable;
	for (Int32 a = 0; a < nArcs; ++a) {
		const API_PolyArc& arc = (*parcs)[a];
		if (std::fabs (arc.arcAngle) < 1e-9 || arc.begIndex < 1 || arc.begIndex > nCoords)
			continue;
		arcs.Push (OS ("index", (Int32) (arc.begIndex - 1), "angle", RadToDeg (arc.arcAngle)));
	}
	OS out;
	out.Add ("points", pts);
	if (!arcs.IsEmpty ())
		out.Add ("arcs", arcs);
	return out;
}


static double SegmentLength (const API_Coord& a, const API_Coord& b, double arcAngle)
{
	const double chord = std::hypot (b.x - a.x, b.y - a.y);
	if (std::fabs (arcAngle) < 1e-9)
		return chord;
	const double radius = chord / (2.0 * std::sin (std::fabs (arcAngle) / 2.0));
	return radius * std::fabs (arcAngle);
}


double PathLength (const Contour& path)
{
	double length = 0.0;
	for (UIndex i = 1; i < path.points.GetSize (); ++i) {
		double angle = 0.0;
		for (const auto& a : path.arcs) {
			if (a.first == (Int32) i - 1)
				angle = a.second;
		}
		length += SegmentLength (path.points[i - 1], path.points[i], angle);
	}
	return length;
}


double HandlePathLength (API_Coord** coords, API_PolyArc** parcs, Int32 nCoords, Int32 nArcs)
{
	const Int32 available = (Int32) HdlCount (coords) - 1;
	if (nCoords <= 0 || nCoords > available)
		nCoords = available;
	const Int32 arcsAvailable = (Int32) HdlCount (parcs);
	if (nArcs <= 0 || nArcs > arcsAvailable)
		nArcs = arcsAvailable;
	double length = 0.0;
	for (Int32 i = 2; i <= nCoords; ++i) {
		double angle = 0.0;
		for (Int32 a = 0; a < nArcs; ++a) {
			if ((*parcs)[a].begIndex == i - 1)
				angle = (*parcs)[a].arcAngle;
		}
		length += SegmentLength ((*coords)[i - 1], (*coords)[i], angle);
	}
	return length;
}


void AddBoundingBox (OS& out, const API_Elem_Head& head)
{
	API_Elem_Head h = head;
	API_Box3D box;
	BNZeroMemory (&box, sizeof (box));
	if (ACAPI_Database (APIDb_CalcBoundsID, &h, &box) == NoError)
		out.Add ("boundingBox", Box3DObj (box));
}


void AddOwner (OS& out, const API_Guid& owner)
{
	if (owner != APINULLGuid)
		out.Add ("owner", GuidStr (owner));
}

} // namespace complex


void RegisterComplexElementCommands ()
{
	complex::RegisterMorphFamily ();
	complex::RegisterCurtainWallFamily ();
	complex::RegisterStairRailingFamily ();
}

} // namespace cc
