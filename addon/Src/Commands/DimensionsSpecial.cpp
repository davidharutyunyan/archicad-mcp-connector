// *****************************************************************************
// DimensionsSpecial — level, radial and angle dimension adapters (dimensions family).
//
// LevelDimension create / modify fields (meters, degrees, paper mm for sizes):
//   position* {x,y} (marker location), element? (guid: show that element's level, e.g. a Slab/Mesh;
//   "" or false on modify = back to the story level), level? (static value in m; makes it static),
//   static?, elevationReference (alias: origin) "ProjectZero"|"ReferenceLevel1"|"ReferenceLevel2"|"StoredOrigin"|"SeaLevel",
//   markerStyle 0..9, markerSize, pen, markerAngle, showPlusSign, text (custom text, "" = measured),
//   secondText (second note of marker styles 8/9), text style fields
//
// RadialDimension: element* (Arc, Circle, curved Wall, curved Beam), at? {x,y} (point near the arc
//   where the dimension touches it; default: arc middle), lineEnd? {x,y} (end of the radial line;
//   default: the arc point), inIndex?/line?/special? (expert: explicit reference), showCenter, prefix,
//   textPosition, textDirection ("Horizontal"|"Vertical"|"Radial"), linePen, marker*, text*, onlyDimensionText
//
// AngleDimension: line1 {begin, end} + line2 {begin, end} (static) | elements [guid, guid]
//   (straight walls / beams / lines; associative when possible), arcPoint? {x,y} (point the arc passes
//   through), radius? (m, default 1), smallArc (default true), textPosition, textDirection
//   ("Parallel"|"Horizontal"|"Perpendicular"), witnessForm, witnessVal, linePen, marker*, text*
// *****************************************************************************

#include "Commands/DimensionsCommon.hpp"
#include "Core/Command.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cc {
namespace dimension {

namespace {

constexpr double kExactTolerance = 0.002;

// ============================================================================
// Level dimension
// ============================================================================

void ApplyLevelFields (API_Element& element, API_Element* mask, const OS& spec, StringPool& pool)
{
	API_LevelDimensionType& l = element.levelDimension;
#define LEV_SET(f) if (mask) ACAPI_ELEMENT_MASK_SET (*mask, API_LevelDimensionType, f)
	if (auto c = OptCoord (spec, "position")) {
		l.loc = *c;
		LEV_SET (loc); LEV_SET (loc.x); LEV_SET (loc.y);
	}
	if (spec.Contains ("element")) {
		bool detach = false;
		if (spec.IsBool ("element")) {
			if (GetBool (spec, "element"))
				Fail ("'element' must be an element GUID, or \"\"/false to show the story level.");
			detach = true;
		} else if (spec.IsString ("element") && GetString (spec, "element").IsEmpty ()) {
			detach = true;
		}
		if (detach) {
			l.parentType = API_ElemType (API_ZombieElemID);
			l.parentGuid = APINULLGuid;
		} else {
			const API_Guid guid = GetGuid (spec, "element");
			const API_Elem_Head head = GetHeader (guid);
			l.parentType = head.type;
			l.parentGuid = guid;
		}
		if (!spec.Contains ("level") && !spec.Contains ("static"))
			l.staticLevel = false;
		LEV_SET (parentType); LEV_SET (parentGuid); LEV_SET (staticLevel);
	}
	if (auto v = OptDouble (spec, "level"))		{ l.level = *v; l.staticLevel = true; LEV_SET (level); LEV_SET (staticLevel); }
	if (auto b = OptBool (spec, "static"))			{ l.staticLevel = *b; LEV_SET (staticLevel); }
	// "elevationReference" (the documented name; "origin" is accepted as an alias)
	const char* referenceKey = spec.Contains ("elevationReference") ? "elevationReference" : "origin";
	if (Has (spec, referenceKey))					{ l.origin = (API_DimOriginID) ParseNamed (kOrigins, spec, referenceKey); LEV_SET (origin); }
	if (auto v = OptInt (spec, "markerStyle")) {
		if (*v < 0 || *v > 9)
			Fail ("markerStyle must be 0..9 (Archicad's level dimension marker forms).");
		l.dimForm = (short) *v; LEV_SET (dimForm);
	}
	if (auto v = OptDouble (spec, "markerSize")) {
		if (*v <= 0.0)
			Fail ("markerSize must be positive (paper millimeters).");
		l.markerSize = *v; LEV_SET (markerSize);
	}
	if (auto p = OptPen (spec, "pen"))				{ l.pen = *p; LEV_SET (pen); }
	if (auto a = OptAngle (spec, "markerAngle"))	{ l.angle = *a; LEV_SET (angle); }
	if (auto b = OptBool (spec, "showPlusSign"))	{ l.needPlus = *b; LEV_SET (needPlus); }
#undef LEV_SET
	ApplyNoteFields (l.note1, MaskOf (element, mask, l.note1), spec, pool, "text");
	ApplyNoteFields (l.note2, MaskOf (element, mask, l.note2), spec, pool, "secondText");
}


API_Guid CreateLevelDimension (const OS& spec)
{
	if (!Has (spec, "position"))
		Fail ("LevelDimension requires 'position' {x, y}: where the level marker is placed.");
	GS::Array<API_Guid> parents;
	if (spec.Contains ("element") && !spec.IsBool ("element") && !(spec.IsString ("element") && GetString (spec, "element").IsEmpty ()))
		parents.Push (GetGuid (spec, "element"));
	PlanDatabaseScope planScope (NeedsFloorPlan (parents));		// slabs/meshes live on the floor plan
	API_Element element = NewElement (API_LevelDimensionID);
	GetDefaults (element, nullptr);
	ApplyCommonFields (element, nullptr, spec);
	API_LevelDimensionType& l = element.levelDimension;
	if (!spec.Contains ("element")) {
		l.parentType = API_ElemType (API_ZombieElemID);		// story level
		l.parentGuid = APINULLGuid;
	}
	if (!spec.Contains ("level") && !spec.Contains ("static"))
		l.staticLevel = false;
	StringPool pool;
	ApplyLevelFields (element, nullptr, spec, pool);

	const GSErrCode err = ACAPI_Element_Create (&element, nullptr);
	if (err != NoError) {
		GS::UniString msg = "Cannot create the level dimension: " + ErrorName (err);
		if (spec.Contains ("element"))
			msg += ". Level dimensions attach to elements with a horizontal surface (Slab, Mesh, Roof, Shell, Stair, Object...); "
				   "omit 'element' to show the story level, or give 'level' for a static value.";
		Fail (msg, err);
	}
	return element.header.guid;
}


void SerializeLevel (const API_Element& element, OS& out)
{
	const API_LevelDimensionType& l = element.levelDimension;
	const bool instance = element.header.guid != APINULLGuid;		// false: tool defaults (get_tool_defaults)
	if (instance) {
		out.Add ("position", CoordObj (l.loc));
		out.Add ("level", l.level);
		out.Add ("static", l.staticLevel);
		if (l.parentType.typeID != API_ZombieElemID && l.parentGuid != APINULLGuid) {
			out.Add ("element", GuidStr (l.parentGuid));
			out.Add ("elementType", ElemTypeName (l.parentType));
		}
		try {
			out.Add ("storyLevel", StoryLevel (element.header.floorInd));
		} catch (const Error&) {
		}
	}
	out.Add ("elevationReference", NameOf (kOrigins, l.origin));
	out.Add ("markerStyle", (Int32) l.dimForm);
	out.Add ("markerSize", l.markerSize);
	out.Add ("pen", (Int32) l.pen);
	AddAngle (out, "markerAngle", l.angle);
	out.Add ("showPlusSign", l.needPlus);
	ElementStrings strings;
	if (instance && (l.note1.contentType == API_NoteContent_Custom || l.note2.contentType == API_NoteContent_Custom))
		strings = ReadElementStrings (element.header.guid, API_LevelDimensionID);
	AddNoteJson (out, l.note1, &strings.first, "text");
	if (l.note2.contentType == API_NoteContent_Custom)
		out.Add ("secondText", NoteCustomText (l.note2, &strings.second));
}


void ModifyLevel (API_Element& element, API_Element& mask, API_ElementMemo&, UInt64&, const OS& patch)
{
	StringPool& pool = PendingModifyStrings ();
	pool.Clear ();
	ApplyLevelFields (element, &mask, patch, pool);
}

// ============================================================================
// Radial dimension
// ============================================================================

struct ArcGeometry {
	API_Coord	center;
	double		radius = 0.0;
	API_Coord	middle;			// default point where the dimension touches the arc
	double		minRadius = 0.0;	// accepted measured radius range (walls: faces or reference line)
	double		maxRadius = 0.0;
};


ArcGeometry GetArcGeometry (const API_Element& e)
{
	ArcGeometry g;
	switch (e.header.type.typeID) {
		case API_ArcID:
		case API_CircleID: {
			const API_ArcType& a = e.header.type.typeID == API_ArcID ? e.arc : e.circle;
			if (a.ratio > 0.0 && std::fabs (a.ratio - 1.0) > 1e-6)
				Fail ("Element " + GuidStr (e.header.guid) + " is an ellipse; radial dimensions need a circular arc or circle.");
			g.center = a.origC;
			g.radius = a.r;
			double mid = kPi / 4.0;
			if (e.header.type.typeID == API_ArcID && !a.whole) {
				double span = a.endAng - a.begAng;
				while (span <= 0.0)
					span += 2.0 * kPi;
				mid = a.begAng + span / 2.0;
			}
			g.middle = C (Add (V (g.center), { g.radius * std::cos (mid), g.radius * std::sin (mid) }));
			g.minRadius = g.maxRadius = g.radius;
			break;
		}
		case API_WallID:
		case API_BeamID: {
			const RefLine rl = GetRefLine (e);
			if (std::fabs (rl.arcAngle) < 1e-9)
				Fail ("Element " + GuidStr (e.header.guid) + " is a straight " + ElemTypeName (e.header.type) +
					  "; radial dimensions need a curved wall/beam (arcAngle != 0), an Arc or a Circle.");
			g.center = ArcCenter (rl.begin, rl.end, rl.arcAngle);
			g.radius = Dist (rl.begin, g.center);
			g.middle = C (Add (V (g.center), Rotate (Sub (V (rl.begin), V (g.center)), rl.arcAngle / 2.0)));
			const double band = e.header.type.typeID == API_WallID ? rl.thickness + std::fabs (e.wall.offset) : 1.0;
			g.minRadius = std::max (0.0, g.radius - band);
			g.maxRadius = g.radius + band;
			break;
		}
		default:
			Fail ("Radial dimensions need an Arc, a Circle, a curved Wall or a curved Beam; element " + GuidStr (e.header.guid) +
				  " is a " + ElemTypeName (e.header.type) + ".");
	}
	if (g.radius <= 1e-9)
		Fail ("Element " + GuidStr (e.header.guid) + " has a zero radius.");
	return g;
}


API_Coord PointOnArc (const ArcGeometry& g, const API_Coord& near)
{
	const Vec v = Sub (V (near), V (g.center));
	if (Len (v) < 1e-9)
		Fail ("'at' must not be the arc center: give a point near the arc where the dimension should touch it.");
	return C (Add (V (g.center), Mul (Unit (v), g.radius)));
}


struct BaseCandidate {
	bool	line;
	Int32	inIndex;
	bool	special;
};


GS::Array<BaseCandidate> RadialCandidates (API_ElemTypeID typeID)
{
	GS::Array<BaseCandidate> c;
	switch (typeID) {
		case API_WallID:
			c.Push ({ true, 11, false }); c.Push ({ true, 21, false }); c.Push ({ true, 1, false });
			c.Push ({ true, 2, false }); c.Push ({ true, 0, false });
			break;
		case API_BeamID:
			c.Push ({ true, 1, false }); c.Push ({ true, 0, false }); c.Push ({ true, 11, false });
			break;
		default:	// Arc / Circle
			c.Push ({ true, 1, false }); c.Push ({ true, 0, false }); c.Push ({ false, 1, false }); c.Push ({ false, 0, false });
			break;
	}
	return c;
}


void ApplyRadialStyle (API_Element& element, API_Element* mask, const OS& spec, StringPool& pool)
{
	API_RadialDimensionType& r = element.radialDimension;
#define RAD_SET(f) if (mask) ACAPI_ELEMENT_MASK_SET (*mask, API_RadialDimensionType, f)
	if (auto p = OptPen (spec, "linePen"))				{ r.linPen = *p; RAD_SET (linPen); }
	if (Has (spec, "textPosition"))					{ r.textPos = (API_TextPosID) ParseNamed (kTextPos, spec, "textPosition"); RAD_SET (textPos); }
	if (Has (spec, "textDirection"))					{ r.textWay = (API_DirID) ParseNamed (kTextWay, spec, "textDirection"); RAD_SET (textWay); }
	if (auto b = OptBool (spec, "showCenter"))			{ r.showOrigo = *b; RAD_SET (showOrigo); }
	if (auto b = OptBool (spec, "onlyDimensionText"))	{ r.onlyDimensionText = *b; RAD_SET (onlyDimensionText); }
	if (auto c = OptCoord (spec, "lineEnd"))			{ r.endC = *c; RAD_SET (endC); RAD_SET (endC.x); RAD_SET (endC.y); }
	if (spec.Contains ("prefix")) {
		const GS::UniString prefix = GetString (spec, "prefix");
		std::memset (r.prefix, 0, sizeof (r.prefix));
		GS::String ascii = ToStr (prefix);
		bool isAscii = true;
		for (UIndex i = 0; i < prefix.GetLength (); ++i)
			isAscii = isAscii && (UInt32) (GS::UniChar::Layout) prefix[i] <= 127;
		if (isAscii)
			std::strncpy (r.prefix, ascii.ToCStr (), sizeof (r.prefix) - 1);
		r.prefixUStr = pool.Add (prefix);
		RAD_SET (prefix); RAD_SET (prefixUStr);
	}
#undef RAD_SET
	ApplyMarkerFields (r.markerData, MaskOf (element, mask, r.markerData), spec);
	ApplyNoteFields (r.note, MaskOf (element, mask, r.note), spec, pool, "text");
}


API_Guid CreateRadialDimension (const OS& spec)
{
	const API_Guid target = GetGuid (spec, "element");
	GS::Array<API_Guid> targets;
	targets.Push (target);
	PlanDatabaseScope planScope (NeedsFloorPlan (targets));		// walls/beams live on the floor plan
	const API_Element arcElem = GetElement (target);
	const ArcGeometry g = GetArcGeometry (arcElem);
	const API_Coord arcPoint = Has (spec, "at") ? PointOnArc (g, GetCoord (spec, "at")) : g.middle;

	API_Element element = NewElement (API_RadialDimensionID);
	GetDefaults (element, nullptr);
	ApplyCommonFields (element, nullptr, spec);
	element.radialDimension.endC = arcPoint;
	StringPool pool;
	ApplyRadialStyle (element, nullptr, spec, pool);

	GS::Array<BaseCandidate> candidates;
	const bool explicitRef = spec.Contains ("inIndex");
	if (explicitRef)
		candidates.Push ({ GetBool (spec, "line", true), GetInt (spec, "inIndex"), GetBool (spec, "special", false) });
	else
		candidates = RadialCandidates (arcElem.header.type.typeID);

	GSErrCode lastErr = NoError;
	Int32 zeroIndex = -1;			// first candidate Archicad accepted but whose radius read back as 0 (unverifiable)
	bool sawWrongRadius = false;
	for (UIndex i = 0; i < candidates.GetSize (); ++i) {
		API_Element attempt = element;
		API_DimBase& base = attempt.radialDimension.base;
		BNZeroMemory (&base, sizeof (base));
		base.base.type = arcElem.header.type;
		base.base.guid = target;
		base.base.line = candidates[i].line;
		base.base.inIndex = candidates[i].inIndex;
		base.base.special = candidates[i].special ? 1 : 0;
		base.loc = arcPoint;

		const GSErrCode err = ACAPI_Element_Create (&attempt, nullptr);
		if (err != NoError) {
			lastErr = err;
			if (err == APIERR_NEEDSUNDOSCOPE)
				break;
			continue;
		}
		if (explicitRef)
			return attempt.header.guid;
		API_Element check = NewElement (API_RadialDimensionID);
		check.header.guid = attempt.header.guid;
		if (ACAPI_Element_Get (&check) != NoError)
			return attempt.header.guid;
		const double measured = std::fabs (check.radialDimension.dimVal);
		if (measured >= g.minRadius - kExactTolerance && measured <= g.maxRadius + kExactTolerance && measured > 1e-9)
			return attempt.header.guid;
		if (measured <= 1e-9) {
			if (zeroIndex < 0)
				zeroIndex = (Int32) i;
		} else {
			sawWrongRadius = true;
		}
		DeleteCreated (attempt.header.guid);
	}
	if (zeroIndex >= 0 && !sawWrongRadius) {
		API_Element attempt = element;
		API_DimBase& base = attempt.radialDimension.base;
		BNZeroMemory (&base, sizeof (base));
		base.base.type = arcElem.header.type;
		base.base.guid = target;
		base.base.line = candidates[zeroIndex].line;
		base.base.inIndex = candidates[zeroIndex].inIndex;
		base.base.special = candidates[zeroIndex].special ? 1 : 0;
		base.loc = arcPoint;
		Check (ACAPI_Element_Create (&attempt, nullptr), "Cannot create the radial dimension");
		return attempt.header.guid;
	}
	Fail ("Cannot attach a radial dimension to " + ElemTypeName (arcElem.header.type) + " " + GuidStr (target) +
		  (lastErr != NoError ? " (last error: " + ErrorName (lastErr) + ")" : GS::UniString (" (the measured radius did not match)")) +
		  ". Check that the element is visible on the current story of the Floor Plan, or pass an explicit reference "
		  "(inIndex/line) taken from an existing radial dimension's details.", lastErr != NoError ? lastErr : APIERR_GENERAL);
}


void SerializeRadial (const API_Element& element, OS& out)
{
	const API_RadialDimensionType& r = element.radialDimension;
	const bool instance = element.header.guid != APINULLGuid;		// false: tool defaults (get_tool_defaults)
	if (instance) {
		AddBaseJson (out, r.base.base);
		out.Add ("arcPoint", CoordObj (r.base.loc));
		out.Add ("lineEnd", CoordObj (r.endC));
		out.Add ("radius", r.dimVal);
	}
	out.Add ("showCenter", r.showOrigo);
	out.Add ("onlyDimensionText", r.onlyDimensionText);
	out.Add ("textPosition", NameOf (kTextPos, r.textPos));
	out.Add ("textDirection", NameOf (kTextWay, r.textWay));
	out.Add ("linePen", (Int32) r.linPen);
	AddMarkerJson (out, r.markerData);
	ElementStrings strings;
	if (instance)
		strings = ReadElementStrings (element.header.guid, API_RadialDimensionID);
	GS::UniString prefix = strings.second;
	if (prefix.IsEmpty ()) {
		char buffer[sizeof (r.prefix) + 1];
		std::memcpy (buffer, r.prefix, sizeof (r.prefix));
		buffer[sizeof (r.prefix)] = '\0';
		prefix = GS::UniString (buffer, CC_UTF8);
	}
	out.Add ("prefix", prefix);
	AddNoteJson (out, r.note, &strings.first, "text");
}


void ModifyRadial (API_Element& element, API_Element& mask, API_ElementMemo&, UInt64&, const OS& patch)
{
	StringPool& pool = PendingModifyStrings ();
	pool.Clear ();
	ApplyRadialStyle (element, &mask, patch, pool);
	if (Has (patch, "at")) {
		API_RadialDimensionType& r = element.radialDimension;
		if (r.base.base.guid == APINULLGuid)
			Fail ("This radial dimension has no base element; 'at' cannot be applied.");
		const ArcGeometry g = GetArcGeometry (GetElement (r.base.base.guid));
		r.base.loc = PointOnArc (g, GetCoord (patch, "at"));
		ACAPI_ELEMENT_MASK_SET (mask, API_RadialDimensionType, base);
		ACAPI_ELEMENT_MASK_SET (mask, API_RadialDimensionType, base.loc);
		ACAPI_ELEMENT_MASK_SET (mask, API_RadialDimensionType, base.loc.x);
		ACAPI_ELEMENT_MASK_SET (mask, API_RadialDimensionType, base.loc.y);
	}
}

// ============================================================================
// Angle dimension
// ============================================================================

struct AngleGeometry {
	API_Coord	p[4];		// line1 begin/end, line2 begin/end
	API_Coord	origo;
	Vec			ray1, ray2;	// unit rays from origo towards the far end of each line
};


AngleGeometry ComputeAngleGeometry (const API_Coord (&p)[4])
{
	AngleGeometry g;
	for (int k = 0; k < 4; ++k)
		g.p[k] = p[k];
	if (Dist (p[0], p[1]) < 1e-9 || Dist (p[2], p[3]) < 1e-9)
		Fail ("Each line of an angle dimension needs two distinct points (begin != end).");
	const Vec d1 = Unit (Sub (V (p[1]), V (p[0])));
	const Vec d2 = Unit (Sub (V (p[3]), V (p[2])));
	const double cr = Cross (d1, d2);
	if (std::fabs (cr) < 1e-9)
		Fail ("The two lines are parallel; an angle dimension needs two non-parallel lines.");
	const double s = Cross (Sub (V (p[2]), V (p[0])), d2) / cr;
	g.origo = C (Add (V (p[0]), Mul (d1, s)));
	auto far = [&] (const API_Coord& a, const API_Coord& b) { return Dist (a, g.origo) >= Dist (b, g.origo) ? a : b; };
	g.ray1 = Unit (Sub (V (far (p[0], p[1])), V (g.origo)));
	g.ray2 = Unit (Sub (V (far (p[2], p[3])), V (g.origo)));
	return g;
}


API_Coord DefaultArcPoint (const AngleGeometry& g, double radius)
{
	Vec bis = Add (g.ray1, g.ray2);
	if (Len (bis) < 1e-9)
		bis = LeftNormal (g.ray1);
	return C (Add (V (g.origo), Mul (Unit (bis), radius)));
}


void ReadLine (const OS& spec, const char* key, API_Coord& begin, API_Coord& end)
{
	OS line;
	if (!TryGetObject (spec, key, line))
		Fail ("AngleDimension requires 'line1' and 'line2' ({begin: {x,y}, end: {x,y}}) or 'elements': [guid, guid].");
	begin = GetCoord (line, "begin");
	end = GetCoord (line, "end");
}


void ApplyAngleStyle (API_Element& element, API_Element* mask, const OS& spec, StringPool& pool)
{
	API_AngleDimensionType& a = element.angleDimension;
#define ANG_SET(f) if (mask) ACAPI_ELEMENT_MASK_SET (*mask, API_AngleDimensionType, f)
	if (auto p = OptPen (spec, "linePen"))				{ a.linPen = *p; ANG_SET (linPen); }
	if (Has (spec, "textPosition"))					{ a.textPos = (API_TextPosID) ParseNamed (kTextPos, spec, "textPosition"); ANG_SET (textPos); }
	if (Has (spec, "textDirection"))					{ a.textWay = (API_DirID) ParseNamed (kTextWay, spec, "textDirection"); ANG_SET (textWay); }
	if (Has (spec, "witnessForm"))						{ a.witnessForm = (API_WitnessID) ParseNamed (kWitnessForms, spec, "witnessForm"); ANG_SET (witnessForm); }
	if (auto v = OptDouble (spec, "witnessVal"))		{ a.witnessVal = *v; ANG_SET (witnessVal); }
	if (auto b = OptBool (spec, "smallArc"))			{ a.smallArc = *b; ANG_SET (smallArc); }
	if (auto b = OptBool (spec, "onlyDimensionText"))	{ a.onlyDimensionText = *b; ANG_SET (onlyDimensionText); }
#undef ANG_SET
	ApplyMarkerFields (a.markerData, MaskOf (element, mask, a.markerData), spec);
	ApplyNoteFields (a.note, MaskOf (element, mask, a.note), spec, pool, "text");
}


// Arc placement from arcPoint / radius (mask may be null). `place` forces a placement even when
// neither field is given (creation, or new lines on modify), using defaultRadius.
void ApplyAngleArc (API_AngleDimensionType& a, API_Element* mask, const AngleGeometry& g, const OS& spec, bool place,
					double defaultRadius)
{
	std::optional<API_Coord> arcPoint = OptCoord (spec, "arcPoint");
	std::optional<double> radius = OptDouble (spec, "radius");
	if (radius.has_value () && *radius <= 0.0)
		Fail ("radius must be > 0 (meters).");
	if (!place && !arcPoint.has_value () && !radius.has_value ())
		return;
	const bool creating = place;
	API_Coord pos;
	if (arcPoint.has_value ()) {
		pos = *arcPoint;
		if (radius.has_value ()) {
			const Vec v = Sub (V (pos), V (g.origo));
			pos = C (Add (V (g.origo), Mul (Len (v) > 1e-9 ? Unit (v) : Unit (Add (g.ray1, g.ray2)), *radius)));
		}
	} else if (!creating && radius.has_value ()) {
		const Vec v = Sub (V (a.pos), V (g.origo));
		pos = Len (v) > 1e-9 ? C (Add (V (g.origo), Mul (Unit (v), *radius))) : DefaultArcPoint (g, *radius);
	} else {
		pos = DefaultArcPoint (g, radius.value_or (defaultRadius));
	}
	if (Dist (pos, g.origo) < 1e-6)
		Fail ("arcPoint must not coincide with the intersection of the two lines.");
	a.pos = pos;
	a.origo = g.origo;
	a.radius = Dist (pos, g.origo);
	if (mask != nullptr) {
		ACAPI_ELEMENT_MASK_SET (*mask, API_AngleDimensionType, pos);
		ACAPI_ELEMENT_MASK_SET (*mask, API_AngleDimensionType, pos.x);
		ACAPI_ELEMENT_MASK_SET (*mask, API_AngleDimensionType, pos.y);
		ACAPI_ELEMENT_MASK_SET (*mask, API_AngleDimensionType, origo);
		ACAPI_ELEMENT_MASK_SET (*mask, API_AngleDimensionType, origo.x);
		ACAPI_ELEMENT_MASK_SET (*mask, API_AngleDimensionType, origo.y);
		ACAPI_ELEMENT_MASK_SET (*mask, API_AngleDimensionType, radius);
	}
}


bool AngleMatches (double measured, const AngleGeometry& g)
{
	const double c = std::max (-1.0, std::min (1.0, Dot (g.ray1, g.ray2)));
	const double theta = std::acos (c);
	const double m = std::fabs (measured);
	for (double expected : { theta, kPi - theta, kPi + theta, 2.0 * kPi - theta }) {
		if (std::fabs (m - expected) <= 1e-3)
			return true;
	}
	return false;
}


API_Guid CreateAngleDimension (const OS& spec)
{
	API_Coord p[4];
	API_Guid refs[2] = { APINULLGuid, APINULLGuid };
	const GS::Array<API_Guid> guids = GetGuidArray (spec, "elements", false);
	PlanDatabaseScope planScope (NeedsFloorPlan (guids));		// walls/beams live on the floor plan
	if (spec.Contains ("elements")) {
		if (guids.GetSize () != 2)
			Fail ("'elements' must contain exactly two straight walls / beams / lines.");
		for (int k = 0; k < 2; ++k) {
			const RefLine rl = GetRefLine (GetElement (guids[k]));
			if (std::fabs (rl.arcAngle) > 1e-9)
				Fail ("Element " + GuidStr (guids[k]) + " is curved; angle dimensions need straight lines (use line1/line2 for static lines).");
			p[2 * k] = rl.begin;
			p[2 * k + 1] = rl.end;
			refs[k] = guids[k];
		}
	} else {
		ReadLine (spec, "line1", p[0], p[1]);
		ReadLine (spec, "line2", p[2], p[3]);
	}
	const AngleGeometry g = ComputeAngleGeometry (p);

	API_Element element = NewElement (API_AngleDimensionID);
	{
		Memo defaults;
		GetDefaults (element, defaults.Ptr ());
	}
	ApplyCommonFields (element, nullptr, spec);
	API_AngleDimensionType& a = element.angleDimension;
	a.smallArc = true;
	StringPool pool;
	ApplyAngleStyle (element, nullptr, spec, pool);
	ApplyAngleArc (a, nullptr, g, spec, true, 1.0);
	for (int k = 0; k < 4; ++k) {
		BNZeroMemory (&a.base[k], sizeof (a.base[k]));
		a.base[k].loc = p[k];
	}

	bool anyAssoc = false;
	if (GetBool (spec, "associative", true)) {
		for (int k = 0; k < 2; ++k) {
			if (refs[k] == APINULLGuid)
				continue;
			const GS::Array<Anchor> anchors = CollectAnchors (refs[k]);
			for (int j = 0; j < 2; ++j) {
				API_DimBase& b = a.base[2 * k + j];
				if (const Anchor* an = NearestAnchor (anchors, b.loc, kExactTolerance)) {
					b.base = BaseFromNeig (an->neig);
					anyAssoc = true;
				}
			}
		}
	}

	API_Element attempt = element;
	GSErrCode err = ACAPI_Element_Create (&attempt, nullptr);
	if (err == NoError && anyAssoc) {
		API_Element check = NewElement (API_AngleDimensionID);
		check.header.guid = attempt.header.guid;
		if (ACAPI_Element_Get (&check) == NoError && std::fabs (check.angleDimension.dimVal) > 1e-9 &&
			!AngleMatches (check.angleDimension.dimVal, g)) {
			DeleteCreated (attempt.header.guid);
			err = APIERR_GENERAL;		// fall back to static below
		}
	}
	if (err != NoError && anyAssoc && err != APIERR_NEEDSUNDOSCOPE) {
		attempt = element;
		for (int k = 0; k < 4; ++k)
			BNZeroMemory (&attempt.angleDimension.base[k].base, sizeof (attempt.angleDimension.base[k].base));
		err = ACAPI_Element_Create (&attempt, nullptr);
	}
	Check (err, "Cannot create the angle dimension");
	return attempt.header.guid;
}


void SerializeAngle (const API_Element& element, OS& out)
{
	const API_AngleDimensionType& a = element.angleDimension;
	const bool instance = element.header.guid != APINULLGuid;		// false: tool defaults (get_tool_defaults)
	if (instance) {
		out.Add ("line1", OS ("begin", CoordObj (a.base[0].loc), "end", CoordObj (a.base[1].loc)));
		out.Add ("line2", OS ("begin", CoordObj (a.base[2].loc), "end", CoordObj (a.base[3].loc)));
		out.Add ("center", CoordObj (a.origo));
		out.Add ("arcPoint", CoordObj (a.pos));
		out.Add ("radius", a.radius);
		AddAngle (out, "angle", a.dimVal);
	}
	out.Add ("smallArc", a.smallArc);
	if (instance) {
		GS::Array<OS> references;
		bool anyAssoc = false;
		for (int k = 0; k < 4; ++k) {
			OS r;
			AddBaseJson (r, a.base[k].base);
			anyAssoc = anyAssoc || IsAssociativeBase (a.base[k].base);
			references.Push (r);
		}
		out.Add ("associative", anyAssoc);
		out.Add ("references", references);
	}
	out.Add ("textPosition", NameOf (kTextPos, a.textPos));
	out.Add ("textDirection", NameOf (kTextWay, a.textWay));
	out.Add ("witnessForm", NameOf (kWitnessForms, a.witnessForm));
	out.Add ("witnessVal", a.witnessVal);
	out.Add ("onlyDimensionText", a.onlyDimensionText);
	out.Add ("linePen", (Int32) a.linPen);
	AddMarkerJson (out, a.markerData);
	ElementStrings strings;
	if (instance && a.note.contentType == API_NoteContent_Custom)
		strings = ReadElementStrings (element.header.guid, API_AngleDimensionID);
	AddNoteJson (out, a.note, &strings.first, "text");
}


void ModifyAngle (API_Element& element, API_Element& mask, API_ElementMemo&, UInt64&, const OS& patch)
{
	StringPool& pool = PendingModifyStrings ();
	pool.Clear ();
	ApplyAngleStyle (element, &mask, patch, pool);
	API_AngleDimensionType& a = element.angleDimension;

	const bool newLines = patch.Contains ("line1") || patch.Contains ("line2");
	if (!newLines && !Has (patch, "arcPoint") && !Has (patch, "radius"))
		return;		// style only: the geometry is left alone
	if (element.header.guid == APINULLGuid)
		Fail ("line1 / line2 / arcPoint / radius are per-dimension geometry and cannot be stored in the Angle Dimension tool defaults.");

	API_Coord p[4];
	for (int k = 0; k < 4; ++k)
		p[k] = a.base[k].loc;
	if (patch.Contains ("line1"))
		ReadLine (patch, "line1", p[0], p[1]);
	if (patch.Contains ("line2"))
		ReadLine (patch, "line2", p[2], p[3]);
	const AngleGeometry g = ComputeAngleGeometry (p);
	if (newLines) {
		// New static lines: the references to elements are dropped.
		for (int k = 0; k < 4; ++k) {
			BNZeroMemory (&a.base[k], sizeof (a.base[k]));
			a.base[k].loc = p[k];
		}
		ACAPI_ELEMENT_MASK_SET (mask, API_AngleDimensionType, base);
		for (int k = 0; k < 4; ++k) {
			ACAPI_ELEMENT_MASK_SET (mask, API_AngleDimensionType, base[k]);
			ACAPI_ELEMENT_MASK_SET (mask, API_AngleDimensionType, base[k].base);
			ACAPI_ELEMENT_MASK_SET (mask, API_AngleDimensionType, base[k].loc);
			ACAPI_ELEMENT_MASK_SET (mask, API_AngleDimensionType, base[k].loc.x);
			ACAPI_ELEMENT_MASK_SET (mask, API_AngleDimensionType, base[k].loc.y);
		}
		ApplyAngleArc (a, &mask, g, patch, true, a.radius > 1e-6 ? a.radius : 1.0);
	} else {
		ApplyAngleArc (a, &mask, g, patch, false, 1.0);
	}
}

} // namespace


void RegisterSpecialDimensionAdapters ()
{
	RegisterAdapter ({ API_LevelDimensionID, CreateLevelDimension, SerializeLevel, ModifyLevel });
	RegisterAdapter ({ API_RadialDimensionID, CreateRadialDimension, SerializeRadial, ModifyRadial });
	RegisterAdapter ({ API_AngleDimensionID, CreateAngleDimension, SerializeAngle, ModifyAngle });
}

} // namespace dimension
} // namespace cc
