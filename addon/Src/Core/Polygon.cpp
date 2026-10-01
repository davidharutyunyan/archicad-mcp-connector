#include "Core/Polygon.hpp"

#include <cmath>

namespace cc {

namespace {

bool SamePoint (const API_Coord& a, const API_Coord& b)
{
	return std::fabs (a.x - b.x) < 1e-9 && std::fabs (a.y - b.y) < 1e-9;
}


Contour ContourFrom (const OS& value, const GS::UniString& fieldName, bool closed)
{
	Contour c;
	GS::Array<OS> pts;
	GS::Array<OS> arcs;

	// value is either {"points": [...], "arcs": [...]} or a wrapper {"_": [...]} created by callers
	if (value.Contains ("points")) {
		pts = GetObjectArray (value, "points");
		arcs = GetObjectArray (value, "arcs", false);
	} else {
		Fail ("Polygon '" + fieldName + "' must have a 'points' array.");
	}

	for (const OS& p : pts) {
		c.points.Push (CoordFrom (p));
		if (p.Contains ("z"))
			c.z.Push (GetDouble (p, "z"));
	}
	if (!c.z.IsEmpty () && c.z.GetSize () != c.points.GetSize ())
		Fail ("Polygon '" + fieldName + "': either all points or none must have 'z'.");

	if (closed && c.points.GetSize () >= 2 && SamePoint (c.points[0], c.points[c.points.GetSize () - 1])) {
		c.points.Pop ();
		if (!c.z.IsEmpty ())
			c.z.Pop ();
	}

	const Int32 minPts = closed ? 3 : 2;
	if ((Int32) c.points.GetSize () < minPts) {
		GS::UniString msg;
		msg.Printf (" needs at least %d points.", (int) minPts);
		Fail ("Polygon '" + fieldName + "'" + msg);
	}

	const Int32 n = (Int32) c.points.GetSize ();
	const Int32 edgeCount = closed ? n : n - 1;
	for (const OS& a : arcs) {
		Int32 idx = GetInt (a, "index");
		double angle = GetAngle (a, "angle");
		if (idx < 0 || idx >= edgeCount)
			Fail ("Polygon '" + fieldName + "': arc index out of range.");
		if (std::fabs (angle) > 1e-9)
			c.arcs.Push ({ idx, angle });
	}
	return c;
}


// Wraps a plain array field so ContourFrom can read it.
Contour ContourFromField (const OS& os, const char* key, const GS::UniString& fieldName, bool closed)
{
	if (os.IsList (key)) {
		OS wrapper;
		GS::Array<OS> pts = GetObjectArray (os, key);
		wrapper.Add ("points", pts);
		return ContourFrom (wrapper, fieldName, closed);
	}
	return ContourFrom (GetObject (os, key), fieldName, closed);
}


template <class T>
T** AllocHandle (Int32 count)
{
	T** h = reinterpret_cast<T**> (BMAllocateHandle (count * sizeof (T), ALLOCATE_CLEAR, 0));
	if (h == nullptr)
		Fail ("Out of memory.", APIERR_MEMFULL);
	return h;
}


void KillHandle (void* handlePtr)
{
	GSHandle* h = reinterpret_cast<GSHandle*> (handlePtr);
	if (*h != nullptr)
		BMKillHandle (h);
}

} // namespace


Int32 PolygonData::NumCoords () const
{
	Int32 n = (Int32) outline.points.GetSize () + 1;
	for (const Contour& h : holes)
		n += (Int32) h.points.GetSize () + 1;
	return n;
}


PolygonData PolygonFrom (const OS& value, const GS::UniString& fieldName)
{
	PolygonData data;
	data.outline = ContourFrom (value, fieldName, true);
	if (value.Contains ("holes")) {
		GS::Array<OS> holes = GetObjectArray (value, "holes");
		for (const OS& h : holes)
			data.holes.Push (ContourFrom (h, fieldName + ".holes", true));
	}
	data.hasZ = !data.outline.z.IsEmpty ();
	for (const Contour& h : data.holes) {
		if (data.hasZ != !h.z.IsEmpty ())
			Fail ("Polygon '" + fieldName + "': either all points (outline and holes) or none must have 'z'.");
	}
	return data;
}


PolygonData GetPolygon (const OS& os, const char* key)
{
	if (!os.Contains (key))
		Fail ("Missing required polygon field '" + GS::UniString (key) + "'.");
	if (os.IsList (key)) {
		OS wrapper;
		wrapper.Add ("points", GetObjectArray (os, key));
		return PolygonFrom (wrapper, key);
	}
	return PolygonFrom (GetObject (os, key), key);
}


Contour GetPolyline (const OS& os, const char* key)
{
	if (!os.Contains (key))
		Fail ("Missing required polyline field '" + GS::UniString (key) + "'.");
	return ContourFromField (os, key, key, false);
}


double SignedArea (const GS::Array<API_Coord>& pts)
{
	double a = 0.0;
	const UIndex n = pts.GetSize ();
	for (UIndex i = 0; i < n; ++i) {
		const API_Coord& p = pts[i];
		const API_Coord& q = pts[(i + 1) % n];
		a += p.x * q.y - q.x * p.y;
	}
	return a / 2.0;
}


static void ReverseContour (Contour& c)
{
	const Int32 n = (Int32) c.points.GetSize ();
	GS::Array<API_Coord> pts;
	GS::Array<double> z;
	for (Int32 i = n - 1; i >= 0; --i) {
		pts.Push (c.points[i]);
		if (!c.z.IsEmpty ())
			z.Push (c.z[i]);
	}
	// Edge i (points[i] -> points[i+1]) becomes edge (n - 2 - i) mod n with negated angle.
	GS::Array<std::pair<Int32, double>> arcs;
	for (const auto& a : c.arcs) {
		Int32 newIdx = ((n - 2 - a.first) % n + n) % n;
		arcs.Push ({ newIdx, -a.second });
	}
	c.points = pts;
	c.z = z;
	c.arcs = arcs;
}


void NormalizeOrientation (PolygonData& data)
{
	if (SignedArea (data.outline.points) < 0)
		ReverseContour (data.outline);
	for (Contour& h : data.holes) {
		if (SignedArea (h.points) > 0)
			ReverseContour (h);
	}
}


void WritePolygonToMemo (const PolygonData& input, API_Polygon& poly, API_ElementMemo& memo, bool withZ)
{
	PolygonData data = input;
	NormalizeOrientation (data);

	KillHandle (&memo.coords);
	KillHandle (&memo.pends);
	KillHandle (&memo.parcs);
	KillHandle (&memo.vertexIDs);
	if (withZ)
		KillHandle (&memo.meshPolyZ);

	GS::Array<const Contour*> contours;
	contours.Push (&data.outline);
	for (const Contour& h : data.holes)
		contours.Push (&h);

	const Int32 nCoords = data.NumCoords ();
	Int32 nArcs = 0;
	for (const Contour* c : contours)
		nArcs += (Int32) c->arcs.GetSize ();

	memo.coords = AllocHandle<API_Coord> (nCoords + 1);
	memo.pends = AllocHandle<Int32> ((Int32) contours.GetSize () + 1);
	memo.parcs = nArcs > 0 ? AllocHandle<API_PolyArc> (nArcs) : nullptr;
	memo.vertexIDs = AllocHandle<UInt32> (nCoords + 1);
	if (withZ)
		memo.meshPolyZ = AllocHandle<double> (nCoords + 1);

	Int32 idx = 1;
	Int32 arcIdx = 0;
	UInt32 vertexId = 1;
	(*memo.pends)[0] = 0;
	for (UIndex ci = 0; ci < contours.GetSize (); ++ci) {
		const Contour& c = *contours[ci];
		const Int32 first = idx;
		const Int32 n = (Int32) c.points.GetSize ();
		for (Int32 i = 0; i < n; ++i) {
			(*memo.coords)[idx] = c.points[i];
			(*memo.vertexIDs)[idx] = vertexId++;
			if (withZ)
				(*memo.meshPolyZ)[idx] = c.z.IsEmpty () ? 0.0 : c.z[i];
			++idx;
		}
		// closing vertex
		(*memo.coords)[idx] = c.points[0];
		(*memo.vertexIDs)[idx] = (*memo.vertexIDs)[first];
		if (withZ)
			(*memo.meshPolyZ)[idx] = c.z.IsEmpty () ? 0.0 : c.z[0];
		(*memo.pends)[ci + 1] = idx;
		++idx;

		for (const auto& a : c.arcs) {
			(*memo.parcs)[arcIdx].begIndex = first + a.first;
			(*memo.parcs)[arcIdx].endIndex = first + a.first + 1;
			(*memo.parcs)[arcIdx].arcAngle = a.second;
			++arcIdx;
		}
	}
	(*memo.vertexIDs)[0] = vertexId - 1;	// max vertex id

	poly.nCoords = nCoords;
	poly.nSubPolys = (Int32) contours.GetSize ();
	poly.nArcs = nArcs;
}


void WritePolylineToMemo (const Contour& line, API_Polygon& poly, API_ElementMemo& memo)
{
	KillHandle (&memo.coords);
	KillHandle (&memo.pends);
	KillHandle (&memo.parcs);

	const Int32 n = (Int32) line.points.GetSize ();
	memo.coords = AllocHandle<API_Coord> (n + 1);
	memo.pends = AllocHandle<Int32> (2);
	const Int32 nArcs = (Int32) line.arcs.GetSize ();
	memo.parcs = nArcs > 0 ? AllocHandle<API_PolyArc> (nArcs) : nullptr;

	for (Int32 i = 0; i < n; ++i)
		(*memo.coords)[i + 1] = line.points[i];
	(*memo.pends)[0] = 0;
	(*memo.pends)[1] = n;
	for (Int32 i = 0; i < nArcs; ++i) {
		(*memo.parcs)[i].begIndex = line.arcs[i].first + 1;
		(*memo.parcs)[i].endIndex = line.arcs[i].first + 2;
		(*memo.parcs)[i].arcAngle = line.arcs[i].second;
	}
	poly.nCoords = n;
	poly.nSubPolys = 1;
	poly.nArcs = nArcs;
}


static OS ContourJson (const API_Polygon& poly, const API_ElementMemo& memo, Int32 begin, Int32 end, bool withZ, bool closed)
{
	// begin..end are memo coord indices (inclusive); for closed contours end is the closing vertex.
	GS::Array<OS> pts;
	// Never read past the memo handles (inconsistent nCoords / pends would otherwise crash Archicad).
	const Int32 coordCap = (Int32) (BMGetHandleSize (reinterpret_cast<GSHandle> (memo.coords)) / sizeof (API_Coord)) - 1;
	const Int32 zCap = memo.meshPolyZ != nullptr ? (Int32) (BMGetHandleSize (reinterpret_cast<GSHandle> (memo.meshPolyZ)) / sizeof (double)) - 1 : -1;
	Int32 last = closed ? end - 1 : end;
	if (last > coordCap)
		last = coordCap;
	if (begin < 1)
		begin = 1;
	for (Int32 i = begin; i <= last; ++i) {
		const API_Coord& c = (*memo.coords)[i];
		if (withZ && memo.meshPolyZ != nullptr && i <= zCap)
			pts.Push (Coord3DObj (c.x, c.y, (*memo.meshPolyZ)[i]));
		else
			pts.Push (CoordObj (c));
	}
	GS::Array<OS> arcs;
	if (memo.parcs != nullptr) {
		for (Int32 a = 0; a < poly.nArcs; ++a) {
			const API_PolyArc& arc = (*memo.parcs)[a];
			if (arc.begIndex >= begin && arc.begIndex <= last)
				arcs.Push (OS ("index", (Int32) (arc.begIndex - begin), "angle", RadToDeg (arc.arcAngle)));
		}
	}
	OS out;
	out.Add ("points", pts);
	if (!arcs.IsEmpty ())
		out.Add ("arcs", arcs);
	return out;
}


OS PolygonToJson (const API_Polygon& poly, const API_ElementMemo& memo, bool withZ)
{
	if (memo.coords == nullptr || memo.pends == nullptr || poly.nSubPolys < 1)
		return OS ("points", GS::Array<OS> ());
	const Int32 pendCap = (Int32) (BMGetHandleSize (reinterpret_cast<GSHandle> (memo.pends)) / sizeof (Int32)) - 1;
	const Int32 nSubPolys = poly.nSubPolys < pendCap ? poly.nSubPolys : pendCap;
	if (nSubPolys < 1)
		return OS ("points", GS::Array<OS> ());

	OS out = ContourJson (poly, memo, 1, (*memo.pends)[1], withZ, true);
	GS::Array<OS> holes;
	for (Int32 k = 2; k <= nSubPolys; ++k)
		holes.Push (ContourJson (poly, memo, (*memo.pends)[k - 1] + 1, (*memo.pends)[k], withZ, true));
	if (!holes.IsEmpty ())
		out.Add ("holes", holes);
	return out;
}


OS PolylineToJson (const API_Polygon& poly, const API_ElementMemo& memo)
{
	if (memo.coords == nullptr)
		return OS ("points", GS::Array<OS> ());
	return ContourJson (poly, memo, 1, poly.nCoords, false, false);
}

} // namespace cc
