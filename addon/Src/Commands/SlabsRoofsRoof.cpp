// *****************************************************************************
// SlabsRoofsRoof — Roof adapter (single-plane and multi-plane roofs).
//
// Create / modify fields (meters, degrees):
//   roofClass "SinglePlane"|"MultiPlane" (create only; default: MultiPlane when pivotPolygon is given)
//   common (API_ShellBaseType): level (pivot line / pivot polygon elevation from the home story),
//     thickness, buildingMaterial | composite, topSurface / bottomSurface / sideSurface,
//     surfacesChained, edgeTrim / edgeAngle (default edge trim), connectionBody, floorPlanDisplay,
//     viewDepth, pens / line types / cover fill fields, storyVisibility, autoStoryVisibility
//   SinglePlane: polygon* (roof outline in plan), pivotLine* {begin, end}, slopeAngle,
//     risesToLeft (default: towards the polygon), edges [{contour?, index, trim?, angle?, surface?, edgeType?}]
//   MultiPlane: pivotPolygon*, slopeAngle (one level), levels [{angle, height?}] (1..16; height = where
//     that level ENDS above the pivot polygon, required except for the top level),
//     eavesOverhang, arcSegments | circleSegments, fitSkylightsToCurve,
//     pivotEdges [{contour?, index, angle?, angles?, gable?, eavesOverhang?}]
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

const NamedValue kRoofClasses[] = {
	{ "SinglePlane",	API_PlaneRoofID },
	{ "MultiPlane",		API_PolyRoofID },
};

const NamedValue kOverhangTypes[] = {
	{ "Offset",		API_OffsetOverhang },
	{ "Manual",		API_ManualOverhang },
};

constexpr UInt64 kPlaneRoofPolygonMask = APIMemoMask_Polygon | APIMemoMask_EdgeTrims | APIMemoMask_SideMaterials | APIMemoMask_RoofEdgeTypes;


void FailIfPresent (const OS& spec, std::initializer_list<const char*> keys, const GS::UniString& why)
{
	for (const char* key : keys) {
		if (Has (spec, key))
			Fail ("Field '" + GS::UniString (key) + "' " + why);
	}
}


double SlopeFrom (const OS& spec, const char* key, bool allowZero)
{
	const double a = GetAngle (spec, key);
	if (a < 0.0 || (!allowZero && a <= 0.0) || a >= kPi / 2.0)
		Fail ("Field '" + GS::UniString (key) + "' must be a slope in degrees, " + (allowZero ? "0" : "above 0") + " to below 90.");
	return a;
}


bool AutoRisesToLeft (const API_Sector& line, const GS::Array<API_Coord>& outline)
{
	if (outline.IsEmpty ())
		return true;
	API_Coord c { 0.0, 0.0 };
	for (const API_Coord& p : outline) { c.x += p.x; c.y += p.y; }
	c.x /= outline.GetSize ();
	c.y /= outline.GetSize ();
	const double cross = (line.c2.x - line.c1.x) * (c.y - line.c1.y) - (line.c2.y - line.c1.y) * (c.x - line.c1.x);
	return cross >= 0.0;
}


void ApplyRoofFields (API_Element& element, API_Element* mask, const OS& spec)
{
	API_RoofType& roof = element.roof;
	const FieldMask m (element, mask);
	if (Has (spec, "profile"))
		Fail ("Roofs cannot use complex profiles; use 'buildingMaterial' or 'composite'.", APIERR_NOTSUPPORTED);
	ApplyShellBaseFields (roof.shellBase, m, spec);
	CheckCompositeUsage (spec, roof.shellBase.composite, APICWall_ForRoof, "roofs");

	if (roof.roofClass == API_PlaneRoofID) {
		FailIfPresent (spec, { "pivotPolygon", "levels", "eavesOverhang", "pivotEdges", "arcSegments", "circleSegments", "fitSkylightsToCurve" },
					   "is only valid for multi-plane roofs (roofClass 'MultiPlane'). Single-plane roofs use polygon + pivotLine + slopeAngle.");
		API_PlaneRoofData& p = roof.u.planeRoof;
		if (Has (spec, "slopeAngle")) {
			p.angle = SlopeFrom (spec, "slopeAngle", true);
			m.Set (&p.angle);
		}
		OS line;
		if (TryGetObject (spec, "pivotLine", line)) {
			const API_Coord c1 = GetCoord (line, "begin");
			const API_Coord c2 = GetCoord (line, "end");
			if (std::hypot (c2.x - c1.x, c2.y - c1.y) < 1e-6)
				Fail ("'pivotLine' must have distinct 'begin' and 'end' points.");
			p.baseLine.c1 = c1;
			p.baseLine.c2 = c2;
			m.Set (&p.baseLine);
		}
		if (auto b = OptBool (spec, "risesToLeft")) {
			p.posSign = *b;
			m.Set (&p.posSign);
		}
		return;
	}

	FailIfPresent (spec, { "pivotLine", "risesToLeft", "edges" },
				   "is only valid for single-plane roofs (roofClass 'SinglePlane'). Multi-plane roofs use pivotPolygon + levels/slopeAngle (+ pivotEdges).");
	API_PolyRoofData& p = roof.u.polyRoof;
	// Level semantics (verified live): levelData[i].levelHeight = height above the pivot polygon where
	// level i ENDS and level i+1 begins; the top level continues to the ridge (its height is not a cap).
	constexpr double kOpenTop = 20.0;		// Archicad's default top-level height
	if (Has (spec, "slopeAngle")) {
		p.levelNum = 1;
		p.levelData[0].levelAngle = SlopeFrom (spec, "slopeAngle", false);
		if (p.levelData[0].levelHeight <= 0.0)
			p.levelData[0].levelHeight = kOpenTop;
		m.Set (&p.levelNum);
		m.Set (&p.levelData);
	}
	if (Has (spec, "levels")) {
		const GS::Array<OS> levels = GetObjectArray (spec, "levels");
		if (levels.IsEmpty () || levels.GetSize () > 16)
			Fail ("'levels' must contain 1 to 16 roof levels [{angle, height}].");
		const Int32 n = (Int32) levels.GetSize ();
		double previous = 0.0;		// the pivot polygon
		for (Int32 i = 0; i < n; ++i) {
			const OS& lv = levels[i];
			const bool last = i == n - 1;
			const double angle = SlopeFrom (lv, "angle", false);
			double height = 0.0;
			if (auto h = OptDouble (lv, "height")) {
				height = *h;
			} else if (last) {
				height = std::max (p.levelData[i].levelHeight, previous + kOpenTop);
			} else {
				Fail (GS::UniString::Printf ("levels[%d] needs 'height': the height above the pivot polygon (m) where this level ends "
											 "and levels[%d] begins (only the top level may omit it).", (int) i, (int) i + 1));
			}
			if (height <= previous) {
				if (i == 0)
					Fail ("levels[0].height must be above 0 (it is the height above the pivot polygon where the first pitch ends).");
				Fail (GS::UniString::Printf ("levels[%d].height must be greater than levels[%d].height (heights are measured from the pivot polygon and must increase).",
											 (int) i, (int) i - 1));
			}
			p.levelData[i].levelAngle = angle;
			p.levelData[i].levelHeight = height;
			previous = height;
		}
		p.levelNum = (short) n;
		m.Set (&p.levelNum);
		m.Set (&p.levelData);
	}
	if (auto v = OptDouble (spec, "eavesOverhang")) {
		p.overHangType = API_OffsetOverhang;
		p.eavesOverHang = *v;
		m.Set (&p.overHangType);
		m.Set (&p.eavesOverHang);
	}
	if (auto n = OptInt (spec, "arcSegments")) {
		if (*n < 1 || *n > 360) Fail ("'arcSegments' must be 1..360.");
		p.segmentType = APIShellBase_SegmentsByArc;
		p.segmentsByArc = *n;
		m.Set (&p.segmentType);
		m.Set (&p.segmentsByArc);
	}
	if (auto n = OptInt (spec, "circleSegments")) {
		if (*n < 3 || *n > 360) Fail ("'circleSegments' must be 3..360.");
		p.segmentType = APIShellBase_SegmentsByCircle;
		p.segmentsByCircle = *n;
		m.Set (&p.segmentType);
		m.Set (&p.segmentsByCircle);
	}
	if (ApplyFlag (spec, "fitSkylightsToCurve", p.fitSkylightToCurve))
		m.Set (&p.fitSkylightToCurve);
}


// Moves the given additional-polygon handles into a scratch memo and disposes them with Archicad's disposer.
void DisposeAdditionalPolygon (API_ElementMemo& memo, bool alsoIds)
{
	API_ElementMemo scratch;
	BNZeroMemory (&scratch, sizeof (scratch));
	std::swap (scratch.additionalPolyCoords, memo.additionalPolyCoords);
	std::swap (scratch.additionalPolyPends, memo.additionalPolyPends);
	std::swap (scratch.additionalPolyParcs, memo.additionalPolyParcs);
	if (alsoIds) {
		std::swap (scratch.additionalPolyVertexIDs, memo.additionalPolyVertexIDs);
		std::swap (scratch.additionalPolyEdgeIDs, memo.additionalPolyEdgeIDs);
		std::swap (scratch.additionalPolyContourIDs, memo.additionalPolyContourIDs);
	}
	ACAPI_DisposeElemMemoHdls (&scratch);
}


// Writes the pivot polygon into memo.additionalPoly* (existing coords/pends/parcs are replaced).
// For an existing roof (keepIds) with the SAME vertex/contour count, the vertex/edge/contour IDs and the
// per-edge data (pivotPolyEdges: pitch/gable/overhang overrides) are kept — verified live. When the
// layout changes, the IDs and per-edge data are dropped (like on creation; resized ID handles with new
// zero IDs make ACAPI_Element_Change fail with APIERR_BADPARS — verified live), so Archicad
// regenerates them from the roof levels. Returns true when the layout changed.
bool WritePivotPolygon (const PolygonData& polygon, API_Polygon& pivotPoly, API_ElementMemo& memo, bool keepIds)
{
	Memo tmp;
	API_Polygon poly;
	BNZeroMemory (&poly, sizeof (poly));
	WritePolygonToMemo (polygon, poly, *tmp);

	const API_Polygon old = PolygonCounts (memo.additionalPolyCoords, memo.additionalPolyPends, memo.additionalPolyParcs);
	const bool layoutChanged = old.nCoords != poly.nCoords || old.nSubPolys != poly.nSubPolys;
	const bool keep = keepIds && !layoutChanged;
	DisposeAdditionalPolygon (memo, !keep);
	std::swap (memo.additionalPolyCoords, tmp->coords);
	std::swap (memo.additionalPolyPends, tmp->pends);
	std::swap (memo.additionalPolyParcs, tmp->parcs);		// stays null when there are no arcs
	if (!keep)
		DisposePivotEdges (memo);
	pivotPoly = poly;
	return keepIds && layoutChanged;
}


OS PivotPolygonJson (const API_ElementMemo& memo)
{
	API_ElementMemo view;		// non-owning view: never disposed
	BNZeroMemory (&view, sizeof (view));
	view.coords = memo.additionalPolyCoords;
	view.pends = memo.additionalPolyPends;
	view.parcs = memo.additionalPolyParcs;
	const API_Polygon counts = PolygonCounts (view.coords, view.pends, view.parcs);
	return PolygonToJson (counts, view);
}


void AddPivotEdgesJson (OS& out, const API_ElementMemo& memo)
{
	if (memo.pivotPolyEdges == nullptr || memo.additionalPolyPends == nullptr)
		return;
	const Int32 n = PtrCount (reinterpret_cast<GSConstPtr> (memo.pivotPolyEdges), sizeof (API_PivotPolyEdgeData));
	GS::Array<OS> list;
	const GS::Array<ContourRange> contours = ContoursOf (memo.additionalPolyPends);
	for (UIndex c = 0; c < contours.GetSize (); ++c) {
		for (Int32 i = 0; i < contours[c].count; ++i) {
			const Int32 j = contours[c].first + i;
			if (j >= n)
				continue;
			const API_PivotPolyEdgeData& e = memo.pivotPolyEdges[j];
			if (e.levelEdgeData == nullptr || e.nLevelEdgeData < 1)
				continue;
			GS::Array<double> angles;
			bool gable = false;
			for (Int32 l = 0; l < e.nLevelEdgeData; ++l) {
				angles.Push (RadToDeg (e.levelEdgeData[l].angle));
				if (e.levelEdgeData[l].angleType == APIPolyRoof_SegmentAngleTypeGable)
					gable = true;
			}
			OS item ("contour", (Int32) c, "index", i);
			item.Add ("angles", angles);
			item.Add ("gable", gable);
			item.Add ("eavesOverhang", e.levelEdgeData[0].eavesOverhang);
			list.Push (item);
		}
	}
	if (!list.IsEmpty ())
		out.Add ("pivotEdges", list);
}


// Stored pivot edge (memo index of its first vertex) whose endpoints are a and b (either direction), or -1.
Int32 FindStoredPivotEdge (const API_ElementMemo& memo, const GS::Array<ContourRange>& contours, const API_Coord& a, const API_Coord& b)
{
	const Int32 nCoords = HandleCount (reinterpret_cast<GSConstHandle> (memo.additionalPolyCoords), sizeof (API_Coord));
	auto same = [] (const API_Coord& p, const API_Coord& q) { return std::fabs (p.x - q.x) < 1e-6 && std::fabs (p.y - q.y) < 1e-6; };
	for (const ContourRange& c : contours) {
		for (Int32 i = 0; i < c.count; ++i) {
			const Int32 j = c.first + i;
			if (j + 1 >= nCoords)
				break;
			const API_Coord& p = (*memo.additionalPolyCoords)[j];
			const API_Coord& q = (*memo.additionalPolyCoords)[j + 1];		// closing vertex repeats the first
			if ((same (p, a) && same (q, b)) || (same (p, b) && same (q, a)))
				return j;
		}
	}
	return -1;
}


// Applies "pivotEdges" to an existing multi-plane roof (reads Archicad's per-edge data, changes the
// requested values, writes it back with ACAPI_Element_Change + APIMemoMask_AdditionalPolygon).
// source = the pivot polygon given in the same request: edges are then located by their endpoints
// (independent of how Archicad orders the stored polygon); otherwise indices refer to the stored polygon.
void ApplyPivotEdges (const API_Guid& guid, const OS& spec, const PolygonData* source)
{
	const GS::Array<OS> items = GetObjectArray (spec, "pivotEdges");
	if (items.IsEmpty ())
		return;

	API_Element element = GetElement (guid);
	Memo memo;
	LoadMemo (guid, *memo, APIMemoMask_AdditionalPolygon);
	if (memo->pivotPolyEdges == nullptr || memo->additionalPolyPends == nullptr || memo->additionalPolyCoords == nullptr)
		Fail ("Archicad returned no pivot edge data for roof " + GuidStr (guid) + "; 'pivotEdges' cannot be applied.", APIERR_GENERAL);
	const GS::Array<ContourRange> contours = ContoursOf (memo->additionalPolyPends);
	const Int32 nData = PtrCount (reinterpret_cast<GSConstPtr> (memo->pivotPolyEdges), sizeof (API_PivotPolyEdgeData));

	for (UIndex k = 0; k < items.GetSize (); ++k) {
		const OS& item = items[k];
		GS::UniString where;
		where = GS::UniString::Printf ("pivotEdges[%u]", (unsigned) k);
		const Int32 nContours = source != nullptr ? 1 + (Int32) source->holes.GetSize () : (Int32) contours.GetSize ();
		const Int32 c = GetInt (item, "contour", 0);
		if (c < 0 || c >= nContours) {
			GS::UniString msg;
			msg = GS::UniString::Printf (".contour %d is out of range: 0 = pivot polygon outline, 1..%d = holes.", (int) c, (int) nContours - 1);
			Fail (where + msg);
		}
		const Int32 count = source != nullptr ? (Int32) (c == 0 ? source->outline : source->holes[c - 1]).points.GetSize ()
											  : contours[c].count;
		const Int32 idx = GetInt (item, "index");
		if (idx < 0 || idx >= count) {
			GS::UniString msg;
			msg = GS::UniString::Printf (".index %d is out of range: contour %d has %d edges (0..%d; edge i runs from point i to point i+1).",
						(int) idx, (int) c, (int) count, (int) count - 1);
			Fail (where + msg);
		}
		Int32 j = -1;
		if (source != nullptr) {
			const Contour& sc = c == 0 ? source->outline : source->holes[c - 1];
			j = FindStoredPivotEdge (*memo, contours, sc.points[idx], sc.points[(idx + 1) % count]);
			if (j < 0)
				Fail (where + ": this edge was not found in the stored pivot polygon (Archicad may have merged collinear or duplicate points). "
					  "Read the roof with get_element_details and address the edge by its stored index in a separate modify_roofs call.", APIERR_GENERAL);
		} else {
			j = contours[c].first + idx;
		}
		if (j >= nData)
			Fail (where + ": Archicad returned no data for this pivot edge.", APIERR_GENERAL);
		API_PivotPolyEdgeData& e = memo->pivotPolyEdges[j];
		if (e.levelEdgeData == nullptr || e.nLevelEdgeData < 1)
			Fail (where + ": Archicad returned no per-level data for this pivot edge.", APIERR_GENERAL);

		const auto angle = OptAngle (item, "angle");
		const GS::Array<double> angles = GetNumberArray (item, "angles", false);
		const auto gable = OptBool (item, "gable");
		const auto overhang = OptDouble (item, "eavesOverhang");
		if (!angle.has_value () && angles.IsEmpty () && !gable.has_value () && !overhang.has_value ())
			Fail (where + " has nothing to change: give angle, angles, gable or eavesOverhang.");
		if ((Int32) angles.GetSize () > e.nLevelEdgeData) {
			GS::UniString msg;
			msg = GS::UniString::Printf (".angles has %u values but the roof has %d levels.", (unsigned) angles.GetSize (), (int) e.nLevelEdgeData);
			Fail (where + msg);
		}
		auto checkSlope = [&] (double rad) {
			if (rad <= 0.0 || rad >= kPi / 2.0)
				Fail (where + ": slope angles must be above 0 and below 90 degrees (use gable: true for a vertical gable end).");
			return rad;
		};
		for (Int32 l = 0; l < e.nLevelEdgeData; ++l) {
			API_RoofSegmentData& s = e.levelEdgeData[l];
			if (angle.has_value ()) {
				s.angle = checkSlope (*angle);
				s.angleType = APIPolyRoof_SegmentAngleTypeSloped;
			}
			if (l < (Int32) angles.GetSize ()) {
				s.angle = checkSlope (DegToRad (angles[l]));
				s.angleType = APIPolyRoof_SegmentAngleTypeSloped;
			}
			if (gable.has_value ())
				s.angleType = *gable ? APIPolyRoof_SegmentAngleTypeGable : APIPolyRoof_SegmentAngleTypeSloped;
		}
		if (overhang.has_value ())
			e.levelEdgeData[0].eavesOverhang = *overhang;
	}

	// The closing vertex of each contour repeats the first edge's data (as for slab edge trims);
	// Archicad reads edge 0 from there — without this sync a change of edge 0 was lost (verified live).
	for (const ContourRange& c : contours) {
		const Int32 first = c.first;
		const Int32 closing = c.first + c.count;
		if (closing >= nData)
			continue;
		const API_PivotPolyEdgeData& src = memo->pivotPolyEdges[first];
		API_PivotPolyEdgeData& dst = memo->pivotPolyEdges[closing];
		if (src.levelEdgeData == nullptr || dst.levelEdgeData == nullptr)
			continue;
		const Int32 n = std::min (src.nLevelEdgeData, dst.nLevelEdgeData);
		for (Int32 l = 0; l < n; ++l)
			dst.levelEdgeData[l] = src.levelEdgeData[l];
	}

	API_Element mask;
	ACAPI_ELEMENT_MASK_CLEAR (mask);
	Check (ACAPI_Element_Change (&element, &mask, memo.Ptr (), APIMemoMask_AdditionalPolygon, true),
		   "Cannot change the pivot edges of roof " + GuidStr (guid));
}


GS::UniString PolyHint (GSErrCode err)
{
	if (err == APIERR_IRREGULARPOLY || err == APIERR_BADPOLY)
		return " (the polygon is irregular: self-intersecting, repeated/collinear points or holes outside the outline — fix the points)";
	return GS::UniString ();
}


API_Guid CreateRoof (const OS& spec)
{
	API_RoofClassID cls = Has (spec, "pivotPolygon") ? API_PolyRoofID : API_PlaneRoofID;
	if (Has (spec, "roofClass"))
		cls = (API_RoofClassID) ParseNamed (kRoofClasses, spec, "roofClass");

	API_Element element = NewElement (API_RoofID);
	element.roof.roofClass = cls;
	GetDefaults (element, nullptr);
	element.roof.roofClass = cls;
	ApplyCommonFields (element, nullptr, spec);
	ApplyRoofFields (element, nullptr, spec);

	Memo memo;
	GS::Array<bool> reversed;
	PolygonData pivot;
	if (cls == API_PlaneRoofID) {
		if (!Has (spec, "polygon") || !Has (spec, "pivotLine"))
			Fail ("Single-plane roof requires 'polygon' (roof outline in plan) and 'pivotLine' {begin, end} (the line the plane rotates around, usually the eaves).");
		const PolygonData polygon = GetPolygon (spec, "polygon");
		reversed = ReversedContours (polygon);
		WritePolygonToMemo (polygon, element.roof.u.planeRoof.poly, *memo);
		if (!Has (spec, "risesToLeft"))
			element.roof.u.planeRoof.posSign = AutoRisesToLeft (element.roof.u.planeRoof.baseLine, polygon.outline.points);
		if (Has (spec, "edges")) {
			AllocEdgeData (*memo, element.roof.u.planeRoof.poly.nCoords, element.roof.shellBase.edgeTrim, element.roof.shellBase.sidMat);
			ApplyEdgeOverrides (spec, *memo, &reversed, true);
		}
	} else {
		if (Has (spec, "polygon"))
			Fail ("Multi-plane roofs take 'pivotPolygon' (the eaves line polygon); their contour is generated by offsetting it by 'eavesOverhang'. Do not pass 'polygon'.");
		if (!Has (spec, "pivotPolygon"))
			Fail ("Multi-plane roof requires 'pivotPolygon' (closed polygon of the eaves/pivot lines, usually the outer wall outline).");
		pivot = GetPolygon (spec, "pivotPolygon");
		WritePivotPolygon (pivot, element.roof.u.polyRoof.pivotPolygon, *memo, false);
		element.roof.u.polyRoof.overHangType = API_OffsetOverhang;	// contour = pivot polygon offset by eavesOverhang
	}

	GSErrCode err = ACAPI_Element_Create (&element, memo.Ptr ());
	if (err == APIERR_IRREGULARPOLY && cls == API_PlaneRoofID && RegularizeMemoPolygon (*memo, element.roof.u.planeRoof.poly, "Roof"))
		err = ACAPI_Element_Create (&element, memo.Ptr ());
	Check (err, "Cannot create roof" + PolyHint (err));
	const API_Guid guid = element.header.guid;

	if (cls == API_PolyRoofID && Has (spec, "pivotEdges")) {
		try {
			ApplyPivotEdges (guid, spec, &pivot);
		} catch (const Error& e) {
			GS::Array<API_Guid> created;
			created.Push (guid);
			ACAPI_Element_Delete (created);		// do not leave a half-configured roof behind
			throw Error ("Roof not created: " + e.message, e.code);
		}
	}
	return guid;
}


void SerializeRoof (const API_Element& element, OS& out)
{
	const API_RoofType& roof = element.roof;
	out.Add ("roofClass", NameOf (kRoofClasses, roof.roofClass));
	AddShellBaseJson (out, roof.shellBase);

	Memo memo;
	const UInt64 mask = roof.roofClass == API_PlaneRoofID ? kPlaneRoofPolygonMask : (APIMemoMask_Polygon | APIMemoMask_AdditionalPolygon);
	const bool haveMemo = ACAPI_Element_GetMemo (element.header.guid, memo.Ptr (), mask) == NoError;

	if (roof.roofClass == API_PlaneRoofID) {
		const API_PlaneRoofData& p = roof.u.planeRoof;
		out.Add ("slopeAngle", RadToDeg (p.angle));
		out.Add ("pivotLine", OS ("begin", CoordObj (p.baseLine.c1), "end", CoordObj (p.baseLine.c2)));
		out.Add ("risesToLeft", p.posSign);
		if (haveMemo) {
			out.Add ("polygon", PolygonToJson (PolygonCounts (memo->coords, memo->pends, memo->parcs), *memo));
			AddEdgesJson (out, *memo, roof.shellBase.edgeTrim, roof.shellBase.sidMat, true);
		}
		return;
	}

	const API_PolyRoofData& p = roof.u.polyRoof;
	GS::Array<OS> levels;
	for (short i = 0; i < p.levelNum && i < 16; ++i)
		levels.Push (OS ("angle", RadToDeg (p.levelData[i].levelAngle), "height", p.levelData[i].levelHeight));
	out.Add ("levels", levels);
	out.Add ("eavesOverhang", p.eavesOverHang);
	out.Add ("overhangType", NameOf (kOverhangTypes, p.overHangType));
	out.Add ("curveSegmentation", NameOf (kSegmentTypes, p.segmentType));
	out.Add ("arcSegments", p.segmentsByArc);
	out.Add ("circleSegments", p.segmentsByCircle);
	out.Add ("fitSkylightsToCurve", p.fitSkylightToCurve);
	if (haveMemo) {
		out.Add ("pivotPolygon", PivotPolygonJson (*memo));
		out.Add ("contourPolygon", PolygonToJson (PolygonCounts (memo->coords, memo->pends, memo->parcs), *memo));
		AddPivotEdgesJson (out, *memo);
	}
}


void ModifyRoof (API_Element& element, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask, const OS& patch)
{
	API_RoofType& roof = element.roof;
	if (Has (patch, "roofClass")) {
		const API_RoofClassID cls = (API_RoofClassID) ParseNamed (kRoofClasses, patch, "roofClass");
		if (cls != roof.roofClass)
			Fail ("Cannot change roofClass of an existing roof (it is " + NameOf (kRoofClasses, roof.roofClass) +
				  "). Create a new roof with create_roofs and delete this one.", APIERR_NOTSUPPORTED);
	}
	ApplyRoofFields (element, &mask, patch);
	API_Guid guid = element.header.guid;

	if (roof.roofClass == API_PlaneRoofID) {
		const bool polygonChange = Has (patch, "polygon");
		const bool edgesGiven = Has (patch, "edges");
		const bool trimGiven = Has (patch, "edgeTrim") || Has (patch, "edgeAngle");
		const bool sideGiven = Has (patch, "sideSurface");
		if (!polygonChange && !edgesGiven && !trimGiven && !sideGiven)
			return;

		CommitStructChange (element, mask, "Cannot modify roof " + GuidStr (guid));
		Memo own;
		LoadMemo (guid, *own, APIMemoMask_All);
		API_Polygon counts = PolygonCounts (own->coords, own->pends, own->parcs);
		if (!polygonChange && !edgesGiven && !HasEdgeData (*own, counts.nCoords))
			return;		// no per-edge data stored: the roof-wide trim / side surface already applies to all edges

		GS::Array<bool> reversed;
		if (polygonChange) {
			const PolygonData polygon = GetPolygon (patch, "polygon");
			reversed = ReversedContours (polygon);
			ReshapePolygonInPlace (*own, polygon);
			counts = PolygonCounts (own->coords, own->pends, own->parcs);
		}
		if (polygonChange || !HasEdgeData (*own, counts.nCoords)) {
			AllocEdgeData (*own, counts.nCoords, roof.shellBase.edgeTrim, roof.shellBase.sidMat);
			if (polygonChange && own->roofEdgeTypes != nullptr) {
				GSPtr p = reinterpret_cast<GSPtr> (own->roofEdgeTypes);
				BMKillPtr (&p);
				own->roofEdgeTypes = nullptr;		// null = let Archicad recompute the edge types
			}
		} else {
			for (const ContourRange& c : ContoursOf (own->pends)) {
				for (Int32 i = 0; i <= c.count; ++i) {
					if (trimGiven)	(*own->edgeTrims)[c.first + i] = roof.shellBase.edgeTrim;
					if (sideGiven)	own->sideMaterials[c.first + i] = roof.shellBase.sidMat;
				}
			}
		}
		ApplyEdgeOverrides (patch, *own, polygonChange ? &reversed : nullptr, true);
		SyncClosingEdgeData (*own);
		GSErrCode err = ACAPI_Element_ChangeMemo (guid, kPlaneRoofPolygonMask, own.Ptr ());
		Check (err, "Cannot change the polygon/edges of roof " + GuidStr (guid) + PolyHint (err));
		return;
	}

	// Multi-plane roof
	if (Has (patch, "polygon"))
		Fail ("The contour of a multi-plane roof is generated from 'pivotPolygon' and 'eavesOverhang'; change those instead of 'polygon'.", APIERR_NOTSUPPORTED);
	PolygonData pivot;
	const bool pivotChange = Has (patch, "pivotPolygon");
	if (pivotChange) {
		LoadMemo (guid, memo, APIMemoMask_AdditionalPolygon);
		pivot = GetPolygon (patch, "pivotPolygon");
		const bool layoutChanged = WritePivotPolygon (pivot, roof.u.polyRoof.pivotPolygon, memo, true);
		if (roof.u.polyRoof.overHangType != API_OffsetOverhang) {
			roof.u.polyRoof.overHangType = API_OffsetOverhang;
			FieldMask (element, &mask).Set (&roof.u.polyRoof.overHangType);
		}
		memoMask |= APIMemoMask_AdditionalPolygon;
		if (layoutChanged) {
			// Store it here to give an actionable message when Archicad refuses the new vertex count.
			GSErrCode err = ACAPI_Element_Change (&element, &mask, &memo, memoMask, true);
			if (err != NoError) {
				Check (err, "Cannot change the pivot polygon of multi-plane roof " + GuidStr (guid) +
					   " to a different number of vertices/holes" + PolyHint (err) +
					   ". Workaround: keep the vertex count (move points instead), or create a new roof with create_roofs "
					   "(copy the settings from get_element_details) and delete this one");
			}
			ACAPI_ELEMENT_MASK_CLEAR (mask);
			memoMask = 0;
		}
	}
	if (Has (patch, "pivotEdges")) {
		// The per-edge data must be read after every other change is stored.
		if (memoMask != 0) {
			GSErrCode err = ACAPI_Element_Change (&element, &mask, &memo, memoMask, true);
			Check (err, "Cannot modify roof " + GuidStr (guid) + PolyHint (err));
			ACAPI_ELEMENT_MASK_CLEAR (mask);
			memoMask = 0;
		} else {
			CommitStructChange (element, mask, "Cannot modify roof " + GuidStr (guid));
		}
		ApplyPivotEdges (element.header.guid, patch, pivotChange ? &pivot : nullptr);
	}
}

} // namespace


void RegisterRoofAdapter ()
{
	RegisterAdapter ({ API_RoofID, CreateRoof, SerializeRoof, ModifyRoof });
}

} // namespace cc
