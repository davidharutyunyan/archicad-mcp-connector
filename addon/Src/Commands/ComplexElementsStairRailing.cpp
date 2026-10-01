// *****************************************************************************
// ComplexElementsStairRailing — Stair (API_StairID) and Railing (API_RailingID) adapters
// plus serializers of their sub-elements (risers, treads, stair structures, railing parts).
//
// Stair create fields (meters, degrees):
//   baseline* (open polyline {points, arcs?} | point array) | begin + end (straight stair),
//   baselinePosition "Left"|"Center"|"Right"|"Auto" (default Center), baselineOffset,
//   height (total height, unlinks the top unless topLinkedStory is given), topLinkedStory, topOffset,
//   width (flight width), riserHeight, riserCount (height == riserCount x riserHeight is always kept:
//   any two define the third; height alone keeps the riser height as close as possible),
//   treadDepth, treadDepthLocked, totalHeightLocked,
//   walkingLinePosition, walkingLineOffset, direction "Upward"|"Inverse", numbering "Treads"|"Risers",
//   extraTopTread, extraBottomTread, treadThickness, riserThickness, riserCrossSection "Simple"|"Slanted",
//   riserAngle, ignoreRules (true = switch off the stair rule checks), + common fields.
// Modify: the same fields except the baseline.
//
// Railing create fields:
//   path* (open polyline; points may carry z = height of the node above the railing base) | begin + end,
//   height (rail height of every segment), bottomOffset, referenceLine "Left"|"Center"|"Right",
//   offset (horizontal offset of the segments from the reference line), postOffset,
//   referenceLinePen, contourPen, + common fields.
// Modify: the same fields except the path.
// *****************************************************************************

#include "Commands/ComplexElementsCommon.hpp"

#include <cmath>

namespace cc {
namespace complex {

namespace {

const NamedValue kLinePositions[] = {
	{ "Left",	APILP_Left },
	{ "Right",	APILP_Right },
	{ "Center",	APILP_Center },
	{ "Auto",	APILP_Auto },
};

const NamedValue kDirections[] = {
	{ "Upward",		APISD_Upward },
	{ "Inverse",	APISD_Inverse },
};

const NamedValue kNumbering[] = {
	{ "Treads",	APISN_Treads },
	{ "Risers",	APISN_Risers },
};

const NamedValue kStructTypes[] = {
	{ "Side",			APIST_Side },
	{ "Monolith",		APIST_Monolith },
	{ "BeamSupport",	APIST_BeamSupport },
	{ "Cantilevered",	APIST_CantileveredSupport },
};

const NamedValue kStructSides[] = {
	{ "Left",	APISS_LeftSide },
	{ "Right",	APISS_RightSide },
	{ "Under",	APISS_UnderSide },
};

const NamedValue kStructBottoms[] = {
	{ "Stepped",	APISB_Stepped },
	{ "Flat",		APISB_Flat },
	{ "Smooth",		APISB_Smooth },
};

const NamedValue kRoles[] = {
	{ "Run",		APISR_Run },
	{ "Landing",	APISR_Landing },
};

const NamedValue kRiserCross[] = {
	{ "Simple",		APIRCS_Simple },
	{ "Slanted",	APIRCS_Slanted },
};

const NamedValue kRiserPositions[] = {
	{ "OnTread",		APIRP_OnTread },
	{ "BehindTread",	APIRP_BehindTread },
};

const NamedValue kNosing[] = {
	{ "ByValue",	APITN_ByValue },
	{ "BySlanting",	APITN_BySlanting },
};

const NamedValue kRailRefLines[] = {
	{ "Left",	APIRLL_Left },
	{ "Center",	APIRLL_Center },
	{ "Right",	APIRLL_Right },
};

const NamedValue kDistribution[] = {
	{ "Divisions",				APIDT_Divisions },
	{ "PatternLength",			APIDT_PatternLength },
	{ "BestDivisionByLength",	APIDT_BestDivisionByLength },
};

// ============================================================================================
// Stair
// ============================================================================================

void ApplyStairFields (API_StairType& s, API_Element* mask, const OS& spec)
{
#define ST_SET(field) if (mask != nullptr) ACAPI_ELEMENT_MASK_SET (*mask, API_StairType, field)

	const bool hasHeight = ApplyLength (spec, "height", s.totalHeight, true);
	if (auto v = OptInt (spec, "topLinkedStory")) {
		if (*v < 0)
			Fail ("topLinkedStory must be >= 0 (0 = not linked, 1 = the story above, ...).");
		s.relativeTopStory = (short) *v;
		ST_SET (relativeTopStory);
	}
	if (ApplyLength (spec, "topOffset", s.topOffset))					ST_SET (topOffset);
	if (ApplyLength (spec, "width", s.flightWidth, true))				ST_SET (flightWidth);

	// Keep totalHeight == stepNum x riserHeight whatever combination of the three is given.
	auto riserCount = OptInt (spec, "riserCount");
	if (riserCount.has_value () && (*riserCount < 1 || *riserCount > 1000))
		Fail ("riserCount must be between 1 and 1000.");
	double riserHeight = 0.0;
	const bool hasRiserHeight = ApplyLength (spec, "riserHeight", riserHeight, true);
	auto countFor = [] (double total, double riser) -> UInt32 {
		const double n = std::round (total / riser);
		if (!(n >= 1.0 && n <= 1000.0))
			Fail ("height / riserHeight gives an impossible number of risers (must be 1..1000); check both values (meters).");
		return (UInt32) n;
	};
	bool heightChanged = hasHeight;
	if (riserCount.has_value () && hasRiserHeight && hasHeight) {
		if (std::fabs ((double) *riserCount * riserHeight - s.totalHeight) > 1e-4)
			Fail ("height, riserCount and riserHeight disagree (height must equal riserCount x riserHeight); give only two of them.");
		s.stepNum = (UInt32) *riserCount;
		s.riserHeight = riserHeight;
	} else if (riserCount.has_value () && hasRiserHeight) {
		s.stepNum = (UInt32) *riserCount;
		s.riserHeight = riserHeight;
		s.totalHeight = s.stepNum * riserHeight;
		heightChanged = true;
	} else if (riserCount.has_value ()) {
		s.stepNum = (UInt32) *riserCount;
		if (s.totalHeight > 0.0)
			s.riserHeight = s.totalHeight / (double) s.stepNum;
	} else if (hasRiserHeight) {
		if (s.totalHeight <= 0.0)
			Fail ("The stair has no total height yet: give 'height' together with 'riserHeight'.");
		s.stepNum = countFor (s.totalHeight, riserHeight);
		s.riserHeight = s.totalHeight / (double) s.stepNum;		// rounded to fit the height exactly
	} else if (hasHeight) {
		// Only the height changes: keep the current riser height as closely as possible.
		const double current = s.riserHeight > 0.05 ? s.riserHeight : 0.175;
		s.stepNum = countFor (s.totalHeight, current);
		s.riserHeight = s.totalHeight / (double) s.stepNum;
	}
	if (riserCount.has_value () || hasRiserHeight || hasHeight) {
		ST_SET (stepNum);
		ST_SET (riserHeight);
	}
	if (heightChanged) {
		ST_SET (totalHeight);
		if (!spec.Contains ("topLinkedStory")) { s.relativeTopStory = 0; ST_SET (relativeTopStory); }
	}
	if (ApplyLength (spec, "treadDepth", s.treadDepth, true)) {
		ST_SET (treadDepth);
		if (!spec.Contains ("treadDepthLocked")) { s.treadDepthLocked = true; ST_SET (treadDepthLocked); }
	}
	if (ApplyFlag (spec, "treadDepthLocked", s.treadDepthLocked))		ST_SET (treadDepthLocked);
	if (ApplyFlag (spec, "totalHeightLocked", s.totalHeightLocked))		ST_SET (totalHeightLocked);

	if (Has (spec, "baselinePosition")) {
		s.baselinePosition = (API_LinePositionID) ParseNamed (kLinePositions, spec, "baselinePosition");
		ST_SET (baselinePosition);
	}
	if (ApplyLength (spec, "baselineOffset", s.baselineOffset))		ST_SET (baselineOffset);
	if (Has (spec, "walkingLinePosition")) {
		s.walkingLinePosition = (API_LinePositionID) ParseNamed (kLinePositions, spec, "walkingLinePosition");
		ST_SET (walkingLinePosition);
	}
	if (ApplyLength (spec, "walkingLineOffset", s.walkingLineOffset))	ST_SET (walkingLineOffset);
	if (Has (spec, "direction")) {
		s.inputDirection = (API_StairDirectionID) ParseNamed (kDirections, spec, "direction");
		ST_SET (inputDirection);
	}
	if (Has (spec, "numbering")) {
		s.numberingType = (API_StairNumberingID) ParseNamed (kNumbering, spec, "numbering");
		ST_SET (numberingType);
	}
	if (ApplyFlag (spec, "extraTopTread", s.extraTopTread))			ST_SET (extraTopTread);
	if (ApplyFlag (spec, "extraBottomTread", s.extraBottomTread))		ST_SET (extraBottomTread);

	double v = 0.0;
	if (ApplyLength (spec, "treadThickness", v, true)) {
		s.tread[APISR_Run].thickness = v;
		s.tread[APISR_Landing].thickness = v;
		ST_SET (tread[APISR_Run].thickness);
		ST_SET (tread[APISR_Landing].thickness);
	}
	if (ApplyLength (spec, "riserThickness", v, true)) {
		s.riser[APISR_Run].thickness = v;
		s.riser[APISR_Landing].thickness = v;
		ST_SET (riser[APISR_Run].thickness);
		ST_SET (riser[APISR_Landing].thickness);
	}
	if (Has (spec, "riserCrossSection")) {
		const API_RiserCrossSectID cs = (API_RiserCrossSectID) ParseNamed (kRiserCross, spec, "riserCrossSection");
		s.riser[APISR_Run].crossSect = cs;
		s.riser[APISR_Landing].crossSect = cs;
		ST_SET (riser[APISR_Run].crossSect);
		ST_SET (riser[APISR_Landing].crossSect);
	}
	if (ApplyAngleField (spec, "riserAngle", v)) {
		s.riser[APISR_Run].angle = v;
		s.riser[APISR_Landing].angle = v;
		ST_SET (riser[APISR_Run].angle);
		ST_SET (riser[APISR_Landing].angle);
	}

	if (auto ignore = OptBool (spec, "ignoreRules")) {
		if (*ignore) {
			API_StairRulesData& r = s.rules;
			r.riserHeightMinApplied = r.riserHeightMaxApplied = false;
			r.treadDepthMinApplied = r.treadDepthMaxApplied = false;
			r.ruleMinApplied = r.ruleMaxApplied = false;
			r.landingLengthMinApplied = r.walkingLineOffsetMinApplied = r.winderTurnOffsetMinApplied = false;
			r.riserGoingRatioMinApplied = r.riserGoingRatioMaxApplied = false;
			r.riserPlusGoingMinApplied = r.riserPlusGoingMaxApplied = false;
			r.stairPitchMinApplied = r.stairPitchMaxApplied = false;
			ST_SET (rules.riserHeightMinApplied);		ST_SET (rules.riserHeightMaxApplied);
			ST_SET (rules.treadDepthMinApplied);		ST_SET (rules.treadDepthMaxApplied);
			ST_SET (rules.ruleMinApplied);				ST_SET (rules.ruleMaxApplied);
			ST_SET (rules.landingLengthMinApplied);		ST_SET (rules.walkingLineOffsetMinApplied);
			ST_SET (rules.winderTurnOffsetMinApplied);
			ST_SET (rules.riserGoingRatioMinApplied);	ST_SET (rules.riserGoingRatioMaxApplied);
			ST_SET (rules.riserPlusGoingMinApplied);	ST_SET (rules.riserPlusGoingMaxApplied);
			ST_SET (rules.stairPitchMinApplied);		ST_SET (rules.stairPitchMaxApplied);
		}
	}
#undef ST_SET
}


// Replaces the stair base line in the memo (all related handles/pointers are released first).
void SetStairBaseline (API_StairPolylineData& baseline, const Contour& path)
{
	Memo tmp;
	API_Polygon poly;
	BNZeroMemory (&poly, sizeof (poly));
	WritePolylineToMemo (path, poly, *tmp);

	KillHdl (baseline.coords);
	KillHdl (baseline.pends);
	KillHdl (baseline.parcs);
	KillPtr (baseline.edgeData);
	KillPtr (baseline.vertexData);

	baseline.coords = tmp->coords;	tmp->coords = nullptr;
	baseline.pends = tmp->pends;	tmp->pends = nullptr;
	baseline.parcs = tmp->parcs;	tmp->parcs = nullptr;
	baseline.polygon = poly;
}


GS::UniString StairCreateHint (GSErrCode err)
{
	if (err == NoError)
		return GS::UniString ();
	return " (Archicad could not generate the stair: check that the baseline is long enough for riserCount x treadDepth, that "
		   "riserHeight/treadDepth satisfy the stair rules — or pass ignoreRules: true — and that the baseline does not self-intersect)";
}


API_Guid CreateStair (const OS& spec)
{
	API_Element element = NewElement (API_StairID);
	Memo memo;
	GetDefaults (element, memo.Ptr ());
	ApplyCommonFields (element, nullptr, spec);
	API_StairType& stair = element.stair;
	ApplyStoryCreationMode (stair.linkToSettings, spec);

	const Contour baseline = GetPath (spec, "baseline", true, 2);
	if (!baseline.z.IsEmpty ())
		Fail ("Stair baseline points must be 2D {x, y}; the stair height comes from 'height'.");
	if (!spec.Contains ("baselinePosition"))
		stair.baselinePosition = APILP_Center;
	ApplyStairFields (stair, nullptr, spec);
	SetStairBaseline (memo->stairBaseLine, baseline);

	GSErrCode err = ACAPI_Element_Create (&element, memo.Ptr ());
	Check (err, "Cannot create stair" + StairCreateHint (err));
	return element.header.guid;
}


OS StairPolylineJson (const API_StairPolylineData& line)
{
	return HandlePolylineJson (line.coords, line.parcs, line.polygon.nCoords, line.polygon.nArcs);
}


OS StairBoundaryJson (const API_StairBoundaryData& b)
{
	return HandlePolylineJson (b.coords, b.parcs, b.polygon.nCoords, b.polygon.nArcs);
}


OS RulesJson (const API_StairRulesData& r)
{
	OS o;
	if (r.riserHeightMinApplied)		o.Add ("riserHeightMin", r.riserHeightMinValue);
	if (r.riserHeightMaxApplied)		o.Add ("riserHeightMax", r.riserHeightMaxValue);
	if (r.treadDepthMinApplied)			o.Add ("treadDepthMin", r.treadDepthMinValue);
	if (r.treadDepthMaxApplied)			o.Add ("treadDepthMax", r.treadDepthMaxValue);
	if (r.ruleMinApplied)				o.Add ("twoRisersPlusGoingMin", r.ruleMinValue);
	if (r.ruleMaxApplied)				o.Add ("twoRisersPlusGoingMax", r.ruleMaxValue);
	if (r.landingLengthMinApplied)		o.Add ("landingLengthMin", r.landingLengthMinValue);
	if (r.walkingLineOffsetMinApplied)	o.Add ("walkingLineOffsetMin", r.walkingLineOffsetMinValue);
	if (r.stairPitchMinApplied)			o.Add ("pitchMin", RadToDeg (r.stairPitchMinValue));
	if (r.stairPitchMaxApplied)			o.Add ("pitchMax", RadToDeg (r.stairPitchMaxValue));
	return o;
}


void SerializeStair (const API_Element& element, OS& out)
{
	const API_StairType& s = element.stair;
	out.Add ("height", s.totalHeight);
	out.Add ("topLinkedStory", (Int32) s.relativeTopStory);
	out.Add ("topOffset", s.topOffset);
	out.Add ("width", s.flightWidth);
	out.Add ("riserCount", (Int32) s.stepNum);
	out.Add ("treadCount", (Int32) s.treadNum);
	out.Add ("riserHeight", s.riserHeight);
	out.Add ("treadDepth", s.treadDepth);
	if (s.treadDepth > 1e-9)
		out.Add ("pitch", RadToDeg (std::atan2 (s.riserHeight, s.treadDepth)));
	out.Add ("treadDepthLocked", s.treadDepthLocked);
	out.Add ("totalHeightLocked", s.totalHeightLocked);
	out.Add ("baselinePosition", NameOf (kLinePositions, s.baselinePosition));
	out.Add ("baselineOffset", s.baselineOffset);
	out.Add ("walkingLinePosition", NameOf (kLinePositions, s.walkingLinePosition));
	out.Add ("walkingLineOffset", s.walkingLineOffset);
	out.Add ("direction", NameOf (kDirections, s.inputDirection));
	out.Add ("numbering", NameOf (kNumbering, s.numberingType));
	out.Add ("extraTopTread", s.extraTopTread);
	out.Add ("extraBottomTread", s.extraBottomTread);
	out.Add ("treadThickness", s.tread[APISR_Run].thickness);
	out.Add ("landingTreadThickness", s.tread[APISR_Landing].thickness);
	out.Add ("riserThickness", s.riser[APISR_Run].thickness);
	out.Add ("riserCrossSection", NameOf (kRiserCross, s.riser[APISR_Run].crossSect));
	AddAngle (out, "riserAngle", s.riser[APISR_Run].angle);
	out.Add ("rules", RulesJson (s.rules));

	GS::Array<OS> structures;
	for (Int32 role = 0; role < API_StairPartRoleNum; ++role) {
		for (Int32 side = 0; side < API_StairStructureSideNum; ++side) {
			const API_StairStructureType& st = s.structure[role][side];
			structures.Push (OS ("role", NameOf (kRoles, role), "side", NameOf (kStructSides, side),
								 "type", NameOf (kStructTypes, st.structType), "bottom", NameOf (kStructBottoms, st.bottomType),
								 "thickness", st.thickness, "horizontalThickness", st.horizontalThickness));
		}
	}
	out.Add ("structures", structures);
	out.Add ("basePoint", Coord3DObj (s.basePlane.basePoint));
	AddBoundingBox (out, element.header);

	Memo memo;
	if (ACAPI_Element_GetMemo (element.header.guid, memo.Ptr (), APIMemoMask_All) != NoError)
		return;
	const API_ElementMemo& m = *memo;
	if (m.stairBaseLine.coords != nullptr) {
		out.Add ("baseline", StairPolylineJson (m.stairBaseLine));
		out.Add ("baselineLength", HandlePathLength (m.stairBaseLine.coords, m.stairBaseLine.parcs,
													 m.stairBaseLine.polygon.nCoords, m.stairBaseLine.polygon.nArcs));
	}
	if (m.stairWalkingLine.coords != nullptr)
		out.Add ("walkingLine", StairPolylineJson (m.stairWalkingLine));
	if (m.stairBoundary[0].coords != nullptr)
		out.Add ("leftBoundary", StairBoundaryJson (m.stairBoundary[0]));
	if (m.stairBoundary[1].coords != nullptr)
		out.Add ("rightBoundary", StairBoundaryJson (m.stairBoundary[1]));

	bool truncated = false;
	out.Add ("risers", HeadGuids (m.stairRisers, PtrCount (m.stairRisers), kMaxListedParts, truncated));
	out.Add ("treads", HeadGuids (m.stairTreads, PtrCount (m.stairTreads), kMaxListedParts, truncated));
	out.Add ("structureParts", HeadGuids (m.stairStructures, PtrCount (m.stairStructures), kMaxListedParts, truncated));
	if (truncated)
		out.Add ("partsTruncated", GS::UniString ("Only the first 1000 GUIDs per kind are listed; use get_subelements for all parts."));
}


void ModifyStair (API_Element& element, API_Element& mask, API_ElementMemo&, UInt64&, const OS& patch)
{
	if (Has (patch, "baseline") || Has (patch, "begin") || Has (patch, "end"))
		Fail ("The baseline of an existing stair cannot be changed through the API. Use move_elements / rotate_elements, "
			  "or delete the stair and create a new one with create_stairs.", APIERR_NOTSUPPORTED);
	ApplyStairFields (element.stair, &mask, patch);
}

// --- Stair parts ---------------------------------------------------------------------------------

void SerializeRiser (const API_Element& element, OS& out)
{
	const API_StairRiserType& r = element.stairRiser;
	AddOwner (out, r.owner);
	out.Add ("role", NameOf (kRoles, r.role));
	out.Add ("sequenceNumber", (Int32) r.sequenceNumber);
	out.Add ("visible", r.visible);
	out.Add ("thickness", r.thickness);
	out.Add ("offset", r.offset);
	out.Add ("crossSection", NameOf (kRiserCross, r.crossSect));
	AddAngle (out, "angle", r.angle);
	out.Add ("position", NameOf (kRiserPositions, r.riserPosition));
	AddBoundingBox (out, element.header);
}


void SerializeTread (const API_Element& element, OS& out)
{
	const API_StairTreadType& t = element.stairTread;
	AddOwner (out, t.owner);
	out.Add ("role", NameOf (kRoles, t.role));
	out.Add ("sequenceNumber", (Int32) t.sequenceNumber);
	out.Add ("visible", t.visible);
	out.Add ("isCustom", t.isCustom);
	out.Add ("thickness", t.thickness);
	out.Add ("offset", t.offset);
	out.Add ("zOffset", t.zOffset);
	out.Add ("nosingType", NameOf (kNosing, t.nosingType));
	out.Add ("nosing", t.nosingValue);
	AddBoundingBox (out, element.header);
}


void SerializeStairStructure (const API_Element& element, OS& out)
{
	const API_StairStructureType& s = element.stairStructure;
	AddOwner (out, s.owner);
	out.Add ("role", NameOf (kRoles, s.role));
	out.Add ("side", NameOf (kStructSides, s.side));
	out.Add ("sequenceNumber", (Int32) s.sequenceNumber);
	out.Add ("visible", s.visible);
	out.Add ("isLanding", s.isLanding);
	out.Add ("type", NameOf (kStructTypes, s.structType));
	out.Add ("bottom", NameOf (kStructBottoms, s.bottomType));
	out.Add ("thickness", s.thickness);
	out.Add ("horizontalThickness", s.horizontalThickness);
	AddBoundingBox (out, element.header);
}

// ============================================================================================
// Railing
// ============================================================================================

void ApplyRailingFields (API_RailingType& r, API_Element* mask, const OS& spec)
{
#define RL_SET(field) if (mask != nullptr) ACAPI_ELEMENT_MASK_SET (*mask, API_RailingType, field)
	if (ApplyLength (spec, "height", r.defSegment.height, true))		RL_SET (defSegment.height);
	if (ApplyLength (spec, "bottomOffset", r.bottomOffset))				RL_SET (bottomOffset);
	if (Has (spec, "referenceLine")) {
		r.defSegment.refLineLocation = (API_RailingRefLineLocationID) ParseNamed (kRailRefLines, spec, "referenceLine");
		RL_SET (defSegment.refLineLocation);
	}
	if (ApplyLength (spec, "offset", r.defSegment.yOffset))				RL_SET (defSegment.yOffset);
	if (ApplyLength (spec, "postOffset", r.defNode.postOffset))			RL_SET (defNode.postOffset);
	if (ApplyPenField (spec, "referenceLinePen", r.referenceLinePen))		RL_SET (referenceLinePen);
	if (ApplyPenField (spec, "contourPen", r.contourPen))					RL_SET (contourPen);
#undef RL_SET
}


API_Guid CreateRailing (const OS& spec)
{
	API_Element element = NewElement (API_RailingID);
	Memo memo;
	GetDefaults (element, memo.Ptr ());
	ApplyCommonFields (element, nullptr, spec);
	API_RailingType& railing = element.railing;
	ApplyStoryCreationMode (railing.linkToSettings, spec);

	const Contour path = GetPath (spec, "path", true, 2);
	const UInt32 n = (UInt32) path.points.GetSize ();
	API_Polygon poly;
	BNZeroMemory (&poly, sizeof (poly));
	WritePolylineToMemo (path, poly, *memo);

	KillHdl (memo->polyZCoords);
	memo->polyZCoords = AllocHdl<double> (n + 1);
	for (UInt32 i = 0; i < n; ++i)
		(*memo->polyZCoords)[i + 1] = path.z.IsEmpty () ? 0.0 : path.z[i];
	railing.nVertices = n;

	ApplyRailingFields (railing, nullptr, spec);

	GSErrCode err = ACAPI_Element_Create (&element, memo.Ptr ());
	Check (err, "Cannot create railing" +
		   GS::UniString (err != NoError ? " (check the path: at least 2 distinct points, no self-intersections; segment height > 0)" : ""));
	return element.header.guid;
}


void SerializeRailing (const API_Element& element, OS& out)
{
	const API_RailingType& r = element.railing;
	out.Add ("height", r.defSegment.height);
	out.Add ("bottomOffset", r.bottomOffset);
	out.Add ("referenceLine", NameOf (kRailRefLines, r.defSegment.refLineLocation));
	out.Add ("offset", r.defSegment.yOffset);
	out.Add ("postOffset", r.defNode.postOffset);
	out.Add ("referenceLinePen", (Int32) r.referenceLinePen);
	out.Add ("contourPen", (Int32) r.contourPen);
	out.Add ("counts", OS ("vertices", (Int32) r.nVertices, "nodes", (Int32) r.nNodes, "segments", (Int32) r.nSegments));
	AddBoundingBox (out, element.header);

	Memo memo;
	if (ACAPI_Element_GetMemo (element.header.guid, memo.Ptr (), APIMemoMask_All) != NoError)
		return;
	const API_ElementMemo& m = *memo;
	if (m.coords != nullptr) {
		out.Add ("path", HandlePolylineJson (m.coords, m.parcs, (Int32) r.nVertices, 0, m.polyZCoords));
		out.Add ("length", HandlePathLength (m.coords, m.parcs, (Int32) r.nVertices, 0));
	}

	GS::Array<OS> segments;
	const UInt32 nSeg = PtrCount (m.railingSegments);
	for (UInt32 i = 0; i < nSeg && i < kMaxListedParts; ++i) {
		const API_RailingSegmentType& s = m.railingSegments[i];
		segments.Push (OS ("guid", GuidStr (s.head.guid), "height", s.height, "visible", s.visible));
	}
	out.Add ("segments", segments);

	bool truncated = nSeg > kMaxListedParts;
	out.Add ("nodes", HeadGuids (m.railingNodes, PtrCount (m.railingNodes), kMaxListedParts, truncated));
	out.Add ("posts", HeadGuids (m.railingPosts, PtrCount (m.railingPosts), kMaxListedParts, truncated));
	out.Add ("innerPosts", HeadGuids (m.railingInnerPosts, PtrCount (m.railingInnerPosts), kMaxListedParts, truncated));
	out.Add ("toprails", HeadGuids (m.railingToprails, PtrCount (m.railingToprails), kMaxListedParts, truncated));
	out.Add ("handrails", HeadGuids (m.railingHandrails, PtrCount (m.railingHandrails), kMaxListedParts, truncated));
	out.Add ("rails", HeadGuids (m.railingRails, PtrCount (m.railingRails), kMaxListedParts, truncated));
	out.Add ("panels", HeadGuids (m.railingPanels, PtrCount (m.railingPanels), kMaxListedParts, truncated));
	out.Add ("balusterSets", HeadGuids (m.railingBalusterSets, PtrCount (m.railingBalusterSets), kMaxListedParts, truncated));
	out.Add ("balusters", HeadGuids (m.railingBalusters, PtrCount (m.railingBalusters), kMaxListedParts, truncated));
	if (truncated)
		out.Add ("partsTruncated", GS::UniString ("Only the first 1000 items per kind are listed; use get_subelements for all parts."));
}


void ModifyRailing (API_Element& element, API_Element& mask, API_ElementMemo&, UInt64&, const OS& patch)
{
	if (Has (patch, "path") || Has (patch, "begin") || Has (patch, "end"))
		Fail ("The path of an existing railing cannot be changed through the API. Use move_elements / rotate_elements, "
			  "or delete the railing and create a new one with create_railings.", APIERR_NOTSUPPORTED);
	ApplyRailingFields (element.railing, &mask, patch);
}

// --- Railing parts ------------------------------------------------------------------------------

void SerializeRailingSegment (const API_Element& element, OS& out)
{
	const API_RailingSegmentType& s = element.railingSegment;
	AddOwner (out, s.owner);
	out.Add ("visible", s.visible);
	out.Add ("height", s.height);
	out.Add ("referenceLine", NameOf (kRailRefLines, s.refLineLocation));
	out.Add ("offset", s.yOffset);
	out.Add ("zOffset", s.zOffset);
	AddAngle (out, "slantAngle", s.slantAngle);
	AddAngle (out, "skewAngle", s.skewAngle);
	AddBoundingBox (out, element.header);
}


void SerializeRailingNode (const API_Element& element, OS& out)
{
	const API_RailingNodeType& n = element.railingNode;
	AddOwner (out, n.owner);
	out.Add ("visible", n.visible);
	out.Add ("elevation", n.elevation);
	out.Add ("postOffset", n.postOffset);
	out.Add ("visiblePostCount", (Int32) n.visiblePostNum);
	out.Add ("tiltedPost", n.tiltedPost);
}


void SerializeRailingPost (const API_Element& element, OS& out)
{
	AddOwner (out, element.railingPost.owner);
	AddBoundingBox (out, element.header);
}


void SerializeRailingInnerPost (const API_Element& element, OS& out)
{
	const API_RailingInnerPostType& p = element.railingInnerPost;
	AddOwner (out, p.owner);
	out.Add ("postCount", (Int32) p.postNum);
	out.Add ("position", p.horizontalPosition.position);
	AddBoundingBox (out, element.header);
}


void SerializeRailingToprail (const API_Element& element, OS& out)
{
	AddOwner (out, element.railingToprail.owner);
	out.Add ("visible", element.railingToprail.visible);
	AddBoundingBox (out, element.header);
}


void SerializeRailingHandrail (const API_Element& element, OS& out)
{
	const API_RailingHandrailType& h = element.railingHandrail;
	AddOwner (out, h.owner);
	out.Add ("visible", h.visible);
	out.Add ("height", h.height);
	out.Add ("horizontalOffset", h.horizontalOffset);
	out.Add ("doubleHandrail", h.doubleHandrail);
	AddBoundingBox (out, element.header);
}


void SerializeRailingRail (const API_Element& element, OS& out)
{
	const API_RailingRailType& r = element.railingRail;
	AddOwner (out, r.owner);
	out.Add ("visible", r.visible);
	out.Add ("height", r.height);
	out.Add ("horizontalOffset", r.horizontalOffset);
	out.Add ("relative", r.relative);
	AddBoundingBox (out, element.header);
}


void SerializeRailingPanel (const API_Element& element, OS& out)
{
	const API_RailingPanelType& p = element.railingPanel;
	AddOwner (out, p.owner);
	out.Add ("visible", p.visible);
	out.Add ("frame", OS ("offset", p.frame.yOffset, "topOffset", p.frame.zTopOffset, "bottomOffset", p.frame.zBottomOffset,
						  "beginOffset", p.frame.xBeginOffset, "endOffset", p.frame.xEndOffset));
	AddBoundingBox (out, element.header);
}


void SerializeRailingBalusterSet (const API_Element& element, OS& out)
{
	const API_RailingBalusterSetType& b = element.railingBalusterSet;
	AddOwner (out, b.owner);
	out.Add ("balusterCount", (Int32) b.nBalusters);
	out.Add ("distribution", OS ("type", NameOf (kDistribution, b.distribution.type), "length", b.distribution.length,
								 "patternAmount", (Int32) b.distribution.patternAmount));
}


void SerializeRailingBaluster (const API_Element& element, OS& out)
{
	AddOwner (out, element.railingBaluster.owner);
	out.Add ("position", element.railingBaluster.horizontalPosition.position);
	AddBoundingBox (out, element.header);
}


void SerializeRailingPattern (const API_Element& element, OS& out)
{
	const API_RailingPatternType& p = element.railingPattern;
	AddOwner (out, p.owner);
	out.Add ("panelCount", (Int32) p.nPanels);
	out.Add ("balusterSetCount", (Int32) p.nBalusterSets);
	out.Add ("innerPostCount", (Int32) p.nInnerPosts);
	out.Add ("distribution", OS ("type", NameOf (kDistribution, p.distribution.type), "length", p.distribution.length,
								 "patternAmount", (Int32) p.distribution.patternAmount));
}

} // namespace


void RegisterStairRailingFamily ()
{
	RegisterAdapter ({ API_StairID, CreateStair, SerializeStair, ModifyStair });
	RegisterAdapter ({ API_RiserID, nullptr, SerializeRiser, nullptr });
	RegisterAdapter ({ API_TreadID, nullptr, SerializeTread, nullptr });
	RegisterAdapter ({ API_StairStructureID, nullptr, SerializeStairStructure, nullptr });

	RegisterAdapter ({ API_RailingID, CreateRailing, SerializeRailing, ModifyRailing });
	RegisterAdapter ({ API_RailingSegmentID, nullptr, SerializeRailingSegment, nullptr });
	RegisterAdapter ({ API_RailingNodeID, nullptr, SerializeRailingNode, nullptr });
	RegisterAdapter ({ API_RailingPostID, nullptr, SerializeRailingPost, nullptr });
	RegisterAdapter ({ API_RailingInnerPostID, nullptr, SerializeRailingInnerPost, nullptr });
	RegisterAdapter ({ API_RailingToprailID, nullptr, SerializeRailingToprail, nullptr });
	RegisterAdapter ({ API_RailingHandrailID, nullptr, SerializeRailingHandrail, nullptr });
	RegisterAdapter ({ API_RailingRailID, nullptr, SerializeRailingRail, nullptr });
	RegisterAdapter ({ API_RailingPanelID, nullptr, SerializeRailingPanel, nullptr });
	RegisterAdapter ({ API_RailingBalusterSetID, nullptr, SerializeRailingBalusterSet, nullptr });
	RegisterAdapter ({ API_RailingBalusterID, nullptr, SerializeRailingBaluster, nullptr });
	RegisterAdapter ({ API_RailingPatternID, nullptr, SerializeRailingPattern, nullptr });
}

} // namespace complex
} // namespace cc
