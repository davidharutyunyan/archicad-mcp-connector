// *****************************************************************************
// Zones — Zone adapter (API_ZoneID) and zone utilities.
//
// Create / modify fields (lengths in meters, angles in degrees):
//   geometry (create: exactly one of them is required):
//     polygon {points, arcs?, holes?}   manual zone with a fixed outline
//     referencePoint {x,y}              automatic zone: Archicad finds the boundary (walls, columns,
//                                       room separator lines ...) around this point on the zone's story
//     boundary "InnerEdge"|"ReferenceLine"   automatic zones: follow the inner wall faces or the wall reference lines
//     automatic (modify only)           false = freeze an automatic zone as manual; true = make automatic (needs referencePoint
//                                       unless it is already automatic)
//     showFoundPolygon                  raw show_found_poly flag (ReferenceLine zones)
//   identity: name, number, category (ZoneCategory attribute)
//   vertical: height (unlinks the top unless topLinkedStory is given), topLinkedStory, topOffset,
//             bottomOffset (from the home story), floorThickness (sub-floor), areaReduction (%)
//   stamp:    stampPosition {x,y}, stampAngle, fixedStampAngle, stampPen, useStampPens,
//             stamp (Zone library part), stampParameters {gdlName: value}
//   look:     surface, useSurfaceForAllFaces, showFill, fill, fillPen, fillBackgroundPen, fillFromSurface,
//             showContour, contourPen, contourLineType, fillOrigin {x,y}, fillAngle, localFillOrientation
//   + common: layer, storyIndex, renovationStatus, elementId
//
// Serialization adds: quantities (API_ZoneAllQuantity: area, netArea, calculatedArea, perimeter,
// volume, walls/doors/windows surfaces ...), polygon (for ReferenceLine zones: the inner-edge outline)
// + referenceLinePolygon (ReferenceLine zones: the outline on the wall reference lines),
// bottomElevation, stamp library part, and relations (boundary walls/beams/curtain wall segments,
// contained elements grouped by type).
//
// Commands:
//   GetZones     — zone schedule: filtered/sorted list with areas and totals
//   UpdateZones  — re-detects the boundary of automatic zones (Archicad 26 has no API for
//                  Design > Update Zones: a temporary automatic zone is placed at each zone's reference
//                  point and its polygon is written back into the zone, or the zone is recreated).
// *****************************************************************************

#include "Commands/Commands.hpp"
#include "Core/Command.hpp"
#include "Core/Elements.hpp"
#include "Core/Enums.hpp"
#include "Core/LibParts.hpp"
#include "Core/Polygon.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <vector>

namespace cc {

namespace {

constexpr double	kCoordTolerance = 1e-5;
constexpr Int32		kRoomNoLen = 32;			// API_ZoneType::roomNoStr capacity

const NamedValue kBoundaryMethods[] = {
	{ "InnerEdge",		0 },
	{ "ReferenceLine",	1 },
};

enum UpdateMethod { CopyBoundary = 0, Recreate = 1 };

const NamedValue kUpdateMethods[] = {
	{ "copyBoundary",	CopyBoundary },
	{ "recreate",		Recreate },
};

// --- Small helpers -------------------------------------------------------------------

// Accepts a string or a number (e.g. zone number 101) and returns it as text.
std::optional<GS::UniString> OptText (const OS& os, const char* key)
{
	if (!os.Contains (key))
		return std::nullopt;
	if (IsNumber (os, key)) {
		const double v = GetDouble (os, key);
		if (std::fabs (v - std::round (v)) < 1e-9 && std::fabs (v) < 2.0e9)
			return GS::ValueToUniString ((Int32) std::lround (v));
		GS::UniString s;
		s = GS::UniString::Printf ("%g", v);
		return s;
	}
	return GetString (os, key);
}


// NOTE: GS::UniString::Contains (str, from, range) has no case flag: passing CaseInsensitive (= 1) as the
// second argument silently starts the search at index 1.
bool ContainsIgnoreCase (const GS::UniString& text, const GS::UniString& part)
{
	return text.ToLowerCase ().Contains (part.ToLowerCase ());
}


void SetUStr (GS::uchar_t* dst, Int32 capacity, const GS::UniString& value, const char* field)
{
	if ((Int32) value.GetLength () >= capacity) {
		GS::UniString msg;
		msg = GS::UniString::Printf ("' is too long (max %d characters).", (int) (capacity - 1));
		Fail ("Zone field '" + GS::UniString (field) + msg);
	}
	BNZeroMemory (dst, capacity * sizeof (GS::uchar_t));
	GS::ucsncpy (dst, value.ToUStr ().Get (), capacity - 1);
	dst[capacity - 1] = 0;
}


std::optional<short> OptPen (const OS& os, const char* key, Int32 minValue = 1)
{
	auto v = OptInt (os, key);
	if (!v.has_value ())
		return std::nullopt;
	if (*v < minValue || *v > 255) {
		GS::UniString msg;
		msg = GS::UniString::Printf ("' must be a pen index %d..255.", (int) minValue);
		Fail ("Field '" + GS::UniString (key) + msg);
	}
	return (short) *v;
}


GS::UniString BoundaryName (const API_ZoneType& zone)
{
	return zone.refLineFlag ? "ReferenceLine" : "InnerEdge";
}


GS::UniString CoordText (const API_Coord& c)
{
	GS::UniString s;
	s = GS::UniString::Printf ("(%.3f, %.3f)", c.x, c.y);
	return s;
}


// Interior point of a polygon (used as the default stamp position of manual zones):
// the area centroid when it lies inside, otherwise the middle of the widest scanline span.
bool PointInPolygon (const API_Coord& p, const PolygonData& data)
{
	bool inside = false;
	auto test = [&] (const GS::Array<API_Coord>& ring) {
		const UIndex n = ring.GetSize ();
		for (UIndex i = 0, j = n - 1; i < n; j = i++) {
			const API_Coord& a = ring[i];
			const API_Coord& b = ring[j];
			if (((a.y > p.y) != (b.y > p.y)) && (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x))
				inside = !inside;
		}
	};
	test (data.outline.points);
	for (const Contour& h : data.holes)
		test (h.points);
	return inside;
}


API_Coord InteriorPoint (const PolygonData& data)
{
	const GS::Array<API_Coord>& pts = data.outline.points;
	const UIndex n = pts.GetSize ();
	API_Coord c = { 0.0, 0.0 };
	double a = 0.0, cx = 0.0, cy = 0.0;
	for (UIndex i = 0; i < n; ++i) {
		const API_Coord& p = pts[i];
		const API_Coord& q = pts[(i + 1) % n];
		const double cross = p.x * q.y - q.x * p.y;
		a += cross;
		cx += (p.x + q.x) * cross;
		cy += (p.y + q.y) * cross;
	}
	if (std::fabs (a) > 1e-12) {
		c.x = cx / (3.0 * a);
		c.y = cy / (3.0 * a);
	} else {
		for (const API_Coord& p : pts) { c.x += p.x; c.y += p.y; }
		c.x /= (double) n;
		c.y /= (double) n;
	}
	if (PointInPolygon (c, data))
		return c;

	// Scanline through the centroid height: take the widest inside span.
	std::vector<double> xs;
	auto addRing = [&] (const GS::Array<API_Coord>& ring) {
		const UIndex m = ring.GetSize ();
		for (UIndex i = 0; i < m; ++i) {
			const API_Coord& p = ring[i];
			const API_Coord& q = ring[(i + 1) % m];
			if ((p.y > c.y) != (q.y > c.y))
				xs.push_back (p.x + (c.y - p.y) * (q.x - p.x) / (q.y - p.y));
		}
	};
	addRing (pts);
	for (const Contour& h : data.holes)
		addRing (h.points);
	std::sort (xs.begin (), xs.end ());
	double bestWidth = -1.0;
	API_Coord best = pts[0];
	for (size_t i = 0; i + 1 < xs.size (); i += 2) {
		const double w = xs[i + 1] - xs[i];
		if (w > bestWidth) {
			bestWidth = w;
			best.x = (xs[i] + xs[i + 1]) / 2.0;
			best.y = c.y;
		}
	}
	return best;
}

// --- Polygon views / comparison -------------------------------------------------------

struct PolyView {
	Int32			nCoords = 0;
	Int32			nSubPolys = 0;
	Int32			nArcs = 0;
	API_Coord**		coords = nullptr;
	Int32**			pends = nullptr;
	API_PolyArc**	parcs = nullptr;
};


PolyView MainPoly (const API_Element& e, const API_ElementMemo& m)
{
	PolyView v;
	v.nCoords = e.zone.poly.nCoords;
	v.nSubPolys = e.zone.poly.nSubPolys;
	v.nArcs = e.zone.poly.nArcs;
	v.coords = m.coords;
	v.pends = m.pends;
	v.parcs = m.parcs;
	return v;
}


PolyView FoundPoly (const API_Element& e, const API_ElementMemo& m)
{
	PolyView v;
	v.nCoords = e.zone.refPoly.nCoords;
	v.nSubPolys = e.zone.refPoly.nSubPolys;
	v.nArcs = e.zone.refPoly.nArcs;
	v.coords = m.additionalPolyCoords;
	v.pends = m.additionalPolyPends;
	v.parcs = m.additionalPolyParcs;
	return v;
}


template <class T>
Int32 HandleCount (T** h)
{
	if (h == nullptr)
		return 0;
	return (Int32) (BMGetHandleSize (reinterpret_cast<GSConstHandle> (h)) / sizeof (T));
}


bool IsValid (const PolyView& v)
{
	if (v.nCoords <= 0)
		return true;
	return HandleCount (v.coords) >= v.nCoords + 1 &&
		   HandleCount (v.pends) >= v.nSubPolys + 1 &&
		   (v.nArcs == 0 || HandleCount (v.parcs) >= v.nArcs);
}


bool ContainsAllVertices (const PolyView& a, const PolyView& b)
{
	for (Int32 i = 1; i <= a.nCoords; ++i) {
		const API_Coord& p = (*a.coords)[i];
		bool found = false;
		for (Int32 j = 1; j <= b.nCoords && !found; ++j) {
			const API_Coord& q = (*b.coords)[j];
			found = std::fabs (p.x - q.x) < kCoordTolerance && std::fabs (p.y - q.y) < kCoordTolerance;
		}
		if (!found)
			return false;
	}
	return true;
}


bool SamePolygon (const PolyView& a, const PolyView& b)
{
	if (a.nCoords != b.nCoords || a.nSubPolys != b.nSubPolys || a.nArcs != b.nArcs)
		return false;
	if (a.nCoords <= 0)
		return true;
	if (!IsValid (a) || !IsValid (b))
		return false;
	if (!ContainsAllVertices (a, b) || !ContainsAllVertices (b, a))
		return false;
	double arcsA = 0.0, arcsB = 0.0;
	for (Int32 i = 0; i < a.nArcs; ++i) arcsA += std::fabs ((*a.parcs)[i].arcAngle);
	for (Int32 i = 0; i < b.nArcs; ++i) arcsB += std::fabs ((*b.parcs)[i].arcAngle);
	return std::fabs (arcsA - arcsB) < 1e-6;
}


OS PolyViewToJson (const PolyView& v)
{
	if (v.nCoords <= 0 || !IsValid (v))
		return OS ("points", GS::Array<OS> ());
	API_ElementMemo tmp;
	BNZeroMemory (&tmp, sizeof (tmp));
	tmp.coords = v.coords;
	tmp.pends = v.pends;
	tmp.parcs = v.parcs;
	API_Polygon poly;
	BNZeroMemory (&poly, sizeof (poly));
	poly.nCoords = v.nCoords;
	poly.nSubPolys = v.nSubPolys;
	poly.nArcs = v.nArcs;
	return PolygonToJson (poly, tmp);		// tmp only aliases the handles: it is NOT disposed
}

// ReferenceLine zones: zone.poly is the inner-edge outline, the "found" polygon (refPoly, memo additionalPoly*)
// follows the wall reference lines and is the one Archicad measures 'area' on.
void AddReferenceLinePolygon (OS& out, const API_Element& element, const API_ElementMemo& memo)
{
	if (element.zone.manual || !element.zone.refLineFlag)
		return;
	const PolyView found = FoundPoly (element, memo);
	if (found.nCoords > 0 && found.coords != nullptr)
		out.Add ("referenceLinePolygon", PolyViewToJson (found));
}

// --- Quantities / relations / reductions ------------------------------------------------

bool GetZoneQuantity (const API_Guid& guid, API_ZoneAllQuantity& out)
{
	API_QuantityPar params;
	BNZeroMemory (&params, sizeof (params));
	params.minOpeningSize = 1e-6;

	API_ElementQuantity quantity;
	BNZeroMemory (&quantity, sizeof (quantity));
	API_Quantities quantities;
	quantities.elements = &quantity;

	API_QuantitiesMask mask;
	ACAPI_ELEMENT_QUANTITY_MASK_SETFULL (mask);
	if (ACAPI_Element_GetQuantities (guid, &params, &quantities, &mask) != NoError)
		return false;
	out = quantity.zone;
	return true;
}


OS QuantitiesToJson (const API_ZoneAllQuantity& q)
{
	OS out;
	out.Add ("area", q.area);
	out.Add ("netArea", q.netarea);
	out.Add ("calculatedArea", q.calcArea);
	out.Add ("reducedArea", q.reducementArea);
	out.Add ("perimeter", q.perimeter);
	out.Add ("netPerimeter", q.netperimeter);
	out.Add ("holesPerimeter", q.holesPrm);
	out.Add ("wallsPerimeter", q.wallsPrm);
	out.Add ("allCorners", q.allCorners);
	out.Add ("concaveCorners", q.concaveCorners);
	out.Add ("wallsSurface", q.wallsSurf);
	out.Add ("doorsWidth", q.doorsWidth);
	out.Add ("doorsSurface", q.doorsSurf);
	out.Add ("windowsWidth", q.windowsWidth);
	out.Add ("windowsSurface", q.windowsSurf);
	out.Add ("baseLevel", q.baseLevel);
	out.Add ("floorThickness", q.floorThick);
	out.Add ("height", q.height);
	out.Add ("volume", q.volume);
	out.Add ("totalExtractedArea", q.totalExtrArea);
	out.Add ("areaFactor", q.reducedExtrArea);		// = 1 - areaReduction/100 (verified live; ROOM_AREA_FACTOR)
	out.Add ("lowExtractedArea", q.lowExtrArea);
	out.Add ("wallExtractedArea", q.wallExtrArea);
	out.Add ("curtainWallExtractedArea", q.curtainWallExtrArea);
	out.Add ("columnExtractedArea", q.coluExtrArea);
	out.Add ("fillExtractedArea", q.fillExtrArea);
	out.Add ("insetTopSurface", q.insetTopSurf);
	out.Add ("insetBackSurface", q.insetBackSurf);
	out.Add ("insetSideSurface", q.insetSideSurf);
	return out;
}


struct RoomRelationGuard {
	API_RoomRelation rel {};
	~RoomRelationGuard () { ACAPI_DisposeRoomRelationHdls (&rel); }
};


OS WallPartJson (const API_WallPart& part)
{
	OS o;
	o.Add ("guid", GuidStr (part.guid));
	o.Add ("zoneEdge", part.roomEdge);
	o.Add ("tBegin", part.tBeg);
	o.Add ("tEnd", part.tEnd);
	return o;
}


OS ZoneRelationsJson (const API_Guid& guid)
{
	RoomRelationGuard guard;
	const GSErrCode err = ACAPI_Element_GetRelations (guid, API_ElemType (API_ZombieElemID), &guard.rel);
	if (err != NoError)
		return OS ("error", GS::UniString ("Cannot read zone relations: " + ErrorName (err)));

	OS out;
	OS grouped;
	for (auto it = guard.rel.elementsGroupedByType.EnumeratePairs (); it != nullptr; ++it) {
		GS::Array<GS::UniString> guids;
		for (const API_Guid& g : *it->value)
			guids.Push (GuidStr (g));
		grouped.Add (ToStr (ElemTypeName (*it->key)), guids);
	}
	out.Add ("elementsByType", grouped);

	GS::Array<OS> walls;
	for (const API_WallPart& part : guard.rel.wallPart)
		walls.Push (WallPartJson (part));
	out.Add ("boundaryWalls", walls);

	GS::Array<OS> beams;
	for (const API_BeamPart& part : guard.rel.beamPart) {
		OS o;
		o.Add ("guid", GuidStr (part.guid));
		o.Add ("tBegin", part.tBeg);
		o.Add ("tEnd", part.tEnd);
		beams.Push (o);
	}
	out.Add ("boundaryBeams", beams);

	GS::Array<OS> cwSegments;
	for (const API_CWSegmentPart& part : guard.rel.cwSegmentPart)
		cwSegments.Push (WallPartJson (part));
	out.Add ("boundaryCurtainWallSegments", cwSegments);

	GS::Array<double> nicheHeights;
	for (const API_Niche& niche : guard.rel.niches)
		nicheHeights.Push (niche.height);
	out.Add ("nicheHeights", nicheHeights);
	return out;
}


// APIDb_RoomReductionsID reports reductions through a plain C callback without user data,
// so the sink is a (main-thread only) static pointer set for the duration of the call.
GS::Array<OS>* gReductionSink = nullptr;


GS::UniString ReductionTypeName (short type)
{
	switch (type) {
		case APIRoomReduction_Rest:		return "Rest";
		case APIRoomReduction_Wall:		return "Wall";
		case APIRoomReduction_Column:	return "Column";
		case APIRoomReduction_Hatch:	return "Fill";
		case APIRoomReduction_Gable:	return "Gable";
		default:						return "Other";
	}
}


void __ACENV_CALL CollectReduction (const API_RoomReductionPolyType* red)
{
	if (red == nullptr)
		return;
	try {
		if (gReductionSink != nullptr) {
			OS item;
			item.Add ("type", ReductionTypeName (red->type));
			item.Add ("percent", (Int32) red->percent);
			item.Add ("area", red->area);
			PolyView v;
			v.nCoords = red->nCoords;
			v.nSubPolys = red->nSubPolys;
			v.nArcs = red->nArcs;
			v.coords = red->coords;
			v.pends = red->subPolys;
			v.parcs = red->arcs;
			if (v.nCoords > 0 && IsValid (v))
				item.Add ("polygon", PolyViewToJson (v));
			gReductionSink->Push (item);
		}
	} catch (...) {
		// never let an exception cross the API callback boundary
	}
	// The callback owns the handles (see the DevKit Element_Test example).
	if (red->coords != nullptr)		BMKillHandle (reinterpret_cast<GSHandle*> (const_cast<API_Coord***> (&red->coords)));
	if (red->subPolys != nullptr)	BMKillHandle (reinterpret_cast<GSHandle*> (const_cast<Int32***> (&red->subPolys)));
	if (red->arcs != nullptr)		BMKillHandle (reinterpret_cast<GSHandle*> (const_cast<API_PolyArc***> (&red->arcs)));
}


OS ZoneReductionsJson (const API_Guid& guid)
{
	GS::Array<OS> items;
	gReductionSink = &items;
	API_Guid g = guid;
	const GSErrCode err = ACAPI_Database (APIDb_RoomReductionsID, &g, reinterpret_cast<void*> (reinterpret_cast<GS::IntPtr> (&CollectReduction)));
	gReductionSink = nullptr;
	if (err != NoError)
		return OS ("error", GS::UniString ("Cannot read zone reductions: " + ErrorName (err)));
	return OS ("items", items);
}


OS StampJson (Int32 libInd)
{
	API_LibPart lp;
	BNZeroMemory (&lp, sizeof (lp));
	lp.index = libInd;
	const GSErrCode err = ACAPI_LibPart_Get (&lp);
	if (lp.location != nullptr) {
		delete lp.location;
		lp.location = nullptr;
	}
	if (err != NoError)
		return OS ("index", libInd, "missing", true);
	return OS ("index", libInd, "name", GS::UniString (lp.docu_UName), "guid", GS::UniString (lp.ownUnID));
}

// --- Fields ------------------------------------------------------------------------------

// Applies zone settings shared by create and modify (mask == nullptr on create).
// guid == nullptr on create (stamp parameters then start from the tool defaults / library part defaults).
void ApplyZoneFields (API_Element& element, API_Element* mask, API_ElementMemo& memo, UInt64* memoMask,
					  const OS& spec, const API_Guid* guid)
{
#define ZONE_SET(field) if (mask) ACAPI_ELEMENT_MASK_SET (*mask, API_ZoneType, field)
	API_ZoneType& zone = element.zone;

	// identity
	if (auto s = OptText (spec, "name"))		{ SetUStr (zone.roomName, API_UniLongNameLen, *s, "name"); ZONE_SET (roomName); }
	if (auto s = OptText (spec, "number"))		{ SetUStr (zone.roomNoStr, kRoomNoLen, *s, "number"); ZONE_SET (roomNoStr); }
	if (auto c = OptAttr (API_ZoneCatID, spec, "category")) { zone.catInd = *c; ZONE_SET (catInd); }

	// vertical extent
	if (auto v = OptDouble (spec, "height")) {
		if (*v <= 0.0)
			Fail ("Zone 'height' must be positive (meters).");
		zone.roomHeight = *v; ZONE_SET (roomHeight);
		// An explicit height only takes effect on a zone whose top is not linked to a story.
		if (!spec.Contains ("topLinkedStory")) { zone.relativeTopStory = 0; ZONE_SET (relativeTopStory); }
	}
	if (auto v = OptInt (spec, "topLinkedStory")) {
		if (*v < 0)
			Fail ("'topLinkedStory' must be >= 0 (0 = not linked, 1 = story above, ...).");
		zone.relativeTopStory = (short) *v; ZONE_SET (relativeTopStory);
	}
	if (auto v = OptDouble (spec, "topOffset"))		{ zone.roomTopOffset = *v; ZONE_SET (roomTopOffset); }
	if (auto v = OptDouble (spec, "bottomOffset"))		{ zone.roomBaseLev = *v; ZONE_SET (roomBaseLev); }
	if (auto v = OptDouble (spec, "floorThickness")) {
		if (*v < 0.0)
			Fail ("'floorThickness' must be >= 0 (meters).");
		zone.roomFlThick = *v; ZONE_SET (roomFlThick);
	}
	if (auto v = OptDouble (spec, "areaReduction")) {
		if (*v < 0.0 || *v > 100.0)
			Fail ("'areaReduction' is a percentage 0..100.");
		zone.reducePercent = *v; ZONE_SET (reducePercent);
	}

	// automatic boundary method
	if (Has (spec, "boundary")) {
		zone.refLineFlag = ParseNamed (kBoundaryMethods, spec, "boundary") == 1;
		ZONE_SET (refLineFlag);
	}
	if (auto b = OptBool (spec, "showFoundPolygon"))	{ zone.show_found_poly = *b; ZONE_SET (show_found_poly); }

	// stamp placement / look
	if (auto c = OptCoord (spec, "stampPosition"))		{ zone.pos = *c; ZONE_SET (pos); }
	if (auto a = OptAngle (spec, "stampAngle"))		{ zone.stampAngle = *a; ZONE_SET (stampAngle); }
	if (auto b = OptBool (spec, "fixedStampAngle"))	{ zone.fixedAngle = *b; ZONE_SET (fixedAngle); }
	if (auto p = OptPen (spec, "stampPen"))			{ zone.pen = *p; ZONE_SET (pen); }
	if (auto b = OptBool (spec, "useStampPens"))		{ zone.useStampPens = *b; ZONE_SET (useStampPens); }

	// 3D surface
	if (auto m = OptAttr (API_MaterialID, spec, "surface"))	{ zone.material = *m; ZONE_SET (material); }
	if (auto b = OptBool (spec, "useSurfaceForAllFaces"))		{ zone.oneMat = *b; ZONE_SET (oneMat); }

	// floor plan fill & contour
	if (auto b = OptBool (spec, "showFill"))			{ zone.useFloorFill = *b; ZONE_SET (useFloorFill); }
	if (auto f = OptAttr (API_FilltypeID, spec, "fill")) {
		zone.floorFillInd = *f; ZONE_SET (floorFillInd);
		if (!spec.Contains ("showFill")) { zone.useFloorFill = true; ZONE_SET (useFloorFill); }
	}
	if (auto p = OptPen (spec, "fillPen"))				{ zone.floorFillPen = *p; ZONE_SET (floorFillPen); }
	if (auto p = OptPen (spec, "fillBackgroundPen", 0))	{ zone.floorFillBGPen = *p; ZONE_SET (floorFillBGPen); }
	if (auto b = OptBool (spec, "fillFromSurface"))	{ zone.use3DHatching = *b; ZONE_SET (use3DHatching); }
	if (auto b = OptBool (spec, "showContour"))		{ zone.useContourLine = *b; ZONE_SET (useContourLine); }
	if (auto p = OptPen (spec, "contourPen"))			{ zone.floorContLPen = *p; ZONE_SET (floorContLPen); }
	if (auto l = OptAttr (API_LinetypeID, spec, "contourLineType")) {
		zone.floorContLType = *l; ZONE_SET (floorContLType);
		if (!spec.Contains ("showContour")) { zone.useContourLine = true; ZONE_SET (useContourLine); }
	}
	if (auto c = OptCoord (spec, "fillOrigin")) {
		zone.locOrigo = *c; ZONE_SET (locOrigo);
		zone.useLocalOrigo = true; ZONE_SET (useLocalOrigo);
	}
	if (auto a = OptAngle (spec, "fillAngle"))			{ zone.fillAngle = *a; ZONE_SET (fillAngle); }
	if (auto b = OptBool (spec, "localFillOrientation"))	{ zone.useLocalOrigo = *b; ZONE_SET (useLocalOrigo); }

	// zone stamp library part + GDL parameters
	const bool hasStamp = spec.Contains ("stamp");
	const bool hasStampParams = spec.Contains ("stampParameters");
	if (hasStamp || hasStampParams) {
		Int32 libInd = zone.libInd;
		if (hasStamp)
			libInd = FindLibPart (spec, "stamp", APILib_RoomID).index;
		if (libInd <= 0)
			Fail ("This zone has no zone stamp library part. Pass 'stamp' (a Zone-type library part; find names with search_library_parts, they are localized).", APIERR_BADINDEX);

		OS values;
		if (hasStampParams && !TryGetObject (spec, "stampParameters", values))
			Fail ("'stampParameters' must be an object {gdlParameterName: value}.");

		API_AddParType** params = nullptr;
		if (libInd != zone.libInd) {
			// New stamp: start from the new library part's defaults (parameter script applied).
			params = ChangeParamsWithScriptForLibPart (libInd, API_ElemType (API_ZoneID), values);
		} else if (guid != nullptr) {
			// Same stamp of an existing zone: change its current values through the parameter script.
			params = ChangeParamsWithScript (*guid, element.header.type, libInd, values);
		} else {
			// New zone with the default stamp: keep the tool-default values and set the given ones.
			if (memo.params == nullptr)
				Fail ("The Zone tool defaults have no stamp parameters; pass 'stamp' explicitly.", APIERR_GENERAL);
			ApplyParamValues (memo.params, values);
		}
		if (params != nullptr) {
			if (memo.params != nullptr)
				ACAPI_DisposeAddParHdl (&memo.params);
			memo.params = params;
		}
		if (memoMask != nullptr)
			*memoMask |= APIMemoMask_AddPars;
		if (libInd != zone.libInd) { zone.libInd = libInd; ZONE_SET (libInd); }
	}
#undef ZONE_SET
}


GS::UniString NoBoundaryHint (const API_Coord& ref, short floorInd)
{
	GS::UniString story;
	story = GS::UniString::Printf ("%d", (int) floorInd);
	return "Archicad found no closed boundary around the reference point " + CoordText (ref) + " on story " + story +
		   ". Make sure walls (zoneRelation 'Boundary'), columns, curtain walls or room-separator lines fully enclose the point on that story, "
		   "or pass 'polygon' to create a manual zone.";
}


API_Guid CreateZone (const OS& spec)
{
	const bool hasPolygon = spec.Contains ("polygon");
	const bool hasRef = spec.Contains ("referencePoint");
	if (hasPolygon == hasRef)
		Fail ("A zone needs exactly one of 'polygon' (manual zone with the given outline) or 'referencePoint' "
			  "(automatic zone: Archicad detects the room boundary formed by walls / room separators around the point).");

	API_Element element = NewElement (API_ZoneID);
	Memo memo;
	GetDefaults (element, memo.Ptr ());
	ApplyCommonFields (element, nullptr, spec);
	ApplyZoneFields (element, nullptr, *memo, nullptr, spec, nullptr);

	API_ZoneType& zone = element.zone;
	if (hasPolygon) {
		if (spec.Contains ("boundary"))
			Fail ("'boundary' applies to automatic zones (referencePoint) only.");
		PolygonData data = GetPolygon (spec, "polygon");
		if (data.hasZ)
			Fail ("Zone polygon points must not have 'z' (use bottomOffset for the elevation).");
		WritePolygonToMemo (data, zone.poly, *memo);
		zone.manual = true;
		if (!spec.Contains ("stampPosition"))
			zone.pos = InteriorPoint (data);
		zone.refPos = zone.pos;
	} else {
		zone.manual = false;
		zone.refPos = GetCoord (spec, "referencePoint");
		if (!spec.Contains ("stampPosition"))
			zone.pos = zone.refPos;
	}

	// Archicad detects the boundary of an automatic zone from the walls of the CURRENT story, whatever
	// the zone's own story (verified live: a zone on story 1 created while story 0 was current got the
	// outline of story 0's exterior walls and ignored story 1's partitions).
	std::optional<CurrentStoryScope> storyScope;
	if (!hasPolygon)
		storyScope.emplace (element.header.floorInd);
	const GSErrCode err = ACAPI_Element_Create (&element, memo.Ptr ());
	if (err != NoError) {
		if (!hasPolygon)
			Fail ("Cannot create automatic zone (" + ErrorName (err) + "). " + NoBoundaryHint (zone.refPos, element.header.floorInd), err);
		Fail ("Cannot create zone: " + ErrorName (err) + ". Check that the polygon is simple (no self-intersections), holes lie inside the outline, and the story/layer is editable.", err);
	}
	return element.header.guid;
}


void SerializeZone (const API_Element& element, OS& out)
{
	const API_ZoneType& zone = element.zone;
	const API_Guid& guid = element.header.guid;

	out.Add ("name", GS::UniString (zone.roomName));
	out.Add ("number", GS::UniString (zone.roomNoStr));
	out.Add ("category", AttrRef (API_ZoneCatID, zone.catInd));
	out.Add ("automatic", !zone.manual);
	out.Add ("constructionMethod", zone.manual ? GS::UniString ("Manual") : BoundaryName (zone));
	out.Add ("boundary", BoundaryName (zone));
	if (!zone.manual)
		out.Add ("referencePoint", CoordObj (zone.refPos));
	out.Add ("showFoundPolygon", zone.show_found_poly);

	out.Add ("height", zone.roomHeight);
	out.Add ("topLinkedStory", (Int32) zone.relativeTopStory);
	out.Add ("topOffset", zone.roomTopOffset);
	out.Add ("bottomOffset", zone.roomBaseLev);
	out.Add ("bottomElevation", StoryLevel (element.header.floorInd) + zone.roomBaseLev);
	out.Add ("floorThickness", zone.roomFlThick);
	out.Add ("areaReduction", zone.reducePercent);

	out.Add ("stampPosition", CoordObj (zone.pos));
	AddAngle (out, "stampAngle", zone.stampAngle);
	out.Add ("fixedStampAngle", zone.fixedAngle);
	out.Add ("stampPen", (Int32) zone.pen);
	out.Add ("useStampPens", zone.useStampPens);
	if (zone.libInd > 0)
		out.Add ("stamp", StampJson (zone.libInd));
	if (zone.stampGuid != APINULLGuid)
		out.Add ("stampGuid", GuidStr (zone.stampGuid));

	out.Add ("surface", AttrRef (API_MaterialID, zone.material));
	out.Add ("useSurfaceForAllFaces", zone.oneMat);
	out.Add ("showFill", zone.useFloorFill);
	out.Add ("fill", AttrRef (API_FilltypeID, zone.floorFillInd));
	out.Add ("fillPen", (Int32) zone.floorFillPen);
	out.Add ("fillBackgroundPen", (Int32) zone.floorFillBGPen);
	out.Add ("fillFromSurface", zone.use3DHatching);
	out.Add ("showContour", zone.useContourLine);
	out.Add ("contourPen", (Int32) zone.floorContLPen);
	out.Add ("contourLineType", AttrRef (API_LinetypeID, zone.floorContLType));
	out.Add ("localFillOrientation", zone.useLocalOrigo);
	if (zone.useLocalOrigo) {
		out.Add ("fillOrigin", CoordObj (zone.locOrigo));
		AddAngle (out, "fillAngle", zone.fillAngle);
	}

	if (guid == APINULLGuid)
		return;		// tool defaults (get_tool_defaults): no geometry, quantities or relations

	Memo memo;
	if (ACAPI_Element_GetMemo (guid, memo.Ptr (), APIMemoMask_Polygon | APIMemoMask_AdditionalPolygon) == NoError) {
		out.Add ("polygon", PolyViewToJson (MainPoly (element, *memo)));
		AddReferenceLinePolygon (out, element, *memo);
	}

	API_ZoneAllQuantity q;
	BNZeroMemory (&q, sizeof (q));
	if (GetZoneQuantity (guid, q))
		out.Add ("quantities", QuantitiesToJson (q));

	out.Add ("relations", ZoneRelationsJson (guid));
}


void ModifyZone (API_Element& element, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask, const OS& patch)
{
	const bool hasPolygon = patch.Contains ("polygon");
	const bool hasRef = patch.Contains ("referencePoint");
	if (hasPolygon && hasRef)
		Fail ("Pass either 'polygon' (makes the zone manual) or 'referencePoint' (makes it automatic), not both.");

	const API_Guid guid = element.header.guid;
	ApplyZoneFields (element, &mask, memo, &memoMask, patch, &guid);

	API_ZoneType& zone = element.zone;
	if (hasPolygon) {
		PolygonData data = GetPolygon (patch, "polygon");
		if (data.hasZ)
			Fail ("Zone polygon points must not have 'z'.");
		WritePolygonToMemo (data, zone.poly, memo);
		ACAPI_ELEMENT_MASK_SET (mask, API_ZoneType, poly);
		memoMask |= APIMemoMask_Polygon;
		if (!zone.manual) {
			zone.manual = true;
			ACAPI_ELEMENT_MASK_SET (mask, API_ZoneType, manual);
		}
	}
	if (hasRef) {
		// Verified live: Archicad 26 ignores refPos changes of automatic zones and refuses (APIERR_GENERAL)
		// to turn a manual zone automatic in place. Relocation = re-creation (RelocateZones).
		Fail ("Archicad cannot move the reference point of an existing zone in place. Use modify_zones with "
			  "'referencePoint': it re-creates the zone as an automatic zone at the new point (keeps settings, stamp "
			  "parameters, Element ID, classifications and custom properties; the zone gets a NEW GUID).", APIERR_NOTSUPPORTED);
	}
	if (auto automatic = OptBool (patch, "automatic")) {
		if (*automatic && hasPolygon)
			Fail ("'automatic: true' contradicts 'polygon' (a polygon makes the zone manual).");
		if (!*automatic && hasRef)
			Fail ("'automatic: false' contradicts 'referencePoint' (a reference point makes the zone automatic).");
		if (*automatic && zone.manual)
			Fail ("To make a manual zone automatic pass 'referencePoint' (a point inside the room).");
		if (!*automatic && !zone.manual) {
			zone.manual = true;		// freeze the current polygon
			ACAPI_ELEMENT_MASK_SET (mask, API_ZoneType, manual);
		}
	}
}

// --- UpdateZones -------------------------------------------------------------------------

// Deletes the element on scope exit unless released (cleanup of temporary / half-created zones).
class TempElement {
public:
	TempElement () = default;
	~TempElement ()
	{
		if (guid != APINULLGuid) {
			GS::Array<API_Guid> list;
			list.Push (guid);
			ACAPI_Element_Delete (list);
		}
	}
	TempElement (const TempElement&) = delete;
	TempElement& operator= (const TempElement&) = delete;

	void	Release ()	{ guid = APINULLGuid; }

	API_Guid guid = APINULLGuid;
};


// Archicad refuses to create an automatic zone in a room that already contains one (APIERR_GENERAL,
// verified live). While a probe / replacement zone is created, the original is therefore "parked"
// on another story and moved back afterwards (no-op when the zone was deleted meanwhile).
class ParkedZone {
public:
	explicit ParkedZone (const API_Element& zone) : guid (zone.header.guid), home (zone.header.floorInd)
	{
		API_StoryInfo info;
		BNZeroMemory (&info, sizeof (info));
		if (ACAPI_Environment (APIEnv_GetStorySettingsID, &info, nullptr) != NoError || info.data == nullptr)
			return;
		std::optional<short> other;
		const Int32 n = info.lastStory - info.firstStory + 1;
		for (Int32 i = 0; i < n && !other.has_value (); ++i) {
			if ((*info.data)[i].index != home)
				other = (*info.data)[i].index;
		}
		BMKillHandle (reinterpret_cast<GSHandle*> (&info.data));
		if (other.has_value ())
			parked = MoveTo (*other);
	}
	~ParkedZone ()
	{
		if (parked)
			MoveTo (home);
	}
	ParkedZone (const ParkedZone&) = delete;
	ParkedZone& operator= (const ParkedZone&) = delete;

	bool IsParked () const { return parked; }

private:
	bool MoveTo (short story)
	{
		API_Element e;
		BNZeroMemory (&e, sizeof (e));
		e.header.guid = guid;
		if (ACAPI_Element_Get (&e) != NoError)
			return false;
		API_Element mask;
		ACAPI_ELEMENT_MASK_CLEAR (mask);
		e.header.floorInd = story;
		ACAPI_ELEMENT_MASK_SET (mask, API_Elem_Head, floorInd);
		return ACAPI_Element_Change (&e, &mask, nullptr, 0, true) == NoError;
	}

	API_Guid	guid;
	short		home;
	bool		parked = false;
};


// Copy of a zone's settings as a new automatic zone (header reset so it can be created).
API_Element CloneAsAutomatic (const API_Element& source)
{
	API_Element e = source;
	API_Elem_Head head;
	BNZeroMemory (&head, sizeof (head));
	head.type = source.header.type;
	head.floorInd = source.header.floorInd;
	head.layer = source.header.layer;
	head.renovationStatus = source.header.renovationStatus;
	head.drwIndex = source.header.drwIndex;
	e.header = head;
	e.zone.manual = false;
	BNZeroMemory (&e.zone.poly, sizeof (e.zone.poly));
	BNZeroMemory (&e.zone.refPoly, sizeof (e.zone.refPoly));
	e.zone.stampGuid = APINULLGuid;
	return e;
}


// Creates an automatic zone with the settings (and stamp parameters) of `source` at its reference point.
API_Guid CreateAutomaticClone (const API_Element& source)
{
	API_Element e = CloneAsAutomatic (source);
	Memo params;
	LoadMemo (source.header.guid, *params, APIMemoMask_AddPars);
	CurrentStoryScope storyScope (e.header.floorInd);		// boundary detection uses the current story (see CreateZone)
	const GSErrCode err = ACAPI_Element_Create (&e, params.Ptr ());
	if (err != NoError)
		Fail ("Cannot re-detect the boundary (" + ErrorName (err) + "): " + NoBoundaryHint (source.zone.refPos, source.header.floorInd) +
			  " The zone was left unchanged. If walls moved over the reference point, move it into the room with "
			  "modify_zones {guid, referencePoint}, or freeze the zone with modify_zones {guid, automatic: false}.", err);
	return e.header.guid;
}


struct UpdateOptions {
	bool			dryRun = false;
	UpdateMethod	method = CopyBoundary;
};


void AddAreas (OS& out, const char* key, const API_Guid& guid)
{
	API_ZoneAllQuantity q;
	BNZeroMemory (&q, sizeof (q));
	if (GetZoneQuantity (guid, q))
		out.Add (key, OS ("area", q.area, "netArea", q.netarea, "calculatedArea", q.calcArea));
}


OS RecreateZone (const API_Element& zoneElem, GS::UniString& status)
{
	const API_Guid oldGuid = zoneElem.header.guid;
	ParkedZone park (zoneElem);		// frees the room for the replacement; the original is deleted below
	TempElement created;
	created.guid = CreateAutomaticClone (zoneElem);
	const API_Guid newGuid = created.guid;

	GS::Array<GS::UniString> warnings;

	const GS::UniString elemId = GetElementInfoString (oldGuid);
	if (!elemId.IsEmpty ()) {
		OS r = Try ([&] () -> OS { SetElementInfoString (newGuid, elemId); return OS (); });
		if (r.Contains ("error"))
			warnings.Push ("Element ID was not copied.");
	}

	GS::Array<GS::Pair<API_Guid, API_Guid>> classItems;
	if (ACAPI_Element_GetClassificationItems (oldGuid, classItems) == NoError) {
		for (const auto& item : classItems) {
			if (ACAPI_Element_AddClassificationItem (newGuid, item.second) != NoError)
				warnings.Push ("A classification item (" + GuidStr (item.second) + ") could not be copied.");
		}
	}

	GS::Array<API_PropertyDefinition> defs;
	if (ACAPI_Element_GetPropertyDefinitions (oldGuid, API_PropertyDefinitionFilter_UserDefined, defs) == NoError && !defs.IsEmpty ()) {
		GS::Array<API_Property> props;
		if (ACAPI_Element_GetPropertyValues (oldGuid, defs, props) == NoError) {
			GS::Array<API_Property> custom;
			for (const API_Property& p : props) {
				if (!p.isDefault && p.status == API_Property_HasValue && p.definition.canValueBeEditable)
					custom.Push (p);
			}
			if (!custom.IsEmpty () && ACAPI_Element_SetProperties (newGuid, custom) != NoError)
				warnings.Push ("Some custom property values could not be copied.");
		}
	}

	if (zoneElem.header.groupGuid != APINULLGuid)
		warnings.Push ("The old zone was grouped; the new zone is not part of the group.");

	GS::Array<API_Guid> toDelete;
	toDelete.Push (oldGuid);
	const GSErrCode delErr = ACAPI_Element_Delete (toDelete);
	if (delErr != NoError)
		Fail ("Cannot delete the outdated zone (" + ErrorName (delErr) + "); nothing was changed. Is it locked, reserved by another Teamwork user, or on a locked layer?", delErr);
	created.Release ();

	status = "recreated";
	OS out;
	out.Add ("guid", GuidStr (oldGuid));
	out.Add ("status", status);
	out.Add ("newGuid", GuidStr (newGuid));
	AddAreas (out, "after", newGuid);
	if (!warnings.IsEmpty ())
		out.Add ("warnings", warnings);
	return out;
}


OS UpdateOneZone (const API_Guid& guid, const UpdateOptions& opt, GS::UniString& status)
{
	const API_Element zoneElem = GetElement (guid);
	if (zoneElem.header.type.typeID != API_ZoneID)
		Fail ("Element " + GuidStr (guid) + " is a " + ElemTypeName (zoneElem.header.type) + ", not a Zone.", APIERR_BADELEMENTTYPE);

	OS out;
	out.Add ("guid", GuidStr (guid));
	out.Add ("name", GS::UniString (zoneElem.zone.roomName));
	out.Add ("number", GS::UniString (zoneElem.zone.roomNoStr));

	if (zoneElem.zone.manual) {
		status = "skipped";
		out.Add ("status", status);
		out.Add ("reason", GS::UniString ("Manual zone: its polygon is fixed. Change it with modify_zones (polygon), or make it automatic with modify_zones (referencePoint)."));
		return out;
	}
	if (zoneElem.header.hotlinkGuid != APINULLGuid) {
		status = "skipped";
		out.Add ("status", status);
		out.Add ("reason", GS::UniString ("The zone belongs to a hotlinked module; update it in the source file."));
		return out;
	}

	OS before;
	AddAreas (before, "before", guid);

	if (opt.method == Recreate && !opt.dryRun) {
		OS r = RecreateZone (zoneElem, status);
		OS b;
		if (before.Contains ("before") && TryGetObject (before, "before", b))
			r.Add ("before", b);
		r.Add ("name", GS::UniString (zoneElem.zone.roomName));
		r.Add ("number", GS::UniString (zoneElem.zone.roomNoStr));
		return r;
	}

	Memo current;
	LoadMemo (guid, *current, APIMemoMask_Polygon | APIMemoMask_AdditionalPolygon);

	// Probe: a temporary automatic zone at the same reference point with the same settings.
	// The zone itself is parked on another story meanwhile (see ParkedZone); the probe is deleted
	// before the zone returns (destruction order: probe first, then park).
	API_Element probeElem;
	Memo probeMemo;
	{
		ParkedZone park (zoneElem);
		TempElement probe;
		probe.guid = CreateAutomaticClone (zoneElem);
		probeElem = GetElement (probe.guid);
		LoadMemo (probe.guid, *probeMemo, APIMemoMask_Polygon | APIMemoMask_AdditionalPolygon);
	}

	const bool refLine = zoneElem.zone.refLineFlag;
	const bool same = SamePolygon (MainPoly (zoneElem, *current), MainPoly (probeElem, *probeMemo)) &&
					  (!refLine || SamePolygon (FoundPoly (zoneElem, *current), FoundPoly (probeElem, *probeMemo)));

	OS b;
	if (TryGetObject (before, "before", b))
		out.Add ("before", b);

	if (same) {
		status = "upToDate";
		out.Add ("status", status);
		return out;
	}
	if (opt.dryRun) {
		status = "outdated";
		out.Add ("status", status);
		out.Add ("detectedPolygon", PolyViewToJson (MainPoly (probeElem, *probeMemo)));
		return out;
	}

	API_Element changed = zoneElem;
	API_Element mask;
	ACAPI_ELEMENT_MASK_CLEAR (mask);
	changed.zone.poly = probeElem.zone.poly;
	ACAPI_ELEMENT_MASK_SET (mask, API_ZoneType, poly);
	UInt64 memoMask = APIMemoMask_Polygon;
	if (refLine && probeMemo->additionalPolyCoords != nullptr) {
		changed.zone.refPoly = probeElem.zone.refPoly;
		ACAPI_ELEMENT_MASK_SET (mask, API_ZoneType, refPoly);
		memoMask |= APIMemoMask_AdditionalPolygon;
	}
	const GSErrCode err = ACAPI_Element_Change (&changed, &mask, probeMemo.Ptr (), memoMask, true);
	if (err != NoError)
		Fail ("Cannot write the re-detected boundary into zone " + GuidStr (guid) + " (" + ErrorName (err) +
			  "). Retry with method 'recreate', or use Design > Update Zones in Archicad.", err);

	// Verify
	bool verified = false;
	{
		API_Element after;
		BNZeroMemory (&after, sizeof (after));
		after.header.guid = guid;
		Memo afterMemo;
		if (ACAPI_Element_Get (&after) == NoError &&
			ACAPI_Element_GetMemo (guid, afterMemo.Ptr (), APIMemoMask_Polygon) == NoError)
			verified = SamePolygon (MainPoly (after, *afterMemo), MainPoly (probeElem, *probeMemo));
	}
	status = "updated";
	out.Add ("status", status);
	out.Add ("verified", verified);
	AddAreas (out, "after", guid);
	if (!verified)
		out.Add ("warning", GS::UniString ("Archicad accepted the change but the stored polygon differs from the detected one; check the zone, retry with method 'recreate', or use Design > Update Zones."));
	return out;
}

// --- GetZones ----------------------------------------------------------------------------

bool NaturalLess (const GS::UniString& a, const GS::UniString& b)
{
	const GS::String sa = ToStr (a), sb = ToStr (b);
	char* endA = nullptr;
	char* endB = nullptr;
	const double na = std::strtod (sa.ToCStr (), &endA);
	const double nb = std::strtod (sb.ToCStr (), &endB);
	const bool numA = endA != sa.ToCStr () && !sa.IsEmpty ();
	const bool numB = endB != sb.ToCStr () && !sb.IsEmpty ();
	if (numA && numB && na != nb)
		return na < nb;
	if (numA != numB)
		return numA;		// numbered zones first
	return a.Compare (b, GS::UniString::CaseInsensitive) == GS::UniString::Less;
}


struct ZoneEntry {
	short			story = 0;
	GS::UniString	number;
	GS::UniString	name;
	OS				data;
};


struct StoryCache {
	std::map<short, double>			levels;
	std::map<short, GS::UniString>	names;

	double Level (short s)
	{
		auto it = levels.find (s);
		if (it != levels.end ())
			return it->second;
		return levels[s] = StoryLevel (s);
	}
	GS::UniString Name (short s)
	{
		auto it = names.find (s);
		if (it != names.end ())
			return it->second;
		return names[s] = StoryName (s);
	}
};


struct ZoneListOptions {
	bool polygon = false;
	bool relations = false;
	bool reductions = false;
	bool quantities = false;
};


OS ZoneSummary (const API_Element& element, const API_ZoneAllQuantity* q, const ZoneListOptions& opt, StoryCache& stories)
{
	const API_ZoneType& zone = element.zone;
	const API_Guid& guid = element.header.guid;
	OS out;
	out.Add ("guid", GuidStr (guid));
	out.Add ("name", GS::UniString (zone.roomName));
	out.Add ("number", GS::UniString (zone.roomNoStr));
	out.Add ("category", AttrRef (API_ZoneCatID, zone.catInd));
	out.Add ("storyIndex", (Int32) element.header.floorInd);
	out.Add ("storyName", stories.Name (element.header.floorInd));
	out.Add ("layer", AttrRef (API_LayerID, element.header.layer));
	const GS::UniString elemId = GetElementInfoString (guid);
	if (!elemId.IsEmpty ())
		out.Add ("elementId", elemId);
	out.Add ("constructionMethod", zone.manual ? GS::UniString ("Manual") : BoundaryName (zone));
	if (!zone.manual)
		out.Add ("referencePoint", CoordObj (zone.refPos));
	if (q != nullptr) {
		out.Add ("area", q->area);
		out.Add ("netArea", q->netarea);
		out.Add ("calculatedArea", q->calcArea);
		out.Add ("perimeter", q->perimeter);
		out.Add ("volume", q->volume);
		if (opt.quantities)
			out.Add ("quantities", QuantitiesToJson (*q));
	}
	out.Add ("height", zone.roomHeight);
	out.Add ("topLinkedStory", (Int32) zone.relativeTopStory);
	out.Add ("bottomOffset", zone.roomBaseLev);
	out.Add ("bottomElevation", stories.Level (element.header.floorInd) + zone.roomBaseLev);
	out.Add ("areaReduction", zone.reducePercent);
	out.Add ("stampPosition", CoordObj (zone.pos));

	if (opt.polygon) {
		Memo memo;
		if (ACAPI_Element_GetMemo (guid, memo.Ptr (), APIMemoMask_Polygon | APIMemoMask_AdditionalPolygon) == NoError) {
			out.Add ("polygon", PolyViewToJson (MainPoly (element, *memo)));
			AddReferenceLinePolygon (out, element, *memo);
		}
	}
	if (opt.relations)
		out.Add ("relations", ZoneRelationsJson (guid));
	if (opt.reductions)
		out.Add ("reductions", ZoneReductionsJson (guid));
	return out;
}


GS::Array<Int32> StoryFilter (const OS& params)
{
	GS::Array<Int32> result;
	for (double v : GetNumberArray (params, "stories", false))
		result.Push ((Int32) std::lround (v));
	return result;
}


bool StoryMatches (const GS::Array<Int32>& filter, short floorInd)
{
	if (filter.IsEmpty ())
		return true;
	for (Int32 s : filter)
		if (s == floorInd)
			return true;
	return false;
}

} // namespace


void RegisterZoneCommands ()
{
	RegisterAdapter ({ API_ZoneID, CreateZone, SerializeZone, ModifyZone });

	RegisterCommand ("GetZones",
		"Zone schedule. Input: {zones?: [guid], stories?: [storyIndex], category?: attrRef, search?: text (in name or number), "
		"includePolygon?, includeRelations?, includeReductions?, includeQuantities?, offset?, limit? (default 500)}. "
		"Output: {zones: [{guid, name, number, category, storyIndex, storyName, constructionMethod, area, netArea, calculatedArea, "
		"perimeter, volume, height, bottomElevation, stampPosition, ...}], total, offset, hasMore, totals: {count, area, netArea, calculatedArea, volume}} "
		"sorted by story, then number. Areas in m2, volumes in m3.",
		[] (const OS& params) -> OS {
			GS::Array<API_Guid> guids = GetGuidArray (params, "zones", false);
			const bool explicitList = !guids.IsEmpty ();
			if (!explicitList)
				guids = ListElements (API_ZoneID);

			const GS::Array<Int32> stories = StoryFilter (params);
			const std::optional<API_AttributeIndex> category = OptAttr (API_ZoneCatID, params, "category");
			const GS::UniString search = GetString (params, "search", GS::UniString ());
			ZoneListOptions opt;
			opt.polygon = GetBool (params, "includePolygon", false);
			opt.relations = GetBool (params, "includeRelations", false);
			opt.reductions = GetBool (params, "includeReductions", false);
			opt.quantities = GetBool (params, "includeQuantities", false);
			const Int32 offset = std::max<Int32> (0, GetInt (params, "offset", 0));
			const Int32 limit = std::max<Int32> (1, GetInt (params, "limit", 500));

			StoryCache storyCache;
			std::vector<ZoneEntry> entries;
			GS::Array<OS> errors;
			double totalArea = 0.0, totalNet = 0.0, totalCalc = 0.0, totalVolume = 0.0;

			for (const API_Guid& guid : guids) {
				API_Element element;
				BNZeroMemory (&element, sizeof (element));
				element.header.guid = guid;
				const GSErrCode err = ACAPI_Element_Get (&element);
				if (err != NoError) {
					if (explicitList)
						errors.Push (OS ("guid", GuidStr (guid), "error", GS::UniString ("Element not found: " + ErrorName (err))));
					continue;
				}
				if (element.header.type.typeID != API_ZoneID) {
					if (explicitList)
						errors.Push (OS ("guid", GuidStr (guid), "error", GS::UniString ("Not a zone (" + ElemTypeName (element.header.type) + ").")));
					continue;
				}
				if (!StoryMatches (stories, element.header.floorInd))
					continue;
				if (category.has_value () && element.zone.catInd != *category)
					continue;
				const GS::UniString name (element.zone.roomName);
				const GS::UniString number (element.zone.roomNoStr);
				if (!search.IsEmpty () &&
					!ContainsIgnoreCase (name, search) &&
					!ContainsIgnoreCase (number, search))
					continue;

				API_ZoneAllQuantity q;
				BNZeroMemory (&q, sizeof (q));
				const bool hasQ = GetZoneQuantity (guid, q);
				if (hasQ) {
					totalArea += q.area;
					totalNet += q.netarea;
					totalCalc += q.calcArea;
					totalVolume += q.volume;
				}
				ZoneEntry entry;
				entry.story = element.header.floorInd;
				entry.number = number;
				entry.name = name;
				entry.data = ZoneSummary (element, hasQ ? &q : nullptr, opt, storyCache);
				entries.push_back (entry);
			}

			std::stable_sort (entries.begin (), entries.end (), [] (const ZoneEntry& a, const ZoneEntry& b) {
				if (a.story != b.story)
					return a.story < b.story;
				if (a.number != b.number)
					return NaturalLess (a.number, b.number);
				return NaturalLess (a.name, b.name);
			});

			GS::Array<OS> page;
			const Int32 total = (Int32) entries.size ();
			for (Int32 i = offset; i < total && (Int32) page.GetSize () < limit; ++i)
				page.Push (entries[i].data);

			OS totals;
			totals.Add ("count", total);
			totals.Add ("area", totalArea);
			totals.Add ("netArea", totalNet);
			totals.Add ("calculatedArea", totalCalc);
			totals.Add ("volume", totalVolume);

			OS out;
			out.Add ("zones", page);
			out.Add ("total", total);
			out.Add ("offset", offset);
			out.Add ("hasMore", offset + (Int32) page.GetSize () < total);
			out.Add ("totals", totals);
			if (!errors.IsEmpty ())
				out.Add ("errors", errors);
			return out;
		});

	RegisterCommand ("RelocateZones",
		"Moves zones to a new reference point by re-creating them as AUTOMATIC zones there (Archicad cannot move the "
		"reference point in place). Keeps settings, stamp parameters, Element ID, classifications and custom property values; "
		"the stamp moves with the reference point. Input: {zones: [{guid, referencePoint: {x, y}}]}. "
		"Output: {results: [{guid (old), newGuid, status: 'relocated', warnings?} | {guid, error}]}.",
		[] (const OS& params) -> OS {
			GS::Array<OS> items = GetObjectArray (params, "zones");
			GS::Array<OS> results;
			Undoable ("Relocate zones (Claude)", [&] () {
				for (const OS& item : items) {
					API_Guid guid = APINULLGuid;
					OS r = Try ([&] () -> OS {
						guid = GetGuid (item, "guid");
						const API_Coord ref = GetCoord (item, "referencePoint");
						ModelDatabaseScope planScope;
						API_Element zoneElem = GetElement (guid);
						if (zoneElem.header.type.typeID != API_ZoneID)
							Fail ("Element " + GuidStr (guid) + " is not a zone.", APIERR_BADELEMENTTYPE);
						if (zoneElem.header.hotlinkGuid != APINULLGuid)
							Fail ("Zone " + GuidStr (guid) + " belongs to a hotlinked module and cannot be relocated.", APIERR_NOTEDITABLE);
						const double dx = ref.x - zoneElem.zone.refPos.x;
						const double dy = ref.y - zoneElem.zone.refPos.y;
						zoneElem.zone.refPos = ref;
						zoneElem.zone.pos.x += dx;
						zoneElem.zone.pos.y += dy;
						GS::UniString status;
						OS out = RecreateZone (zoneElem, status);
						out.Add ("relocated", true);
						return out;
					});
					if (r.Contains ("error") && !r.Contains ("guid") && guid != APINULLGuid) {
						OS withGuid ("guid", GuidStr (guid));
						OS error;
						if (TryGetObject (r, "error", error))
							withGuid.Add ("error", error);
						r = withGuid;
					}
					results.Push (r);
				}
			});
			return OS ("results", results);
		});

	RegisterCommand ("UpdateZones",
		"Re-detects the boundaries of AUTOMATIC zones after walls/columns/room separators changed (the Archicad 26 API has no "
		"'Design > Update Zones' call). Input: {zones?: [guid] (default: all automatic zones), stories?: [storyIndex], "
		"dryRun?: bool (only report outdated zones), method?: 'copyBoundary' (default: keeps the GUID, writes the re-detected polygon "
		"into the zone) | 'recreate' (new zone with the same settings/stamp parameters/element ID/classifications/custom properties, "
		"old one deleted: new GUID)}. Output: {results: [{guid, status: updated|upToDate|outdated|recreated|skipped, before, after, "
		"verified?, newGuid?} | {error}], summary}.",
		[] (const OS& params) -> OS {
			UpdateOptions opt;
			opt.dryRun = GetBool (params, "dryRun", false);
			if (auto m = OptNamed (kUpdateMethods, params, "method"))
				opt.method = (UpdateMethod) *m;

			GS::Array<API_Guid> guids = GetGuidArray (params, "zones", false);
			const bool explicitList = !guids.IsEmpty ();
			const GS::Array<Int32> stories = StoryFilter (params);
			if (!explicitList)
				guids = ListElements (API_ZoneID);

			GS::Array<OS> results;
			Int32 nUpdated = 0, nUpToDate = 0, nOutdated = 0, nRecreated = 0, nSkipped = 0, nFailed = 0, nManual = 0;

			auto run = [&] () {
				for (const API_Guid& guid : guids) {
					if (!explicitList || !stories.IsEmpty ()) {
						API_Element element;
						BNZeroMemory (&element, sizeof (element));
						element.header.guid = guid;
						if (ACAPI_Element_Get (&element) == NoError) {
							if (!StoryMatches (stories, element.header.floorInd))
								continue;
							if (!explicitList && (element.zone.manual || element.header.hotlinkGuid != APINULLGuid)) {
								++nManual;
								continue;
							}
						}
					}
					GS::UniString status;
					OS r = Try ([&] () -> OS { return UpdateOneZone (guid, opt, status); });
					if (r.Contains ("error")) {
						++nFailed;
						OS withGuid ("guid", GuidStr (guid));
						OS error;
						if (TryGetObject (r, "error", error))
							withGuid.Add ("error", error);
						r = withGuid;
					} else if (status == "updated")		++nUpdated;
					else if (status == "upToDate")		++nUpToDate;
					else if (status == "outdated")		++nOutdated;
					else if (status == "recreated")		++nRecreated;
					else								++nSkipped;
					results.Push (r);
				}
			};

			if (!guids.IsEmpty ())
				Undoable (opt.dryRun ? "Check zones (Claude)" : "Update zones (Claude)", run);

			OS summary;
			summary.Add ("processed", (Int32) results.GetSize ());
			summary.Add ("updated", nUpdated);
			summary.Add ("upToDate", nUpToDate);
			summary.Add ("outdated", nOutdated);
			summary.Add ("recreated", nRecreated);
			summary.Add ("skipped", nSkipped);
			summary.Add ("failed", nFailed);
			if (!explicitList)
				summary.Add ("ignoredManualOrHotlinked", nManual);

			OS out;
			out.Add ("results", results);
			out.Add ("summary", summary);
			out.Add ("dryRun", opt.dryRun);
			out.Add ("method", NameOf (kUpdateMethods, opt.method));
			if (results.IsEmpty ())
				out.Add ("note", GS::UniString (explicitList ? "Nothing to update." : "No automatic zones found (manual zones keep their polygon; change them with modify_zones)."));
			return out;
		});
}

} // namespace cc
