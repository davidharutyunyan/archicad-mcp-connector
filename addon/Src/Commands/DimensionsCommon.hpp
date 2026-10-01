// *****************************************************************************
// DimensionsCommon.hpp — helpers shared by the dimension adapters (dimensions
// family): enum tables, text (note) / marker fields, element "anchors" used for
// associative dimension points, and small 2D geometry helpers.
//
// Everything lives in cc::dimension so it cannot clash with other families.
// JSON conventions (docs/DEVELOPING.md): meters, DEGREES, camelCase, enums as strings.
// Text sizes and marker sizes are in paper millimeters (Archicad's convention).
// *****************************************************************************

#pragma once

#include "Core/Elements.hpp"
#include "Core/Enums.hpp"

#include <memory>
#include <vector>

namespace cc {
namespace dimension {

// --- Enum tables ---------------------------------------------------------------

inline const NamedValue kTextPos[] = {
	{ "Above",	APIPos_Above },
	{ "In",		APIPos_In },
	{ "Below",	APIPos_Below },
};

inline const NamedValue kTextWay[] = {
	{ "Parallel",		APIDir_Parallel },
	{ "Horizontal",		APIDir_Horizontal },
	{ "Vertical",		APIDir_Vertical },
	{ "General",		APIDir_General },
	{ "Radial",			APIDir_Radial },
	{ "ArcDim",			APIDir_ArcDim },
	{ "Perpendicular",	APIDir_Perpendicular },
};

inline const NamedValue kAppearances[] = {
	{ "Normal",			APIApp_Normal },
	{ "Cumulative",		APIApp_Cumu },
	{ "CumulativeSV",	APIApp_CumuSv },
	{ "Elevation",		APIApp_Elev },
};

inline const NamedValue kWitnessForms[] = {
	{ "None",	APIWtn_None },
	{ "Small",	APIWtn_Small },
	{ "Large",	APIWtn_Large },
	{ "Fixed",	APIWtn_Fix },
};

inline const NamedValue kMarkers[] = {
	{ "CrossLine",		APIMark_CrossLine },
	{ "EmptyCircle",	APIMark_EmptyCirc },
	{ "SlashLine60",	APIMark_SlashLine },
	{ "OpenArrow30",	APIMark_OpenArrow30 },
	{ "ClosedArrow30",	APIMark_ClosArrow30 },
	{ "FullArrow30",	APIMark_FullArrow30 },
	{ "SlashLine45",	APIMark_SlashLine45 },
	{ "CrossCircle",	APIMark_CrossCirc },
	{ "OpenArrow90",	APIMark_OpenArrow90 },
	{ "ClosedArrow90",	APIMark_ClosArrow90 },
	{ "FullArrow90",	APIMark_FullArrow90 },
	{ "FullCircle",		APIMark_FullCirc },
	{ "PepitaCircle",	APIMark_PepitaCirc },
	{ "BandArrow",		APIMark_BandArrow },
	{ "OpenArrow60",	APIMark_OpenArrow60 },
	{ "ClosedArrow60",	APIMark_ClosArrow60 },
	{ "FullArrow60",	APIMark_FullArrow60 },
	{ "SlashLine75",	APIMark_SlashLine75 },
	{ "SlashLine",		APIMark_SlashLine },		// alias of SlashLine60
};

inline const NamedValue kLayouts[] = {
	{ "Legacy",		API_Legacy },
	{ "Flexible",	API_Flexible },
	{ "Centered",	API_Centered },
	{ "Off",		API_LayoutOff },
};

inline const NamedValue kOrigins[] = {
	{ "ProjectZero",		APIDimOrigin_ProjectZero },
	{ "ReferenceLevel1",	APIDimOrigin_RefLevel1 },
	{ "ReferenceLevel2",	APIDimOrigin_RefLevel2 },
	{ "StoredOrigin",		APIDimOrigin_StoredOrigin },
	{ "SeaLevel",			APIDimOrigin_Altitude },
	{ "Altitude",			APIDimOrigin_Altitude },	// alias of SeaLevel
};

// --- Mask helper -----------------------------------------------------------------

// Returns the member of `mask` at the same offset as `field` inside `element`
// (nullptr when mask is null, i.e. on creation). Lets one apply-function set masks
// of nested structs (notes, markers) regardless of where they live in API_Element.
template <typename T>
T* MaskOf (API_Element& element, API_Element* mask, T& field)
{
	if (mask == nullptr)
		return nullptr;
	const std::ptrdiff_t offset = reinterpret_cast<char*> (&field) - reinterpret_cast<char*> (&element);
	return reinterpret_cast<T*> (reinterpret_cast<char*> (mask) + offset);
}

// --- Scalar readers ----------------------------------------------------------------

std::optional<short>	OptPen (const OS& spec, const char* key);			// 1..255, throws otherwise

// --- Text (API_NoteType) -----------------------------------------------------------

// Owns UniStrings handed to the API through element-level contentUStr / prefixUStr pointers
// (the API only reads them during Create/Change; it never frees them).
struct StringPool {
	std::vector<std::unique_ptr<GS::UniString>>	strings;
	GS::UniString*	Add (const GS::UniString& s);
	void			Clear () { strings.clear (); }
};
// Pool for pointers that must stay valid until the core calls ACAPI_Element_Change after an
// adapter's modify callback returned. Cleared at the start of every dimension modify.
StringPool&	PendingModifyStrings ();

// Applies textPen, textSize (mm), textFont (font name or index), textBold, textItalic,
// textUnderline, textFrame, textOpaque (background), and — when textKey != nullptr —
// the custom text spec[textKey] ("" = back to the measured value).
// maskNote may be null (creation). Returns true when anything changed.
bool		ApplyNoteFields (API_NoteType& note, API_NoteType* maskNote, const OS& spec, StringPool& pool,
							 const char* textKey = "text");
// Only the style fields (no custom text), e.g. to propagate a style to every point of a chain.
bool		HasNoteStyleFields (const OS& spec);
// Sets a custom text (nullptr/empty = measured value). `text` must outlive the API call that uses the note.
void		SetNoteCustomText (API_NoteType& note, GS::UniString* text);
// Only the custom text spec[textKey]: a string, or ""/false for the measured value. Returns true when present.
bool		ApplyNoteText (API_NoteType& note, API_NoteType* maskNote, const OS& spec, const char* textKey, StringPool& pool);
void		CopyNoteStyle (const API_NoteType& from, API_NoteType& to);
// {textPen, textSize, textFont, textBold, textItalic, textUnderline, textFrame, textOpaque} (+ customText via textKey)
void		AddNoteJson (OS& out, const API_NoteType& note, const GS::UniString* customText, const char* textKey = "customText");
// Custom text stored in a note (contentUStr if present, else content[]). Empty for measured values.
GS::UniString NoteCustomText (const API_NoteType& note, const GS::UniString* ustr);

// --- Marker (API_MarkerData) ---------------------------------------------------------

// markerType, markerPen, markerSize (mm)
bool		ApplyMarkerFields (API_MarkerData& marker, API_MarkerData* maskMarker, const OS& spec);
void		AddMarkerJson (OS& out, const API_MarkerData& marker);

// --- Geometry ------------------------------------------------------------------------

struct Vec { double x = 0, y = 0; };

inline Vec		V (const API_Coord& c)							{ return { c.x, c.y }; }
inline API_Coord C (const Vec& v)								{ API_Coord c; c.x = v.x; c.y = v.y; return c; }
inline Vec		Sub (const Vec& a, const Vec& b)				{ return { a.x - b.x, a.y - b.y }; }
inline Vec		Add (const Vec& a, const Vec& b)				{ return { a.x + b.x, a.y + b.y }; }
inline Vec		Mul (const Vec& a, double k)					{ return { a.x * k, a.y * k }; }
inline double	Dot (const Vec& a, const Vec& b)				{ return a.x * b.x + a.y * b.y; }
inline double	Cross (const Vec& a, const Vec& b)				{ return a.x * b.y - a.y * b.x; }
inline Vec		LeftNormal (const Vec& d)						{ return { -d.y, d.x }; }
double			Len (const Vec& a);
Vec				Unit (const Vec& a);							// throws on a zero vector
Vec				Rotate (const Vec& a, double radians);
double			Dist (const API_Coord& a, const API_Coord& b);
// Projection of p onto the line through `origin` with unit direction `dir`.
API_Coord		ProjectOnLine (const API_Coord& p, const API_Coord& origin, const Vec& dir);

// Straight or curved reference line of a linear element (Wall, Beam, Line, CurtainWall...).
struct RefLine {
	API_Coord	begin;
	API_Coord	end;
	double		arcAngle = 0.0;		// radians, 0 = straight (positive = counter-clockwise)
	double		thickness = 0.0;	// walls/beams: body width
	bool		isWall = false;
};
// Throws with an actionable message for element types without a reference line.
RefLine			GetRefLine (const API_Element& element);
// Center of the arc from begin to end with the given central angle (radians, != 0).
API_Coord		ArcCenter (const API_Coord& begin, const API_Coord& end, double arcAngle);

// --- Anchors (element points usable as associative dimension points) --------------------

struct Anchor {
	API_Neig		neig;
	API_Coord3D		coord;
	GS::UniString	source;		// "hotspot" | "probe"
};

// Hotspots of the element (ACAPI_Element_GetHotspots) plus probed reference points
// (APIAny_NeigToCoordID on the element's main neig types). Never throws for missing
// hotspots — returns what could be found.
GS::Array<Anchor>	CollectAnchors (const API_Guid& guid);

// Nearest anchor to target (2D) within tolerance; nullptr when none.
const Anchor*		NearestAnchor (const GS::Array<Anchor>& anchors, const API_Coord& target, double tolerance);
// Anchor whose projection on `dir` equals the target's (|Δ| <= tolerance) and which lies
// within maxPerp of the target perpendicular to dir; the closest one wins.
const Anchor*		AnchorAtProjection (const GS::Array<Anchor>& anchors, const API_Coord& target, const Vec& dir,
										double tolerance, double maxPerp);

// Converts a neig into an API_Base (type from the element header; line/special flags as documented for API_Base).
API_Base			BaseFromNeig (const API_Neig& neig);
bool				IsLineNeig (API_NeigID id);
// True for neigs that are plain 2D points (not edges/lines, not 3D-only hotspots).
bool				IsDimensionPointNeig (API_NeigID id);
bool				IsSpecialNeig (API_NeigID id);
GS::UniString		NeigName (API_NeigID id);

// {element, elementType, inIndex, line, special, nodeId?} — or nothing for static points.
bool				IsAssociativeBase (const API_Base& base);
void				AddBaseJson (OS& out, const API_Base& base);

// Raw base from a spec: {element, inIndex, line?, special?}. Returns false when spec has no "inIndex".
bool				RawBaseFromSpec (const OS& spec, API_Base& base);

// --- Memo / element helpers -------------------------------------------------------------

Int32				DimElemCount (const API_ElementMemo& memo, Int32 declared);
// Deletes the contentUStr of every dimElem and kills the handle.
void				FreeDimElems (API_ElementMemo& memo);
// Unicode strings of an existing dimension, read through the optional contentUStr/prefixUStr pointers:
//   Dimension: first = default note; RadialDimension: first = note, second = prefix;
//   LevelDimension: first = note1, second = note2; AngleDimension: first = note.
struct ElementStrings { GS::UniString first, second; };
ElementStrings		ReadElementStrings (const API_Guid& guid, API_ElemTypeID typeID);
// Switches to the Floor Plan database for its lifetime when `enable` and another database is active
// (elements referenced by associative dimensions live on the floor plan).
class PlanDatabaseScope {
public:
	explicit PlanDatabaseScope (bool enable);
	~PlanDatabaseScope ();
	PlanDatabaseScope (const PlanDatabaseScope&) = delete;
	PlanDatabaseScope& operator= (const PlanDatabaseScope&) = delete;
private:
	API_DatabaseInfo	previous;
	bool				switched = false;
};

// True when an element of this type lives only in the model (Floor Plan) — i.e. it is not a 2D drafting
// element (Line, Arc, Circle, PolyLine, Spline, Hatch, Text, Label, Hotspot, dimensions, Picture ...),
// which can exist in any 2D database (details, worksheets, layouts, sections).
bool				IsModelElementType (API_ElemTypeID typeID);
// True when the dimension must be created on the Floor Plan: any of the guids is a model element, or is not found
// in the active database but exists on the Floor Plan. Elements found nowhere are ignored (they fail later with a
// clear error).
bool				NeedsFloorPlan (const GS::Array<API_Guid>& guids);

// Deletes an element created by this family in the current undo scope (used when a verification fails).
void				DeleteCreated (const API_Guid& guid);

// Registration of the level / radial / angle dimension adapters (DimensionsSpecial.cpp).
void				RegisterSpecialDimensionAdapters ();

} // namespace dimension
} // namespace cc
