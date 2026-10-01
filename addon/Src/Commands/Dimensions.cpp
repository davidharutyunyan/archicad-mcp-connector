// *****************************************************************************
// Dimensions — linear dimension adapter (API_DimensionID) and the family's
// commands DimensionWalls / GetDimensionAnchors. The level / radial / angle
// dimension adapters live in DimensionsSpecial.cpp, helpers in DimensionsCommon.*.
//
// Linear dimension ("Dimension") create / modify fields (meters, degrees; text and
// marker sizes in paper millimeters):
//   points* [ {x, y}                                   static point
//           | {element, at: "begin"|"end"}             associative: wall/beam/line reference-line end
//           | {element, x, y}                          associative: element anchor (hotspot) nearest to x,y
//           | {element, inIndex, line?, special?, nodeId?, x, y}   raw API_Base (expert; as returned in details)
//           ] (+ per point: witnessForm, witnessVal, text = custom text of the segment ending at this point)
//   direction ("Horizontal"|"Vertical"|angle|{x,y}; default: from the first two distinct points),
//   linePoint {x,y} (a point of the dimension line) | offset (m, default 1, + = left of direction),
//   associative (default true), snapTolerance (m, default 0.02),
//   appearance "Normal"|"Cumulative"|"CumulativeSV"|"Elevation", textPosition "Above"|"In"|"Below",
//   textDirection "Parallel"|"Horizontal"|"Vertical"|..., linePen, witnessForm "None"|"Small"|"Large"|"Fixed",
//   witnessVal, markerType/markerPen/markerSize, textPen/textSize/textFont/textBold/textItalic/textUnderline/
//   textFrame/textOpaque, horizontalText, onlyDimensionText, layout "Legacy"|"Flexible"|"Centered"|"Off",
//   clipOtherSide; modify only: makeStatic (detach every point from its element),
//   pointTexts [{index, text}] (custom texts of single points without replacing the points)
//   + common: layer, storyIndex, renovationStatus, elementId
//
// DimensionWalls modes: Chain | EachWall (wall ends + opening jambs) | Thickness (wall faces, inIndex 11/21).
// Tool defaults (set_tool_defaults / get_tool_defaults call the adapters with a null GUID): only the style
// fields apply and per-instance geometry is not serialized.
//
// Associative points are verified after creation (the dimension must measure the
// expected points); when Archicad refuses or resolves them differently, the dimension
// is recreated with static points at the same coordinates (visible in the details as
// points[i].associative = false).
// *****************************************************************************

#include "Commands/Commands.hpp"
#include "Commands/DimensionsCommon.hpp"
#include "Core/Command.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <list>

namespace cc {

namespace {

using namespace dimension;

constexpr double	kDefaultSnapTolerance	= 0.02;		// m, snapping {element, x, y} points to element anchors
constexpr double	kExactTolerance			= 0.002;	// m, anchors of computed points (wall ends, jambs) / verification
constexpr Int32		kMaxPoints				= 1000;

// --- Chain points --------------------------------------------------------------------

struct ChainPoint {
	API_DimBase						base;				// base.loc always set; base.base set when associative
	bool							associative = false;
	bool							raw = false;		// caller-supplied API_Base (not verified)
	bool							references = false;	// the spec referenced an element
	std::optional<API_WitnessID>	witnessForm;
	std::optional<double>			witnessVal;
	std::optional<GS::UniString>	text;				// custom text of the segment ending here ("" = measured)

	ChainPoint () { BNZeroMemory (&base, sizeof (base)); }
};

struct ChainLine {
	Vec			dir;		// unit vector
	API_Coord	refC;		// a point of the dimension line
};


class AnchorCache {
public:
	const GS::Array<Anchor>& Get (const API_Guid& guid)
	{
		for (const auto& entry : entries) {
			if (entry.first == guid)
				return entry.second;
		}
		entries.emplace_back (guid, CollectAnchors (guid));
		return entries.back ().second;
	}

private:
	std::list<std::pair<API_Guid, GS::Array<Anchor>>> entries;	// list: references stay valid
};


GS::UniString PointLabel (Int32 index)
{
	return "points[" + GS::ValueToUniString (index) + "]";
}


// spec[key]: a custom text string, or ""/false for the measured value (returned as "").
GS::UniString ParseCustomText (const OS& spec, const char* key, const GS::UniString& where)
{
	if (spec.IsBool (key)) {
		if (GetBool (spec, key))
			Fail (where + " must be a custom text string, or \"\"/false for the measured value.");
		return GS::UniString ();
	}
	return GetString (spec, key);
}


ChainPoint ResolvePoint (const OS& p, Int32 index, bool allowAssociative, double tolerance, AnchorCache& cache)
{
	ChainPoint cp;
	const GS::UniString where = PointLabel (index);

	std::optional<API_Coord> loc;
	if (p.Contains ("x") || p.Contains ("y")) {
		API_Coord c;
		c.x = GetDouble (p, "x");
		c.y = GetDouble (p, "y");
		loc = c;
	}
	if (Has (p, "witnessForm"))
		cp.witnessForm = (API_WitnessID) ParseNamed (kWitnessForms, p, "witnessForm");
	cp.witnessVal = OptDouble (p, "witnessVal");
	if (p.Contains ("text"))
		cp.text = ParseCustomText (p, "text", where + ".text");

	if (RawBaseFromSpec (p, cp.base.base)) {
		if (!loc.has_value ())
			Fail (where + ": a raw associative point {element, inIndex} also needs 'x','y' (its approximate location).");
		cp.base.loc = *loc;
		cp.references = true;
		if (allowAssociative) {
			cp.associative = true;
			cp.raw = true;
		} else {
			BNZeroMemory (&cp.base.base, sizeof (cp.base.base));
		}
		return cp;
	}

	if (p.Contains ("element")) {
		const API_Guid guid = GetGuid (p, "element");
		API_Coord target;
		if (auto at = OptString (p, "at")) {
			const RefLine rl = GetRefLine (GetElement (guid));
			if (EqualsIgnoreCase (*at, "begin"))
				target = rl.begin;
			else if (EqualsIgnoreCase (*at, "end"))
				target = rl.end;
			else
				Fail (where + ".at must be \"begin\" or \"end\".");
		} else if (loc.has_value ()) {
			target = *loc;
		} else {
			Fail (where + ": a point on an element needs 'at' (\"begin\"|\"end\" of a wall, beam or line) or 'x','y' near the "
				  "element point to link (list candidates with get_dimension_anchors).");
		}
		cp.base.loc = target;
		cp.references = true;
		if (allowAssociative) {
			if (const Anchor* a = NearestAnchor (cache.Get (guid), target, tolerance)) {
				cp.base.base = BaseFromNeig (a->neig);
				cp.base.loc.x = a->coord.x;
				cp.base.loc.y = a->coord.y;
				cp.associative = true;
			}
		}
		return cp;
	}

	if (!loc.has_value ())
		Fail (where + ": give 'x','y' (static point) or 'element' (+ 'at' or 'x','y') for a point linked to an element.");
	cp.base.loc = *loc;
	return cp;
}


GS::Array<ChainPoint> ResolvePoints (const OS& spec, AnchorCache& cache)
{
	GS::Array<OS> specs = GetObjectArray (spec, "points");
	if (specs.GetSize () < 2)
		Fail ("A linear dimension needs at least 2 points in 'points'.");
	if ((Int32) specs.GetSize () > kMaxPoints)
		Fail ("Too many points (max " + GS::ValueToUniString (kMaxPoints) + ").");
	const bool allowAssociative = GetBool (spec, "associative", true);
	const double tolerance = GetDouble (spec, "snapTolerance", kDefaultSnapTolerance);
	if (tolerance < 0.0)
		Fail ("snapTolerance must be >= 0 (meters).");
	GS::Array<ChainPoint> pts;
	for (UIndex i = 0; i < specs.GetSize (); ++i)
		pts.Push (ResolvePoint (specs[i], (Int32) i, allowAssociative, tolerance, cache));
	return pts;
}


// GUIDs of the elements referenced by the point specs (to pick the database before resolving anchors).
GS::Array<API_Guid> ReferencedElements (const OS& spec)
{
	GS::Array<API_Guid> guids;
	for (const OS& p : GetObjectArray (spec, "points", false)) {
		if (p.Contains ("element")) {
			try {
				guids.Push (GetGuid (p, "element"));
			} catch (const Error&) {
				// reported with its point index while resolving
			}
		}
	}
	return guids;
}

// --- Line geometry ---------------------------------------------------------------------

std::optional<Vec> OptDirection (const OS& spec, const char* key)
{
	if (!spec.Contains (key))
		return std::nullopt;
	if (spec.IsString (key)) {
		const GS::UniString s = GetString (spec, key);
		if (EqualsIgnoreCase (s, "Horizontal"))
			return Vec { 1.0, 0.0 };
		if (EqualsIgnoreCase (s, "Vertical"))
			return Vec { 0.0, 1.0 };
	} else if (IsNumber (spec, key)) {
		const double a = GetAngle (spec, key);
		return Vec { std::cos (a), std::sin (a) };
	} else if (spec.IsObject (key)) {
		const OS v = GetObject (spec, key);
		return Unit ({ GetDouble (v, "x"), GetDouble (v, "y") });
	}
	Fail ("'" + GS::UniString (key) + "' must be \"Horizontal\", \"Vertical\", an angle in degrees (0 = +X, 90 = +Y) or a vector {x, y}.");
}


ChainLine ComputeLine (const OS& spec, const GS::Array<ChainPoint>& pts)
{
	ChainLine line;
	if (auto d = OptDirection (spec, "direction")) {
		line.dir = *d;
	} else {
		bool found = false;
		for (UIndex i = 1; i < pts.GetSize () && !found; ++i) {
			if (Dist (pts[i].base.loc, pts[0].base.loc) > 1e-6) {
				line.dir = Unit (Sub (V (pts[i].base.loc), V (pts[0].base.loc)));
				found = true;
			}
		}
		if (!found)
			Fail ("All dimension points coincide; give distinct points or an explicit 'direction'.");
	}
	if (auto lp = OptCoord (spec, "linePoint")) {
		line.refC = *lp;
	} else {
		const double offset = GetDouble (spec, "offset", 1.0);
		line.refC = C (Add (V (pts[0].base.loc), Mul (LeftNormal (line.dir), offset)));
	}
	return line;
}

// --- Style ------------------------------------------------------------------------------

void ApplyLinearStyle (API_Element& element, API_Element* mask, const OS& spec, StringPool& pool)
{
	API_DimensionType& d = element.dimension;
#define DIM_SET(f) if (mask) ACAPI_ELEMENT_MASK_SET (*mask, API_DimensionType, f)
	if (auto p = OptPen (spec, "linePen"))					{ d.linPen = *p; DIM_SET (linPen); }
	if (Has (spec, "textPosition"))						{ d.textPos = (API_TextPosID) ParseNamed (kTextPos, spec, "textPosition"); DIM_SET (textPos); }
	if (Has (spec, "textDirection"))						{ d.textWay = (API_DirID) ParseNamed (kTextWay, spec, "textDirection"); DIM_SET (textWay); }
	if (Has (spec, "appearance"))							{ d.dimAppear = (API_AppearID) ParseNamed (kAppearances, spec, "appearance"); DIM_SET (dimAppear); }
	if (Has (spec, "witnessForm"))							{ d.defWitnessForm = (API_WitnessID) ParseNamed (kWitnessForms, spec, "witnessForm"); DIM_SET (defWitnessForm); }
	if (auto v = OptDouble (spec, "witnessVal"))			{ d.defWitnessVal = *v; DIM_SET (defWitnessVal); }
	if (auto b = OptBool (spec, "horizontalText"))			{ d.horizontalText = *b; DIM_SET (horizontalText); }
	if (auto b = OptBool (spec, "onlyDimensionText"))		{ d.onlyDimensionText = *b; DIM_SET (onlyDimensionText); }
	if (Has (spec, "layout"))								{ d.dimLayout = (API_DimLayoutID) ParseNamed (kLayouts, spec, "layout"); DIM_SET (dimLayout); }
	if (auto b = OptBool (spec, "clipOtherSide"))			{ d.clipOtherSide = *b; DIM_SET (clipOtherSide); }
#undef DIM_SET
	ApplyMarkerFields (d.markerData, MaskOf (element, mask, d.markerData), spec);
	ApplyNoteFields (d.defNote, MaskOf (element, mask, d.defNote), spec, pool, nullptr);
}

// --- Creation ---------------------------------------------------------------------------

// Fills dimElem i from a chain point (the note starts as a copy of the default note).
void FillDimElem (API_DimElem& de, const ChainPoint& p, const API_DimensionType& d, const ChainLine& line, bool forceStatic,
				  GS::UniString* customText)
{
	de.base = p.base;
	const bool assoc = p.associative && !forceStatic;
	if (!assoc)
		BNZeroMemory (&de.base.base, sizeof (de.base.base));
	de.note = d.defNote;
	de.note.contentUStr = nullptr;
	if (p.text.has_value ())
		SetNoteCustomText (de.note, customText);
	de.witnessForm = p.witnessForm.value_or (d.defWitnessForm);
	de.witnessVal = p.witnessVal.value_or (d.defWitnessVal);
	de.fixedPos = !assoc;
	de.pos = ProjectOnLine (de.base.loc, line.refC, line.dir);
}


// One ACAPI_Element_Create attempt. Returns APINULLGuid (and err) on failure.
API_Guid CreateChain (API_Element element, const GS::Array<ChainPoint>& pts, const ChainLine& line, bool forceStatic, GSErrCode& err)
{
	const Int32 n = (Int32) pts.GetSize ();
	API_DimensionType& d = element.dimension;
	d.refC = line.refC;
	d.direction.x = line.dir.x;
	d.direction.y = line.dir.y;
	d.nDimElem = n;
	bool anyAssoc = false;
	for (const ChainPoint& p : pts)
		anyAssoc = anyAssoc || (p.associative && !forceStatic);
	d.defStaticDim = !anyAssoc;
	d.usedIn3D = false;

	Memo memo;
	memo->dimElems = reinterpret_cast<API_DimElem**> (BMhAllClear (n * (GSSize) sizeof (API_DimElem)));
	if (memo->dimElems == nullptr || *memo->dimElems == nullptr) {
		err = APIERR_MEMFULL;
		return APINULLGuid;
	}
	std::vector<std::unique_ptr<GS::UniString>> texts;		// custom texts, owned here (detached below)
	for (Int32 i = 0; i < n; ++i) {
		GS::UniString* text = nullptr;
		if (pts[i].text.has_value () && !pts[i].text->IsEmpty ()) {
			texts.push_back (std::make_unique<GS::UniString> (*pts[i].text));
			text = texts.back ().get ();
		}
		FillDimElem ((*memo->dimElems)[i], pts[i], d, line, forceStatic, text);
	}

	err = ACAPI_Element_Create (&element, memo.Ptr ());

	for (Int32 i = 0; i < n; ++i)
		(*memo->dimElems)[i].note.contentUStr = nullptr;		// ours: never let the memo dispose them
	return err == NoError ? element.header.guid : APINULLGuid;
}


// Checks that an associative chain measures the expected points (Archicad resolves the
// references itself; a wrong reference shows up as a different location / distance).
bool VerifyChain (const API_Guid& guid, const GS::Array<ChainPoint>& pts, const ChainLine& line)
{
	bool check = false;
	for (const ChainPoint& p : pts) {
		if (p.raw)
			return true;		// caller-supplied references: nothing to compare against
		if (p.associative && !p.base.base.line)
			check = true;
	}
	if (!check)
		return true;

	API_Element e = NewElement (API_DimensionID);
	e.header.guid = guid;
	if (ACAPI_Element_Get (&e) != NoError)
		return true;
	Memo memo;
	if (ACAPI_Element_GetMemo (guid, memo.Ptr (), APIMemoMask_All) != NoError)
		return true;
	const Int32 n = DimElemCount (*memo, e.dimension.nDimElem);
	bool ok = n == (Int32) pts.GetSize ();
	if (ok) {
		std::vector<double> expected, actual;
		for (const ChainPoint& p : pts)
			expected.push_back (Dot (V (p.base.loc), line.dir));
		for (Int32 i = 0; i < n; ++i)
			actual.push_back (Dot (V ((*memo->dimElems)[i].base.loc), line.dir));
		std::sort (expected.begin (), expected.end ());
		std::sort (actual.begin (), actual.end ());
		for (Int32 i = 0; i < n && ok; ++i)
			ok = std::fabs (expected[i] - actual[i]) <= kExactTolerance;
	}
	if (ok && e.dimension.dimAppear == APIApp_Normal) {
		bool anyValue = false;
		for (Int32 i = 1; i < n; ++i)
			anyValue = anyValue || std::fabs ((*memo->dimElems)[i].dimVal) > 1e-9;
		for (Int32 i = 1; i < n && anyValue && ok; ++i) {
			const API_DimElem& a = (*memo->dimElems)[i - 1];
			const API_DimElem& b = (*memo->dimElems)[i];
			const double exp = std::fabs (Dot (Sub (V (b.base.loc), V (a.base.loc)), line.dir));
			ok = std::fabs (std::fabs (b.dimVal) - exp) <= kExactTolerance;
		}
	}
	FreeDimElems (*memo);
	return ok;
}


void MarkStatic (GS::Array<ChainPoint>& pts)
{
	for (ChainPoint& p : pts) {
		p.associative = false;
		p.raw = false;
		BNZeroMemory (&p.base.base, sizeof (p.base.base));
	}
}


// Creates the chain; associative references that Archicad refuses or resolves differently
// fall back to a static dimension at the same points (reported through `warnings`).
API_Guid CreateVerifiedChain (const API_Element& element, GS::Array<ChainPoint>& pts, const ChainLine& line,
							  GS::Array<GS::UniString>& warnings)
{
	bool anyAssoc = false;
	for (const ChainPoint& p : pts)
		anyAssoc = anyAssoc || p.associative;

	GSErrCode err = NoError;
	API_Guid guid = CreateChain (element, pts, line, false, err);
	if (guid == APINULLGuid) {
		if (!anyAssoc || err == APIERR_NEEDSUNDOSCOPE)
			Check (err, "Cannot create the dimension");
		const GSErrCode assocErr = err;
		guid = CreateChain (element, pts, line, true, err);
		if (guid == APINULLGuid)
			Check (err, "Cannot create the dimension");
		warnings.Push ("Archicad refused the associative references (" + ErrorName (assocErr) +
					   "); a static dimension was created at the same points.");
		MarkStatic (pts);
		return guid;
	}
	if (anyAssoc && !VerifyChain (guid, pts, line)) {
		DeleteCreated (guid);
		guid = CreateChain (element, pts, line, true, err);
		if (guid == APINULLGuid)
			Check (err, "Cannot create the dimension");
		warnings.Push ("The associative references did not measure the expected points; a static dimension was created instead.");
		MarkStatic (pts);
	}
	return guid;
}


API_Element DefaultDimension ()
{
	API_Element element = NewElement (API_DimensionID);
	Memo defaults;
	GetDefaults (element, defaults.Ptr ());
	FreeDimElems (*defaults);
	element.dimension.nDimElem = 0;
	return element;
}


API_Guid CreateLinearDimension (const OS& spec)
{
	API_Element element = DefaultDimension ();
	ApplyCommonFields (element, nullptr, spec);
	StringPool pool;
	ApplyLinearStyle (element, nullptr, spec, pool);

	// Points on model elements are floor plan coordinates: create such dimensions on the floor plan
	// (static ones and ones on 2D drafting elements go into the active window's database).
	PlanDatabaseScope planScope (NeedsFloorPlan (ReferencedElements (spec)));
	AnchorCache cache;
	GS::Array<ChainPoint> pts = ResolvePoints (spec, cache);
	const ChainLine line = ComputeLine (spec, pts);
	GS::Array<GS::UniString> warnings;
	return CreateVerifiedChain (element, pts, line, warnings);
}

// --- Serialization -----------------------------------------------------------------------

void SerializeLinear (const API_Element& element, OS& out)
{
	const API_DimensionType& d = element.dimension;
	const bool instance = element.header.guid != APINULLGuid;		// false: tool defaults (get_tool_defaults)
	if (instance) {
		Vec dir { d.direction.x, d.direction.y };
		if (Len (dir) > 1e-12)
			dir = Unit (dir);
		out.Add ("linePoint", CoordObj (d.refC));
		out.Add ("direction", CoordObj (dir.x, dir.y));
		AddAngle (out, "directionAngle", std::atan2 (dir.y, dir.x));
	}
	out.Add ("appearance", NameOf (kAppearances, d.dimAppear));
	out.Add ("textPosition", NameOf (kTextPos, d.textPos));
	out.Add ("textDirection", NameOf (kTextWay, d.textWay));
	out.Add ("linePen", (Int32) d.linPen);
	out.Add ("witnessForm", NameOf (kWitnessForms, d.defWitnessForm));
	out.Add ("witnessVal", d.defWitnessVal);
	if (instance)
		out.Add ("static", d.defStaticDim);
	out.Add ("horizontalText", d.horizontalText);
	out.Add ("onlyDimensionText", d.onlyDimensionText);
	out.Add ("layout", NameOf (kLayouts, d.dimLayout));
	out.Add ("clipOtherSide", d.clipOtherSide);
	AddMarkerJson (out, d.markerData);
	AddNoteJson (out, d.defNote, nullptr, nullptr);
	if (!instance)
		return;

	GS::Array<OS> points;
	GS::Array<double> segments;
	double total = 0.0;
	Int32 associative = 0;
	Memo memo;
	if (ACAPI_Element_GetMemo (element.header.guid, memo.Ptr (), APIMemoMask_All) == NoError) {
		const Int32 n = DimElemCount (*memo, d.nDimElem);
		for (Int32 i = 0; i < n; ++i) {
			const API_DimElem& de = (*memo->dimElems)[i];
			OS p;
			p.Add ("x", de.base.loc.x);
			p.Add ("y", de.base.loc.y);
			p.Add ("linePosition", CoordObj (de.pos));
			AddBaseJson (p, de.base.base);
			if (IsAssociativeBase (de.base.base))
				++associative;
			p.Add ("witnessForm", NameOf (kWitnessForms, de.witnessForm));
			p.Add ("witnessVal", de.witnessVal);
			if (i > 0) {
				p.Add ("distanceFromPrevious", de.dimVal);
				segments.Push (de.dimVal);
				total += de.dimVal;
			}
			if (de.note.contentType == API_NoteContent_Custom)
				p.Add ("text", NoteCustomText (de.note, nullptr));
			points.Push (p);
		}
		FreeDimElems (*memo);
	}
	out.Add ("pointCount", (Int32) points.GetSize ());
	out.Add ("associativePoints", associative);
	out.Add ("points", points);
	out.Add ("segments", segments);
	out.Add ("total", total);
}

// --- Modification --------------------------------------------------------------------------

void ModifyLinear (API_Element& element, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask, const OS& patch)
{
	StringPool& pool = PendingModifyStrings ();
	pool.Clear ();
	ApplyLinearStyle (element, &mask, patch, pool);

	static const char* const chainKeys[] = { "points", "linePoint", "offset", "direction", "witnessForm", "witnessVal", "makeStatic", "pointTexts" };
	bool chain = HasNoteStyleFields (patch);
	for (const char* k : chainKeys)
		chain = chain || patch.Contains (k);
	if (!chain)
		return;

	if (element.header.guid == APINULLGuid) {
		// Tool defaults (set_tool_defaults): the style went into defNote / defWitness* above; there are no points.
		static const char* const geometryKeys[] = { "points", "linePoint", "offset", "direction", "makeStatic", "pointTexts" };
		for (const char* k : geometryKeys) {
			if (patch.Contains (k))
				Fail ("'" + GS::UniString (k) + "' is per-dimension geometry and cannot be stored in the Dimension tool defaults.");
		}
		return;
	}

#define DIM_SET(f) ACAPI_ELEMENT_MASK_SET (mask, API_DimensionType, f)
	API_DimensionType& d = element.dimension;
	LoadMemo (element.header.guid, memo, APIMemoMask_All);
	memoMask = APIMemoMask_All;

	Vec dir { d.direction.x, d.direction.y };
	dir = Len (dir) > 1e-12 ? Unit (dir) : Vec { 1.0, 0.0 };
	if (auto nd = OptDirection (patch, "direction")) {
		dir = *nd;
		d.direction.x = dir.x;
		d.direction.y = dir.y;
		DIM_SET (direction); DIM_SET (direction.x); DIM_SET (direction.y);
	}

	if (patch.Contains ("points")) {
		AnchorCache cache;
		GS::Array<ChainPoint> pts = ResolvePoints (patch, cache);
		const Int32 n = (Int32) pts.GetSize ();
		FreeDimElems (memo);
		memo.dimElems = reinterpret_cast<API_DimElem**> (BMhAllClear (n * (GSSize) sizeof (API_DimElem)));
		if (memo.dimElems == nullptr || *memo.dimElems == nullptr)
			Fail ("Out of memory while building the dimension points.", APIERR_MEMFULL);
		ChainLine line { dir, d.refC };
		bool anyAssoc = false;
		for (Int32 i = 0; i < n; ++i) {
			// Custom texts are handed to the memo (disposed with it, as in the DevKit examples).
			GS::UniString* text = (pts[i].text.has_value () && !pts[i].text->IsEmpty ()) ? new GS::UniString (*pts[i].text) : nullptr;
			FillDimElem ((*memo.dimElems)[i], pts[i], d, line, false, text);
			anyAssoc = anyAssoc || pts[i].associative;
		}
		d.nDimElem = n;
		d.defStaticDim = !anyAssoc;
		DIM_SET (nDimElem); DIM_SET (defStaticDim);
	}

	const Int32 n = DimElemCount (memo, d.nDimElem);
	if (n < 1)
		Fail ("This dimension has no points; recreate it with create_dimensions.");

	if (GetBool (patch, "makeStatic", false)) {
		for (Int32 i = 0; i < n; ++i) {
			API_DimElem& de = (*memo.dimElems)[i];
			BNZeroMemory (&de.base.base, sizeof (de.base.base));
			de.fixedPos = true;
		}
		d.defStaticDim = true;
		DIM_SET (defStaticDim);
	}

	if (auto lp = OptCoord (patch, "linePoint")) {
		d.refC = *lp;
		DIM_SET (refC); DIM_SET (refC.x); DIM_SET (refC.y);
	} else if (auto off = OptDouble (patch, "offset")) {
		d.refC = C (Add (V ((*memo.dimElems)[0].base.loc), Mul (LeftNormal (dir), *off)));
		DIM_SET (refC); DIM_SET (refC.x); DIM_SET (refC.y);
	}

	for (Int32 i = 0; i < n; ++i) {
		API_DimElem& de = (*memo.dimElems)[i];
		de.pos = ProjectOnLine (de.base.loc, d.refC, dir);
		if (patch.Contains ("witnessForm"))
			de.witnessForm = d.defWitnessForm;
		if (patch.Contains ("witnessVal"))
			de.witnessVal = d.defWitnessVal;
		if (HasNoteStyleFields (patch))
			CopyNoteStyle (d.defNote, de.note);
	}

	// Custom texts of individual points, without replacing the points.
	if (patch.Contains ("pointTexts")) {
		for (const OS& t : GetObjectArray (patch, "pointTexts")) {
			const Int32 idx = GetInt (t, "index");
			if (idx < 0 || idx >= n)
				Fail ("pointTexts[].index must be 0.." + GS::ValueToUniString (n - 1) + " (index into the dimension's points; "
					  "in a Normal chain point i carries the text of the segment ending at it).");
			if (!t.Contains ("text"))
				Fail ("Each pointTexts item needs 'text' (custom string, or \"\"/false for the measured value).");
			const GS::UniString text = ParseCustomText (t, "text", "pointTexts[].text");
			API_NoteType& note = (*memo.dimElems)[idx].note;
			if (note.contentUStr != nullptr) {
				delete note.contentUStr;		// the memo's own string (as in the DevKit's Do_Dimensions_Test)
				note.contentUStr = nullptr;
			}
			// Handed to the memo, disposed with it.
			SetNoteCustomText (note, text.IsEmpty () ? nullptr : new GS::UniString (text));
		}
	}
#undef DIM_SET
}

// --- DimensionWalls ----------------------------------------------------------------------------

struct WallInfo {
	API_Guid	guid;
	API_Element	element;
	RefLine		rl;
	Vec			u;						// unit vector begin -> end (chord for curved walls)
	double		leftExtent = 0.0;		// wall body width left / right of the reference line (relative to u)
	double		rightExtent = 0.0;
};


WallInfo LoadWall (const API_Guid& guid)
{
	WallInfo w;
	w.guid = guid;
	w.element = GetElement (guid);
	if (w.element.header.type.typeID != API_WallID)
		Fail ("Element " + GuidStr (guid) + " is a " + ElemTypeName (w.element.header.type) +
			  ", not a Wall. dimension_walls only takes walls; use create_dimensions for other elements.");
	w.rl = GetRefLine (w.element);
	if (Dist (w.rl.begin, w.rl.end) < 1e-9)
		Fail ("Wall " + GuidStr (guid) + " has zero length.");
	w.u = Unit (Sub (V (w.rl.end), V (w.rl.begin)));
	const double t = w.rl.thickness;
	switch (w.element.wall.referenceLineLocation) {
		case APIWallRefLine_Outside:	w.leftExtent = 0.0;		w.rightExtent = t;		break;	// body right of begin->end
		case APIWallRefLine_Inside:		w.leftExtent = t;		w.rightExtent = 0.0;	break;
		case APIWallRefLine_Center:		w.leftExtent = t / 2;	w.rightExtent = t / 2;	break;
		default:						w.leftExtent = t;		w.rightExtent = t;		break;	// core reference lines: skins on both sides
	}
	if (w.element.wall.flipped)
		std::swap (w.leftExtent, w.rightExtent);
	const double off = std::fabs (w.element.wall.offset);
	w.leftExtent += off;
	w.rightExtent += off;
	return w;
}


struct Target {
	API_Coord	p;
	API_Guid	element;
	bool		opening = false;
};


void AddWallTargets (const WallInfo& w, const Vec& d, bool includeOpenings, bool openingCenters,
					 GS::Array<Target>& targets, GS::Array<GS::UniString>& warnings)
{
	targets.Push ({ w.rl.begin, w.guid, false });
	targets.Push ({ w.rl.end, w.guid, false });
	if (!includeOpenings)
		return;

	GS::Array<API_Guid> openings;
	for (API_ElemTypeID typeID : { API_WindowID, API_DoorID }) {
		GS::Array<API_Guid> list;
		if (ACAPI_Element_GetConnectedElements (w.guid, typeID, &list) == NoError)
			openings.Append (list);
	}
	if (openings.IsEmpty ())
		return;
	if (std::fabs (w.rl.arcAngle) > 1e-9) {
		warnings.Push ("Wall " + GuidStr (w.guid) + " is curved: its openings are not dimensioned.");
		return;
	}
	if (std::fabs (Cross (w.u, d)) > 1e-3)
		return;		// openings of walls across the chain would all project onto the wall's own position

	for (const API_Guid& g : openings) {
		API_Element o = NewElement (API_ZombieElemID);
		o.header.guid = g;
		if (ACAPI_Element_Get (&o) != NoError)
			continue;
		const API_WindowType& wd = o.header.type.typeID == API_DoorID ? o.door : o.window;
		const Vec center = Add (V (w.rl.begin), Mul (w.u, wd.objLoc));
		if (openingCenters) {
			targets.Push ({ C (center), g, true });
		} else {
			const Vec half = Mul (w.u, wd.openingBase.width / 2.0);
			targets.Push ({ C (Sub (center, half)), g, true });
			targets.Push ({ C (Add (center, half)), g, true });
		}
	}
}


OS ChainResultJson (const API_Guid& guid, const char* role, const GS::Array<WallInfo>& walls, const GS::Array<GS::UniString>& warnings)
{
	OS out;
	out.Add ("guid", GuidStr (guid));
	out.Add ("type", GS::UniString ("Dimension"));
	out.Add ("role", GS::UniString (role));
	GS::Array<GS::UniString> wallIds;
	for (const WallInfo& w : walls)
		wallIds.Push (GuidStr (w.guid));
	out.Add ("walls", wallIds);
	if (!warnings.IsEmpty ())
		out.Add ("warnings", warnings);
	// Compact summary of what was created (full data: get_element_details).
	API_Element e = NewElement (API_DimensionID);
	e.header.guid = guid;
	if (ACAPI_Element_Get (&e) == NoError) {
		out.Add ("linePoint", CoordObj (e.dimension.refC));
		Memo memo;
		if (ACAPI_Element_GetMemo (guid, memo.Ptr (), APIMemoMask_All) == NoError) {
			const Int32 n = DimElemCount (*memo, e.dimension.nDimElem);
			GS::Array<double> segments;
			double total = 0.0;
			Int32 assoc = 0;
			for (Int32 i = 0; i < n; ++i) {
				const API_DimElem& de = (*memo->dimElems)[i];
				if (IsAssociativeBase (de.base.base))
					++assoc;
				if (i > 0) {
					segments.Push (de.dimVal);
					total += de.dimVal;
				}
			}
			out.Add ("pointCount", n);
			out.Add ("associativePoints", assoc);
			out.Add ("segments", segments);
			out.Add ("total", total);
			FreeDimElems (*memo);
		}
	}
	return out;
}


GS::Array<OS> DimensionWallGroup (const GS::Array<WallInfo>& walls, const OS& params, const std::optional<Vec>& forcedDir)
{
	const Vec d = forcedDir.has_value () ? *forcedDir : walls[0].u;
	const Vec n = LeftNormal (d);
	GS::Array<GS::UniString> warnings;

	const bool includeOpenings = GetBool (params, "includeOpenings", true);
	const GS::UniString openingPoints = GetString (params, "openingPoints", "Jambs");
	if (!EqualsIgnoreCase (openingPoints, "Jambs") && !EqualsIgnoreCase (openingPoints, "Centers"))
		Fail ("openingPoints must be \"Jambs\" (both sides of each opening) or \"Centers\".");
	const bool centers = EqualsIgnoreCase (openingPoints, "Centers");

	GS::Array<Target> collected;
	for (const WallInfo& w : walls)
		AddWallTargets (w, d, includeOpenings, centers, collected, warnings);

	std::vector<Target> targets;
	for (const Target& t : collected)
		targets.push_back (t);
	std::stable_sort (targets.begin (), targets.end (), [&] (const Target& a, const Target& b) {
		return Dot (V (a.p), d) < Dot (V (b.p), d);
	});
	std::vector<Target> unique;
	for (const Target& t : targets) {
		if (!unique.empty () && std::fabs (Dot (V (t.p), d) - Dot (V (unique.back ().p), d)) < 1e-3) {
			if (unique.back ().opening && !t.opening)
				unique.back () = t;		// prefer the wall end over an opening jamb at the same position
			continue;
		}
		unique.push_back (t);
	}
	if (unique.size () < 2)
		Fail ("The walls have fewer than two distinct points along the dimension direction; check 'direction' or use mode \"eachWall\".");

	// Dimension line position: beyond the outermost wall face on the chosen side.
	double sMax = -std::numeric_limits<double>::max ();
	double sMin = std::numeric_limits<double>::max ();
	double maxThickness = 0.0;
	for (const WallInfo& w : walls) {
		const bool parallel = std::fabs (Cross (w.u, d)) < 1e-3;
		const bool same = Dot (w.u, d) >= 0.0;
		const double widest = std::max (w.leftExtent, w.rightExtent);
		const double left = parallel ? (same ? w.leftExtent : w.rightExtent) : widest;
		const double right = parallel ? (same ? w.rightExtent : w.leftExtent) : widest;
		for (const API_Coord& p : { w.rl.begin, w.rl.end }) {
			const double s = Dot (V (p), n);
			sMax = std::max (sMax, s + left);
			sMin = std::min (sMin, s - right);
		}
		maxThickness = std::max (maxThickness, w.rl.thickness + std::fabs (w.element.wall.offset));
	}
	const double distance = GetDouble (params, "distance", 1.0);
	if (distance < 0.0)
		Fail ("distance must be >= 0 (meters from the wall face to the dimension line).");
	const GS::UniString side = GetString (params, "side", "Auto");
	bool onLeft;
	if (EqualsIgnoreCase (side, "Left")) {
		onLeft = true;
	} else if (EqualsIgnoreCase (side, "Right")) {
		onLeft = false;
	} else if (EqualsIgnoreCase (side, "Auto")) {
		const WallInfo& w0 = walls[0];
		const bool same = Dot (w0.u, d) >= 0.0;
		const double l0 = same ? w0.leftExtent : w0.rightExtent;
		const double r0 = same ? w0.rightExtent : w0.leftExtent;
		onLeft = !(l0 > r0 + 1e-9);		// the side without (or with less of) the first wall's body
	} else {
		Fail ("side must be \"Auto\" (away from the first wall's body), \"Left\" or \"Right\" (relative to the first wall's begin->end, or to 'direction').");
	}
	const double lineS = onLeft ? sMax + distance : sMin - distance;
	const double t0 = Dot (V (unique[0].p), d);
	ChainLine line;
	line.dir = d;
	line.refC = C (Add (Mul (d, t0), Mul (n, lineS)));

	// Points (associative to wall ends / opening anchors when possible).
	const bool associative = GetBool (params, "associative", true);
	AnchorCache cache;
	GS::Array<ChainPoint> pts;
	Int32 unlinked = 0;
	for (const Target& t : unique) {
		ChainPoint cp;
		cp.base.loc = t.p;
		cp.references = true;
		if (associative) {
			const GS::Array<Anchor>& anchors = cache.Get (t.element);
			const Anchor* a = t.opening ? AnchorAtProjection (anchors, t.p, d, kExactTolerance, maxThickness + 0.3)
										: NearestAnchor (anchors, t.p, kExactTolerance);
			if (a != nullptr) {
				cp.base.base = BaseFromNeig (a->neig);
				cp.base.loc.x = a->coord.x;
				cp.base.loc.y = a->coord.y;
				cp.associative = true;
			} else {
				++unlinked;
			}
		}
		pts.Push (cp);
	}
	if (unlinked > 0)
		warnings.Push (GS::ValueToUniString (unlinked) + " of " + GS::ValueToUniString ((Int32) pts.GetSize ()) +
					   " points have no matching element anchor and are static (they will not follow the walls/openings).");

	API_Element element = DefaultDimension ();
	ApplyCommonFields (element, nullptr, params);
	if (!params.Contains ("storyIndex"))
		element.header.floorInd = walls[0].element.header.floorInd;
	StringPool pool;
	ApplyLinearStyle (element, nullptr, params, pool);

	GS::Array<OS> results;
	const API_Guid guid = CreateVerifiedChain (element, pts, line, warnings);
	results.Push (ChainResultJson (guid, "chain", walls, warnings));

	if (GetBool (params, "overall", false) && pts.GetSize () > 2) {
		const double spacing = GetDouble (params, "overallSpacing", 0.8);
		if (spacing <= 0.0)
			Fail ("overallSpacing must be > 0 (meters between the chain and the overall dimension line).");
		GS::Array<ChainPoint> ends;
		ends.Push (pts[0]);
		ends.Push (pts.GetLast ());
		ChainLine outer = line;
		outer.refC = C (Add (V (line.refC), Mul (n, onLeft ? spacing : -spacing)));
		GS::Array<GS::UniString> outerWarnings;
		const API_Guid overallGuid = CreateVerifiedChain (element, ends, outer, outerWarnings);
		results.Push (ChainResultJson (overallGuid, "overall", walls, outerWarnings));
	}
	return results;
}


// Offsets (m, + = left of begin->end) of the wall's two faces from its reference line, measured from the wall's
// body corner hotspots. Returns false when the hotspots do not span a width (then the caller estimates).
bool MeasureWallFaces (const WallInfo& w, double& leftFace, double& rightFace)
{
	const Vec n = LeftNormal (w.u);
	double maxOff = -std::numeric_limits<double>::max ();
	double minOff = std::numeric_limits<double>::max ();
	Int32 count = 0;
	for (const Anchor& a : CollectAnchors (w.guid)) {
		const API_NeigID id = a.neig.neigID;
		if (id != APINeig_Wall && id != APINeig_WallPl && id != APINeig_WallPlClOff)
			continue;
		const double off = Dot (Sub ({ a.coord.x, a.coord.y }, V (w.rl.begin)), n);
		maxOff = std::max (maxOff, off);
		minOff = std::min (minOff, off);
		++count;
	}
	if (count < 2 || maxOff - minOff < 1e-4)
		return false;
	leftFace = maxOff;
	rightFace = minOff;
	return true;
}


// Estimate of the face offsets from the wall's settings (used when no corner hotspots are available).
void EstimateWallFaces (const WallInfo& w, double& leftFace, double& rightFace)
{
	const API_WallType& wall = w.element.wall;
	const double t = w.rl.thickness;
	// offsetFromOutside: distance between the reference line and the outside face; unflipped walls have
	// their outside face on the left of begin->end (reference line "Outside" = body on the right).
	const double outside = std::max (0.0, std::min (t, wall.offsetFromOutside));
	if (!wall.flipped) {
		leftFace = outside;
		rightFace = outside - t;
	} else {
		leftFace = t - outside;
		rightFace = -outside;
	}
}


// mode "Thickness": one dimension across a straight wall, associative to both wall faces (Archicad's wall-face
// references: line = true, inIndex 11 / 21 — DevKit Element_Basics.cpp, Do_CreateAssociativeDimensions).
GS::Array<OS> DimensionWallThickness (const WallInfo& w, const OS& params)
{
	if (std::fabs (w.rl.arcAngle) > 1e-9)
		Fail ("Wall " + GuidStr (w.guid) + " is curved; mode \"Thickness\" works with straight walls only "
			  "(use create_dimensions with a point on each face and a radial 'direction').");
	if (w.element.wall.type == APIWtyp_Poly)
		Fail ("Wall " + GuidStr (w.guid) + " is a polygonal wall (no parallel faces); dimension it with create_dimensions "
			  "using its corner points (see get_dimension_anchors).");
	const double length = Dist (w.rl.begin, w.rl.end);
	const double s = GetDouble (params, "thicknessPosition", length / 2.0);
	const Vec n = LeftNormal (w.u);
	const Vec onRef = Add (V (w.rl.begin), Mul (w.u, s));

	GS::Array<GS::UniString> warnings;
	double leftFace = 0.0, rightFace = 0.0;
	const bool measured = MeasureWallFaces (w, leftFace, rightFace);
	if (!measured)
		EstimateWallFaces (w, leftFace, rightFace);
	const bool trapezoid = w.element.wall.type == APIWtyp_Trapez;
	const bool associative = GetBool (params, "associative", true);

	GS::Array<ChainPoint> pts;
	for (int k = 0; k < 2; ++k) {
		ChainPoint cp;
		cp.base.loc = C (Add (onRef, Mul (n, k == 0 ? rightFace : leftFace)));
		cp.references = true;
		if (associative) {
			API_Base& b = cp.base.base;
			b.type = w.element.header.type;
			b.guid = w.guid;
			b.line = true;
			b.special = 0;
			b.inIndex = k == 0 ? 11 : 21;
			cp.associative = true;
			cp.raw = true;		// line references: Archicad resolves the face positions itself
		}
		pts.Push (cp);
	}
	ChainLine line;
	line.dir = n;
	line.refC = C (onRef);		// the dimension line crosses the wall at thicknessPosition

	API_Element element = DefaultDimension ();
	ApplyCommonFields (element, nullptr, params);
	if (!params.Contains ("storyIndex"))
		element.header.floorInd = w.element.header.floorInd;
	StringPool pool;
	ApplyLinearStyle (element, nullptr, params, pool);

	API_Guid guid = CreateVerifiedChain (element, pts, line, warnings);

	// Verify the associative measurement against the corner hotspots (exact for straight, non-trapezoid walls).
	if (associative && pts[0].associative && measured && !trapezoid) {
		const double expected = leftFace - rightFace;
		double value = 0.0;
		API_Element e = NewElement (API_DimensionID);
		e.header.guid = guid;
		Memo memo;
		if (ACAPI_Element_Get (&e) == NoError && ACAPI_Element_GetMemo (guid, memo.Ptr (), APIMemoMask_All) == NoError) {
			const Int32 cnt = DimElemCount (*memo, e.dimension.nDimElem);
			for (Int32 i = 1; i < cnt; ++i)
				value += std::fabs ((*memo->dimElems)[i].dimVal);
			FreeDimElems (*memo);
		}
		if (value > 1e-9 && std::fabs (value - expected) > kExactTolerance) {
			DeleteCreated (guid);
			MarkStatic (pts);
			GSErrCode err = NoError;
			guid = CreateChain (element, pts, line, true, err);
			if (guid == APINULLGuid)
				Check (err, "Cannot create the wall thickness dimension");
			warnings.Push ("The wall-face references measured " + GS::ValueToUniString (value) + " m instead of " +
						   GS::ValueToUniString (expected) + " m; a static dimension was created at the wall faces instead.");
		}
	}
	if (!measured && !pts[0].associative)
		warnings.Push ("The wall has no corner hotspots; the face positions of the static points were estimated from its settings "
					   "(check the measured total).");

	GS::Array<WallInfo> walls;
	walls.Push (w);
	GS::Array<OS> results;
	results.Push (ChainResultJson (guid, "thickness", walls, warnings));
	return results;
}


OS DimensionWalls (const OS& params)
{
	const GS::Array<API_Guid> guids = GetGuidArray (params, "walls", true);
	if (guids.IsEmpty ())
		Fail ("Pass at least one wall GUID in 'walls'.");
	if (guids.GetSize () > 500)
		Fail ("Too many walls (max 500 per call).");
	const GS::UniString mode = GetString (params, "mode", "Chain");
	const bool chain = EqualsIgnoreCase (mode, "Chain");
	const bool thickness = EqualsIgnoreCase (mode, "Thickness");
	if (!chain && !thickness && !EqualsIgnoreCase (mode, "EachWall"))
		Fail ("mode must be \"Chain\" (one chain through all walls), \"EachWall\" (one dimension per wall) or "
			  "\"Thickness\" (one dimension across each wall measuring its width).");
	const std::optional<Vec> direction = OptDirection (params, "direction");
	if (thickness && direction.has_value ())
		Fail ("'direction' does not apply to mode \"Thickness\" (the dimension is always perpendicular to the wall).");

	GS::Array<OS> results;
	PlanDatabaseScope planScope (true);
	Undoable (GetString (params, "undoName", "Dimension walls (Claude)"), [&] () {
		auto run = [&] (const GS::Array<API_Guid>& group) {
			OS r = Try ([&] () -> OS {
				GS::Array<WallInfo> walls;
				for (const API_Guid& g : group)
					walls.Push (LoadWall (g));
				if (thickness)
					return OS ("dimensions", DimensionWallThickness (walls[0], params));
				return OS ("dimensions", DimensionWallGroup (walls, params, direction));
			});
			GS::Array<OS> dims;
			if (r.Contains ("dimensions") && r.Get ("dimensions", dims)) {
				results.Append (dims);
			} else {
				if (!chain)
					r.Add ("wall", GuidStr (group[0]));
				results.Push (r);
			}
		};
		if (chain) {
			run (guids);
		} else {
			for (const API_Guid& g : guids) {
				GS::Array<API_Guid> one;
				one.Push (g);
				run (one);
			}
		}
	});
	return OS ("results", results);
}

// --- GetDimensionAnchors -------------------------------------------------------------------------

OS GetDimensionAnchors (const OS& params)
{
	const GS::Array<API_Guid> guids = GetGuidArray (params, "elements", true);
	if (guids.IsEmpty ())
		Fail ("Pass at least one element GUID in 'elements'.");
	const std::optional<API_Coord> nearPoint = OptCoord (params, "near");
	const std::optional<double> radius = OptDouble (params, "radius");
	const Int32 limit = std::max ((Int32) 1, std::min (GetInt (params, "limit", 100), (Int32) 2000));

	GS::Array<OS> results;
	for (const API_Guid& guid : guids) {
		results.Push (Try ([&] () -> OS {
			GS::Array<API_Guid> one;
			one.Push (guid);
			PlanDatabaseScope planScope (NeedsFloorPlan (one));		// model elements: Floor Plan coordinates
			const API_Elem_Head head = GetHeader (guid);
			GS::Array<Anchor> anchors = CollectAnchors (guid);
			struct Item { const Anchor* a; double dist; };
			std::vector<Item> items;
			for (const Anchor& a : anchors) {
				double dist = 0.0;
				if (nearPoint.has_value ()) {
					API_Coord c; c.x = a.coord.x; c.y = a.coord.y;
					dist = Dist (c, *nearPoint);
					if (radius.has_value () && dist > *radius)
						continue;
				}
				items.push_back ({ &a, dist });
			}
			if (nearPoint.has_value ())
				std::stable_sort (items.begin (), items.end (), [] (const Item& x, const Item& y) { return x.dist < y.dist; });
			GS::Array<OS> list;
			for (const Item& it : items) {
				if ((Int32) list.GetSize () >= limit)
					break;
				OS o;
				o.Add ("x", it.a->coord.x);
				o.Add ("y", it.a->coord.y);
				o.Add ("z", it.a->coord.z);
				o.Add ("neig", NeigName (it.a->neig.neigID));
				o.Add ("inIndex", (Int32) it.a->neig.inIndex);
				o.Add ("line", IsLineNeig (it.a->neig.neigID));
				o.Add ("special", IsSpecialNeig (it.a->neig.neigID));
				o.Add ("usableAsPoint", IsDimensionPointNeig (it.a->neig.neigID));
				o.Add ("source", it.a->source);
				if (nearPoint.has_value ())
					o.Add ("distance", it.dist);
				list.Push (o);
			}
			OS out;
			out.Add ("guid", GuidStr (guid));
			out.Add ("type", ElemTypeName (head.type));
			out.Add ("anchorCount", (Int32) items.size ());
			out.Add ("anchors", list);
			return out;
		}));
	}
	return OS ("elements", results);
}

} // namespace


void RegisterDimensionCommands ()
{
	RegisterAdapter ({ API_DimensionID, CreateLinearDimension, SerializeLinear, ModifyLinear });
	dimension::RegisterSpecialDimensionAdapters ();

	RegisterCommand ("DimensionWalls",
		"Creates linear dimension chains along walls in one undo step (on the Floor Plan). Input: {walls: [guid], mode: \"Chain\" "
		"(default: one chain through all walls' end points, projected on the direction) | \"EachWall\" (one dimension per wall, "
		"parallel to it) | \"Thickness\" (one dimension across each straight wall, linked to both faces), direction? "
		"(\"Horizontal\"|\"Vertical\"|degrees|{x,y}; default: first wall begin->end; not with Thickness), side: \"Auto\" "
		"(default, away from the wall body) | \"Left\" | \"Right\", distance (m from the outermost wall face, default 1), "
		"includeOpenings (default true: window/door jambs of walls parallel to the chain), openingPoints \"Jambs\"|\"Centers\", "
		"overall (bool: add an overall dimension), overallSpacing (m, default 0.8), thicknessPosition (Thickness: m from the wall "
		"begin along its reference line where the dimension crosses it, default: middle), associative (default true), storyIndex?, "
		"layer?, + linear dimension style fields}. Output: {results: [{guid, role: \"chain\"|\"overall\"|\"thickness\", walls, pointCount, "
		"associativePoints, segments, total, linePoint, warnings?} | {error, wall?}]}.",
		DimensionWalls);

	RegisterCommand ("GetDimensionAnchors",
		"Lists the points of elements that associative dimensions can attach to (element hotspots + probed reference points). "
		"Input: {elements: [guid], near?: {x,y} (sort by distance), radius? (m, with near), limit? (per element, default 100)}. "
		"Output: {elements: [{guid, type, anchorCount, anchors: [{x, y, z, neig, inIndex, line, special, usableAsPoint, source, distance?}]}]}.",
		GetDimensionAnchors);
}

} // namespace cc
