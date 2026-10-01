// *****************************************************************************
// Attributes — every Archicad attribute type:
//   Pen, Layer, Line, Fill, Composite, Surface, LayerCombination, ZoneCategory, Font,
//   Profile, PenTable, DimensionStandard, ModelViewOption, MEPSystem, OperationProfile,
//   BuildingMaterial.
//
// Commands (namespace ClaudeConnector):
//   GetAttributes          {type?, attributes?, nameFilter?, detailed?, offset?, limit?}
//   CreateAttributes       {type, attributes: [{name, basedOn?, folder?, ifExists?, ...type fields}], ifExists?}
//   ModifyAttributes       {type?, attributes: [{type?, attribute, name?, folder?, ...type fields}]}
//   DeleteAttributes       {type, attributes: [ref], force?}
//   ModifyPens             {penTable?, pens: [{index, color?, width?, description?}]}
//   ApplyLayerCombination  {layerCombination}
//   SetLayerStates         {layers: [{layer | match, hidden | visible, locked, wireframe, intersectionGroup}]}
//
// Type-specific fields use the SAME names on input (create/modify) and output (GetAttributes);
// see the Apply*/Serialize* pairs below. Units: lengths in meters (composite skins, profile
// shapes, texture sizes), pen widths in paper millimeters, angles in degrees, colors "#RRGGBB".
// *****************************************************************************

#include "Commands/Commands.hpp"
#include "Commands/AttributesCommon.hpp"
#include "Core/Command.hpp"
#include "Core/Enums.hpp"
#include "Core/LibParts.hpp"
#include "Core/Types.hpp"

#include "Location.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace cc {
namespace attr {

// =============================================================================
// RAII holders
// =============================================================================

static GS::UniString FromHeaderName (const char (&buf)[API_AttrNameLen])
{
	char tmp[API_AttrNameLen] = {};
	for (USize i = 0; i + 1 < API_AttrNameLen && buf[i] != 0; ++i)
		tmp[i] = buf[i];
	return GS::UniString (tmp);
}


// The Archicad layer (layer index 1) stores a one-character placeholder name (0x14) that the UI
// shows localized. Report (and accept) the localized name instead.
static const GS::UniString& ArchicadLayerName ()
{
	static const GS::UniString name = [] {
		API_ServerApplicationInfo info;		// has UniString members: do not memset
		ACAPI_GetReleaseNumber (&info);
		if (info.language == "RUS")
			return GS::UniString ("Слой Archicad", CC_UTF8);
		if (info.language == "GER")
			return GS::UniString ("Archicad Ebene");
		return GS::UniString ("Archicad Layer");
	} ();
	return name;
}


static bool IsPlaceholderName (const GS::UniString& s)
{
	return s.GetLength () == 1 && (UInt32) (GS::UniChar::Layout) s[0] < 0x20;
}


AttrData::AttrData ()
{
	BNZeroMemory (&attr, sizeof (attr));
}


AttrData::AttrData (API_AttrTypeID typeID) : AttrData ()
{
	attr.header.typeID = typeID;
}


AttrData::~AttrData ()
{
	Reset ();
}


void AttrData::Reset ()
{
	const API_AttrTypeID typeID = attr.header.typeID;
	if (typeID == API_MaterialID && attr.material.texture.fileLoc != nullptr) {
		delete attr.material.texture.fileLoc;
		attr.material.texture.fileLoc = nullptr;
	}
	if (typeID == API_ModelViewOptionsID && attr.modelViewOpt.modelViewOpt.gdlOptions != nullptr)
		ACAPI_FreeGDLModelViewOptionsPtr (&attr.modelViewOpt.modelViewOpt.gdlOptions);
	BNZeroMemory (&attr, sizeof (attr));
	attr.header.typeID = typeID;
}


void AttrData::PrepareForWrite ()
{
	// Keep the Archicad layer's stored placeholder name unless the caller really renamed it.
	attr.header.uniStringNamePtr = (!rawName.IsEmpty () && name == ArchicadLayerName ()) ? &rawName : &name;
	if (attr.header.typeID == API_BuildingMaterialID) {
		attr.buildingMaterial.id = &bmId;
		attr.buildingMaterial.manufacturer = &bmManufacturer;
		attr.buildingMaterial.description = &bmDescription;
	}
}


GSErrCode AttrData::Load (API_AttrTypeID typeID, API_AttributeIndex index)
{
	Reset ();					// frees what the previous Load returned (uses the previous type)
	attr.header.typeID = typeID;
	attr.header.index = index;
	name.Clear ();
	rawName.Clear ();
	bmId.Clear ();
	bmManufacturer.Clear ();
	bmDescription.Clear ();
	PrepareForWrite ();
	const GSErrCode err = ACAPI_Attribute_Get (&attr);
	if (err == NoError && typeID == API_BuildingMaterialID) {
		// The string pointers are caller-provided buffers; copy defensively should Archicad ever
		// hand back its own strings instead of filling ours.
		const API_BuildingMaterialType& b = attr.buildingMaterial;
		if (b.id != nullptr && b.id != &bmId) bmId = *b.id;
		if (b.manufacturer != nullptr && b.manufacturer != &bmManufacturer) bmManufacturer = *b.manufacturer;
		if (b.description != nullptr && b.description != &bmDescription) bmDescription = *b.description;
	}
	if (err == NoError && attr.header.uniStringNamePtr != nullptr && attr.header.uniStringNamePtr != &name)
		name = *attr.header.uniStringNamePtr;
	if (err == NoError && name.IsEmpty () && attr.header.name[0] != 0)
		name = FromHeaderName (attr.header.name);
	if (err == NoError && typeID == API_LayerID && index == 1 && IsPlaceholderName (name)) {
		rawName = name;
		name = ArchicadLayerName ();
	}
	PrepareForWrite ();
	return err;
}


DefsExt::DefsExt ()
{
	BNZeroMemory (&defs, sizeof (defs));
}


DefsExt::~DefsExt ()
{
	Clear ();
}


void DefsExt::Clear ()
{
	ACAPI_DisposeAttrDefsHdlsExt (&defs);
	BNZeroMemory (&defs, sizeof (defs));
}


GSErrCode DefsExt::Load (API_AttrTypeID typeID, API_AttributeIndex index)
{
	Clear ();
	const GSErrCode err = ACAPI_Attribute_GetDefExt (typeID, index, &defs);
	if (err == APIERR_BADID) {		// the attribute has no definition data
		Clear ();
		return NoError;
	}
	return err;
}

namespace {

// =============================================================================
// Small helpers
// =============================================================================

bool HasDefs (API_AttrTypeID t)
{
	switch (t) {
		case API_LinetypeID:
		case API_FilltypeID:
		case API_CompWallID:
		case API_LayerCombID:
		case API_ZoneCatID:
		case API_ProfileID:
		case API_PenTableID:
		case API_OperationProfileID:
			return true;
		default:
			return false;
	}
}


GS::UniString TypeName (API_AttrTypeID t)
{
	return AttrTypeName (t);
}


GS::UniString IntStr (Int32 v)
{
	return GS::ValueToUniString (v);
}


GS::UniString Quote (const GS::UniString& s)
{
	return "'" + s + "'";
}


template <typename T>
T** NewHandle (Int32 count)
{
	if (count <= 0)
		return nullptr;
	T** h = reinterpret_cast<T**> (BMAllocateHandle ((GSSize) count * (GSSize) sizeof (T), ALLOCATE_CLEAR, 0));
	if (h == nullptr)
		Fail ("Out of memory.", APIERR_MEMFULL);
	return h;
}


template <typename T>
Int32 HandleCount (T** h)
{
	if (h == nullptr)
		return 0;
	return (Int32) (BMGetHandleSize (reinterpret_cast<GSConstHandle> (h)) / (GSSize) sizeof (T));
}


template <typename T>
void SetHandle (T**& slot, T** h)
{
	if (slot != nullptr)
		BMKillHandle (reinterpret_cast<GSHandle*> (&slot));
	slot = h;
}


template <typename T>
std::vector<T> HandleToVector (T** h, Int32 maxCount)
{
	std::vector<T> v;
	const Int32 n = std::min (HandleCount (h), std::max (maxCount, (Int32) 0));
	for (Int32 i = 0; i < n; ++i)
		v.push_back ((*h)[i]);
	return v;
}


template <typename T>
T** VectorToHandle (const std::vector<T>& v)
{
	T** h = NewHandle<T> ((Int32) v.size ());
	for (size_t i = 0; i < v.size (); ++i)
		(*h)[i] = v[i];
	return h;
}


template <typename F>
void SetBits (F& flags, Int32 bits, bool on)
{
	flags = on ? (F) (flags | bits) : (F) (flags & ~bits);
}


template <typename F>
void ApplyBit (F& flags, const OS& s, const char* key, Int32 bits)
{
	if (auto v = OptBool (s, key))
		SetBits (flags, bits, *v);
}


double RangeDouble (const OS& s, const char* key, double minV, double maxV)
{
	const double v = GetDouble (s, key);
	if (v < minV || v > maxV) {
		char buf[256] = {};
		std::snprintf (buf, sizeof (buf), "'%s' must be between %g and %g (got %g).", key, minV, maxV, v);
		Fail (GS::UniString (buf));
	}
	return v;
}


std::optional<double> OptRange (const OS& s, const char* key, double minV, double maxV)
{
	if (!s.Contains (key))
		return std::nullopt;
	return RangeDouble (s, key, minV, maxV);
}


std::optional<double> OptPositive (const OS& s, const char* key)
{
	auto v = OptDouble (s, key);
	if (v.has_value () && !(*v > 0.0))
		Fail ("'" + GS::UniString (key) + "' must be greater than 0.");
	return v;
}


std::optional<double> OptNonNegative (const OS& s, const char* key)
{
	auto v = OptDouble (s, key);
	if (v.has_value () && *v < 0.0)
		Fail ("'" + GS::UniString (key) + "' must not be negative.");
	return v;
}


// "hidden": true | "visible": false (either spelling).
std::optional<bool> OptHidden (const OS& s)
{
	auto h = OptBool (s, "hidden");
	auto v = OptBool (s, "visible");
	if (h.has_value () && v.has_value () && *h == *v)
		Fail ("'hidden' and 'visible' contradict each other; give only one of them.");
	if (h.has_value ())
		return h;
	if (v.has_value ())
		return !*v;
	return std::nullopt;
}


GS::UniString FromUBuf (const GS::uchar_t* buf, USize cap)
{
	USize n = 0;
	while (n < cap && buf[n] != 0)
		++n;
	return GS::UniString (buf, n);
}


void ToUBuf (GS::uchar_t* dst, USize cap, const GS::UniString& s, const char* what)
{
	if (s.GetLength () >= cap)
		Fail ("'" + GS::UniString (what) + "' is too long (max " + IntStr ((Int32) cap - 1) + " characters).");
	BNZeroMemory (dst, cap * sizeof (GS::uchar_t));
	const GS::uchar_t* src = s.ToUStr ();
	for (USize i = 0; i < s.GetLength (); ++i)
		dst[i] = src[i];
}


GS::UniString FromCBuf (const char* buf, USize cap)
{
	char tmp[256] = {};
	const USize n = std::min<USize> (cap, sizeof (tmp) - 1);
	for (USize i = 0; i < n && buf[i] != 0; ++i)
		tmp[i] = buf[i];
	return GS::UniString (tmp);
}


void ToCBuf (char* dst, USize cap, const GS::UniString& s, const char* what)
{
	GS::String str (s.ToCStr ().Get ());
	if (str.GetLength () >= cap)
		Fail ("'" + GS::UniString (what) + "' is too long (max " + IntStr ((Int32) cap - 1) + " bytes).");
	BNZeroMemory (dst, cap);
	for (USize i = 0; i < str.GetLength (); ++i)
		dst[i] = str.ToCStr ()[i];
}


GS::Array<GS::UniString> SplitPath (const GS::UniString& s)
{
	GS::Array<GS::UniString> parts;
	GS::UniString cur;
	for (UIndex i = 0; i < s.GetLength (); ++i) {
		if (s[i] == '/') {
			if (!cur.IsEmpty ()) parts.Push (cur);
			cur.Clear ();
		} else {
			cur.Append (s[i]);
		}
	}
	if (!cur.IsEmpty ())
		parts.Push (cur);
	return parts;
}


GS::UniString JoinPath (const GS::Array<GS::UniString>& path)
{
	GS::UniString s;
	for (const GS::UniString& p : path) {
		if (!s.IsEmpty ()) s += "/";
		s += p;
	}
	return s;
}


// Case-insensitive wildcard match ('*' any run, '?' one character).
bool GlobMatch (const GS::UniString& pattern, const GS::UniString& text)
{
	const GS::UniString p = pattern.ToLowerCase ();
	const GS::UniString t = text.ToLowerCase ();
	const USize pn = p.GetLength (), tn = t.GetLength ();
	USize pi = 0, ti = 0, star = MaxUSize, mark = 0;
	while (ti < tn) {
		if (pi < pn && (p[pi] == '?' || p[pi] == t[ti])) {
			++pi; ++ti;
		} else if (pi < pn && p[pi] == '*') {
			star = pi++;
			mark = ti;
		} else if (star != MaxUSize) {
			pi = star + 1;
			ti = ++mark;
		} else {
			return false;
		}
	}
	while (pi < pn && p[pi] == '*')
		++pi;
	return pi == pn;
}


bool HasWildcards (const GS::UniString& s)
{
	return s.Contains ('*') || s.Contains ('?');
}


// Name filter: wildcard pattern when it contains * or ?, otherwise case-insensitive substring.
bool NameMatches (const GS::UniString& filter, const GS::UniString& name)
{
	if (filter.IsEmpty ())
		return true;
	if (HasWildcards (filter))
		return GlobMatch (filter, name);
	return name.ToLowerCase ().Contains (filter.ToLowerCase ());
}

// =============================================================================
// Colors, pens, patterns
// =============================================================================

GS::UniString ColorHex (const API_RGBColor& c)
{
	auto b = [] (double v) { return (int) std::lround (std::clamp (v, 0.0, 1.0) * 255.0); };
	// NOTE: GS::UniString::Printf is STATIC (s.Printf (...) discards its result) -> use snprintf.
	char buf[16] = {};
	std::snprintf (buf, sizeof (buf), "#%02X%02X%02X", b (c.f_red), b (c.f_green), b (c.f_blue));
	return GS::UniString (buf);
}


int HexValue (char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}


// Parses exactly `digits` hex digits (an optional leading '#' or "0x" is skipped).
bool ParseHexDigits (const GS::UniString& text, int digits, std::vector<int>& bytes)
{
	GS::String s (text.ToCStr ().Get ());
	const char* p = s.ToCStr ();
	while (*p == ' ') ++p;
	if (*p == '#') ++p;
	else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
	std::vector<int> nibbles;
	for (; *p != 0 && *p != ' '; ++p) {
		const int v = HexValue (*p);
		if (v < 0)
			return false;
		nibbles.push_back (v);
	}
	if ((int) nibbles.size () != digits)
		return false;
	bytes.clear ();
	for (size_t i = 0; i + 1 < nibbles.size (); i += 2)
		bytes.push_back (nibbles[i] * 16 + nibbles[i + 1]);
	return true;
}


std::optional<API_RGBColor> OptColor (const OS& s, const char* key)
{
	if (!s.Contains (key))
		return std::nullopt;
	const GS::UniString help = "'" + GS::UniString (key) + "' must be a color: \"#RRGGBB\" or {red, green, blue} with 0..1 components.";
	API_RGBColor c = {};
	if (s.IsString (key)) {
		std::vector<int> bytes;
		if (!ParseHexDigits (GetString (s, key), 6, bytes))
			Fail (help);
		c.f_red = bytes[0] / 255.0;
		c.f_green = bytes[1] / 255.0;
		c.f_blue = bytes[2] / 255.0;
		return c;
	}
	if (s.IsObject (key)) {
		OS o = GetObject (s, key);
		const bool longNames = o.Contains ("red") || o.Contains ("green") || o.Contains ("blue");
		const double r = GetDouble (o, longNames ? "red" : "r", 0.0);
		const double g = GetDouble (o, longNames ? "green" : "g", 0.0);
		const double b = GetDouble (o, longNames ? "blue" : "b", 0.0);
		// {r,g,b} or components above 1 are taken as 0..255.
		const double scale = (!longNames || r > 1.0 || g > 1.0 || b > 1.0) ? 255.0 : 1.0;
		c.f_red = r / scale;
		c.f_green = g / scale;
		c.f_blue = b / scale;
		if (c.f_red < 0 || c.f_red > 1 || c.f_green < 0 || c.f_green > 1 || c.f_blue < 0 || c.f_blue > 1)
			Fail (help);
		return c;
	}
	Fail (help);
}


void ApplyColor (API_RGBColor& target, const OS& s, const char* key)
{
	if (auto c = OptColor (s, key))
		target = *c;
}


GS::UniString PatternHex (const API_Pattern& pat)
{
	char buf[20] = {};
	for (int i = 0; i < 8; ++i)
		std::snprintf (buf + 2 * i, sizeof (buf) - 2 * i, "%02X", (unsigned) (unsigned char) pat[i]);
	return GS::UniString (buf);
}


bool ApplyPattern (API_Pattern& pat, const OS& s, const char* key)
{
	if (!s.Contains (key))
		return false;
	const GS::UniString help = "'" + GS::UniString (key) + "' must be 16 hex digits (8 rows of 8 pixels, first row first, e.g. \"FFFFFFFFFFFFFFFF\" = solid) or an array of 8 numbers 0..255.";
	if (s.IsString (key)) {
		std::vector<int> bytes;
		if (!ParseHexDigits (GetString (s, key), 16, bytes))
			Fail (help);
		for (int i = 0; i < 8; ++i)
			pat[i] = (unsigned char) bytes[i];
		return true;
	}
	GS::Array<double> nums = GetNumberArray (s, key, true);
	if (nums.GetSize () != 8)
		Fail (help);
	for (int i = 0; i < 8; ++i) {
		if (nums[i] < 0 || nums[i] > 255)
			Fail (help);
		pat[i] = (unsigned char) std::lround (nums[i]);
	}
	return true;
}

// =============================================================================
// Enums
// =============================================================================

const NamedValue kFillOrientations[] = {
	{ "ProjectOrigin",	APIFillOrientation_ProjectOrigin },
	{ "ElementOrigin",	APIFillOrientation_ElementOrigin },
	{ "FitToSkin",		APIFillOrientation_FitToSkin },
};

const NamedValue kMaterialTypes[] = {
	{ "General",	APIMater_GeneralID },
	{ "Simple",		APIMater_SimpleID },
	{ "Matte",		APIMater_MatteID },
	{ "Metal",		APIMater_MetalID },
	{ "Plastic",	APIMater_PlasticID },
	{ "Glass",		APIMater_GlassID },
	{ "Glowing",	APIMater_GlowingID },
	{ "Constant",	APIMater_ConstID },
};

const NamedValue kFillTypes[] = {
	{ "Vector",			APIFill_Vector },
	{ "Symbol",			APIFill_Symbol },
	{ "Solid",			APIFill_Solid },
	{ "Empty",			APIFill_Empty },
	{ "LinearGradient",	APIFill_LinearGradient },
	{ "RadialGradient",	APIFill_RadialGradient },
	{ "Image",			APIFill_Image },
};

const NamedValue kLineTypes[] = {
	{ "Solid",		APILine_SolidLine },
	{ "Dashed",		APILine_DashedLine },
	{ "Symbol",		APILine_SymbolLine },
};

const NamedValue kLineItemTypes[] = {
	{ "Separator",	APILine_SeparatorItemType },
	{ "CenterDot",	APILine_CenterDotItemType },
	{ "CenterLine",	APILine_CenterLineItemType },
	{ "Dot",		APILine_DotItemType },
	{ "RightAngle",	APILine_RightAngleItemType },
	{ "Parallel",	APILine_ParallelItemType },
	{ "Line",		APILine_LineItemType },
	{ "Circle",		APILine_CircItemType },
	{ "Arc",		APILine_ArcItemType },
};

const NamedValue kOccupancyTypes[] = {
	{ "Residential",	APIOccupancyType_Residential },
	{ "NonResidential",	APIOccupancyType_NonResidential },
};

enum class IfExists { Error, Skip, Update };

const NamedValue kIfExists[] = {
	{ "error",	(Int32) IfExists::Error },
	{ "skip",	(Int32) IfExists::Skip },
	{ "update",	(Int32) IfExists::Update },
};

enum class LayerBase { Unchanged, Current, AllVisible, AllHidden };

const NamedValue kLayerBases[] = {
	{ "unchanged",	(Int32) LayerBase::Unchanged },
	{ "current",	(Int32) LayerBase::Current },
	{ "allVisible",	(Int32) LayerBase::AllVisible },
	{ "allHidden",	(Int32) LayerBase::AllHidden },
};

// =============================================================================
// Lookup
// =============================================================================

template <typename Fn>
void ForEachAttr (API_AttrTypeID typeID, Fn&& fn)
{
	API_AttributeIndex count = 0;
	if (ACAPI_Attribute_GetNum (typeID, &count) != NoError)
		return;
	AttrData a;
	for (API_AttributeIndex i = 1; i <= count; ++i) {
		if (a.Load (typeID, i) != NoError)
			continue;
		if (!fn (a))
			break;
	}
}


std::optional<API_AttributeIndex> FindExact (API_AttrTypeID typeID, const GS::UniString& name)
{
	API_Attr_Head head = {};
	GS::UniString searchName = name;
	head.typeID = typeID;
	head.uniStringNamePtr = &searchName;
	if (ACAPI_Attribute_Search (&head) == NoError && head.index > 0)
		return head.index;
	return std::nullopt;
}


std::optional<API_AttributeIndex> FindByName (API_AttrTypeID typeID, const GS::UniString& name)
{
	if (auto idx = FindExact (typeID, name))
		return idx;
	std::optional<API_AttributeIndex> found;
	ForEachAttr (typeID, [&] (AttrData& a) {
		if (EqualsIgnoreCase (a.name, name)) {
			found = a.Index ();
			return false;
		}
		return true;
	});
	return found;
}


// Up to 6 existing names similar to `name` (substring either way, or a shared word of 4+ letters).
GS::UniString Suggestions (API_AttrTypeID typeID, const GS::UniString& name)
{
	const GS::UniString lower = name.ToLowerCase ();
	GS::Array<GS::UniString> words;
	{
		GS::UniString cur;
		for (UIndex i = 0; i <= lower.GetLength (); ++i) {
			const bool sep = i == lower.GetLength () || lower[i] == ' ' || lower[i] == '-' || lower[i] == '_' || lower[i] == ',';
			if (sep) {
				if (cur.GetLength () >= 4) words.Push (cur);
				cur.Clear ();
			} else {
				cur.Append (lower[i]);
			}
		}
	}
	GS::Array<GS::UniString> hits;
	ForEachAttr (typeID, [&] (AttrData& a) {
		if (a.name.IsEmpty ())
			return true;
		const GS::UniString n = a.name.ToLowerCase ();
		bool match = n.Contains (lower) || (n.GetLength () >= 3 && lower.Contains (n));
		for (UIndex w = 0; !match && w < words.GetSize (); ++w)
			match = n.Contains (words[w]);
		if (match)
			hits.Push (a.name);
		return hits.GetSize () < 6;
	});
	if (hits.IsEmpty ())
		return GS::UniString ();
	GS::UniString s = " Similar names: ";
	for (UIndex i = 0; i < hits.GetSize (); ++i) {
		if (i > 0) s += ", ";
		s += Quote (hits[i]);
	}
	return s + ".";
}


std::optional<API_AttributeIndex> FindByGuid (API_AttrTypeID typeID, const API_Guid& guid)
{
	std::optional<API_AttributeIndex> found;
	ForEachAttr (typeID, [&] (AttrData& a) {
		if (a.attr.header.guid == guid) {
			found = a.Index ();
			return false;
		}
		return true;
	});
	return found;
}


GS::UniString ListHint (API_AttrTypeID typeID)
{
	return " List them with get_attributes {type: '" + TypeName (typeID) + "'} (names are localized).";
}

} // namespace


std::optional<API_AttributeIndex> OptRef (API_AttrTypeID typeID, const OS& os, const char* key)
{
	if (!os.Contains (key))
		return std::nullopt;
	const GS::UniString tname = TypeName (typeID);

	if (IsNumber (os, key)) {
		const Int32 idx = GetInt (os, key);
		if (typeID == API_PenID) {
			if (idx < 1 || idx > 255)
				Fail ("Pen index must be 1..255 (got " + IntStr (idx) + ").", APIERR_BADINDEX);
			return idx;
		}
		AttrData a;
		if (idx < 1 || a.Load (typeID, idx) != NoError)
			Fail (tname + " with index " + IntStr (idx) + " does not exist (deleted or out of range)." + ListHint (typeID), APIERR_BADINDEX);
		return idx;
	}
	if (os.IsString (key)) {
		const GS::UniString name = GetString (os, key);
		if (name.IsEmpty ())
			Fail ("Field '" + GS::UniString (key) + "' is an empty " + tname + " name.");
		auto idx = FindByName (typeID, name);
		if (!idx.has_value ())
			Fail (tname + " named " + Quote (name) + " not found." + Suggestions (typeID, name) + ListHint (typeID), APIERR_BADNAME);
		return idx;
	}
	if (os.IsObject (key)) {
		OS ref = GetObject (os, key);
		if (ref.Contains ("index"))
			return OptRef (typeID, ref, "index");
		if (ref.Contains ("name"))
			return OptRef (typeID, ref, "name");
		std::optional<API_Guid> guid;
		if (ref.Contains ("attributeId"))
			guid = GuidFromItem (GetObject (ref, "attributeId"));
		else if (ref.Contains ("guid"))
			guid = GetGuid (ref, "guid");
		if (guid.has_value ()) {
			auto idx = FindByGuid (typeID, *guid);
			if (!idx.has_value ())
				Fail (tname + " with GUID " + GuidStr (*guid) + " not found." + ListHint (typeID), APIERR_BADID);
			return idx;
		}
	}
	Fail ("Field '" + GS::UniString (key) + "' must be a " + tname + " index, name, or {index}|{name}|{guid}.");
}


API_AttributeIndex ResolveRef (API_AttrTypeID typeID, const OS& os, const char* key)
{
	auto idx = OptRef (typeID, os, key);
	if (!idx.has_value ())
		Fail ("Missing required " + TypeName (typeID) + " reference '" + GS::UniString (key) + "'.");
	return *idx;
}


OS Ref (API_AttrTypeID typeID, API_AttributeIndex index)
{
	OS ref ("index", (Int32) index);
	if (typeID == API_PenID)
		return ref;
	AttrData a;
	if (index > 0 && a.Load (typeID, index) == NoError) {
		ref.Add ("name", a.name);
		ref.Add ("guid", GuidStr (a.attr.header.guid));
	}
	return ref;
}


static GS::UniString DisplayName (API_AttrTypeID typeID, API_AttributeIndex index)
{
	AttrData a;
	if (a.Load (typeID, index) == NoError)
		return a.name;
	return "#" + GS::ValueToUniString ((Int32) index);
}


std::optional<short> OptPen (const OS& os, const char* key, bool allowSpecial)
{
	auto v = OptInt (os, key);
	if (!v.has_value ())
		return std::nullopt;
	if (*v >= 1 && *v <= 255)
		return (short) *v;
	if (allowSpecial && (*v == APIPen_Transparent || *v == APIPen_WindowBackground))
		return (short) *v;
	Fail ("'" + GS::UniString (key) + "' must be a pen index 1..255" +
		  GS::UniString (allowSpecial ? ", 0 (transparent) or -1 (window background)" : "") + " (got " + IntStr (*v) + ").");
}

namespace {

// Optional attribute reference where 0 / false means "none" (returns 0).
std::optional<API_AttributeIndex> OptRefOrNone (API_AttrTypeID typeID, const OS& s, const char* key)
{
	if (!s.Contains (key))
		return std::nullopt;
	if (s.IsBool (key)) {
		if (GetBool (s, key))
			Fail ("'" + GS::UniString (key) + "' must be a " + TypeName (typeID) + " reference, or false for none.");
		return 0;
	}
	if (IsNumber (s, key) && GetInt (s, key) == 0)
		return 0;
	return OptRef (typeID, s, key);
}


// Reads a list of attribute references. Objects ({index}|{name}|{guid}) are preferred; a uniform
// list of indices or names is accepted as well.
GS::Array<OS> GetRefList (const OS& p, const char* key)
{
	GS::Array<OS> out;
	if (!p.Contains (key))
		return out;
	if (!p.IsList (key))
		Fail ("'" + GS::UniString (key) + "' must be an array of attribute references.");
	GS::Array<OS> objects;
	p.Get (key, objects);
	if (!objects.IsEmpty ()) {
		for (const OS& o : objects)
			out.Push (OS ("attribute", o));
		return out;
	}
	GS::Array<GS::UniString> names;
	p.Get (key, names);
	if (!names.IsEmpty ()) {
		for (const GS::UniString& n : names)
			out.Push (OS ("attribute", n));
		return out;
	}
	for (double v : GetNumberArray (p, key))
		out.Push (OS ("attribute", (Int32) std::llround (v)));
	return out;
}


GS::Array<GS::UniString> GetPathField (const OS& s, const char* key)
{
	if (s.IsString (key))
		return SplitPath (GetString (s, key));
	return GetStringArray (s, key, true);
}

// =============================================================================
// Folders
// =============================================================================

using FolderMap = GS::HashTable<GS::Guid, GS::Array<GS::UniString>>;

void CollectFolders (const API_AttributeFolder& folder, Int32 depth, FolderMap& out)
{
	API_AttributeFolderContent content;
	if (ACAPI_Attribute_GetFolderContent (folder, content) != NoError)
		return;
	if (!folder.path.IsEmpty ()) {
		for (const GS::Guid& id : content.attributeIds)
			out.Put (id, folder.path);
	}
	if (depth >= 32)
		return;
	for (const API_AttributeFolder& sub : content.subFolders)
		CollectFolders (sub, depth + 1, out);
}


// Attribute folders exist in AC26 only for these types (tested live on build 5002: creating a folder for
// Line/Fill types fails with APIERR_BADPARS, and reading the folders of other types, e.g. dimension
// standards, trips an assertion inside Archicad (AttributeFolder::GetRootInAttributeContainer)), so
// never touch the folders of other types.
bool SupportsFolders (API_AttrTypeID t)
{
	switch (t) {
		case API_LayerID:
		case API_CompWallID:
		case API_ProfileID:
		case API_MaterialID:
		case API_BuildingMaterialID:
			return true;
		default:
			return false;
	}
}


FolderMap LoadFolderMap (API_AttrTypeID t)
{
	FolderMap map;
	if (!SupportsFolders (t))
		return map;
	API_AttributeFolder root;
	root.typeID = t;
	CollectFolders (root, 0, map);
	return map;
}


// Validates a 'folder' field BEFORE anything is written (so a bad folder never leaves a half-done item).
void CheckFolderField (API_AttrTypeID t, const OS& spec)
{
	if (!Has (spec, "folder"))
		return;
	if (!SupportsFolders (t))
		Fail (TypeName (t) + " attributes cannot be organized in folders (Archicad 26 has folders only for Layer, Composite, "
			  "Profile, Surface and BuildingMaterial); omit 'folder'.");
	for (const GS::UniString& part : GetPathField (spec, "folder")) {
		if (part.IsEmpty ())
			Fail ("'folder' contains an empty folder name.");
	}
}


void MoveToFolder (API_AttrTypeID t, API_AttributeIndex index, const GS::Array<GS::UniString>& path)
{
	if (!SupportsFolders (t))
		Fail (TypeName (t) + " attributes cannot be organized in folders (Archicad 26 has folders only for Layer, Composite, "
			  "Profile, Surface and BuildingMaterial); omit 'folder'.");
	AttrData a;
	Check (a.Load (t, index), "Cannot read " + TypeName (t) + " " + IntStr (index));
	// Create the folder chain level by level (parents first).
	API_AttributeFolder folder;
	folder.typeID = t;
	for (UIndex level = 1; level <= path.GetSize (); ++level) {
		API_AttributeFolder part;
		part.typeID = t;
		for (UIndex i = 0; i < level; ++i)
			part.path.Push (path[i]);
		if (ACAPI_Attribute_GetFolder (part) != NoError)
			Check (ACAPI_Attribute_CreateFolder (part), "Cannot create attribute folder '" + JoinPath (part.path) + "'");
	}
	folder.path = path;
	if (ACAPI_Attribute_GetFolder (folder) != NoError && !path.IsEmpty ())
		Fail ("Cannot access attribute folder '" + JoinPath (path) + "' of " + TypeName (t) + ".");
	GS::Array<GS::Guid> ids;
	ids.Push (APIGuid2GSGuid (a.attr.header.guid));
	Check (ACAPI_Attribute_Move (GS::Array<API_AttributeFolder> (), ids, folder),
		   "Cannot move " + Quote (a.name) + " into folder '" + JoinPath (path) + "'");
}

// =============================================================================
// Serialization
// =============================================================================

struct SerializeCtx {
	bool		detailed = false;
	FolderMap	folders;
	std::map<std::pair<Int32, Int32>, OS> refs;

	OS RefOf (API_AttrTypeID t, API_AttributeIndex i)
	{
		const auto key = std::make_pair ((Int32) t, (Int32) i);
		auto it = refs.find (key);
		if (it != refs.end ())
			return it->second;
		OS r = Ref (t, i);
		refs[key] = r;
		return r;
	}
};


void SerializeLayer (const API_Attribute& a, OS& out)
{
	const short f = a.header.flags;
	out.Add ("hidden", (f & APILay_Hidden) != 0);
	out.Add ("locked", (f & APILay_Locked) != 0);
	out.Add ("wireframe", (f & APILay_ForceToWire) != 0);
	out.Add ("intersectionGroup", (Int32) a.layer.conClassId);
	if (f & APILay_Xref)
		out.Add ("xref", true);
	if (f & APILay_NotMine)
		out.Add ("notMine", true);
}


OS PenJson (const API_PenType& pen)
{
	OS o ("index", (Int32) pen.head.index);
	o.Add ("color", ColorHex (pen.rgb));
	o.Add ("width", pen.width);
	GS::UniString desc = FromCBuf (pen.description, sizeof (pen.description));
	if (!desc.IsEmpty ())
		o.Add ("description", desc);
	return o;
}


void SerializeLine (const API_Attribute& a, const DefsExt* d, OS& out)
{
	const API_LinetypeType& lt = a.linetype;
	out.Add ("lineType", NameOf (kLineTypes, lt.type));
	out.Add ("scaleWithPlan", (a.header.flags & APILine_ScaleWithPlan) != 0);
	out.Add ("defineScale", lt.defineScale);
	if (lt.type != APILine_SolidLine) {
		out.Add ("period", lt.period);
		out.Add ("itemCount", (Int32) lt.nItems);
	}
	if (lt.type == APILine_SymbolLine)
		out.Add ("height", lt.height);
	if (d == nullptr)
		return;
	if (lt.type == APILine_DashedLine && d->defs.ltype_dashItems != nullptr) {
		GS::Array<OS> dashes;
		const Int32 n = std::min ((Int32) lt.nItems, HandleCount (d->defs.ltype_dashItems));
		for (Int32 i = 0; i < n; ++i) {
			const API_DashItems& di = (*d->defs.ltype_dashItems)[i];
			dashes.Push (OS ("dash", di.dash, "gap", di.gap));
		}
		out.Add ("dashes", dashes);
	}
	if (lt.type == APILine_SymbolLine && d->defs.ltype_lineItems != nullptr) {
		GS::Array<OS> items;
		const Int32 n = std::min ((Int32) lt.nItems, HandleCount (d->defs.ltype_lineItems));
		for (Int32 i = 0; i < n; ++i) {
			const API_LineItems& li = (*d->defs.ltype_lineItems)[i];
			OS item ("type", NameOf (kLineItemTypes, li.itemType));
			item.Add ("centerOffset", li.itemCenterOffs);
			item.Add ("length", li.itemLength);
			item.Add ("begin", CoordObj (li.itemBegPos));
			item.Add ("end", CoordObj (li.itemEndPos));
			item.Add ("radius", li.itemRadius);
			AddAngle (item, "beginAngle", li.itemBegAngle);
			AddAngle (item, "endAngle", li.itemEndAngle);
			items.Push (item);
		}
		out.Add ("items", items);
	}
}


void SerializeFill (const API_Attribute& a, const DefsExt* d, OS& out)
{
	const API_FilltypeType& ft = a.filltype;
	const short f = a.header.flags;
	out.Add ("fillType", NameOf (kFillTypes, ft.subType));
	out.Add ("usage", OS ("drafting", (f & APIFill_ForPoly) != 0, "cut", (f & APIFill_ForWall) != 0, "cover", (f & APIFill_ForCover) != 0));
	out.Add ("scaleWithPlan", (f & APIFill_ScaleWithPlan) != 0);
	out.Add ("bitmapPattern", PatternHex (ft.bitPat));
	if (ft.isPercentFill)
		out.Add ("percentFill", true);
	switch (ft.subType) {
		case APIFill_Vector:
		case APIFill_Symbol:
			out.Add ("spacingX", ft.hXSpac);
			out.Add ("spacingY", ft.hYSpac);
			AddAngle (out, "angle", ft.hAngle);
			out.Add ("lineCount", (Int32) ft.linNumb);
			if (ft.subType == APIFill_Symbol) {
				out.Add ("arcCount", (Int32) ft.arcNumb);
				out.Add ("fillCount", (Int32) ft.filNumb);
				out.Add ("hotspotCount", (Int32) ft.hotNumb);
				out.Add ("symbolOffset1", CoordObj (ft.c1));
				out.Add ("symbolOffset2", CoordObj (ft.c2));
			}
			break;
		case APIFill_Image: {
			OS tex ("name", FromUBuf (ft.textureName, API_UniLongNameLen));
			tex.Add ("width", ft.textureXSize);
			tex.Add ("height", ft.textureYSize);
			AddAngle (tex, "angle", ft.textureRotAng);
			tex.Add ("mirrorX", (ft.textureStatus & APITxtr_MirrorX) != 0);
			tex.Add ("mirrorY", (ft.textureStatus & APITxtr_MirrorY) != 0);
			out.Add ("texture", tex);
			break;
		}
		case APIFill_Solid:
			if (ft.percent != 0.0)
				out.Add ("percent", ft.percent);
			break;
		default:
			break;
	}
	if (d == nullptr || ft.subType != APIFill_Vector || d->defs.fill_lineItems == nullptr)
		return;
	GS::Array<OS> lines;
	const Int32 n = std::min ((Int32) ft.linNumb, HandleCount (d->defs.fill_lineItems));
	const Int32 nParts = HandleCount (d->defs.fill_lineLength);
	for (Int32 i = 0; i < n; ++i) {
		const API_FillLine& fl = (*d->defs.fill_lineItems)[i];
		OS line;
		AddAngle (line, "angle", fl.lDir);
		line.Add ("spacing", fl.lFreq);
		line.Add ("offset", CoordObj (fl.lOffset));
		line.Add ("offsetAlongLine", fl.lOffsetLine);
		GS::Array<double> parts;
		for (Int32 k = 0; k < fl.lPartNumb; ++k) {
			const Int32 at = fl.lPartOffs + k;
			if (at >= 0 && at < nParts)
				parts.Push ((*d->defs.fill_lineLength)[at]);
		}
		line.Add ("dashes", parts);
		lines.Push (line);
	}
	out.Add ("lines", lines);
}


void SerializeComposite (const API_Attribute& a, const DefsExt* d, SerializeCtx& ctx, OS& out)
{
	const API_CompWallType& cw = a.compWall;
	const short f = a.header.flags;
	out.Add ("totalThickness", cw.totalThick);
	out.Add ("skinCount", (Int32) cw.nComps);
	out.Add ("usage", OS ("walls", (f & APICWall_ForWall) != 0, "slabs", (f & APICWall_ForSlab) != 0,
						  "roofs", (f & APICWall_ForRoof) != 0, "shells", (f & APICWall_ForShell) != 0));
	if (d == nullptr)
		return;
	GS::Array<OS> skins;
	const Int32 n = std::min ((Int32) cw.nComps, HandleCount (d->defs.cwall_compItems));
	for (Int32 i = 0; i < n; ++i) {
		const API_CWallComponent& c = (*d->defs.cwall_compItems)[i];
		OS skin ("thickness", c.fillThick);
		skin.Add ("buildingMaterial", ctx.RefOf (API_BuildingMaterialID, c.buildingMaterial));
		skin.Add ("core", (c.flagBits & APICWallComp_Core) != 0);
		skin.Add ("finish", (c.flagBits & APICWallComp_Finish) != 0);
		skin.Add ("endLinePen", (Int32) c.framePen);
		skins.Push (skin);
	}
	out.Add ("skins", skins);
	GS::Array<OS> lines;
	const Int32 nl = HandleCount (d->defs.cwall_compLItems);
	for (Int32 i = 0; i < nl && i <= (Int32) cw.nComps; ++i) {
		const API_CWallLineComponent& l = (*d->defs.cwall_compLItems)[i];
		lines.Push (OS ("lineType", ctx.RefOf (API_LinetypeID, l.ltypeInd), "pen", (Int32) l.linePen));
	}
	out.Add ("skinLines", lines);
}


OS TextureJson (const API_Texture& t)
{
	OS tex ("name", FromUBuf (t.texName, API_UniLongNameLen));
	tex.Add ("width", t.xSize);
	tex.Add ("height", t.ySize);
	AddAngle (tex, "angle", t.rotAng);
	const short s = t.status;
	tex.Add ("mirrorX", (s & APITxtr_MirrorX) != 0);
	tex.Add ("mirrorY", (s & APITxtr_MirrorY) != 0);
	tex.Add ("randomShift", (s & APITxtr_RandomShift) != 0);
	tex.Add ("useAlphaChannel", (s & APITxtr_UseAlpha) != 0);
	tex.Add ("alphaAffects", OS ("surfaceColor", (s & APITxtr_SurfacePattern) != 0,
								 "ambientColor", (s & APITxtr_AmbientPattern) != 0,
								 "specularColor", (s & APITxtr_SpecularPattern) != 0,
								 "diffuseColor", (s & APITxtr_DiffusePattern) != 0,
								 "bump", (s & APITxtr_BumpPattern) != 0,
								 "transparency", (s & APITxtr_TransPattern) != 0));
	tex.Add ("fit", GS::UniString ((s & APITxtr_FillRectNatur) ? "fillRectangle" : (s & APITxtr_FitPictNatur) ? "fitPicture" : "none"));
	if (t.missingPict)
		tex.Add ("missing", true);
	if (t.fileLoc != nullptr)
		tex.Add ("file", t.fileLoc->ToDisplayText ());
	return tex;
}


void SerializeSurface (const API_Attribute& a, SerializeCtx& ctx, OS& out)
{
	const API_MaterialType& m = a.material;
	out.Add ("materialType", NameOf (kMaterialTypes, m.mtype));
	out.Add ("color", ColorHex (m.surfaceRGB));
	out.Add ("ambient", (Int32) m.ambientPc);
	out.Add ("diffuse", (Int32) m.diffusePc);
	out.Add ("specular", (Int32) m.specularPc);
	out.Add ("transparency", (Int32) m.transpPc);
	out.Add ("shininess", m.shine / 100.0);
	out.Add ("transparencyAttenuation", m.transpAtt / 100.0);
	out.Add ("specularColor", ColorHex (m.specularRGB));
	out.Add ("emissionColor", ColorHex (m.emissionRGB));
	out.Add ("emissionAttenuation", (UInt16) m.emissionAtt / 100.0);
	if (m.ifill > 0)
		out.Add ("fill", ctx.RefOf (API_FilltypeID, m.ifill));
	out.Add ("fillPen", (Int32) m.fillCol);
	if (m.texture.texName[0] != 0)
		out.Add ("texture", TextureJson (m.texture));
}


void SerializeLayerComb (const API_Attribute& a, const DefsExt* d, SerializeCtx& ctx, OS& out)
{
	out.Add ("active", a.layerComb.inEffect);
	out.Add ("layerCount", (Int32) a.layerComb.lNumb);
	if (d == nullptr || d->defs.layer_statItems == nullptr)
		return;
	GS::Array<OS> layers;
	Int32 visible = 0, locked = 0;
	const Int32 n = std::min ((Int32) a.layerComb.lNumb, HandleCount (d->defs.layer_statItems));
	for (Int32 i = 0; i < n; ++i) {
		const API_LayerStat& st = (*d->defs.layer_statItems)[i];
		OS l ("layer", ctx.RefOf (API_LayerID, st.lInd));
		const bool hidden = (st.lFlags & APILay_Hidden) != 0;
		l.Add ("hidden", hidden);
		l.Add ("locked", (st.lFlags & APILay_Locked) != 0);
		l.Add ("wireframe", (st.lFlags & APILay_ForceToWire) != 0);
		l.Add ("intersectionGroup", (Int32) st.conClassId);
		if (!hidden) ++visible;
		if (st.lFlags & APILay_Locked) ++locked;
		layers.Push (l);
	}
	out.Add ("visibleLayerCount", visible);
	out.Add ("lockedLayerCount", locked);
	out.Add ("layers", layers);
}


GS::UniString StampGuidString (const API_Guid& mainGuid, const API_Guid& revGuid)
{
	return "{" + GuidStr (mainGuid) + "}-{" + GuidStr (revGuid) + "}";
}


void SerializeZoneCat (const API_Attribute& a, const DefsExt* d, OS& out)
{
	const API_ZoneCatType& z = a.zoneCat;
	out.Add ("code", FromUBuf (z.catCode, API_UniLongNameLen));
	out.Add ("color", ColorHex (z.rgb));
	OS stamp ("name", FromUBuf (z.stampName, API_UniLongNameLen));
	stamp.Add ("guid", StampGuidString (z.stampMainGuid, z.stampRevGuid));
	out.Add ("stamp", stamp);
	if (d != nullptr)
		out.Add ("stampParameterCount", HandleCount (d->defs.zone_addParItems));
}


void SerializeProfile (const API_Attribute& a, const DefsExt* d, OS& out)
{
	const API_ProfileAttrType& p = a.profile;
	out.Add ("usage", OS ("walls", p.wallType, "beams", p.beamType, "columns", p.coluType,
						  "handrails", p.handrailType, "otherObjects", p.otherGDLObjectType));
	if (d != nullptr && d->defs.profile_vectorImageItems != nullptr)
		AddProfileGeometryJson (*d->defs.profile_vectorImageItems, out);
}


void SerializePenTable (const API_Attribute& a, const DefsExt* d, OS& out)
{
	out.Add ("activeForModel", a.penTable.inEffectForModel);
	out.Add ("activeForLayout", a.penTable.inEffectForLayout);
	if (d == nullptr || d->defs.penTable_Items == nullptr)
		return;
	GS::Array<OS> pens;
	const Int32 n = HandleCount (d->defs.penTable_Items);
	for (Int32 i = 0; i < n; ++i)
		pens.Push (PenJson ((*d->defs.penTable_Items)[i]));
	out.Add ("pens", pens);
}


template <size_t N>
GS::UniString UnitName (const char* const (&names)[N], Int32 v)
{
	if (v >= 0 && v < (Int32) N)
		return GS::UniString (names[v]);
	return "Unknown(" + IntStr (v) + ")";
}

// Names in API_LengthTypeID / API_AngleTypeID / API_AreaTypeID order (APIdefs_Environment.h).
const char* const kLengthUnits[] = { "Meter", "Decimeter", "Centimeter", "Millimeter", "FootFracInch", "FootDecInch", "DecFoot",
									 "FracInch", "DecInch", "KiloMeter", "Yard" };
const char* const kAngleUnits[] = { "DecimalDegree", "DegreeMinSec", "Grad", "Radian", "Surveyors" };
const char* const kAreaUnits[] = { "SquareMeter", "SquareKiloMeter", "SquareDeciMeter", "SquareCentimeter", "SquareMillimeter",
								   "SquareFoot", "SquareInch", "SquareYard" };


OS LengthFormatJson (const API_LengthDimFormat& f)
{
	return OS ("unit", UnitName (kLengthUnits, (Int32) f.unit), "decimals", (Int32) f.lenDecimals, "hideZeroDecimals", f.hide0Dec);
}


void SerializeDimStandard (const API_Attribute& a, OS& out)
{
	const API_DimensionPrefs& p = a.dimension.dim;
	out.Add ("linear", LengthFormatJson (p.linear));
	out.Add ("angle", OS ("unit", UnitName (kAngleUnits, (Int32) p.angle.unit), "decimals", (Int32) p.angle.angleDecimals,
						  "hideZeroDecimals", p.angle.hide0Dec));
	out.Add ("radial", LengthFormatJson (p.radial));
	out.Add ("level", LengthFormatJson (p.level));
	out.Add ("elevation", LengthFormatJson (p.elevation));
	out.Add ("doorWindow", LengthFormatJson (p.doorwindow));
	out.Add ("sillHeight", LengthFormatJson (p.parapet));
	out.Add ("area", OS ("unit", UnitName (kAreaUnits, (Int32) p.area.unit), "decimals", (Int32) p.area.lenDecimals,
						 "hideZeroDecimals", p.area.hide0Dec));
}


void SerializeMEP (const API_Attribute& a, SerializeCtx& ctx, OS& out)
{
	const API_MEPSystemType& m = a.mepSystem;
	out.Add ("ductwork", m.isForDuctwork);
	out.Add ("pipework", m.isForPipework);
	out.Add ("cabling", m.isForCabling);
	out.Add ("contourPen", (Int32) m.contourPen);
	out.Add ("fillPen", (Int32) m.fillPen);
	out.Add ("fillBackgroundPen", (Int32) m.fillBgPen);
	out.Add ("centerLinePen", (Int32) m.centerLinePen);
	if (m.fillInd > 0) out.Add ("fill", ctx.RefOf (API_FilltypeID, m.fillInd));
	if (m.centerLTypeInd > 0) out.Add ("centerLineType", ctx.RefOf (API_LinetypeID, m.centerLTypeInd));
	if (m.materialInd > 0) out.Add ("surface", ctx.RefOf (API_MaterialID, m.materialInd));
	if (m.insulationMatInd > 0) out.Add ("insulationSurface", ctx.RefOf (API_MaterialID, m.insulationMatInd));
}


void SerializeOpProfile (const API_Attribute& a, const DefsExt* d, OS& out)
{
	const API_OperationProfileType& o = a.operationProfile;
	out.Add ("occupancyType", NameOf (kOccupancyTypes, o.occupancyType));
	out.Add ("hotWaterLoad", o.hotWaterLoad);
	out.Add ("humanHeatGain", o.humanHeatGain);
	out.Add ("humidity", o.humidity);
	out.Add ("dailyProfileCount", (Int32) o.nDailyProf);
	out.Add ("usageCount", (Int32) o.nDailyProfUsage);
	if (d == nullptr)
		return;
	GS::Array<OS> profiles;
	const Int32 np = std::min ((Int32) o.nDailyProf, HandleCount (d->defs.op_dailyProfiles));
	for (Int32 i = 0; i < np; ++i)
		profiles.Push (OS ("index", i, "name", FromUBuf ((*d->defs.op_dailyProfiles)[i].name, API_UniLongNameLen)));
	out.Add ("dailyProfiles", profiles);
	GS::Array<OS> usages;
	const Int32 nu = std::min ((Int32) o.nDailyProfUsage, HandleCount (d->defs.op_dailyProfileUsages));
	for (Int32 i = 0; i < nu; ++i) {
		const API_DailyProfileUsage& u = (*d->defs.op_dailyProfileUsages)[i];
		OS usage ("startMonth", (Int32) u.startMonth, "startDay", (Int32) u.startDay, "endMonth", (Int32) u.endMonth);
		usage.Add ("endDay", (Int32) u.endDay);
		usage.Add ("recurrence", (Int32) u.recurrence);
		usage.Add ("dailyProfileIndex", (Int32) u.dailyProfileIndex);
		usages.Push (usage);
	}
	out.Add ("usages", usages);
}


Int32 ElemToUIPriority (Int32 elemPriority)
{
	Int32 elem = elemPriority;
	Int32 ui = 0;
	if (ACAPI_Goodies (APIAny_Elem2UIPriorityID, &elem, &ui) != NoError)
		return elemPriority;
	return ui;
}


Int32 UIToElemPriority (Int32 uiPriority)
{
	Int32 ui = uiPriority;
	Int32 elem = 0;
	Check (ACAPI_Goodies (APIAny_UI2ElemPriorityID, &ui, &elem), "Cannot convert the building material priority");
	return elem;
}


void SerializeBuildingMaterial (const AttrData& a, SerializeCtx& ctx, OS& out)
{
	const API_BuildingMaterialType& b = a.attr.buildingMaterial;
	out.Add ("id", a.bmId);
	out.Add ("manufacturer", a.bmManufacturer);
	out.Add ("description", a.bmDescription);
	if (b.cutFill > 0) out.Add ("cutFill", ctx.RefOf (API_FilltypeID, b.cutFill));
	out.Add ("cutFillPen", (Int32) b.cutFillPen);
	out.Add ("cutFillBackgroundPen", (Int32) b.cutFillBackgroundPen);
	if (b.cutMaterial > 0) out.Add ("surface", ctx.RefOf (API_MaterialID, b.cutMaterial));
	out.Add ("cutFillOrientation", NameOf (kFillOrientations, b.cutFillOrientation));
	out.Add ("priority", (Int32) b.connPriority);				// internal stored value
	out.Add ("uiPriority", ElemToUIPriority (b.connPriority));	// 0..999 as in the UI (= connectionPriority of the official JSON API)
	out.Add ("thermalConductivity", b.thermalConductivity);
	out.Add ("density", b.density);
	out.Add ("heatCapacity", b.heatCapacity);
	out.Add ("embodiedEnergy", b.embodiedEnergy);
	out.Add ("embodiedCarbon", b.embodiedCarbon);
	out.Add ("showUncutLines", b.showUncutLines);
	out.Add ("participatesInCollisionDetection", !b.doNotParticipateInCollDet);
}


OS SerializeAttr (AttrData& a, SerializeCtx& ctx)
{
	const API_AttrTypeID t = a.Type ();
	OS out ("index", (Int32) a.Index ());
	if (t != API_PenID || !a.name.IsEmpty ())
		out.Add ("name", a.name);
	if (a.attr.header.guid != APINULLGuid)		// pens have no GUID
		out.Add ("guid", GuidStr (a.attr.header.guid));
	if (t == API_LayerID && a.Index () == 1)
		out.Add ("isArchicadLayer", true);		// cannot be hidden, locked, renamed or deleted
	if (a.attr.header.flags & APIAttr_IsFromGDL)
		out.Add ("fromLibrary", true);
	if (const GS::Array<GS::UniString>* path = ctx.folders.GetPtr (APIGuid2GSGuid (a.attr.header.guid)))
		out.Add ("folder", *path);

	DefsExt d;
	const DefsExt* dp = nullptr;
	if (ctx.detailed && HasDefs (t)) {
		const GSErrCode err = d.Load (t, a.Index ());
		if (err == NoError)
			dp = &d;
		else
			out.Add ("detailsError", ErrorName (err));
	}

	switch (t) {
		case API_PenID: {
			out.Add ("color", ColorHex (a.attr.pen.rgb));
			out.Add ("width", a.attr.pen.width);
			GS::UniString desc = FromCBuf (a.attr.pen.description, sizeof (a.attr.pen.description));
			if (!desc.IsEmpty ())
				out.Add ("description", desc);
			break;
		}
		case API_LayerID:				SerializeLayer (a.attr, out);					break;
		case API_LinetypeID:			SerializeLine (a.attr, dp, out);				break;
		case API_FilltypeID:			SerializeFill (a.attr, dp, out);				break;
		case API_CompWallID:			SerializeComposite (a.attr, dp, ctx, out);		break;
		case API_MaterialID:			SerializeSurface (a.attr, ctx, out);			break;
		case API_LayerCombID:			SerializeLayerComb (a.attr, dp, ctx, out);		break;
		case API_ZoneCatID:				SerializeZoneCat (a.attr, dp, out);				break;
		case API_ProfileID:				SerializeProfile (a.attr, dp, out);				break;
		case API_PenTableID:			SerializePenTable (a.attr, dp, out);			break;
		case API_DimStandID:			if (ctx.detailed) SerializeDimStandard (a.attr, out);	break;
		case API_MEPSystemID:			SerializeMEP (a.attr, ctx, out);				break;
		case API_OperationProfileID:	SerializeOpProfile (a.attr, dp, out);			break;
		case API_BuildingMaterialID:	SerializeBuildingMaterial (a, ctx, out);		break;
		case API_FontID:
		case API_ModelViewOptionsID:
		default:
			break;
	}
	return out;
}

// =============================================================================
// Field application (shared by create and modify)
// =============================================================================

struct ApplyCtx {
	bool creating = false;		// CreateAttributes
	bool fresh = false;			// created from an all-zero struct (no basedOn / template)
};


void CheckFields (API_AttrTypeID t, const OS& s, bool creating)
{
	static const std::map<API_AttrTypeID, std::vector<const char*>> kFields = {
		{ API_PenID,				{ "color", "width", "description" } },
		{ API_LayerID,				{ "hidden", "visible", "locked", "wireframe", "intersectionGroup" } },
		{ API_LinetypeID,			{ "lineType", "scaleWithPlan", "defineScale", "dashes", "items", "period", "height" } },
		{ API_FilltypeID,			{ "fillType", "usage", "scaleWithPlan", "bitmapPattern", "spacingX", "spacingY", "angle", "percent", "lines", "texture" } },
		{ API_CompWallID,			{ "skins", "skinLines", "usage" } },
		{ API_MaterialID,			{ "materialType", "color", "ambient", "diffuse", "specular", "transparency", "shininess", "transparencyAttenuation",
									  "specularColor", "emissionColor", "emissionAttenuation", "fill", "fillPen", "texture" } },
		{ API_LayerCombID,			{ "base", "layers" } },
		{ API_ZoneCatID,			{ "code", "color", "stamp" } },
		{ API_FontID,				{} },
		{ API_ProfileID,			{ "usage", "shapes" } },
		{ API_PenTableID,			{ "pens" } },
		{ API_DimStandID,			{} },
		{ API_ModelViewOptionsID,	{} },
		{ API_MEPSystemID,			{ "ductwork", "pipework", "cabling", "contourPen", "fillPen", "fillBackgroundPen", "centerLinePen",
									  "fill", "centerLineType", "surface", "insulationSurface" } },
		{ API_OperationProfileID,	{ "occupancyType", "hotWaterLoad", "humanHeatGain", "humidity" } },
		{ API_BuildingMaterialID,	{ "id", "manufacturer", "description", "cutFill", "cutFillPen", "cutFillBackgroundPen", "surface",
									  "cutFillOrientation", "priority", "uiPriority", "thermalConductivity", "density", "heatCapacity", "embodiedEnergy",
									  "embodiedCarbon", "showUncutLines", "participatesInCollisionDetection" } },
	};
	const std::vector<const char*> common = creating
		? std::vector<const char*> { "name", "basedOn", "folder", "ifExists", "type" }
		: std::vector<const char*> { "name", "attribute", "folder", "type" };

	auto it = kFields.find (t);
	GS::UniString unknown;
	for (const GS::String& field : s.GetFieldNames ()) {
		bool ok = false;
		for (const char* c : common)
			ok = ok || field == c;
		if (it != kFields.end ()) {
			for (const char* c : it->second)
				ok = ok || field == c;
		}
		if (!ok) {
			if (!unknown.IsEmpty ()) unknown += ", ";
			unknown += GS::UniString (field.ToCStr ());
		}
	}
	if (unknown.IsEmpty ())
		return;
	GS::UniString allowed;
	if (it != kFields.end ()) {
		for (const char* c : it->second) {
			if (!allowed.IsEmpty ()) allowed += ", ";
			allowed += c;
		}
	}
	Fail ("Unknown field(s) for " + TypeName (t) + ": " + unknown + ". Allowed: " + (allowed.IsEmpty () ? GS::UniString ("(only name/folder)") : allowed) + ".");
}


// Resolves a texture image of the loaded libraries (a 'Picture' library part) by its name, with or
// without file extension, case-insensitively. Returns the name Archicad stores (without extension).
GS::UniString ResolvePictureName (const GS::UniString& name, const char* what)
{
	GS::UniString trimmed = name;
	trimmed.Trim ();
	Int32 count = 0;
	if (ACAPI_LibPart_GetNum (&count) == NoError) {
		for (Int32 i = 1; i <= count; ++i) {
			API_LibPart lp;
			BNZeroMemory (&lp, sizeof (lp));
			lp.index = i;
			const GSErrCode err = ACAPI_LibPart_Get (&lp);
			if (lp.location != nullptr) {
				delete lp.location;
				lp.location = nullptr;
			}
			if (err != NoError || lp.typeID != APILib_PictID)
				continue;
			const GS::UniString docu (lp.docu_UName);
			const GS::UniString file (lp.file_UName);
			if (EqualsIgnoreCase (docu, trimmed) || EqualsIgnoreCase (file, trimmed))
				return docu.IsEmpty () ? trimmed : docu;
		}
	}
	Fail ("Texture image " + Quote (trimmed) + " (" + GS::UniString (what) + ") was not found in the loaded libraries. Find image names "
		  "with search_library_parts {type: 'Picture', placeableOnly: false, query: '...'} (localized names, e.g. the texture.name of an "
		  "existing surface from get_attributes).", APIERR_BADNAME);
}


void ApplyLayer (API_Attribute& a, const OS& s, const ApplyCtx& ctx)
{
	if (auto h = OptHidden (s))
		SetBits (a.header.flags, APILay_Hidden, *h);
	ApplyBit (a.header.flags, s, "locked", APILay_Locked);
	ApplyBit (a.header.flags, s, "wireframe", APILay_ForceToWire);
	if (auto g = OptInt (s, "intersectionGroup")) {
		if (*g < 0)
			Fail ("'intersectionGroup' must be >= 0.");
		a.layer.conClassId = *g;
	} else if (ctx.fresh) {
		a.layer.conClassId = 1;
	}
}


void ApplyPen (API_PenType& pen, const OS& s)
{
	ApplyColor (pen.rgb, s, "color");
	if (auto w = OptRange (s, "width", 0.0, 100.0))
		pen.width = *w;
	if (auto d = OptString (s, "description"))
		ToCBuf (pen.description, sizeof (pen.description), *d, "description");
}


void ApplyLine (API_Attribute& a, DefsExt& d, const OS& s, const ApplyCtx& ctx)
{
	API_LinetypeType& lt = a.linetype;
	std::optional<Int32> type = OptNamed (kLineTypes, s, "lineType");
	if (!type.has_value () && ctx.fresh)
		type = Has (s, "dashes") ? APILine_DashedLine : Has (s, "items") ? APILine_SymbolLine : APILine_SolidLine;
	const bool typeChanged = type.has_value () && *type != (Int32) lt.type;
	if (type.has_value ())
		lt.type = (API_LtypTypeID) *type;

	if (auto v = OptBool (s, "scaleWithPlan")) {
		SetBits (a.header.flags, APILine_ScaleWithPlan, *v);
		SetBits (a.header.flags, APILine_FixScale, !*v);
	} else if (ctx.fresh) {
		SetBits (a.header.flags, APILine_FixScale, true);
	}
	const bool scaled = (a.header.flags & APILine_ScaleWithPlan) != 0;
	if (auto v = OptPositive (s, "defineScale"))
		lt.defineScale = *v;
	else if (ctx.fresh || lt.defineScale <= 0.0)
		lt.defineScale = scaled ? 100.0 : 1.0;

	if (lt.type == APILine_SolidLine) {
		if (Has (s, "dashes") || Has (s, "items"))
			Fail ("A Solid line type has no 'dashes' or 'items'; set lineType 'Dashed' or 'Symbol'.");
		if (typeChanged || ctx.fresh) {
			lt.nItems = 0;
			lt.period = 0.0;
			lt.height = 0.0;
			SetHandle (d.defs.ltype_dashItems, (API_DashItems**) nullptr);
			SetHandle (d.defs.ltype_lineItems, (API_LineItems**) nullptr);
		}
		return;
	}

	if (Has (s, "dashes")) {
		if (lt.type != APILine_DashedLine)
			Fail ("'dashes' needs lineType 'Dashed'.");
		GS::Array<OS> dashes = GetObjectArray (s, "dashes");
		if (dashes.IsEmpty ())
			Fail ("'dashes' needs at least one {dash, gap} item.");
		std::vector<API_DashItems> items;
		double period = 0.0;
		for (const OS& ds : dashes) {
			API_DashItems di = {};
			di.dash = GetDouble (ds, "dash");
			di.gap = GetDouble (ds, "gap");
			if (di.dash < 0.0 || di.gap < 0.0)
				Fail ("Dash and gap lengths must not be negative.");
			period += di.dash + di.gap;
			items.push_back (di);
		}
		if (!(period > 0.0))
			Fail ("The dash pattern has zero length; give positive dash/gap values.");
		SetHandle (d.defs.ltype_dashItems, VectorToHandle (items));
		SetHandle (d.defs.ltype_lineItems, (API_LineItems**) nullptr);
		lt.nItems = (Int32) items.size ();
		lt.period = period;
		lt.height = 0.0;
	} else if (lt.type == APILine_DashedLine && (ctx.fresh || typeChanged)) {
		Fail ("A Dashed line type needs 'dashes': [{dash, gap}, ...].");
	}

	if (Has (s, "items")) {
		if (lt.type != APILine_SymbolLine)
			Fail ("'items' needs lineType 'Symbol'.");
		GS::Array<OS> list = GetObjectArray (s, "items");
		if (list.IsEmpty ())
			Fail ("'items' needs at least one symbol item.");
		std::vector<API_LineItems> items;
		for (const OS& is : list) {
			API_LineItems li = {};
			li.itemType = (API_LtypItemID) ParseNamed (kLineItemTypes, is, "type");
			li.itemCenterOffs = GetDouble (is, "centerOffset", 0.0);
			li.itemLength = GetDouble (is, "length", 0.0);
			if (auto c = OptCoord (is, "begin")) li.itemBegPos = *c;
			if (auto c = OptCoord (is, "end")) li.itemEndPos = *c;
			li.itemRadius = GetDouble (is, "radius", 0.0);
			li.itemBegAngle = GetAngle (is, "beginAngle", 0.0);
			li.itemEndAngle = GetAngle (is, "endAngle", 0.0);
			items.push_back (li);
		}
		SetHandle (d.defs.ltype_lineItems, VectorToHandle (items));
		SetHandle (d.defs.ltype_dashItems, (API_DashItems**) nullptr);
		lt.nItems = (Int32) items.size ();
	} else if (lt.type == APILine_SymbolLine && (ctx.fresh || typeChanged)) {
		Fail ("A Symbol line type needs 'items' (plus 'period' and 'height'). Copying an existing symbol line with duplicate_attributes is easier.");
	}
	if (lt.type == APILine_SymbolLine) {
		if (auto p = OptPositive (s, "period")) lt.period = *p;
		if (auto h = OptNonNegative (s, "height")) lt.height = *h;
		if (!(lt.period > 0.0))
			Fail ("A Symbol line type needs a positive 'period' (length of one repetition).");
	} else if (Has (s, "period") || Has (s, "height")) {
		if (lt.type == APILine_DashedLine && Has (s, "height"))
			Fail ("'height' only applies to Symbol line types.");
		if (lt.type == APILine_DashedLine && Has (s, "period") && !Has (s, "dashes"))
			Fail ("The period of a Dashed line type is the sum of its dashes and gaps; change 'dashes' instead.");
	}
}


void ApplyFill (API_Attribute& a, DefsExt& d, const OS& s, const ApplyCtx& ctx)
{
	API_FilltypeType& ft = a.filltype;
	std::optional<Int32> sub = OptNamed (kFillTypes, s, "fillType");
	if (ctx.fresh) {
		ft.subType = (API_FillSubtype) (sub.has_value () ? *sub : (Has (s, "lines") ? APIFill_Vector : APIFill_Solid));
		a.header.flags = APIFill_ForWall | APIFill_ForPoly | APIFill_ForCover;
		ft.hXSpac = 1.0;
		ft.hYSpac = 1.0;
		const unsigned char solid[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
		const unsigned char diagonal[8] = { 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01 };
		for (int i = 0; i < 8; ++i)
			ft.bitPat[i] = ft.subType == APIFill_Empty ? 0 : ft.subType == APIFill_Vector ? diagonal[i] : solid[i];
		if (ft.subType == APIFill_Symbol)
			Fail ("Symbol fills cannot be defined from scratch; copy an existing symbol fill with duplicate_attributes (or pass basedOn).");
	} else if (sub.has_value () && *sub != (Int32) ft.subType) {
		Fail ("Changing the fill type of an existing fill (" + NameOf (kFillTypes, ft.subType) + ") is not supported; create a new fill instead.");
	}

	OS usage;
	if (TryGetObject (s, "usage", usage)) {
		ApplyBit (a.header.flags, usage, "drafting", APIFill_ForPoly);
		ApplyBit (a.header.flags, usage, "cut", APIFill_ForWall);
		ApplyBit (a.header.flags, usage, "cover", APIFill_ForCover);
	}
	ApplyBit (a.header.flags, s, "scaleWithPlan", APIFill_ScaleWithPlan);
	if (ctx.fresh) {
		// hXSpac/hYSpac scale the hatch-line geometry into METERS: 0.001 makes the given line lengths
		// millimeters on paper (scale-independent fills), 1 makes them meters in the model (scaled fills).
		const double unit = (a.header.flags & APIFill_ScaleWithPlan) ? 1.0 : 0.001;
		ft.hXSpac = unit;
		ft.hYSpac = unit;
	}
	const bool hasPattern = ApplyPattern (ft.bitPat, s, "bitmapPattern");
	if (auto v = OptPositive (s, "spacingX")) ft.hXSpac = *v;
	if (auto v = OptPositive (s, "spacingY")) ft.hYSpac = *v;
	if (auto v = OptAngle (s, "angle")) ft.hAngle = *v;
	if (auto v = OptDouble (s, "percent")) {
		double pc = *v;
		if (pc > 1.0 && pc <= 100.0)
			pc /= 100.0;					// 25 -> 0.25
		if (pc < 0.0 || pc > 1.0)
			Fail ("'percent' is the coverage of a percentage fill: 0..1 (or 0..100).");
		if (ft.subType != APIFill_Solid)
			Fail ("'percent' only applies to Solid fills (percentage fills).");
		ft.percent = pc;
		if (!hasPattern) {
			// Ordered-dither 8x8 screen pattern with the requested coverage (Archicad shows it on screen).
			static const int kBayer[8][8] = {
				{  0, 32,  8, 40,  2, 34, 10, 42 }, { 48, 16, 56, 24, 50, 18, 58, 26 },
				{ 12, 44,  4, 36, 14, 46,  6, 38 }, { 60, 28, 52, 20, 62, 30, 54, 22 },
				{  3, 35, 11, 43,  1, 33,  9, 41 }, { 51, 19, 59, 27, 49, 17, 57, 25 },
				{ 15, 47,  7, 39, 13, 45,  5, 37 }, { 63, 31, 55, 23, 61, 29, 53, 21 },
			};
			const int on = (int) std::lround (pc * 64.0);
			for (int r = 0; r < 8; ++r) {
				unsigned char row = 0;
				for (int c = 0; c < 8; ++c) {
					if (kBayer[r][c] < on)
						row = (unsigned char) (row | (0x80 >> c));
				}
				ft.bitPat[r] = row;
			}
		}
	}

	if (Has (s, "lines")) {
		if (ft.subType != APIFill_Vector)
			Fail ("'lines' only applies to Vector fills.");
		GS::Array<OS> lines = GetObjectArray (s, "lines");
		if (lines.IsEmpty ())
			Fail ("A Vector fill needs at least one hatch line.");
		std::vector<API_FillLine> items;
		std::vector<double> parts;
		for (const OS& ls : lines) {
			API_FillLine fl = {};
			fl.lDir = GetAngle (ls, "angle", 0.0);
			fl.lFreq = GetDouble (ls, "spacing");
			if (!(fl.lFreq > 0.0))
				Fail ("Hatch line 'spacing' (distance between the parallel lines) must be greater than 0.");
			if (auto c = OptCoord (ls, "offset")) fl.lOffset = *c;
			fl.lOffsetLine = GetDouble (ls, "offsetAlongLine", 0.0);
			GS::Array<double> dashes = GetNumberArray (ls, "dashes", false);
			if (dashes.IsEmpty ()) {
				dashes.Push (1.0);	// continuous line: one dash, no gap
				dashes.Push (0.0);
			}
			if (dashes.GetSize () % 2 != 0)
				Fail ("Hatch line 'dashes' must alternate dash, gap, dash, gap... (an even number of values).");
			double total = 0.0;
			for (double v : dashes) {
				if (v < 0.0)
					Fail ("Hatch line dash/gap lengths must not be negative.");
				total += v;
			}
			if (!(total > 0.0))
				Fail ("Hatch line 'dashes' have zero total length.");
			fl.lPartOffs = (Int32) parts.size ();
			fl.lPartNumb = (short) dashes.GetSize ();
			for (double v : dashes)
				parts.push_back (v);
			items.push_back (fl);
		}
		SetHandle (d.defs.fill_lineItems, VectorToHandle (items));
		SetHandle (d.defs.fill_lineLength, VectorToHandle (parts));
		ft.linNumb = (Int32) items.size ();
		ft.arcNumb = 0;
	} else if (ctx.fresh && ft.subType == APIFill_Vector) {
		Fail ("A Vector fill needs 'lines': [{angle, spacing, offset?, offsetAlongLine?, dashes?}].");
	}

	OS tex;
	if (TryGetObject (s, "texture", tex)) {
		if (ft.subType != APIFill_Image)
			Fail ("'texture' only applies to Image fills.");
		if (auto n = OptString (tex, "name"))
			ToUBuf (ft.textureName, API_UniLongNameLen, ResolvePictureName (*n, "texture.name"), "texture.name");
		if (auto v = OptPositive (tex, "width")) ft.textureXSize = *v;
		if (auto v = OptPositive (tex, "height")) ft.textureYSize = *v;
		if (auto v = OptAngle (tex, "angle")) ft.textureRotAng = *v;
		ApplyBit (ft.textureStatus, tex, "mirrorX", APITxtr_MirrorX);
		ApplyBit (ft.textureStatus, tex, "mirrorY", APITxtr_MirrorY);
	}
	if (ctx.fresh && ft.subType == APIFill_Image) {
		if (ft.textureName[0] == 0)
			Fail ("An Image fill needs texture.name (an image file name from the loaded libraries).");
		if (!(ft.textureXSize > 0.0)) ft.textureXSize = 1.0;
		if (!(ft.textureYSize > 0.0)) ft.textureYSize = 1.0;
	}
}


void ApplyComposite (API_Attribute& a, DefsExt& d, const OS& s, const ApplyCtx& ctx)
{
	API_CompWallType& cw = a.compWall;
	OS usage;
	if (TryGetObject (s, "usage", usage)) {
		ApplyBit (a.header.flags, usage, "walls", APICWall_ForWall);
		ApplyBit (a.header.flags, usage, "slabs", APICWall_ForSlab);
		ApplyBit (a.header.flags, usage, "roofs", APICWall_ForRoof);
		ApplyBit (a.header.flags, usage, "shells", APICWall_ForShell);
	} else if (ctx.fresh) {
		a.header.flags |= APICWall_ForWall | APICWall_ForSlab | APICWall_ForRoof | APICWall_ForShell;
	}

	std::vector<API_CWallComponent> skins = HandleToVector (d.defs.cwall_compItems, cw.nComps);
	std::vector<API_CWallLineComponent> lines = HandleToVector (d.defs.cwall_compLItems, cw.nComps + 1);
	const Int32 oldCount = (Int32) skins.size ();
	bool changed = false;

	if (Has (s, "skins")) {
		GS::Array<OS> specs = GetObjectArray (s, "skins");
		if (specs.IsEmpty ())
			Fail ("A composite needs at least one skin.");
		if (specs.GetSize () > 100)
			Fail ("Too many skins (max 100).");
		std::vector<API_CWallComponent> newSkins;
		for (UIndex i = 0; i < specs.GetSize (); ++i) {
			const OS& ss = specs[i];
			API_CWallComponent c = {};
			const bool existing = (Int32) i < oldCount;
			if (existing) {
				c = skins[i];
			} else {
				c.framePen = newSkins.empty () ? (short) 1 : newSkins.back ().framePen;
				if (!Has (ss, "thickness") || !Has (ss, "buildingMaterial"))
					Fail ("Skin " + IntStr ((Int32) i + 1) + " needs 'thickness' (m) and 'buildingMaterial'.");
			}
			if (auto t = OptPositive (ss, "thickness")) c.fillThick = *t;
			if (auto bm = OptRef (API_BuildingMaterialID, ss, "buildingMaterial")) c.buildingMaterial = *bm;
			ApplyBit (c.flagBits, ss, "core", APICWallComp_Core);
			ApplyBit (c.flagBits, ss, "finish", APICWallComp_Finish);
			if (auto p = OptPen (ss, "endLinePen")) c.framePen = *p;
			newSkins.push_back (c);
		}
		// Face lines: [outer contour, separators..., inner contour] = skinCount + 1 entries.
		const Int32 n = (Int32) newSkins.size ();
		API_CWallLineComponent def = {};
		def.ltypeInd = 1;
		def.linePen = 1;
		std::vector<API_CWallLineComponent> newLines ((size_t) n + 1, def);
		if (!lines.empty ()) {
			newLines[0] = lines.front ();
			newLines[n] = lines.back ();
			for (Int32 i = 1; i < n; ++i)
				newLines[i] = i < (Int32) lines.size () - 1 ? lines[i] : (lines.size () >= 3 ? lines[lines.size () - 2] : lines.front ());
		}
		skins = newSkins;
		lines = newLines;
		changed = true;
	} else if (ctx.fresh) {
		Fail ("A composite needs 'skins': [{thickness, buildingMaterial, core?, finish?}] (outside to inside), or 'basedOn' an existing composite.");
	}

	if (Has (s, "skinLines")) {
		GS::Array<OS> specs = GetObjectArray (s, "skinLines");
		if (specs.GetSize () != lines.size ())
			Fail ("'skinLines' needs exactly skinCount + 1 = " + IntStr ((Int32) lines.size ()) +
				  " entries (outer contour, the separators between skins, inner contour).");
		for (UIndex i = 0; i < specs.GetSize (); ++i) {
			if (auto lt = OptRef (API_LinetypeID, specs[i], "lineType")) lines[i].ltypeInd = *lt;
			if (auto p = OptPen (specs[i], "pen")) lines[i].linePen = *p;
		}
		changed = true;
	}

	if (changed) {
		double total = 0.0;
		for (const API_CWallComponent& c : skins)
			total += c.fillThick;
		SetHandle (d.defs.cwall_compItems, VectorToHandle (skins));
		SetHandle (d.defs.cwall_compLItems, VectorToHandle (lines));
		cw.nComps = (short) skins.size ();
		cw.totalThick = total;
	}
}


void ClearTexture (API_Texture& t)
{
	if (t.fileLoc != nullptr) {
		delete t.fileLoc;
		t.fileLoc = nullptr;
	}
	BNZeroMemory (t.texName, sizeof (t.texName));
	t.status = 0;
	t.rotAng = 0.0;
	t.missingPict = false;
}


void ApplyTexture (API_Texture& t, const OS& tex, bool fresh)
{
	if (auto n = OptString (tex, "name")) {
		if (n->IsEmpty ())
			Fail ("texture.name must not be empty (use texture: false to remove the texture).");
		ToUBuf (t.texName, API_UniLongNameLen, ResolvePictureName (*n, "texture.name"), "texture.name");
		t.withoutExtension = true;		// stored like Archicad's own surfaces: library picture name without extension
		if (t.fileLoc != nullptr) {		// let Archicad look the new picture up in the loaded libraries
			delete t.fileLoc;
			t.fileLoc = nullptr;
		}
	} else if (fresh || t.texName[0] == 0) {
		Fail ("texture.name is required (an image name from the loaded libraries: search_library_parts {type: 'Picture', placeableOnly: false}).");
	}
	t.status = (short) (t.status | APITxtr_LinkMat);	// without this flag Archicad drops the texture
	if (auto v = OptPositive (tex, "width")) t.xSize = *v;
	if (auto v = OptPositive (tex, "height")) t.ySize = *v;
	if (!(t.xSize > 0.0)) t.xSize = 1.0;
	if (!(t.ySize > 0.0)) t.ySize = 1.0;
	if (auto v = OptAngle (tex, "angle")) t.rotAng = *v;
	ApplyBit (t.status, tex, "mirrorX", APITxtr_MirrorX);
	ApplyBit (t.status, tex, "mirrorY", APITxtr_MirrorY);
	ApplyBit (t.status, tex, "randomShift", APITxtr_RandomShift);
	ApplyBit (t.status, tex, "useAlphaChannel", APITxtr_UseAlpha);
	OS alpha;
	if (TryGetObject (tex, "alphaAffects", alpha)) {
		ApplyBit (t.status, alpha, "surfaceColor", APITxtr_SurfacePattern);
		ApplyBit (t.status, alpha, "ambientColor", APITxtr_AmbientPattern);
		ApplyBit (t.status, alpha, "specularColor", APITxtr_SpecularPattern);
		ApplyBit (t.status, alpha, "diffuseColor", APITxtr_DiffusePattern);
		ApplyBit (t.status, alpha, "bump", APITxtr_BumpPattern);
		ApplyBit (t.status, alpha, "transparency", APITxtr_TransPattern);
	}
	if (auto fit = OptString (tex, "fit")) {
		SetBits (t.status, APITxtr_FillRectNatur | APITxtr_FitPictNatur, false);
		if (EqualsIgnoreCase (*fit, "fillRectangle"))
			SetBits (t.status, APITxtr_FillRectNatur, true);
		else if (EqualsIgnoreCase (*fit, "fitPicture"))
			SetBits (t.status, APITxtr_FitPictNatur, true);
		else if (!EqualsIgnoreCase (*fit, "none"))
			Fail ("texture.fit must be 'none', 'fillRectangle' or 'fitPicture'.");
	}
}


void ApplySurface (API_Attribute& a, const OS& s, const ApplyCtx& ctx)
{
	API_MaterialType& m = a.material;
	if (auto v = OptNamed (kMaterialTypes, s, "materialType")) m.mtype = (API_MaterTypeID) *v;
	ApplyColor (m.surfaceRGB, s, "color");
	if (auto v = OptRange (s, "ambient", 0, 100)) m.ambientPc = (short) std::lround (*v);
	if (auto v = OptRange (s, "diffuse", 0, 100)) m.diffusePc = (short) std::lround (*v);
	if (auto v = OptRange (s, "specular", 0, 100)) m.specularPc = (short) std::lround (*v);
	if (auto v = OptRange (s, "transparency", 0, 100)) m.transpPc = (short) std::lround (*v);
	if (auto v = OptRange (s, "shininess", 0, 100)) m.shine = (short) std::lround (*v * 100.0);
	if (auto v = OptRange (s, "transparencyAttenuation", 0, 4)) m.transpAtt = (short) std::lround (*v * 100.0);
	if (auto v = OptRange (s, "emissionAttenuation", 0, 327)) m.emissionAtt = (short) std::lround (*v * 100.0);
	ApplyColor (m.specularRGB, s, "specularColor");
	ApplyColor (m.emissionRGB, s, "emissionColor");
	if (auto f = OptRefOrNone (API_FilltypeID, s, "fill")) m.ifill = *f;
	if (auto p = OptInt (s, "fillPen")) {
		if (*p > 255)
			Fail ("'fillPen' must be a pen index 1..255, or 0 to use the surface color.");
		m.fillCol = (short) std::max (*p, (Int32) 0);
	}
	if (s.Contains ("texture")) {
		if (s.IsBool ("texture")) {
			if (GetBool (s, "texture"))
				Fail ("'texture' must be an object {name, width, height, ...} or false to remove it.");
			ClearTexture (m.texture);
		} else {
			ApplyTexture (m.texture, GetObject (s, "texture"), ctx.fresh || m.texture.texName[0] == 0);
		}
	}
}


void ApplyLayerComb (API_Attribute& a, DefsExt& d, const OS& s, const ApplyCtx& ctx)
{
	LayerBase base = ctx.creating && !Has (s, "basedOn") ? LayerBase::Current : LayerBase::Unchanged;
	if (auto b = OptNamed (kLayerBases, s, "base"))
		base = (LayerBase) *b;
	if (base == LayerBase::Unchanged && !Has (s, "layers"))
		return;

	// Current state of every layer (flags + intersection group).
	std::map<API_AttributeIndex, API_LayerStat> current;
	ForEachAttr (API_LayerID, [&] (AttrData& l) {
		API_LayerStat st = {};
		st.lInd = l.Index ();
		st.lFlags = (short) (l.attr.header.flags & (APILay_Hidden | APILay_Locked | APILay_ForceToWire));
		st.conClassId = l.attr.layer.conClassId;
		current[st.lInd] = st;
		return true;
	});

	std::map<API_AttributeIndex, API_LayerStat> stats;
	if (base == LayerBase::Unchanged) {
		for (const API_LayerStat& st : HandleToVector (d.defs.layer_statItems, a.layerComb.lNumb)) {
			if (current.count (st.lInd) > 0)		// drop deleted layers
				stats[st.lInd] = st;
		}
	} else {
		for (const auto& [idx, cur] : current) {
			API_LayerStat st = cur;
			if (base == LayerBase::AllVisible)
				st.lFlags = 0;
			else if (base == LayerBase::AllHidden)
				st.lFlags = idx == 1 ? (short) 0 : (short) APILay_Hidden;
			stats[idx] = st;
		}
	}

	GS::Array<OS> layers = GetObjectArray (s, "layers", false);
	for (UIndex i = 0; i < layers.GetSize (); ++i) {
		const OS& ls = layers[i];
		std::vector<API_AttributeIndex> targets;
		if (Has (ls, "layer")) {
			targets.push_back (ResolveRef (API_LayerID, ls, "layer"));
		} else if (Has (ls, "match")) {
			const GS::UniString pattern = GetString (ls, "match");
			ForEachAttr (API_LayerID, [&] (AttrData& l) {
				if (NameMatches (pattern, l.name))
					targets.push_back (l.Index ());
				return true;
			});
			if (targets.empty ())
				Fail ("layers[" + IntStr ((Int32) i) + "]: no layer name matches " + Quote (pattern) + "." + ListHint (API_LayerID));
		} else {
			Fail ("layers[" + IntStr ((Int32) i) + "] needs 'layer' (name/index) or 'match' (name pattern).");
		}
		auto hidden = OptHidden (ls);
		for (API_AttributeIndex idx : targets) {
			auto it = stats.find (idx);
			if (it == stats.end ()) {		// not yet in the combination: layers missing from a combination are hidden
				API_LayerStat st = current.count (idx) > 0 ? current[idx] : API_LayerStat {};
				st.lInd = idx;
				st.lFlags = APILay_Hidden;
				it = stats.emplace (idx, st).first;
			}
			API_LayerStat& st = it->second;
			if (hidden.has_value ()) {
				if (idx == 1 && *hidden)
					Fail ("The Archicad layer (index 1) cannot be hidden.");
				SetBits (st.lFlags, APILay_Hidden, *hidden);
			}
			if (auto v = OptBool (ls, "locked")) {
				if (idx == 1 && *v)
					Fail ("The Archicad layer (index 1) cannot be locked.");
				SetBits (st.lFlags, APILay_Locked, *v);
			}
			ApplyBit (st.lFlags, ls, "wireframe", APILay_ForceToWire);
			if (auto g = OptInt (ls, "intersectionGroup")) {
				if (*g < 0)
					Fail ("'intersectionGroup' must be >= 0.");
				st.conClassId = *g;
			}
		}
	}

	std::vector<API_LayerStat> list;
	for (const auto& [idx, st] : stats)
		list.push_back (st);
	SetHandle (d.defs.layer_statItems, VectorToHandle (list));
	a.layerComb.lNumb = (Int32) list.size ();
}


bool ParseStampGuids (const char* unId, API_Guid& mainGuid, API_Guid& revGuid)
{
	// "{MAIN-GUID}-{REV-GUID}"
	const std::string s (unId);
	const size_t sep = s.find ("}-{");
	if (s.size () < 5 || s[0] != '{' || sep == std::string::npos)
		return false;
	const std::string mainStr = s.substr (1, sep - 1);
	std::string revStr = s.substr (sep + 3);
	if (!revStr.empty () && revStr.back () == '}')
		revStr.pop_back ();
	GS::Guid m, r;
	if (m.ConvertFromString (mainStr.c_str ()) != NoError || r.ConvertFromString (revStr.c_str ()) != NoError)
		return false;
	mainGuid = GSGuid2APIGuid (m);
	revGuid = GSGuid2APIGuid (r);
	return true;
}


void ApplyZoneCat (API_Attribute& a, DefsExt& d, const OS& s)
{
	API_ZoneCatType& z = a.zoneCat;
	if (auto c = OptString (s, "code"))
		ToUBuf (z.catCode, API_UniLongNameLen, *c, "code");
	ApplyColor (z.rgb, s, "color");
	if (Has (s, "stamp")) {
		API_LibPart lp = FindLibPart (s, "stamp", APILib_RoomID);
		API_Guid mainGuid = {}, revGuid = {};
		if (!ParseStampGuids (lp.ownUnID, mainGuid, revGuid))
			Fail ("Cannot read the unique ID of zone stamp '" + GS::UniString (lp.docu_UName) + "'.");
		BNZeroMemory (z.stampName, sizeof (z.stampName));
		for (USize i = 0; i < API_UniLongNameLen - 1 && lp.docu_UName[i] != 0; ++i)
			z.stampName[i] = lp.docu_UName[i];
		z.stampMainGuid = mainGuid;
		z.stampRevGuid = revGuid;
		ParamsHandle params;
		LoadDefaultParams (lp.index, params);
		SetHandle (d.defs.zone_addParItems, params.Release ());
	}
}


void ApplyProfile (API_Attribute& a, DefsExt& d, const OS& s, const ApplyCtx& ctx)
{
	API_ProfileAttrType& p = a.profile;
	OS usage;
	if (TryGetObject (s, "usage", usage)) {
		if (auto v = OptBool (usage, "walls")) p.wallType = *v;
		if (auto v = OptBool (usage, "beams")) p.beamType = *v;
		if (auto v = OptBool (usage, "columns")) p.coluType = *v;
		if (auto v = OptBool (usage, "handrails")) p.handrailType = *v;
		if (auto v = OptBool (usage, "otherObjects")) p.otherGDLObjectType = *v;
	} else if (ctx.fresh) {
		p.wallType = p.beamType = p.coluType = true;
	}
	if (Has (s, "shapes")) {
		SetProfileImage (d.defs, NewProfileImage (GetObjectArray (s, "shapes")));
	} else if (ctx.fresh) {
		Fail ("A profile needs 'shapes': [{polygon, buildingMaterial}] (or basedOn an existing profile).");
	}
}


void ApplyPenTable (DefsExt& d, const OS& s)
{
	GS::Array<OS> pens = GetObjectArray (s, "pens", false);
	const Int32 n = HandleCount (d.defs.penTable_Items);
	for (const OS& ps : pens) {
		const Int32 idx = GetInt (ps, "index");
		API_PenType* pen = nullptr;
		for (Int32 i = 0; i < n && pen == nullptr; ++i) {
			if ((*d.defs.penTable_Items)[i].head.index == idx)
				pen = &(*d.defs.penTable_Items)[i];
		}
		if (pen == nullptr && idx >= 1 && idx <= n)
			pen = &(*d.defs.penTable_Items)[idx - 1];
		if (pen == nullptr)
			Fail ("Pen index " + IntStr (idx) + " is not in this pen table (1.." + IntStr (n) + ").");
		ApplyPen (*pen, ps);
	}
}


void ApplyMEP (API_Attribute& a, const OS& s)
{
	API_MEPSystemType& m = a.mepSystem;
	if (auto v = OptBool (s, "ductwork")) m.isForDuctwork = *v;
	if (auto v = OptBool (s, "pipework")) m.isForPipework = *v;
	if (auto v = OptBool (s, "cabling")) m.isForCabling = *v;
	if (auto v = OptPen (s, "contourPen")) m.contourPen = *v;
	if (auto v = OptPen (s, "fillPen")) m.fillPen = *v;
	if (auto v = OptPen (s, "fillBackgroundPen", true)) m.fillBgPen = *v;
	if (auto v = OptPen (s, "centerLinePen")) m.centerLinePen = *v;
	if (auto v = OptRef (API_FilltypeID, s, "fill")) m.fillInd = *v;
	if (auto v = OptRef (API_LinetypeID, s, "centerLineType")) m.centerLTypeInd = *v;
	if (auto v = OptRef (API_MaterialID, s, "surface")) m.materialInd = *v;
	if (auto v = OptRef (API_MaterialID, s, "insulationSurface")) m.insulationMatInd = *v;
}


void ApplyOpProfile (API_Attribute& a, const OS& s)
{
	API_OperationProfileType& o = a.operationProfile;
	if (auto v = OptNamed (kOccupancyTypes, s, "occupancyType")) o.occupancyType = (API_OccupancyTypeID) *v;
	if (auto v = OptNonNegative (s, "hotWaterLoad")) o.hotWaterLoad = *v;
	if (auto v = OptNonNegative (s, "humanHeatGain")) o.humanHeatGain = *v;
	if (auto v = OptRange (s, "humidity", 0, 100)) o.humidity = *v;
}


void ApplyBuildingMaterial (AttrData& ad, const OS& s)
{
	API_BuildingMaterialType& b = ad.attr.buildingMaterial;
	if (auto v = OptString (s, "id")) ad.bmId = *v;
	if (auto v = OptString (s, "manufacturer")) ad.bmManufacturer = *v;
	if (auto v = OptString (s, "description")) ad.bmDescription = *v;
	if (auto v = OptRef (API_FilltypeID, s, "cutFill")) b.cutFill = *v;
	if (auto v = OptPen (s, "cutFillPen")) b.cutFillPen = *v;
	if (auto v = OptPen (s, "cutFillBackgroundPen", true)) b.cutFillBackgroundPen = *v;
	if (auto v = OptRef (API_MaterialID, s, "surface")) b.cutMaterial = *v;
	if (auto v = OptNamed (kFillOrientations, s, "cutFillOrientation")) b.cutFillOrientation = (API_FillOrientationID) *v;
	if (Has (s, "priority") && Has (s, "uiPriority"))
		Fail ("Give either 'priority' (stored value) or 'uiPriority', not both.");
	if (auto v = OptRange (s, "priority", 0, 100000)) b.connPriority = (Int32) std::lround (*v);
	if (auto v = OptRange (s, "uiPriority", 0, 999)) b.connPriority = UIToElemPriority ((Int32) std::lround (*v));
	if (auto v = OptNonNegative (s, "thermalConductivity")) b.thermalConductivity = *v;
	if (auto v = OptNonNegative (s, "density")) b.density = *v;
	if (auto v = OptNonNegative (s, "heatCapacity")) b.heatCapacity = *v;
	if (auto v = OptNonNegative (s, "embodiedEnergy")) b.embodiedEnergy = *v;
	if (auto v = OptNonNegative (s, "embodiedCarbon")) b.embodiedCarbon = *v;
	if (auto v = OptBool (s, "showUncutLines")) b.showUncutLines = *v;
	if (auto v = OptBool (s, "participatesInCollisionDetection")) b.doNotParticipateInCollDet = !*v;
}


void ApplyFields (AttrData& a, DefsExt& d, const OS& s, const ApplyCtx& ctx)
{
	switch (a.Type ()) {
		case API_PenID:					ApplyPen (a.attr.pen, s);				break;
		case API_LayerID:				ApplyLayer (a.attr, s, ctx);			break;
		case API_LinetypeID:			ApplyLine (a.attr, d, s, ctx);			break;
		case API_FilltypeID:			ApplyFill (a.attr, d, s, ctx);			break;
		case API_CompWallID:			ApplyComposite (a.attr, d, s, ctx);		break;
		case API_MaterialID:			ApplySurface (a.attr, s, ctx);			break;
		case API_LayerCombID:			ApplyLayerComb (a.attr, d, s, ctx);		break;
		case API_ZoneCatID:				ApplyZoneCat (a.attr, d, s);			break;
		case API_ProfileID:				ApplyProfile (a.attr, d, s, ctx);		break;
		case API_PenTableID:			ApplyPenTable (d, s);					break;
		case API_MEPSystemID:			ApplyMEP (a.attr, s);					break;
		case API_OperationProfileID:	ApplyOpProfile (a.attr, s);				break;
		case API_BuildingMaterialID:	ApplyBuildingMaterial (a, s);			break;
		default:																break;
	}
}

// =============================================================================
// Create / modify / delete
// =============================================================================

GS::UniString WriteHint (GSErrCode err)
{
	switch (err) {
		case APIERR_NOTMINE:
		case APIERR_NOACCESSRIGHT:
			return " In Teamwork, reserve the attribute (or get the create/modify right for this attribute type) first.";
		case APIERR_ATTREXIST:
			return " An attribute with this name already exists; choose another name.";
		case APIERR_NEEDSUNDOSCOPE:
			return " (internal: missing undo scope).";
		default:
			return GS::UniString ();
	}
}


OS ModifyIndex (API_AttrTypeID t, API_AttributeIndex idx, const OS& spec, bool fromCreate)
{
	const GS::UniString tname = TypeName (t);
	if (t == API_FontID)
		Fail ("Fonts cannot be modified through the API.");
	AttrData a;
	Check (a.Load (t, idx), "Cannot read " + tname + " " + IntStr (idx));
	DefsExt d;
	if (HasDefs (t))
		Check (d.Load (t, idx), "Cannot read the definition of " + tname + " " + Quote (a.name));

	if (t == API_PenID && Has (spec, "name"))
		Fail ("Pens have no names; set 'description' instead.");
	CheckFolderField (t, spec);
	if (!fromCreate) {
		if (auto n = OptString (spec, "name")) {
			if (n->IsEmpty ())
				Fail ("'name' must not be empty.");
			if (*n != a.name) {
				auto other = FindExact (t, *n);
				if (other.has_value () && *other != idx)
					Fail (tname + " named " + Quote (*n) + " already exists (index " + IntStr (*other) + ").", APIERR_ATTREXIST);
				if (t == API_LayerID && idx == 1)
					Fail ("The Archicad layer (index 1) cannot be renamed.");
				a.name = *n;
			}
		}
	}
	const GS::UniString nameForMsg = a.name;
	ApplyCtx ctx;
	ApplyFields (a, d, spec, ctx);
	a.PrepareForWrite ();

	GSErrCode err = (t == API_PenID) ? ACAPI_Attribute_Modify (&a.attr, nullptr) : ACAPI_Attribute_ModifyExt (&a.attr, &d.defs);
	if (err != NoError) {
		GS::UniString hint = WriteHint (err);
		if (t == API_LayerID && idx == 1)
			hint += " The Archicad layer (index 1) cannot be hidden, locked or renamed.";
		Fail ("Cannot modify " + tname + " " + Quote (nameForMsg) + ": " + ErrorName (err) + "." + hint, err);
	}
	if (Has (spec, "folder"))
		MoveToFolder (t, idx, GetPathField (spec, "folder"));
	return Ref (t, idx);
}


enum class CreateMode { Fresh, Template, BasedOnOnly, NotCreatable };

CreateMode CreateModeOf (API_AttrTypeID t)
{
	switch (t) {
		case API_PenID:
		case API_FontID:
			return CreateMode::NotCreatable;
		case API_BuildingMaterialID:
		case API_MaterialID:
		case API_ZoneCatID:
		case API_MEPSystemID:
		case API_PenTableID:
			return CreateMode::Template;
		case API_DimStandID:
		case API_ModelViewOptionsID:
		case API_OperationProfileID:
			return CreateMode::BasedOnOnly;
		default:
			return CreateMode::Fresh;
	}
}


// First percentage fill of the project (e.g. "25 %"), used as template for new percentage fills.
std::optional<API_AttributeIndex> PercentFillTemplate ()
{
	std::optional<API_AttributeIndex> found;
	ForEachAttr (API_FilltypeID, [&] (AttrData& a) {
		if (a.attr.filltype.subType == APIFill_Solid && a.attr.filltype.isPercentFill) {
			found = a.Index ();
			return false;
		}
		return true;
	});
	return found;
}


std::optional<API_AttributeIndex> TemplateIndex (API_AttrTypeID t)
{
	std::optional<API_AttributeIndex> found;
	if (t == API_PenTableID) {		// prefer the pen set in effect in the model
		ForEachAttr (t, [&] (AttrData& a) {
			if (a.attr.penTable.inEffectForModel) {
				found = a.Index ();
				return false;
			}
			return true;
		});
		if (found.has_value ())
			return found;
	}
	ForEachAttr (t, [&] (AttrData& a) {
		found = a.Index ();
		return false;
	});
	return found;
}


OS ExistingResult (API_AttrTypeID t, API_AttributeIndex idx, const GS::UniString& name, IfExists mode, const OS& spec)
{
	switch (mode) {
		case IfExists::Skip: {
			OS r = Ref (t, idx);
			r.Add ("existed", true);
			return r;
		}
		case IfExists::Update: {
			OS r = ModifyIndex (t, idx, spec, true);
			r.Add ("existed", true);
			r.Add ("updated", true);
			return r;
		}
		case IfExists::Error:
		default:
			Fail (TypeName (t) + " named " + Quote (name) + " already exists (index " + IntStr (idx) +
				  "). Pass ifExists: 'skip' to reuse it, 'update' to apply these settings to it, or choose another name.", APIERR_ATTREXIST);
	}
}


OS CreateOne (API_AttrTypeID t, const OS& spec, IfExists defaultMode)
{
	const GS::UniString tname = TypeName (t);
	CheckFields (t, spec, true);
	const CreateMode mode = CreateModeOf (t);
	if (mode == CreateMode::NotCreatable)
		Fail (t == API_PenID ? GS::UniString ("Pens cannot be created (there are always 255); change them with modify_pens.")
							 : GS::UniString ("Fonts cannot be created; they come from the operating system."));

	CheckFolderField (t, spec);
	const GS::UniString name = GetString (spec, "name");
	if (name.IsEmpty ())
		Fail ("'name' must not be empty.");
	if (name.GetLength () >= API_AttrNameLen)
		Fail ("'name' is too long (max 255 characters).");
	IfExists ifExists = defaultMode;
	if (auto m = OptNamed (kIfExists, spec, "ifExists"))
		ifExists = (IfExists) *m;

	if (auto existing = FindExact (t, name))
		return ExistingResult (t, *existing, name, ifExists, spec);

	AttrData a (t);
	DefsExt d;
	ApplyCtx ctx;
	ctx.creating = true;
	if (auto basedOn = OptRef (t, spec, "basedOn")) {
		Check (a.Load (t, *basedOn), "Cannot read the basedOn " + tname);
		if (HasDefs (t))
			Check (d.Load (t, *basedOn), "Cannot read the definition of the basedOn " + tname);
	} else if (mode == CreateMode::Template) {
		auto tpl = TemplateIndex (t);
		if (!tpl.has_value ())
			Fail ("The project has no " + tname + " to start from; pass 'basedOn'.");
		Check (a.Load (t, *tpl), "Cannot read the template " + tname);
		if (HasDefs (t))
			Check (d.Load (t, *tpl), "Cannot read the definition of the template " + tname);
		// Implicit template: keep graphic/technical defaults, drop identity-like data.
		if (t == API_BuildingMaterialID) {
			a.bmId.Clear ();
			a.bmManufacturer.Clear ();
			a.bmDescription.Clear ();
		} else if (t == API_MaterialID) {
			ClearTexture (a.attr.material.texture);
		} else if (t == API_ZoneCatID) {
			BNZeroMemory (a.attr.zoneCat.catCode, sizeof (a.attr.zoneCat.catCode));
		}
	} else if (auto percentTpl = (t == API_FilltypeID && Has (spec, "percent")) ? PercentFillTemplate () : std::nullopt) {
		// Percentage (screen) fills carry internal flags that the API does not expose: a fill built from
		// scratch never becomes a percentage fill, so start from an existing one (percent/pattern are replaced).
		Check (a.Load (t, *percentTpl), "Cannot read the percentage fill template");
		Check (d.Load (t, *percentTpl), "Cannot read the definition of the percentage fill template");
	} else if (mode == CreateMode::BasedOnOnly || (t == API_ProfileID && !Has (spec, "shapes"))) {
		Fail (tname + " attributes cannot be defined from scratch here; copy an existing one with duplicate_attributes (basedOn).");
	} else {
		ctx.fresh = true;
	}

	// New identity.
	a.attr.header.index = 0;
	a.attr.header.guid = APINULLGuid;
	a.attr.header.modiTime = 0;
	a.attr.header.flags = (short) (a.attr.header.flags & ~(APIAttr_IsFromGDL | APIAttr_IsClientCreated));
	BNZeroMemory (a.attr.header.name, sizeof (a.attr.header.name));
	if (t == API_LayerCombID)
		a.attr.layerComb.inEffect = false;
	if (t == API_PenTableID) {
		a.attr.penTable.inEffectForModel = false;
		a.attr.penTable.inEffectForLayout = false;
	}
	a.name = name;

	ApplyFields (a, d, spec, ctx);
	a.PrepareForWrite ();

	const GSErrCode err = ACAPI_Attribute_CreateExt (&a.attr, &d.defs);
	if (err == APIERR_ATTREXIST) {
		auto existing = FindByName (t, name);
		if (existing.has_value ())
			return ExistingResult (t, *existing, name, ifExists, spec);
	}
	if (err != NoError)
		Fail ("Cannot create " + tname + " " + Quote (name) + ": " + ErrorName (err) + "." + WriteHint (err), err);

	API_AttributeIndex idx = a.attr.header.index;
	if (idx <= 0) {
		auto found = FindExact (t, name);
		if (!found.has_value ())
			Fail (tname + " " + Quote (name) + " was created but cannot be found afterwards.");
		idx = *found;
	}
	if (Has (spec, "folder"))
		MoveToFolder (t, idx, GetPathField (spec, "folder"));
	OS r = Ref (t, idx);
	r.Add ("created", true);
	return r;
}


std::map<API_AttributeIndex, Int32> CountElementsPerLayer ()
{
	std::map<API_AttributeIndex, Int32> counts;
	GS::Array<API_Guid> all;
	if (ACAPI_Element_GetElemList (API_ElemType (API_ZombieElemID), &all) != NoError)
		return counts;
	for (const API_Guid& g : all) {
		API_Elem_Head head = {};
		head.guid = g;
		if (ACAPI_Element_GetHeader (&head) == NoError)
			counts[head.layer]++;
	}
	return counts;
}


std::optional<API_AttributeIndex> ActiveLayerCombination ()
{
	API_AttributeIndex idx = 0;
	if (ACAPI_Environment (APIEnv_GetCurrLayerCombID, &idx) == NoError && idx > 0) {
		AttrData a;
		if (a.Load (API_LayerCombID, idx) == NoError)
			return idx;
	}
	std::optional<API_AttributeIndex> found;
	ForEachAttr (API_LayerCombID, [&] (AttrData& a) {
		if (a.attr.layerComb.inEffect) {
			found = a.Index ();
			return false;
		}
		return true;
	});
	return found;
}


void AddActiveLayerCombination (OS& out)
{
	if (auto idx = ActiveLayerCombination ())
		out.Add ("activeLayerCombination", Ref (API_LayerCombID, *idx));
	else
		out.Add ("customLayerSettings", true);		// the current layer states match no saved combination
}


IfExists ParseIfExists (const OS& p)
{
	if (auto m = OptNamed (kIfExists, p, "ifExists"))
		return (IfExists) *m;
	return IfExists::Error;
}

// =============================================================================
// Commands
// =============================================================================

OS GetAttributesCmd (const OS& p)
{
	if (!Has (p, "type")) {
		GS::Array<OS> counts;
		for (API_AttrTypeID t : AllAttrTypes ()) {
			Int32 n = 0;
			ForEachAttr (t, [&] (AttrData&) { ++n; return true; });
			counts.Push (OS ("type", TypeName (t), "count", n));
		}
		OS out ("types", counts);
		out.Add ("hint", GS::UniString ("Pass type (and optionally nameFilter / detailed: true) to list the attributes of one type."));
		return out;
	}

	const API_AttrTypeID t = GetAttrType (p, "type");
	SerializeCtx ctx;
	ctx.detailed = GetBool (p, "detailed", false);
	ctx.folders = LoadFolderMap (t);
	const GS::UniString filter = GetString (p, "nameFilter", GS::UniString ());
	const Int32 offset = std::max (GetInt (p, "offset", 0), (Int32) 0);
	const Int32 limit = std::clamp (GetInt (p, "limit", 1000), (Int32) 1, (Int32) 5000);

	OS out ("type", TypeName (t));
	GS::Array<OS> items;
	if (Has (p, "attributes")) {
		for (const OS& ref : GetRefList (p, "attributes")) {
			items.Push (Try ([&] () -> OS {
				const API_AttributeIndex idx = ResolveRef (t, ref, "attribute");
				AttrData a;
				Check (a.Load (t, idx), "Cannot read " + TypeName (t) + " " + IntStr (idx));
				return SerializeAttr (a, ctx);
			}));
		}
		out.Add ("attributes", items);
	} else {
		Int32 total = 0;
		ForEachAttr (t, [&] (AttrData& a) {
			if (!NameMatches (filter, a.name))
				return true;
			if (total >= offset && (Int32) items.GetSize () < limit)
				items.Push (SerializeAttr (a, ctx));
			++total;
			return true;
		});
		out.Add ("total", total);
		out.Add ("offset", offset);
		out.Add ("returned", (Int32) items.GetSize ());
		out.Add ("hasMore", offset + (Int32) items.GetSize () < total);
		out.Add ("attributes", items);
	}
	if (t == API_LayerID || t == API_LayerCombID)
		AddActiveLayerCombination (out);
	if (t == API_PenID) {
		API_AttributeIndex penSet = 0;
		if (ACAPI_Environment (APIEnv_GetCurrPenSetID, &penSet) == NoError && penSet > 0)
			out.Add ("activePenTable", Ref (API_PenTableID, penSet));
	}
	return out;
}


OS CreateAttributesCmd (const OS& p)
{
	const API_AttrTypeID t = GetAttrType (p, "type");
	GS::Array<OS> specs = GetObjectArray (p, "attributes");
	if (specs.IsEmpty ())
		Fail ("'attributes' must contain at least one item.");
	const IfExists ifExists = ParseIfExists (p);
	GS::Array<OS> results;
	Undoable (GetString (p, "undoName", "Create " + TypeName (t) + " attributes (Claude)"), [&] {
		for (const OS& spec : specs)
			results.Push (Try ([&] { return CreateOne (t, spec, ifExists); }));
	});
	return OS ("results", results);
}


OS ModifyAttributesCmd (const OS& p)
{
	std::optional<API_AttrTypeID> defaultType;
	if (Has (p, "type"))
		defaultType = GetAttrType (p, "type");
	GS::Array<OS> items = GetObjectArray (p, "attributes");
	if (items.IsEmpty ())
		Fail ("'attributes' must contain at least one item.");
	GS::Array<OS> results;
	Undoable (GetString (p, "undoName", "Modify attributes (Claude)"), [&] {
		for (const OS& item : items) {
			results.Push (Try ([&] {
				API_AttrTypeID t = API_ZombieAttrID;
				if (Has (item, "type"))
					t = GetAttrType (item, "type");
				else if (defaultType.has_value ())
					t = *defaultType;
				else
					Fail ("Each item needs 'type' (or give a top-level 'type').");
				CheckFields (t, item, false);
				const API_AttributeIndex idx = ResolveRef (t, item, "attribute");
				return ModifyIndex (t, idx, item, false);
			}));
		}
	});
	return OS ("results", results);
}


OS DeleteAttributesCmd (const OS& p)
{
	const API_AttrTypeID t = GetAttrType (p, "type");
	if (t == API_PenID || t == API_FontID)
		Fail (TypeName (t) + " attributes cannot be deleted.");
	const bool force = GetBool (p, "force", false);
	GS::Array<OS> refs = GetRefList (p, "attributes");
	if (refs.IsEmpty ())
		Fail ("'attributes' must list at least one attribute (index, name or {guid}).");
	std::optional<std::map<API_AttributeIndex, Int32>> layerUse;
	GS::Array<OS> results;
	Undoable (GetString (p, "undoName", "Delete " + TypeName (t) + " attributes (Claude)"), [&] {
		for (const OS& ref : refs) {
			results.Push (Try ([&] {
				const API_AttributeIndex idx = ResolveRef (t, ref, "attribute");
				OS r = Ref (t, idx);
				Int32 elementsOnLayer = 0;
				if (t == API_LayerID) {
					if (idx == 1)
						Fail ("The Archicad layer (index 1) cannot be deleted.");
					if (!layerUse.has_value ())
						layerUse = CountElementsPerLayer ();
					auto it = layerUse->find (idx);
					elementsOnLayer = it != layerUse->end () ? it->second : 0;
					if (elementsOnLayer > 0 && !force)
						Fail ("Layer " + Quote (DisplayName (API_LayerID, idx)) + " holds " + IntStr (elementsOnLayer) +
							  " element(s) that would be DELETED with it. Move them to another layer first (modify_elements {layer}) or pass force: true.");
				}
				API_Attr_Head head = {};
				head.typeID = t;
				head.index = idx;
				const GSErrCode err = ACAPI_Attribute_Delete (head);
				if (err != NoError) {
					GS::UniString hint = WriteHint (err);
					if (err == APIERR_BADINDEX || err == APIERR_REFUSEDCMD || err == APIERR_GENERAL)
						hint += " Some attributes cannot be deleted (e.g. the last one of its type, the Solid/Empty fills, line type 1).";
					Fail ("Cannot delete " + TypeName (t) + " " + IntStr (idx) + ": " + ErrorName (err) + "." + hint, err);
				}
				r.Add ("deleted", true);
				if (elementsOnLayer > 0)
					r.Add ("elementsDeleted", elementsOnLayer);
				return r;
			}));
		}
	});
	return OS ("results", results);
}


OS ModifyPensCmd (const OS& p)
{
	GS::Array<OS> pens = GetObjectArray (p, "pens");
	if (pens.IsEmpty ())
		Fail ("'pens' must contain at least one {index, color?, width?, description?} item.");
	std::optional<API_AttributeIndex> penTable = OptRef (API_PenTableID, p, "penTable");
	GS::Array<OS> results;
	OS out;
	Undoable (GetString (p, "undoName", "Modify pens (Claude)"), [&] {
		if (penTable.has_value ()) {
			AttrData a;
			Check (a.Load (API_PenTableID, *penTable), "Cannot read the pen table");
			DefsExt d;
			Check (d.Load (API_PenTableID, *penTable), "Cannot read the pens of the pen table");
			const Int32 n = HandleCount (d.defs.penTable_Items);
			bool any = false;
			for (const OS& ps : pens) {
				results.Push (Try ([&] {
					OS one ("pens", GS::Array<OS> { ps });
					ApplyPenTable (d, one);
					const Int32 idx = GetInt (ps, "index");
					for (Int32 i = 0; i < n; ++i) {
						if ((*d.defs.penTable_Items)[i].head.index == idx)
							return PenJson ((*d.defs.penTable_Items)[i]);
					}
					return PenJson ((*d.defs.penTable_Items)[idx - 1]);
				}));
				any = any || !results.GetLast ().Contains ("error");
			}
			if (any) {
				a.PrepareForWrite ();
				const GSErrCode err = ACAPI_Attribute_ModifyExt (&a.attr, &d.defs);
				if (err != NoError)
					Fail ("Cannot modify pen table " + Quote (a.name) + ": " + ErrorName (err) + "." + WriteHint (err), err);
			}
			out.Add ("penTable", Ref (API_PenTableID, *penTable));
		} else {
			for (const OS& ps : pens) {
				results.Push (Try ([&] {
					const Int32 idx = GetInt (ps, "index");
					if (idx < 1 || idx > 255)
						Fail ("Pen index must be 1..255 (got " + IntStr (idx) + ").", APIERR_BADINDEX);
					AttrData a;
					Check (a.Load (API_PenID, idx), "Cannot read pen " + IntStr (idx));
					ApplyPen (a.attr.pen, ps);
					a.PrepareForWrite ();
					const GSErrCode err = ACAPI_Attribute_Modify (&a.attr, nullptr);
					if (err != NoError)
						Fail ("Cannot modify pen " + IntStr (idx) + ": " + ErrorName (err) + "." + WriteHint (err), err);
					AttrData after;
					if (after.Load (API_PenID, idx) == NoError)
						return PenJson (after.attr.pen);
					return PenJson (a.attr.pen);
				}));
			}
			API_AttributeIndex penSet = 0;
			if (ACAPI_Environment (APIEnv_GetCurrPenSetID, &penSet) == NoError && penSet > 0)
				out.Add ("activePenTable", Ref (API_PenTableID, penSet));
		}
	});
	out.Add ("results", results);
	return out;
}


OS ApplyLayerCombinationCmd (const OS& p)
{
	API_AttributeIndex idx = ResolveRef (API_LayerCombID, p, "layerCombination");
	GSErrCode err = ACAPI_Environment (APIEnv_ChangeCurrLayerCombID, &idx);
	if (err == APIERR_NEEDSUNDOSCOPE) {
		Undoable ("Apply layer combination (Claude)", [&] {
			Check (ACAPI_Environment (APIEnv_ChangeCurrLayerCombID, &idx), "Cannot apply the layer combination");
		});
	} else if (err != NoError) {
		GS::UniString hint = err == APIERR_BADWINDOW || err == APIERR_BADDATABASE
			? GS::UniString (" Activate a model window (floor plan, section, 3D) first.") : GS::UniString ();
		Fail ("Cannot apply layer combination " + Quote (DisplayName (API_LayerCombID, idx)) + ": " + ErrorName (err) + "." + hint, err);
	}
	OS out ("ok", true);
	out.Add ("layerCombination", Ref (API_LayerCombID, idx));
	AddActiveLayerCombination (out);
	return out;
}


OS SetLayerStatesCmd (const OS& p)
{
	GS::Array<OS> items = GetObjectArray (p, "layers");
	if (items.IsEmpty ())
		Fail ("'layers' must contain at least one {layer | match, hidden|visible, locked, wireframe, intersectionGroup} item.");
	GS::Array<OS> results;
	Undoable (GetString (p, "undoName", "Set layer states (Claude)"), [&] {
		for (const OS& item : items) {
			results.Push (Try ([&] {
				static const char* const kAllowed[] = { "layer", "match", "hidden", "visible", "locked", "wireframe", "intersectionGroup" };
				for (const GS::String& field : item.GetFieldNames ()) {
					bool ok = false;
					for (const char* k : kAllowed)
						ok = ok || field == k;
					if (!ok)
						Fail ("Unknown field '" + GS::UniString (field.ToCStr ()) + "'. Allowed: layer, match, hidden, visible, locked, wireframe, intersectionGroup.");
				}
				const auto hidden = OptHidden (item);
				const auto locked = OptBool (item, "locked");
				const auto wire = OptBool (item, "wireframe");
				const auto group = OptInt (item, "intersectionGroup");
				if (!hidden && !locked && !wire && !group)
					Fail ("Nothing to change: give hidden/visible, locked, wireframe or intersectionGroup.");
				if (group.has_value () && *group < 0)
					Fail ("'intersectionGroup' must be >= 0.");

				std::vector<API_AttributeIndex> targets;
				bool byPattern = false;
				if (Has (item, "layer")) {
					targets.push_back (ResolveRef (API_LayerID, item, "layer"));
				} else if (Has (item, "match")) {
					byPattern = true;
					const GS::UniString pattern = GetString (item, "match");
					ForEachAttr (API_LayerID, [&] (AttrData& l) {
						if (NameMatches (pattern, l.name))
							targets.push_back (l.Index ());
						return true;
					});
					if (targets.empty ())
						Fail ("No layer name matches " + Quote (pattern) + "." + ListHint (API_LayerID));
				} else {
					Fail ("Give 'layer' (name/index) or 'match' (name pattern, '*' = all layers).");
				}

				GS::Array<OS> changed, skipped;
				Int32 unchanged = 0;
				for (API_AttributeIndex idx : targets) {
					AttrData a;
					Check (a.Load (API_LayerID, idx), "Cannot read layer " + IntStr (idx));
					if (idx == 1 && ((hidden.has_value () && *hidden) || (locked.has_value () && *locked))) {
						if (!byPattern)
							Fail ("The Archicad layer (index 1) cannot be hidden or locked.");
						skipped.Push (OS ("index", (Int32) idx, "name", a.name, "reason", GS::UniString ("the Archicad layer cannot be hidden or locked")));
						continue;
					}
					const short flagsBefore = a.attr.header.flags;
					const Int32 groupBefore = a.attr.layer.conClassId;
					if (hidden.has_value ()) SetBits (a.attr.header.flags, APILay_Hidden, *hidden);
					if (locked.has_value ()) SetBits (a.attr.header.flags, APILay_Locked, *locked);
					if (wire.has_value ()) SetBits (a.attr.header.flags, APILay_ForceToWire, *wire);
					if (group.has_value ()) a.attr.layer.conClassId = *group;
					if (a.attr.header.flags == flagsBefore && a.attr.layer.conClassId == groupBefore) {
						++unchanged;
						continue;
					}
					a.PrepareForWrite ();
					const GSErrCode err = ACAPI_Attribute_Modify (&a.attr, nullptr);
					if (err != NoError) {
						if (!byPattern)
							Fail ("Cannot change layer " + Quote (a.name) + ": " + ErrorName (err) + "." + WriteHint (err), err);
						skipped.Push (OS ("index", (Int32) idx, "name", a.name, "reason", ErrorName (err)));
						continue;
					}
					changed.Push (OS ("index", (Int32) idx, "name", a.name));
				}
				OS r ("changed", changed);
				r.Add ("changedCount", (Int32) changed.GetSize ());
				r.Add ("alreadyInStateCount", unchanged);
				if (!skipped.IsEmpty ())
					r.Add ("skipped", skipped);
				return r;
			}));
		}
	});
	OS out ("results", results);
	AddActiveLayerCombination (out);
	return out;
}

} // namespace
} // namespace attr


void RegisterAttributeCommands ()
{
	using namespace attr;

	RegisterCommand ("GetAttributes",
		"Lists attributes of one type with type-specific details. Input: {type: 'Pen'|'Layer'|'Line'|'Fill'|'Composite'|'Surface'|"
		"'LayerCombination'|'ZoneCategory'|'Font'|'Profile'|'PenTable'|'DimensionStandard'|'ModelViewOption'|'MEPSystem'|"
		"'OperationProfile'|'BuildingMaterial', attributes?: [{index}|{name}|{guid}], nameFilter?: substring or wildcard pattern, "
		"detailed?: bool (adds skins, layer states, dash/hatch definitions, pen table pens...), offset?, limit? (default 1000)}. "
		"Without type: attribute counts per type.",
		GetAttributesCmd);

	RegisterCommand ("CreateAttributes",
		"Creates attributes of one type in one undo step. Input: {type, attributes: [{name, basedOn?: ref (copy this attribute), "
		"folder?: ['A','B'], ifExists?, ...type fields}], ifExists?: 'error'(default)|'skip'|'update'}. Output: {results: [{index, name, guid, "
		"created|existed} | {error}]}. Creatable from fields: Layer, LayerCombination, Line, Fill (Solid/Empty/Vector/Image/gradients), "
		"Composite, Surface, BuildingMaterial, ZoneCategory, MEPSystem, PenTable, Profile (shapes); others only with basedOn.",
		CreateAttributesCmd);

	RegisterCommand ("ModifyAttributes",
		"Changes attributes in one undo step. Input: {type?, attributes: [{type?, attribute: ref, name?: new name, folder?, "
		"...type fields (same names as CreateAttributes/GetAttributes)}]}. Output: {results: [{index, name, guid} | {error}]}.",
		ModifyAttributesCmd);

	RegisterCommand ("DeleteAttributes",
		"Deletes attributes in one undo step. Input: {type, attributes: [ref], force?: bool (required to delete layers that still hold "
		"elements — those elements are deleted too)}. Output: {results: [{index, name, deleted} | {error}]}.",
		DeleteAttributesCmd);

	RegisterCommand ("ModifyPens",
		"Changes pen color/width/description. Input: {penTable?: ref (default: the pens in effect in the model), pens: [{index 1..255, "
		"color?: '#RRGGBB', width?: mm, description?}]}. Output: {results: [{index, color, width} | {error}]}.",
		ModifyPensCmd);

	RegisterCommand ("ApplyLayerCombination",
		"Makes a layer combination the active layer settings of the current window. Input: {layerCombination: ref}.",
		ApplyLayerCombinationCmd);

	RegisterCommand ("SetLayerStates",
		"Shows/hides/locks/unlocks layers, switches wireframe and intersection groups, in order, in one undo step. Input: "
		"{layers: [{layer: ref | match: 'pattern*', hidden?|visible?, locked?, wireframe?, intersectionGroup?}]}.",
		SetLayerStatesCmd);
}

} // namespace cc
