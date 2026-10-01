// *****************************************************************************
// ComplexElementsCurtainWall — CurtainWall adapter (API_CurtainWallID) and the
// serializers / modifiers of its sub-elements (segment, frame, panel, junction, accessory).
//
// Curtain wall create fields (meters, degrees):
//   path* (open polyline {points, arcs?} or point array; repeat the first point to close it) | begin + end,
//   height, bottomOffset (create only), flipped, zoneRelation, boundaryFramePosition,
//   primaryGrid / secondaryGrid {logic "FixedSizes"|"BestDivision"|"NumberOfDivisions", sizes [m], divisions,
//     origin "StartWithPattern"|"StartFromCenter"|"AlignToCenter"|"EndWithPattern", flexible [indices], endWith index},
//   primarySpacing / secondarySpacing (shorthand for {logic: FixedSizes, sizes: [value]}),
//   panelSurface, panelOuterSurface, panelInnerSurface, panelThickness, frameSurface, frameBuildingMaterial,
//   floorPlanDisplay, viewDepth, + common: layer, storyIndex, renovationStatus, elementId
// Modify: all of the above except path/begin/end/bottomOffset.
//
// CurtainWallPanel modify: outerSurface, innerSurface, cutSurface, thickness, buildingMaterial, classId, deleted.
// CurtainWallFrame modify: surface, buildingMaterial, classId.
// *****************************************************************************

#include "Commands/ComplexElementsCommon.hpp"

#include <cmath>

namespace cc {
namespace complex {

namespace {

const NamedValue kPatternLogic[] = {
	{ "FixedSizes",			APICWSePL_FixedSizes },
	{ "BestDivision",		APICWSePL_BestDivision },
	{ "NumberOfDivisions",	APICWSePL_NumberOfDivisions },
};

const NamedValue kGridOrigin[] = {
	{ "StartWithPattern",	APICWSeGridOrigin_StartWithPattern },
	{ "StartFromCenter",	APICWSeGridOrigin_StartFromCenter },
	{ "AlignToCenter",		APICWSeGridOrigin_AlignToCenter },
	{ "EndWithPattern",		APICWSeGridOrigin_EndWithPattern },
};

const NamedValue kBoundaryFramePos[] = {
	{ "Outside",	APICW_Boundary_OutSide },
	{ "Center",		APICW_Boundary_Center },
	{ "Inside",		APICW_Boundary_Inside },
	{ "Unknown",	APICW_Boundary_Unknown },
};

const NamedValue kSegmentTypes[] = {
	{ "Line",		APICWSeT_Line },
	{ "Arc",		APICWSeT_Arc },
	{ "Invalid",	APICWSeT_Invalid },
};

const NamedValue kFrameObjectTypes[] = {
	{ "Invisible",					APICWFrObjectType_Invisible },
	{ "GDL",						APICWFrObjectType_GDL },
	{ "InvisibleConnectedTurning",	APICWFrObjectType_InvisibleConnectedTurning },
	{ "InvisibleConnectedStill",	APICWFrObjectType_InvisibleConnectedStill },
};

const NamedValue kPanelObjectTypes[] = {
	{ "Generic",	APICWPaObjectType_Generic },
	{ "GDL",		APICWPaObjectType_GDL },
};

const NamedValue kZoneRels[] = {
	{ "Boundary",	APIZRel_Boundary },
	{ "ReduceArea",	APIZRel_ReduceArea },
	{ "None",		APIZRel_None },
};

const NamedValue kDisplayOptions[] = {
	{ "Projected",				API_Standard },
	{ "ProjectedWithOverhead",	API_StandardWithAbstract },
	{ "CutOnly",				API_CutOnly },
	{ "OutlinesOnly",			API_OutLinesOnly },
	{ "OverheadAll",			API_AbstractAll },
	{ "SymbolicCut",			API_CutAll },
	{ "Symbolic",				API_CutAll },
};

const NamedValue kViewDepth[] = {
	{ "ToFloorPlanRange",	API_ToFloorPlanRange },
	{ "ToAbsoluteLimit",	API_ToAbsoluteLimit },
	{ "EntireElement",		API_EntireElement },
};

constexpr UInt64 kCWClassPatternMask = APIMemoMask_CWSegPrimaryPattern | APIMemoMask_CWSegSecPattern |
									   APIMemoMask_CWSegPanelPattern | APIMemoMask_CWallFrameClasses |
									   APIMemoMask_CWallPanelClasses;

constexpr UInt64 kCWDetailsMask = APIMemoMask_Polygon | kCWClassPatternMask | APIMemoMask_CWallSegments |
								  APIMemoMask_CWallFrames | APIMemoMask_CWallPanels | APIMemoMask_CWallJunctions |
								  APIMemoMask_CWallAccessories;

constexpr UInt32 kMaxPatternItems = 1000;

// --- Grid patterns -----------------------------------------------------------------------

// Reads spec[key] (full grid object) or spec[spacingKey] (fixed module shorthand). Returns false when absent.
bool GetGridSpec (const OS& spec, const char* key, const char* spacingKey, OS& grid)
{
	const bool full = spec.Contains (key);
	const bool shorthand = spec.Contains (spacingKey);
	if (full && shorthand)
		Fail ("Give either '" + GS::UniString (key) + "' or '" + GS::UniString (spacingKey) + "', not both.");
	if (full) {
		grid = GetObject (spec, key);
		return true;
	}
	if (shorthand) {
		const double s = GetDouble (spec, spacingKey);
		GS::Array<double> sizes;
		sizes.Push (s);
		grid = OS ("logic", GS::UniString ("FixedSizes"), "sizes", sizes);
		return true;
	}
	return false;
}


void ReplaceUIntArray (UInt32*& target, UInt32& count, const GS::Array<UInt32>& values)
{
	KillPtr (target);
	count = (UInt32) values.GetSize ();
	target = AllocPtr<UInt32> (count);
	for (UInt32 i = 0; i < count; ++i)
		target[i] = values[i];
}


// Applies a grid spec to a segment pattern. Returns true when the number of pattern items changed.
bool ApplyGrid (const OS& grid, API_CWSegmentPatternData& pat, const GS::UniString& name)
{
	const UInt32 oldCount = pat.nPattern;
	const Int32 oldLogic = (Int32) pat.logic;
	const Int32 logic = grid.Contains ("logic") ? ParseNamed (kPatternLogic, grid, "logic") : oldLogic;

	if (grid.Contains ("sizes")) {
		const GS::Array<double> sizes = GetNumberArray (grid, "sizes", true);
		if (sizes.IsEmpty () || sizes.GetSize () > kMaxPatternItems)
			Fail (name + ".sizes needs 1..1000 module sizes (meters).");
		for (double s : sizes) {
			if (!(s >= 0.001) || !std::isfinite (s))
				Fail (name + ".sizes: every module size must be at least 0.001 m.");
		}
		KillPtr (pat.pattern);
		pat.nPattern = (UInt32) sizes.GetSize ();
		pat.pattern = AllocPtr<double> (pat.nPattern);
		for (UInt32 i = 0; i < pat.nPattern; ++i)
			pat.pattern[i] = sizes[i];
	}

	if (logic == APICWSePL_NumberOfDivisions) {
		if (grid.Contains ("divisions")) {
			const Int32 d = GetInt (grid, "divisions");
			if (d < 1 || d > 10000)
				Fail (name + ".divisions must be between 1 and 10000.");
			pat.nDivisions = (UInt32) d;
		} else if (oldLogic != APICWSePL_NumberOfDivisions) {
			Fail (name + ": logic 'NumberOfDivisions' needs 'divisions' (the number of equal parts).");
		}
		if (pat.nPattern == 0 || pat.pattern == nullptr) {
			KillPtr (pat.pattern);
			pat.nPattern = 1;
			pat.pattern = AllocPtr<double> (1);
			pat.pattern[0] = 1.0;
		}
		if (!grid.Contains ("flexible") && (oldLogic != APICWSePL_NumberOfDivisions || pat.nPattern != oldCount)) {
			GS::Array<UInt32> all;
			for (UInt32 i = 0; i < pat.nPattern; ++i) all.Push (i);
			ReplaceUIntArray (pat.flexibleIDs, pat.nFlexibleIDs, all);
		}
	} else {
		if (grid.Contains ("divisions"))
			Fail (name + ".divisions is only used with logic 'NumberOfDivisions'; give module 'sizes' for '" + NameOf (kPatternLogic, logic) + "'.");
		if (pat.nPattern == 0 || pat.pattern == nullptr || (oldLogic == APICWSePL_NumberOfDivisions && !grid.Contains ("sizes")))
			Fail (name + ": give 'sizes' (module sizes in meters) for logic '" + NameOf (kPatternLogic, logic) + "'.");
		if (!grid.Contains ("flexible") && (grid.Contains ("sizes") || logic != oldLogic)) {
			// Archicad rejects (APIERR_BADPARS) a re-sized pattern without any flexible module (verified
			// live); like its own defaults, the first module absorbs the remainder unless told otherwise.
			GS::Array<UInt32> first;
			first.Push (0);
			ReplaceUIntArray (pat.flexibleIDs, pat.nFlexibleIDs, first);
		}
	}

	if (grid.Contains ("flexible")) {
		GS::Array<UInt32> ids;
		for (double v : GetNumberArray (grid, "flexible", true)) {
			if (v < 0 || v >= (double) pat.nPattern || std::fabs (v - std::round (v)) > 1e-9)
				Fail (name + ".flexible: indices must be integers 0.." + GS::ValueToUniString ((Int32) pat.nPattern - 1) + " (0-based into sizes).");
			ids.Push ((UInt32) std::llround (v));
		}
		ReplaceUIntArray (pat.flexibleIDs, pat.nFlexibleIDs, ids);
	}

	if (grid.Contains ("endWith")) {
		const Int32 e = GetInt (grid, "endWith");
		if (e < 0 || e >= (Int32) pat.nPattern)
			Fail (name + ".endWith must be an index 0.." + GS::ValueToUniString ((Int32) pat.nPattern - 1) + " into sizes.");
		pat.endWithID = (UInt32) e;
	} else if (grid.Contains ("sizes") || pat.endWithID >= pat.nPattern) {
		pat.endWithID = pat.nPattern - 1;
	}

	if (grid.Contains ("origin"))
		pat.gridOriginType = (API_CWSegmentGridOrigPosTypeID) ParseNamed (kGridOrigin, grid, "origin");
	pat.logic = (API_CWSegmentPatternLogicID) logic;
	return pat.nPattern != oldCount;
}


// Makes the pattern cell table match primary x secondary pattern items (all cells copy the first cell).
void SyncPatternCells (API_ElementMemo& memo)
{
	const UInt32 nPrim = std::max<UInt32> (1, memo.cWSegPrimaryPattern.nPattern);
	const UInt32 nSec = std::max<UInt32> (1, memo.cWSegSecondaryPattern.nPattern);
	const UInt32 nCells = nPrim * nSec;
	const UInt32 oldCells = PtrCount (memo.cWSegPatternCells);
	if (oldCells == nCells)
		return;

	API_CWSegmentPatternCellData cell;
	BNZeroMemory (&cell, sizeof (cell));
	if (oldCells > 0) {
		cell = memo.cWSegPatternCells[0];
	} else {
		cell.crossingFrameType = APICWCFT_NoCrossingFrame;
		cell.leftPanelID = APICWPanelClass_FirstCustomClass;
		cell.rightPanelID = APICWPanelClass_FirstCustomClass;
		cell.leftFrameID = APICWFrameClass_Division;
		cell.bottomFrameID = APICWFrameClass_Division;
		cell.crossingFrameID = APICWFrameClass_Division;
	}
	API_CWSegmentPatternCellData* cells = AllocPtr<API_CWSegmentPatternCellData> (nCells);
	for (UInt32 i = 0; i < nCells; ++i)
		cells[i] = cell;
	KillPtr (memo.cWSegPatternCells);
	memo.cWSegPatternCells = cells;
}


OS PatternJson (const API_CWSegmentPatternData& pat)
{
	OS out;
	out.Add ("logic", NameOf (kPatternLogic, pat.logic));
	out.Add ("origin", NameOf (kGridOrigin, pat.gridOriginType));
	GS::Array<double> sizes;
	const UInt32 n = std::min (pat.nPattern, PtrCount (pat.pattern));
	for (UInt32 i = 0; i < n; ++i)
		sizes.Push (pat.pattern[i]);
	out.Add ("sizes", sizes);
	GS::Array<Int32> flexible;
	const UInt32 nf = std::min (pat.nFlexibleIDs, PtrCount (pat.flexibleIDs));
	for (UInt32 i = 0; i < nf; ++i)
		flexible.Push ((Int32) pat.flexibleIDs[i]);
	out.Add ("flexible", flexible);
	if (pat.logic == APICWSePL_NumberOfDivisions)
		out.Add ("divisions", (Int32) pat.nDivisions);
	out.Add ("endWith", (Int32) pat.endWithID);
	return out;
}

// --- Frame / panel classes -------------------------------------------------------------------

bool NeedsCWMemo (const OS& spec)
{
	static const char* keys[] = { "primaryGrid", "secondaryGrid", "primarySpacing", "secondarySpacing", "panelSurface",
								  "panelOuterSurface", "panelInnerSurface", "panelThickness", "frameSurface", "frameBuildingMaterial" };
	for (const char* k : keys) {
		if (spec.Contains (k))
			return true;
	}
	return false;
}


void SetFrameSurface (API_CWFrameType& frame, API_AttributeIndex surface)
{
	frame.material = surface;
	frame.useOwnMaterial = false;
}


// Applies grid + class fields to the memo (and the corner/boundary frame defaults of the struct).
void ApplyCWMemoFields (API_CurtainWallType& cw, API_Element* mask, API_ElementMemo& memo, const OS& spec)
{
	OS grid;
	bool counts = false;
	if (GetGridSpec (spec, "primaryGrid", "primarySpacing", grid))
		counts = ApplyGrid (grid, memo.cWSegPrimaryPattern, "primaryGrid") || counts;
	if (GetGridSpec (spec, "secondaryGrid", "secondarySpacing", grid))
		counts = ApplyGrid (grid, memo.cWSegSecondaryPattern, "secondaryGrid") || counts;
	SyncPatternCells (memo);

	const UInt32 nPanels = std::min (cw.nPanelDefaults, PtrCount (memo.cWallPanelDefaults));
	const bool panelChange = spec.Contains ("panelSurface") || spec.Contains ("panelOuterSurface") ||
							 spec.Contains ("panelInnerSurface") || spec.Contains ("panelThickness");
	if (panelChange && nPanels == 0)
		Fail ("This curtain wall has no panel classes to change.", APIERR_NOTSUPPORTED);
	double thickness = 0.0;
	const bool hasThickness = ApplyLength (spec, "panelThickness", thickness, true);
	for (UInt32 i = 0; i < nPanels; ++i) {
		API_CWPanelType& p = memo.cWallPanelDefaults[i];
		ApplyOverriddenSurface (spec, "panelSurface", p.outerSurfaceMaterial);
		ApplyOverriddenSurface (spec, "panelSurface", p.innerSurfaceMaterial);
		ApplyOverriddenSurface (spec, "panelSurface", p.cutSurfaceMaterial);
		ApplyOverriddenSurface (spec, "panelOuterSurface", p.outerSurfaceMaterial);
		ApplyOverriddenSurface (spec, "panelInnerSurface", p.innerSurfaceMaterial);
		if (hasThickness)
			p.thickness = thickness;
	}

	const UInt32 nFrames = std::min (cw.nFrameDefaults, PtrCount (memo.cWallFrameDefaults));
	if (auto surface = OptAttr (API_MaterialID, spec, "frameSurface")) {
		for (UInt32 i = 0; i < nFrames; ++i)
			SetFrameSurface (memo.cWallFrameDefaults[i], *surface);
		SetFrameSurface (cw.cornerFrameData, *surface);
		SetFrameSurface (cw.boundaryFrameData, *surface);
		if (mask != nullptr) {
			ACAPI_ELEMENT_MASK_SET (*mask, API_CurtainWallType, cornerFrameData);
			ACAPI_ELEMENT_MASK_SET (*mask, API_CurtainWallType, cornerFrameData.material);
			ACAPI_ELEMENT_MASK_SET (*mask, API_CurtainWallType, cornerFrameData.useOwnMaterial);
			ACAPI_ELEMENT_MASK_SET (*mask, API_CurtainWallType, boundaryFrameData);
			ACAPI_ELEMENT_MASK_SET (*mask, API_CurtainWallType, boundaryFrameData.material);
			ACAPI_ELEMENT_MASK_SET (*mask, API_CurtainWallType, boundaryFrameData.useOwnMaterial);
		}
	}
	if (auto bm = OptAttr (API_BuildingMaterialID, spec, "frameBuildingMaterial")) {
		for (UInt32 i = 0; i < nFrames; ++i)
			memo.cWallFrameDefaults[i].buildingMaterial = *bm;
		cw.cornerFrameData.buildingMaterial = *bm;
		cw.boundaryFrameData.buildingMaterial = *bm;
		if (mask != nullptr) {
			ACAPI_ELEMENT_MASK_SET (*mask, API_CurtainWallType, cornerFrameData);
			ACAPI_ELEMENT_MASK_SET (*mask, API_CurtainWallType, cornerFrameData.buildingMaterial);
			ACAPI_ELEMENT_MASK_SET (*mask, API_CurtainWallType, boundaryFrameData);
			ACAPI_ELEMENT_MASK_SET (*mask, API_CurtainWallType, boundaryFrameData.buildingMaterial);
		}
	}
	(void) counts;
}


void ApplyCWStructFields (API_CurtainWallType& cw, API_Element* mask, const OS& spec)
{
#define CW_SET(field) if (mask != nullptr) ACAPI_ELEMENT_MASK_SET (*mask, API_CurtainWallType, field)
	if (ApplyLength (spec, "height", cw.height, true))		CW_SET (height);
	if (ApplyFlag (spec, "flipped", cw.flipped))				CW_SET (flipped);
	if (Has (spec, "zoneRelation")) {
		cw.zoneRel = (API_ZoneRelID) ParseNamed (kZoneRels, spec, "zoneRelation");
		CW_SET (zoneRel);
	}
	if (Has (spec, "boundaryFramePosition")) {
		cw.boundaryFramePosition = (API_CWBoundaryFramePosID) ParseNamed (kBoundaryFramePos, spec, "boundaryFramePosition");
		CW_SET (boundaryFramePosition);
	}
	if (Has (spec, "floorPlanDisplay")) {
		cw.displayOption = (API_ElemDisplayOptionsID) ParseNamed (kDisplayOptions, spec, "floorPlanDisplay");
		CW_SET (displayOption);
	}
	if (Has (spec, "viewDepth")) {
		cw.viewDepthLimitation = (API_ElemViewDepthLimitationsID) ParseNamed (kViewDepth, spec, "viewDepth");
		CW_SET (viewDepthLimitation);
	}
#undef CW_SET
}


API_Guid CreateCurtainWall (const OS& spec)
{
	API_Element element = NewElement (API_CurtainWallID);
	Memo memo;
	GetDefaults (element, memo.Ptr ());
	ApplyCommonFields (element, nullptr, spec);
	API_CurtainWallType& cw = element.curtainWall;
	ApplyStoryCreationMode (cw.linkToSettings, spec);

	const Contour path = GetPath (spec, "path", true, 2);
	if (!path.z.IsEmpty ())
		Fail ("Curtain wall path points must be 2D {x, y}; use 'bottomOffset' for the base elevation.");
	WritePolylineToMemo (path, cw.polygon, *memo);
	cw.nSegments = (UInt32) path.points.GetSize () - 1;

	ApplyCWStructFields (cw, nullptr, spec);
	ApplyLength (spec, "bottomOffset", cw.storyRelLevel);
	ApplyCWMemoFields (cw, nullptr, *memo, spec);

	GSErrCode err = ACAPI_Element_Create (&element, memo.Ptr ());
	Check (err, "Cannot create curtain wall" +
		   GS::UniString (err == APIERR_BADPARS ? " (check the path: no self-intersections or zero-length segments; grid sizes must fit the wall)" : ""));
	return element.header.guid;
}


OS FrameClassJson (const API_CWFrameType& f, UInt32 classId)
{
	OS o;
	o.Add ("classId", (Int32) classId);
	o.Add ("name", GS::UniString (f.className));
	o.Add ("objectType", NameOf (kFrameObjectTypes, f.objectType));
	o.Add ("width", f.a1 + f.a2);
	o.Add ("depth", f.b1 + f.b2);
	o.Add ("surface", AttrRef (API_MaterialID, f.material));
	o.Add ("useOwnMaterial", f.useOwnMaterial);
	o.Add ("buildingMaterial", AttrRef (API_BuildingMaterialID, f.buildingMaterial));
	return o;
}


OS PanelClassJson (const API_CWPanelType& p, UInt32 classId)
{
	OS o;
	o.Add ("classId", (Int32) classId);
	o.Add ("name", GS::UniString (p.className));
	o.Add ("objectType", NameOf (kPanelObjectTypes, p.objectType));
	o.Add ("thickness", p.thickness);
	AddOverriddenSurfaceJson (o, "outerSurface", p.outerSurfaceMaterial);
	AddOverriddenSurfaceJson (o, "innerSurface", p.innerSurfaceMaterial);
	AddOverriddenSurfaceJson (o, "cutSurface", p.cutSurfaceMaterial);
	o.Add ("buildingMaterial", AttrRef (API_BuildingMaterialID, p.buildingMaterial));
	return o;
}


void SerializeCurtainWall (const API_Element& element, OS& out)
{
	const API_CurtainWallType& cw = element.curtainWall;
	out.Add ("height", cw.height);
	AddAngle (out, "angle", cw.angle);
	out.Add ("flipped", cw.flipped);
	out.Add ("offset", cw.offset);
	out.Add ("nominalWidth", cw.nominalWidth);
	out.Add ("distanceInside", cw.distanceInside);
	out.Add ("distanceOutside", cw.distanceOutside);
	out.Add ("boundaryFramePosition", NameOf (kBoundaryFramePos, cw.boundaryFramePosition));
	out.Add ("zoneRelation", NameOf (kZoneRels, cw.zoneRel));
	out.Add ("floorPlanDisplay", NameOf (kDisplayOptions, cw.displayOption));
	out.Add ("viewDepth", NameOf (kViewDepth, cw.viewDepthLimitation));
	out.Add ("basePlaneOrigin", Coord3DObj (cw.planeMatrix.tmx[3], cw.planeMatrix.tmx[7], cw.planeMatrix.tmx[11]));
	out.Add ("planeOffset", cw.planeOffset);
	out.Add ("counts", OS ("segments", (Int32) cw.nSegments, "frames", (Int32) cw.nFrames, "panels", (Int32) cw.nPanels,
						   "junctions", (Int32) cw.nJunctions, "accessories", (Int32) cw.nAccessories,
						   "frameClasses", (Int32) cw.nFrameDefaults, "panelClasses", (Int32) cw.nPanelDefaults));
	AddBoundingBox (out, element.header);

	Memo memo;
	if (ACAPI_Element_GetMemo (element.header.guid, memo.Ptr (), kCWDetailsMask) != NoError)
		return;
	const API_ElementMemo& m = *memo;

	if (m.coords != nullptr) {
		out.Add ("path", HandlePolylineJson (m.coords, m.parcs, cw.polygon.nCoords, cw.polygon.nArcs));
		out.Add ("length", HandlePathLength (m.coords, m.parcs, cw.polygon.nCoords, cw.polygon.nArcs));
	}
	out.Add ("primaryGrid", PatternJson (m.cWSegPrimaryPattern));
	out.Add ("secondaryGrid", PatternJson (m.cWSegSecondaryPattern));

	GS::Array<OS> segments;
	const UInt32 nSeg = PtrCount (m.cWallSegments);
	for (UInt32 i = 0; i < nSeg; ++i) {
		const API_CWSegmentType& s = m.cWallSegments[i];
		OS seg;
		seg.Add ("guid", GuidStr (s.head.guid));
		seg.Add ("type", NameOf (kSegmentTypes, s.segmentType));
		seg.Add ("begin", Coord3DObj (s.begC));
		seg.Add ("end", Coord3DObj (s.endC));
		if (s.segmentType == APICWSeT_Arc)
			seg.Add ("arcOrigin", Coord3DObj (s.arcOrigin));
		segments.Push (seg);
	}
	out.Add ("segments", segments);

	GS::Array<OS> frameClasses, panelClasses;
	const UInt32 nFC = std::min (cw.nFrameDefaults, PtrCount (m.cWallFrameDefaults));
	for (UInt32 i = 0; i < nFC; ++i)
		frameClasses.Push (FrameClassJson (m.cWallFrameDefaults[i], APICWFrameClass_FirstCustomClass + i));
	const UInt32 nPC = std::min (cw.nPanelDefaults, PtrCount (m.cWallPanelDefaults));
	for (UInt32 i = 0; i < nPC; ++i)
		panelClasses.Push (PanelClassJson (m.cWallPanelDefaults[i], APICWPanelClass_FirstCustomClass + i));
	out.Add ("frameClasses", frameClasses);
	out.Add ("panelClasses", panelClasses);

	bool truncated = false;
	out.Add ("frames", HeadGuids (m.cWallFrames, PtrCount (m.cWallFrames), kMaxListedParts, truncated));
	out.Add ("panels", HeadGuids (m.cWallPanels, PtrCount (m.cWallPanels), kMaxListedParts, truncated));
	out.Add ("junctions", HeadGuids (m.cWallJunctions, PtrCount (m.cWallJunctions), kMaxListedParts, truncated));
	out.Add ("accessories", HeadGuids (m.cWallAccessories, PtrCount (m.cWallAccessories), kMaxListedParts, truncated));
	if (truncated)
		out.Add ("partsTruncated", GS::UniString ("Only the first 1000 GUIDs per kind are listed; use get_subelements for all parts."));
}


void ModifyCurtainWall (API_Element& element, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask, const OS& patch)
{
	if (Has (patch, "path") || Has (patch, "begin") || Has (patch, "end"))
		Fail ("The base line of an existing curtain wall cannot be changed through the API. Use move_elements / rotate_elements, "
			  "or delete it and create a new one with create_curtain_walls.", APIERR_NOTSUPPORTED);
	if (Has (patch, "bottomOffset"))
		Fail ("'bottomOffset' can only be set when creating a curtain wall; move it vertically with elevate_elements / move_elements (z).",
			  APIERR_NOTSUPPORTED);
	API_CurtainWallType& cw = element.curtainWall;
	ApplyCWStructFields (cw, &mask, patch);
	if (NeedsCWMemo (patch)) {
		LoadMemo (element.header.guid, memo, kCWClassPatternMask);
		ApplyCWMemoFields (cw, &mask, memo, patch);
		memoMask |= kCWClassPatternMask;
		// Same as the DevKit's curtain wall edit: the class counts travel with the class tables.
		ACAPI_ELEMENT_MASK_SET (mask, API_CurtainWallType, nFrameDefaults);
		ACAPI_ELEMENT_MASK_SET (mask, API_CurtainWallType, nPanelDefaults);
	}
}

// --- Sub-elements --------------------------------------------------------------------------------

void SerializeCWSegment (const API_Element& element, OS& out)
{
	const API_CWSegmentType& s = element.cwSegment;
	AddOwner (out, s.owner);
	out.Add ("type", NameOf (kSegmentTypes, s.segmentType));
	out.Add ("begin", Coord3DObj (s.begC));
	out.Add ("end", Coord3DObj (s.endC));
	if (s.segmentType == APICWSeT_Arc) {
		out.Add ("arcOrigin", Coord3DObj (s.arcOrigin));
		out.Add ("negativeArc", s.negArc);
	}
	out.Add ("gridOrigin", Coord3DObj (s.gridOrigin));
	AddAngle (out, "gridAngle", s.gridAngle);
	out.Add ("extrusion", Coord3DObj (s.extrusion.x, s.extrusion.y, s.extrusion.z));
	out.Add ("patternCellCount", (Int32) s.patternCellNum);
	AddBoundingBox (out, element.header);
}


void SerializeCWFrame (const API_Element& element, OS& out)
{
	const API_CWFrameType& f = element.cwFrame;
	AddOwner (out, f.owner);
	out.Add ("classId", (Int32) f.classID);
	out.Add ("className", GS::UniString (f.className));
	out.Add ("objectType", NameOf (kFrameObjectTypes, f.objectType));
	out.Add ("segmentIndex", (Int32) f.segmentID);
	out.Add ("begin", Coord3DObj (f.begC));
	out.Add ("end", Coord3DObj (f.endC));
	out.Add ("length", std::sqrt ((f.endC.x - f.begC.x) * (f.endC.x - f.begC.x) + (f.endC.y - f.begC.y) * (f.endC.y - f.begC.y) +
								  (f.endC.z - f.begC.z) * (f.endC.z - f.begC.z)));
	out.Add ("width", f.a1 + f.a2);
	out.Add ("depth", f.b1 + f.b2);
	AddAngle (out, "angle", f.angle);
	out.Add ("surface", AttrRef (API_MaterialID, f.material));
	out.Add ("useOwnMaterial", f.useOwnMaterial);
	out.Add ("buildingMaterial", AttrRef (API_BuildingMaterialID, f.buildingMaterial));
	out.Add ("isContourFrame", f.contourID >= 0);
	out.Add ("hasSymbol", f.hasSymbol);
	AddBoundingBox (out, element.header);
}


void SerializeCWPanel (const API_Element& element, OS& out)
{
	const API_CWPanelType& p = element.cwPanel;
	AddOwner (out, p.owner);
	out.Add ("classId", (Int32) p.classID);
	out.Add ("className", GS::UniString (p.className));
	out.Add ("deleted", p.classID == APICWPanelClass_Deleted);
	out.Add ("hidden", p.hidden);
	out.Add ("objectType", NameOf (kPanelObjectTypes, p.objectType));
	out.Add ("segmentIndex", (Int32) p.segmentID);
	out.Add ("thickness", p.thickness);
	AddOverriddenSurfaceJson (out, "outerSurface", p.outerSurfaceMaterial);
	AddOverriddenSurfaceJson (out, "innerSurface", p.innerSurfaceMaterial);
	AddOverriddenSurfaceJson (out, "cutSurface", p.cutSurfaceMaterial);
	out.Add ("buildingMaterial", AttrRef (API_BuildingMaterialID, p.buildingMaterial));
	out.Add ("centroid", Coord3DObj (p.centroid));
	out.Add ("edgeCount", (Int32) p.edgesNum);
	AddBoundingBox (out, element.header);
}


void SerializeCWJunction (const API_Element& element, OS& out)
{
	const API_CWJunctionType& j = element.cwJunction;
	AddOwner (out, j.owner);
	out.Add ("category", GS::UniString (j.category == APICWJunC_Custom ? "Custom" : "System"));
	out.Add ("flipped", j.flipped);
	out.Add ("hasSymbol", j.hasSymbol);
}


void SerializeCWAccessory (const API_Element& element, OS& out)
{
	const API_CWAccessoryType& a = element.cwAccessory;
	AddOwner (out, a.owner);
	out.Add ("category", GS::UniString (a.category == APICWAccC_Custom ? "Custom" : "System"));
	out.Add ("flipped", a.flipped);
	if (a.refFrame != APINULLGuid)
		out.Add ("referenceFrame", GuidStr (a.refFrame));
}


void ModifyCWFrame (API_Element& element, API_Element& mask, API_ElementMemo&, UInt64&, const OS& patch)
{
	API_CWFrameType& f = element.cwFrame;
	bool customize = false;
	if (auto surface = OptAttr (API_MaterialID, patch, "surface")) {
		SetFrameSurface (f, *surface);
		ACAPI_ELEMENT_MASK_SET (mask, API_CWFrameType, material);
		ACAPI_ELEMENT_MASK_SET (mask, API_CWFrameType, useOwnMaterial);
		customize = true;
	}
	if (ApplyAttrField (patch, "buildingMaterial", API_BuildingMaterialID, f.buildingMaterial)) {
		ACAPI_ELEMENT_MASK_SET (mask, API_CWFrameType, buildingMaterial);
		customize = true;
	}
	if (auto cls = OptInt (patch, "classId")) {
		if (*cls < 0)
			Fail ("classId must be >= 0 (0 Merged, 1 Division, 2 Corner, 3 Boundary, 4+ = frameClasses[classId-4] of the curtain wall).");
		f.classID = (UInt32) *cls;
		ACAPI_ELEMENT_MASK_SET (mask, API_CWFrameType, classID);
	} else if (customize) {
		f.classID = APICWFrameClass_Customized;
		ACAPI_ELEMENT_MASK_SET (mask, API_CWFrameType, classID);
	}
}


void ModifyCWPanel (API_Element& element, API_Element& mask, API_ElementMemo&, UInt64&, const OS& patch)
{
	API_CWPanelType& p = element.cwPanel;
	if (auto del = OptBool (patch, "deleted")) {
		if (!*del)
			Fail ("To restore a deleted panel give 'classId' (1 = the first panel class of the curtain wall, see panelClasses).");
		p.classID = APICWPanelClass_Deleted;
		ACAPI_ELEMENT_MASK_SET (mask, API_CWPanelType, classID);
		return;
	}
	bool customize = false;
	if (ApplyOverriddenSurface (patch, "outerSurface", p.outerSurfaceMaterial)) {
		ACAPI_ELEMENT_MASK_SET (mask, API_CWPanelType, outerSurfaceMaterial);
		customize = true;
	}
	if (ApplyOverriddenSurface (patch, "innerSurface", p.innerSurfaceMaterial)) {
		ACAPI_ELEMENT_MASK_SET (mask, API_CWPanelType, innerSurfaceMaterial);
		customize = true;
	}
	if (ApplyOverriddenSurface (patch, "cutSurface", p.cutSurfaceMaterial)) {
		ACAPI_ELEMENT_MASK_SET (mask, API_CWPanelType, cutSurfaceMaterial);
		customize = true;
	}
	if (ApplyLength (patch, "thickness", p.thickness, true)) {
		ACAPI_ELEMENT_MASK_SET (mask, API_CWPanelType, thickness);
		customize = true;
	}
	if (ApplyAttrField (patch, "buildingMaterial", API_BuildingMaterialID, p.buildingMaterial)) {
		ACAPI_ELEMENT_MASK_SET (mask, API_CWPanelType, buildingMaterial);
		customize = true;
	}
	if (auto cls = OptInt (patch, "classId")) {
		if (*cls < 0)
			Fail ("classId must be >= 0 (0 = deleted panel, 1+ = panelClasses[classId-1] of the curtain wall).");
		p.classID = (UInt32) *cls;
		ACAPI_ELEMENT_MASK_SET (mask, API_CWPanelType, classID);
	} else if (customize) {
		p.classID = APICWPanelClass_Customized;
		ACAPI_ELEMENT_MASK_SET (mask, API_CWPanelType, classID);
	}
}

} // namespace


void RegisterCurtainWallFamily ()
{
	RegisterAdapter ({ API_CurtainWallID, CreateCurtainWall, SerializeCurtainWall, ModifyCurtainWall });
	RegisterAdapter ({ API_CurtainWallSegmentID, nullptr, SerializeCWSegment, nullptr });
	RegisterAdapter ({ API_CurtainWallFrameID, nullptr, SerializeCWFrame, ModifyCWFrame });
	RegisterAdapter ({ API_CurtainWallPanelID, nullptr, SerializeCWPanel, ModifyCWPanel });
	RegisterAdapter ({ API_CurtainWallJunctionID, nullptr, SerializeCWJunction, nullptr });
	RegisterAdapter ({ API_CurtainWallAccessoryID, nullptr, SerializeCWAccessory, nullptr });
}

} // namespace complex
} // namespace cc
