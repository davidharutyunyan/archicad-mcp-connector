// *****************************************************************************
// SlabsRoofs — Slab adapter + registration of the Roof / Shell / Mesh adapters
// (SlabsRoofsRoof.cpp, SlabsRoofsShell.cpp, SlabsRoofsMesh.cpp).
//
// Slab create / modify fields (meters, degrees):
//   polygon* {points, arcs?, holes?}       (* required on create)
//   thickness, level (reference plane elevation from the home story),
//   referencePlane "Top"|"CoreTop"|"CoreBottom"|"Bottom",
//   buildingMaterial | composite, topSurface / bottomSurface / sideSurface (false = remove override),
//   edgeTrim "Vertical"|"Perpendicular"|"CustomAngle", edgeAngle (deg) — applied to ALL edges
//     (Archicad resets Horizontal / AlignWithCut slab edges to Vertical, so they are rejected),
//   edges [{contour?, index, trim?, angle?, surface?}] — per-edge overrides,
//   contourPen, contourLineType, cutContourPen, cutContourLineType, hiddenContourPen, hiddenContourLineType,
//   cutFillPen, cutFillBackgroundPen (false = no override), showCoverFill, coverFill, coverFillPen,
//   coverFillBackgroundPen, coverFillFromSurface, storyVisibility {...}
//   + common: layer, storyIndex, renovationStatus, elementId
//
// Polygon / edge changes on existing slabs go through Archicad's polygon primitives +
// ACAPI_Element_ChangeMemo (the only reliable way when the vertex count changes).
// *****************************************************************************

#include "Commands/Commands.hpp"
#include "Commands/SlabsRoofsCommon.hpp"
#include "Core/Command.hpp"

namespace cc {

namespace {

using namespace slabroof;

const NamedValue kSlabRefPlanes[] = {
	{ "Top",		APISlabRefPlane_Top },
	{ "CoreTop",	APISlabRefPlane_CoreTop },
	{ "CoreBottom",	APISlabRefPlane_CoreBottom },
	{ "Bottom",		APISlabRefPlane_Bottom },
};

constexpr UInt64 kSlabPolygonMemoMask = APIMemoMask_Polygon | APIMemoMask_EdgeTrims | APIMemoMask_SideMaterials;


API_EdgeTrim VerticalTrim ()
{
	API_EdgeTrim t;
	BNZeroMemory (&t, sizeof (t));
	t.sideType = APIEdgeTrim_Vertical;
	t.sideAngle = kPi / 2.0;
	return t;
}


// The trim used by most edges (the "slab-wide" trim shown as edgeTrim/edgeAngle).
API_EdgeTrim DominantTrim (const API_ElementMemo& memo)
{
	API_EdgeTrim best = VerticalTrim ();
	if (memo.edgeTrims == nullptr || memo.pends == nullptr)
		return best;
	const Int32 nTrims = HandleCount (reinterpret_cast<GSConstHandle> (memo.edgeTrims), sizeof (API_EdgeTrim));
	GS::Array<API_EdgeTrim> kinds;
	GS::Array<Int32> counts;
	for (const ContourRange& c : ContoursOf (memo.pends)) {
		for (Int32 i = 0; i < c.count; ++i) {
			const Int32 j = c.first + i;
			if (j >= nTrims)
				continue;
			bool found = false;
			for (UIndex k = 0; k < kinds.GetSize (); ++k) {
				if (SameTrim (kinds[k], (*memo.edgeTrims)[j])) { ++counts[k]; found = true; break; }
			}
			if (!found) { kinds.Push ((*memo.edgeTrims)[j]); counts.Push (1); }
		}
	}
	Int32 bestCount = 0;
	for (UIndex k = 0; k < kinds.GetSize (); ++k) {
		if (counts[k] > bestCount) { bestCount = counts[k]; best = kinds[k]; }
	}
	return best;
}


// Archicad keeps only Vertical and CustomAngle edges on (flat) slabs: Perpendicular is stored as
// Vertical (identical geometry), Horizontal / AlignWithCut are silently reset to Vertical (verified live).
void CheckSlabTrimValue (const OS& spec, const char* key, const GS::UniString& where)
{
	if (!Has (spec, key))
		return;
	const Int32 t = ParseNamed (kEdgeTrims, spec, key);
	if (t == APIEdgeTrim_Horizontal || t == APIEdgeTrim_AlignWithCut)
		Fail (where + "'" + GS::UniString (key) + "' = " + NameOf (kEdgeTrims, t) +
			  " is not available for slab edges: use Vertical, Perpendicular (same as Vertical on a flat slab) or "
			  "CustomAngle with an angle in degrees (90 = vertical). Horizontal / AlignWithCut exist only for roofs and shells.");
}


void CheckSlabTrims (const OS& spec)
{
	CheckSlabTrimValue (spec, "edgeTrim", GS::UniString ());
	const GS::Array<OS> items = GetObjectArray (spec, "edges", false);
	for (UIndex k = 0; k < items.GetSize (); ++k)
		CheckSlabTrimValue (items[k], "trim", GS::UniString::Printf ("edges[%u].", (unsigned) k));
}


bool NeedsEdgeRewrite (const OS& spec)
{
	return Has (spec, "polygon") || Has (spec, "edgeTrim") || Has (spec, "edgeAngle") ||
		   Has (spec, "sideSurface") || Has (spec, "edges");
}


void ApplySlabFields (API_Element& element, API_Element* mask, const OS& spec)
{
	API_SlabType& slab = element.slab;
	const FieldMask m (element, mask);
	CheckSlabTrims (spec);

	if (ApplyLength (spec, "thickness", slab.thickness, true))		m.Set (&slab.thickness);
	if (ApplyLength (spec, "level", slab.level))					m.Set (&slab.level);
	if (Has (spec, "referencePlane")) {
		slab.referencePlaneLocation = (API_SlabReferencePlaneLocationID) ParseNamed (kSlabRefPlanes, spec, "referencePlane");
		m.Set (&slab.referencePlaneLocation);
	}
	if (Has (spec, "profile"))
		Fail ("Slabs cannot use complex profiles; use 'buildingMaterial' or 'composite'.", APIERR_NOTSUPPORTED);
	if (ApplyStructure (spec, slab.modelElemStructureType, slab.buildingMaterial, slab.composite, nullptr)) {
		CheckCompositeUsage (spec, slab.composite, APICWall_ForSlab, "slabs");
		m.Set (&slab.modelElemStructureType);
		m.Set (&slab.buildingMaterial);
		m.Set (&slab.composite);
	}
	if (ApplyOverriddenSurface (spec, "topSurface", slab.topMat))		m.Set (&slab.topMat);
	if (ApplyOverriddenSurface (spec, "bottomSurface", slab.botMat))	m.Set (&slab.botMat);
	if (ApplyOverriddenSurface (spec, "sideSurface", slab.sideMat))		m.Set (&slab.sideMat);
	if (ApplyFlag (spec, "surfacesChained", slab.materialsChained))		m.Set (&slab.materialsChained);

	if (ApplyPen (spec, "contourPen", slab.pen))								m.Set (&slab.pen);
	if (ApplyAttr (spec, "contourLineType", API_LinetypeID, slab.ltypeInd))	m.Set (&slab.ltypeInd);
	if (ApplyPen (spec, "cutContourPen", slab.sectContPen))					m.Set (&slab.sectContPen);
	if (ApplyAttr (spec, "cutContourLineType", API_LinetypeID, slab.sectContLtype))	m.Set (&slab.sectContLtype);
	if (ApplyPen (spec, "hiddenContourPen", slab.hiddenContourLinePen))		m.Set (&slab.hiddenContourLinePen);
	if (ApplyAttr (spec, "hiddenContourLineType", API_LinetypeID, slab.hiddenContourLineType))	m.Set (&slab.hiddenContourLineType);
	if (ApplyPenOverride (spec, slab.penOverride))							m.Set (&slab.penOverride);
	if (ApplyFlag (spec, "showCoverFill", slab.useFloorFill))				m.Set (&slab.useFloorFill);
	if (ApplyAttr (spec, "coverFill", API_FilltypeID, slab.floorFillInd))	m.Set (&slab.floorFillInd);
	if (ApplyPen (spec, "coverFillPen", slab.floorFillPen))					m.Set (&slab.floorFillPen);
	if (ApplyPen (spec, "coverFillBackgroundPen", slab.floorFillBGPen))		m.Set (&slab.floorFillBGPen);
	if (ApplyFlag (spec, "coverFillFromSurface", slab.use3DHatching))		m.Set (&slab.use3DHatching);
	if (ApplyStoryVisibility (spec, slab.visibilityCont, slab.visibilityFill)) {
		m.Set (&slab.visibilityCont);
		m.Set (&slab.visibilityFill);
	}
}


GS::UniString CreateErrorHint (GSErrCode err)
{
	if (err == APIERR_IRREGULARPOLY || err == APIERR_BADPOLY)
		return " (the polygon is irregular: self-intersecting, repeated/collinear points, zero-length edges, or holes that are not fully inside the outline — fix the points)";
	return GS::UniString ();
}


API_Guid CreateSlab (const OS& spec)
{
	if (!Has (spec, "polygon"))
		Fail ("Slab requires 'polygon' (at least 3 points, meters).");

	API_Element element = NewElement (API_SlabID);
	GetDefaults (element, nullptr);
	ApplyCommonFields (element, nullptr, spec);
	ApplySlabFields (element, nullptr, spec);

	const PolygonData polygon = GetPolygon (spec, "polygon");
	const GS::Array<bool> reversed = ReversedContours (polygon);

	Memo memo;
	WritePolygonToMemo (polygon, element.slab.poly, *memo);

	API_EdgeTrim trim = VerticalTrim ();
	ApplyEdgeTrim (spec, trim);
	AllocEdgeData (*memo, element.slab.poly.nCoords, trim, element.slab.sideMat);
	ApplyEdgeOverrides (spec, *memo, &reversed, false);

	GSErrCode err = ACAPI_Element_Create (&element, memo.Ptr ());
	if (err == APIERR_IRREGULARPOLY && RegularizeMemoPolygon (*memo, element.slab.poly, "Slab"))
		err = ACAPI_Element_Create (&element, memo.Ptr ());
	Check (err, "Cannot create slab" + CreateErrorHint (err));
	return element.header.guid;
}


void SerializeSlab (const API_Element& element, OS& out)
{
	const API_SlabType& slab = element.slab;
	out.Add ("thickness", slab.thickness);
	out.Add ("level", slab.level);
	out.Add ("referencePlane", NameOf (kSlabRefPlanes, slab.referencePlaneLocation));
	out.Add ("offsetFromTop", slab.offsetFromTop);
	AddStructureJson (out, slab.modelElemStructureType, slab.buildingMaterial, slab.composite, 0);
	AddOverriddenSurfaceJson (out, "topSurface", slab.topMat);
	AddOverriddenSurfaceJson (out, "bottomSurface", slab.botMat);
	AddOverriddenSurfaceJson (out, "sideSurface", slab.sideMat);
	out.Add ("surfacesChained", slab.materialsChained);

	Memo memo;
	if (ACAPI_Element_GetMemo (element.header.guid, memo.Ptr (), kSlabPolygonMemoMask) == NoError) {
		const API_Polygon counts = PolygonCounts (memo->coords, memo->pends, memo->parcs);
		out.Add ("polygon", PolygonToJson (counts, *memo));
		const API_EdgeTrim trim = DominantTrim (*memo);
		AddEdgeTrimJson (out, trim);
		AddEdgesJson (out, *memo, trim, slab.sideMat, false);
	}

	out.Add ("contourPen", (Int32) slab.pen);
	out.Add ("contourLineType", AttrRef (API_LinetypeID, slab.ltypeInd));
	out.Add ("cutContourPen", (Int32) slab.sectContPen);
	out.Add ("cutContourLineType", AttrRef (API_LinetypeID, slab.sectContLtype));
	out.Add ("hiddenContourPen", (Int32) slab.hiddenContourLinePen);
	out.Add ("hiddenContourLineType", AttrRef (API_LinetypeID, slab.hiddenContourLineType));
	AddPenOverrideJson (out, slab.penOverride);
	out.Add ("showCoverFill", slab.useFloorFill);
	out.Add ("coverFill", AttrRef (API_FilltypeID, slab.floorFillInd));
	out.Add ("coverFillPen", (Int32) slab.floorFillPen);
	out.Add ("coverFillBackgroundPen", (Int32) slab.floorFillBGPen);
	out.Add ("coverFillFromSurface", slab.use3DHatching);
	out.Add ("storyVisibility", StoryVisibilityJson (slab.visibilityCont));
}


void ModifySlab (API_Element& element, API_Element& mask, API_ElementMemo&, UInt64&, const OS& patch)
{
	ApplySlabFields (element, &mask, patch);
	if (!NeedsEdgeRewrite (patch))
		return;

	// Struct changes first (e.g. the new sideSurface), then the memo (polygon + edge data).
	CommitStructChange (element, mask, "Cannot modify slab " + GuidStr (element.header.guid));
	API_Guid guid = element.header.guid;

	Memo memo;
	LoadMemo (guid, *memo, APIMemoMask_All);

	const bool polygonChange = Has (patch, "polygon");
	GS::Array<bool> reversed;
	API_EdgeTrim trim = DominantTrim (*memo);
	if (polygonChange) {
		const PolygonData polygon = GetPolygon (patch, "polygon");
		reversed = ReversedContours (polygon);
		ReshapePolygonInPlace (*memo, polygon);
	}

	const API_Polygon counts = PolygonCounts (memo->coords, memo->pends, memo->parcs);
	const bool trimGiven = ApplyEdgeTrim (patch, trim);
	if (polygonChange || !HasEdgeData (*memo, counts.nCoords)) {
		// New polygon: every edge gets the slab-wide trim and side surface (per-edge data is reset).
		AllocEdgeData (*memo, counts.nCoords, trim, element.slab.sideMat);
	} else {
		for (const ContourRange& c : ContoursOf (memo->pends)) {
			for (Int32 i = 0; i <= c.count; ++i) {
				if (trimGiven)
					(*memo->edgeTrims)[c.first + i] = trim;
				if (Has (patch, "sideSurface"))
					memo->sideMaterials[c.first + i] = element.slab.sideMat;
			}
		}
	}
	ApplyEdgeOverrides (patch, *memo, polygonChange ? &reversed : nullptr, false);
	SyncClosingEdgeData (*memo);

	GSErrCode err = ACAPI_Element_ChangeMemo (guid, kSlabPolygonMemoMask, memo.Ptr ());
	Check (err, "Cannot change the polygon/edges of slab " + GuidStr (guid) + CreateErrorHint (err));
}

} // namespace


void RegisterSlabAdapter ()
{
	RegisterAdapter ({ API_SlabID, CreateSlab, SerializeSlab, ModifySlab });
}


void RegisterSlabRoofCommands ()
{
	RegisterSlabAdapter ();
	RegisterRoofAdapter ();
	RegisterShellAdapter ();
	RegisterMeshAdapter ();
}

} // namespace cc
