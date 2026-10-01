// *****************************************************************************
// ViewsMarkers — element adapters for viewpoint markers:
//   CutPlane (section), Elevation, InteriorElevation, Detail, Worksheet.
// They power create_elements / get_element_details / modify_elements and the typed
// create_sections / create_elevations / create_interior_elevations /
// create_details / create_worksheets tools.
//
// CutPlane / Elevation fields (meters, degrees):
//   begin {x,y}*, end {x,y}*        cut / elevation line (* required on create)
//   viewSide  "left" (default) | "right"   side of begin->end the view looks at
//   depth     distance of the depth limit line from the cut line (default 10 m when limited)
//   horizontalRange "Infinite" | "Limited" | "ZeroDepth" (default Limited when depth is given)
//   verticalRange   "Infinite" | {min, max} (absolute elevations, m)
//   name, referenceId               viewpoint name and reference ID shown in the marker
// InteriorElevation: points [{x,y}...] (>= 2), closed?, depth?, offset?, name, referenceId, verticalRange
// Detail / Worksheet: polygon | box {xMin,yMin,xMax,yMax}, markerPosition?, markerAngle?, horizontalMarker?,
//                     name, referenceId
// + common: layer, storyIndex, renovationStatus, elementId
// *****************************************************************************

#include "Commands/ViewsCommon.hpp"
#include "Core/Command.hpp"
#include "Core/Elements.hpp"
#include "Core/Enums.hpp"
#include "Core/Polygon.hpp"
#include "Core/Types.hpp"

#include <cmath>

namespace cc {
namespace views {

namespace {

const NamedValue kHorizRange[] = {
	{ "Infinite",		APIHorRange_Infinite },
	{ "Limited",		APIHorRange_Limited },
	{ "ZeroDepth",		APIHorRange_ZeroDepth },
};

const NamedValue kVertRange[] = {
	{ "Infinite",		APIVerRange_Infinite },
	{ "Limited",		APIVerRange_Limited },
	{ "FitToZoneRange",	APIVerRange_FitToZoneRange },
};

const NamedValue kShowOn[] = {
	{ "AllStories",		APICutPl_ShowAll },
	{ "OneStory",		APICutPl_ShowOnOneStory },
	{ "InRange",		APICutPl_ShowInRange },
	{ "PartRange",		APICutPl_ShowPartRange },
};

const NamedValue kPlanConn[] = {
	{ "Online",			APICutPl_Online },
	{ "Offline",		APICutPl_Offline },
	{ "Drawing",		APICutPl_Drawing },
};

constexpr double kDefaultLimitedDepth = 10.0;

// --- RAII for sub-element memos ------------------------------------------------------------

class SubElementMemoGuard {
public:
	explicit SubElementMemoGuard (API_SubElement& sub) : sub (sub) {}
	~SubElementMemoGuard () { ACAPI_DisposeElemMemoHdls (&sub.memo); }
	SubElementMemoGuard (const SubElementMemoGuard&) = delete;
	SubElementMemoGuard& operator= (const SubElementMemoGuard&) = delete;
private:
	API_SubElement& sub;
};


// Loads the parameters of the marker's library part into marker.memo.params (as the DevKit examples do
// before ACAPI_Element_CreateExt). Keeps the defaults when the library part cannot be found.
void AttachMarkerParams (API_SubElement& marker, const API_ElemType& ownerType)
{
	Int32 libInd = marker.subElem.object.libInd;
	if (libInd <= 0) {
		API_LibPart libPart;
		BNZeroMemory (&libPart, sizeof (libPart));
		if (ACAPI_Goodies_GetMarkerParent (ownerType, libPart) == NoError && ACAPI_LibPart_Search (&libPart, false, true) == NoError)
			libInd = libPart.index;
		delete libPart.location;
	}
	if (libInd <= 0)
		return;
	double a = 0.0, b = 0.0;
	Int32 addParNum = 0;
	API_AddParType** addPars = nullptr;
	if (ACAPI_LibPart_GetParams (libInd, &a, &b, &addParNum, &addPars) != NoError || addPars == nullptr)
		return;
	if (marker.memo.params != nullptr)
		ACAPI_DisposeAddParHdl (&marker.memo.params);
	marker.memo.params = addPars;
	marker.subElem.object.libInd = libInd;
}


API_SubElement NewMainMarker (bool noParams)
{
	API_SubElement marker;
	BNZeroMemory (&marker, sizeof (marker));
	marker.subType = noParams ? (API_SubElementType) (APISubElement_MainMarker | APISubElement_NoParams) : APISubElement_MainMarker;
	return marker;
}


// Section markers live on the floor plan; details/worksheets in any 2D model view.
void EnsureMarkerDatabase (CurrentDatabaseSwitch& dbSwitch, bool floorPlanOnly)
{
	API_DatabaseInfo current;
	BNZeroMemory (&current, sizeof (current));
	const bool ok = ACAPI_Database (APIDb_GetCurrentDatabaseID, &current, nullptr) == NoError;
	if (ok && current.typeID == APIWind_FloorPlanID)
		return;
	if (!floorPlanOnly && ok) {
		switch (current.typeID) {
			case APIWind_SectionID:
			case APIWind_ElevationID:
			case APIWind_InteriorElevationID:
			case APIWind_DetailID:
			case APIWind_WorksheetID:
				return;
			default:
				break;
		}
	}
	dbSwitch.EnsureType (APIWind_FloorPlanID);
}


template <class T>
T* AllocPtr (USize count)
{
	T* p = reinterpret_cast<T*> (BMAllocatePtr ((GSSize) (count * sizeof (T)), ALLOCATE_CLEAR, 0));
	if (p == nullptr)
		Fail ("Out of memory.", APIERR_MEMFULL);
	return p;
}


template <class T>
USize PtrCount (const T* p)
{
	return p == nullptr ? 0 : (USize) (BMGetPtrSize (reinterpret_cast<GSConstPtr> (p)) / sizeof (T));
}

// --- Cut planes / elevations ----------------------------------------------------------------

API_CutPlaneType& CutPlaneOf (API_Element& e)				{ return e.header.type.typeID == API_ElevationID ? e.elevation : e.cutPlane; }
const API_CutPlaneType& CutPlaneOf (const API_Element& e)	{ return e.header.type.typeID == API_ElevationID ? e.elevation : e.cutPlane; }


void ApplyRanges (API_SectionSegment& s, API_Element* mask, const OS& spec, bool isInteriorElevation)
{
#define SEG_SET(field) if (mask) { if (isInteriorElevation) ACAPI_ELEMENT_MASK_SET (*mask, API_InteriorElevationType, segment.field); else ACAPI_ELEMENT_MASK_SET (*mask, API_CutPlaneType, segment.field); }
	if (auto name = OptString (spec, "name"))			{ SetUStr (s.cutPlName, *name); SEG_SET (cutPlName); }
	if (auto ref = OptString (spec, "referenceId"))		{ SetUStr (s.cutPlIdStr, *ref); SEG_SET (cutPlIdStr); }
	if (spec.Contains ("horizontalRange")) {
		s.horizRange = (API_SegmentHorizontalRange) ParseNamed (kHorizRange, spec, "horizontalRange");
		SEG_SET (horizRange);
	} else if (spec.Contains ("depth") && !isInteriorElevation) {
		s.horizRange = APIHorRange_Limited;
		SEG_SET (horizRange);
	}
	if (spec.Contains ("verticalRange")) {
		if (spec.IsString ("verticalRange")) {
			s.vertRange = (API_SegmentVerticalRange) ParseNamed (kVertRange, spec, "verticalRange");
			SEG_SET (vertRange);
		} else {
			OS vr = GetObject (spec, "verticalRange");
			const double vmin = GetDouble (vr, "min");
			const double vmax = GetDouble (vr, "max");
			if (vmax <= vmin)
				Fail ("verticalRange.max must be greater than verticalRange.min (absolute elevations in m).");
			s.vertRange = APIVerRange_Limited;
			s.vertMin = vmin;
			s.vertMax = vmax;
			s.relativeToStory = false;
			SEG_SET (vertRange); SEG_SET (vertMin); SEG_SET (vertMax); SEG_SET (relativeToStory);
		}
	}
#undef SEG_SET
}


Int32 ParseViewSide (const OS& spec, Int32 def)
{
	auto side = OptString (spec, "viewSide");
	if (!side.has_value ())
		return def;
	if (EqualsIgnoreCase (*side, "left"))	return 1;
	if (EqualsIgnoreCase (*side, "right"))	return -1;
	Fail ("viewSide must be 'left' or 'right' (seen when walking from begin to end).");
}


// Writes a straight cut line (2 main coordinates) and the depth line into the memo.
// Returns true when the distant area coordinates were written too.
bool WriteCutPlaneGeometry (API_CutPlaneType& cp, API_ElementMemo& memo, const API_Coord& begin, const API_Coord& end, Int32 side, double depth)
{
	const double dx = end.x - begin.x, dy = end.y - begin.y;
	const double len = std::hypot (dx, dy);
	if (len < 0.01)
		Fail ("begin and end must be at least 1 cm apart.");
	if (depth <= 0.0)
		Fail ("depth must be > 0 m.");
	const double nx = -dy / len * side, ny = dx / len * side;

	BMKillPtr (reinterpret_cast<GSPtr*> (&memo.sectionSegmentMainCoords));
	BMKillPtr (reinterpret_cast<GSPtr*> (&memo.sectionSegmentDepthCoords));
	memo.sectionSegmentMainCoords = AllocPtr<API_Coord> (2);
	memo.sectionSegmentMainCoords[0] = begin;
	memo.sectionSegmentMainCoords[1] = end;
	cp.segment.nMainCoord = 2;

	// The depth (and distant area) limit is a line parallel to the cut line, one vertex per main vertex
	// (the DevKit's Do_CreateCutPlane also passes 2 depth coordinates for a 2-point cut line).
	memo.sectionSegmentDepthCoords = AllocPtr<API_Coord> (2);
	memo.sectionSegmentDepthCoords[0].x = begin.x + nx * depth;
	memo.sectionSegmentDepthCoords[0].y = begin.y + ny * depth;
	memo.sectionSegmentDepthCoords[1].x = end.x + nx * depth;
	memo.sectionSegmentDepthCoords[1].y = end.y + ny * depth;
	cp.segment.nDepthCoord = 2;

	if (cp.segment.markedDistArea) {
		BMKillPtr (reinterpret_cast<GSPtr*> (&memo.sectionSegmentDistCoords));
		memo.sectionSegmentDistCoords = AllocPtr<API_Coord> (2);
		memo.sectionSegmentDistCoords[0].x = begin.x + nx * depth / 2.0;
		memo.sectionSegmentDistCoords[0].y = begin.y + ny * depth / 2.0;
		memo.sectionSegmentDistCoords[1].x = end.x + nx * depth / 2.0;
		memo.sectionSegmentDistCoords[1].y = end.y + ny * depth / 2.0;
		cp.segment.nDistCoord = 2;
		return true;
	}
	return false;
}


double DefaultDepth (const API_SectionSegment& s, const OS& spec)
{
	if (auto d = OptDouble (spec, "depth"))
		return *d;
	return s.horizRange == APIHorRange_Limited ? kDefaultLimitedDepth : 1.0;
}


API_Guid CreateCutPlaneLike (const OS& spec, API_ElemTypeID typeID)
{
	const GS::UniString label = typeID == API_ElevationID ? "elevation" : "section";
	if (!Has (spec, "begin") || !Has (spec, "end"))
		Fail ("A " + label + " requires 'begin' and 'end' points (the " + label + " line, m).");
	const API_Coord begin = GetCoord (spec, "begin");
	const API_Coord end = GetCoord (spec, "end");
	const Int32 side = ParseViewSide (spec, 1);

	CurrentDatabaseSwitch dbSwitch;
	EnsureMarkerDatabase (dbSwitch, true);

	API_Element element = NewElement (typeID);
	Memo memo;
	API_SubElement marker = NewMainMarker (false);
	SubElementMemoGuard markerGuard (marker);
	Check (ACAPI_Element_GetDefaultsExt (&element, memo.Ptr (), 1UL, &marker), "Cannot read the " + label + " tool defaults");

	ApplyCommonFields (element, nullptr, spec);
	API_CutPlaneType& cp = CutPlaneOf (element);
	ApplyRanges (cp.segment, nullptr, spec, false);
	if (!WriteCutPlaneGeometry (cp, *memo, begin, end, side, DefaultDepth (cp.segment, spec)) && memo->sectionSegmentDistCoords == nullptr)
		cp.segment.nDistCoord = 0;		// never announce coordinates the memo does not hold
	cp.linkData.sourceMarker = true;

	marker.subElem.object.useObjPens = true;
	marker.subType = APISubElement_MainMarker;
	Check (ACAPI_Element_CreateExt (&element, memo.Ptr (), 1UL, &marker), "Cannot create the " + label);
	return element.header.guid;
}


struct CutGeometry {
	GS::Array<API_Coord>	line;
	GS::Array<API_Coord>	depth;
	GS::Array<API_Coord>	dist;
};


CutGeometry LoadCutGeometry (const API_Guid& guid, const API_SectionSegment& s)
{
	CutGeometry g;
	Memo memo;
	if (ACAPI_Element_GetMemo (guid, memo.Ptr (), APIMemoMask_SectionMainCoords | APIMemoMask_SectionDepthCoords | APIMemoMask_SectionDistCoords) != NoError)
		return g;
	const USize nMain = GS::Min ((USize) s.nMainCoord, PtrCount (memo->sectionSegmentMainCoords));
	for (USize i = 0; i < nMain; ++i)
		g.line.Push (memo->sectionSegmentMainCoords[i]);
	const USize nDepth = GS::Min ((USize) s.nDepthCoord, PtrCount (memo->sectionSegmentDepthCoords));
	for (USize i = 0; i < nDepth; ++i)
		g.depth.Push (memo->sectionSegmentDepthCoords[i]);
	const USize nDist = GS::Min ((USize) s.nDistCoord, PtrCount (memo->sectionSegmentDistCoords));
	for (USize i = 0; i < nDist; ++i)
		g.dist.Push (memo->sectionSegmentDistCoords[i]);
	return g;
}


// Side (+1 left / -1 right) and distance of point p relative to the directed line a->b.
void SideAndDistance (const API_Coord& a, const API_Coord& b, const API_Coord& p, Int32& side, double& dist)
{
	const double dx = b.x - a.x, dy = b.y - a.y;
	const double len = std::hypot (dx, dy);
	const double cross = dx * (p.y - a.y) - dy * (p.x - a.x);
	side = cross >= 0.0 ? 1 : -1;
	dist = len > 1e-9 ? std::fabs (cross) / len : 0.0;
}


OS RangesJson (const API_SectionSegment& s)
{
	OS out;
	out.Add ("horizontalRange", NameOf (kHorizRange, s.horizRange));
	OS vr ("type", NameOf (kVertRange, s.vertRange));
	if (s.vertRange == APIVerRange_Limited) {
		vr.Add ("min", s.vertMin);
		vr.Add ("max", s.vertMax);
		vr.Add ("relativeToStory", s.relativeToStory);
	}
	out.Add ("verticalRange", vr);
	return out;
}


void SerializeCutPlaneLike (const API_Element& element, OS& out)
{
	const API_CutPlaneType& cp = CutPlaneOf (element);
	const API_SectionSegment& s = cp.segment;
	out.Add ("name", UStr (s.cutPlName));
	out.Add ("referenceId", UStr (s.cutPlIdStr));
	out.Add ("hasViewpoint", s.databaseID.elemSetId != APINULLGuid);
	if (s.databaseID.elemSetId != APINULLGuid)
		out.Add ("database", GuidStr (s.databaseID.elemSetId));
	out.Add ("sourceMarker", cp.linkData.sourceMarker);

	const CutGeometry g = LoadCutGeometry (element.header.guid, s);
	if (!g.line.IsEmpty ()) {
		GS::Array<OS> line;
		for (const API_Coord& c : g.line)
			line.Push (CoordObj (c));
		out.Add ("begin", CoordObj (g.line[0]));
		out.Add ("end", CoordObj (g.line[g.line.GetSize () - 1]));
		if (g.line.GetSize () > 2)
			out.Add ("line", line);
		if (!g.depth.IsEmpty ()) {
			Int32 side = 1;
			double dist = 0.0;
			SideAndDistance (g.line[0], g.line[g.line.GetSize () - 1], g.depth[0], side, dist);
			out.Add ("viewSide", GS::UniString (side > 0 ? "left" : "right"));
			out.Add ("depth", dist);
		}
	}
	OS ranges = RangesJson (s);
	GS::UniString h;
	ranges.Get ("horizontalRange", h);
	out.Add ("horizontalRange", h);
	OS vr;
	ranges.Get ("verticalRange", vr);
	out.Add ("verticalRange", vr);
	out.Add ("markedDistantArea", s.markedDistArea);
	out.Add ("showOnStories", NameOf (kShowOn, cp.cutPlShow));
	out.Add ("windowOpen", s.windOpened);
	out.Add ("active", s.active);
	out.Add ("drawingMode", s.drawingModeON);
	out.Add ("planConnection", NameOf (kPlanConn, s.currPlanConn));
	if (s.begMarkerId != APINULLGuid)	out.Add ("beginMarker", GuidStr (s.begMarkerId));
	if (s.midMarkerId != APINULLGuid)	out.Add ("middleMarker", GuidStr (s.midMarkerId));
	if (s.endMarkerId != APINULLGuid)	out.Add ("endMarker", GuidStr (s.endMarkerId));
}


void ModifyCutPlaneLike (API_Element& element, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask, const OS& patch)
{
	API_CutPlaneType& cp = CutPlaneOf (element);
	ApplyRanges (cp.segment, &mask, patch, false);

	if (Has (patch, "begin") || Has (patch, "end") || Has (patch, "depth") || Has (patch, "viewSide")) {
		const CutGeometry g = LoadCutGeometry (element.header.guid, cp.segment);
		if (g.line.GetSize () < 2 && (!Has (patch, "begin") || !Has (patch, "end")))
			Fail ("Cannot read the current line of the marker; pass both 'begin' and 'end'.", APIERR_GENERAL);
		const API_Coord begin = Has (patch, "begin") ? GetCoord (patch, "begin") : g.line[0];
		const API_Coord end = Has (patch, "end") ? GetCoord (patch, "end") : g.line[g.line.GetSize () - 1];
		Int32 curSide = 1;
		double curDepth = cp.segment.horizRange == APIHorRange_Limited ? kDefaultLimitedDepth : 1.0;
		if (!g.depth.IsEmpty () && g.line.GetSize () >= 2)
			SideAndDistance (g.line[0], g.line[g.line.GetSize () - 1], g.depth[0], curSide, curDepth);
		const Int32 side = ParseViewSide (patch, curSide);
		const double depth = GetDouble (patch, "depth", curDepth > 1e-6 ? curDepth : 1.0);
		const bool distWritten = WriteCutPlaneGeometry (cp, memo, begin, end, side, depth);
		ACAPI_ELEMENT_MASK_SET (mask, API_CutPlaneType, segment.nMainCoord);
		ACAPI_ELEMENT_MASK_SET (mask, API_CutPlaneType, segment.nDepthCoord);
		memoMask |= APIMemoMask_SectionMainCoords | APIMemoMask_SectionDepthCoords;
		if (distWritten) {
			ACAPI_ELEMENT_MASK_SET (mask, API_CutPlaneType, segment.nDistCoord);
			memoMask |= APIMemoMask_SectionDistCoords;
		}
	}
}


API_Guid CreateSection (const OS& spec)		{ return CreateCutPlaneLike (spec, API_CutPlaneID); }
API_Guid CreateElevation (const OS& spec)	{ return CreateCutPlaneLike (spec, API_ElevationID); }

// --- Interior elevations -----------------------------------------------------------------------

API_Guid CreateInteriorElevation (const OS& spec)
{
	if (!Has (spec, "points"))
		Fail ("An interior elevation requires 'points': the polyline of its segments (>= 2 points, m), e.g. along the walls of a room.");
	Contour line = GetPolyline (spec, "points");
	if (!line.arcs.IsEmpty ())
		Fail ("Interior elevation segments must be straight ('arcs' are not supported).");
	GS::Array<API_Coord> pts = line.points;
	if (GetBool (spec, "closed", false) && pts.GetSize () >= 3)
		pts.Push (pts[0]);
	if (pts.GetSize () < 2)
		Fail ("An interior elevation needs at least 2 points.");
	for (UIndex i = 1; i < pts.GetSize (); ++i) {
		if (std::hypot (pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y) < 0.01)
			Fail ("Consecutive interior elevation points must be at least 1 cm apart.");
	}
	const USize nPts = pts.GetSize ();
	const USize nSeg = nPts - 1;

	CurrentDatabaseSwitch dbSwitch;
	EnsureMarkerDatabase (dbSwitch, true);

	API_Element element = NewElement (API_InteriorElevationID);
	Memo memo;
	API_SubElement marker = NewMainMarker (true);
	SubElementMemoGuard markerGuard (marker);
	Check (ACAPI_Element_GetDefaultsExt (&element, memo.Ptr (), 1UL, &marker), "Cannot read the interior elevation tool defaults");
	AttachMarkerParams (marker, element.header.type);
	marker.subElem.object.useObjPens = true;
	marker.subType = APISubElement_MainMarker;

	ApplyCommonFields (element, nullptr, spec);
	API_InteriorElevationType& ie = element.interiorElevation;
	ApplyRanges (ie.segment, nullptr, spec, true);

	double depth = GetDouble (spec, "depth", ie.segment.ieCreationSegmentDepth > 1e-6 ? ie.segment.ieCreationSegmentDepth : 1.0);
	if (depth <= 0.0)
		Fail ("depth must be > 0 m.");
	const double offset = GetDouble (spec, "offset", 0.0);
	ie.segment.ieCreationSegmentDepth = depth;
	ie.segment.ieCreationSegmentHorizontalOffset = offset;
	ie.segment.nMainCoord = (UInt32) nPts;

	const GS::UniString baseName = UStr (ie.segment.cutPlName);
	const GS::UniString baseRef = UStr (ie.segment.cutPlIdStr);
	const bool numberNames = nSeg > 1 && Has (spec, "name");
	const bool numberRefs = nSeg > 1 && Has (spec, "referenceId");

	BMKillPtr (reinterpret_cast<GSPtr*> (&memo->sectionSegmentMainCoords));
	BMKillPtr (reinterpret_cast<GSPtr*> (&memo->intElevSegments));
	memo->sectionSegmentMainCoords = AllocPtr<API_Coord> (nPts);
	memo->intElevSegments = AllocPtr<API_SectionSegment> (nSeg);
	for (USize i = 0; i < nPts; ++i)
		memo->sectionSegmentMainCoords[i] = pts[i];
	for (USize i = 0; i < nSeg; ++i) {
		API_SectionSegment& seg = memo->intElevSegments[i];
		seg = ie.segment;
		seg.ieCreationSegmentDepth = depth;
		seg.ieCreationSegmentHorizontalOffset = offset;
		GS::UniString suffix;
		suffix.Printf ("%u", (unsigned) (i + 1));
		if (numberNames)	SetUStr (seg.cutPlName, baseName + " " + suffix);
		if (numberRefs)		SetUStr (seg.cutPlIdStr, baseRef + "." + suffix);
	}

	Check (ACAPI_Element_CreateExt (&element, memo.Ptr (), 1UL, &marker), "Cannot create the interior elevation");
	return element.header.guid;
}


void SerializeInteriorElevation (const API_Element& element, OS& out)
{
	const API_InteriorElevationType& ie = element.interiorElevation;
	out.Add ("name", UStr (ie.segment.cutPlName));
	out.Add ("referenceId", UStr (ie.segment.cutPlIdStr));
	out.Add ("segmentCount", (Int32) ie.nSegments);
	out.Add ("useCommonMarker", ie.useCommonMarker);
	if (ie.markerGuid != APINULLGuid)
		out.Add ("commonMarkerLibPart", GuidStr (ie.markerGuid));
	OS ranges = RangesJson (ie.segment);
	GS::UniString h;
	ranges.Get ("horizontalRange", h);
	out.Add ("horizontalRange", h);
	OS vr;
	ranges.Get ("verticalRange", vr);
	out.Add ("verticalRange", vr);
	out.Add ("showOnStories", NameOf (kShowOn, ie.cutPlShow));

	Memo memo;
	if (ACAPI_Element_GetMemo (element.header.guid, memo.Ptr (), APIMemoMask_SectionSegments | APIMemoMask_SectionMainCoords) == NoError) {
		GS::Array<OS> points;
		const USize nPts = PtrCount (memo->sectionSegmentMainCoords);
		for (USize i = 0; i < nPts; ++i)
			points.Push (CoordObj (memo->sectionSegmentMainCoords[i]));
		out.Add ("points", points);
		GS::Array<OS> segments;
		const USize nSeg = GS::Min ((USize) ie.nSegments, PtrCount (memo->intElevSegments));
		for (USize i = 0; i < nSeg; ++i) {
			const API_SectionSegment& s = memo->intElevSegments[i];
			OS seg ("index", (Int32) i, "name", UStr (s.cutPlName), "referenceId", UStr (s.cutPlIdStr));
			seg.Add ("hasViewpoint", s.databaseID.elemSetId != APINULLGuid);
			if (s.databaseID.elemSetId != APINULLGuid)
				seg.Add ("database", GuidStr (s.databaseID.elemSetId));
			seg.Add ("windowOpen", s.windOpened);
			segments.Push (seg);
		}
		out.Add ("segments", segments);
	}
}

// --- Details / worksheets ---------------------------------------------------------------------------

API_DetailType& DetailOf (API_Element& e)				{ return e.header.type.typeID == API_WorksheetID ? e.worksheet : e.detail; }
const API_DetailType& DetailOf (const API_Element& e)	{ return e.header.type.typeID == API_WorksheetID ? e.worksheet : e.detail; }


PolygonData DetailPolygon (const OS& spec)
{
	if (Has (spec, "polygon"))
		return GetPolygon (spec, "polygon");
	if (Has (spec, "box")) {
		OS b = GetObject (spec, "box");
		double x0 = GetDouble (b, "xMin"), y0 = GetDouble (b, "yMin"), x1 = GetDouble (b, "xMax"), y1 = GetDouble (b, "yMax");
		if (x1 < x0) std::swap (x0, x1);
		if (y1 < y0) std::swap (y0, y1);
		if (x1 - x0 < 0.01 || y1 - y0 < 0.01)
			Fail ("'box' must be at least 1 cm wide and high.");
		PolygonData data;
		API_Coord c;
		c.x = x0; c.y = y0; data.outline.points.Push (c);
		c.x = x1; c.y = y0; data.outline.points.Push (c);
		c.x = x1; c.y = y1; data.outline.points.Push (c);
		c.x = x0; c.y = y1; data.outline.points.Push (c);
		return data;
	}
	Fail ("Give the boundary as 'polygon' (points in m) or 'box' {xMin, yMin, xMax, yMax}.");
}


void ApplyDetailFields (API_DetailType& d, API_Element* mask, const OS& spec)
{
#define DET_SET(field) if (mask) ACAPI_ELEMENT_MASK_SET (*mask, API_DetailType, field)
	if (auto name = OptString (spec, "name"))			{ SetUStr (d.detailName, *name); DET_SET (detailName); }
	if (auto ref = OptString (spec, "referenceId"))		{ SetUStr (d.detailIdStr, *ref); DET_SET (detailIdStr); }
	if (auto a = OptAngle (spec, "markerAngle"))		{ d.angle = *a; DET_SET (angle); }
	if (auto h = OptBool (spec, "horizontalMarker"))	{ d.horizontalMarker = *h; DET_SET (horizontalMarker); }
	if (auto p = OptCoord (spec, "markerPosition"))		{ d.pos = *p; DET_SET (pos); }
#undef DET_SET
}


API_Guid CreateDetailLike (const OS& spec, API_ElemTypeID typeID)
{
	const GS::UniString label = typeID == API_WorksheetID ? "worksheet" : "detail";
	const PolygonData poly = DetailPolygon (spec);

	CurrentDatabaseSwitch dbSwitch;
	EnsureMarkerDatabase (dbSwitch, false);

	API_Element element = NewElement (typeID);
	Memo memo;
	API_SubElement marker = NewMainMarker (true);
	SubElementMemoGuard markerGuard (marker);
	Check (ACAPI_Element_GetDefaultsExt (&element, memo.Ptr (), 1UL, &marker), "Cannot read the " + label + " tool defaults");
	AttachMarkerParams (marker, element.header.type);

	ApplyCommonFields (element, nullptr, spec);
	API_DetailType& d = DetailOf (element);
	WritePolygonToMemo (poly, d.poly, *memo);

	double xMax = poly.outline.points[0].x, yMax = poly.outline.points[0].y;
	for (const API_Coord& c : poly.outline.points) {
		xMax = GS::Max (xMax, c.x);
		yMax = GS::Max (yMax, c.y);
	}
	API_Coord markerPos;
	markerPos.x = xMax + 0.5;
	markerPos.y = yMax + 0.5;
	d.pos = markerPos;
	ApplyDetailFields (d, nullptr, spec);
	d.linkData.sourceMarker = true;

	marker.subElem.object.pos = d.pos;
	marker.subElem.object.useObjPens = true;
	marker.subType = APISubElement_MainMarker;
	Check (ACAPI_Element_CreateExt (&element, memo.Ptr (), 1UL, &marker), "Cannot create the " + label);
	return element.header.guid;
}


void SerializeDetailLike (const API_Element& element, OS& out)
{
	const API_DetailType& d = DetailOf (element);
	out.Add ("name", UStr (d.detailName));
	out.Add ("referenceId", UStr (d.detailIdStr));
	out.Add ("hasViewpoint", d.databaseID.elemSetId != APINULLGuid);
	if (d.databaseID.elemSetId != APINULLGuid)
		out.Add ("database", GuidStr (d.databaseID.elemSetId));
	out.Add ("sourceMarker", d.linkData.sourceMarker);
	out.Add ("markerPosition", CoordObj (d.pos));
	AddAngle (out, "markerAngle", d.angle);
	out.Add ("horizontalMarker", d.horizontalMarker);
	if (d.markId != APINULLGuid)
		out.Add ("marker", GuidStr (d.markId));
	out.Add ("windowOpen", d.windOpened);
	Memo memo;
	if (ACAPI_Element_GetMemo (element.header.guid, memo.Ptr (), APIMemoMask_Polygon) == NoError && memo->coords != nullptr)
		out.Add ("polygon", PolygonToJson (d.poly, *memo));
}


void ModifyDetailLike (API_Element& element, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask, const OS& patch)
{
	API_DetailType& d = DetailOf (element);
	ApplyDetailFields (d, &mask, patch);
	if (Has (patch, "polygon") || Has (patch, "box")) {
		WritePolygonToMemo (DetailPolygon (patch), d.poly, memo);
		ACAPI_ELEMENT_MASK_SET (mask, API_DetailType, poly);
		memoMask |= APIMemoMask_Polygon;
	}
}


API_Guid CreateDetail (const OS& spec)		{ return CreateDetailLike (spec, API_DetailID); }
API_Guid CreateWorksheet (const OS& spec)	{ return CreateDetailLike (spec, API_WorksheetID); }

} // namespace


void RegisterViewMarkerAdapters ()
{
	RegisterAdapter ({ API_CutPlaneID, CreateSection, SerializeCutPlaneLike, ModifyCutPlaneLike });
	RegisterAdapter ({ API_ElevationID, CreateElevation, SerializeCutPlaneLike, ModifyCutPlaneLike });
	RegisterAdapter ({ API_InteriorElevationID, CreateInteriorElevation, SerializeInteriorElevation, nullptr });
	RegisterAdapter ({ API_DetailID, CreateDetail, SerializeDetailLike, ModifyDetailLike });
	RegisterAdapter ({ API_WorksheetID, CreateWorksheet, SerializeDetailLike, ModifyDetailLike });
}

} // namespace views
} // namespace cc
