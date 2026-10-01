// *****************************************************************************
// ColumnsBeams — Column and Beam adapters (AC26 segmented columns / beams).
//
// All lengths in meters, angles in degrees. Unspecified settings come from the
// tool defaults (ACAPI_Element_GetDefaults with memo = default segment data).
//
// COLUMN create / modify fields
//   origin {x,y}* (* required on create), height (unlinks the top unless topLinkedStory is given),
//   bottomOffset, topLinkedStory (0 = unlinked), topOffset, rotationAngle (plan rotation of the section),
//   slanted, slantAngle (from horizontal, 90 = vertical), slantDirection (plan direction the top leans towards), flipped,
//   anchor (0..8 | TopLeft..BottomRight), wrapping, zoneRelation, floorPlanSymbol,
//   floorPlanDisplay, viewDepthLimitation, showOnStories, cutFillPen, cutFillBackgroundPen, coverFill,
//   lines {contour, uncut, overhead, hidden, veneer: {pen, lineType}, symbol: {pen}},
//   section (applies to ALL segments): shape, width, depth, diameter, tapered, endWidth, endDepth,
//     endDiameter, buildingMaterial | profile, surface, sideSurface, endsSurface, surfacesChained,
//     veneerType, veneerThickness, veneerBuildingMaterial
//   segments [{section fields..., length | lengthProportion}] (bottom -> top; resizes the segment list),
//   cuts [{type, angle}] (segmentCount + 1 items: bottom, between segments, top)
//
// BEAM create / modify fields
//   begin {x,y}*, end {x,y}*, level (reference line height above the home story), offset,
//   anchor, arcAngle (horizontally curved), verticalCurveHeight (vertically curved), beamShape,
//   slantAngle, profileRotationAngle, flipped, showContourLines, showReferenceAxis,
//   floorPlanDisplay, viewDepthLimitation, showOnStories, cutFillPen, cutFillBackgroundPen, coverFill,
//   lines {reference, cutContour, uncut, overhead, hidden: {pen, lineType}},
//   section (ALL segments): shape, width, height, diameter, tapered, endWidth, endHeight, endDiameter,
//     buildingMaterial | profile, surface, leftSurface, rightSurface, topSurface, bottomSurface,
//     endsSurface, surfacesChained
//   segments [{section fields..., length | lengthProportion}] (begin -> end), cuts [{type, angle}],
//   holes [{shape, distanceFromBegin*, depthBelowTop | offsetBelowAxis, width, height, diameter, showContour}] (replaces all),
//   addHoles [...] (appends), removeHoles [holeId...]
//
// + common: layer, storyIndex, renovationStatus, elementId
// *****************************************************************************

#include "Commands/Commands.hpp"
#include "Commands/ColumnsBeamsSegments.hpp"
#include "Core/Command.hpp"
#include "Core/Elements.hpp"
#include "Core/Enums.hpp"

#include <cmath>

namespace cc {

namespace {

using namespace cb;

constexpr UInt32 kMaxSegments = 100;
constexpr UInt64 kSegmentMemoBits = APIMemoMask_AssemblySegmentCut | APIMemoMask_AssemblySegmentScheme;

const NamedValue kVeneerTypes[] = {
	{ "Core",		APIVeneer_Core },
	{ "Finish",		APIVeneer_Finish },
	{ "Other",		APIVeneer_Other },
};

const NamedValue kCoreSymbols[] = {
	{ "Plain",		1 },
	{ "Slash",		2 },
	{ "X",			3 },
	{ "Crosshair",	4 },
};

const NamedValue kBeamShapes[] = {
	{ "Straight",			API_StraightBeam },
	{ "HorizontallyCurved",	API_HorizontallyCurvedBeam },
	{ "VerticallyCurved",	API_VerticallyCurvedBeam },
};

const NamedValue kBeamLines[] = {
	{ "Always",				APIBeamLineShowAlways },
	{ "Never",				APIBeamLineHideAlways },
	{ "ByModelViewOptions",	APIBeamLineByMVO },
};

const NamedValue kHoleTypes[] = {
	{ "Rectangular",	APIBHole_Rectangular },
	{ "Circular",		APIBHole_Circular },
};


bool ContainsAny (const OS& spec, std::initializer_list<const char*> keys)
{
	for (const char* key : keys) {
		if (spec.Contains (key))
			return true;
	}
	return false;
}


bool SameSurface (const API_OverriddenAttribute& a, const API_OverriddenAttribute& b)
{
	return a.overridden == b.overridden && (!a.overridden || a.attributeIndex == b.attributeIndex);
}


// Angle in radians normalized to (-pi, pi].
double NormalizeAngle (double a)
{
	a = std::fmod (a, 2.0 * kPi);
	if (a <= -kPi) a += 2.0 * kPi;
	if (a > kPi) a -= 2.0 * kPi;
	return std::fabs (a) < 1e-12 ? 0.0 : a;
}


// =============================================================================
// Generic segment list handling (shared by columns and beams)
// =============================================================================

template <typename TSeg>
struct SegmentAccess {
	TSeg*&										segments;		// memo.columnSegments | memo.beamSegments
	UInt32&										nSegments;
	UInt32&										nCuts;
	UInt32&										nSchemes;
	UInt32&										nProfiles;
	UInt64										segmentMemoBit;	// APIMemoMask_ColumnSegment | APIMemoMask_BeamSegment
	API_AssemblySegmentCutTypeID				innerCutDefault;
	const char*									typeName;
	std::function<void (TSeg&, const OS&)>		applyFields;	// section, surfaces, veneer
	std::function<void ()>						markCountsChanged;	// sets the element mask for n* fields (modify)
};


// Number of segments requested by spec.segments (0 = not given).
UInt32 RequestedSegmentCount (const OS& spec)
{
	if (!spec.Contains ("segments"))
		return 0;
	GS::Array<OS> list = GetObjectArray (spec, "segments", false);
	if (list.IsEmpty ())
		Fail ("'segments' must contain at least one segment (bottom -> top for columns, begin -> end for beams).");
	if (list.GetSize () > kMaxSegments)
		Fail ("Too many segments (" + GS::ValueToUniString ((Int32) list.GetSize ()) + "); at most " + GS::ValueToUniString ((Int32) kMaxSegments) + " are supported.");
	return list.GetSize ();
}


template <typename TSeg>
void ApplySegments (SegmentAccess<TSeg>& a, API_ElementMemo& memo, const OS& spec, double totalLength, UInt64& memoMask)
{
	CheckSegmentMemo (a.segments, a.nSegments, a.typeName);

	const UInt32 requested = RequestedSegmentCount (spec);
	GS::Array<OS> segSpecs = GetObjectArray (spec, "segments", false);
	UInt64 bits = a.segmentMemoBit;
	bool schemesTouched = false;
	bool countsChanged = false;

	if (requested > 0 && requested != a.nSegments) {
		a.segments = ResizeMemoArray (a.segments, a.nSegments, requested);
		if (a.nProfiles > 0) {
			a.nProfiles = FilterProfiles (memo.assemblySegmentProfiles, a.nProfiles, requested);
			bits |= APIMemoMask_AssemblySegmentProfile;
		}
		a.nSegments = requested;
		countsChanged = true;
	}

	// Keep the cut / scheme arrays consistent with the segment count (nSegments + 1 cuts, nSegments schemes).
	if (a.nCuts != a.nSegments + 1 || PtrCount (memo.assemblySegmentCuts) < a.nSegments + 1) {
		memo.assemblySegmentCuts = ResizeCuts (memo.assemblySegmentCuts, a.nCuts, a.nSegments, a.innerCutDefault);
		a.nCuts = a.nSegments + 1;
		countsChanged = true;
		bits |= APIMemoMask_AssemblySegmentCut;
	}
	if (a.nSchemes != a.nSegments || PtrCount (memo.assemblySegmentSchemes) < a.nSegments) {
		// New segments share the proportional length (weight = average weight of the existing proportional segments).
		const UInt32 available = std::min (a.nSchemes, PtrCount (memo.assemblySegmentSchemes));
		double weightSum = 0.0;
		UInt32 weightCount = 0;
		for (UInt32 i = 0; i < available; ++i) {
			const API_AssemblySegmentSchemeData& scheme = memo.assemblySegmentSchemes[i];
			if (scheme.lengthType == APIAssemblySegment_Proportional && scheme.lengthProportion > kEps) {
				weightSum += scheme.lengthProportion;
				++weightCount;
			}
		}
		const double newWeight = weightCount > 0 ? weightSum / weightCount : 1.0;
		memo.assemblySegmentSchemes = ResizeMemoArray (memo.assemblySegmentSchemes, a.nSchemes, a.nSegments);
		for (UInt32 i = std::min (available, a.nSegments); i < a.nSegments; ++i) {
			memo.assemblySegmentSchemes[i].lengthType = APIAssemblySegment_Proportional;
			memo.assemblySegmentSchemes[i].fixedLength = 0.0;
			memo.assemblySegmentSchemes[i].lengthProportion = newWeight;
		}
		a.nSchemes = a.nSegments;
		countsChanged = true;
		schemesTouched = true;
	}
	if (countsChanged) {
		if (a.markCountsChanged)
			a.markCountsChanged ();
		bits |= kSegmentMemoBits;
	}

	// Top-level section/surface fields apply to every segment, per-segment specs override them.
	for (UInt32 i = 0; i < a.nSegments; ++i)
		a.applyFields (a.segments[i], spec);
	for (UInt32 i = 0; i < segSpecs.GetSize () && i < a.nSegments; ++i) {
		WithContext (IndexedName ("segments", i), [&] () {
			a.applyFields (a.segments[i], segSpecs[i]);
			if (ApplyScheme (memo.assemblySegmentSchemes[i], segSpecs[i]))
				schemesTouched = true;
		});
	}

	if (spec.Contains ("cuts")) {
		GS::Array<OS> cuts = GetObjectArray (spec, "cuts", true);
		if (cuts.GetSize () != a.nSegments + 1) {
			Fail ("'cuts' must have segmentCount + 1 = " + GS::ValueToUniString ((Int32) (a.nSegments + 1)) +
				  " items (start/bottom cut, one between each pair of segments, end/top cut); the " + a.typeName + " has " +
				  GS::ValueToUniString ((Int32) a.nSegments) + " segment(s).");
		}
		for (UInt32 i = 0; i < cuts.GetSize (); ++i)
			WithContext (IndexedName ("cuts", i), [&] () { ApplyCut (memo.assemblySegmentCuts[i], cuts[i]); });
		bits |= APIMemoMask_AssemblySegmentCut;
	}

	if (schemesTouched) {
		NormalizeSchemes (memo.assemblySegmentSchemes, a.nSchemes, totalLength);
		bits |= APIMemoMask_AssemblySegmentScheme;
	}
	memoMask |= bits;
}


// Adds segments / cuts arrays (and mirrors the only segment's fields at top level).
template <typename TSeg>
void AddSegmentsJson (OS& out, const TSeg* segments, UInt32 nSegments, const API_ElementMemo& memo, UInt32 nCuts, UInt32 nSchemes,
					  double totalLength, const std::function<void (OS&, const TSeg&)>& addFields)
{
	const UInt32 n = std::min (nSegments, PtrCount (segments));
	const UInt32 nSch = std::min (std::min (nSchemes, PtrCount (memo.assemblySegmentSchemes)), n);
	GS::Array<double> lengths = totalLength > 0.0 ? SegmentLengths (memo.assemblySegmentSchemes, nSch, totalLength) : GS::Array<double> ();

	GS::Array<OS> list;
	for (UInt32 i = 0; i < n; ++i) {
		OS seg;
		seg.Add ("index", (Int32) i);
		if (segments[i].head.guid != APINULLGuid)
			seg.Add ("guid", GuidStr (segments[i].head.guid));
		addFields (seg, segments[i]);
		if (i < nSch)
			AddSchemeJson (seg, memo.assemblySegmentSchemes[i], i < lengths.GetSize () ? lengths[i] : -1.0);
		list.Push (seg);
	}
	out.Add ("segmentCount", (Int32) nSegments);
	out.Add ("segments", list);

	GS::Array<OS> cuts;
	const UInt32 nc = std::min (nCuts, PtrCount (memo.assemblySegmentCuts));
	for (UInt32 i = 0; i < nc; ++i)
		cuts.Push (CutToJson (memo.assemblySegmentCuts[i]));
	out.Add ("cuts", cuts);

	if (n == 1)
		addFields (out, segments[0]);
}


// =============================================================================
// Column
// =============================================================================

bool TouchesColumnSegments (const OS& spec)
{
	return HasSectionFields (spec, kColumnSectionKeys) ||
		   ContainsAny (spec, { "surface", "sideSurface", "endsSurface", "surfacesChained",
								"veneerType", "veneerThickness", "veneerBuildingMaterial", "segments", "cuts" });
}


void ApplyColumnSegmentFields (API_ColumnSegmentType& seg, const OS& spec)
{
	ApplySection (seg.assemblySegmentData, spec, kColumnSectionKeys);

	if (spec.Contains ("surface")) {
		ApplyOverriddenSurface (spec, "surface", seg.extrusionSurfaceMaterial);
		ApplyOverriddenSurface (spec, "surface", seg.endsMaterial);
	}
	const bool side = ApplyOverriddenSurface (spec, "sideSurface", seg.extrusionSurfaceMaterial);
	const bool ends = ApplyOverriddenSurface (spec, "endsSurface", seg.endsMaterial);
	if (auto b = OptBool (spec, "surfacesChained"))
		seg.materialsChained = *b;
	else if ((side || ends) && !SameSurface (seg.extrusionSurfaceMaterial, seg.endsMaterial))
		seg.materialsChained = false;	// individual faces were set differently: unchain so both are kept

	if (spec.Contains ("veneerType"))
		seg.venType = (API_VeneerTypeID) ParseNamed (kVeneerTypes, spec, "veneerType");
	if (auto v = OptNonNegative (spec, "veneerThickness"))
		seg.venThick = *v;
	if (auto bm = OptAttr (API_BuildingMaterialID, spec, "veneerBuildingMaterial"))
		seg.venBuildingMaterial = *bm;
}


void AddColumnSegmentFields (OS& out, const API_ColumnSegmentType& seg)
{
	AddSectionJson (out, seg.assemblySegmentData, kColumnSectionKeys);
	AddOverriddenSurfaceJson (out, "sideSurface", seg.extrusionSurfaceMaterial);
	AddOverriddenSurfaceJson (out, "endsSurface", seg.endsMaterial);
	out.Add ("surfacesChained", seg.materialsChained);
	out.Add ("veneerType", NameOf (kVeneerTypes, seg.venType));
	out.Add ("veneerThickness", seg.venThick);
	if (seg.venThick > kEps)
		out.Add ("veneerBuildingMaterial", AttrRef (API_BuildingMaterialID, seg.venBuildingMaterial));
}


void ApplyColumnFields (API_ColumnType& col, API_Element* mask, const OS& spec)
{
#define COL_SET(field) if (mask) ACAPI_ELEMENT_MASK_SET (*mask, API_ColumnType, field)

	if (auto c = OptCoord (spec, "origin"))		{ col.origoPos = *c; COL_SET (origoPos.x); COL_SET (origoPos.y); }
	if (auto v = OptPositive (spec, "height")) {
		col.height = *v; COL_SET (height);
		// An explicit height only takes effect on an unlinked column (tool defaults are often top-linked).
		if (!spec.Contains ("topLinkedStory")) { col.relativeTopStory = 0; COL_SET (relativeTopStory); }
	}
	if (auto v = OptDouble (spec, "bottomOffset"))	{ col.bottomOffset = *v; COL_SET (bottomOffset); }
	if (auto v = OptInt (spec, "topLinkedStory")) {
		if (*v < 0)
			Fail ("'topLinkedStory' must be >= 0 (number of stories above the home story, 0 = not linked).");
		col.relativeTopStory = (short) *v; COL_SET (relativeTopStory);
	}
	if (auto v = OptDouble (spec, "topOffset"))	{ col.topOffset = *v; COL_SET (topOffset); }
	// Plan angles (verified live on AC26): Archicad rotates the section by axisRotationAngle + slantDirectionAngle
	// in plan, and a slanted column leans towards slantDirectionAngle + 90 deg. The JSON uses plan semantics:
	//   rotationAngle  = axisRotationAngle + slantDirectionAngle  (plan direction of the section's local x axis)
	//   slantDirection = slantDirectionAngle + 90 deg              (plan direction the top leans towards)
	// Changing only slantDirection keeps the plan rotation of the section.
	{
		const double oldPlanRotation = col.axisRotationAngle + col.slantDirectionAngle;
		const auto rotation = OptAngle (spec, "rotationAngle");
		const auto direction = OptAngle (spec, "slantDirection");
		if (direction.has_value ()) {
			col.slantDirectionAngle = NormalizeAngle (*direction - kPi / 2.0); COL_SET (slantDirectionAngle);
		}
		if (rotation.has_value () || direction.has_value ()) {
			col.axisRotationAngle = NormalizeAngle (rotation.value_or (oldPlanRotation) - col.slantDirectionAngle);
			COL_SET (axisRotationAngle);
		}
	}

	if (auto v = OptAngle (spec, "slantAngle")) {
		if (!(*v > 1e-6 && *v < kPi - 1e-6))
			Fail ("'slantAngle' must be between 0 and 180 degrees (measured from the horizontal plane, 90 = vertical).");
		col.slantAngle = *v; COL_SET (slantAngle);
		if (!spec.Contains ("slanted")) { col.isSlanted = std::fabs (*v - kPi / 2) > 1e-6; COL_SET (isSlanted); }
	}
	if (auto b = OptBool (spec, "slanted")) {
		col.isSlanted = *b; COL_SET (isSlanted);
		if (*b && !(col.slantAngle > 1e-6 && col.slantAngle < kPi - 1e-6))
			Fail ("'slanted': true needs a 'slantAngle' between 0 and 180 degrees (from the horizontal, 90 = vertical).");
	}
	if (auto b = OptBool (spec, "flipped"))		{ col.isFlipped = *b; COL_SET (isFlipped); }
	if (auto a = OptAnchor (spec, "anchor"))		{ col.coreAnchor = *a; COL_SET (coreAnchor); }
	if (auto b = OptBool (spec, "wrapping"))		{ col.wrapping = *b; COL_SET (wrapping); }
	if (Has (spec, "zoneRelation")) {
		col.zoneRel = (API_ZoneRelID) ParseNamed (kZoneRelations, spec, "zoneRelation"); COL_SET (zoneRel);
	}
	if (Has (spec, "floorPlanSymbol")) {
		col.coreSymbolType = (short) ParseNamed (kCoreSymbols, spec, "floorPlanSymbol"); COL_SET (coreSymbolType);
	}
	if (Has (spec, "floorPlanDisplay")) {
		col.displayOption = (API_ElemDisplayOptionsID) ParseNamed (kDisplayOptions, spec, "floorPlanDisplay"); COL_SET (displayOption);
	}
	if (Has (spec, "viewDepthLimitation")) {
		col.viewDepthLimitation = (API_ElemViewDepthLimitationsID) ParseNamed (kViewDepthLimitations, spec, "viewDepthLimitation");
		COL_SET (viewDepthLimitation);
	}
	// ACAPI_ELEMENT_MASK_SET only marks the first byte of a field: struct fields need every sub-field masked.
	if (ApplyStoryVisibility (spec, "showOnStories", col.visibility, col.isAutoOnStoryVisibility)) {
		COL_SET (visibility.showOnHome); COL_SET (visibility.showAllAbove); COL_SET (visibility.showAllBelow);
		COL_SET (visibility.showRelAbove); COL_SET (visibility.showRelBelow); COL_SET (isAutoOnStoryVisibility);
	}
	if (ApplyPenOverride (spec, col.penOverride)) {
		COL_SET (penOverride.cutFillPen); COL_SET (penOverride.cutFillBackgroundPen);
		COL_SET (penOverride.overrideCutFillPen); COL_SET (penOverride.overrideCutFillBackgroundPen);
	}
	if (ApplyCoverFill (spec, { col.useCoverFill, col.useCoverFillFromSurface, col.coverFillOrientationComesFrom3D,
								col.coverFillType, col.coverFillForegroundPen, col.coverFillBackgroundPen })) {
		COL_SET (useCoverFill); COL_SET (useCoverFillFromSurface); COL_SET (coverFillOrientationComesFrom3D);
		COL_SET (coverFillType); COL_SET (coverFillForegroundPen); COL_SET (coverFillBackgroundPen);
	}
	if (spec.Contains ("lines")) {
		const OS lines = GetObject (spec, "lines");
		if (ApplyLineStyle (lines, "contour", &col.corePen, &col.contLtype))						{ COL_SET (corePen); COL_SET (contLtype); }
		if (ApplyLineStyle (lines, "uncut", &col.belowViewLinePen, &col.belowViewLineType))		{ COL_SET (belowViewLinePen); COL_SET (belowViewLineType); }
		if (ApplyLineStyle (lines, "overhead", &col.aboveViewLinePen, &col.aboveViewLineType))	{ COL_SET (aboveViewLinePen); COL_SET (aboveViewLineType); }
		if (ApplyLineStyle (lines, "hidden", &col.hiddenLinePen, &col.hiddenLineType))			{ COL_SET (hiddenLinePen); COL_SET (hiddenLineType); }
		if (ApplyLineStyle (lines, "veneer", &col.venLinePen, &col.venLineType))					{ COL_SET (venLinePen); COL_SET (venLineType); }
		if (ApplyLineStyle (lines, "symbol", &col.coreSymbolPen, nullptr))						{ COL_SET (coreSymbolPen); }
	}
#undef COL_SET
}


SegmentAccess<API_ColumnSegmentType> ColumnAccess (API_Element& element, API_Element* mask, API_ElementMemo& memo)
{
	API_ColumnType& col = element.column;
	return SegmentAccess<API_ColumnSegmentType> {
		memo.columnSegments, col.nSegments, col.nCuts, col.nSchemes, col.nProfiles,
		APIMemoMask_ColumnSegment, APIAssemblySegmentCut_Horizontal, "column",
		ApplyColumnSegmentFields,
		[mask] () {
			if (mask == nullptr) return;
			ACAPI_ELEMENT_MASK_SET (*mask, API_ColumnType, nSegments);
			ACAPI_ELEMENT_MASK_SET (*mask, API_ColumnType, nCuts);
			ACAPI_ELEMENT_MASK_SET (*mask, API_ColumnType, nSchemes);
			ACAPI_ELEMENT_MASK_SET (*mask, API_ColumnType, nProfiles);
		}
	};
}


// Axis length used to check fixed segment lengths (0 = unknown -> no check). Top-linked or slanted
// columns are skipped: their axis length is only known after Archicad recalculates the element.
double ColumnSegmentTotal (const API_ColumnType& col)
{
	if (col.relativeTopStory != 0 || col.isSlanted)
		return 0.0;
	return col.height;
}


// Axis length reported for existing columns (actualLength of the segments; 0 = not reported).
double ColumnAxisLength (const API_ColumnType& col)
{
	return col.isSlanted ? 0.0 : col.height;
}

// --- Column anchor point --------------------------------------------------------------
// Archicad STORES API_ColumnType::origoPos as the centre of the column section, but it INTERPRETS the
// origoPos passed to ACAPI_Element_Create / ACAPI_Element_Change as the position of the anchor point
// (coreAnchor) — even when origoPos is not in the change mask (verified live on AC26: changing only the
// height of a BottomLeft-anchored 0.3 x 0.3 column moved it by (0.15, 0.15)). The adapter therefore
// exposes "origin" as the anchor point (create input == serialized value) and always passes the anchor
// point to Archicad on modify.

struct SectionSize {
	double width = 0.0;		// along the column's local x axis
	double depth = 0.0;		// along the local y axis
};


// Size Archicad uses for the anchor grid: the nominal (bottom) size of the first (bottom) segment.
SectionSize ColumnSectionSize (const API_ColumnSegmentType* segments, UInt32 nSegments)
{
	SectionSize size;
	if (segments == nullptr || nSegments == 0 || PtrCount (segments) == 0)
		return size;
	const API_AssemblySegmentData& d = segments[0].assemblySegmentData;
	size.width = d.nominalWidth;
	size.depth = d.circleBased ? d.nominalWidth : d.nominalHeight;
	return size;
}


SectionSize LoadColumnSectionSize (const API_Element& element)
{
	Memo memo;
	if (ACAPI_Element_GetMemo (element.header.guid, memo.Ptr (), APIMemoMask_ColumnSegment) != NoError)
		return SectionSize ();
	return ColumnSectionSize (memo->columnSegments, element.column.nSegments);
}


// How Archicad maps an offset in the section's local axes to the plan (verified live on AC26 with rotated,
// slanted, non-square columns): plan = R(slantDirectionAngle) * StretchY(1 / sin(slantAngle)) * R(axisRotationAngle) * local.
// The stretch is the horizontal footprint of a slanted section (only for slanted columns).
struct ColumnFrame {
	double	rotation = 0.0;		// axisRotationAngle
	double	direction = 0.0;	// slantDirectionAngle
	double	stretch = 1.0;
};


ColumnFrame FrameOf (const API_ColumnType& col)
{
	ColumnFrame f;
	f.rotation = col.axisRotationAngle;
	f.direction = col.slantDirectionAngle;
	const double sn = std::sin (col.slantAngle);
	if (col.isSlanted && std::fabs (sn) > 1e-6)
		f.stretch = 1.0 / std::fabs (sn);
	return f;
}


// Anchor point on the plan for a column whose (bottom) section centre is 'center'
// (anchor 0..8 = 3 x 3 grid in the section's local axes, row 0 = local +y side, column 0 = local -x side).
API_Coord ColumnAnchorPoint (const API_Coord& center, const ColumnFrame& frame, short anchor, const SectionSize& size)
{
	if (anchor < 0 || anchor > 8)
		return center;
	const double lx = ((anchor % 3) - 1) * size.width / 2.0;
	const double ly = (1 - (anchor / 3)) * size.depth / 2.0;
	const double cr = std::cos (frame.rotation), sr = std::sin (frame.rotation);
	const double x1 = lx * cr - ly * sr;
	const double y1 = (lx * sr + ly * cr) * frame.stretch;
	const double cd = std::cos (frame.direction), sd = std::sin (frame.direction);
	API_Coord p;
	p.x = center.x + x1 * cd - y1 * sd;
	p.y = center.y + x1 * sd + y1 * cd;
	return p;
}


bool MaskIsEmpty (const API_Element& mask)
{
	const char* bytes = reinterpret_cast<const char*> (&mask);
	for (size_t i = 0; i < sizeof (mask); ++i) {
		if (bytes[i] != 0)
			return false;
	}
	return true;
}


void ApplyStoryCreationMode (API_LinkToSettings& link, const OS& spec)
{
	// Make the explicit storyIndex win over "relative to the current story" creation mode.
	if (spec.Contains ("storyIndex")) {
		link.newCreationMode = false;
		link.homeStoryDifference = 0;
	}
}


API_Guid CreateColumn (const OS& spec)
{
	if (!Has (spec, "origin"))
		Fail ("Column requires 'origin' {x, y} (the column axis position on the plan, m).");

	API_Element element = NewElement (API_ColumnID);
	Memo memo;
	GetDefaults (element, memo.Ptr ());

	ApplyCommonFields (element, nullptr, spec);
	ApplyStoryCreationMode (element.column.linkToSettings, spec);
	ApplyColumnFields (element.column, nullptr, spec);

	CheckSegmentMemo (memo->columnSegments, element.column.nSegments, "column tool default");
	if (TouchesColumnSegments (spec)) {
		UInt64 memoMask = 0;
		SegmentAccess<API_ColumnSegmentType> access = ColumnAccess (element, nullptr, *memo);
		ApplySegments (access, *memo, spec, ColumnSegmentTotal (element.column), memoMask);
	}

	Check (ACAPI_Element_Create (&element, memo.Ptr ()), "Cannot create column");
	return element.header.guid;
}


void SerializeColumn (const API_Element& element, OS& out)
{
	const API_ColumnType& col = element.column;
	const double storyLevel = StoryLevel (element.header.floorInd);

	Memo memo;
	const bool hasMemo = ACAPI_Element_GetMemo (element.header.guid, memo.Ptr (), APIMemoMask_ColumnSegment | kSegmentMemoBits) == NoError;
	const SectionSize size = hasMemo ? ColumnSectionSize (memo->columnSegments, col.nSegments) : SectionSize ();

	// "origin" = anchor point (same meaning as on create), "center" = centre of the (bottom) section.
	out.Add ("origin", CoordObj (ColumnAnchorPoint (col.origoPos, FrameOf (col), col.coreAnchor, size)));
	out.Add ("center", CoordObj (col.origoPos));
	AddAngle (out, "rotationAngle", NormalizeAngle (col.axisRotationAngle + col.slantDirectionAngle));	// plan rotation, see ApplyColumnFields
	out.Add ("height", col.height);
	out.Add ("bottomOffset", col.bottomOffset);
	out.Add ("topLinkedStory", (Int32) col.relativeTopStory);
	out.Add ("topOffset", col.topOffset);
	out.Add ("bottomElevation", storyLevel + col.bottomOffset);
	out.Add ("topElevation", storyLevel + col.bottomOffset + col.height);
	out.Add ("slanted", col.isSlanted);
	AddAngle (out, "slantAngle", col.slantAngle);
	AddAngle (out, "slantDirection", NormalizeAngle (col.slantDirectionAngle + kPi / 2.0));	// direction the top leans towards
	out.Add ("flipped", col.isFlipped);
	out.Add ("anchor", AnchorName (col.coreAnchor));
	out.Add ("wrapping", col.wrapping);
	out.Add ("zoneRelation", NameOf (kZoneRelations, col.zoneRel));
	out.Add ("floorPlanSymbol", NameOf (kCoreSymbols, col.coreSymbolType));
	out.Add ("floorPlanDisplay", NameOf (kDisplayOptions, col.displayOption));
	out.Add ("viewDepthLimitation", NameOf (kViewDepthLimitations, col.viewDepthLimitation));
	AddStoryVisibilityJson (out, "showOnStories", col.visibility, col.isAutoOnStoryVisibility);
	AddPenOverrideJson (out, col.penOverride);
	AddCoverFillJson (out, col.useCoverFill, col.useCoverFillFromSurface, col.coverFillOrientationComesFrom3D,
					  col.coverFillType, col.coverFillForegroundPen, col.coverFillBackgroundPen);
	out.Add ("lines", OS ("contour", LineStyleJson (&col.corePen, &col.contLtype),
						  "uncut", LineStyleJson (&col.belowViewLinePen, &col.belowViewLineType),
						  "overhead", LineStyleJson (&col.aboveViewLinePen, &col.aboveViewLineType),
						  "hidden", LineStyleJson (&col.hiddenLinePen, &col.hiddenLineType),
						  "veneer", LineStyleJson (&col.venLinePen, &col.venLineType),
						  "symbol", LineStyleJson (&col.coreSymbolPen, nullptr)));
	if (col.nProfiles > 0)
		out.Add ("customProfileCount", (Int32) col.nProfiles);

	if (hasMemo) {
		AddSegmentsJson<API_ColumnSegmentType> (out, memo->columnSegments, col.nSegments, *memo, col.nCuts, col.nSchemes,
												ColumnAxisLength (col), AddColumnSegmentFields);
	} else {
		out.Add ("segmentCount", (Int32) col.nSegments);
	}
}


void ModifyColumn (API_Element& element, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask, const OS& patch)
{
	// Geometry before the change: Archicad re-places the column from origoPos as ANCHOR point on every
	// change (see ColumnAnchorPoint), so the current anchor point must be passed instead of the stored centre.
	const API_Coord oldCenter = element.column.origoPos;
	const ColumnFrame oldFrame = FrameOf (element.column);
	const SectionSize oldSize = LoadColumnSectionSize (element);

	ApplyColumnFields (element.column, &mask, patch);
	if (TouchesColumnSegments (patch)) {
		UInt64 loadMask = APIMemoMask_ColumnSegment | kSegmentMemoBits;
		const UInt32 requested = RequestedSegmentCount (patch);
		if (requested > 0 && requested != element.column.nSegments && element.column.nProfiles > 0)
			loadMask |= APIMemoMask_AssemblySegmentProfile;
		LoadMemo (element.header.guid, memo, loadMask);

		SegmentAccess<API_ColumnSegmentType> access = ColumnAccess (element, &mask, memo);
		ApplySegments (access, memo, patch, ColumnSegmentTotal (element.column), memoMask);
	}

	// Without a new 'origin' the point of the CURRENT body where the (possibly new) anchor sits stays fixed:
	// size / rotation changes pivot around the anchor, a pure anchor change leaves the column in place.
	if (!Has (patch, "origin") && (memoMask != 0 || !MaskIsEmpty (mask))) {
		element.column.origoPos = ColumnAnchorPoint (oldCenter, oldFrame, element.column.coreAnchor, oldSize);
		ACAPI_ELEMENT_MASK_SET (mask, API_ColumnType, origoPos.x);
		ACAPI_ELEMENT_MASK_SET (mask, API_ColumnType, origoPos.y);
	}
}


// =============================================================================
// Beam
// =============================================================================

bool TouchesBeamSegments (const OS& spec)
{
	return HasSectionFields (spec, kBeamSectionKeys) ||
		   ContainsAny (spec, { "surface", "leftSurface", "rightSurface", "topSurface", "bottomSurface", "endsSurface",
								"surfacesChained", "segments", "cuts" });
}


bool TouchesBeamHoles (const OS& spec)
{
	return ContainsAny (spec, { "holes", "addHoles", "removeHoles" });
}


void ApplyBeamSegmentFields (API_BeamSegmentType& seg, const OS& spec)
{
	ApplySection (seg.assemblySegmentData, spec, kBeamSectionKeys);

	if (spec.Contains ("surface")) {
		for (API_OverriddenAttribute* attr : { &seg.leftMaterial, &seg.topMaterial, &seg.rightMaterial, &seg.bottomMaterial, &seg.endsMaterial })
			ApplyOverriddenSurface (spec, "surface", *attr);
	}
	bool individual = false;
	individual |= ApplyOverriddenSurface (spec, "leftSurface", seg.leftMaterial);
	individual |= ApplyOverriddenSurface (spec, "rightSurface", seg.rightMaterial);
	individual |= ApplyOverriddenSurface (spec, "topSurface", seg.topMaterial);
	individual |= ApplyOverriddenSurface (spec, "bottomSurface", seg.bottomMaterial);
	individual |= ApplyOverriddenSurface (spec, "endsSurface", seg.endsMaterial);
	if (auto b = OptBool (spec, "surfacesChained")) {
		seg.materialsChained = *b;
	} else if (individual) {
		const bool allSame = SameSurface (seg.leftMaterial, seg.rightMaterial) && SameSurface (seg.leftMaterial, seg.topMaterial) &&
							 SameSurface (seg.leftMaterial, seg.bottomMaterial) && SameSurface (seg.leftMaterial, seg.endsMaterial);
		if (!allSame)
			seg.materialsChained = false;	// individual faces differ: unchain so each face keeps its surface
	}
}


void AddBeamSegmentFields (OS& out, const API_BeamSegmentType& seg)
{
	AddSectionJson (out, seg.assemblySegmentData, kBeamSectionKeys);
	AddOverriddenSurfaceJson (out, "leftSurface", seg.leftMaterial);
	AddOverriddenSurfaceJson (out, "rightSurface", seg.rightMaterial);
	AddOverriddenSurfaceJson (out, "topSurface", seg.topMaterial);
	AddOverriddenSurfaceJson (out, "bottomSurface", seg.bottomMaterial);
	AddOverriddenSurfaceJson (out, "endsSurface", seg.endsMaterial);
	out.Add ("surfacesChained", seg.materialsChained);
}


double BeamHorizontalLength (const API_BeamType& beam)
{
	const double chord = std::hypot (beam.endC.x - beam.begC.x, beam.endC.y - beam.begC.y);
	if (beam.beamShape == API_HorizontallyCurvedBeam && std::fabs (beam.curveAngle) > 1e-9) {
		const double half = std::sin (std::fabs (beam.curveAngle) / 2.0);
		if (half > 1e-12)
			return chord / (2.0 * half) * std::fabs (beam.curveAngle);
	}
	return chord;
}


// Length of the beam reference axis (curves and slant included).
double BeamAxisLength (const API_BeamType& beam)
{
	const double chord = std::hypot (beam.endC.x - beam.begC.x, beam.endC.y - beam.begC.y);
	if (beam.beamShape == API_VerticallyCurvedBeam && std::fabs (beam.verticalCurveHeight) > 1e-9 && chord > 1e-9) {
		const double h = std::fabs (beam.verticalCurveHeight);
		const double r = (chord * chord / 4.0 + h * h) / (2.0 * h);
		const double ratio = std::min (1.0, chord / (2.0 * r));
		double angle = 2.0 * std::asin (ratio);
		if (h > chord / 2.0)
			angle = 2.0 * kPi - angle;
		return r * angle;
	}
	double length = BeamHorizontalLength (beam);
	if (beam.isSlanted && std::fabs (std::cos (beam.slantAngle)) > 1e-9)
		length /= std::fabs (std::cos (beam.slantAngle));
	return length;
}


void ApplyBeamFields (API_BeamType& beam, API_Element* mask, const OS& spec)
{
#define BEAM_SET(field) if (mask) ACAPI_ELEMENT_MASK_SET (*mask, API_BeamType, field)

	if (auto c = OptCoord (spec, "begin"))			{ beam.begC = *c; BEAM_SET (begC.x); BEAM_SET (begC.y); }
	if (auto c = OptCoord (spec, "end"))			{ beam.endC = *c; BEAM_SET (endC.x); BEAM_SET (endC.y); }
	if (auto v = OptDouble (spec, "level"))		{ beam.level = *v; BEAM_SET (level); }
	if (auto v = OptDouble (spec, "offset"))		{ beam.offset = *v; BEAM_SET (offset); }
	if (auto a = OptAnchor (spec, "anchor"))		{ beam.anchorPoint = *a; BEAM_SET (anchorPoint); }

	if (spec.Contains ("arcAngle") && spec.Contains ("verticalCurveHeight")) {
		const double arc = GetDouble (spec, "arcAngle");
		const double vch = GetDouble (spec, "verticalCurveHeight");
		if (std::fabs (arc) > 1e-9 && std::fabs (vch) > 1e-9)
			Fail ("A beam is either horizontally curved (arcAngle) or vertically curved (verticalCurveHeight), not both.");
	}
	if (auto v = OptAngle (spec, "arcAngle")) {
		if (std::fabs (*v) >= 2 * kPi - 1e-6)
			Fail ("'arcAngle' must be between -360 and 360 degrees (0 = straight).");
		beam.curveAngle = *v; BEAM_SET (curveAngle);
		if (!spec.Contains ("beamShape")) {
			if (std::fabs (*v) > 1e-9)
				beam.beamShape = API_HorizontallyCurvedBeam;
			else if (beam.beamShape == API_HorizontallyCurvedBeam)
				beam.beamShape = API_StraightBeam;
			BEAM_SET (beamShape);
		}
	}
	if (auto v = OptDouble (spec, "verticalCurveHeight")) {
		beam.verticalCurveHeight = *v; BEAM_SET (verticalCurveHeight);
		if (!spec.Contains ("beamShape")) {
			if (std::fabs (*v) > 1e-9)
				beam.beamShape = API_VerticallyCurvedBeam;
			else if (beam.beamShape == API_VerticallyCurvedBeam)
				beam.beamShape = API_StraightBeam;
			BEAM_SET (beamShape);
		}
	}
	if (Has (spec, "beamShape")) {
		beam.beamShape = (API_BeamShapeTypeID) ParseNamed (kBeamShapes, spec, "beamShape"); BEAM_SET (beamShape);
	}
	if (beam.beamShape == API_HorizontallyCurvedBeam && std::fabs (beam.curveAngle) < 1e-9 && ContainsAny (spec, { "beamShape", "arcAngle" }))
		Fail ("A horizontally curved beam needs a non-zero 'arcAngle' (degrees).");
	if (beam.beamShape == API_VerticallyCurvedBeam && std::fabs (beam.verticalCurveHeight) < 1e-9 && ContainsAny (spec, { "beamShape", "verticalCurveHeight" }))
		Fail ("A vertically curved beam needs a non-zero 'verticalCurveHeight' (m).");

	if (auto v = OptAngle (spec, "slantAngle")) {
		if (std::fabs (*v) >= kPi / 2 - 1e-6)
			Fail ("'slantAngle' must be between -90 and 90 degrees (0 = horizontal).");
		beam.slantAngle = *v; BEAM_SET (slantAngle);
		beam.isSlanted = std::fabs (*v) > 1e-9; BEAM_SET (isSlanted);
	}
	if (auto v = OptAngle (spec, "profileRotationAngle"))	{ beam.profileAngle = *v; BEAM_SET (profileAngle); }
	if (auto b = OptBool (spec, "flipped"))		{ beam.isFlipped = *b; BEAM_SET (isFlipped); }
	if (spec.Contains ("sequence")) {
		// Verified live: any value written to API_BeamType::sequence is replaced by Archicad (always 4200).
		Fail ("'sequence' cannot be set through the Archicad 26 API (Archicad recalculates it). Beam junction priority follows the "
			  "building material's intersection priority: change it with modify_attributes (BuildingMaterial priority).", APIERR_NOTSUPPORTED);
	}
	if (Has (spec, "showContourLines")) {
		beam.showContourLines = (API_BeamVisibleLinesID) ParseNamed (kBeamLines, spec, "showContourLines"); BEAM_SET (showContourLines);
	}
	if (Has (spec, "showReferenceAxis")) {
		beam.showReferenceAxis = (API_BeamVisibleLinesID) ParseNamed (kBeamLines, spec, "showReferenceAxis"); BEAM_SET (showReferenceAxis);
	}
	if (Has (spec, "floorPlanDisplay")) {
		beam.displayOption = (API_ElemDisplayOptionsID) ParseNamed (kDisplayOptions, spec, "floorPlanDisplay"); BEAM_SET (displayOption);
	}
	if (Has (spec, "viewDepthLimitation")) {
		beam.viewDepthLimitation = (API_ElemViewDepthLimitationsID) ParseNamed (kViewDepthLimitations, spec, "viewDepthLimitation");
		BEAM_SET (viewDepthLimitation);
	}
	// Struct fields: mask every sub-field (ACAPI_ELEMENT_MASK_SET only marks the first byte).
	if (ApplyStoryVisibility (spec, "showOnStories", beam.visibility, beam.isAutoOnStoryVisibility)) {
		BEAM_SET (visibility.showOnHome); BEAM_SET (visibility.showAllAbove); BEAM_SET (visibility.showAllBelow);
		BEAM_SET (visibility.showRelAbove); BEAM_SET (visibility.showRelBelow); BEAM_SET (isAutoOnStoryVisibility);
	}
	if (ApplyPenOverride (spec, beam.penOverride)) {
		BEAM_SET (penOverride.cutFillPen); BEAM_SET (penOverride.cutFillBackgroundPen);
		BEAM_SET (penOverride.overrideCutFillPen); BEAM_SET (penOverride.overrideCutFillBackgroundPen);
	}
	if (ApplyCoverFill (spec, { beam.useCoverFill, beam.useCoverFillFromSurface, beam.coverFillOrientationComesFrom3D,
								beam.coverFillType, beam.coverFillForegroundPen, beam.coverFillBackgroundPen })) {
		BEAM_SET (useCoverFill); BEAM_SET (useCoverFillFromSurface); BEAM_SET (coverFillOrientationComesFrom3D);
		BEAM_SET (coverFillType); BEAM_SET (coverFillForegroundPen); BEAM_SET (coverFillBackgroundPen);
	}
	if (spec.Contains ("lines")) {
		const OS lines = GetObject (spec, "lines");
		if (ApplyLineStyle (lines, "reference", &beam.refPen, &beam.refLtype))								{ BEAM_SET (refPen); BEAM_SET (refLtype); }
		if (ApplyLineStyle (lines, "cutContour", &beam.cutContourLinePen, &beam.cutContourLineType))		{ BEAM_SET (cutContourLinePen); BEAM_SET (cutContourLineType); }
		if (ApplyLineStyle (lines, "uncut", &beam.belowViewLinePen, &beam.belowViewLineType))				{ BEAM_SET (belowViewLinePen); BEAM_SET (belowViewLineType); }
		if (ApplyLineStyle (lines, "overhead", &beam.aboveViewLinePen, &beam.aboveViewLineType))			{ BEAM_SET (aboveViewLinePen); BEAM_SET (aboveViewLineType); }
		if (ApplyLineStyle (lines, "hidden", &beam.hiddenLinePen, &beam.hiddenLineType))					{ BEAM_SET (hiddenLinePen); BEAM_SET (hiddenLineType); }
	}
#undef BEAM_SET

	if (std::hypot (beam.endC.x - beam.begC.x, beam.endC.y - beam.begC.y) < 1e-6)
		Fail ("Beam 'begin' and 'end' must be different points (a vertical member is a column: use create_columns).");
}


SegmentAccess<API_BeamSegmentType> BeamAccess (API_Element& element, API_Element* mask, API_ElementMemo& memo)
{
	API_BeamType& beam = element.beam;
	return SegmentAccess<API_BeamSegmentType> {
		memo.beamSegments, beam.nSegments, beam.nCuts, beam.nSchemes, beam.nProfiles,
		APIMemoMask_BeamSegment, APIAssemblySegmentCut_Vertical, "beam",
		ApplyBeamSegmentFields,
		[mask] () {
			if (mask == nullptr) return;
			ACAPI_ELEMENT_MASK_SET (*mask, API_BeamType, nSegments);
			ACAPI_ELEMENT_MASK_SET (*mask, API_BeamType, nCuts);
			ACAPI_ELEMENT_MASK_SET (*mask, API_BeamType, nSchemes);
			ACAPI_ELEMENT_MASK_SET (*mask, API_BeamType, nProfiles);
		}
	};
}

// --- Holes ------------------------------------------------------------------------------

GS::Array<API_Beam_Hole> ReadHoles (const API_ElementMemo& memo)
{
	GS::Array<API_Beam_Hole> holes;
	if (memo.beamHoles == nullptr || *memo.beamHoles == nullptr)
		return holes;
	const GSSize n = BMGetHandleSize (reinterpret_cast<GSConstHandle> (memo.beamHoles)) / (GSSize) sizeof (API_Beam_Hole);
	for (GSSize i = 0; i < n; ++i)
		holes.Push ((*memo.beamHoles)[i]);
	return holes;
}


void WriteHoles (API_ElementMemo& memo, const GS::Array<API_Beam_Hole>& holes)
{
	if (memo.beamHoles != nullptr)
		BMKillHandle (reinterpret_cast<GSHandle*> (&memo.beamHoles));
	memo.beamHoles = reinterpret_cast<API_Beam_Hole**> (BMAllocateHandle ((GSSize) (holes.GetSize () * sizeof (API_Beam_Hole)), ALLOCATE_CLEAR, 0));
	if (memo.beamHoles == nullptr) {
		if (holes.IsEmpty ())
			return;
		Fail ("Out of memory while writing beam holes.", APIERR_MEMFULL);
	}
	for (UInt32 i = 0; i < holes.GetSize (); ++i)
		(*memo.beamHoles)[i] = holes[i];
}


// Vertical frame of the holes. API_Beam_Hole::centerz is the distance of the hole centre BELOW the beam
// reference axis (verified live on AC26: centerz 0 is the top face for Top* anchors, mid-height for Middle*
// anchors; +0.3 lies outside a 0.6 m bottom-anchored beam). The JSON uses depthBelowTop (from the top face)
// and offsetBelowAxis (= centerz). Sizes come from the first segment's nominal section.
struct HoleFrame {
	bool	known = false;		// false for complex profiles (the top face is not known)
	double	height = 0.0;		// nominal section height
	double	topToAxis = 0.0;	// distance from the top face down to the reference axis
};


HoleFrame BeamHoleFrame (const API_BeamType& beam, const API_BeamSegmentType* segments, UInt32 nSegments)
{
	HoleFrame f;
	if (segments == nullptr || nSegments == 0 || PtrCount (segments) == 0)
		return f;
	const API_AssemblySegmentData& d = segments[0].assemblySegmentData;
	if (d.modelElemStructureType == API_ProfileStructure)
		return f;
	f.height = d.circleBased ? d.nominalWidth : d.nominalHeight;
	const short anchor = (beam.anchorPoint >= 0 && beam.anchorPoint <= 8) ? beam.anchorPoint : 1;
	f.topToAxis = (anchor / 3) * f.height / 2.0;
	f.known = f.height > kEps;
	return f;
}


// Hole defaults: the beam's own hole settings, or the Beam tool defaults when those are empty.
struct HoleDefaults {
	API_BHoleTypeID	type = APIBHole_Rectangular;
	bool			contour = false;
	double			width = 0.0;
	double			height = 0.0;
};


HoleDefaults GetHoleDefaults (const API_BeamType& beam)
{
	HoleDefaults d;
	d.type = beam.holeType;
	d.contour = beam.holeContureOn;
	d.width = beam.holeWidth;
	d.height = beam.holeHeight;
	if (d.width > kEps && d.height > kEps)
		return d;
	API_Element def = NewElement (API_BeamID);
	if (ACAPI_Element_GetDefaults (&def, nullptr) == NoError) {
		if (!(d.width > kEps)) d.width = def.beam.holeWidth;
		if (!(d.height > kEps)) d.height = def.beam.holeHeight;
	}
	return d;
}


API_Beam_Hole HoleFromSpec (const OS& spec, const HoleDefaults& defaults, const HoleFrame& frame, Int32 holeId, double axisLength)
{
	API_Beam_Hole hole;
	BNZeroMemory (&hole, sizeof (hole));
	hole.holeType = spec.Contains ("shape") ? (API_BHoleTypeID) ParseNamed (kHoleTypes, spec, "shape")
											: (spec.Contains ("diameter") ? APIBHole_Circular : defaults.type);
	if (spec.Contains ("diameter") && hole.holeType != APIBHole_Circular)
		Fail ("'diameter' is only valid for shape 'Circular'.");
	hole.holeContureOn = GetBool (spec, "showContour", defaults.contour);
	hole.holeID = holeId;

	hole.centerx = GetDouble (spec, "distanceFromBegin");
	if (hole.centerx < 0.0 || (axisLength > 0.0 && hole.centerx > axisLength + 1e-6))
		Fail ("'distanceFromBegin' must be between 0 and the beam length (" + Num (axisLength) + " m).");

	std::optional<double> width = OptPositive (spec, "diameter");
	if (!width.has_value ())
		width = OptPositive (spec, "width");
	hole.width = width.value_or (defaults.width);
	if (hole.holeType == APIBHole_Circular)
		hole.height = hole.width;
	else
		hole.height = OptPositive (spec, "height").value_or (defaults.height);
	if (!(hole.width > kEps) || !(hole.height > kEps))
		Fail ("Hole size is zero: give 'width' (and 'height' for rectangular holes) or 'diameter' in meters.");

	const bool hasDepth = spec.Contains ("depthBelowTop");
	const bool hasOffset = spec.Contains ("offsetBelowAxis");
	if (hasDepth && hasOffset)
		Fail ("Give either 'depthBelowTop' (from the top face) or 'offsetBelowAxis' (from the reference axis), not both.");
	if (hasOffset) {
		hole.centerz = GetDouble (spec, "offsetBelowAxis");
	} else if (hasDepth) {
		if (!frame.known)
			Fail ("'depthBelowTop' needs a rectangular/circular beam section; for complex-profile beams give 'offsetBelowAxis' (hole centre below the reference axis, m).");
		const double depth = GetDouble (spec, "depthBelowTop");
		if (depth < 0.0 || depth > frame.height + 1e-6)
			Fail ("'depthBelowTop' " + Num (depth) + " m puts the hole centre outside the beam (section height " + Num (frame.height) + " m).");
		hole.centerz = depth - frame.topToAxis;
	} else {
		hole.centerz = frame.known ? frame.height / 2.0 - frame.topToAxis : 0.0;		// centred in the section height
	}
	return hole;
}


// segments: the beam's current (possibly just modified) segment array.
void ApplyHoles (const API_BeamType& beam, const API_BeamSegmentType* segments, API_ElementMemo& memo, const OS& spec)
{
	const double axisLength = BeamAxisLength (beam);
	const HoleFrame frame = BeamHoleFrame (beam, segments, beam.nSegments);
	const HoleDefaults defaults = GetHoleDefaults (beam);
	GS::Array<API_Beam_Hole> holes;
	if (spec.Contains ("holes")) {
		GS::Array<OS> list = GetObjectArray (spec, "holes", true);
		for (UInt32 i = 0; i < list.GetSize (); ++i)
			WithContext (IndexedName ("holes", i), [&] () { holes.Push (HoleFromSpec (list[i], defaults, frame, (Int32) i + 1, axisLength)); });
	} else {
		holes = ReadHoles (memo);
	}

	if (spec.Contains ("removeHoles")) {
		GS::Array<double> ids = GetNumberArray (spec, "removeHoles", true);
		for (double idValue : ids) {
			const Int32 id = (Int32) std::llround (idValue);
			bool found = false;
			for (UInt32 i = holes.GetSize (); i-- > 0;) {
				if (holes[i].holeID == id) { holes.Delete (i); found = true; }
			}
			if (!found)
				Fail ("Beam has no hole with id " + GS::ValueToUniString (id) + ". Read the hole ids with get_element_details.", APIERR_BADINDEX);
		}
	}

	if (spec.Contains ("addHoles")) {
		GS::Array<OS> list = GetObjectArray (spec, "addHoles", true);
		Int32 nextId = 1;
		for (const API_Beam_Hole& h : holes)
			nextId = std::max (nextId, h.holeID + 1);
		for (UInt32 i = 0; i < list.GetSize (); ++i)
			WithContext (IndexedName ("addHoles", i), [&] () { holes.Push (HoleFromSpec (list[i], defaults, frame, nextId++, axisLength)); });
	}

	WriteHoles (memo, holes);
}


OS HoleToJson (const API_Beam_Hole& hole, const HoleFrame& frame)
{
	OS out;
	out.Add ("id", hole.holeID);
	out.Add ("shape", NameOf (kHoleTypes, hole.holeType));
	out.Add ("distanceFromBegin", hole.centerx);
	if (frame.known)
		out.Add ("depthBelowTop", hole.centerz + frame.topToAxis);
	out.Add ("offsetBelowAxis", hole.centerz);
	if (hole.holeType == APIBHole_Circular) {
		out.Add ("diameter", hole.width);
	} else {
		out.Add ("width", hole.width);
		out.Add ("height", hole.height);
	}
	out.Add ("showContour", hole.holeContureOn);
	return out;
}

// --- Beam adapter -----------------------------------------------------------------------

API_Guid CreateBeam (const OS& spec)
{
	if (!Has (spec, "begin") || !Has (spec, "end"))
		Fail ("Beam requires 'begin' and 'end' points {x, y} (the reference axis on the plan, m).");

	API_Element element = NewElement (API_BeamID);
	Memo memo;
	GetDefaults (element, memo.Ptr ());

	ApplyCommonFields (element, nullptr, spec);
	ApplyStoryCreationMode (element.beam.linkToSettings, spec);
	ApplyBeamFields (element.beam, nullptr, spec);

	CheckSegmentMemo (memo->beamSegments, element.beam.nSegments, "beam tool default");
	if (TouchesBeamSegments (spec)) {
		UInt64 memoMask = 0;
		SegmentAccess<API_BeamSegmentType> access = BeamAccess (element, nullptr, *memo);
		ApplySegments (access, *memo, spec, BeamAxisLength (element.beam), memoMask);
	}
	if (TouchesBeamHoles (spec))
		ApplyHoles (element.beam, memo->beamSegments, *memo, spec);

	Check (ACAPI_Element_Create (&element, memo.Ptr ()), "Cannot create beam");
	return element.header.guid;
}


void SerializeBeam (const API_Element& element, OS& out)
{
	const API_BeamType& beam = element.beam;
	const double storyLevel = StoryLevel (element.header.floorInd);
	const double horizontalLength = BeamHorizontalLength (beam);

	out.Add ("begin", CoordObj (beam.begC));
	out.Add ("end", CoordObj (beam.endC));
	out.Add ("length", BeamAxisLength (beam));
	out.Add ("horizontalLength", horizontalLength);
	out.Add ("level", beam.level);
	out.Add ("referenceElevation", storyLevel + beam.level);
	if (beam.isSlanted)
		out.Add ("endReferenceElevation", storyLevel + beam.level + std::hypot (beam.endC.x - beam.begC.x, beam.endC.y - beam.begC.y) * std::tan (beam.slantAngle));
	out.Add ("offset", beam.offset);
	out.Add ("anchor", AnchorName (beam.anchorPoint));
	out.Add ("beamShape", NameOf (kBeamShapes, beam.beamShape));
	AddAngle (out, "arcAngle", beam.curveAngle);
	out.Add ("verticalCurveHeight", beam.verticalCurveHeight);
	out.Add ("slanted", beam.isSlanted);
	AddAngle (out, "slantAngle", beam.slantAngle);
	AddAngle (out, "profileRotationAngle", beam.profileAngle);
	out.Add ("flipped", beam.isFlipped);
	out.Add ("showContourLines", NameOf (kBeamLines, beam.showContourLines));
	out.Add ("showReferenceAxis", NameOf (kBeamLines, beam.showReferenceAxis));
	out.Add ("floorPlanDisplay", NameOf (kDisplayOptions, beam.displayOption));
	out.Add ("viewDepthLimitation", NameOf (kViewDepthLimitations, beam.viewDepthLimitation));
	AddStoryVisibilityJson (out, "showOnStories", beam.visibility, beam.isAutoOnStoryVisibility);
	AddPenOverrideJson (out, beam.penOverride);
	AddCoverFillJson (out, beam.useCoverFill, beam.useCoverFillFromSurface, beam.coverFillOrientationComesFrom3D,
					  beam.coverFillType, beam.coverFillForegroundPen, beam.coverFillBackgroundPen);
	out.Add ("lines", OS ("reference", LineStyleJson (&beam.refPen, &beam.refLtype),
						  "cutContour", LineStyleJson (&beam.cutContourLinePen, &beam.cutContourLineType),
						  "uncut", LineStyleJson (&beam.belowViewLinePen, &beam.belowViewLineType),
						  "overhead", LineStyleJson (&beam.aboveViewLinePen, &beam.aboveViewLineType),
						  "hidden", LineStyleJson (&beam.hiddenLinePen, &beam.hiddenLineType)));
	if (beam.nProfiles > 0)
		out.Add ("customProfileCount", (Int32) beam.nProfiles);

	Memo memo;
	if (ACAPI_Element_GetMemo (element.header.guid, memo.Ptr (), APIMemoMask_BeamSegment | kSegmentMemoBits | APIMemoMask_BeamHole) == NoError) {
		AddSegmentsJson<API_BeamSegmentType> (out, memo->beamSegments, beam.nSegments, *memo, beam.nCuts, beam.nSchemes,
											  BeamAxisLength (beam), AddBeamSegmentFields);
		const HoleFrame frame = BeamHoleFrame (beam, memo->beamSegments, beam.nSegments);
		GS::Array<OS> holes;
		for (const API_Beam_Hole& hole : ReadHoles (*memo))
			holes.Push (HoleToJson (hole, frame));
		out.Add ("holes", holes);
	} else {
		out.Add ("segmentCount", (Int32) beam.nSegments);
	}
}


void ModifyBeam (API_Element& element, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask, const OS& patch)
{
	ApplyBeamFields (element.beam, &mask, patch);
	const bool segments = TouchesBeamSegments (patch);
	const bool holes = TouchesBeamHoles (patch);
	if (!segments && !holes)
		return;

	UInt64 loadMask = 0;
	if (segments) {
		loadMask |= APIMemoMask_BeamSegment | kSegmentMemoBits;
		const UInt32 requested = RequestedSegmentCount (patch);
		if (requested > 0 && requested != element.beam.nSegments && element.beam.nProfiles > 0)
			loadMask |= APIMemoMask_AssemblySegmentProfile;
	}
	if (holes)
		loadMask |= APIMemoMask_BeamHole;
	LoadMemo (element.header.guid, memo, loadMask);

	if (segments) {
		SegmentAccess<API_BeamSegmentType> access = BeamAccess (element, &mask, memo);
		ApplySegments (access, memo, patch, BeamAxisLength (element.beam), memoMask);
	}
	if (holes) {
		// Hole heights are measured with the current segment section (the modified one when segments changed).
		Memo current;
		const API_BeamSegmentType* segs = memo.beamSegments;
		if (!segments && ACAPI_Element_GetMemo (element.header.guid, current.Ptr (), APIMemoMask_BeamSegment) == NoError)
			segs = current->beamSegments;
		ApplyHoles (element.beam, segs, memo, patch);
		memoMask |= APIMemoMask_BeamHole;
	}
}

} // namespace


void RegisterColumnBeamCommands ()
{
	RegisterAdapter ({ API_ColumnID, CreateColumn, SerializeColumn, ModifyColumn });
	RegisterAdapter ({ API_BeamID, CreateBeam, SerializeBeam, ModifyBeam });
}

} // namespace cc
