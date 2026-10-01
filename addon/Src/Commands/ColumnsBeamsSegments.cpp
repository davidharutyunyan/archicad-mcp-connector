// ColumnsBeamsSegments — shared assembly-segment helpers for the Column / Beam adapters.

#include "Commands/ColumnsBeamsSegments.hpp"

#include <cmath>

namespace cc {
namespace cb {

// --- Enum tables ----------------------------------------------------------------------

// 3 x 3 anchor grid as shown in the Settings dialogs: row 0 = top (column: +y side on plan,
// beam: top of the cross-section), column 0 = left.
const NamedValue kAnchors[9] = {
	{ "TopLeft",		0 },
	{ "TopCenter",		1 },
	{ "TopRight",		2 },
	{ "MiddleLeft",		3 },
	{ "Center",			4 },
	{ "MiddleRight",	5 },
	{ "BottomLeft",		6 },
	{ "BottomCenter",	7 },
	{ "BottomRight",	8 },
};

const NamedValue kZoneRelations[3] = {
	{ "Boundary",		APIZRel_Boundary },
	{ "ReduceArea",		APIZRel_ReduceArea },
	{ "None",			APIZRel_None },
};

const NamedValue kDisplayOptions[6] = {
	{ "Projected",				API_Standard },
	{ "ProjectedWithOverhead",	API_StandardWithAbstract },
	{ "CutOnly",				API_CutOnly },
	{ "OutlinesOnly",			API_OutLinesOnly },
	{ "OverheadAll",			API_AbstractAll },
	{ "SymbolicCut",			API_CutAll },
};

const NamedValue kViewDepthLimitations[3] = {
	{ "ToFloorPlanRange",	API_ToFloorPlanRange },
	{ "ToAbsoluteLimit",	API_ToAbsoluteLimit },
	{ "EntireElement",		API_EntireElement },
};

const NamedValue kCutTypes[3] = {
	{ "Horizontal",		APIAssemblySegmentCut_Horizontal },
	{ "Vertical",		APIAssemblySegmentCut_Vertical },
	{ "Custom",			APIAssemblySegmentCut_Custom },
};

namespace {

const NamedValue kShapes[] = {
	{ "Rectangular",	0 },
	{ "Circular",		1 },
};

} // namespace

const SectionKeys kColumnSectionKeys = { "depth", "endDepth", APICSect_Poly, APICSect_Normal, true };
const SectionKeys kBeamSectionKeys = { "height", "endHeight", APIBSect_Poly, APIBSect_Normal, false };

// --- Error context ----------------------------------------------------------------------

void WithContext (const GS::UniString& context, const std::function<void ()>& fn)
{
	try {
		fn ();
	} catch (const Error& e) {
		throw Error (context + ": " + e.message, e.code);
	}
}


GS::UniString IndexedName (const char* arrayName, UInt32 index)
{
	return GS::UniString (arrayName) + "[" + GS::ValueToUniString ((Int32) index) + "]";
}


GS::UniString Num (double value)
{
	// GS::UniString::Printf is static (an instance call would discard the result).
	return GS::UniString::Printf ("%.3f", value);
}

// --- Small field readers ------------------------------------------------------------------

static GS::UniString AllowedAnchors ()
{
	GS::UniString allowed;
	for (const NamedValue& nv : kAnchors) {
		if (!allowed.IsEmpty ()) allowed += ", ";
		allowed += nv.name;
	}
	return allowed;
}


std::optional<short> OptAnchor (const OS& spec, const char* key)
{
	if (!spec.Contains (key))
		return std::nullopt;
	if (IsNumber (spec, key)) {
		const Int32 v = GetInt (spec, key);
		if (v < 0 || v > 8)
			Fail ("'" + GS::UniString (key) + "' must be 0..8 or one of " + AllowedAnchors () + ".");
		return (short) v;
	}
	return (short) ParseNamed (kAnchors, spec, key);
}


GS::UniString AnchorName (short anchor)
{
	return NameOf (kAnchors, anchor);
}


std::optional<double> OptPositive (const OS& spec, const char* key)
{
	auto v = OptDouble (spec, key);
	if (v.has_value () && !(*v > kEps))
		Fail ("'" + GS::UniString (key) + "' must be greater than 0 (meters).");
	return v;
}


std::optional<double> OptNonNegative (const OS& spec, const char* key)
{
	auto v = OptDouble (spec, key);
	if (v.has_value () && *v < 0.0)
		Fail ("'" + GS::UniString (key) + "' must not be negative (meters).");
	return v;
}


bool ApplyStoryVisibility (const OS& spec, const char* key, API_StoryVisibility& visibility, bool& isAuto)
{
	if (!spec.Contains (key))
		return false;
	const GS::UniString keyName (key);
	if (spec.IsString (key)) {
		const GS::UniString mode = GetString (spec, key);
		if (EqualsIgnoreCase (mode, "Auto")) {
			isAuto = true;
		} else if (EqualsIgnoreCase (mode, "HomeOnly")) {
			isAuto = false;
			visibility.showOnHome = true;
			visibility.showAllAbove = visibility.showAllBelow = false;
			visibility.showRelAbove = visibility.showRelBelow = 0;
		} else if (EqualsIgnoreCase (mode, "AllStories")) {
			isAuto = false;
			visibility.showOnHome = visibility.showAllAbove = visibility.showAllBelow = true;
			visibility.showRelAbove = visibility.showRelBelow = 0;
		} else {
			Fail ("'" + keyName + "' must be \"Auto\", \"HomeOnly\", \"AllStories\" or {homeStory, allAbove, allBelow, storiesAbove, storiesBelow}.");
		}
		return true;
	}
	OS o = GetObject (spec, key);
	isAuto = false;
	if (auto b = OptBool (o, "homeStory"))	visibility.showOnHome = *b;
	if (auto b = OptBool (o, "allAbove"))	visibility.showAllAbove = *b;
	if (auto b = OptBool (o, "allBelow"))	visibility.showAllBelow = *b;
	if (auto n = OptInt (o, "storiesAbove")) {
		if (*n < 0) Fail ("'" + keyName + ".storiesAbove' must be >= 0.");
		visibility.showRelAbove = (short) *n;
	}
	if (auto n = OptInt (o, "storiesBelow")) {
		if (*n < 0) Fail ("'" + keyName + ".storiesBelow' must be >= 0.");
		visibility.showRelBelow = (short) *n;
	}
	return true;
}


void AddStoryVisibilityJson (OS& out, const char* key, const API_StoryVisibility& visibility, bool isAuto)
{
	if (isAuto) {
		out.Add (key, GS::UniString ("Auto"));
		return;
	}
	out.Add (key, OS ("homeStory", visibility.showOnHome,
					  "allAbove", visibility.showAllAbove,
					  "allBelow", visibility.showAllBelow,
					  "storiesAbove", (Int32) visibility.showRelAbove,
					  "storiesBelow", (Int32) visibility.showRelBelow));
}

// --- Floor plan attributes -------------------------------------------------------------------

std::optional<short> OptPen (const OS& spec, const char* key)
{
	auto v = OptInt (spec, key);
	if (v.has_value () && (*v < 1 || *v > 255))
		Fail ("'" + GS::UniString (key) + "' must be a pen index 1..255.");
	if (!v.has_value ())
		return std::nullopt;
	return (short) *v;
}


bool ApplyLineStyle (const OS& lines, const char* key, short* pen, API_AttributeIndex* lineType)
{
	if (!lines.Contains (key))
		return false;
	OS style = GetObject (lines, key);
	WithContext (GS::UniString ("lines.") + key, [&] () {
		if (auto p = OptPen (style, "pen")) {
			if (pen == nullptr) Fail ("'pen' is not available here.");
			*pen = *p;
		}
		if (style.Contains ("lineType")) {
			if (lineType == nullptr) Fail ("'lineType' is not available here (only 'pen').");
			*lineType = GetAttr (API_LinetypeID, style, "lineType");
		}
	});
	return true;
}


OS LineStyleJson (const short* pen, const API_AttributeIndex* lineType)
{
	OS out;
	if (pen != nullptr)
		out.Add ("pen", (Int32) *pen);
	if (lineType != nullptr)
		out.Add ("lineType", AttrRef (API_LinetypeID, *lineType));
	return out;
}


static bool ApplyOnePenOverride (const OS& spec, const char* key, bool& overridden, short& pen)
{
	if (!spec.Contains (key))
		return false;
	if (spec.IsBool (key)) {
		if (GetBool (spec, key))
			Fail ("'" + GS::UniString (key) + "' must be a pen index 1..255, or false to use the pen of the structure.");
		overridden = false;
		return true;
	}
	pen = *OptPen (spec, key);
	overridden = true;
	return true;
}


bool ApplyPenOverride (const OS& spec, API_PenOverrideType& po)
{
	bool changed = ApplyOnePenOverride (spec, "cutFillPen", po.overrideCutFillPen, po.cutFillPen);
	changed |= ApplyOnePenOverride (spec, "cutFillBackgroundPen", po.overrideCutFillBackgroundPen, po.cutFillBackgroundPen);
	return changed;
}


void AddPenOverrideJson (OS& out, const API_PenOverrideType& po)
{
	if (po.overrideCutFillPen)
		out.Add ("cutFillPen", (Int32) po.cutFillPen);
	if (po.overrideCutFillBackgroundPen)
		out.Add ("cutFillBackgroundPen", (Int32) po.cutFillBackgroundPen);
}


bool ApplyCoverFill (const OS& spec, const CoverFillRefs& r)
{
	if (!spec.Contains ("coverFill"))
		return false;
	if (spec.IsBool ("coverFill")) {
		r.use = GetBool (spec, "coverFill");
		return true;
	}
	OS cf = GetObject (spec, "coverFill");
	WithContext ("coverFill", [&] () {
		if (auto b = OptBool (cf, "enabled"))			r.use = *b;
		if (auto f = OptAttr (API_FilltypeID, cf, "fill"))	{ r.fill = *f; if (!cf.Contains ("enabled")) r.use = true; }
		if (auto p = OptPen (cf, "pen"))				r.pen = *p;
		if (auto p = OptPen (cf, "backgroundPen"))		r.backgroundPen = *p;
		if (auto b = OptBool (cf, "fromSurface"))		r.fromSurface = *b;
		if (auto b = OptBool (cf, "orientationFrom3D"))	r.orientationFrom3D = *b;
	});
	return true;
}


void AddCoverFillJson (OS& out, bool use, bool fromSurface, bool orientationFrom3D, API_AttributeIndex fill, short pen, short backgroundPen)
{
	OS cf ("enabled", use);
	if (use) {
		cf.Add ("fromSurface", fromSurface);
		cf.Add ("fill", AttrRef (API_FilltypeID, fill));
		cf.Add ("pen", (Int32) pen);
		cf.Add ("backgroundPen", (Int32) backgroundPen);
		cf.Add ("orientationFrom3D", orientationFrom3D);
	}
	out.Add ("coverFill", cf);
}

// --- Section --------------------------------------------------------------------------------

bool HasSectionFields (const OS& spec, const SectionKeys& keys)
{
	static const char* const kKeys[] = { "shape", "width", "diameter", "tapered", "endWidth", "endDiameter",
										 "buildingMaterial", "profile", "composite" };
	for (const char* key : kKeys) {
		if (spec.Contains (key))
			return true;
	}
	return spec.Contains (keys.depth) || spec.Contains (keys.endDepth);
}


static void CheckProfileUsage (API_AttributeIndex index, const SectionKeys& keys)
{
	API_Attribute attr;
	BNZeroMemory (&attr, sizeof (attr));
	attr.header.typeID = API_ProfileID;
	attr.header.index = index;
	if (ACAPI_Attribute_Get (&attr) != NoError)
		return;		// existence was already checked by GetAttr
	const bool usable = keys.isColumn ? attr.profile.coluType : attr.profile.beamType;
	if (!usable) {
		Fail ("Complex profile '" + AttrName (API_ProfileID, index) +
			  "' is not enabled for " + (keys.isColumn ? "columns" : "beams") +
			  ". Pick a profile whose usage includes " + (keys.isColumn ? "Column" : "Beam") + " (see get_attributes type Profile), or enable it in the Profile Manager.",
			  APIERR_BADPARS);
	}
}


void ApplySection (API_AssemblySegmentData& d, const OS& spec, const SectionKeys& k)
{
	if (!HasSectionFields (spec, k))
		return;

	if (spec.Contains ("composite"))
		Fail ("Columns and beams cannot use a composite. Use 'buildingMaterial' (basic structure) or 'profile' (complex profile).");
	if (spec.Contains ("buildingMaterial") && spec.Contains ("profile"))
		Fail ("Give either 'buildingMaterial' (basic structure) or 'profile' (complex profile), not both.");

	// Structure
	if (auto bm = OptAttr (API_BuildingMaterialID, spec, "buildingMaterial")) {
		d.modelElemStructureType = API_BasicStructure;
		d.buildingMaterial = *bm;
		if (d.profileType == k.polyProfileType)
			d.profileType = k.normalProfileType;
	}
	if (spec.Contains ("profile")) {
		const API_AttributeIndex profile = GetAttr (API_ProfileID, spec, "profile");
		CheckProfileUsage (profile, k);
		d.modelElemStructureType = API_ProfileStructure;
		d.profileAttr = profile;
		d.profileType = k.polyProfileType;
		d.circleBased = false;
	}

	// Shape
	std::optional<bool> circular;
	if (spec.Contains ("shape"))
		circular = ParseNamed (kShapes, spec, "shape") == 1;
	const bool hasDiameter = spec.Contains ("diameter") || spec.Contains ("endDiameter");
	const bool hasRectSizes = spec.Contains ("width") || spec.Contains (k.depth) || spec.Contains ("endWidth") || spec.Contains (k.endDepth);
	if (hasDiameter) {
		if (circular.has_value () && !*circular)
			Fail ("'diameter'/'endDiameter' require shape 'Circular' (remove 'shape' or use width/" + GS::UniString (k.depth) + ").");
		circular = true;
	}
	if (!circular.has_value () && hasRectSizes && d.modelElemStructureType != API_ProfileStructure)
		circular = false;
	if (circular.has_value ()) {
		if (*circular && d.modelElemStructureType == API_ProfileStructure)
			Fail ("A complex-profile segment cannot be circular. Also pass 'buildingMaterial' to switch it to a basic (circular) section.");
		if (*circular && !d.circleBased)
			d.nominalHeight = d.nominalWidth;		// switching to round: keep the width as diameter unless a diameter is given
		d.circleBased = *circular;
	}

	// Sizes
	if (d.circleBased) {
		std::optional<double> dia = OptPositive (spec, "diameter");
		if (!dia.has_value ())
			dia = OptPositive (spec, "width");
		if (dia.has_value ())
			d.nominalWidth = *dia;
		d.nominalHeight = d.nominalWidth;
		d.isWidthAndHeightLinked = true;

		std::optional<double> endDia = OptPositive (spec, "endDiameter");
		if (!endDia.has_value ())
			endDia = OptPositive (spec, "endWidth");
		if (endDia.has_value ()) {
			d.endWidth = *endDia;
			d.isHomogeneous = false;
		}
	} else {
		auto w = OptPositive (spec, "width");
		auto h = OptPositive (spec, k.depth);
		if (w.has_value ()) d.nominalWidth = *w;
		if (h.has_value ()) d.nominalHeight = *h;
		if (w.has_value () || h.has_value ())
			d.isWidthAndHeightLinked = std::fabs (d.nominalWidth - d.nominalHeight) < kEps;

		auto ew = OptPositive (spec, "endWidth");
		auto eh = OptPositive (spec, k.endDepth);
		if (ew.has_value ()) { d.endWidth = *ew; d.isHomogeneous = false; }
		if (eh.has_value ()) { d.endHeight = *eh; d.isHomogeneous = false; }
	}

	// Taper
	if (auto t = OptBool (spec, "tapered")) {
		if (*t) {
			d.isHomogeneous = false;
		} else {
			if (spec.Contains ("endWidth") || spec.Contains (k.endDepth) || spec.Contains ("endDiameter"))
				Fail ("'tapered': false conflicts with the given end sizes (endWidth/" + GS::UniString (k.endDepth) + "/endDiameter).");
			d.isHomogeneous = true;
		}
	}
	if (!d.isHomogeneous) {
		if (d.endWidth <= kEps) d.endWidth = d.nominalWidth;
		if (d.endHeight <= kEps) d.endHeight = d.nominalHeight;
		if (d.circleBased) d.endHeight = d.endWidth;
		d.isEndWidthAndHeightLinked = std::fabs (d.endWidth - d.endHeight) < kEps;
	}
}


void AddSectionJson (OS& out, const API_AssemblySegmentData& d, const SectionKeys& k)
{
	const bool isProfile = d.modelElemStructureType == API_ProfileStructure;
	out.Add ("shape", GS::UniString (isProfile ? "Profile" : (d.circleBased ? "Circular" : "Rectangular")));
	if (d.circleBased && !isProfile) {
		out.Add ("diameter", d.nominalWidth);
	} else {
		out.Add ("width", d.nominalWidth);
		out.Add (k.depth, d.nominalHeight);
	}
	out.Add ("tapered", !d.isHomogeneous);
	if (!d.isHomogeneous) {
		if (d.circleBased && !isProfile) {
			out.Add ("endDiameter", d.endWidth);
		} else {
			out.Add ("endWidth", d.endWidth);
			out.Add (k.endDepth, d.endHeight);
		}
	}
	AddStructureJson (out, d.modelElemStructureType, d.buildingMaterial, 0, d.profileAttr);
}

// --- Schemes ------------------------------------------------------------------------------

bool ApplyScheme (API_AssemblySegmentSchemeData& scheme, const OS& spec)
{
	if (spec.Contains ("length") && spec.Contains ("lengthProportion"))
		Fail ("Give either 'length' (fixed, m) or 'lengthProportion' (relative share), not both.");
	if (auto v = OptPositive (spec, "length")) {
		scheme.lengthType = APIAssemblySegment_Fixed;
		scheme.fixedLength = *v;
		return true;
	}
	if (auto v = OptDouble (spec, "lengthProportion")) {
		if (!(*v > kEps))
			Fail ("'lengthProportion' must be greater than 0.");
		scheme.lengthType = APIAssemblySegment_Proportional;
		scheme.lengthProportion = *v;
		return true;
	}
	return false;
}


void AddSchemeJson (OS& out, const API_AssemblySegmentSchemeData& scheme, double actualLength)
{
	if (scheme.lengthType == APIAssemblySegment_Fixed) {
		out.Add ("lengthType", GS::UniString ("Fixed"));
		out.Add ("length", scheme.fixedLength);
	} else {
		out.Add ("lengthType", GS::UniString ("Proportional"));
		out.Add ("lengthProportion", scheme.lengthProportion);
	}
	if (actualLength >= 0.0)
		out.Add ("actualLength", actualLength);
}


void NormalizeSchemes (API_AssemblySegmentSchemeData* schemes, UInt32 count, double totalLength)
{
	if (schemes == nullptr || count == 0)
		return;
	double fixedSum = 0.0;
	double propSum = 0.0;
	UInt32 nProp = 0;
	for (UInt32 i = 0; i < count; ++i) {
		if (schemes[i].lengthType == APIAssemblySegment_Fixed) {
			fixedSum += schemes[i].fixedLength;
		} else {
			propSum += std::max (0.0, schemes[i].lengthProportion);
			++nProp;
		}
	}
	if (nProp > 0) {
		for (UInt32 i = 0; i < count; ++i) {
			if (schemes[i].lengthType != APIAssemblySegment_Fixed)
				schemes[i].lengthProportion = propSum > kEps ? std::max (0.0, schemes[i].lengthProportion) / propSum : 1.0 / nProp;
		}
	}
	if (totalLength > kEps && fixedSum > totalLength + 1e-6) {
		Fail ("The fixed segment lengths add up to " + Num (fixedSum) + " m, more than the total length " + Num (totalLength) +
			  " m. Shorten them or give some segments 'lengthProportion' instead of 'length'.");
	}
	if (totalLength > kEps && nProp > 0 && fixedSum > totalLength - 1e-6) {
		Fail ("The fixed segment lengths (" + Num (fixedSum) + " m) leave no room for the proportional segments (total length " +
			  Num (totalLength) + " m). Shorten the fixed segments.");
	}
}


GS::Array<double> SegmentLengths (const API_AssemblySegmentSchemeData* schemes, UInt32 count, double totalLength)
{
	GS::Array<double> lengths;
	if (schemes == nullptr)
		return lengths;
	double fixedSum = 0.0, propSum = 0.0;
	UInt32 nProp = 0;
	for (UInt32 i = 0; i < count; ++i) {
		if (schemes[i].lengthType == APIAssemblySegment_Fixed) {
			fixedSum += schemes[i].fixedLength;
		} else {
			propSum += std::max (0.0, schemes[i].lengthProportion);
			++nProp;
		}
	}
	const double remaining = std::max (0.0, totalLength - fixedSum);
	for (UInt32 i = 0; i < count; ++i) {
		if (schemes[i].lengthType == APIAssemblySegment_Fixed)
			lengths.Push (schemes[i].fixedLength);
		else
			lengths.Push (propSum > kEps ? remaining * std::max (0.0, schemes[i].lengthProportion) / propSum : (nProp > 0 ? remaining / nProp : 0.0));
	}
	return lengths;
}

// --- Cuts -----------------------------------------------------------------------------------

void ApplyCut (API_AssemblySegmentCutData& cut, const OS& spec)
{
	if (spec.Contains ("type"))
		cut.cutType = (API_AssemblySegmentCutTypeID) ParseNamed (kCutTypes, spec, "type");
	if (auto a = OptAngle (spec, "angle")) {
		if (std::fabs (*a) >= kPi / 2 - 1e-6)
			Fail ("Cut 'angle' must be between -90 and 90 degrees.");
		cut.customAngle = *a;
		if (!spec.Contains ("type"))
			cut.cutType = APIAssemblySegmentCut_Custom;
	}
}


OS CutToJson (const API_AssemblySegmentCutData& cut)
{
	OS out ("type", NameOf (kCutTypes, cut.cutType));
	if (cut.cutType == APIAssemblySegmentCut_Custom)
		AddAngle (out, "angle", cut.customAngle);
	return out;
}

// --- Memo arrays ------------------------------------------------------------------------------

API_AssemblySegmentCutData* ResizeCuts (API_AssemblySegmentCutData* old, UInt32 oldCount, UInt32 newSegmentCount,
										API_AssemblySegmentCutTypeID innerDefault)
{
	const UInt32 newCount = newSegmentCount + 1;
	const UInt32 available = std::min (oldCount, PtrCount (old));

	API_AssemblySegmentCutData startCut;
	BNZeroMemory (&startCut, sizeof (startCut));
	startCut.cutType = innerDefault;
	API_AssemblySegmentCutData endCut = startCut;
	if (available > 0) {
		startCut = old[0];
		endCut = old[available - 1];
	}
	API_AssemblySegmentCutData innerTemplate = startCut;
	if (innerTemplate.cutType == APIAssemblySegmentCut_Custom) {
		innerTemplate.cutType = innerDefault;
		innerTemplate.customAngle = 0.0;
	}
	if (available > 2)
		innerTemplate = old[available - 2];

	API_AssemblySegmentCutData* result = reinterpret_cast<API_AssemblySegmentCutData*> (
		BMAllocatePtr ((GSSize) (newCount * sizeof (API_AssemblySegmentCutData)), ALLOCATE_CLEAR, 0));
	if (result == nullptr)
		Fail ("Out of memory while resizing segment cuts.", APIERR_MEMFULL);

	result[0] = startCut;
	result[newCount - 1] = endCut;
	for (UInt32 i = 1; i + 1 < newCount; ++i)
		result[i] = (available >= 2 && i + 1 < available) ? old[i] : innerTemplate;

	if (old != nullptr) {
		GSPtr p = reinterpret_cast<GSPtr> (old);
		BMKillPtr (&p);
	}
	return result;
}


UInt32 FilterProfiles (API_AssemblySegmentProfileData*& profiles, UInt32 count, UInt32 newSegmentCount)
{
	const UInt32 available = std::min (count, PtrCount (profiles));
	UInt32 kept = 0;
	for (UInt32 i = 0; i < available; ++i) {
		if (profiles[i].segmentIndex < newSegmentCount)
			++kept;
	}
	if (kept == available)
		return kept;

	const UInt32 dropped = available - kept;
	API_AssemblySegmentProfileData* keptArr = kept > 0 ? reinterpret_cast<API_AssemblySegmentProfileData*> (
		BMAllocatePtr ((GSSize) (kept * sizeof (API_AssemblySegmentProfileData)), ALLOCATE_CLEAR, 0)) : nullptr;
	API_AssemblySegmentProfileData* droppedArr = reinterpret_cast<API_AssemblySegmentProfileData*> (
		BMAllocatePtr ((GSSize) (dropped * sizeof (API_AssemblySegmentProfileData)), ALLOCATE_CLEAR, 0));
	if ((kept > 0 && keptArr == nullptr) || droppedArr == nullptr) {
		if (keptArr != nullptr) { GSPtr p = reinterpret_cast<GSPtr> (keptArr); BMKillPtr (&p); }
		if (droppedArr != nullptr) { GSPtr p = reinterpret_cast<GSPtr> (droppedArr); BMKillPtr (&p); }
		Fail ("Out of memory while removing segment profiles.", APIERR_MEMFULL);
	}

	UInt32 k = 0, d = 0;
	for (UInt32 i = 0; i < available; ++i) {
		if (profiles[i].segmentIndex < newSegmentCount)
			keptArr[k++] = profiles[i];
		else
			droppedArr[d++] = profiles[i];
	}

	// Let Archicad release the profile images of the removed segments exactly as it releases memo data.
	API_ElementMemo trash;
	BNZeroMemory (&trash, sizeof (trash));
	trash.assemblySegmentProfiles = droppedArr;
	ACAPI_DisposeElemMemoHdls (&trash);

	GSPtr p = reinterpret_cast<GSPtr> (profiles);
	BMKillPtr (&p);
	profiles = keptArr;
	return kept;
}

} // namespace cb
} // namespace cc
