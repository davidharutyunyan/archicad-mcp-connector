// *****************************************************************************
// ColumnsBeamsSegments — helpers shared by the Column and Beam adapters for the
// AC26 "assembly segment" model: every column/beam consists of nSegments
// segments (memo.columnSegments / memo.beamSegments), nSegments + 1 cuts
// (memo.assemblySegmentCuts: start/bottom, between segments, end/top),
// nSegments length schemes (memo.assemblySegmentSchemes) and optional custom
// profiles (memo.assemblySegmentProfiles, nProfiles entries).
//
// JSON conventions (meters / degrees):
//   section  : shape "Rectangular"|"Circular", width, depth (column) | height (beam),
//              diameter, tapered, endWidth, endDepth|endHeight, endDiameter,
//              buildingMaterial | profile   (composites are not allowed)
//   scheme   : length (fixed, m) | lengthProportion (relative weight, normalized to 1)
//   cut      : {type: "Horizontal"|"Vertical"|"Custom", angle (deg, Custom only)}
// *****************************************************************************

#pragma once

#include "Core/Elements.hpp"
#include "Core/Enums.hpp"

#include <algorithm>
#include <functional>

namespace cc {
namespace cb {

constexpr double kEps = 1e-9;

// --- Enum tables ------------------------------------------------------------------

extern const NamedValue kAnchors[9];
extern const NamedValue kZoneRelations[3];
extern const NamedValue kDisplayOptions[6];
extern const NamedValue kViewDepthLimitations[3];
extern const NamedValue kCutTypes[3];

// --- Error context ------------------------------------------------------------------

// Runs fn; a cc::Error thrown inside gets "<context>: " prepended to its message.
void			WithContext (const GS::UniString& context, const std::function<void ()>& fn);
GS::UniString	IndexedName (const char* arrayName, UInt32 index);		// "segments[2]"
GS::UniString	Num (double value);										// "0.300"

// --- Small field readers -------------------------------------------------------------

// Anchor point: integer 0..8 or a kAnchors name.
std::optional<short>	OptAnchor (const OS& spec, const char* key);
GS::UniString			AnchorName (short anchor);

// Positive number (> 0) / non-negative number (>= 0) with a descriptive error.
std::optional<double>	OptPositive (const OS& spec, const char* key);
std::optional<double>	OptNonNegative (const OS& spec, const char* key);

// Story visibility: "Auto" or {homeStory, allAbove, allBelow, storiesAbove, storiesBelow}.
// Returns true if the field was present.
bool			ApplyStoryVisibility (const OS& spec, const char* key, API_StoryVisibility& visibility, bool& isAuto);
void			AddStoryVisibilityJson (OS& out, const char* key, const API_StoryVisibility& visibility, bool isAuto);

// --- Floor plan attributes ---------------------------------------------------------------

// Pen index 1..255.
std::optional<short>	OptPen (const OS& spec, const char* key);

// lines[key] = {pen?, lineType?}; pen / lineType may be null for "pen only" entries.
// Returns true when lines[key] was present.
bool			ApplyLineStyle (const OS& lines, const char* key, short* pen, API_AttributeIndex* lineType);
OS				LineStyleJson (const short* pen, const API_AttributeIndex* lineType);

// "cutFillPen" / "cutFillBackgroundPen": pen index, or false to use the structure's pens.
bool			ApplyPenOverride (const OS& spec, API_PenOverrideType& penOverride);
void			AddPenOverrideJson (OS& out, const API_PenOverrideType& penOverride);

// "coverFill": {enabled, fill, pen, backgroundPen, fromSurface, orientationFrom3D} (uncut floor plan fill).
struct CoverFillRefs {
	bool&				use;
	bool&				fromSurface;
	bool&				orientationFrom3D;
	API_AttributeIndex&	fill;
	short&				pen;
	short&				backgroundPen;
};
bool			ApplyCoverFill (const OS& spec, const CoverFillRefs& refs);
void			AddCoverFillJson (OS& out, bool use, bool fromSurface, bool orientationFrom3D, API_AttributeIndex fill, short pen, short backgroundPen);

// --- Assembly segment section / scheme / cut ------------------------------------------

struct SectionKeys {
	const char*	depth;			// "depth" (column) | "height" (beam)
	const char*	endDepth;		// "endDepth" | "endHeight"
	short		polyProfileType;	// APICSect_Poly | APIBSect_Poly
	short		normalProfileType;	// APICSect_Normal | APIBSect_Normal
	bool		isColumn;
};

extern const SectionKeys kColumnSectionKeys;
extern const SectionKeys kBeamSectionKeys;

// True when spec contains any section field (shape, width, depth/height, diameter, tapered,
// end sizes, buildingMaterial, profile, composite).
bool			HasSectionFields (const OS& spec, const SectionKeys& keys);
// Applies the section fields to one segment. Throws actionable errors on conflicts.
void			ApplySection (API_AssemblySegmentData& data, const OS& spec, const SectionKeys& keys);
// Adds shape, sizes, taper and structure fields.
void			AddSectionJson (OS& out, const API_AssemblySegmentData& data, const SectionKeys& keys);

// Returns true if "length" or "lengthProportion" was given.
bool			ApplyScheme (API_AssemblySegmentSchemeData& scheme, const OS& spec);
void			AddSchemeJson (OS& out, const API_AssemblySegmentSchemeData& scheme, double actualLength);
// Normalizes proportional schemes so their proportions add up to 1 and checks that the fixed
// lengths fit into totalLength (totalLength <= 0 skips the check).
void			NormalizeSchemes (API_AssemblySegmentSchemeData* schemes, UInt32 count, double totalLength);
// Actual length of every segment for the given total length (fixed lengths + proportional share).
GS::Array<double> SegmentLengths (const API_AssemblySegmentSchemeData* schemes, UInt32 count, double totalLength);

void			ApplyCut (API_AssemblySegmentCutData& cut, const OS& spec);
OS				CutToJson (const API_AssemblySegmentCutData& cut);

// --- Memo arrays ------------------------------------------------------------------------

template <typename T>
UInt32 PtrCount (const T* ptr)
{
	if (ptr == nullptr)
		return 0;
	return (UInt32) (BMGetPtrSize (reinterpret_cast<GSConstPtr> (ptr)) / (GSSize) sizeof (T));
}

// Reallocates a memo pointer array (BMAllocatePtr) to newCount items. Existing items are kept;
// new items copy the last existing item (zeroed when there was none). The old block is released.
template <typename T>
T* ResizeMemoArray (T* old, UInt32 oldCount, UInt32 newCount)
{
	const UInt32 available = std::min (oldCount, PtrCount (old));
	T* result = reinterpret_cast<T*> (BMAllocatePtr ((GSSize) (std::max<UInt32> (newCount, 1) * sizeof (T)), ALLOCATE_CLEAR, 0));
	if (result == nullptr)
		Fail ("Out of memory while resizing segment data.", APIERR_MEMFULL);
	for (UInt32 i = 0; i < newCount && available > 0; ++i)
		result[i] = old[i < available ? i : available - 1];
	if (old != nullptr) {
		GSPtr p = reinterpret_cast<GSPtr> (old);
		BMKillPtr (&p);
	}
	return result;
}

// Resizes the cut array to newSegmentCount + 1 items keeping the start and end cuts at the ends.
// New inner cuts copy the last existing inner cut, or the start cut (Custom replaced by innerDefault).
API_AssemblySegmentCutData* ResizeCuts (API_AssemblySegmentCutData* old, UInt32 oldCount, UInt32 newSegmentCount,
										API_AssemblySegmentCutTypeID innerDefault);

// Drops custom profiles that belong to segments >= newSegmentCount (deleting their images).
// Returns the new number of profiles.
UInt32			FilterProfiles (API_AssemblySegmentProfileData*& profiles, UInt32 count, UInt32 newSegmentCount);

// Validates the segment array of a column/beam memo (must hold nSegments >= 1 items). Cut and scheme
// arrays are repaired by the adapters when their counts do not match.
template <typename TSeg>
void CheckSegmentMemo (const TSeg* segments, UInt32 nSegments, const char* typeName)
{
	if (nSegments == 0 || PtrCount (segments) < nSegments) {
		Fail (GS::UniString ("The ") + typeName + " has no valid segment data (nSegments = " + GS::ValueToUniString ((Int32) nSegments) +
			  "). Check the tool settings in Archicad (or pick another favorite) and retry.", APIERR_GENERAL);
	}
}

} // namespace cb
} // namespace cc
