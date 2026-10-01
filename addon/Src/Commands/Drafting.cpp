// *****************************************************************************
// Drafting — 2D element adapters (create / serialize / modify through the generic
// CreateElements / GetElementDetails / ModifyElements commands).
//
// Units: coordinates and lengths in meters (model), angles in DEGREES, pen weights, arrow and
// text sizes in millimeters (paper). Elements go into the database of the active window
// (floor plan: the given/current story; section, detail, worksheet, layout: that database).
//
// Line-family style fields (Line, Arc, Circle, PolyLine, Spline):
//   pen (1-255), colorOverridePen (0 = none), lineType (Line attribute), lineWeight (mm, -1 = pen's),
//   category "Drafting"|"Cut"|"SkinSeparator", zoneBoundary (bool, acts as zone boundary),
//   arrows {begin, end, type, size (mm), pen}   (not on circles)
//
// Line      begin*, end*
// Arc       center* + radius* + beginAngle* + endAngle*  [+ minorRadius | ratio, axisAngle, reflected]
//           or begin* + end* + arcAngle* (signed sweep, + = counter-clockwise)
//           or begin* + through* + end* (three points)
// Circle    center*, radius*  [+ minorRadius | ratio, axisAngle]   (ellipse when minorRadius != radius)
// PolyLine  points* [{x,y}...], arcs [{index, angle}], closed, continuousPattern
// Spline    points*, closed, directions [{angle, lengthPrev, lengthNext}] (default: smooth auto spline)
//           (modify: settings only — Archicad cannot change spline geometry in place)
// Hatch     polygon* {points, arcs?, holes?}, fillType | buildingMaterial, fillPen, fillColorOverridePen,
//           backgroundPen (0 = transparent), foregroundColor / backgroundColor ("#RRGGBB" | false),
//           fillCategory "Drafting"|"Cut"|"Cover", contour, contourPen, contourColorOverridePen,
//           contourLineType, contourLineWeight, orientation {type, origin, angle, xAxis, yAxis,
//           innerRadius, fitX, fitY, keepProportion}, showArea, areaText {position, pen, font, size, angle}
// Hotspot   position*, height, pen
// Picture   file* (absolute path; PNG/JPEG/GIF/TIFF/BMP), position*, width, height, anchor, angle,
//           mirrored, transparent, name
// (* = required on create)   Text and Label: see DraftingText.cpp.
// + common: layer, storyIndex, renovationStatus, elementId, drawIndex (handled by the core)
// *****************************************************************************

#include "Commands/Commands.hpp"
#include "Commands/DraftingCommon.hpp"
#include "Core/Command.hpp"
#include "Core/Elements.hpp"
#include "Core/Enums.hpp"
#include "Core/Polygon.hpp"

#include "BezierDetails.hpp"
#include "Spline2DData.h"
#include "GXImageBase.h"
#include "Location.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

namespace cc {
namespace drafting {

namespace {

constexpr double kEps = 1e-9;
constexpr double kTwoPi = 2.0 * kPi;

// --- Tables --------------------------------------------------------------------------

const NamedValue kLineCategories[] = {
	{ "Drafting",		APILine_DetOrigin },
	{ "Cut",			APILine_DetContourLine },
	{ "SkinSeparator",	APILine_DetInnerLine },
	{ "Origin",			APILine_DetOrigin },			// aliases (API names)
	{ "Contour",		APILine_DetContourLine },
	{ "Inner",			APILine_DetInnerLine },
};

const NamedValue kArrowTypes[] = {
	{ "EmptyCircle",		APIArr_EmptyCirc },
	{ "CrossCircle",		APIArr_CrossCircIs },
	{ "FullCircle",			APIArr_FullCirc },
	{ "OpenArrow15",		APIArr_OpenArrow15 },
	{ "ClosedArrow15",		APIArr_ClosArrow15 },
	{ "FullArrow15",		APIArr_FullArrow15 },
	{ "OpenArrow30",		APIArr_OpenArrow30 },
	{ "ClosedArrow30",		APIArr_ClosArrow30 },
	{ "FullArrow30",		APIArr_FullArrow30 },
	{ "SlashLine45",		APIArr_SlashLine45 },
	{ "OpenArrow45",		APIArr_OpenArrow45 },
	{ "ClosedArrow45",		APIArr_ClosArrow45 },
	{ "FullArrow45",		APIArr_FullArrow45 },
	{ "SlashLine60",		APIArr_SlashLine60 },
	{ "SlashLine75",		APIArr_SlashLine75 },
	{ "SlashLine90",		APIArr_SlashLine90 },
	{ "PepitaCircle",		APIArr_PepitaCirc },
	{ "BandArrow",			APIArr_BandArrow },
	{ "HalfArrowCcw15",		APIArr_HalfArrowCcw15 },
	{ "HalfArrowCw15",		APIArr_HalfArrowCw15 },
	{ "HalfArrowCcw30",		APIArr_HalfArrowCcw30 },
	{ "HalfArrowCw30",		APIArr_HalfArrowCw30 },
	{ "HalfArrowCcw45",		APIArr_HalfArrowCcw45 },
	{ "HalfArrowCw45",		APIArr_HalfArrowCw45 },
};

const NamedValue kFillCategories[] = {
	{ "Drafting",	APIHatch_DraftingFills },
	{ "Cut",		APIHatch_CutFills },
	{ "Cover",		APIHatch_CoverFills },
};

const NamedValue kHatchOrientations[] = {
	{ "Global",		API_HatchGlobal },
	{ "Rotated",	API_HatchRotated },
	{ "Distorted",	API_HatchDistorted },
	{ "Radial",		API_HatchCentered },
	{ "Centered",	API_HatchCentered },		// alias (API name)
};

const NamedValue kAnchors[] = {
	{ "TopLeft",		APIAnc_LT },
	{ "TopCenter",		APIAnc_MT },
	{ "TopRight",		APIAnc_RT },
	{ "MiddleLeft",		APIAnc_LM },
	{ "Center",			APIAnc_MM },
	{ "MiddleRight",	APIAnc_RM },
	{ "BottomLeft",		APIAnc_LB },
	{ "BottomCenter",	APIAnc_MB },
	{ "BottomRight",	APIAnc_RB },
};

const NamedValue kPictureFormats[] = {
	{ "Default",	APIPictForm_Default },
	{ "Bitmap",		APIPictForm_Bitmap },
	{ "GIF",		APIPictForm_GIF },
	{ "TIFF",		APIPictForm_TIFF },
	{ "JPEG",		APIPictForm_JPEG },
	{ "PNG",		APIPictForm_PNG },
};

// --- Small geometry helpers --------------------------------------------------------------

double NormalizeAngle (double a)		// -> [0, 2pi)
{
	double r = std::fmod (a, kTwoPi);
	if (r < 0) r += kTwoPi;
	return r;
}


double DirAngle (const API_Coord& from, const API_Coord& to)
{
	return std::atan2 (to.y - from.y, to.x - from.x);
}


double Dist (const API_Coord& a, const API_Coord& b)
{
	return std::hypot (b.x - a.x, b.y - a.y);
}


bool SamePoint (const API_Coord& a, const API_Coord& b)
{
	return std::fabs (a.x - b.x) < 1e-9 && std::fabs (a.y - b.y) < 1e-9;
}


// Length of a polyline edge p->q that may be an arc with signed central angle.
double EdgeLength (const API_Coord& p, const API_Coord& q, double arcAngle)
{
	const double chord = Dist (p, q);
	if (std::fabs (arcAngle) < kEps || chord < kEps)
		return chord;
	const double r = chord / (2.0 * std::sin (std::fabs (arcAngle) / 2.0));
	return r * std::fabs (arcAngle);
}


template <class T>
T** AllocHandle (Int32 count)
{
	T** h = reinterpret_cast<T**> (BMAllocateHandle ((GSSize) count * (GSSize) sizeof (T), ALLOCATE_CLEAR, 0));
	if (h == nullptr)
		Fail ("Out of memory.", APIERR_MEMFULL);
	return h;
}


Int32 HandleCount (GSConstHandle h, size_t itemSize)
{
	if (h == nullptr)
		return 0;
	return (Int32) (BMGetHandleSize (h) / (GSSize) itemSize);
}

} // namespace

// =============================================================================
// Shared helpers (declared in DraftingCommon.hpp)
// =============================================================================

std::optional<short> OptPen (const OS& os, const char* key, Int32 minValue)
{
	auto v = OptInt (os, key);
	if (!v.has_value ())
		return std::nullopt;
	if (*v < minValue || *v > 255) {
		GS::UniString msg;
		msg.Printf ("' must be a pen index %d..255.", (int) minValue);
		Fail ("Field '" + GS::UniString (key) + msg);
	}
	return (short) *v;
}


std::optional<double> OptPositive (const OS& os, const char* key)
{
	auto v = OptDouble (os, key);
	if (v.has_value () && !(*v > 0.0))
		Fail ("Field '" + GS::UniString (key) + "' must be a positive number.");
	return v;
}


std::optional<double> OptNonNegative (const OS& os, const char* key)
{
	auto v = OptDouble (os, key);
	if (v.has_value () && *v < 0.0)
		Fail ("Field '" + GS::UniString (key) + "' must not be negative.");
	return v;
}


bool OptColor (const OS& os, const char* key, API_RGBColor& color, bool& remove)
{
	remove = false;
	if (!os.Contains (key))
		return false;
	const GS::UniString field (key);
	if (os.IsBool (key)) {
		if (GetBool (os, key))
			Fail ("Field '" + field + "' must be \"#RRGGBB\", {r, g, b} (0-255) or false to remove the colour.");
		remove = true;
		return true;
	}
	if (os.IsString (key)) {
		GS::UniString s = GetString (os, key);
		s.Trim ();
		if (s.BeginsWith ("#"))
			s = s.GetSubstring (1, s.GetLength () - 1);
		unsigned int r = 0, g = 0, b = 0;
		GS::String cs = ToStr (s);
		if (cs.GetLength () != 6 || std::sscanf (cs.ToCStr (), "%2x%2x%2x", &r, &g, &b) != 3)
			Fail ("Field '" + field + "' must be a colour like \"#FF8800\".");
		color.f_red = r / 255.0;
		color.f_green = g / 255.0;
		color.f_blue = b / 255.0;
		return true;
	}
	OS c = GetObject (os, key);
	const double r = GetDouble (c, "r"), g = GetDouble (c, "g"), b = GetDouble (c, "b");
	for (double v : { r, g, b }) {
		if (v < 0.0 || v > 255.0)
			Fail ("Field '" + field + "': r, g, b must be 0..255.");
	}
	color.f_red = r / 255.0;
	color.f_green = g / 255.0;
	color.f_blue = b / 255.0;
	return true;
}


GS::UniString ColorHex (const API_RGBColor& color)
{
	auto comp = [] (double v) -> int {
		long c = std::lround (v * 255.0);
		return (int) (c < 0 ? 0 : (c > 255 ? 255 : c));
	};
	GS::UniString s;
	s.Printf ("#%02X%02X%02X", comp (color.f_red), comp (color.f_green), comp (color.f_blue));
	return s;
}


void ApplyArrows (API_Element& element, API_Element* mask, const OS& a, API_ArrowData& arrows)
{
	if (auto b = OptBool (a, "begin"))	{ arrows.begArrow = *b; MaskField (mask, element, arrows.begArrow); }
	if (auto b = OptBool (a, "end"))	{ arrows.endArrow = *b; MaskField (mask, element, arrows.endArrow); }
	if (Has (a, "type")) {
		arrows.arrowType = (API_ArrowID) ParseNamed (kArrowTypes, a, "type");
		MaskField (mask, element, arrows.arrowType);
	}
	if (auto s = OptPositive (a, "size"))	{ arrows.arrowSize = *s; MaskField (mask, element, arrows.arrowSize); }
	if (auto p = OptPen (a, "pen"))		{ arrows.arrowPen = *p; MaskField (mask, element, arrows.arrowPen); }
}


OS ArrowsJson (const API_ArrowData& arrows)
{
	OS out;
	out.Add ("begin", arrows.begArrow);
	out.Add ("end", arrows.endArrow);
	out.Add ("type", NameOf (kArrowTypes, arrows.arrowType));
	out.Add ("size", arrows.arrowSize);
	out.Add ("pen", (Int32) arrows.arrowPen);
	return out;
}


void ApplyLineStyle (API_Element& element, API_Element* mask, const OS& spec, const LineStyleRefs& r)
{
	if (auto p = OptPen (spec, "pen")) {
		r.linePen->penIndex = *p;
		MaskField (mask, element, r.linePen->penIndex);
	}
	if (auto p = OptPen (spec, "colorOverridePen", 0)) {
		r.linePen->colorOverridePenIndex = *p;
		MaskField (mask, element, r.linePen->colorOverridePenIndex);
	}
	if (auto lt = OptAttr (API_LinetypeID, spec, "lineType")) {
		*r.lineType = *lt;
		MaskField (mask, element, *r.lineType);
	}
	if (auto w = OptDouble (spec, "lineWeight")) {
		if (!(*w > 0.0) && std::fabs (*w - API_DefPenWeigth) > kEps)
			Fail ("Field 'lineWeight' must be a positive weight in mm, or -1 to use the pen's own weight.");
		*r.penWeight = *w;
		MaskField (mask, element, *r.penWeight);
	}
	if (Has (spec, "category")) {
		*r.determination = (short) ParseNamed (kLineCategories, spec, "category");
		MaskField (mask, element, *r.determination);
	}
	if (auto b = OptBool (spec, "zoneBoundary")) {
		*r.roomSeparator = *b;
		MaskField (mask, element, *r.roomSeparator);
	}
	if (Has (spec, "arrows")) {
		if (r.arrows == nullptr)
			Fail ("This element type has no arrows (remove 'arrows').", APIERR_NOTSUPPORTED);
		ApplyArrows (element, mask, GetObject (spec, "arrows"), *r.arrows);
	}
}


void AddLineStyleJson (OS& out, const LineStyleRefs& r)
{
	out.Add ("pen", (Int32) r.linePen->penIndex);
	out.Add ("colorOverridePen", (Int32) r.linePen->colorOverridePenIndex);
	out.Add ("lineType", AttrRef (API_LinetypeID, *r.lineType));
	out.Add ("lineWeight", *r.penWeight);
	out.Add ("category", NameOf (kLineCategories, *r.determination));
	out.Add ("zoneBoundary", *r.roomSeparator);
	if (r.arrows != nullptr)
		out.Add ("arrows", ArrowsJson (*r.arrows));
}


void CheckDrafting (GSErrCode err, const GS::UniString& what)
{
	if (err == NoError)
		return;
	GS::UniString hint;
	switch (err) {
		case APIERR_BADDATABASE:
		case APIERR_BADWINDOW:
		case APIERR_REFUSEDCMD:
			hint = " Drafting elements are placed into the database of the ACTIVE window: activate a floor plan, section, "
				   "elevation, detail, worksheet or layout window first (not a 3D window, schedule or list).";
			break;
		case APIERR_LOCKEDLAY:
		case APIERR_HIDDENLAY:
		case APIERR_NOTEDITABLE:
			hint = " Use an unlocked, visible layer ('layer' field), or unlock/show the layer first.";
			break;
		case APIERR_BADPOLY:
		case APIERR_IRREGULARPOLY:
			hint = " Check the geometry: no self-intersections, no repeated consecutive points, holes strictly inside the outline.";
			break;
		case APIERR_INVALFLOOR:
			hint = " The story does not exist — list stories with get_stories.";
			break;
		case APIERR_BADINDEX:
		case APIERR_BADNAME:
			hint = " An attribute (pen, line type, fill, font, building material) does not exist — list them with get_attributes.";
			break;
		default:
			break;
	}
	throw Error (what + ": " + ErrorName (err) + "." + hint, err);
}

// =============================================================================
// Line
// =============================================================================

namespace {

LineStyleRefs LineRefs (API_LineType& l)
{
	LineStyleRefs r;
	r.linePen = &l.linePen; r.lineType = &l.ltypeInd; r.roomSeparator = &l.roomSeparator;
	r.determination = &l.determination; r.arrows = &l.arrowData; r.penWeight = &l.penWeight;
	return r;
}


void ApplyLineFields (API_Element& e, API_Element* mask, const OS& spec)
{
	if (auto c = OptCoord (spec, "begin"))	{ e.line.begC = *c; MaskField (mask, e, e.line.begC); }
	if (auto c = OptCoord (spec, "end"))	{ e.line.endC = *c; MaskField (mask, e, e.line.endC); }
	ApplyLineStyle (e, mask, spec, LineRefs (e.line));
	if (SamePoint (e.line.begC, e.line.endC))
		Fail ("Line 'begin' and 'end' must be different points.");
}


API_Guid CreateLine (const OS& spec)
{
	if (!Has (spec, "begin") || !Has (spec, "end"))
		Fail ("Line requires 'begin' and 'end' points {x, y} in meters.");
	API_Element e = NewElement (API_LineID);
	GetDefaults (e, nullptr);
	ApplyCommonFields (e, nullptr, spec);
	ApplyLineFields (e, nullptr, spec);
	CheckDrafting (ACAPI_Element_Create (&e, nullptr), "Cannot create line");
	return e.header.guid;
}


void SerializeLine (const API_Element& element, OS& out)
{
	API_Element e = element;
	out.Add ("begin", CoordObj (e.line.begC));
	out.Add ("end", CoordObj (e.line.endC));
	out.Add ("length", Dist (e.line.begC, e.line.endC));
	AddAngle (out, "direction", DirAngle (e.line.begC, e.line.endC));
	AddLineStyleJson (out, LineRefs (e.line));
}


void ModifyLine (API_Element& element, API_Element& mask, API_ElementMemo&, UInt64&, const OS& patch)
{
	ApplyLineFields (element, &mask, patch);
}

// =============================================================================
// Arc / Circle
// =============================================================================

LineStyleRefs ArcRefs (API_ArcType& a, bool withArrows)
{
	LineStyleRefs r;
	r.linePen = &a.linePen; r.lineType = &a.ltypeInd; r.roomSeparator = &a.roomSeparator;
	r.determination = &a.determination; r.arrows = withArrows ? &a.arrowData : nullptr; r.penWeight = &a.penWeight;
	return r;
}


// Circular arc through begin -> end with a signed sweep (radians, + = CCW).
void ArcFromChord (API_ArcType& arc, API_Coord b, API_Coord e, double sweep)
{
	if (std::fabs (sweep) < 1e-6 || std::fabs (sweep) >= kTwoPi - 1e-6)
		Fail ("'arcAngle' must be between -360 and 360 degrees and not 0 (use create_circles for full circles).");
	if (SamePoint (b, e))
		Fail ("Arc 'begin' and 'end' must be different points.");
	if (sweep < 0) {			// clockwise from b to e == counter-clockwise from e to b
		std::swap (b, e);
		sweep = -sweep;
	}
	const double chord = Dist (b, e);
	const API_Coord mid = { (b.x + e.x) / 2.0, (b.y + e.y) / 2.0 };
	const double ux = (e.x - b.x) / chord, uy = (e.y - b.y) / chord;
	const double nx = -uy, ny = ux;							// left normal of b->e
	const double d = (chord / 2.0) / std::tan (sweep / 2.0);	// signed distance of the centre from the chord
	arc.origC = { mid.x + nx * d, mid.y + ny * d };
	arc.r = chord / (2.0 * std::sin (sweep / 2.0));
	arc.begAng = NormalizeAngle (DirAngle (arc.origC, b));
	arc.endAng = NormalizeAngle (DirAngle (arc.origC, e));
	arc.angle = 0.0;
	arc.ratio = 1.0;
}


void ArcFromThreePoints (API_ArcType& arc, API_Coord b, const API_Coord& t, API_Coord e)
{
	const double ax = b.x, ay = b.y, bx = t.x, by = t.y, cx = e.x, cy = e.y;
	const double d = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
	if (std::fabs (d) < 1e-12)
		Fail ("Arc points 'begin', 'through' and 'end' are collinear (or coincide) — they do not define an arc.");
	const double a2 = ax * ax + ay * ay, b2 = bx * bx + by * by, c2 = cx * cx + cy * cy;
	arc.origC.x = (a2 * (by - cy) + b2 * (cy - ay) + c2 * (ay - by)) / d;
	arc.origC.y = (a2 * (cx - bx) + b2 * (ax - cx) + c2 * (bx - ax)) / d;
	arc.r = Dist (arc.origC, b);
	// Going counter-clockwise from b we meet t before e iff (b, t, e) is a CCW triangle.
	const double cross = (t.x - b.x) * (e.y - b.y) - (t.y - b.y) * (e.x - b.x);
	if (cross < 0)
		std::swap (b, e);
	arc.begAng = NormalizeAngle (DirAngle (arc.origC, b));
	arc.endAng = NormalizeAngle (DirAngle (arc.origC, e));
	arc.angle = 0.0;
	arc.ratio = 1.0;
}


void ApplyArcShape (API_Element& e, API_Element* mask, const OS& spec, bool isCircle)
{
	API_ArcType& arc = isCircle ? e.circle : e.arc;
	const bool pointForm = Has (spec, "begin") || Has (spec, "end") || Has (spec, "through") || Has (spec, "arcAngle");

	if (pointForm) {
		if (isCircle)
			Fail ("Circles are defined by 'center' and 'radius' (begin/end/through/arcAngle are for arcs).");
		if (Has (spec, "center") || Has (spec, "radius") || Has (spec, "beginAngle") || Has (spec, "endAngle"))
			Fail ("Give EITHER center + radius + beginAngle + endAngle, OR begin + end + arcAngle, OR begin + through + end.");
		if (!Has (spec, "begin") || !Has (spec, "end"))
			Fail ("Arc point form needs 'begin' and 'end' plus 'arcAngle' or 'through'.");
		const API_Coord b = GetCoord (spec, "begin");
		const API_Coord en = GetCoord (spec, "end");
		if (Has (spec, "through") && Has (spec, "arcAngle"))
			Fail ("Give either 'through' or 'arcAngle', not both.");
		if (Has (spec, "through"))
			ArcFromThreePoints (arc, b, GetCoord (spec, "through"), en);
		else if (Has (spec, "arcAngle"))
			ArcFromChord (arc, b, en, GetAngle (spec, "arcAngle"));
		else
			Fail ("Arc point form needs 'arcAngle' (signed sweep in degrees) or a 'through' point.");
		if (Has (spec, "minorRadius") || Has (spec, "ratio") || Has (spec, "axisAngle"))
			Fail ("minorRadius / ratio / axisAngle (elliptical arcs) only work with the center + radius form.");
		MaskField (mask, e, arc.origC);
		MaskField (mask, e, arc.r);
		MaskField (mask, e, arc.begAng);
		MaskField (mask, e, arc.endAng);
		MaskField (mask, e, arc.angle);
		MaskField (mask, e, arc.ratio);
	} else {
		if (isCircle && (Has (spec, "beginAngle") || Has (spec, "endAngle")))
			Fail ("Circles have no begin/end angles — use create_arcs for arcs.");
		if (auto c = OptCoord (spec, "center"))		{ arc.origC = *c; MaskField (mask, e, arc.origC); }
		if (auto v = OptPositive (spec, "radius"))		{ arc.r = *v; MaskField (mask, e, arc.r); }
		if (Has (spec, "minorRadius") && Has (spec, "ratio"))
			Fail ("Give either 'minorRadius' or 'ratio', not both.");
		if (auto v = OptPositive (spec, "minorRadius"))	{ arc.ratio = arc.r / *v; MaskField (mask, e, arc.ratio); }
		if (auto v = OptPositive (spec, "ratio"))		{ arc.ratio = *v; MaskField (mask, e, arc.ratio); }
		if (Has (spec, "radius") && !Has (spec, "minorRadius") && !Has (spec, "ratio") && mask == nullptr)
			{ arc.ratio = 1.0; }
		if (auto v = OptAngle (spec, "axisAngle"))		{ arc.angle = *v; MaskField (mask, e, arc.angle); }
		if (auto v = OptAngle (spec, "beginAngle"))	{ arc.begAng = *v; MaskField (mask, e, arc.begAng); }
		if (auto v = OptAngle (spec, "endAngle"))		{ arc.endAng = *v; MaskField (mask, e, arc.endAng); }
	}
	if (auto v = OptBool (spec, "reflected"))		{ arc.reflected = *v; MaskField (mask, e, arc.reflected); }

	if (!isCircle) {
		const double sweep = NormalizeAngle (arc.endAng - arc.begAng);
		if (sweep < 1e-9)
			Fail ("Arc 'beginAngle' and 'endAngle' must differ (for a full circle use create_circles).");
	}
}


API_Guid CreateArcOrCircle (const OS& spec, bool isCircle)
{
	API_Element e = NewElement (isCircle ? API_CircleID : API_ArcID);
	if (ACAPI_Element_GetDefaults (&e, nullptr) != NoError) {
		// Circles and arcs share one tool and one struct: fall back to the arc defaults.
		e = NewElement (API_ArcID);
		GetDefaults (e, nullptr);
		e.header.type = API_ElemType (isCircle ? API_CircleID : API_ArcID);
	}
	API_ArcType& arc = isCircle ? e.circle : e.arc;
	// Start from a clean circular shape: the tool defaults only matter for the style.
	arc.angle = 0.0;
	arc.ratio = 1.0;
	arc.begAng = 0.0;
	arc.endAng = 0.0;
	arc.reflected = false;

	const bool pointForm = Has (spec, "begin") || Has (spec, "through") || Has (spec, "arcAngle");
	if (!pointForm) {
		if (!Has (spec, "center") || !Has (spec, "radius"))
			Fail (isCircle ? GS::UniString ("Circle requires 'center' {x, y} and 'radius' (m).")
						   : GS::UniString ("Arc requires center + radius + beginAngle + endAngle, or begin + end + arcAngle, or begin + through + end."));
		if (!isCircle && (!Has (spec, "beginAngle") || !Has (spec, "endAngle")))
			Fail ("Arc with 'center' and 'radius' also needs 'beginAngle' and 'endAngle' (degrees, counter-clockwise from +X).");
	}
	ApplyCommonFields (e, nullptr, spec);
	ApplyArcShape (e, nullptr, spec, isCircle);
	ApplyLineStyle (e, nullptr, spec, ArcRefs (arc, !isCircle));
	CheckDrafting (ACAPI_Element_Create (&e, nullptr), isCircle ? "Cannot create circle" : "Cannot create arc");
	return e.header.guid;
}


void SerializeArcOrCircle (const API_Element& element, OS& out, bool isCircle)
{
	API_Element e = element;
	API_ArcType& arc = isCircle ? e.circle : e.arc;
	out.Add ("center", CoordObj (arc.origC));
	out.Add ("radius", arc.r);
	const double ratio = arc.ratio > kEps ? arc.ratio : 1.0;
	out.Add ("minorRadius", arc.r / ratio);
	out.Add ("ratio", ratio);
	AddAngle (out, "axisAngle", arc.angle);
	out.Add ("reflected", arc.reflected);
	const bool circular = std::fabs (ratio - 1.0) < 1e-9;
	if (isCircle) {
		if (circular) {
			out.Add ("circumference", kTwoPi * arc.r);
			out.Add ("area", kPi * arc.r * arc.r);
		} else {
			out.Add ("area", kPi * arc.r * (arc.r / ratio));
		}
	} else {
		AddAngle (out, "beginAngle", arc.begAng);
		AddAngle (out, "endAngle", arc.endAng);
		const double sweep = NormalizeAngle (arc.endAng - arc.begAng);
		AddAngle (out, "arcAngle", sweep < 1e-12 ? kTwoPi : sweep);
		out.Add ("whole", arc.whole);
		if (circular && std::fabs (arc.angle) < 1e-12) {
			out.Add ("begin", CoordObj (arc.origC.x + arc.r * std::cos (arc.begAng), arc.origC.y + arc.r * std::sin (arc.begAng)));
			out.Add ("end", CoordObj (arc.origC.x + arc.r * std::cos (arc.endAng), arc.origC.y + arc.r * std::sin (arc.endAng)));
			out.Add ("length", arc.r * (sweep < 1e-12 ? kTwoPi : sweep));
		}
	}
	AddLineStyleJson (out, ArcRefs (arc, !isCircle));
}


API_Guid CreateArc (const OS& spec)		{ return CreateArcOrCircle (spec, false); }
API_Guid CreateCircle (const OS& spec)		{ return CreateArcOrCircle (spec, true); }
void SerializeArc (const API_Element& e, OS& out)	{ SerializeArcOrCircle (e, out, false); }
void SerializeCircle (const API_Element& e, OS& out)	{ SerializeArcOrCircle (e, out, true); }

void ModifyArc (API_Element& e, API_Element& mask, API_ElementMemo&, UInt64&, const OS& patch)
{
	ApplyArcShape (e, &mask, patch, false);
	ApplyLineStyle (e, &mask, patch, ArcRefs (e.arc, true));
}

void ModifyCircle (API_Element& e, API_Element& mask, API_ElementMemo&, UInt64&, const OS& patch)
{
	ApplyArcShape (e, &mask, patch, true);
	ApplyLineStyle (e, &mask, patch, ArcRefs (e.circle, false));
}

// =============================================================================
// PolyLine
// =============================================================================

LineStyleRefs PolyLineRefs (API_PolyLineType& p)
{
	LineStyleRefs r;
	r.linePen = &p.linePen; r.lineType = &p.ltypeInd; r.roomSeparator = &p.roomSeparator;
	r.determination = &p.determination; r.arrows = &p.arrowData; r.penWeight = &p.penWeight;
	return r;
}


// points (+ arcs, closed) -> open contour; a closed polyline repeats its first point at the end.
Contour ParsePolylinePoints (const OS& spec)
{
	GS::Array<OS> pts = GetObjectArray (spec, "points");
	const bool closed = GetBool (spec, "closed", false);
	if (closed) {
		if (pts.GetSize () < 3)
			Fail ("A closed polyline needs at least 3 points.");
		if (!SamePoint (CoordFrom (pts[0]), CoordFrom (pts[pts.GetSize () - 1])))
			pts.Push (pts[0]);
	}
	OS inner;
	inner.Add ("points", pts);
	if (Has (spec, "arcs"))
		inner.Add ("arcs", GetObjectArray (spec, "arcs"));
	OS wrapper;
	wrapper.Add ("points", inner);
	Contour c = GetPolyline (wrapper, "points");
	for (UIndex i = 1; i < c.points.GetSize (); ++i) {
		if (SamePoint (c.points[i - 1], c.points[i]))
			Fail ("Polyline has two identical consecutive points (index " + GS::ValueToUniString ((Int32) i) + ").");
	}
	return c;
}


void ApplyPolyLineFields (API_Element& e, API_Element* mask, const OS& spec, API_ElementMemo& memo, UInt64& memoMask)
{
	ApplyLineStyle (e, mask, spec, PolyLineRefs (e.polyLine));
	if (auto b = OptBool (spec, "continuousPattern")) {
		e.polyLine.drawSegmentMode = *b ? 1 : 0;
		MaskField (mask, e, e.polyLine.drawSegmentMode);
	}
	if (Has (spec, "points")) {
		Contour c = ParsePolylinePoints (spec);
		WritePolylineToMemo (c, e.polyLine.poly, memo);
		// Vertex IDs like Archicad's own polylines (a closed polyline's last vertex is its first one).
		const Int32 n = (Int32) c.points.GetSize ();
		const bool closed = n > 2 && SamePoint (c.points[0], c.points[n - 1]);
		if (memo.vertexIDs != nullptr)
			BMKillHandle (reinterpret_cast<GSHandle*> (&memo.vertexIDs));
		memo.vertexIDs = AllocHandle<UInt32> (n + 1);
		for (Int32 i = 1; i <= n; ++i)
			(*memo.vertexIDs)[i] = (UInt32) i;
		if (closed)
			(*memo.vertexIDs)[n] = 1;
		(*memo.vertexIDs)[0] = (UInt32) (closed ? n - 1 : n);		// highest vertex ID
		MaskField (mask, e, e.polyLine.poly);
		memoMask |= APIMemoMask_Polygon;
	} else if (Has (spec, "arcs") || Has (spec, "closed")) {
		Fail ("Pass 'points' together with 'arcs' / 'closed': the whole polyline geometry is replaced.");
	}
}


API_Guid CreatePolyLine (const OS& spec)
{
	if (!Has (spec, "points"))
		Fail ("PolyLine requires 'points' (at least 2 {x, y} in meters).");
	API_Element e = NewElement (API_PolyLineID);
	GetDefaults (e, nullptr);
	ApplyCommonFields (e, nullptr, spec);
	Memo memo;
	UInt64 memoMask = 0;
	ApplyPolyLineFields (e, nullptr, spec, *memo, memoMask);
	CheckDrafting (ACAPI_Element_Create (&e, memo.Ptr ()), "Cannot create polyline");
	return e.header.guid;
}


void SerializePolyLine (const API_Element& element, OS& out)
{
	API_Element e = element;
	Memo memo;
	if (ACAPI_Element_GetMemo (e.header.guid, memo.Ptr (), APIMemoMask_Polygon) == NoError && memo->coords != nullptr) {
		OS geom = PolylineToJson (e.polyLine.poly, *memo);
		GS::Array<OS> pts = GetObjectArray (geom, "points", false);
		GS::Array<OS> arcs = GetObjectArray (geom, "arcs", false);
		out.Add ("points", pts);
		if (!arcs.IsEmpty ())
			out.Add ("arcs", arcs);

		const Int32 n = e.polyLine.poly.nCoords;
		const Int32 available = HandleCount (reinterpret_cast<GSConstHandle> (memo->coords), sizeof (API_Coord)) - 1;
		const Int32 count = n < available ? n : available;
		double length = 0.0;
		for (Int32 i = 1; i < count; ++i) {
			double angle = 0.0;
			if (memo->parcs != nullptr) {
				for (Int32 a = 0; a < e.polyLine.poly.nArcs; ++a) {
					if ((*memo->parcs)[a].begIndex == i)
						angle = (*memo->parcs)[a].arcAngle;
				}
			}
			length += EdgeLength ((*memo->coords)[i], (*memo->coords)[i + 1], angle);
		}
		out.Add ("length", length);
		out.Add ("closed", count > 2 && SamePoint ((*memo->coords)[1], (*memo->coords)[count]));
	}
	out.Add ("continuousPattern", e.polyLine.drawSegmentMode != 0);
	AddLineStyleJson (out, PolyLineRefs (e.polyLine));
}


void ModifyPolyLine (API_Element& e, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask, const OS& patch)
{
	ApplyPolyLineFields (e, &mask, patch, memo, memoMask);
}

// =============================================================================
// Spline
// =============================================================================

LineStyleRefs SplineRefs (API_SplineType& s)
{
	LineStyleRefs r;
	r.linePen = &s.linePen; r.lineType = &s.ltypeInd; r.roomSeparator = &s.roomSeparator;
	r.determination = &s.determination; r.arrows = &s.arrowData; r.penWeight = &s.penWeight;
	return r;
}

static_assert (sizeof (API_SplineDir) == sizeof (Geometry::DirType), "API_SplineDir must match Geometry::DirType");
static_assert (sizeof (API_Coord) == sizeof (Coord), "API_Coord must match Coord");


API_Guid CreateSpline (const OS& spec)
{
	if (!Has (spec, "points"))
		Fail ("Spline requires 'points' (at least 2 {x, y} control points in meters).");
	GS::Array<OS> pts = GetObjectArray (spec, "points");
	const bool closed = GetBool (spec, "closed", false);
	GS::Array<API_Coord> coords;
	for (const OS& p : pts)
		coords.Push (CoordFrom (p));
	if (closed && coords.GetSize () >= 2 && SamePoint (coords[0], coords[coords.GetSize () - 1]))
		coords.Pop ();		// a closed spline does not repeat its first point
	const Int32 n = (Int32) coords.GetSize ();
	if (n < (closed ? 3 : 2))
		Fail (closed ? GS::UniString ("A closed spline needs at least 3 points.") : GS::UniString ("A spline needs at least 2 points."));
	for (Int32 i = 1; i < n; ++i) {
		if (SamePoint (coords[i - 1], coords[i]))
			Fail ("Spline has two identical consecutive points (index " + GS::ValueToUniString (i) + ").");
	}

	GS::Array<OS> dirs = GetObjectArray (spec, "directions", false);
	if (!dirs.IsEmpty () && (Int32) dirs.GetSize () != n)
		Fail ("'directions' must have exactly one entry per point (" + GS::ValueToUniString (n) + ").");

	API_Element e = NewElement (API_SplineID);
	GetDefaults (e, nullptr);
	ApplyCommonFields (e, nullptr, spec);
	ApplyLineStyle (e, nullptr, spec, SplineRefs (e.spline));
	e.spline.closed = closed;

	Memo memo;
	memo->coords = AllocHandle<API_Coord> (n);			// splines use 0-based coordinates (no dummy [0])
	memo->bezierDirs = AllocHandle<API_SplineDir> (n);
	for (Int32 i = 0; i < n; ++i)
		(*memo->coords)[i] = coords[i];

	if (dirs.IsEmpty ()) {
		e.spline.autoSmooth = true;
		if (!Geometry::CalcSpline (reinterpret_cast<const Coord*> (*memo->coords),
								   reinterpret_cast<Geometry::DirType*> (*memo->bezierDirs), n, closed))
			Fail ("Cannot compute smooth spline directions for these points (try other points or give 'directions').");
	} else {
		e.spline.autoSmooth = false;
		for (Int32 i = 0; i < n; ++i) {
			API_SplineDir& d = (*memo->bezierDirs)[i];
			d.dirAng = GetAngle (dirs[i], "angle");
			const double lp = GetDouble (dirs[i], "lengthPrev", 0.0);
			const double ln = GetDouble (dirs[i], "lengthNext", 0.0);
			if (lp < 0 || ln < 0)
				Fail ("Spline direction lengths must not be negative.");
			d.lenPrev = lp;
			d.lenNext = ln;
		}
	}
	CheckDrafting (ACAPI_Element_Create (&e, memo.Ptr ()), "Cannot create spline");
	return e.header.guid;
}


void SerializeSpline (const API_Element& element, OS& out)
{
	API_Element e = element;
	Memo memo;
	GSErrCode err = ACAPI_Element_GetMemo (e.header.guid, memo.Ptr (), APIMemoMask_Polygon);
	if (err == NoError && memo->bezierDirs == nullptr) {
		ACAPI_DisposeElemMemoHdls (memo.Ptr ());
		BNZeroMemory (memo.Ptr (), sizeof (API_ElementMemo));
		err = ACAPI_Element_GetMemo (e.header.guid, memo.Ptr (), APIMemoMask_All);
	}
	if (err == NoError && memo->coords != nullptr) {
		const Int32 n = HandleCount (reinterpret_cast<GSConstHandle> (memo->coords), sizeof (API_Coord));
		const Int32 nDirs = HandleCount (reinterpret_cast<GSConstHandle> (memo->bezierDirs), sizeof (API_SplineDir));
		GS::Array<OS> pts, dirs;
		for (Int32 i = 0; i < n; ++i)
			pts.Push (CoordObj ((*memo->coords)[i]));
		for (Int32 i = 0; i < nDirs && i < n; ++i) {
			const API_SplineDir& d = (*memo->bezierDirs)[i];
			OS dj ("angle", RadToDeg (d.dirAng), "lengthPrev", d.lenPrev, "lengthNext", d.lenNext);
			dirs.Push (dj);
		}
		out.Add ("points", pts);
		if (!dirs.IsEmpty ())
			out.Add ("directions", dirs);
	}
	out.Add ("closed", e.spline.closed);
	out.Add ("autoSmooth", e.spline.autoSmooth);
	AddLineStyleJson (out, SplineRefs (e.spline));
}


void ModifySpline (API_Element& e, API_Element& mask, API_ElementMemo&, UInt64&, const OS& patch)
{
	for (const char* key : { "points", "directions", "closed", "autoSmooth" }) {
		if (Has (patch, key))
			Fail ("Archicad cannot change the geometry of an existing spline ('" + GS::UniString (key) +
				  "'). Read it with get_element_details, create the new shape with create_splines (same style fields) and delete the old one.",
				  APIERR_NOTSUPPORTED);
	}
	ApplyLineStyle (e, &mask, patch, SplineRefs (e.spline));
}

// =============================================================================
// Hatch (fill)
// =============================================================================

void ApplyOrientation (API_Element& e, API_Element* mask, const OS& o)
{
	API_HatchOrientation& ho = e.hatch.hatchOrientation;
	if (Has (o, "type")) {
		ho.type = (API_HatchOrientationTypeID) ParseNamed (kHatchOrientations, o, "type");
		MaskField (mask, e, ho.type);
	}
	if (auto c = OptCoord (o, "origin")) {
		ho.origo = *c;
		ho.flags &= ~APIHatchOrinFlag_GlobalOrigo;		// the origin is local now
		MaskField (mask, e, ho.origo);
		MaskField (mask, e, ho.flags);
	}
	const bool hasAxes = Has (o, "xAxis") || Has (o, "yAxis");
	if (hasAxes && Has (o, "angle"))
		Fail ("orientation: give either 'angle' (rotation) or 'xAxis'/'yAxis' (distortion vectors), not both.");
	if (auto a = OptAngle (o, "angle")) {
		const double s = std::sin (*a), c = std::cos (*a);
		ho.matrix00 = c; ho.matrix10 = s; ho.matrix01 = -s; ho.matrix11 = c;
		if (!Has (o, "type") && ho.type == API_HatchGlobal) {
			ho.type = API_HatchRotated;
			MaskField (mask, e, ho.type);
		}
		MaskField (mask, e, ho.matrix00); MaskField (mask, e, ho.matrix10);
		MaskField (mask, e, ho.matrix01); MaskField (mask, e, ho.matrix11);
	}
	if (hasAxes) {
		if (!Has (o, "xAxis") || !Has (o, "yAxis"))
			Fail ("orientation: 'xAxis' and 'yAxis' must be given together.");
		const API_Coord x = GetCoord (o, "xAxis"), y = GetCoord (o, "yAxis");
		if (std::fabs (x.x * y.y - x.y * y.x) < 1e-12)
			Fail ("orientation: 'xAxis' and 'yAxis' must not be parallel or zero.");
		ho.matrix00 = x.x; ho.matrix10 = x.y; ho.matrix01 = y.x; ho.matrix11 = y.y;
		if (!Has (o, "type")) {
			ho.type = API_HatchDistorted;
			MaskField (mask, e, ho.type);
		}
		MaskField (mask, e, ho.matrix00); MaskField (mask, e, ho.matrix10);
		MaskField (mask, e, ho.matrix01); MaskField (mask, e, ho.matrix11);
	}
	if (auto r = OptNonNegative (o, "innerRadius")) {
		ho.innerRadius = *r;
		if (*r > 0) ho.flags |= APIHatchOrinFlag_UseInnerRadius; else ho.flags &= ~APIHatchOrinFlag_UseInnerRadius;
		MaskField (mask, e, ho.innerRadius);
		MaskField (mask, e, ho.flags);
	}
	auto flag = [&] (const char* key, Int32 bit) {
		if (auto b = OptBool (o, key)) {
			if (*b) ho.flags |= bit; else ho.flags &= ~bit;
			MaskField (mask, e, ho.flags);
		}
	};
	flag ("fitX", APIHatchOrinFlag_FitX);
	flag ("fitY", APIHatchOrinFlag_FitY);
	flag ("keepProportion", APIHatchOrinFlag_KeepProportion);
	flag ("globalOrigin", APIHatchOrinFlag_GlobalOrigo);
}


OS OrientationJson (const API_HatchOrientation& ho)
{
	OS out;
	out.Add ("type", NameOf (kHatchOrientations, ho.type));
	out.Add ("origin", CoordObj (ho.origo));
	AddAngle (out, "angle", std::atan2 (ho.matrix10, ho.matrix00));
	out.Add ("xAxis", CoordObj (ho.matrix00, ho.matrix10));
	out.Add ("yAxis", CoordObj (ho.matrix01, ho.matrix11));
	out.Add ("innerRadius", ho.innerRadius);
	out.Add ("globalOrigin", (ho.flags & APIHatchOrinFlag_GlobalOrigo) == APIHatchOrinFlag_GlobalOrigo);
	out.Add ("useInnerRadius", (ho.flags & APIHatchOrinFlag_UseInnerRadius) != 0);
	out.Add ("fitX", (ho.flags & APIHatchOrinFlag_FitX) != 0);
	out.Add ("fitY", (ho.flags & APIHatchOrinFlag_FitY) != 0);
	out.Add ("keepProportion", (ho.flags & APIHatchOrinFlag_KeepProportion) != 0);
	return out;
}


API_Coord OutlineCenter (const PolygonData& poly)
{
	double xMin = 0, xMax = 0, yMin = 0, yMax = 0;
	bool first = true;
	for (const API_Coord& c : poly.outline.points) {
		if (first || c.x < xMin) xMin = c.x;
		if (first || c.x > xMax) xMax = c.x;
		if (first || c.y < yMin) yMin = c.y;
		if (first || c.y > yMax) yMax = c.y;
		first = false;
	}
	return { (xMin + xMax) / 2.0, (yMin + yMax) / 2.0 };
}


void ApplyHatchFields (API_Element& e, API_Element* mask, const OS& spec, API_ElementMemo& memo, UInt64& memoMask)
{
	API_HatchType& h = e.hatch;
	auto setFlag = [&] (UInt32 bit, bool on) {
		if (on) h.hatchFlags |= bit; else h.hatchFlags &= ~bit;
		MaskField (mask, e, h.hatchFlags);
	};

	// --- fill source
	const bool bmGiven = Has (spec, "buildingMaterial");
	const bool fillGiven = Has (spec, "fillType");
	if (bmGiven) {
		h.buildingMaterial = GetAttr (API_BuildingMaterialID, spec, "buildingMaterial");
		h.hatchType = API_BuildingMaterialHatch;
		MaskField (mask, e, h.buildingMaterial);
		MaskField (mask, e, h.hatchType);
	}
	if (fillGiven) {
		h.fillInd = GetAttr (API_FilltypeID, spec, "fillType");
		MaskField (mask, e, h.fillInd);
		if (bmGiven) {
			setFlag (APIHatch_OverrideFillInd, true);
		} else {
			h.hatchType = API_FillHatch;
			MaskField (mask, e, h.hatchType);
			setFlag (APIHatch_OverrideFillInd, false);
		}
	}
	const bool isBM = h.hatchType == API_BuildingMaterialHatch;

	// --- pens and colours
	if (auto p = OptPen (spec, "fillPen")) {
		h.fillPen.penIndex = *p;
		MaskField (mask, e, h.fillPen.penIndex);
		if (isBM) setFlag (APIHatch_OverrideFgPen, true);
	}
	if (auto p = OptPen (spec, "fillColorOverridePen", 0)) {
		h.fillPen.colorOverridePenIndex = *p;
		MaskField (mask, e, h.fillPen.colorOverridePenIndex);
	}
	if (auto p = OptPen (spec, "backgroundPen", 0)) {
		h.fillBGPen = *p;
		MaskField (mask, e, h.fillBGPen);
		if (isBM) setFlag (APIHatch_OverrideBkgPen, true);
	}
	if (auto b = OptBool (spec, "overrideBuildingMaterialPens")) {
		setFlag (APIHatch_OverrideFgPen, *b);
		setFlag (APIHatch_OverrideBkgPen, *b);
	}
	{
		bool remove = false;
		if (OptColor (spec, "foregroundColor", h.foregroundRGB, remove)) {
			setFlag (APIHatch_HasFgRGBColor, !remove);
			MaskField (mask, e, h.foregroundRGB);
		}
		if (OptColor (spec, "backgroundColor", h.backgroundRGB, remove)) {
			setFlag (APIHatch_HasBkgRGBColor, !remove);
			MaskField (mask, e, h.backgroundRGB);
		}
	}
	if (Has (spec, "fillCategory")) {
		h.determination = (short) ParseNamed (kFillCategories, spec, "fillCategory");
		MaskField (mask, e, h.determination);
	}

	// --- contour
	auto contour = OptBool (spec, "contour");
	auto contourPen = OptPen (spec, "contourPen");
	if (contour.has_value () && !*contour && contourPen.has_value ())
		Fail ("'contour': false conflicts with 'contourPen' (a contour pen switches the contour on).");
	if (contourPen.has_value ()) {
		h.contPen.penIndex = *contourPen;
		MaskField (mask, e, h.contPen.penIndex);
	} else if (contour.has_value ()) {
		if (!*contour)
			h.contPen.penIndex = 0;
		else if (h.contPen.penIndex <= 0)
			h.contPen.penIndex = h.fillPen.penIndex > 0 ? h.fillPen.penIndex : 1;
		MaskField (mask, e, h.contPen.penIndex);
	}
	if (auto p = OptPen (spec, "contourColorOverridePen", 0)) {
		h.contPen.colorOverridePenIndex = *p;
		MaskField (mask, e, h.contPen.colorOverridePenIndex);
	}
	if (auto lt = OptAttr (API_LinetypeID, spec, "contourLineType")) {
		h.ltypeInd = *lt;
		MaskField (mask, e, h.ltypeInd);
	}
	if (auto w = OptDouble (spec, "contourLineWeight")) {
		if (!(*w > 0.0) && std::fabs (*w - API_DefPenWeigth) > kEps)
			Fail ("Field 'contourLineWeight' must be a positive weight in mm, or -1 to use the pen's own weight.");
		h.penWeight = *w;
		MaskField (mask, e, h.penWeight);
	}

	// --- orientation
	if (Has (spec, "orientation"))
		ApplyOrientation (e, mask, GetObject (spec, "orientation"));

	// --- polygon
	std::optional<PolygonData> polygon;
	if (Has (spec, "polygon")) {
		polygon = GetPolygon (spec, "polygon");
		WritePolygonToMemo (*polygon, h.poly, memo);
		MaskField (mask, e, h.poly);
		memoMask |= APIMemoMask_Polygon;
	}

	// --- area text
	if (auto b = OptBool (spec, "showArea")) {
		h.showArea = *b;
		MaskField (mask, e, h.showArea);
	}
	OS areaText;
	const bool hasAreaText = TryGetObject (spec, "areaText", areaText);
	if (hasAreaText) {
		if (auto c = OptCoord (areaText, "position"))	{ h.note.pos = *c; MaskField (mask, e, h.note.pos); }
		if (auto p = OptPen (areaText, "pen"))		{ h.note.notePen = *p; MaskField (mask, e, h.note.notePen); }
		if (auto f = OptAttr (API_FontID, areaText, "font"))	{ h.note.noteFont = (short) *f; MaskField (mask, e, h.note.noteFont); }
		if (auto s = OptPositive (areaText, "size"))	{ h.note.noteSize = *s; MaskField (mask, e, h.note.noteSize); }
		if (auto a = OptAngle (areaText, "angle"))		{ h.note.noteAngle = *a; MaskField (mask, e, h.note.noteAngle); }
	}
	// Put a newly shown area text in the middle of the (new) polygon unless placed explicitly.
	if (h.showArea && polygon.has_value () && !(hasAreaText && Has (areaText, "position"))) {
		h.note.pos = OutlineCenter (*polygon);
		MaskField (mask, e, h.note.pos);
	}
}


API_Guid CreateHatch (const OS& spec)
{
	if (!Has (spec, "polygon"))
		Fail ("Hatch requires 'polygon' (points {x, y} in meters, optional arcs and holes).");
	API_Element e = NewElement (API_HatchID);
	GetDefaults (e, nullptr);
	ApplyCommonFields (e, nullptr, spec);
	Memo memo;
	UInt64 memoMask = 0;
	ApplyHatchFields (e, nullptr, spec, *memo, memoMask);
	CheckDrafting (ACAPI_Element_Create (&e, memo.Ptr ()), "Cannot create hatch");
	return e.header.guid;
}


double ContourArea (const API_ElementMemo& memo, const API_Polygon& poly, Int32 begin, Int32 end)
{
	// begin..end: memo coord indices, end = closing vertex.
	double a = 0.0;
	for (Int32 i = begin; i < end; ++i) {
		const API_Coord& p = (*memo.coords)[i];
		const API_Coord& q = (*memo.coords)[i + 1];
		a += (p.x * q.y - q.x * p.y) / 2.0;
		if (memo.parcs != nullptr) {
			for (Int32 k = 0; k < poly.nArcs; ++k) {
				const API_PolyArc& arc = (*memo.parcs)[k];
				if (arc.begIndex == i && std::fabs (arc.arcAngle) > kEps) {
					const double chord = Dist (p, q);
					const double r = chord / (2.0 * std::sin (std::fabs (arc.arcAngle) / 2.0));
					a += r * r / 2.0 * (arc.arcAngle - std::sin (arc.arcAngle));
				}
			}
		}
	}
	return a;
}


void SerializeHatch (const API_Element& element, OS& out)
{
	API_Element e = element;
	const API_HatchType& h = e.hatch;
	const bool isBM = h.hatchType == API_BuildingMaterialHatch;
	out.Add ("hatchKind", GS::UniString (isBM ? "BuildingMaterial" : "Fill"));
	if (isBM)
		out.Add ("buildingMaterial", AttrRef (API_BuildingMaterialID, h.buildingMaterial));
	if (!isBM || (h.hatchFlags & APIHatch_OverrideFillInd) != 0)
		out.Add ("fillType", AttrRef (API_FilltypeID, h.fillInd));
	out.Add ("fillCategory", NameOf (kFillCategories, h.determination));
	out.Add ("fillPen", (Int32) h.fillPen.penIndex);
	out.Add ("fillColorOverridePen", (Int32) h.fillPen.colorOverridePenIndex);
	out.Add ("backgroundPen", (Int32) h.fillBGPen);
	if (isBM) {
		out.Add ("overrideFill", (h.hatchFlags & APIHatch_OverrideFillInd) != 0);
		out.Add ("overrideFillPen", (h.hatchFlags & APIHatch_OverrideFgPen) != 0);
		out.Add ("overrideBackgroundPen", (h.hatchFlags & APIHatch_OverrideBkgPen) != 0);
	}
	if ((h.hatchFlags & APIHatch_HasFgRGBColor) != 0)
		out.Add ("foregroundColor", ColorHex (h.foregroundRGB));
	if ((h.hatchFlags & APIHatch_HasBkgRGBColor) != 0)
		out.Add ("backgroundColor", ColorHex (h.backgroundRGB));
	out.Add ("contour", h.contPen.penIndex > 0);
	out.Add ("contourPen", (Int32) h.contPen.penIndex);
	out.Add ("contourColorOverridePen", (Int32) h.contPen.colorOverridePenIndex);
	out.Add ("contourLineType", AttrRef (API_LinetypeID, h.ltypeInd));
	out.Add ("contourLineWeight", h.penWeight);
	out.Add ("orientation", OrientationJson (h.hatchOrientation));
	out.Add ("showArea", h.showArea);
	if (h.showArea) {
		OS note;
		note.Add ("position", CoordObj (h.note.pos));
		note.Add ("pen", (Int32) h.note.notePen);
		note.Add ("font", AttrRef (API_FontID, h.note.noteFont));
		note.Add ("size", h.note.noteSize);
		AddAngle (note, "angle", h.note.noteAngle);
		out.Add ("areaText", note);
	}

	Memo memo;
	double geomArea = -1.0;
	if (ACAPI_Element_GetMemo (e.header.guid, memo.Ptr (), APIMemoMask_Polygon) == NoError && memo->coords != nullptr && memo->pends != nullptr) {
		out.Add ("polygon", PolygonToJson (h.poly, *memo));
		geomArea = 0.0;
		const Int32 nCoordsInMemo = HandleCount (reinterpret_cast<GSConstHandle> (memo->coords), sizeof (API_Coord));
		const Int32 nPends = HandleCount (reinterpret_cast<GSConstHandle> (memo->pends), sizeof (Int32));
		for (Int32 k = 1; k <= h.poly.nSubPolys && k < nPends; ++k) {
			const Int32 begin = (*memo->pends)[k - 1] + 1;
			const Int32 end = (*memo->pends)[k];
			if (begin < 1 || end >= nCoordsInMemo || end <= begin)
				break;
			const double a = std::fabs (ContourArea (*memo, h.poly, begin, end));
			geomArea += (k == 1) ? a : -a;
		}
	}

	// Archicad's own quantities (area, perimeters); geometric area as fallback.
	API_ElementQuantity quantity;
	BNZeroMemory (&quantity, sizeof (quantity));
	GS::Array<API_CompositeQuantity> composites;
	GS::Array<API_ElemPartQuantity> elemPartQuantities;
	GS::Array<API_ElemPartCompositeQuantity> elemPartComposites;
	API_Quantities quantities;
	quantities.elements = &quantity;
	quantities.composites = &composites;
	quantities.elemPartQuantities = &elemPartQuantities;
	quantities.elemPartComposites = &elemPartComposites;
	API_QuantityPar params;
	BNZeroMemory (&params, sizeof (params));
	params.minOpeningSize = 1e-5;
	API_QuantitiesMask qmask;
	ACAPI_ELEMENT_QUANTITY_MASK_CLEAR (qmask);
	ACAPI_ELEMENT_QUANTITY_MASK_SET (qmask, hatch, surface);
	ACAPI_ELEMENT_QUANTITY_MASK_SET (qmask, hatch, perimeter);
	ACAPI_ELEMENT_QUANTITY_MASK_SET (qmask, hatch, holesPrm);
	ACAPI_ELEMENT_QUANTITY_MASK_SET (qmask, hatch, holesSurf);
	if (ACAPI_Element_GetQuantities (e.header.guid, &params, &quantities, &qmask) == NoError) {
		out.Add ("area", quantity.hatch.surface);
		out.Add ("perimeter", quantity.hatch.perimeter);
		out.Add ("holesPerimeter", quantity.hatch.holesPrm);
		out.Add ("holesArea", quantity.hatch.holesSurf);
	} else if (geomArea >= 0.0) {
		out.Add ("area", geomArea);
	}
}


void ModifyHatch (API_Element& e, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask, const OS& patch)
{
	ApplyHatchFields (e, &mask, patch, memo, memoMask);
}

// =============================================================================
// Hotspot
// =============================================================================

void ApplyHotspotFields (API_Element& e, API_Element* mask, const OS& spec)
{
	if (auto c = OptCoord (spec, "position"))	{ e.hotspot.pos = *c; MaskField (mask, e, e.hotspot.pos); }
	if (auto v = OptDouble (spec, "height"))	{ e.hotspot.height = *v; MaskField (mask, e, e.hotspot.height); }
	if (auto p = OptPen (spec, "pen"))			{ e.hotspot.pen = *p; MaskField (mask, e, e.hotspot.pen); }
}


API_Guid CreateHotspot (const OS& spec)
{
	if (!Has (spec, "position"))
		Fail ("Hotspot requires 'position' {x, y} in meters.");
	API_Element e = NewElement (API_HotspotID);
	GetDefaults (e, nullptr);
	ApplyCommonFields (e, nullptr, spec);
	ApplyHotspotFields (e, nullptr, spec);
	CheckDrafting (ACAPI_Element_Create (&e, nullptr), "Cannot create hotspot");
	return e.header.guid;
}


void SerializeHotspot (const API_Element& e, OS& out)
{
	out.Add ("position", CoordObj (e.hotspot.pos));
	out.Add ("height", e.hotspot.height);
	out.Add ("pen", (Int32) e.hotspot.pen);
}


void ModifyHotspot (API_Element& e, API_Element& mask, API_ElementMemo&, UInt64&, const OS& patch)
{
	ApplyHotspotFields (e, &mask, patch);
}

// =============================================================================
// Picture
// =============================================================================

struct ImageInfo {
	API_PictureFormat	format = APIPictForm_Default;
	Int32				width = 0;		// pixels
	Int32				height = 0;
	GS::UniString		formatName;
};


UInt32 BE16 (const unsigned char* p) { return ((UInt32) p[0] << 8) | p[1]; }
UInt32 BE32 (const unsigned char* p) { return ((UInt32) p[0] << 24) | ((UInt32) p[1] << 16) | ((UInt32) p[2] << 8) | p[3]; }
UInt32 LE16 (const unsigned char* p) { return ((UInt32) p[1] << 8) | p[0]; }
Int32  LE32 (const unsigned char* p) { return (Int32) (((UInt32) p[3] << 24) | ((UInt32) p[2] << 16) | ((UInt32) p[1] << 8) | p[0]); }


// Detects the format from the file signature and reads the pixel size from the header
// (TIFF sizes come from GX::ImageBase).
ImageInfo SniffImage (const std::vector<unsigned char>& d)
{
	ImageInfo info;
	const size_t n = d.size ();
	if (n >= 24 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') {
		info.format = APIPictForm_PNG; info.formatName = "PNG";
		info.width = (Int32) BE32 (&d[16]); info.height = (Int32) BE32 (&d[20]);
	} else if (n >= 4 && d[0] == 0xFF && d[1] == 0xD8) {
		info.format = APIPictForm_JPEG; info.formatName = "JPEG";
		size_t i = 2;
		while (i + 9 < n) {
			if (d[i] != 0xFF) { ++i; continue; }
			const unsigned char marker = d[i + 1];
			if (marker == 0xFF) { ++i; continue; }
			if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) { i += 2; continue; }
			const UInt32 segLen = BE16 (&d[i + 2]);
			const bool isSOF = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
			if (isSOF) {
				info.height = (Int32) BE16 (&d[i + 5]);
				info.width = (Int32) BE16 (&d[i + 7]);
				break;
			}
			if (segLen < 2) break;
			i += 2 + segLen;
		}
	} else if (n >= 10 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F' && d[3] == '8') {
		info.format = APIPictForm_GIF; info.formatName = "GIF";
		info.width = (Int32) LE16 (&d[6]); info.height = (Int32) LE16 (&d[8]);
	} else if (n >= 26 && d[0] == 'B' && d[1] == 'M') {
		info.format = APIPictForm_Bitmap; info.formatName = "BMP";
		info.width = LE32 (&d[18]);
		const Int32 h = LE32 (&d[22]);
		info.height = h < 0 ? -h : h;
	} else if (n >= 4 && ((d[0] == 'I' && d[1] == 'I' && d[2] == 42 && d[3] == 0) || (d[0] == 'M' && d[1] == 'M' && d[2] == 0 && d[3] == 42))) {
		info.format = APIPictForm_TIFF; info.formatName = "TIFF";
	}
	return info;
}


// Anchor offsets within the box as fractions (0 = min, 0.5 = middle, 1 = max).
void AnchorFractions (API_AnchorID anchor, double& fx, double& fy)
{
	switch (anchor) {
		case APIAnc_LT: fx = 0.0; fy = 1.0; break;
		case APIAnc_MT: fx = 0.5; fy = 1.0; break;
		case APIAnc_RT: fx = 1.0; fy = 1.0; break;
		case APIAnc_LM: fx = 0.0; fy = 0.5; break;
		case APIAnc_MM: fx = 0.5; fy = 0.5; break;
		case APIAnc_RM: fx = 1.0; fy = 0.5; break;
		case APIAnc_LB: fx = 0.0; fy = 0.0; break;
		case APIAnc_MB: fx = 0.5; fy = 0.0; break;
		case APIAnc_RB: fx = 1.0; fy = 0.0; break;
		default:        fx = 0.0; fy = 0.0; break;
	}
}


API_Box BoxAtAnchor (const API_Coord& pos, API_AnchorID anchor, double w, double h)
{
	double fx = 0, fy = 0;
	AnchorFractions (anchor, fx, fy);
	API_Box box;
	box.xMin = pos.x - fx * w;
	box.yMin = pos.y - fy * h;
	box.xMax = box.xMin + w;
	box.yMax = box.yMin + h;
	return box;
}


API_Coord AnchorOfBox (const API_Box& box, API_AnchorID anchor)
{
	double fx = 0, fy = 0;
	AnchorFractions (anchor, fx, fy);
	return { box.xMin + fx * (box.xMax - box.xMin), box.yMin + fy * (box.yMax - box.yMin) };
}


void SetPictureName (API_PictureType& pict, const GS::UniString& name)
{
	if (name.GetLength () >= API_UniLongNameLen)
		Fail ("Picture 'name' is too long (max 255 characters).");
	BNZeroMemory (pict.pictName, sizeof (pict.pictName));
	GS::ucsncpy (pict.pictName, name.ToUStr ().Get (), API_UniLongNameLen - 1);
}


// Resolves the final size (m) from width/height and the pixel aspect ratio.
void ResolvePictureSize (const OS& spec, double aspect, double& w, double& h)
{
	auto width = OptPositive (spec, "width");
	auto height = OptPositive (spec, "height");
	if (width.has_value () && height.has_value ()) { w = *width; h = *height; }
	else if (width.has_value ()) { w = *width; h = *width / aspect; }
	else if (height.has_value ()) { h = *height; w = *height * aspect; }
}


API_Guid CreatePicture (const OS& spec)
{
	if (!Has (spec, "file") || !Has (spec, "position"))
		Fail ("Picture requires 'file' (absolute path of a PNG/JPEG/GIF/TIFF/BMP image) and 'position' {x, y}.");
	const GS::UniString path = GetString (spec, "file");
	const API_Coord pos = GetCoord (spec, "position");

	// Read the image file.
	std::vector<unsigned char> bytes;
	{
		std::ifstream in (ToStr (path).ToCStr (), std::ios::binary | std::ios::ate);
		if (!in)
			Fail ("Cannot open image file '" + path + "'. Give an absolute path readable by Archicad.", APIERR_BADNAME);
		const std::streamoff size = in.tellg ();
		constexpr std::streamoff kMaxBytes = 200LL * 1024 * 1024;
		if (size <= 0 || size > kMaxBytes)
			Fail ("Image file '" + path + "' is empty or larger than 200 MB.");
		bytes.resize ((size_t) size);
		in.seekg (0);
		if (!in.read (reinterpret_cast<char*> (bytes.data ()), size))
			Fail ("Cannot read image file '" + path + "'.");
	}
	ImageInfo info = SniffImage (bytes);
	if (info.format == APIPictForm_Default)
		Fail ("Unsupported image format in '" + path + "': use PNG, JPEG, GIF, TIFF or BMP.", APIERR_NOTSUPPORTED);
	{
		Int32 hSize = 0, vSize = 0, hRes = 0, vRes = 0, bits = 0;
		IO::Location loc (path);
		if (GX::ImageBase::GetFileInfo (loc, &hSize, &vSize, &hRes, &vRes, &bits) == NoError && hSize > 0 && vSize > 0) {
			info.width = hSize;
			info.height = vSize;
		}
	}
	if (info.width <= 0 || info.height <= 0)
		Fail ("Cannot determine the pixel size of '" + path + "' (corrupt or unsupported image).");
	if (info.width > 32767 || info.height > 32767)
		Fail ("Image is too large (" + GS::ValueToUniString (info.width) + " x " + GS::ValueToUniString (info.height) +
			  " px); Archicad pictures support at most 32767 px per side — downscale it first.", APIERR_NOTSUPPORTED);

	API_Element e = NewElement (API_PictureID);
	GetDefaults (e, nullptr);
	ApplyCommonFields (e, nullptr, spec);
	API_PictureType& pict = e.picture;
	pict.storageFormat = info.format;
	pict.pixelSizeX = (short) info.width;
	pict.pixelSizeY = (short) info.height;
	pict.colorDepth = APIColorDepth_FromSourceImage;
	pict.mirrored = GetBool (spec, "mirrored", false);
	pict.transparent = GetBool (spec, "transparent", false);
	pict.rotAngle = GetAngle (spec, "angle", 0.0);
	GS::UniString name = GetString (spec, "name", GS::UniString ());
	if (name.IsEmpty ()) {
		IO::Location loc (path);
		IO::Name last;
		if (loc.GetLastLocalName (&last) == NoError)
			name = last.ToString ();
	}
	SetPictureName (pict, name);

	const double aspect = (double) info.width / (double) info.height;
	double w = 0, h = 0;
	ResolvePictureSize (spec, aspect, w, h);
	if (w > 0 && h > 0) {
		pict.usePixelSize = false;
		pict.anchorPoint = Has (spec, "anchor") ? (API_AnchorID) ParseNamed (kAnchors, spec, "anchor") : APIAnc_LB;
		pict.destBox = BoxAtAnchor (pos, pict.anchorPoint, w, h);
	} else {
		if (Has (spec, "anchor") && ParseNamed (kAnchors, spec, "anchor") != APIAnc_LB)
			Fail ("'anchor' other than BottomLeft needs 'width' or 'height' (without a size the picture is placed at its pixel size from its bottom-left corner).");
		pict.usePixelSize = true;
		pict.anchorPoint = APIAnc_LB;
		pict.destBox.xMin = pos.x;
		pict.destBox.yMin = pos.y;
		pict.destBox.xMax = pos.x;
		pict.destBox.yMax = pos.y;
	}

	Memo memo;
	memo->pictHdl = BMAllocateHandle ((GSSize) bytes.size (), ALLOCATE_CLEAR, 0);
	if (memo->pictHdl == nullptr)
		Fail ("Out of memory while loading the image.", APIERR_MEMFULL);
	std::memcpy (*memo->pictHdl, bytes.data (), bytes.size ());

	CheckDrafting (ACAPI_Element_Create (&e, memo.Ptr ()), "Cannot create picture");
	return e.header.guid;
}


void SerializePicture (const API_Element& e, OS& out)
{
	const API_PictureType& pict = e.picture;
	out.Add ("name", GS::UniString (pict.pictName));
	out.Add ("mime", GS::UniString (pict.mime, CC_UTF8));
	out.Add ("format", NameOf (kPictureFormats, pict.storageFormat));
	out.Add ("anchor", NameOf (kAnchors, pict.anchorPoint));
	out.Add ("position", CoordObj (AnchorOfBox (pict.destBox, pict.anchorPoint)));
	out.Add ("box", BoxObj (pict.destBox));
	out.Add ("width", pict.destBox.xMax - pict.destBox.xMin);
	out.Add ("height", pict.destBox.yMax - pict.destBox.yMin);
	AddAngle (out, "angle", pict.rotAngle);
	out.Add ("mirrored", pict.mirrored);
	out.Add ("transparent", pict.transparent);
	out.Add ("usePixelSize", pict.usePixelSize);
	out.Add ("pixelSize", OS ("x", (Int32) pict.pixelSizeX, "y", (Int32) pict.pixelSizeY));
}


void ModifyPicture (API_Element& e, API_Element& mask, API_ElementMemo&, UInt64&, const OS& patch)
{
	if (Has (patch, "file"))
		Fail ("The image of an existing picture cannot be replaced. Create a new one with create_pictures and delete the old one.", APIERR_NOTSUPPORTED);
	API_PictureType& pict = e.picture;
	if (auto b = OptBool (patch, "mirrored"))		{ pict.mirrored = *b; MaskField (&mask, e, pict.mirrored); }
	if (auto b = OptBool (patch, "transparent"))	{ pict.transparent = *b; MaskField (&mask, e, pict.transparent); }
	if (auto a = OptAngle (patch, "angle"))		{ pict.rotAngle = *a; MaskField (&mask, e, pict.rotAngle); }
	if (auto n = OptString (patch, "name"))		{ SetPictureName (pict, *n); MaskField (&mask, e, pict.pictName); }

	const bool geometry = Has (patch, "position") || Has (patch, "width") || Has (patch, "height") || Has (patch, "anchor");
	if (geometry) {
		const API_AnchorID oldAnchor = pict.anchorPoint;
		const API_AnchorID anchor = Has (patch, "anchor") ? (API_AnchorID) ParseNamed (kAnchors, patch, "anchor") : oldAnchor;
		double curW = pict.destBox.xMax - pict.destBox.xMin;
		double curH = pict.destBox.yMax - pict.destBox.yMin;
		const double aspect = (curW > kEps && curH > kEps) ? curW / curH
							: (pict.pixelSizeY > 0 ? (double) pict.pixelSizeX / (double) pict.pixelSizeY : 1.0);
		double w = curW, h = curH;
		ResolvePictureSize (patch, aspect, w, h);
		if (!(w > kEps && h > kEps))
			Fail ("The picture's current size is unknown (placed at pixel size): give 'width' and/or 'height' in meters.");
		// The anchor point stays where it is unless a new position is given.
		const API_Coord pos = Has (patch, "position") ? GetCoord (patch, "position") : AnchorOfBox (pict.destBox, anchor);
		pict.anchorPoint = anchor;
		pict.destBox = BoxAtAnchor (pos, anchor, w, h);
		pict.usePixelSize = false;
		MaskField (&mask, e, pict.anchorPoint);
		MaskField (&mask, e, pict.destBox);
		MaskField (&mask, e, pict.usePixelSize);
	}
}

} // namespace
} // namespace drafting


void RegisterDraftingCommands ()
{
	using namespace drafting;
	RegisterAdapter ({ API_LineID,		CreateLine,		SerializeLine,		ModifyLine });
	RegisterAdapter ({ API_ArcID,		CreateArc,		SerializeArc,		ModifyArc });
	RegisterAdapter ({ API_CircleID,	CreateCircle,	SerializeCircle,	ModifyCircle });
	RegisterAdapter ({ API_PolyLineID,	CreatePolyLine,	SerializePolyLine,	ModifyPolyLine });
	RegisterAdapter ({ API_SplineID,	CreateSpline,	SerializeSpline,	ModifySpline });
	RegisterAdapter ({ API_HatchID,		CreateHatch,	SerializeHatch,		ModifyHatch });
	RegisterAdapter ({ API_HotspotID,	CreateHotspot,	SerializeHotspot,	ModifyHotspot });
	RegisterAdapter ({ API_PictureID,	CreatePicture,	SerializePicture,	ModifyPicture });
	RegisterTextLabelAdapters ();
}

} // namespace cc
