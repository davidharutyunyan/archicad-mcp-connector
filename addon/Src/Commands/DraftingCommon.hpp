// *****************************************************************************
// DraftingCommon.hpp — helpers shared by the drafting family files
// (Drafting.cpp: lines / arcs / circles / polylines / splines / hatches / hotspots / pictures,
//  DraftingText.cpp: texts / labels). Private to the family: other families must not include it.
// *****************************************************************************

#pragma once

#include "Core/Json.hpp"
#include "Core/Elements.hpp"
#include "Core/Enums.hpp"

#include <cstring>

namespace cc {
namespace drafting {

// --- Masks ---------------------------------------------------------------------------

// Flags EVERY byte of `field` (a member somewhere inside `element`) in `mask`.
// ACAPI_ELEMENT_MASK_SET only flags the first byte of a field, which is not enough for
// sub-structures such as API_ExtendedPenType, API_ArrowData or API_Coord (Archicad checks
// their members one by one). No-op when mask == nullptr (creation).
template <class T>
void MaskField (API_Element* mask, const API_Element& element, const T& field)
{
	if (mask == nullptr)
		return;
	const char* base = reinterpret_cast<const char*> (&element);
	const char* ptr = reinterpret_cast<const char*> (&field);
	const std::ptrdiff_t offset = ptr - base;
	if (offset < 0 || (size_t) offset + sizeof (T) > sizeof (API_Element))
		return;		// not a member of element: programming error, never write outside the mask
	std::memset (reinterpret_cast<char*> (mask) + offset, 0xFF, sizeof (T));
}

// --- Value readers -------------------------------------------------------------------

// Pen index minValue..255 (throws with the allowed range).
std::optional<short>	OptPen (const OS& os, const char* key, Int32 minValue = 1);
// Positive number (throws when <= 0).
std::optional<double>	OptPositive (const OS& os, const char* key);
// Non-negative number.
std::optional<double>	OptNonNegative (const OS& os, const char* key);

// RGB colour: "#RRGGBB" or {r, g, b} with 0-255 components. Returns false-y when the value is
// the boolean false (= remove the colour override); `remove` is set in that case.
bool					OptColor (const OS& os, const char* key, API_RGBColor& color, bool& remove);
GS::UniString			ColorHex (const API_RGBColor& color);

// --- Line-family settings (lines, arcs, circles, polylines, splines) -------------------

struct LineStyleRefs {
	API_ExtendedPenType*	linePen = nullptr;
	API_AttributeIndex*		lineType = nullptr;
	bool*					roomSeparator = nullptr;
	short*					determination = nullptr;
	API_ArrowData*			arrows = nullptr;			// nullptr: type has no arrows (Archicad 27+: the start arrow)
	API_ArrowData*			endArrows = nullptr;		// Archicad 27+ only: the end arrow (26 has one record for both ends)
	double*					penWeight = nullptr;
};

// Applies: pen, colorOverridePen, lineType, lineWeight, category, zoneBoundary, arrows {begin,end,type,size,pen}.
void		ApplyLineStyle (API_Element& element, API_Element* mask, const OS& spec, const LineStyleRefs& refs);
void		AddLineStyleJson (OS& out, const LineStyleRefs& refs);

// Arrow data: {begin, end, type, size (mm), pen}
// endArrows: the separate end-arrow record of Archicad 27+ lines/arcs/polylines/splines (nullptr: labels and Archicad 26).
void		ApplyArrows (API_Element& element, API_Element* mask, const OS& arrowsSpec, API_ArrowData& arrows, API_ArrowData* endArrows = nullptr);
OS			ArrowsJson (const API_ArrowData& arrows, const API_ArrowData* endArrows = nullptr);

// --- Errors --------------------------------------------------------------------------

// Like Check (), but appends a hint for the typical failure causes of drafting elements
// (no 2D window active, locked/hidden layer, bad polygon ...).
void		CheckDrafting (GSErrCode err, const GS::UniString& what);

// Registration of the text / label adapters (DraftingText.cpp).
void		RegisterTextLabelAdapters ();

} // namespace drafting
} // namespace cc
