// *****************************************************************************
// DimensionsCommon.cpp — shared helpers of the dimensions family (see the header).
// *****************************************************************************

#include "Commands/DimensionsCommon.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cc {
namespace dimension {

// --- Scalars ---------------------------------------------------------------------

std::optional<short> OptPen (const OS& spec, const char* key)
{
	auto v = OptInt (spec, key);
	if (!v.has_value ())
		return std::nullopt;
	if (*v < 1 || *v > 255)
		Fail ("Field '" + GS::UniString (key) + "' must be a pen index 1-255 (list pens with the attribute tools).");
	return (short) *v;
}

// --- Strings -----------------------------------------------------------------------

GS::UniString* StringPool::Add (const GS::UniString& s)
{
	strings.push_back (std::make_unique<GS::UniString> (s));
	return strings.back ().get ();
}


StringPool& PendingModifyStrings ()
{
	// Intentionally never destroyed: no GS::UniString destructor may run during add-on unload / static teardown.
	static StringPool* pool = new StringPool ();
	return *pool;
}

namespace {

bool IsAscii (const GS::UniString& s)
{
	for (UIndex i = 0; i < s.GetLength (); ++i) {
		if ((UInt32) (GS::UniChar::Layout) s[i] > 127)
			return false;
	}
	return true;
}


// Copies an ASCII-only string into a fixed char buffer (the Unicode pointer carries the real value).
void CopyAsciiField (char* dst, size_t capacity, const GS::UniString& s)
{
	std::memset (dst, 0, capacity);
	if (!IsAscii (s))
		return;
	GS::String str = ToStr (s);
	std::strncpy (dst, str.ToCStr (), capacity - 1);
}

} // namespace

// --- Notes ---------------------------------------------------------------------------

bool HasNoteStyleFields (const OS& spec)
{
	static const char* const keys[] = { "textPen", "textSize", "textFont", "textBold", "textItalic", "textUnderline", "textFrame", "textOpaque" };
	for (const char* k : keys) {
		if (spec.Contains (k))
			return true;
	}
	return false;
}


bool ApplyNoteFields (API_NoteType& note, API_NoteType* m, const OS& spec, StringPool& pool, const char* textKey)
{
#define NOTE_SET(f) if (m) ACAPI_ELEMENT_MASK_SET (*m, API_NoteType, f)
	bool changed = false;
	if (auto p = OptPen (spec, "textPen"))				{ note.notePen = *p; NOTE_SET (notePen); changed = true; }
	if (auto v = OptDouble (spec, "textSize")) {
		if (*v <= 0.0)
			Fail ("textSize must be a positive character height in paper millimeters (e.g. 2.5).");
		note.noteSize = *v; NOTE_SET (noteSize); changed = true;
	}
	if (auto f = OptAttr (API_FontID, spec, "textFont"))	{ note.noteFont = (short) *f; NOTE_SET (noteFont); changed = true; }
	auto face = [&] (const char* key, unsigned short bit) {
		if (auto b = OptBool (spec, key)) {
			if (*b) note.faceBits = (unsigned short) (note.faceBits | bit);
			else    note.faceBits = (unsigned short) (note.faceBits & ~bit);
			NOTE_SET (faceBits);
			changed = true;
		}
	};
	face ("textBold", APIFace_Bold);
	face ("textItalic", APIFace_Italic);
	face ("textUnderline", APIFace_Underline);
	if (auto b = OptBool (spec, "textFrame"))			{ note.frame = *b; NOTE_SET (frame); changed = true; }
	if (auto b = OptBool (spec, "textOpaque"))			{ note.opaque = *b; NOTE_SET (opaque); changed = true; }

	if (textKey != nullptr && ApplyNoteText (note, m, spec, textKey, pool))
		changed = true;
#undef NOTE_SET
	return changed;
}


void SetNoteCustomText (API_NoteType& note, GS::UniString* text)
{
	if (text == nullptr || text->IsEmpty ()) {
		note.contentType = API_NoteContent_Measured;
		note.contentUStr = nullptr;
		std::memset (note.content, 0, sizeof (note.content));
	} else {
		note.contentType = API_NoteContent_Custom;
		note.contentUStr = text;
		CopyAsciiField (note.content, sizeof (note.content), *text);
	}
}


bool ApplyNoteText (API_NoteType& note, API_NoteType* m, const OS& spec, const char* textKey, StringPool& pool)
{
#define NOTE_SET(f) if (m) ACAPI_ELEMENT_MASK_SET (*m, API_NoteType, f)
	if (!spec.Contains (textKey))
		return false;
	GS::UniString text;
	if (spec.IsBool (textKey)) {
		if (GetBool (spec, textKey))
			Fail ("Field '" + GS::UniString (textKey) + "' must be a custom text string, or \"\"/false to show the measured value.");
	} else {
		text = GetString (spec, textKey);
	}
	SetNoteCustomText (note, text.IsEmpty () ? nullptr : pool.Add (text));
	NOTE_SET (contentType);
	NOTE_SET (content);
	NOTE_SET (contentUStr);
#undef NOTE_SET
	return true;
}


void CopyNoteStyle (const API_NoteType& from, API_NoteType& to)
{
	to.notePen = from.notePen;
	to.noteFont = from.noteFont;
	to.noteSize = from.noteSize;
	to.faceBits = from.faceBits;
	to.frame = from.frame;
	to.opaque = from.opaque;
	to.backgroundPen = from.backgroundPen;
	to.framePen = from.framePen;
}


GS::UniString NoteCustomText (const API_NoteType& note, const GS::UniString* ustr)
{
	if (note.contentType != API_NoteContent_Custom)
		return GS::UniString ();
	if (ustr != nullptr && !ustr->IsEmpty ())
		return *ustr;
	if (note.contentUStr != nullptr && !note.contentUStr->IsEmpty ())
		return *note.contentUStr;
	char buffer[sizeof (note.content) + 1];
	std::memcpy (buffer, note.content, sizeof (note.content));
	buffer[sizeof (note.content)] = '\0';
	return GS::UniString (buffer, CC_UTF8);
}


void AddNoteJson (OS& out, const API_NoteType& note, const GS::UniString* customText, const char* textKey)
{
	out.Add ("textPen", (Int32) note.notePen);
	out.Add ("textSize", note.noteSize);
	out.Add ("textFont", AttrRef (API_FontID, note.noteFont));
	out.Add ("textBold", (note.faceBits & APIFace_Bold) != 0);
	out.Add ("textItalic", (note.faceBits & APIFace_Italic) != 0);
	out.Add ("textUnderline", (note.faceBits & APIFace_Underline) != 0);
	out.Add ("textFrame", note.frame);
	out.Add ("textOpaque", note.opaque);
	if (textKey != nullptr && note.contentType == API_NoteContent_Custom)
		out.Add (textKey, NoteCustomText (note, customText));
}

// --- Markers ----------------------------------------------------------------------------

bool ApplyMarkerFields (API_MarkerData& marker, API_MarkerData* m, const OS& spec)
{
#define MARK_SET(f) if (m) ACAPI_ELEMENT_MASK_SET (*m, API_MarkerData, f)
	bool changed = false;
	if (Has (spec, "markerType")) {
		marker.markerType = (API_MarkerID) ParseNamed (kMarkers, spec, "markerType");
		MARK_SET (markerType); changed = true;
	}
	if (auto p = OptPen (spec, "markerPen"))		{ marker.markerPen = *p; MARK_SET (markerPen); changed = true; }
	if (auto v = OptDouble (spec, "markerSize")) {
		if (*v <= 0.0)
			Fail ("markerSize must be positive (paper millimeters, e.g. 2).");
		marker.markerSize = *v; MARK_SET (markerSize); changed = true;
	}
#undef MARK_SET
	return changed;
}


void AddMarkerJson (OS& out, const API_MarkerData& marker)
{
	out.Add ("markerType", NameOf (kMarkers, marker.markerType));
	out.Add ("markerPen", (Int32) marker.markerPen);
	out.Add ("markerSize", marker.markerSize);
}

// --- Geometry ------------------------------------------------------------------------------

double Len (const Vec& a)
{
	return std::hypot (a.x, a.y);
}


Vec Unit (const Vec& a)
{
	const double l = Len (a);
	if (l < 1e-12)
		Fail ("Cannot derive a direction from two coincident points; give an explicit 'direction'.");
	return { a.x / l, a.y / l };
}


Vec Rotate (const Vec& a, double radians)
{
	const double c = std::cos (radians), s = std::sin (radians);
	return { a.x * c - a.y * s, a.x * s + a.y * c };
}


double Dist (const API_Coord& a, const API_Coord& b)
{
	return std::hypot (a.x - b.x, a.y - b.y);
}


API_Coord ProjectOnLine (const API_Coord& p, const API_Coord& origin, const Vec& dir)
{
	const Vec o = V (origin);
	const double t = Dot (Sub (V (p), o), dir);
	return C (Add (o, Mul (dir, t)));
}


API_Coord ArcCenter (const API_Coord& begin, const API_Coord& end, double arcAngle)
{
	const Vec b = V (begin), e = V (end);
	const Vec chord = Sub (e, b);
	const double l = Len (chord);
	if (l < 1e-12 || std::fabs (arcAngle) < 1e-12)
		Fail ("Cannot compute the center of a degenerate arc.");
	const Vec mid = Mul (Add (b, e), 0.5);
	const Vec n = LeftNormal (Mul (chord, 1.0 / l));
	return C (Add (mid, Mul (n, l / (2.0 * std::tan (arcAngle / 2.0)))));
}


RefLine GetRefLine (const API_Element& element)
{
	RefLine rl;
	switch (element.header.type.typeID) {
		case API_WallID:
			rl.begin = element.wall.begC;
			rl.end = element.wall.endC;
			rl.arcAngle = element.wall.angle;
			rl.thickness = std::max (element.wall.thickness, element.wall.type == APIWtyp_Trapez ? element.wall.thickness1 : 0.0);
			rl.isWall = true;
			return rl;
		case API_BeamID:
			rl.begin = element.beam.begC;
			rl.end = element.beam.endC;
			rl.arcAngle = element.beam.curveAngle;
			return rl;
		case API_LineID:
			rl.begin = element.line.begC;
			rl.end = element.line.endC;
			return rl;
		default:
			Fail ("Element " + GuidStr (element.header.guid) + " is a " + ElemTypeName (element.header.type) +
				  "; 'at': 'begin'|'end' and wall/line references work with Wall, Beam and Line elements only. "
				  "Give 'x','y' near the point to link instead (see get_dimension_anchors).");
	}
}

// --- Neigs ---------------------------------------------------------------------------------

bool IsLineNeig (API_NeigID id)
{
	switch (id) {
		case APINeig_WallOn: case APINeig_WallPlOn: case APINeig_BeamOn: case APINeig_CeilOn:
		case APINeig_RoofOn: case APINeig_MeshOn: case APINeig_HatchOn: case APINeig_LineOn:
		case APINeig_ArcOn: case APINeig_SplineOn:
			return true;
		default:
			return false;
	}
}


bool IsSpecialNeig (API_NeigID id)
{
	switch (id) {
		case APINeig_WallPl: case APINeig_WallPlOn: case APINeig_BeamHole: case APINeig_WindHole:
		case APINeig_DoorHole: case APINeig_MeshRidge: case APINeig_MeshRidgeOn:
			return true;
		default:
			return false;
	}
}


namespace {

struct NeigNameEntry { API_NeigID id; const char* name; };

const NeigNameEntry kNeigNames[] = {
	{ APINeig_Wall, "Wall" }, { APINeig_WallPl, "WallPl" }, { APINeig_WallOn, "WallOn" }, { APINeig_WallPlOn, "WallPlOn" },
	{ APINeig_Colu, "Colu" }, { APINeig_Beam, "Beam" }, { APINeig_BeamOn, "BeamOn" }, { APINeig_BeamHole, "BeamHole" },
	{ APINeig_Wind, "Wind" }, { APINeig_WindHole, "WindHole" }, { APINeig_Door, "Door" }, { APINeig_DoorHole, "DoorHole" },
	{ APINeig_Symb, "Symb" }, { APINeig_Light, "Light" }, { APINeig_Ceil, "Ceil" }, { APINeig_CeilOn, "CeilOn" },
	{ APINeig_Roof, "Roof" }, { APINeig_RoofOn, "RoofOn" }, { APINeig_RBL, "RBL" }, { APINeig_RBLOn, "RBLOn" },
	{ APINeig_Mesh, "Mesh" }, { APINeig_MeshOn, "MeshOn" }, { APINeig_MeshRidge, "MeshRidge" }, { APINeig_MeshRidgeOn, "MeshRidgeOn" },
	{ APINeig_DimInt, "DimInt" }, { APINeig_DimPt, "DimPt" }, { APINeig_DimOn, "DimOn" }, { APINeig_RadDim, "RadDim" },
	{ APINeig_LevDim, "LevDim" }, { APINeig_AngDim, "AngDim" }, { APINeig_AngDimOn, "AngDimOn" }, { APINeig_Word, "Word" },
	{ APINeig_Label, "Label" }, { APINeig_Room, "Room" }, { APINeig_RoomStamp, "RoomStamp" }, { APINeig_RoomOn, "RoomOn" },
	{ APINeig_RoomRefPt, "RoomRefPt" }, { APINeig_Hatch, "Hatch" }, { APINeig_HatchOn, "HatchOn" }, { APINeig_Line, "Line" },
	{ APINeig_LineOn, "LineOn" }, { APINeig_PolyLine, "PolyLine" }, { APINeig_PolyLineOn, "PolyLineOn" }, { APINeig_Arc, "Arc" },
	{ APINeig_ArcOn, "ArcOn" }, { APINeig_Circ, "Circ" }, { APINeig_CircOn, "CircOn" }, { APINeig_Spline, "Spline" },
	{ APINeig_SplineOn, "SplineOn" }, { APINeig_Hot, "Hot" }, { APINeig_Symb3D, "Symb3D" }, { APINeig_Light3D, "Light3D" },
	{ APINeig_Wind3D, "Wind3D" }, { APINeig_Door3D, "Door3D" }, { APINeig_WindMark, "WindMark" }, { APINeig_DoorMark, "DoorMark" },
	{ APINeig_WallPlClOff, "WallPlClOff" }, { APINeig_WallPlOnClOff, "WallPlOnClOff" }, { APINeig_SymbOn, "SymbOn" },
	{ APINeig_WindOn, "WindOn" }, { APINeig_DoorOn, "DoorOn" }, { APINeig_LightOn, "LightOn" }, { APINeig_Note, "Note" },
	{ APINeig_CurtainWall, "CurtainWall" }, { APINeig_CurtainWallOn, "CurtainWallOn" }, { APINeig_CWFrame, "CWFrame" },
	{ APINeig_CWPanel, "CWPanel" }, { APINeig_Shell, "Shell" }, { APINeig_ShellOn, "ShellOn" }, { APINeig_Skylight, "Skylight" },
	{ APINeig_SkylightHole, "SkylightHole" }, { APINeig_Morph, "Morph" }, { APINeig_MorphOn, "MorphOn" },
	{ APINeig_ColuOn, "ColuOn" }, { APINeig_Stair, "Stair" }, { APINeig_StairOn, "StairOn" }, { APINeig_Railing, "Railing" },
	{ APINeig_RailingOn, "RailingOn" }, { APINeig_BeamSegment, "BeamSegment" }, { APINeig_BeamSegmentOn, "BeamSegmentOn" },
	{ APINeig_ColumnSegment, "ColumnSegment" }, { APINeig_ColumnSegmentOn, "ColumnSegmentOn" },
};


} // namespace


// Neig kinds that are not a plain 2D point (lines/edges, 3D-only hotspots, "clean intersections off" variants).
bool IsDimensionPointNeig (API_NeigID id)
{
	switch (id) {
		case APINeig_Symb3D: case APINeig_Light3D: case APINeig_Wind3D: case APINeig_Door3D:
		case APINeig_CWFrame3D: case APINeig_CWPanel3D: case APINeig_CWJunction3D: case APINeig_CWAccessory3D:
		case APINeig_Skylight3D: case APINeig_SkylightOn3D:
		case APINeig_WallPlClOff: case APINeig_WallPlOnClOff:
			return false;
		default:
			break;
	}
	if (IsLineNeig (id))
		return false;
	// Remaining "...On" kinds (edges / lines) are not points either.
	const GS::UniString name = NeigName (id);
	return !(name.GetLength () > 2 && name.EndsWith (GS::UniString ("On")));
}


namespace {


// Tie-break between anchors at the same distance: plain points before "special" ones, hotspots before probes.
int AnchorRank (const Anchor& a)
{
	return (IsSpecialNeig (a.neig.neigID) ? 2 : 0) + (a.source == "probe" ? 1 : 0);
}

} // namespace


GS::UniString NeigName (API_NeigID id)
{
	for (const NeigNameEntry& e : kNeigNames) {
		if (e.id == id)
			return e.name;
	}
	GS::UniString s;
	s.Printf ("Neig%d", (int) id);
	return s;
}


API_Base BaseFromNeig (const API_Neig& neig)
{
	API_Base base;
	BNZeroMemory (&base, sizeof (base));
	API_Elem_Head head;
	BNZeroMemory (&head, sizeof (head));
	head.guid = neig.guid;
	if (ACAPI_Element_GetHeader (&head) == NoError) {
		base.type = head.type;
	} else {
		API_ElemType type (API_ZombieElemID);
		if (ACAPI_Goodies_NeigIDToElemType (neig.neigID, type) == NoError)
			base.type = type;
	}
	base.line = IsLineNeig (neig.neigID);
	base.special = IsSpecialNeig (neig.neigID) ? 1 : 0;
	base.inIndex = neig.inIndex;
	base.guid = neig.guid;
	return base;
}


bool IsAssociativeBase (const API_Base& base)
{
	return base.guid != APINULLGuid;
}


void AddBaseJson (OS& out, const API_Base& base)
{
	out.Add ("associative", IsAssociativeBase (base));
	if (!IsAssociativeBase (base))
		return;
	out.Add ("element", GuidStr (base.guid));
	out.Add ("elementType", ElemTypeName (base.type));
	out.Add ("inIndex", (Int32) base.inIndex);
	out.Add ("line", base.line);
	out.Add ("special", base.special != 0);
	if (base.node_id != 0)
		out.Add ("nodeId", (Int32) base.node_id);
}


bool RawBaseFromSpec (const OS& spec, API_Base& base)
{
	if (!spec.Contains ("inIndex"))
		return false;
	BNZeroMemory (&base, sizeof (base));
	const API_Guid guid = GetGuid (spec, "element");
	const API_Elem_Head head = GetHeader (guid);
	base.type = head.type;
	base.guid = guid;
	base.inIndex = GetInt (spec, "inIndex");
	base.line = GetBool (spec, "line", false);
	base.special = GetBool (spec, "special", false) ? 1 : 0;
	if (auto nodeId = OptInt (spec, "nodeId")) {
		if (*nodeId < 0)
			Fail ("nodeId must be >= 0.");
		base.node_id = (UInt32) *nodeId;
	}
	return true;
}

// --- Anchors -------------------------------------------------------------------------------

GS::Array<Anchor> CollectAnchors (const API_Guid& guid)
{
	GS::Array<Anchor> result;
	const API_Elem_Head head = GetHeader (guid);

	GS::Array<API_ElementHotspot> hotspots;
	if (ACAPI_Element_GetHotspots (guid, &hotspots) == NoError) {
		for (const API_ElementHotspot& h : hotspots) {
			Anchor a;
			a.neig = h.first;
			if (a.neig.guid == APINULLGuid)
				a.neig.guid = guid;
			a.coord = h.second;
			a.source = "hotspot";
			result.Push (a);
		}
	}

	// Probe the element's main reference points (in case hotspots omit them).
	auto probe = [&] (API_NeigID id, Int32 from, Int32 to) {
		for (Int32 i = from; i <= to; ++i) {
			bool known = false;
			for (const Anchor& a : result) {
				if (a.neig.neigID == id && a.neig.inIndex == i) { known = true; break; }
			}
			if (known)
				continue;
			API_Neig neig (guid);
			neig.neigID = id;
			neig.inIndex = i;
			API_Coord3D c, c2;
			BNZeroMemory (&c, sizeof (c));
			BNZeroMemory (&c2, sizeof (c2));
			if (ACAPI_Goodies (APIAny_NeigToCoordID, &neig, &c, &c2) == NoError) {
				Anchor a;
				a.neig = neig;
				a.coord = c;
				a.source = "probe";
				result.Push (a);
			}
		}
	};

	switch (head.type.typeID) {
		case API_WallID:	probe (APINeig_Wall, 1, 2);										break;
		case API_BeamID:	probe (APINeig_Beam, 1, 2);										break;
		case API_LineID:	probe (APINeig_Line, 1, 3);										break;
		case API_WindowID:	probe (APINeig_Wind, 1, 1); probe (APINeig_WindHole, 1, 4);	break;
		case API_DoorID:	probe (APINeig_Door, 1, 1); probe (APINeig_DoorHole, 1, 4);	break;
		default:																			break;
	}
	return result;
}


const Anchor* NearestAnchor (const GS::Array<Anchor>& anchors, const API_Coord& target, double tolerance)
{
	const Anchor* best = nullptr;
	double bestDist = 0.0;
	for (const Anchor& a : anchors) {
		if (!IsDimensionPointNeig (a.neig.neigID))
			continue;
		API_Coord c; c.x = a.coord.x; c.y = a.coord.y;
		const double d = Dist (c, target);
		if (d > tolerance)
			continue;
		if (best == nullptr || d < bestDist - 1e-9 || (std::fabs (d - bestDist) <= 1e-9 && AnchorRank (a) < AnchorRank (*best))) {
			best = &a;
			bestDist = d;
		}
	}
	return best;
}


const Anchor* AnchorAtProjection (const GS::Array<Anchor>& anchors, const API_Coord& target, const Vec& dir,
								  double tolerance, double maxPerp)
{
	const Anchor* best = nullptr;
	double bestPerp = 0.0;
	const Vec n = LeftNormal (dir);
	for (const Anchor& a : anchors) {
		if (!IsDimensionPointNeig (a.neig.neigID))
			continue;
		const Vec delta = Sub ({ a.coord.x, a.coord.y }, V (target));
		if (std::fabs (Dot (delta, dir)) > tolerance)
			continue;
		const double perp = std::fabs (Dot (delta, n));
		if (perp > maxPerp)
			continue;
		if (best == nullptr || perp < bestPerp - 1e-9 || (std::fabs (perp - bestPerp) <= 1e-9 && AnchorRank (a) < AnchorRank (*best))) {
			best = &a;
			bestPerp = perp;
		}
	}
	return best;
}

// --- Memo helpers -----------------------------------------------------------------------------

Int32 DimElemCount (const API_ElementMemo& memo, Int32 declared)
{
	if (memo.dimElems == nullptr || *memo.dimElems == nullptr)
		return 0;
	const Int32 inHandle = (Int32) (BMGetHandleSize (reinterpret_cast<GSConstHandle> (memo.dimElems)) / (GSSize) sizeof (API_DimElem));
	return declared < 0 ? inHandle : std::min (inHandle, declared);
}


void FreeDimElems (API_ElementMemo& memo)
{
	if (memo.dimElems == nullptr)
		return;
	const Int32 n = DimElemCount (memo, -1);
	for (Int32 i = 0; i < n; ++i) {
		API_NoteType& note = (*memo.dimElems)[i].note;
		if (note.contentUStr != nullptr) {
			delete note.contentUStr;
			note.contentUStr = nullptr;
		}
	}
	BMKillHandle (reinterpret_cast<GSHandle*> (&memo.dimElems));
	memo.dimElems = nullptr;
}


ElementStrings ReadElementStrings (const API_Guid& guid, API_ElemTypeID typeID)
{
	ElementStrings out;
	API_Element element = NewElement (typeID);
	element.header.guid = guid;
	GS::UniString* first = nullptr;
	GS::UniString* second = nullptr;
	GS::UniString** slot1 = nullptr;
	GS::UniString** slot2 = nullptr;
	switch (typeID) {
		case API_DimensionID:		slot1 = &element.dimension.defNote.contentUStr;										break;
		case API_RadialDimensionID:	slot1 = &element.radialDimension.note.contentUStr; slot2 = &element.radialDimension.prefixUStr;	break;
		case API_LevelDimensionID:	slot1 = &element.levelDimension.note1.contentUStr; slot2 = &element.levelDimension.note2.contentUStr;	break;
		case API_AngleDimensionID:	slot1 = &element.angleDimension.note.contentUStr;									break;
		default:					return out;
	}
	first = &out.first;
	if (slot1 != nullptr) *slot1 = first;
	if (slot2 != nullptr) { second = &out.second; *slot2 = second; }
	if (ACAPI_Element_Get (&element) != NoError)
		return out;
	// If the API replaced our pointers with its own strings, copy their content (ownership is not documented,
	// so they are left alone).
	if (slot1 != nullptr && *slot1 != nullptr && *slot1 != first)
		out.first = **slot1;
	if (slot2 != nullptr && *slot2 != nullptr && *slot2 != second)
		out.second = **slot2;
	return out;
}


PlanDatabaseScope::PlanDatabaseScope (bool enable)
{
	BNZeroMemory (&previous, sizeof (previous));
	if (!enable)
		return;
	if (ACAPI_Database (APIDb_GetCurrentDatabaseID, &previous, nullptr) != NoError)
		return;
	if (previous.typeID == APIWind_FloorPlanID)
		return;
	API_DatabaseInfo plan;
	BNZeroMemory (&plan, sizeof (plan));
	plan.typeID = APIWind_FloorPlanID;
	switched = ACAPI_Database (APIDb_ChangeCurrentDatabaseID, &plan, nullptr) == NoError;
}


PlanDatabaseScope::~PlanDatabaseScope ()
{
	if (switched)
		ACAPI_Database (APIDb_ChangeCurrentDatabaseID, &previous, nullptr);
}


bool IsModelElementType (API_ElemTypeID typeID)
{
	switch (typeID) {
		case API_LineID: case API_PolyLineID: case API_ArcID: case API_CircleID: case API_SplineID:
		case API_HatchID: case API_TextID: case API_LabelID: case API_HotspotID: case API_PictureID:
		case API_DimensionID: case API_RadialDimensionID: case API_LevelDimensionID: case API_AngleDimensionID:
		case API_DetailID: case API_WorksheetID: case API_DrawingID: case API_ChangeMarkerID:
		case API_ZombieElemID:
			return false;
		default:
			return true;
	}
}


bool NeedsFloorPlan (const GS::Array<API_Guid>& guids)
{
	GS::Array<API_Guid> unresolved;
	for (const API_Guid& guid : guids) {
		API_Elem_Head head;
		BNZeroMemory (&head, sizeof (head));
		head.guid = guid;
		if (ACAPI_Element_GetHeader (&head) != NoError)
			unresolved.Push (guid);
		else if (IsModelElementType (head.type.typeID))
			return true;
	}
	if (unresolved.IsEmpty ())
		return false;
	// Not found in the active database (e.g. a section / detail window is active): elements that exist on the
	// Floor Plan must be dimensioned there.
	PlanDatabaseScope plan (true);
	for (const API_Guid& guid : unresolved) {
		API_Elem_Head head;
		BNZeroMemory (&head, sizeof (head));
		head.guid = guid;
		if (ACAPI_Element_GetHeader (&head) == NoError)
			return true;
	}
	return false;
}


void DeleteCreated (const API_Guid& guid)
{
	GS::Array<API_Guid> guids;
	guids.Push (guid);
	ACAPI_Element_Delete (guids);
}

} // namespace dimension
} // namespace cc
