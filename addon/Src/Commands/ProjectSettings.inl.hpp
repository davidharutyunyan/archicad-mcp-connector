#pragma once

// *****************************************************************************
// IMPLEMENTATION FILE — included exactly once, at the end of Project.cpp.
// Kept as a header so that the CMake source glob (evaluated only when the build
// directory is configured) does not need to be re-run for this family.
//
// ProjectSettings — Project Preferences (read / write) and project location
// (latitude, longitude, altitude, north, time zone, date, survey point, geo
// reference). Part of the "project" family, registered from Project.cpp.
//
// Commands:
//   GetPreferences  {sections?: [name]}
//   SetPreferences  {<section>: {partial fields}, ...}
//   GetGeoLocation  {}
//   SetGeoLocation  {latitude?, longitude?, altitude?, northDirection?, timeZoneMinutes?, summerTime?,
//                    dateTime?, surveyPoint?, surveyPointVisible?, surveyPointLocked?, unlockSurveyPoint?, geoReference?}
//
// Preference writes use the environment function 'SPRF', which AC26 ships but does not
// declare in APIdefs_Environment.h (it is used by the DevKit's Environment_Control example;
// AC27 renamed it to ACAPI_ProjectSetting_SetPreferences).
// *****************************************************************************

#include "Commands/ProjectShared.hpp"
#include "Core/Command.hpp"
#include "Core/Enums.hpp"
#include "Core/Types.hpp"

#include "Location.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace cc {
namespace project {

namespace {

const API_EnvironmentID kSetPreferencesID		= static_cast<API_EnvironmentID> ('SPRF');
const API_EnvironmentID kChangeSuspendGroupID	= static_cast<API_EnvironmentID> ('CSPG');

// =============================================================================
// Enum tables
// =============================================================================

const NamedValue kLengthUnits[] = {
	{ "Meter",			(Int32) API_LengthTypeID::Meter },
	{ "Decimeter",		(Int32) API_LengthTypeID::Decimeter },
	{ "Centimeter",		(Int32) API_LengthTypeID::Centimeter },
	{ "Millimeter",		(Int32) API_LengthTypeID::Millimeter },
	{ "FootFracInch",	(Int32) API_LengthTypeID::FootFracInch },
	{ "FootDecInch",	(Int32) API_LengthTypeID::FootDecInch },
	{ "DecFoot",		(Int32) API_LengthTypeID::DecFoot },
	{ "FracInch",		(Int32) API_LengthTypeID::FracInch },
	{ "DecInch",		(Int32) API_LengthTypeID::DecInch },
	{ "Kilometer",		(Int32) API_LengthTypeID::KiloMeter },
	{ "Yard",			(Int32) API_LengthTypeID::Yard },
};

const NamedValue kAreaUnits[] = {
	{ "SquareMeter",		(Int32) API_AreaTypeID::SquareMeter },
	{ "SquareKilometer",	(Int32) API_AreaTypeID::SquareKiloMeter },
	{ "SquareDecimeter",	(Int32) API_AreaTypeID::SquareDeciMeter },
	{ "SquareCentimeter",	(Int32) API_AreaTypeID::SquareCentimeter },
	{ "SquareMillimeter",	(Int32) API_AreaTypeID::SquareMillimeter },
	{ "SquareFoot",			(Int32) API_AreaTypeID::SquareFoot },
	{ "SquareInch",			(Int32) API_AreaTypeID::SquareInch },
	{ "SquareYard",			(Int32) API_AreaTypeID::SquareYard },
};

const NamedValue kVolumeUnits[] = {
	{ "CubicMeter",			(Int32) API_VolumeTypeID::CubicMeter },
	{ "CubicKilometer",		(Int32) API_VolumeTypeID::CubicKiloMeter },
	{ "Liter",				(Int32) API_VolumeTypeID::Liter },
	{ "CubicCentimeter",	(Int32) API_VolumeTypeID::CubicCentimeter },
	{ "CubicMillimeter",	(Int32) API_VolumeTypeID::CubicMillimeter },
	{ "CubicFoot",			(Int32) API_VolumeTypeID::CubicFoot },
	{ "CubicInch",			(Int32) API_VolumeTypeID::CubicInch },
	{ "CubicYard",			(Int32) API_VolumeTypeID::CubicYard },
	{ "Gallon",				(Int32) API_VolumeTypeID::Gallon },
};

const NamedValue kAngleUnits[] = {
	{ "DecimalDegree",	(Int32) API_AngleTypeID::DecimalDegree },
	{ "DegreeMinSec",	(Int32) API_AngleTypeID::DegreeMinSec },
	{ "Grad",			(Int32) API_AngleTypeID::Grad },
	{ "Radian",			(Int32) API_AngleTypeID::Radian },
	{ "Surveyors",		(Int32) API_AngleTypeID::Surveyors },
};

const NamedValue kExtraAccuracy[] = {
	{ "Off",		APIExtAc_Off },
	{ "Small5",		APIExtAc_Small5 },
	{ "Small25",	APIExtAc_Small25 },
	{ "Small1",		APIExtAc_Small1 },
	{ "Small01",	APIExtAc_Small01 },
	{ "Fractions",	APIExtAc_Fractions },
};

const NamedValue kConditionalProperties[] = {
	{ "Volume",		APIConditionalElementPropertyType_Volume },
	{ "Surface",	APIConditionalElementPropertyType_Surface },
	{ "Length",		APIConditionalElementPropertyType_Length },
};

const NamedValue kRoofContourDisplay[] = {
	{ "AllVisibleContours",		APIRoofDisplay_AllVisibleContours },
	{ "OnlyTopSurface",			APIRoofDisplay_OnlyTheTopSurfaceOldWay },
};

const NamedValue kRecessCombine[] = {
	{ "And",	APIRecess_And },
	{ "Or",		APIRecess_Or },
};

const NamedValue kReportLevels[] = {
	{ "None",		APIReportLevel_No },
	{ "Brief",		APIReportLevel_Brief },
	{ "Short",		APIReportLevel_Short },
	{ "Detailed",	APIReportLevel_Detailed },
	{ "Full",		APIReportLevel_Full },
};

const NamedValue kSelectedListing[] = {
	{ "ShowAlert",	APICalcSelElems_ShowAlert },
	{ "ListAll",	APICalcSelElems_ListAll },
	{ "UseFilters",	APICalcSelElems_UseFilters },
};

const NamedValue kNorthUnits[] = {
	{ "DecimalDegree",	APINorthUnit_DecimalDegree },
	{ "DegreeMinSec",	APINorthUnit_DegreeMinSec },
};

const NamedValue kAltitudeUnits[] = {
	{ "Meter",		APIAltitudeUnit_Meter },
	{ "DecFoot",	APIAltitudeUnit_DecFoot },
};

const NamedValue kLongLatUnits[] = {
	{ "DecimalDegree",	(Int32) API_LongLatUnitID::APILongLatUnit_DecimalDegree },
	{ "DegreeMinSec",	(Int32) API_LongLatUnitID::APILongLatUnit_DegreeMinSec },
};

// =============================================================================
// Small field helpers
// =============================================================================

void ApplyBool (const OS& s, const char* key, bool& field)
{
	if (auto v = OptBool (s, key))
		field = *v;
}


void ApplyDouble (const OS& s, const char* key, double& field)
{
	if (auto v = OptDouble (s, key))
		field = *v;
}


// NOTE: GS::UniString::Printf is STATIC (s.Printf (...) discards its result): format with snprintf.
GS::UniString RangeMessage (const char* key, const char* what, double minV, double maxV, bool integer)
{
	char buf[256];
	if (integer)
		std::snprintf (buf, sizeof (buf), " must be %s between %d and %d.", what, (int) minV, (int) maxV);
	else
		std::snprintf (buf, sizeof (buf), " must be %s between %g and %g.", what, minV, maxV);
	return GS::UniString ("'") + GS::UniString (key, CC_UTF8) + "'" + GS::UniString (buf, CC_UTF8);
}


void ApplyRangedDouble (const OS& s, const char* key, double& field, double minV, double maxV)
{
	if (auto v = OptDouble (s, key)) {
		if (*v < minV || *v > maxV)
			Fail (RangeMessage (key, "a number", minV, maxV, false));
		field = *v;
	}
}


template <typename T>
void ApplyInt (const OS& s, const char* key, T& field, Int32 minV, Int32 maxV)
{
	if (auto v = OptInt (s, key)) {
		if (*v < minV || *v > maxV)
			Fail (RangeMessage (key, "an integer", minV, maxV, true));
		field = static_cast<T> (*v);
	}
}


template <size_t N, typename T>
void ApplyEnum (const NamedValue (&table)[N], const OS& s, const char* key, T& field)
{
	if (auto v = OptNamed (table, s, key))
		field = static_cast<T> (*v);
}


// roundInch must be a power of two between 1 and 64.
template <typename T>
void ApplyRoundInch (const OS& s, const char* key, T& field)
{
	if (auto v = OptInt (s, key)) {
		const Int32 x = *v;
		if (x != 1 && x != 2 && x != 4 && x != 8 && x != 16 && x != 32 && x != 64)
			Fail ("'" + GS::UniString (key) + "' must be one of 1, 2, 4, 8, 16, 32, 64 (fraction denominator for inch units).");
		field = static_cast<T> (x);
	}
}


// Collects the items of a JSON array as {"v": item} objects so that OptAttr can parse each.
class ValueListCollector : public OS::Processor {
public:
	GS::Array<OS> items;
	void IntFound (const GS::String&, Int64 value) override		{ items.Push (OS ("v", (Int32) value)); }
	void UIntFound (const GS::String&, UInt64 value) override	{ items.Push (OS ("v", (Int32) value)); }
	void RealFound (const GS::String&, double value) override	{ items.Push (OS ("v", value)); }
	void StringFound (const GS::String&, const GS::UniString& value) override { items.Push (OS ("v", value)); }
	bool ObjectFound (const GS::String&, const OS& value) override { items.Push (OS ("v", value)); return false; }
};


GS::HashSet<API_AttributeIndex> GetAttrSet (API_AttrTypeID typeID, const OS& s, const char* key)
{
	if (!s.IsList (key))
		Fail ("'" + GS::UniString (key) + "' must be an array of " + AttrTypeName (typeID) + " names or indices.");
	ValueListCollector collector;
	s.Enumerate (key, collector);
	GS::HashSet<API_AttributeIndex> result;
	for (const OS& item : collector.items)
		result.Add (GetAttr (typeID, item, "v"));
	return result;
}


GS::Array<OS> AttrSetJson (API_AttrTypeID typeID, const GS::HashSet<API_AttributeIndex>& set)
{
	std::vector<API_AttributeIndex> sorted;
	for (const API_AttributeIndex& idx : set)
		sorted.push_back (idx);
	std::sort (sorted.begin (), sorted.end ());
	GS::Array<OS> out;
	for (API_AttributeIndex idx : sorted)
		out.Push (AttrRef (typeID, idx));
	return out;
}


OS ColorJson (const API_RGBColor& c)
{
	return OS ("red", c.f_red, "green", c.f_green, "blue", c.f_blue);
}


void ApplyColor (const OS& s, const char* key, API_RGBColor& c)
{
	OS col;
	if (!TryGetObject (s, key, col))
		return;
	ApplyRangedDouble (col, "red", c.f_red, 0.0, 1.0);
	ApplyRangedDouble (col, "green", c.f_green, 0.0, 1.0);
	ApplyRangedDouble (col, "blue", c.f_blue, 0.0, 1.0);
}

// =============================================================================
// Get / set raw preference structs
// =============================================================================

template <typename T>
void GetPrefs (T& prefs, API_PrefsTypeID id, const char* section)
{
	const GSErrCode err = ACAPI_Environment (APIEnv_GetPreferencesID, &prefs, reinterpret_cast<void*> ((GS::IntPtr) id));
	Check (err, GS::UniString ("Cannot read the '") + section + "' preferences");
}


template <typename T>
void SetPrefs (T& prefs, API_PrefsTypeID id, const char* section)
{
	const GSErrCode err = ModifyCall ([&] () {
		return ACAPI_Environment (kSetPreferencesID, &prefs, reinterpret_cast<void*> ((GS::IntPtr) id));
	});
	if (err == APIERR_BADID || err == APIERR_MISSINGCODE || err == APIERR_NOTSUPPORTED)
		Fail (GS::UniString ("Archicad 26 cannot change the '") + section + "' preferences through the API (" + ErrorName (err) + "). Change them in Options > Project Preferences.", err);
	if (err == APIERR_NOTEDITABLE || err == APIERR_NOACCESSRIGHT)
		Fail (GS::UniString ("The '") + section + "' preferences are not editable (" + ErrorName (err) + "). In Teamwork, reserve Project Preferences first.", err);
	if (err == APIERR_BADPARS)
		Fail (GS::UniString ("Archicad rejected the new '") + section + "' preferences as invalid (APIERR_BADPARS). Check value ranges (decimals 0-4, valid units, rule element types).", err);
	Check (err, GS::UniString ("Cannot change the '") + section + "' preferences");
}

// --- Working units ---------------------------------------------------------------

OS WorkingUnitsJson (const API_WorkingUnitPrefs& p)
{
	OS out;
	out.Add ("lengthUnit", NameOf (kLengthUnits, (Int32) p.lengthUnit));
	out.Add ("areaUnit", NameOf (kAreaUnits, (Int32) p.areaUnit));
	out.Add ("volumeUnit", NameOf (kVolumeUnits, (Int32) p.volumeUnit));
	out.Add ("angleUnit", NameOf (kAngleUnits, (Int32) p.angleUnit));
	out.Add ("lengthDecimals", (Int32) p.lenDecimals);
	out.Add ("areaDecimals", (Int32) p.areaDecimals);
	out.Add ("volumeDecimals", (Int32) p.volumeDecimals);
	out.Add ("angleDecimals", (Int32) p.angleDecimals);
	out.Add ("angleAccuracy", (Int32) p.angleAccuracy);
	out.Add ("roundInch", (Int32) p.roundInch);
	return out;
}


void ApplyWorkingUnits (API_WorkingUnitPrefs& p, const OS& s)
{
	ApplyEnum (kLengthUnits, s, "lengthUnit", p.lengthUnit);
	ApplyEnum (kAreaUnits, s, "areaUnit", p.areaUnit);
	ApplyEnum (kVolumeUnits, s, "volumeUnit", p.volumeUnit);
	ApplyEnum (kAngleUnits, s, "angleUnit", p.angleUnit);
	ApplyInt (s, "lengthDecimals", p.lenDecimals, 0, 4);
	ApplyInt (s, "areaDecimals", p.areaDecimals, 0, 4);
	ApplyInt (s, "volumeDecimals", p.volumeDecimals, 0, 4);
	ApplyInt (s, "angleDecimals", p.angleDecimals, 0, 4);
	ApplyInt (s, "angleAccuracy", p.angleAccuracy, 0, 4);
	ApplyRoundInch (s, "roundInch", p.roundInch);
}

// --- Dimensions ------------------------------------------------------------------

OS LengthDimJson (const API_LengthDimFormat& f)
{
	OS out;
	out.Add ("unit", NameOf (kLengthUnits, (Int32) f.unit));
	out.Add ("decimals", (Int32) f.lenDecimals);
	out.Add ("roundInch", (Int32) f.roundInch);
	out.Add ("extraAccuracy", NameOf (kExtraAccuracy, f.showSmall5));
	out.Add ("hideZeroDecimals", f.hide0Dec);
	out.Add ("showZeroWhole", f.show0Whole);
	out.Add ("showZeroInch", f.show0Inch != 0);
	return out;
}


void ApplyLengthDim (API_LengthDimFormat& f, const OS& s)
{
	ApplyEnum (kLengthUnits, s, "unit", f.unit);
	ApplyInt (s, "decimals", f.lenDecimals, 0, 4);
	ApplyRoundInch (s, "roundInch", f.roundInch);
	ApplyEnum (kExtraAccuracy, s, "extraAccuracy", f.showSmall5);
	ApplyBool (s, "hideZeroDecimals", f.hide0Dec);
	ApplyBool (s, "showZeroWhole", f.show0Whole);
	if (auto v = OptBool (s, "showZeroInch"))
		f.show0Inch = *v ? 1 : 0;
}


OS AngleDimJson (const API_AngleDimFormat& f)
{
	OS out;
	out.Add ("unit", NameOf (kAngleUnits, (Int32) f.unit));
	out.Add ("decimals", (Int32) f.angleDecimals);
	out.Add ("accuracy", (Int32) f.angleAccuracy);
	out.Add ("hideZeroDecimals", f.hide0Dec);
	return out;
}


void ApplyAngleDim (API_AngleDimFormat& f, const OS& s)
{
	ApplyEnum (kAngleUnits, s, "unit", f.unit);
	ApplyInt (s, "decimals", f.angleDecimals, 0, 4);
	ApplyInt (s, "accuracy", f.angleAccuracy, 0, 4);
	ApplyBool (s, "hideZeroDecimals", f.hide0Dec);
}


OS AreaDimJson (const API_AreaDimFormat& f)
{
	OS out;
	out.Add ("unit", NameOf (kAreaUnits, (Int32) f.unit));
	out.Add ("decimals", (Int32) f.lenDecimals);
	out.Add ("hideZeroDecimals", f.hide0Dec);
	return out;
}


void ApplyAreaDim (API_AreaDimFormat& f, const OS& s)
{
	ApplyEnum (kAreaUnits, s, "unit", f.unit);
	ApplyInt (s, "decimals", f.lenDecimals, 0, 4);
	ApplyBool (s, "hideZeroDecimals", f.hide0Dec);
}


OS DimensionsJson (const API_DimensionPrefs& p)
{
	OS out;
	out.Add ("linear", LengthDimJson (p.linear));
	out.Add ("angular", AngleDimJson (p.angle));
	out.Add ("radial", LengthDimJson (p.radial));
	out.Add ("level", LengthDimJson (p.level));
	out.Add ("elevation", LengthDimJson (p.elevation));
	out.Add ("doorWindow", LengthDimJson (p.doorwindow));
	out.Add ("sillHeight", LengthDimJson (p.parapet));
	out.Add ("area", AreaDimJson (p.area));
	out.Add ("standardIndex", (Int32) p.index);
	if (p.index > 0)
		out.Add ("standard", AttrRef (API_DimStandID, p.index));
	else
		out.Add ("standard", GS::UniString ("Custom"));
	return out;
}


void ApplyDimensions (API_DimensionPrefs& p, const OS& s)
{
	OS sub;
	if (TryGetObject (s, "linear", sub))		ApplyLengthDim (p.linear, sub);
	if (TryGetObject (s, "angular", sub))		ApplyAngleDim (p.angle, sub);
	if (TryGetObject (s, "radial", sub))		ApplyLengthDim (p.radial, sub);
	if (TryGetObject (s, "level", sub))			ApplyLengthDim (p.level, sub);
	if (TryGetObject (s, "elevation", sub))		ApplyLengthDim (p.elevation, sub);
	if (TryGetObject (s, "doorWindow", sub))	ApplyLengthDim (p.doorwindow, sub);
	if (TryGetObject (s, "sillHeight", sub))	ApplyLengthDim (p.parapet, sub);
	if (TryGetObject (s, "area", sub))			ApplyAreaDim (p.area, sub);
	if (Has (s, "standardIndex"))
		Fail ("'standardIndex' is read-only; select a Dimension Standard with 'standard' (index, name or \"Custom\").");
}


bool SameLengthDim (const API_LengthDimFormat& a, const API_LengthDimFormat& b)
{
	return a.unit == b.unit && a.roundInch == b.roundInch && a.lenDecimals == b.lenDecimals && a.show0Whole == b.show0Whole &&
		   (a.show0Inch != 0) == (b.show0Inch != 0) && a.showSmall5 == b.showSmall5 && a.hide0Dec == b.hide0Dec;
}


bool SameDimFormats (const API_DimensionPrefs& a, const API_DimensionPrefs& b)
{
	return SameLengthDim (a.linear, b.linear) && SameLengthDim (a.radial, b.radial) && SameLengthDim (a.level, b.level) &&
		   SameLengthDim (a.elevation, b.elevation) && SameLengthDim (a.doorwindow, b.doorwindow) && SameLengthDim (a.parapet, b.parapet) &&
		   a.angle.unit == b.angle.unit && a.angle.angleDecimals == b.angle.angleDecimals && a.angle.angleAccuracy == b.angle.angleAccuracy &&
		   a.angle.hide0Dec == b.angle.hide0Dec &&
		   a.area.unit == b.area.unit && a.area.lenDecimals == b.area.lenDecimals && a.area.hide0Dec == b.area.hide0Dec;
}


void CopyDimFormats (API_DimensionPrefs& to, const API_DimensionPrefs& from)
{
	to.linear = from.linear;		to.angle = from.angle;			to.radial = from.radial;
	to.level = from.level;			to.elevation = from.elevation;	to.doorwindow = from.doorwindow;
	to.parapet = from.parapet;		to.area = from.area;
}


API_DimensionPrefs ReadDimensions ()
{
	API_DimensionPrefs p {};
	GetPrefs (p, APIPrefs_DimensionsID, "dimensions");
	return p;
}


// Dimensions are special: while a Dimension Standard attribute is selected (index > 0), Archicad 26 takes
// the formats from that attribute and silently ignores changed formats (verified live). So every write is
// verified by reading back; format changes fall back to "Custom" settings (index 0), and when nothing
// works the original settings are restored and the call fails with an explanation.
OS SetDimensionsSection (const OS& patch)
{
	const API_DimensionPrefs original = ReadDimensions ();
	API_DimensionPrefs wanted = original;
	OS result;

	if (Has (patch, "standard")) {
		if (patch.IsString ("standard") && EqualsIgnoreCase (GetString (patch, "standard"), "Custom")) {
			wanted.index = 0;
		} else {
			const API_AttributeIndex idx = GetAttr (API_DimStandID, patch, "standard");
			API_Attribute attr = {};
			attr.header.typeID = API_DimStandID;
			attr.header.index = idx;
			Check (ACAPI_Attribute_Get (&attr), "Cannot read the Dimension Standard attribute");
			wanted.index = (short) idx;
			CopyDimFormats (wanted, attr.dimension.dim);		// the formats the standard defines
		}
	}
	const API_DimensionPrefs selected = wanted;
	ApplyDimensions (wanted, patch);
	const bool formatsGiven = !SameDimFormats (wanted, selected);

	SetPrefs (wanted, APIPrefs_DimensionsID, "dimensions");
	API_DimensionPrefs got = ReadDimensions ();
	if (SameDimFormats (got, wanted) && (got.index == wanted.index || formatsGiven))
		return result;

	if (formatsGiven && wanted.index != 0) {
		// Formats are ignored while a standard is selected: switch to custom dimension settings.
		API_DimensionPrefs custom = wanted;
		custom.index = 0;
		SetPrefs (custom, APIPrefs_DimensionsID, "dimensions");
		got = ReadDimensions ();
		if (SameDimFormats (got, custom)) {
			result.Add ("note", GS::UniString ("The formats were applied as Custom dimension settings: the previously selected Dimension Standard is no longer active."));
			return result;
		}
	}

	// Nothing worked: restore the original settings and explain.
	API_DimensionPrefs restore = original;
	SetPrefs (restore, APIPrefs_DimensionsID, "dimensions");
	GS::UniString standardName = original.index > 0 ? AttrName (API_DimStandID, original.index) : GS::UniString ("Custom");
	Fail ("Archicad 26 did not apply the dimension settings (it keeps the formats of the Dimension Standard '" + standardName +
		  "'). Select another standard with 'standard', or change the formats in Options > Project Preferences > Dimensions.", APIERR_REFUSEDPAR);
}

// --- Calculation units -------------------------------------------------------------

OS CalcUnitsJson (const API_CalcUnitPrefs& p)
{
	OS out;
	out.Add ("length", OS ("unit", NameOf (kLengthUnits, (Int32) p.length.unit), "decimals", (Int32) p.length.decimals,
						   "roundInch", (Int32) p.length.roundInch, "extraAccuracy", NameOf (kExtraAccuracy, p.length.accuracy)));
	out.Add ("area", OS ("unit", NameOf (kAreaUnits, (Int32) p.area.unit), "decimals", (Int32) p.area.decimals,
						 "extraAccuracy", NameOf (kExtraAccuracy, p.area.accuracy)));
	out.Add ("volume", OS ("unit", NameOf (kVolumeUnits, (Int32) p.volume.unit), "decimals", (Int32) p.volume.decimals,
						   "extraAccuracy", NameOf (kExtraAccuracy, p.volume.accuracy)));
	out.Add ("angle", OS ("unit", NameOf (kAngleUnits, (Int32) p.angle.unit), "decimals", (Int32) p.angle.decimals,
						  "accuracy", (Int32) p.angle.accuracy));
	out.Add ("useDisplayedValues", p.useDisplayedValues);
	return out;
}


void ApplyCalcUnits (API_CalcUnitPrefs& p, const OS& s)
{
	OS sub;
	if (TryGetObject (s, "length", sub)) {
		ApplyEnum (kLengthUnits, sub, "unit", p.length.unit);
		ApplyInt (sub, "decimals", p.length.decimals, 0, 4);
		ApplyRoundInch (sub, "roundInch", p.length.roundInch);
		ApplyEnum (kExtraAccuracy, sub, "extraAccuracy", p.length.accuracy);
	}
	if (TryGetObject (s, "area", sub)) {
		ApplyEnum (kAreaUnits, sub, "unit", p.area.unit);
		ApplyInt (sub, "decimals", p.area.decimals, 0, 4);
		ApplyEnum (kExtraAccuracy, sub, "extraAccuracy", p.area.accuracy);
	}
	if (TryGetObject (s, "volume", sub)) {
		ApplyEnum (kVolumeUnits, sub, "unit", p.volume.unit);
		ApplyInt (sub, "decimals", p.volume.decimals, 0, 4);
		ApplyEnum (kExtraAccuracy, sub, "extraAccuracy", p.volume.accuracy);
	}
	if (TryGetObject (s, "angle", sub)) {
		ApplyEnum (kAngleUnits, sub, "unit", p.angle.unit);
		ApplyInt (sub, "decimals", p.angle.decimals, 0, 4);
		ApplyInt (sub, "accuracy", p.angle.accuracy, 0, 4);
	}
	ApplyBool (s, "useDisplayedValues", p.useDisplayedValues);
}

// --- Calculation rules ---------------------------------------------------------------

OS CalcRulesJson (const API_CalcRulesPrefs& p)
{
	OS out;
	GS::Array<OS> rules;
	for (const API_ConditionalElementRule& rule : p.elementRules) {
		GS::Array<GS::UniString> types;
		for (const API_ElemType& t : rule.elementTypes)
			types.Push (ElemTypeName (t));
		rules.Push (OS ("property", NameOf (kConditionalProperties, rule.propertyType), "holeLimit", rule.holeLimit, "elementTypes", types));
	}
	out.Add ("elementRules", rules);
	// The API names these sets "...Fills", but in AC26 they hold BUILDING MATERIAL indices (verified live:
	// the template's wall air set is index 42 = "Воздушная Прослойка" building material, a "Настил 03" fill).
	out.Add ("wallInsulationMaterials", AttrSetJson (API_BuildingMaterialID, p.wallInsulationFills));
	out.Add ("wallAirMaterials", AttrSetJson (API_BuildingMaterialID, p.wallAirFills));
	out.Add ("roofInsulationMaterials", AttrSetJson (API_BuildingMaterialID, p.roofInsulationFills));
	out.Add ("shellInsulationMaterials", AttrSetJson (API_BuildingMaterialID, p.shellInsulationFills));
	return out;
}


// Reads a building material set by its field name or by the legacy "...Fills" alias.
void ApplyMaterialSet (const OS& s, const char* key, const char* legacyKey, GS::HashSet<API_AttributeIndex>& field)
{
	if (Has (s, key))
		field = GetAttrSet (API_BuildingMaterialID, s, key);
	else if (Has (s, legacyKey))
		field = GetAttrSet (API_BuildingMaterialID, s, legacyKey);
}


void ApplyCalcRules (API_CalcRulesPrefs& p, const OS& s)
{
	if (Has (s, "elementRules")) {
		GS::Array<API_ConditionalElementRule> rules;
		for (const OS& r : GetObjectArray (s, "elementRules")) {
			API_ConditionalElementRule rule {};
			rule.propertyType = static_cast<API_ConditionalElementPropertyTypeID> (ParseNamed (kConditionalProperties, r, "property"));
			rule.holeLimit = GetDouble (r, "holeLimit");
			if (rule.holeLimit < 0)
				Fail ("'holeLimit' must be >= 0.");
			for (const GS::UniString& typeName : GetStringArray (r, "elementTypes", true)) {
				auto t = ParseElemType (typeName);
				if (!t.has_value ())
					Fail ("Unknown element type '" + typeName + "' in elementRules (use names like Wall, Slab, Roof, Shell, Beam, Column, Morph).");
				rule.elementTypes.Add (API_ElemType (*t));
			}
			rules.Push (rule);
		}
		p.elementRules = rules;
	}
	ApplyMaterialSet (s, "wallInsulationMaterials", "wallInsulationFills", p.wallInsulationFills);
	ApplyMaterialSet (s, "wallAirMaterials", "wallAirFills", p.wallAirFills);
	ApplyMaterialSet (s, "roofInsulationMaterials", "roofInsulationFills", p.roofInsulationFills);
	ApplyMaterialSet (s, "shellInsulationMaterials", "shellInsulationFills", p.shellInsulationFills);
}

// --- Reference levels ------------------------------------------------------------------

// Reading: Archicad replaces level1UStr / level2UStr with pointers to strings it owns (as in the DevKit
// Environment_Control example, which passes a zeroed struct and never frees them). They must only be copied:
// deleting them from the add-on aborts Archicad ("pointer being freed was not allocated", seen live on AC26).
struct RefLevels {
	GS::UniString	name1, name2;
	double			value1 = 0.0, value2 = 0.0;
};


RefLevels ReadRefLevels ()
{
	RefLevels r;
	API_RefLevelsPrefs p;
	BNZeroMemory (&p, sizeof (p));
	GetPrefs (p, APIPrefs_ReferenceLevelsID, "referenceLevels");
	if (p.level1UStr != nullptr)
		r.name1 = *p.level1UStr;		// owned by Archicad: copy, never delete
	if (p.level2UStr != nullptr)
		r.name2 = *p.level2UStr;
	r.value1 = p.level1val;
	r.value2 = p.level2val;
	return r;
}


OS RefLevelsJson (const RefLevels& r)
{
	return OS ("level1", OS ("name", r.name1, "elevation", r.value1), "level2", OS ("name", r.name2, "elevation", r.value2));
}


void ApplyAndWriteRefLevels (const OS& s)
{
	RefLevels r = ReadRefLevels ();
	OS sub;
	if (TryGetObject (s, "level1", sub)) {
		if (auto n = OptString (sub, "name")) r.name1 = *n;
		ApplyDouble (sub, "elevation", r.value1);
	}
	if (TryGetObject (s, "level2", sub)) {
		if (auto n = OptString (sub, "name")) r.name2 = *n;
		ApplyDouble (sub, "elevation", r.value2);
	}
	API_RefLevelsPrefs p = {};
	p.level1UStr = &r.name1;
	p.level2UStr = &r.name2;
	p.level1val = r.value1;
	p.level2val = r.value2;
	SetPrefs (p, APIPrefs_ReferenceLevelsID, "referenceLevels");
}

// --- Legacy ------------------------------------------------------------------------------

OS LegacyJson (const API_LegacyPrefs& p)
{
	OS out;
	out.Add ("columnConnectionPriority", (Int32) p.coluConnPriority);
	out.Add ("aboveLineType", AttrRef (API_LinetypeID, p.aboveLineType));
	out.Add ("belowLineType", AttrRef (API_LinetypeID, p.belowLineType));
	out.Add ("roofContourDisplay", NameOf (kRoofContourDisplay, p.roofContourDisplay));
	out.Add ("useLegacyIntersections", p.useLegacyIntersections);
	out.Add ("hideZonesOnSections", p.hideZonesOnSections);
	return out;
}


void ApplyLegacy (API_LegacyPrefs& p, const OS& s)
{
	if (auto v = OptInt (s, "columnConnectionPriority"))	p.coluConnPriority = *v;
	if (auto v = OptAttr (API_LinetypeID, s, "aboveLineType"))	p.aboveLineType = *v;
	if (auto v = OptAttr (API_LinetypeID, s, "belowLineType"))	p.belowLineType = *v;
	ApplyEnum (kRoofContourDisplay, s, "roofContourDisplay", p.roofContourDisplay);
	ApplyBool (s, "useLegacyIntersections", p.useLegacyIntersections);
	ApplyBool (s, "hideZonesOnSections", p.hideZonesOnSections);
}

// --- Zones -------------------------------------------------------------------------------

OS ZonesJson (const API_ZonePrefs& p)
{
	OS out;
	out.Add ("recesses", OS ("depth", p.nichDepth, "size", p.nichSize,
							 "combine", NameOf (kRecessCombine, p.nichCombineMode),
							 "includeWindows", p.nichWindFlag, "includeDoors", p.nichDoorFlag,
							 "checkDepth", p.nichDepthFlag, "checkSize", p.nichSizeFlag));
	out.Add ("walls", OS ("subtract", p.subtrWallFlag, "percent", (Int32) p.subtrWallPercent, "sizeLimit", p.subtrWallSize));
	out.Add ("columns", OS ("subtract", p.subtrColuFlag, "percent", (Int32) p.subtrColuPercent, "sizeLimit", p.subtrColuSize));
	GS::Array<OS> low;
	const Int32 n = p.nLowAreas > 4 ? 4 : (Int32) p.nLowAreas;
	for (Int32 i = 0; i < n; ++i)
		low.Push (OS ("heightLimit", p.roomHeightLimits[i], "reductionPercent", (Int32) p.roomRedPercents[i]));
	out.Add ("lowHeightReductions", low);
	return out;
}


void ApplyZones (API_ZonePrefs& p, const OS& s)
{
	OS sub;
	if (TryGetObject (s, "recesses", sub)) {
		ApplyDouble (sub, "depth", p.nichDepth);
		ApplyDouble (sub, "size", p.nichSize);
		ApplyEnum (kRecessCombine, sub, "combine", p.nichCombineMode);
		ApplyBool (sub, "includeWindows", p.nichWindFlag);
		ApplyBool (sub, "includeDoors", p.nichDoorFlag);
		ApplyBool (sub, "checkDepth", p.nichDepthFlag);
		ApplyBool (sub, "checkSize", p.nichSizeFlag);
	}
	if (TryGetObject (s, "walls", sub)) {
		ApplyBool (sub, "subtract", p.subtrWallFlag);
		ApplyInt (sub, "percent", p.subtrWallPercent, 0, 100);
		ApplyDouble (sub, "sizeLimit", p.subtrWallSize);
	}
	if (TryGetObject (s, "columns", sub)) {
		ApplyBool (sub, "subtract", p.subtrColuFlag);
		ApplyInt (sub, "percent", p.subtrColuPercent, 0, 100);
		ApplyDouble (sub, "sizeLimit", p.subtrColuSize);
	}
	if (Has (s, "lowHeightReductions")) {
		const GS::Array<OS> low = GetObjectArray (s, "lowHeightReductions");
		if (low.GetSize () > 4)
			Fail ("'lowHeightReductions' can have at most 4 rows.");
		for (UIndex i = 0; i < 4; ++i) {
			p.roomHeightLimits[i] = 0.0;
			p.roomRedPercents[i] = 0;
		}
		for (UIndex i = 0; i < low.GetSize (); ++i) {
			p.roomHeightLimits[i] = GetDouble (low[i], "heightLimit");
			ApplyInt (low[i], "reductionPercent", p.roomRedPercents[i], 0, 100);
		}
		p.nLowAreas = (unsigned char) low.GetSize ();
	}
}

// --- Imaging and calculation, floor plan cut plane, layouts --------------------------------

OS ImagingJson (const API_ImagingAndCalcPrefs& p)
{
	OS out;
	out.Add ("autoRebuild3D", p.autoRebuild3D);
	out.Add ("activate3DOnChange", p.activate3DOnChange);
	out.Add ("keepZoomedSection", p.keepZoomedSection);
	out.Add ("showProgressWindow", p.showProgressWindow);
	out.Add ("interruptOnError", p.interruptOnError);
	out.Add ("progressSounds", p.progressSounds);
	out.Add ("write3DReport", p.generate3DReport);
	out.Add ("reportLevel", NameOf (kReportLevels, p.reportLevel));
	out.Add ("selectedElementsListing", NameOf (kSelectedListing, p.selElemsListing));
	return out;
}


void ApplyImaging (API_ImagingAndCalcPrefs& p, const OS& s)
{
	if (Has (s, "progressSounds"))
		Fail ("'progressSounds' cannot be changed through the Archicad 26 API (Archicad keeps its value). Change it in Options > Work Environment.", APIERR_REFUSEDPAR);
	ApplyBool (s, "autoRebuild3D", p.autoRebuild3D);
	ApplyBool (s, "activate3DOnChange", p.activate3DOnChange);
	ApplyBool (s, "keepZoomedSection", p.keepZoomedSection);
	ApplyBool (s, "showProgressWindow", p.showProgressWindow);
	ApplyBool (s, "interruptOnError", p.interruptOnError);
	ApplyBool (s, "progressSounds", p.progressSounds);
	ApplyBool (s, "write3DReport", p.generate3DReport);
	ApplyEnum (kReportLevels, s, "reportLevel", p.reportLevel);
	ApplyEnum (kSelectedListing, s, "selectedElementsListing", p.selElemsListing);
}


OS CutPlaneJson (const API_FloorPlanCutDefinition& p)
{
	OS out;
	out.Add ("cutHeight", p.currCutLevel);
	out.Add ("topLevel", p.topCutLevel);
	out.Add ("topStoryOffset", (Int32) p.topCutBaseStoryRelInd);
	out.Add ("bottomLevel", p.bottomCutLevel);
	out.Add ("bottomStoryOffset", (Int32) p.bottomCutBaseStoryRelInd);
	out.Add ("fixedLevel", p.fixLevel2Absolute0);
	return out;
}


void ApplyCutPlane (API_FloorPlanCutDefinition& p, const OS& s)
{
	ApplyDouble (s, "cutHeight", p.currCutLevel);
	ApplyDouble (s, "topLevel", p.topCutLevel);
	ApplyInt (s, "topStoryOffset", p.topCutBaseStoryRelInd, -100, 100);
	ApplyDouble (s, "bottomLevel", p.bottomCutLevel);
	ApplyInt (s, "bottomStoryOffset", p.bottomCutBaseStoryRelInd, -100, 100);
	ApplyDouble (s, "fixedLevel", p.fixLevel2Absolute0);
}


OS LayoutsJson (const API_LayoutsPrefs& p)
{
	OS out;
	out.Add ("masterItemColor", ColorJson (p.masterColor));
	out.Add ("useOwnMasterColor", p.useOwnMasterColor);
	out.Add ("adjustDrawingFrameToViewZoom", p.adjustDrawingFrameToViewZoom);
	return out;
}


void ApplyLayouts (API_LayoutsPrefs& p, const OS& s)
{
	if (Has (s, "masterItemColor"))
		Fail ("'masterItemColor' cannot be changed through the Archicad 26 API (Archicad keeps its value). Change it in Options > Project Preferences > Layouts.", APIERR_REFUSEDPAR);
	ApplyBool (s, "useOwnMasterColor", p.useOwnMasterColor);
	ApplyBool (s, "adjustDrawingFrameToViewZoom", p.adjustDrawingFrameToViewZoom);
}

// --- Environment switches (not part of Project Preferences, but session settings) ------------

OS EnvironmentJson ()
{
	OS out;
	bool flag = false;
	if (ACAPI_Environment (APIEnv_IsAutoIntersectOnID, &flag) == NoError)	out.Add ("autoIntersect", flag);
	flag = false;
	if (ACAPI_Environment (APIEnv_IsAutoGroupOnID, &flag) == NoError)		out.Add ("autoGroup", flag);
	flag = false;
	if (ACAPI_Environment (APIEnv_IsSuspendGroupOnID, &flag) == NoError)		out.Add ("suspendGroups", flag);
	flag = false;
	if (ACAPI_Goodies (APIAny_GetAutoTextFlagID, &flag) == NoError)			out.Add ("autoTextEnabled", flag);
	double tolerance = 0.0;
	if (ACAPI_Environment (APIEnv_GetExportToleranceID, &tolerance) == NoError)	out.Add ("exportTolerance", tolerance);
	return out;
}


void ApplyEnvironment (const OS& s)
{
	if (Has (s, "autoGroup") || Has (s, "exportTolerance"))
		Fail ("'autoGroup' and 'exportTolerance' are read-only through the API.");
	if (auto v = OptBool (s, "autoIntersect")) {
		bool value = *v;
		Check (ModifyCall ([&] () { return ACAPI_Environment (APIEnv_ChangeAutoIntersectID, &value); }), "Cannot change auto-intersection");
	}
	if (auto v = OptBool (s, "suspendGroups")) {
		auto isOn = [] () { bool on = false; ACAPI_Environment (APIEnv_IsSuspendGroupOnID, &on); return on; };
		if (isOn () != *v) {
			// APITool_SuspendGroups toggles the mode (the element list is unused). The undeclared 'CSPG' switch
			// turned Suspend Groups on but never off in live tests, so it is only a fallback.
			GSErrCode err = ModifyCall ([&] () { return ACAPI_Element_Tool (GS::Array<API_Guid> (), APITool_SuspendGroups, nullptr); });
			if (isOn () != *v) {
				bool value = *v;
				err = ModifyCall ([&] () { return ACAPI_Environment (kChangeSuspendGroupID, &value); });
			}
			Check (err, "Cannot change 'Suspend Groups'");
			if (isOn () != *v)
				Fail (GS::UniString ("Archicad did not switch 'Suspend Groups' ") + (*v ? "on" : "off") + ". Use Edit > Grouping > Suspend Groups.", APIERR_REFUSEDCMD);
		}
	}
	if (auto v = OptBool (s, "autoTextEnabled")) {
		bool value = *v;
		Check (ModifyCall ([&] () { return ACAPI_Goodies (APIAny_ChangeAutoTextFlagID, &value); }), "Cannot change the autotext flag");
	}
}

// =============================================================================
// Section registry
// =============================================================================

struct PrefSection {
	const char*						name;
	std::function<OS ()>			get;
	std::function<void (const OS&)>	set;			// empty (and no setWithNote) = read-only
	std::function<OS (const OS&)>	setWithNote;	// alternative setter returning extra result fields (e.g. {note})

	bool Writable () const { return set || setWithNote; }
};


template <typename T>
PrefSection StructSection (const char* name, API_PrefsTypeID id, OS (*toJson) (const T&), void (*apply) (T&, const OS&))
{
	PrefSection section;
	section.name = name;
	section.get = [=] () -> OS {
		T prefs {};
		GetPrefs (prefs, id, name);
		return toJson (prefs);
	};
	if (apply != nullptr) {
		section.set = [=] (const OS& patch) {
			T prefs {};
			GetPrefs (prefs, id, name);
			apply (prefs, patch);
			SetPrefs (prefs, id, name);
		};
	}
	return section;
}


const GS::Array<PrefSection>& Sections ()
{
	static const GS::Array<PrefSection> sections = [] {
		GS::Array<PrefSection> s;
		s.Push (StructSection<API_WorkingUnitPrefs> ("workingUnits", APIPrefs_WorkingUnitsID, WorkingUnitsJson, ApplyWorkingUnits));
		{
			PrefSection dim;
			dim.name = "dimensions";
			dim.get = [] () -> OS { return DimensionsJson (ReadDimensions ()); };
			dim.setWithNote = SetDimensionsSection;
			s.Push (dim);
		}
		s.Push (StructSection<API_CalcUnitPrefs> ("calculationUnits", APIPrefs_CalcUnitsID, CalcUnitsJson, ApplyCalcUnits));
		s.Push (StructSection<API_CalcRulesPrefs> ("calculationRules", APIPrefs_CalcRulesID, CalcRulesJson, ApplyCalcRules));
		{
			PrefSection ref;
			ref.name = "referenceLevels";
			ref.get = [] () -> OS { return RefLevelsJson (ReadRefLevels ()); };
			ref.set = [] (const OS& patch) { ApplyAndWriteRefLevels (patch); };
			s.Push (ref);
		}
		s.Push (StructSection<API_LegacyPrefs> ("legacy", APIPrefs_LegacyID, LegacyJson, ApplyLegacy));
		s.Push (StructSection<API_ZonePrefs> ("zones", APIPrefs_ZonesID, ZonesJson, ApplyZones));
		s.Push (StructSection<API_ImagingAndCalcPrefs> ("imagingAndCalculation", APIPrefs_ImagingAndCalcID, ImagingJson, ApplyImaging));
		s.Push (StructSection<API_FloorPlanCutDefinition> ("floorPlanCutPlane", APIPrefs_FloorPlanCutPlaneDef, CutPlaneJson, ApplyCutPlane));
		s.Push (StructSection<API_LayoutsPrefs> ("layouts", APIPrefs_LayoutsID, LayoutsJson, ApplyLayouts));
		{
			PrefSection safety;
			safety.name = "dataSafety";
			safety.get = [] () -> OS {
				API_SafetyPrefs p = {};
				GetPrefs (p, APIPrefs_DataSafetyID, "dataSafety");
				OS out;
				if (p.tempFolder != nullptr) {
					GS::UniString path;
					if (p.tempFolder->ToPath (&path) != NoError)
						path = p.tempFolder->ToDisplayText ();
					out.Add ("temporaryFolder", path);
					delete p.tempFolder;
				}
				return out;
			};
			s.Push (safety);
		}
		{
			PrefSection env;
			env.name = "environment";
			env.get = EnvironmentJson;
			env.set = ApplyEnvironment;
			s.Push (env);
		}
		return s;
	} ();
	return sections;
}


GS::UniString SectionNames ()
{
	GS::UniString names;
	for (const PrefSection& s : Sections ()) {
		if (!names.IsEmpty ())
			names += ", ";
		names += s.name;
	}
	return names;
}


const PrefSection* FindSection (const GS::UniString& name)
{
	for (const PrefSection& s : Sections ()) {
		if (EqualsIgnoreCase (name, s.name))
			return &s;
	}
	return nullptr;
}


// Top-level keys of an ObjectState (UTF-8 field names).
GS::Array<GS::UniString> TopLevelKeys (const OS& os)
{
	GS::Array<GS::UniString> keys;
	os.EnumerateFields ([&] (const GS::String& name) { keys.Push (GS::UniString (name.ToCStr (), CC_UTF8)); });
	return keys;
}


OS GetPreferences (const OS& params)
{
	GS::Array<GS::UniString> wanted = GetStringArray (params, "sections", false);
	for (const GS::UniString& w : wanted) {
		if (FindSection (w) == nullptr)
			Fail ("Unknown preferences section '" + w + "'. Allowed: " + SectionNames () + ".");
	}
	OS out;
	for (const PrefSection& s : Sections ()) {
		if (!wanted.IsEmpty ()) {
			bool include = false;
			for (const GS::UniString& w : wanted)
				include = include || EqualsIgnoreCase (w, s.name);
			if (!include)
				continue;
		}
		out.Add (s.name, Try ([&] () -> OS { return s.get (); }));
	}
	return out;
}


OS SetPreferences (const OS& params)
{
	const GS::Array<GS::UniString> keys = TopLevelKeys (params);
	if (keys.IsEmpty ())
		Fail ("Pass at least one preferences section to change, e.g. {\"workingUnits\": {\"lengthUnit\": \"Millimeter\"}}. Sections: " + SectionNames () + ".");
	for (const GS::UniString& key : keys) {
		const PrefSection* s = FindSection (key);
		if (s == nullptr)
			Fail ("Unknown preferences section '" + key + "'. Allowed: " + SectionNames () + ".");
		if (!s->Writable ())
			Fail ("The '" + key + "' preferences are read-only through the Archicad 26 API.");
		if (!params.IsObject (ToStr (key)))
			Fail ("Section '" + key + "' must be an object with the fields to change.");
	}

	return WithOptionalUndo ("Change Project Preferences (Claude)", [&] () -> OS {
		OS results;
		for (const GS::UniString& key : keys) {
			const PrefSection* s = FindSection (key);
			OS patch;
			params.Get (ToStr (key), patch);
			results.Add (s->name, Try ([&] () -> OS {
				OS extra;
				if (s->setWithNote)
					extra = s->setWithNote (patch);
				else
					s->set (patch);
				OS res ("ok", true, "value", s->get ());
				GS::UniString note;
				if (extra.Get ("note", note))
					res.Add ("note", note);
				return res;
			}));
		}
		return OS ("results", results);
	});
}

// =============================================================================
// Geo location
// =============================================================================

OS PlaceJson (const API_PlaceInfo& p)
{
	OS out;
	out.Add ("latitude", p.latitude);
	out.Add ("longitude", p.longitude);
	out.Add ("altitude", p.altitude);
	AddAngle (out, "northDirection", p.north);
	out.Add ("timeZoneMinutes", (Int32) p.timeZoneInMinutes);
	out.Add ("timeZoneOffset", (Int32) p.timeZoneOffset);
	out.Add ("summerTime", p.sumTime);
	out.Add ("dateTime", OS ("year", (Int32) p.year, "month", (Int32) p.month, "day", (Int32) p.day,
							 "hour", (Int32) p.hour, "minute", (Int32) p.minute, "second", (Int32) p.second));
	OS sun;
	AddAngle (sun, "azimuth", p.sunAngXY);
	AddAngle (sun, "altitude", p.sunAngZ);
	out.Add ("sun", sun);
	out.Add ("displayUnits", OS ("north", NameOf (kNorthUnits, p.northUnit), "altitude", NameOf (kAltitudeUnits, p.altitudeUnit),
								 "longitudeLatitude", NameOf (kLongLatUnits, (Int32) p.longLatUnit)));
	return out;
}


// Applies place fields; returns true when something changed that affects the sun position.
bool ApplyPlace (API_PlaceInfo& p, const OS& s)
{
	bool sunChange = false;
	if (Has (s, "latitude"))	{ ApplyRangedDouble (s, "latitude", p.latitude, -90.0, 90.0); sunChange = true; }
	if (Has (s, "longitude"))	{ ApplyRangedDouble (s, "longitude", p.longitude, -180.0, 180.0); sunChange = true; }
	ApplyDouble (s, "altitude", p.altitude);
	if (auto v = OptAngle (s, "northDirection"))	{ p.north = *v; sunChange = true; }
	if (Has (s, "timeZoneMinutes"))	{ ApplyInt (s, "timeZoneMinutes", p.timeZoneInMinutes, -720, 840); sunChange = true; }
	ApplyInt (s, "timeZoneOffset", p.timeZoneOffset, -32768, 32767);
	if (auto v = OptBool (s, "summerTime"))	{ p.sumTime = *v; sunChange = true; }
	OS dt;
	if (TryGetObject (s, "dateTime", dt)) {
		ApplyInt (dt, "year", p.year, 1, 9999);
		ApplyInt (dt, "month", p.month, 1, 12);
		ApplyInt (dt, "day", p.day, 1, 31);
		ApplyInt (dt, "hour", p.hour, 0, 23);
		ApplyInt (dt, "minute", p.minute, 0, 59);
		ApplyInt (dt, "second", p.second, 0, 59);
		static const int kDays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
		const bool leap = (p.year % 4 == 0 && p.year % 100 != 0) || p.year % 400 == 0;
		const int maxDay = (p.month >= 1 && p.month <= 12) ? kDays[p.month - 1] + ((p.month == 2 && leap) ? 1 : 0) : 31;
		if (p.day > maxDay) {
			char buf[160];
			std::snprintf (buf, sizeof (buf), "dateTime: day %d does not exist in %04d-%02d (last day: %d).", (int) p.day, (int) p.year, (int) p.month, maxDay);
			Fail (GS::UniString (buf));
		}
		sunChange = true;
	}
	OS units;
	if (TryGetObject (s, "displayUnits", units)) {
		ApplyEnum (kNorthUnits, units, "north", p.northUnit);
		ApplyEnum (kAltitudeUnits, units, "altitude", p.altitudeUnit);
		ApplyEnum (kLongLatUnits, units, "longitudeLatitude", p.longLatUnit);
	}
	return sunChange;
}


OS GeoReferenceJson (const API_GeoReferenceData& g)
{
	OS out;
	out.Add ("name", g.name);
	out.Add ("description", g.description);
	out.Add ("geodeticDatum", g.geodeticDatum);
	out.Add ("verticalDatum", g.verticalDatum);
	out.Add ("mapProjection", g.mapProjection);
	out.Add ("mapZone", g.mapZone);
	out.Add ("eastings", g.eastings);
	out.Add ("northings", g.northings);
	out.Add ("orthogonalHeight", g.orthogonalHeight);
	out.Add ("xAxisAbscissa", g.xAxisAbscissa);
	out.Add ("xAxisOrdinate", g.xAxisOrdinate);
	out.Add ("scale", g.scale);
	return out;
}


void ApplyGeoReference (API_GeoReferenceData& g, const OS& s)
{
	if (auto v = OptString (s, "name"))				g.name = *v;
	if (auto v = OptString (s, "description"))		g.description = *v;
	if (auto v = OptString (s, "geodeticDatum"))	g.geodeticDatum = *v;
	if (auto v = OptString (s, "verticalDatum"))	g.verticalDatum = *v;
	if (auto v = OptString (s, "mapProjection"))	g.mapProjection = *v;
	if (auto v = OptString (s, "mapZone"))			g.mapZone = *v;
	ApplyDouble (s, "eastings", g.eastings);
	ApplyDouble (s, "northings", g.northings);
	ApplyDouble (s, "orthogonalHeight", g.orthogonalHeight);
	ApplyDouble (s, "xAxisAbscissa", g.xAxisAbscissa);
	ApplyDouble (s, "xAxisOrdinate", g.xAxisOrdinate);
	if (Has (s, "scale")) {
		ApplyDouble (s, "scale", g.scale);
		if (g.scale <= 0.0)
			Fail ("'geoReference.scale' must be > 0 (1 = no scaling).");
	}
}


bool SurveyPointLocked ()
{
	bool locked = false;
	ACAPI_Environment (APIEnv_IsSurveyPointLockedID, &locked);
	return locked;
}


void SetSurveyPointLocked (bool locked)
{
	bool value = locked;
	Check (ModifyCall ([&] () { return ACAPI_Environment (APIEnv_SetSurveyPointLockedID, &value); }),
		   locked ? "Cannot lock the Survey Point" : "Cannot unlock the Survey Point");
}


OS GeoLocationJson ()
{
	API_GeoLocation geo = {};
	Check (ACAPI_Environment (APIEnv_GetGeoLocationID, &geo), "Cannot read the project location (geo location)");

	API_PlaceInfo place = {};
	if (ACAPI_Environment (APIEnv_GetPlaceSetsID, &place) != NoError)
		place = geo.placeInfo;

	OS out = PlaceJson (place);

	OS survey;
	survey.Add ("position", Coord3DObj (geo.surveyPointPosition));
	bool flag = false;
	if (ACAPI_Environment (APIEnv_IsSurveyPointVisibleID, &flag) == NoError)
		survey.Add ("visible", flag);
	flag = false;
	if (ACAPI_Environment (APIEnv_IsSurveyPointLockedID, &flag) == NoError)
		survey.Add ("locked", flag);
	API_Tranmat tm = {};
	if (ACAPI_Environment (APIEnv_GetSurveyPointTransformationID, &tm) == NoError) {
		// Project origin expressed in the Survey Point coordinate system (same math as the DevKit example).
		survey.Add ("projectOriginInSurveyCoordinates", Coord3DObj (tm.tmx[3], tm.tmx[7], tm.tmx[11]));
		GS::Array<double> matrix;
		for (double v : tm.tmx)
			matrix.Push (v);
		survey.Add ("transformation", matrix);
	}
	out.Add ("surveyPoint", survey);
	out.Add ("geoReference", GeoReferenceJson (geo.geoReferenceData));

	bool canChange = false;
	bool withDialog = false;
	canChange = ACAPI_Environment (APIEnv_CanChangePlaceSetsID, &withDialog) == NoError;
	out.Add ("editable", canChange);
	return out;
}


OS SetGeoLocation (const OS& params)
{
	static const char* const kPlaceKeys[] = { "latitude", "longitude", "altitude", "timeZoneMinutes", "timeZoneOffset", "summerTime", "dateTime", "displayUnits" };
	bool placeChange = false;
	for (const char* k : kPlaceKeys)
		placeChange = placeChange || Has (params, k);
	const bool northChange = Has (params, "northDirection");
	const bool geoChange = northChange || Has (params, "surveyPoint") || Has (params, "geoReference");
	const auto visible = OptBool (params, "surveyPointVisible");
	const auto lockedWanted = OptBool (params, "surveyPointLocked");
	const bool unlockTemporarily = GetBool (params, "unlockSurveyPoint", false);
	if (!placeChange && !geoChange && !visible.has_value () && !lockedWanted.has_value ())
		Fail ("Nothing to change. Pass latitude/longitude/altitude/northDirection/timeZoneMinutes/summerTime/dateTime, surveyPoint, geoReference, surveyPointVisible or surveyPointLocked.");

	// Validate everything before changing anything.
	{
		API_PlaceInfo probe = {};
		ApplyPlace (probe, params);
		API_GeoReferenceData probeRef = {};
		OS ref;
		if (TryGetObject (params, "geoReference", ref))
			ApplyGeoReference (probeRef, ref);
		OS sp;
		if (TryGetObject (params, "surveyPoint", sp))
			Coord3DFrom (sp);
	}

	// A locked Survey Point locks the whole project location in AC26 (APIEnv_CanChangePlaceSetsID and every setter
	// answer APIERR_NOTEDITABLE, verified live), so unlock first when the caller allows it.
	const bool unlockAllowed = unlockTemporarily || (lockedWanted.has_value () && !*lockedWanted);
	bool unlockFirst = false;
	if (placeChange || geoChange) {
		bool withDialog = false;
		GSErrCode canErr = ACAPI_Environment (APIEnv_CanChangePlaceSetsID, &withDialog);
		if (canErr == APIERR_NOTEDITABLE && SurveyPointLocked ()) {
			if (!unlockAllowed)
				Fail ("The Survey Point is locked, and Archicad then locks the whole project location (latitude, longitude, north, survey point, "
					  "geo reference). Pass unlockSurveyPoint: true (re-locked afterwards) or surveyPointLocked: false.", canErr);
			unlockFirst = true;
			canErr = NoError;
		}
		if (canErr != NoError)
			Fail ("The project location cannot be changed now (" + ErrorName (canErr) + "). In Teamwork, reserve 'Project Location' first.", canErr);
	}
	// Re-lock after a temporary unlock, also when the change fails.
	const bool relockAfter = unlockTemporarily && !lockedWanted.has_value ();

	return WithOptionalUndo ("Change project location (Claude)", [&] () -> OS {
		struct Relock {
			bool active = false;
			~Relock () {
				if (active && !SurveyPointLocked ()) {
					bool value = true;
					ACAPI_Environment (APIEnv_SetSurveyPointLockedID, &value);
				}
			}
		} relock;

		if (SurveyPointLocked () && (unlockFirst || (lockedWanted.has_value () && !*lockedWanted) || (geoChange && unlockTemporarily))) {
			SetSurveyPointLocked (false);
			relock.active = relockAfter;
		}

		if (geoChange) {
			API_GeoLocation geo = {};
			Check (ACAPI_Environment (APIEnv_GetGeoLocationID, &geo), "Cannot read the project location");
			if (northChange) {
				if (ApplyPlace (geo.placeInfo, params))
					ACAPI_Goodies (APIAny_CalcSunOnPlaceID, &geo.placeInfo);
			}
			OS sp;
			if (TryGetObject (params, "surveyPoint", sp)) {
				const API_Coord3D pos = Coord3DFrom (sp);
				geo.surveyPointPosition.x = pos.x;
				geo.surveyPointPosition.y = pos.y;
				if (Has (sp, "z"))
					geo.surveyPointPosition.z = pos.z;
			}
			OS ref;
			if (TryGetObject (params, "geoReference", ref))
				ApplyGeoReference (geo.geoReferenceData, ref);

			const GSErrCode err = ModifyCall ([&] () { return ACAPI_Environment (APIEnv_SetGeoLocationID, &geo); });
			if (err == APIERR_NOTEDITABLE && SurveyPointLocked ())
				Fail ("The Survey Point is locked, so the project location cannot change. Pass unlockSurveyPoint: true (re-locks afterwards) or surveyPointLocked: false.", err);
			if (err == APIERR_NOTEDITABLE || err == APIERR_NOACCESSRIGHT)
				Fail ("The geo location is not editable (" + ErrorName (err) + "). In Teamwork, reserve 'Project Location' first.", err);
			Check (err, "Cannot change the geo location");
		}

		if (placeChange) {
			API_PlaceInfo place = {};
			Check (ACAPI_Environment (APIEnv_GetPlaceSetsID, &place), "Cannot read the project location");
			if (ApplyPlace (place, params))
				ACAPI_Goodies (APIAny_CalcSunOnPlaceID, &place);
			const GSErrCode err = ModifyCall ([&] () { return ACAPI_Environment (APIEnv_ChangePlaceSetsID, &place); });
			if (err == APIERR_NOTEDITABLE || err == APIERR_NOACCESSRIGHT)
				Fail ("The project location is not editable (" + ErrorName (err) + "). In Teamwork, reserve 'Project Location' first.", err);
			Check (err, "Cannot change the project location (latitude/longitude/date)");
		}

		if (visible.has_value ()) {
			bool value = *visible;
			Check (ModifyCall ([&] () { return ACAPI_Environment (APIEnv_SetSurveyPointVisibilityID, &value); }), "Cannot change the Survey Point visibility");
		}
		if (lockedWanted.has_value () && *lockedWanted && !SurveyPointLocked ())
			SetSurveyPointLocked (true);
		if (relock.active) {
			SetSurveyPointLocked (true);
			relock.active = false;
		}

		return GeoLocationJson ();
	});
}

} // namespace


void RegisterSettingsCommands ()
{
	RegisterCommand ("GetPreferences",
		"Reads Project Preferences and session switches. Input: {sections?: [workingUnits, dimensions, calculationUnits, calculationRules, "
		"referenceLevels, legacy, zones, imagingAndCalculation, floorPlanCutPlane, layouts, dataSafety, environment]} (default: all). "
		"Output: {<section>: {...} | {error}}.",
		GetPreferences);

	RegisterCommand ("SetPreferences",
		"Changes Project Preferences. Input: {<section>: {fields to change}} with the same section/field names as GetPreferences "
		"(dataSafety is read-only). Output: {results: {<section>: {ok, value} | {error}}}.",
		SetPreferences);

	RegisterCommand ("GetGeoLocation",
		"Project location: latitude, longitude (deg), altitude (m), northDirection (deg, CCW from +X), time zone, date/time, sun position, "
		"survey point (position, visible, locked, transformation) and geo reference (CRS, eastings/northings...).",
		[] (const OS&) -> OS { return GeoLocationJson (); });

	RegisterCommand ("SetGeoLocation",
		"Changes the project location. Input: any of {latitude, longitude, altitude, northDirection, timeZoneMinutes, timeZoneOffset, summerTime, "
		"dateTime: {year, month, day, hour, minute, second}, displayUnits, surveyPoint: {x, y, z}, surveyPointVisible, surveyPointLocked, "
		"unlockSurveyPoint, geoReference: {...}}. Output: the new location (as GetGeoLocation).",
		SetGeoLocation);
}

} // namespace project
} // namespace cc
