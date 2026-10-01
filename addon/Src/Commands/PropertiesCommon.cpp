// Helpers shared by the properties family (see PropertiesCommon.hpp).

#include "Commands/PropertiesCommon.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace cc {
namespace props {

// --- Heterogeneous JSON values ------------------------------------------------------

GS::UniString JScalar::Describe () const
{
	switch (kind) {
		case Bool:		return b ? "true" : "false";
		case Int:		return NumberToString ((double) i);
		case Real:		return NumberToString (d);
		case String:	return "\"" + s + "\"";
		case Object:	return "an object";
	}
	return "?";
}


namespace {

class JValueCollector : public OS::Processor {
public:
	explicit JValueCollector (JValue& target) : target (target) {}

	void BoolFound (const GS::String&, bool value) override
	{
		JScalar j; j.kind = JScalar::Bool; j.b = value; target.items.Push (j);
	}
	void IntFound (const GS::String&, Int64 value) override
	{
		JScalar j; j.kind = JScalar::Int; j.i = value; target.items.Push (j);
	}
	void UIntFound (const GS::String&, UInt64 value) override
	{
		JScalar j; j.kind = JScalar::Int; j.i = (Int64) value; target.items.Push (j);
	}
	void RealFound (const GS::String&, double value) override
	{
		JScalar j; j.kind = JScalar::Real; j.d = value; target.items.Push (j);
	}
	void StringFound (const GS::String&, const GS::UniString& value) override
	{
		JScalar j; j.kind = JScalar::String; j.s = value; target.items.Push (j);
	}
	bool ObjectFound (const GS::String&, const OS& value) override
	{
		JScalar j; j.kind = JScalar::Object; j.obj = value; target.items.Push (j);
		return false;	// do not descend
	}
	void ListEntered (const GS::String&) override
	{
		if (++depth > 1)
			target.nested = true;
	}
	void ListExited (const GS::String&) override
	{
		--depth;
	}

private:
	JValue&	target;
	Int32	depth = 0;
};


JScalar ReadScalarField (const OS& os, const char* key)
{
	JScalar j;
	if (os.IsBool (key)) {
		j.kind = JScalar::Bool; os.Get (key, j.b);
	} else if (os.IsInt (key)) {
		j.kind = JScalar::Int; os.Get (key, j.i);
	} else if (os.IsUInt (key)) {
		UInt64 u = 0; os.Get (key, u);
		j.kind = JScalar::Int; j.i = (Int64) u;
	} else if (os.IsReal (key)) {
		j.kind = JScalar::Real; os.Get (key, j.d);
	} else if (os.IsString (key)) {
		j.kind = JScalar::String; os.Get (key, j.s);
	} else if (os.IsObject (key)) {
		j.kind = JScalar::Object; os.Get (key, j.obj);
	} else {
		Fail ("Field '" + GS::UniString (key) + "' has an unsupported JSON type (null is not supported; omit the field instead).");
	}
	return j;
}

} // namespace


JValue ReadJValue (const OS& os, const char* key)
{
	JValue v;
	if (!os.Contains (key))
		return v;
	v.present = true;
	if (os.IsList (key)) {
		v.isList = true;
		JValueCollector collector (v);
		os.Enumerate (key, collector);
	} else {
		v.items.Push (ReadScalarField (os, key));
	}
	return v;
}


JScalar ReadJScalar (const OS& os, const char* key)
{
	if (!os.Contains (key))
		Fail ("Missing required field '" + GS::UniString (key) + "'.");
	if (os.IsList (key))
		Fail ("Field '" + GS::UniString (key) + "' must be a single value, not a list.");
	return ReadScalarField (os, key);
}


GS::UniString Trimmed (const GS::UniString& s)
{
	GS::UniString t = s;
	t.Trim ();
	return t;
}


bool LooksLikeGuid (const GS::UniString& raw)
{
	std::string s (ToStr (Trimmed (raw)).ToCStr ());
	if (s.size () == 38 && s.front () == '{' && s.back () == '}')
		s = s.substr (1, 36);
	if (s.size () != 36)
		return false;
	for (size_t i = 0; i < 36; ++i) {
		const char c = s[i];
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (c != '-')
				return false;
		} else {
			const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
			if (!hex)
				return false;
		}
	}
	return true;
}


API_Guid ToGuid (const GS::UniString& raw)
{
	GS::UniString s = Trimmed (raw);
	if (s.GetLength () == 38 && s.BeginsWith ("{"))
		s = s.GetSubstring (1, 36);
	return ParseGuid (s);
}


bool ParseNumberString (const GS::UniString& text, double& out)
{
	std::string s (ToStr (Trimmed (text)).ToCStr ());
	std::string cleaned;
	for (char c : s) {
		if (c == ',')
			cleaned.push_back ('.');
		else if (c != ' ' && c != '\t')
			cleaned.push_back (c);
	}
	if (cleaned.empty ())
		return false;
	char* end = nullptr;
	const double v = std::strtod (cleaned.c_str (), &end);
	if (end == nullptr || *end != '\0' || !std::isfinite (v))
		return false;
	out = v;
	return true;
}


GS::UniString NumberToString (double v)
{
	char buf[64];
	if (std::fabs (v - std::round (v)) < 1e-9 && std::fabs (v) < 1e15)
		std::snprintf (buf, sizeof (buf), "%.0f", std::round (v));
	else
		std::snprintf (buf, sizeof (buf), "%.10g", v);
	return GS::UniString (buf);
}


GS::UniString JoinNames (const GS::Array<GS::UniString>& names, UIndex maxCount)
{
	GS::UniString out;
	for (UIndex i = 0; i < names.GetSize () && i < maxCount; ++i) {
		if (i > 0)
			out += ", ";
		out += names[i];
	}
	if (names.GetSize () > maxCount)
		out += ", ... (" + NumberToString ((double) (names.GetSize () - maxCount)) + " more)";
	return out;
}

// --- Type names ---------------------------------------------------------------------------

GS::UniString ScalarTypeName (API_VariantType valueType, API_PropertyMeasureType measure)
{
	switch (valueType) {
		case API_PropertyIntegerValueType:	return "integer";
		case API_PropertyRealValueType:
			switch (measure) {
				case API_PropertyLengthMeasureType:	return "length";
				case API_PropertyAreaMeasureType:	return "area";
				case API_PropertyVolumeMeasureType:	return "volume";
				case API_PropertyAngleMeasureType:	return "angle";
				default:							return "number";
			}
		case API_PropertyStringValueType:	return "string";
		case API_PropertyBooleanValueType:	return "boolean";
		case API_PropertyGuidValueType:		return "guid";
		default:							return "undefined";
	}
}


GS::UniString PropertyTypeName (const API_PropertyDefinition& def)
{
	switch (def.collectionType) {
		case API_PropertySingleCollectionType:						return ScalarTypeName (def.valueType, def.measureType);
		case API_PropertyListCollectionType:						return ScalarTypeName (def.valueType, def.measureType) + "List";
		case API_PropertySingleChoiceEnumerationCollectionType:		return "singleEnum";
		case API_PropertyMultipleChoiceEnumerationCollectionType:	return "multiEnum";
		default:													return "undefined";
	}
}


GS::UniString DefinitionKindName (API_PropertyDefinitionType type)
{
	switch (type) {
		case API_PropertyStaticBuiltInDefinitionType:	return "BuiltIn";
		case API_PropertyDynamicBuiltInDefinitionType:	return "DynamicBuiltIn";
		case API_PropertyCustomDefinitionType:			return "Custom";
	}
	return "Unknown";
}


GS::UniString GroupKindName (API_PropertyGroupType type)
{
	switch (type) {
		case API_PropertyStaticBuiltInGroupType:	return "BuiltIn";
		case API_PropertyDynamicBuiltInGroupType:	return "DynamicBuiltIn";
		case API_PropertyCustomGroupType:			return "Custom";
	}
	return "Unknown";
}


GS::UniString CollectionTypeName (API_PropertyCollectionType type)
{
	switch (type) {
		case API_PropertySingleCollectionType:						return "Single";
		case API_PropertyListCollectionType:						return "List";
		case API_PropertySingleChoiceEnumerationCollectionType:		return "SingleChoiceEnumeration";
		case API_PropertyMultipleChoiceEnumerationCollectionType:	return "MultipleChoiceEnumeration";
		default:													return "Undefined";
	}
}


GS::UniString ValueTypeName (API_VariantType type)
{
	switch (type) {
		case API_PropertyIntegerValueType:	return "Integer";
		case API_PropertyRealValueType:		return "Real";
		case API_PropertyStringValueType:	return "String";
		case API_PropertyBooleanValueType:	return "Boolean";
		case API_PropertyGuidValueType:		return "Guid";
		default:							return "Undefined";
	}
}


GS::UniString MeasureTypeName (API_PropertyMeasureType type)
{
	switch (type) {
		case API_PropertyDefaultMeasureType:	return "Default";
		case API_PropertyLengthMeasureType:		return "Length";
		case API_PropertyAreaMeasureType:		return "Area";
		case API_PropertyVolumeMeasureType:		return "Volume";
		case API_PropertyAngleMeasureType:		return "Angle";
		default:								return "Undefined";
	}
}


bool ParseScalarType (const GS::UniString& raw, API_VariantType& valueType, API_PropertyMeasureType& measure)
{
	const GS::UniString name = Trimmed (raw);
	struct Entry { const char* name; API_VariantType vt; API_PropertyMeasureType mt; };
	static const Entry kEntries[] = {
		{ "string",		API_PropertyStringValueType,	API_PropertyDefaultMeasureType },
		{ "text",		API_PropertyStringValueType,	API_PropertyDefaultMeasureType },
		{ "integer",	API_PropertyIntegerValueType,	API_PropertyDefaultMeasureType },
		{ "int",		API_PropertyIntegerValueType,	API_PropertyDefaultMeasureType },
		{ "number",		API_PropertyRealValueType,		API_PropertyDefaultMeasureType },
		{ "real",		API_PropertyRealValueType,		API_PropertyDefaultMeasureType },
		{ "length",		API_PropertyRealValueType,		API_PropertyLengthMeasureType },
		{ "area",		API_PropertyRealValueType,		API_PropertyAreaMeasureType },
		{ "volume",		API_PropertyRealValueType,		API_PropertyVolumeMeasureType },
		{ "angle",		API_PropertyRealValueType,		API_PropertyAngleMeasureType },
		{ "boolean",	API_PropertyBooleanValueType,	API_PropertyDefaultMeasureType },
		{ "bool",		API_PropertyBooleanValueType,	API_PropertyDefaultMeasureType },
	};
	for (const Entry& e : kEntries) {
		if (EqualsIgnoreCase (name, e.name)) {
			valueType = e.vt;
			measure = e.mt;
			return true;
		}
	}
	return false;
}

// --- Variants ---------------------------------------------------------------------------------

bool VariantEquals (const API_Variant& a, const API_Variant& b)
{
	if (a.type != b.type)
		return false;
	switch (a.type) {
		case API_PropertyIntegerValueType:	return a.intValue == b.intValue;
		case API_PropertyRealValueType:		return a.doubleValue == b.doubleValue;
		case API_PropertyStringValueType:	return a.uniStringValue == b.uniStringValue;
		case API_PropertyBooleanValueType:	return a.boolValue == b.boolValue;
		case API_PropertyGuidValueType:		return a.guidValue == b.guidValue;
		default:							return true;
	}
}


static double OutReal (double v, API_PropertyMeasureType measure)
{
	return measure == API_PropertyAngleMeasureType ? RadToDeg (v) : v;
}


GS::UniString VariantToString (const API_Variant& v, API_PropertyMeasureType measure)
{
	switch (v.type) {
		case API_PropertyIntegerValueType:	return NumberToString ((double) v.intValue);
		case API_PropertyRealValueType:		return NumberToString (OutReal (v.doubleValue, measure));
		case API_PropertyStringValueType:	return v.uniStringValue;
		case API_PropertyBooleanValueType:	return v.boolValue ? "true" : "false";
		case API_PropertyGuidValueType:		return GuidStr (v.guidValue);
		default:							return GS::UniString ();
	}
}


void AddVariant (OS& out, const char* key, const API_Variant& v, API_PropertyMeasureType measure)
{
	switch (v.type) {
		case API_PropertyIntegerValueType:	out.Add (key, (Int32) v.intValue); break;
		case API_PropertyRealValueType:		out.Add (key, OutReal (v.doubleValue, measure)); break;
		case API_PropertyStringValueType:	out.Add (key, v.uniStringValue); break;
		case API_PropertyBooleanValueType:	out.Add (key, v.boolValue); break;
		case API_PropertyGuidValueType:		out.Add (key, GuidStr (v.guidValue)); break;
		default:							break;
	}
}


void AddVariantList (OS& out, const char* key, const GS::Array<API_Variant>& variants,
					 API_VariantType valueType, API_PropertyMeasureType measure)
{
	bool homogeneous = true;
	for (const API_Variant& v : variants) {
		if (v.type != valueType)
			homogeneous = false;
	}
	if (homogeneous) {
		switch (valueType) {
			case API_PropertyIntegerValueType: {
				GS::Array<Int32> list;
				for (const API_Variant& v : variants) list.Push ((Int32) v.intValue);
				out.Add (key, list);
				return;
			}
			case API_PropertyRealValueType: {
				GS::Array<double> list;
				for (const API_Variant& v : variants) list.Push (OutReal (v.doubleValue, measure));
				out.Add (key, list);
				return;
			}
			case API_PropertyBooleanValueType: {
				GS::Array<bool> list;
				for (const API_Variant& v : variants) list.Push (v.boolValue);
				out.Add (key, list);
				return;
			}
			default:
				break;
		}
	}
	GS::Array<GS::UniString> list;
	for (const API_Variant& v : variants)
		list.Push (VariantToString (v, measure));
	out.Add (key, list);
}


API_Variant ScalarToVariant (const JScalar& j, API_VariantType valueType, API_PropertyMeasureType measure, const GS::UniString& what)
{
	API_Variant v;
	v.type = valueType;
	const GS::UniString typeName = ScalarTypeName (valueType, measure);
	auto bad = [&] () {
		Fail (what + ": expected a " + typeName + " value, got " + j.Describe () + ".", APIERR_BADVALUE);
	};

	switch (valueType) {
		case API_PropertyStringValueType:
			switch (j.kind) {
				case JScalar::String:	v.uniStringValue = j.s; break;
				case JScalar::Int:
				case JScalar::Real:		v.uniStringValue = NumberToString (j.Number ()); break;
				case JScalar::Bool:		v.uniStringValue = j.b ? "true" : "false"; break;
				default:				bad ();
			}
			break;

		case API_PropertyIntegerValueType: {
			double d = 0.0;
			if (j.IsNumber ())
				d = j.Number ();
			else if (j.kind != JScalar::String || !ParseNumberString (j.s, d))
				bad ();
			if (std::fabs (d - std::round (d)) > 1e-9 || std::fabs (d) > 2147483647.0)
				Fail (what + ": expected an integer, got " + j.Describe () + ".", APIERR_BADVALUE);
			v.intValue = (Int32) std::llround (d);
			break;
		}

		case API_PropertyRealValueType: {
			double d = 0.0;
			if (j.IsNumber ())
				d = j.Number ();
			else if (j.kind != JScalar::String || !ParseNumberString (j.s, d))
				bad ();
			v.doubleValue = measure == API_PropertyAngleMeasureType ? DegToRad (d) : d;
			break;
		}

		case API_PropertyBooleanValueType:
			if (j.kind == JScalar::Bool) {
				v.boolValue = j.b;
			} else if (j.kind == JScalar::Int && (j.i == 0 || j.i == 1)) {
				v.boolValue = j.i == 1;
			} else if (j.kind == JScalar::String) {
				const GS::UniString s = Trimmed (j.s);
				static const char* kTrue[] = { "true", "yes", "1", "да", "on" };
				static const char* kFalse[] = { "false", "no", "0", "нет", "off" };
				bool matched = false;
				for (const char* t : kTrue)  if (EqualsIgnoreCase (s, ToUni (t))) { v.boolValue = true; matched = true; }
				for (const char* f : kFalse) if (EqualsIgnoreCase (s, ToUni (f))) { v.boolValue = false; matched = true; }
				if (!matched)
					bad ();
			} else {
				bad ();
			}
			break;

		case API_PropertyGuidValueType:
			if (j.kind != JScalar::String || !LooksLikeGuid (j.s))
				bad ();
			v.guidValue = ToGuid (j.s);
			break;

		default:
			Fail (what + ": the property has an undefined value type and cannot be written.", APIERR_BADPROPERTY);
	}
	return v;
}

// --- Enumerations -------------------------------------------------------------------------------

const API_SingleEnumerationVariant* FindEnumOptionByKey (const API_PropertyDefinition& def, const API_Variant& key)
{
	for (const API_SingleEnumerationVariant& opt : def.possibleEnumValues) {
		if (VariantEquals (opt.keyVariant, key))
			return &opt;
	}
	return nullptr;
}


GS::Array<GS::UniString> EnumDisplayValues (const API_PropertyDefinition& def)
{
	GS::Array<GS::UniString> names;
	for (const API_SingleEnumerationVariant& opt : def.possibleEnumValues)
		names.Push (VariantToString (opt.displayVariant, def.measureType));
	return names;
}


const API_SingleEnumerationVariant& ResolveEnumOption (const API_PropertyDefinition& def, const JScalar& jIn, const GS::UniString& propertyName)
{
	JScalar j = jIn;
	if (j.kind == JScalar::Object) {
		if (j.obj.Contains ("key")) {
			const GS::UniString keyStr = GetString (j.obj, "key");
			for (const API_SingleEnumerationVariant& opt : def.possibleEnumValues) {
				if (EqualsIgnoreCase (VariantToString (opt.keyVariant, def.measureType), keyStr))
					return opt;
			}
			Fail ("Option key '" + keyStr + "' is not an option of property '" + propertyName + "'.", APIERR_BADVALUE);
		}
		j = ReadJScalar (j.obj, "value");
	}

	const auto& opts = def.possibleEnumValues;
	if (j.kind == JScalar::String) {
		const GS::UniString text = j.s;
		const GS::UniString trimmed = Trimmed (text);
		for (const auto& opt : opts)
			if (VariantToString (opt.displayVariant, def.measureType) == text)
				return opt;
		for (const auto& opt : opts)
			if (EqualsIgnoreCase (Trimmed (VariantToString (opt.displayVariant, def.measureType)), trimmed))
				return opt;
		for (const auto& opt : opts)
			if (opt.nonLocalizedValue.HasValue () && EqualsIgnoreCase (opt.nonLocalizedValue.Get (), trimmed))
				return opt;
		for (const auto& opt : opts)
			if (opt.keyVariant.type == API_PropertyGuidValueType && EqualsIgnoreCase (GuidStr (opt.keyVariant.guidValue), trimmed))
				return opt;
		double d = 0.0;
		if (ParseNumberString (trimmed, d)) {
			JScalar num; num.kind = JScalar::Real; num.d = d;
			j = num;
		}
	}
	if (j.IsNumber ()) {
		for (const auto& opt : opts) {
			const API_Variant& dv = opt.displayVariant;
			if (dv.type == API_PropertyIntegerValueType && std::fabs ((double) dv.intValue - j.Number ()) < 1e-9)
				return opt;
			if (dv.type == API_PropertyRealValueType && std::fabs (OutReal (dv.doubleValue, def.measureType) - j.Number ()) < 1e-9)
				return opt;
			if (dv.type == API_PropertyStringValueType && Trimmed (dv.uniStringValue) == NumberToString (j.Number ()))
				return opt;
		}
	}
	if (j.kind == JScalar::Bool) {
		for (const auto& opt : opts)
			if (opt.displayVariant.type == API_PropertyBooleanValueType && opt.displayVariant.boolValue == j.b)
				return opt;
	}
	Fail ("'" + jIn.Describe () + "' is not an option of property '" + propertyName + "'. Options: " +
		  JoinNames (EnumDisplayValues (def), 40) +
		  ". For a custom property, add the option first with modify_property_definitions (addEnumValues).", APIERR_BADVALUE);
}

// --- Property values -----------------------------------------------------------------------------

API_PropertyValue BuildPropertyValue (const API_PropertyDefinition& def, const JValue& jv, const GS::UniString& propertyName)
{
	if (!jv.present)
		Fail ("No value given for property '" + propertyName + "'.");
	if (jv.nested)
		Fail ("Property '" + propertyName + "': nested lists are not supported.", APIERR_BADVALUE);

	API_PropertyValue value;
	value.variantStatus = API_VariantStatusNormal;
	const GS::UniString what = "Property '" + propertyName + "'";

	switch (def.collectionType) {
		case API_PropertySingleCollectionType:
			if (jv.items.GetSize () != 1)
				Fail (what + " (" + PropertyTypeName (def) + ") takes a single value, got a list of " +
					  NumberToString ((double) jv.items.GetSize ()) + ".", APIERR_BADVALUE);
			value.singleVariant.variant = ScalarToVariant (jv.items[0], def.valueType, def.measureType, what);
			break;

		case API_PropertyListCollectionType:
			for (const JScalar& j : jv.items)
				value.listVariant.variants.Push (ScalarToVariant (j, def.valueType, def.measureType, what));
			break;

		case API_PropertySingleChoiceEnumerationCollectionType:
			if (jv.items.GetSize () != 1)
				Fail (what + " is a single-choice option set: give exactly one option. Options: " +
					  JoinNames (EnumDisplayValues (def), 40) + ".", APIERR_BADVALUE);
			value.singleVariant.variant = ResolveEnumOption (def, jv.items[0], propertyName).keyVariant;
			break;

		case API_PropertyMultipleChoiceEnumerationCollectionType:
			for (const JScalar& j : jv.items) {
				const API_Variant& key = ResolveEnumOption (def, j, propertyName).keyVariant;
				bool dup = false;
				for (const API_Variant& existing : value.listVariant.variants)
					if (VariantEquals (existing, key)) dup = true;
				if (!dup)
					value.listVariant.variants.Push (key);
			}
			break;

		default:
			Fail (what + " has an undefined collection type and cannot be written.", APIERR_BADPROPERTY);
	}
	return value;
}


void AddPropertyValue (OS& out, const char* key, const API_PropertyDefinition& def, const API_PropertyValue& value)
{
	switch (def.collectionType) {
		case API_PropertySingleCollectionType:
			AddVariant (out, key, value.singleVariant.variant, def.measureType);
			break;

		case API_PropertyListCollectionType:
			AddVariantList (out, key, value.listVariant.variants, def.valueType, def.measureType);
			break;

		case API_PropertySingleChoiceEnumerationCollectionType: {
			const API_SingleEnumerationVariant* opt = FindEnumOptionByKey (def, value.singleVariant.variant);
			AddVariant (out, key, opt != nullptr ? opt->displayVariant : value.singleVariant.variant, def.measureType);
			break;
		}

		case API_PropertyMultipleChoiceEnumerationCollectionType: {
			GS::Array<API_Variant> displays;
			for (const API_Variant& k : value.listVariant.variants) {
				const API_SingleEnumerationVariant* opt = FindEnumOptionByKey (def, k);
				displays.Push (opt != nullptr ? opt->displayVariant : k);
			}
			AddVariantList (out, key, displays, def.valueType, def.measureType);
			break;
		}

		default:
			break;
	}
}


OS PropertyCellJson (const API_Property& property, bool withDisplay)
{
	if (property.status == API_Property_NotAvailable)
		return OS ("status", GS::UniString ("NotAvailable"));
	if (property.status == API_Property_NotEvaluated)
		return OS ("status", GS::UniString ("NotEvaluated"));

	OS cell;
	const API_PropertyDefinition& def = property.definition;
	if (property.value.variantStatus == API_VariantStatusUserUndefined) {
		cell.Add ("status", GS::UniString ("Undefined"));
	} else if (property.value.variantStatus == API_VariantStatusNull) {
		cell.Add ("status", GS::UniString ("Empty"));
	} else {
		AddPropertyValue (cell, "value", def, property.value);
		if (withDisplay) {
			GS::UniString display;
			if (ACAPI_Property_GetPropertyValueString (property, &display) == NoError) {
				bool same = false;
				if (cell.IsString ("value")) {
					GS::UniString plain;
					cell.Get ("value", plain);
					same = plain == display;
				}
				if (!same)
					cell.Add ("display", display);
			}
		}
	}
	if (def.definitionType == API_PropertyCustomDefinitionType)
		cell.Add ("isDefault", property.isDefault);
	return cell;
}

// --- PropertyIndex ------------------------------------------------------------------------------------

PropertyIndex::PropertyIndex ()
{
	Reload ();
}


void PropertyIndex::Reload ()
{
	groups.Clear ();
	definitions.Clear ();
	Check (ACAPI_Property_GetPropertyGroups (groups), "Cannot list the property groups");
	Check (ACAPI_Property_GetPropertyDefinitions (APINULLGuid, definitions), "Cannot list the property definitions");

	// Safety net: query groups that got no definition from the "all groups" call one by one
	// (cheap: usually only empty custom groups), so no definition can be missing from the index.
	GS::HashSet<API_Guid> known;
	GS::HashSet<API_Guid> groupsWithDefinitions;
	for (const API_PropertyDefinition& d : definitions) {
		known.Add (d.guid);
		groupsWithDefinitions.Add (d.groupGuid);
	}
	for (const API_PropertyGroup& g : groups) {
		if (groupsWithDefinitions.Contains (g.guid))
			continue;
		GS::Array<API_PropertyDefinition> groupDefinitions;
		if (ACAPI_Property_GetPropertyDefinitions (g.guid, groupDefinitions) != NoError)
			continue;
		for (const API_PropertyDefinition& d : groupDefinitions) {
			if (!known.Contains (d.guid)) {
				known.Add (d.guid);
				definitions.Push (d);
			}
		}
	}
	Rehash ();
}


void PropertyIndex::Rehash ()
{
	groupIndex.Clear ();
	definitionIndex.Clear ();
	for (UIndex i = 0; i < groups.GetSize (); ++i)
		groupIndex.Put (groups[i].guid, i);
	for (UIndex i = 0; i < definitions.GetSize (); ++i)
		definitionIndex.Put (definitions[i].guid, i);
}


void PropertyIndex::AddGroup (const API_PropertyGroup& group)
{
	groupIndex.Put (group.guid, groups.GetSize ());
	groups.Push (group);
}


void PropertyIndex::UpdateGroup (const API_PropertyGroup& group)
{
	if (const UIndex* i = groupIndex.GetPtr (group.guid))
		groups[*i] = group;
	else
		AddGroup (group);
}


void PropertyIndex::RemoveGroup (const API_Guid& guid)
{
	groups.DeleteAll ([&] (const API_PropertyGroup& g) { return g.guid == guid; });
	definitions.DeleteAll ([&] (const API_PropertyDefinition& d) { return d.groupGuid == guid; });
	Rehash ();
}


void PropertyIndex::AddDefinition (const API_PropertyDefinition& def)
{
	definitionIndex.Put (def.guid, definitions.GetSize ());
	definitions.Push (def);
}


void PropertyIndex::UpdateDefinition (const API_PropertyDefinition& def)
{
	if (const UIndex* i = definitionIndex.GetPtr (def.guid))
		definitions[*i] = def;
	else
		AddDefinition (def);
}


void PropertyIndex::RemoveDefinition (const API_Guid& guid)
{
	definitions.DeleteAll ([&] (const API_PropertyDefinition& d) { return d.guid == guid; });
	Rehash ();
}


const API_PropertyGroup* PropertyIndex::GroupByGuid (const API_Guid& guid) const
{
	const UIndex* i = groupIndex.GetPtr (guid);
	return i != nullptr ? &groups[*i] : nullptr;
}


const API_PropertyDefinition* PropertyIndex::DefinitionByGuid (const API_Guid& guid) const
{
	const UIndex* i = definitionIndex.GetPtr (guid);
	return i != nullptr ? &definitions[*i] : nullptr;
}


const API_PropertyGroup* PropertyIndex::FindGroupByName (const GS::UniString& rawName) const
{
	const GS::UniString name = Trimmed (rawName);
	for (const API_PropertyGroup& g : groups)
		if (g.name == name)
			return &g;
	for (const API_PropertyGroup& g : groups)
		if (EqualsIgnoreCase (Trimmed (g.name), name))
			return &g;
	return nullptr;
}


GS::UniString PropertyIndex::GroupName (const API_Guid& groupGuid) const
{
	const API_PropertyGroup* g = GroupByGuid (groupGuid);
	return g != nullptr ? g->name : GS::UniString ();
}


GS::UniString PropertyIndex::FullName (const API_PropertyDefinition& def) const
{
	return GroupName (def.groupGuid) + "/" + def.name;
}


USize PropertyIndex::DefinitionCount (const API_Guid& groupGuid) const
{
	USize n = 0;
	for (const API_PropertyDefinition& d : definitions)
		if (d.groupGuid == groupGuid)
			++n;
	return n;
}


const API_PropertyGroup& PropertyIndex::ResolveGroup (const JScalar& ref) const
{
	GS::UniString text;
	if (ref.kind == JScalar::String) {
		text = ref.s;
	} else if (ref.kind == JScalar::Object && ref.obj.Contains ("guid")) {
		text = GetString (ref.obj, "guid");
	} else if (ref.kind == JScalar::Object && ref.obj.Contains ("name")) {
		text = GetString (ref.obj, "name");
		if (const API_PropertyGroup* g = FindGroupByName (text))
			return *g;
		text = GS::UniString ();
	} else {
		Fail ("A property group reference must be a group name, a GUID or {\"guid\"}/{\"name\"}; got " + ref.Describe () + ".");
	}

	if (!text.IsEmpty ()) {
		if (LooksLikeGuid (text)) {
			if (const API_PropertyGroup* g = GroupByGuid (ToGuid (text)))
				return *g;
		} else if (const API_PropertyGroup* g = FindGroupByName (text)) {
			return *g;
		}
	}
	GS::Array<GS::UniString> custom;
	for (const API_PropertyGroup& g : groups)
		if (g.groupType == API_PropertyCustomGroupType)
			custom.Push (g.name);
	Fail ("Unknown property group " + ref.Describe () + ". Custom groups: " + JoinNames (custom, 40) +
		  ". Create it with create_property_groups (or list all with get_property_definitions {includeGroups: true}).", APIERR_BADID);
}


const API_PropertyDefinition& PropertyIndex::ResolveName (const GS::UniString& raw) const
{
	const GS::UniString text = Trimmed (raw);
	if (text.IsEmpty ())
		Fail ("Empty property reference.");

	if (LooksLikeGuid (text)) {
		if (const API_PropertyDefinition* d = DefinitionByGuid (ToGuid (text)))
			return *d;
		Fail ("No property definition with GUID " + text + ". List definitions with get_property_definitions.", APIERR_BADPROPERTY);
	}

	GS::Array<UIndex> exact;
	GS::Array<UIndex> caseless;
	auto addUnique = [] (GS::Array<UIndex>& list, UIndex i) { if (!list.Contains (i)) list.Push (i); };

	// "Group/Name" — try every "/" as the separator (names themselves may contain "/").
	for (UIndex pos = text.FindFirst ('/'); pos != MaxUIndex; pos = text.FindFirst ('/', pos + 1)) {
		const GS::UniString groupPart = Trimmed (text.GetSubstring (0, pos));
		const GS::UniString namePart = Trimmed (text.GetSubstring (pos + 1, text.GetLength () - pos - 1));
		if (groupPart.IsEmpty () || namePart.IsEmpty ())
			continue;
		for (UIndex i = 0; i < definitions.GetSize (); ++i) {
			const API_PropertyDefinition& d = definitions[i];
			const GS::UniString groupName = GroupName (d.groupGuid);
			if (d.name == namePart && groupName == groupPart)
				addUnique (exact, i);
			else if (EqualsIgnoreCase (Trimmed (d.name), namePart) && EqualsIgnoreCase (Trimmed (groupName), groupPart))
				addUnique (caseless, i);
		}
		if (pos + 1 >= text.GetLength ())
			break;
	}
	// Plain name.
	for (UIndex i = 0; i < definitions.GetSize (); ++i) {
		const API_PropertyDefinition& d = definitions[i];
		if (d.name == text)
			addUnique (exact, i);
		else if (EqualsIgnoreCase (Trimmed (d.name), text))
			addUnique (caseless, i);
	}

	const GS::Array<UIndex>& found = !exact.IsEmpty () ? exact : caseless;
	if (found.GetSize () == 1)
		return definitions[found[0]];

	if (found.GetSize () > 1) {
		GS::Array<GS::UniString> names;
		for (UIndex i : found)
			names.Push ("\"" + FullName (definitions[i]) + "\" (" + GuidStr (definitions[i].guid) + ")");
		Fail ("Property name '" + text + "' is ambiguous: " + JoinNames (names, 10) + ". Use \"Group/Name\" or the GUID.", APIERR_BADPROPERTY);
	}

	// Not found: suggest similar names.
	GS::UniString needle = text;
	const UIndex lastSlash = text.FindLast ('/');
	if (lastSlash != MaxUIndex && lastSlash + 1 < text.GetLength ())
		needle = Trimmed (text.GetSubstring (lastSlash + 1, text.GetLength () - lastSlash - 1));
	needle = needle.ToLowerCase ();
	GS::Array<GS::UniString> suggestions;
	for (const API_PropertyDefinition& d : definitions) {
		if (d.name.ToLowerCase ().Contains (needle))
			suggestions.Push ("\"" + FullName (d) + "\"");
		if (suggestions.GetSize () >= 10)
			break;
	}
	GS::UniString message = "Unknown property '" + text + "'.";
	if (!suggestions.IsEmpty ())
		message += " Similar: " + JoinNames (suggestions, 10) + ".";
	message += " Names are localized: use get_property_definitions {search: \"...\"} to find the exact \"Group/Name\" or GUID"
			   " (built-in properties can also be addressed language-independently with {\"builtIn\": \"General_ElementID\"}).";
	Fail (message, APIERR_BADPROPERTY);
}


const API_PropertyDefinition& PropertyIndex::ResolveDefinition (const JScalar& ref) const
{
	switch (ref.kind) {
		case JScalar::String:
			return ResolveName (ref.s);
		case JScalar::Object: {
			if (ref.obj.Contains ("guid"))
				return ResolveName (GetString (ref.obj, "guid"));
			if (ref.obj.Contains ("builtIn"))
				Fail ("{\"builtIn\": ...} property references must be resolved to GUIDs by the MCP server (use the MCP tools, "
					  "or API.GetPropertyIds of the official JSON API).");
			if (ref.obj.Contains ("name")) {
				const GS::UniString name = GetString (ref.obj, "name");
				if (ref.obj.Contains ("group")) {
					const GS::UniString groupName = GetString (ref.obj, "group");
					const API_PropertyGroup* group = FindGroupByName (groupName);
					if (group == nullptr)
						Fail ("Unknown property group '" + groupName + "'. Use get_property_definitions {includeGroups: true} to list groups.", APIERR_BADPROPERTY);
					const API_PropertyDefinition* caseless = nullptr;
					for (const API_PropertyDefinition& d : definitions) {
						if (d.groupGuid != group->guid)
							continue;
						if (d.name == name)
							return d;
						if (caseless == nullptr && EqualsIgnoreCase (Trimmed (d.name), Trimmed (name)))
							caseless = &d;
					}
					if (caseless != nullptr)
						return *caseless;
					return ResolveName (group->name + "/" + name);	// throws with suggestions
				}
				return ResolveName (name);
			}
			break;
		}
		default:
			break;
	}
	Fail ("A property reference must be a GUID, \"Group/Name\", a unique name, {\"guid\"} or {\"group\", \"name\"}; got " + ref.Describe () + ".", APIERR_BADPROPERTY);
}


const API_PropertyDefinition& PropertyIndex::ResolveDefinition (const OS& os, const char* key) const
{
	return ResolveDefinition (ReadJScalar (os, key));
}


GS::Array<API_PropertyDefinition> PropertyIndex::ResolveDefinitions (const OS& os, const char* key) const
{
	GS::Array<API_PropertyDefinition> result;
	const JValue jv = ReadJValue (os, key);
	if (!jv.present)
		return result;
	for (const JScalar& j : jv.items) {
		const API_PropertyDefinition& d = ResolveDefinition (j);
		bool dup = false;
		for (const API_PropertyDefinition& e : result)
			if (e.guid == d.guid) dup = true;
		if (!dup)
			result.Push (d);
	}
	return result;
}


OS PropertyIndex::DefinitionRef (const API_PropertyDefinition& def) const
{
	return OS ("guid", GuidStr (def.guid), "name", def.name, "group", GroupName (def.groupGuid), "type", PropertyTypeName (def));
}

// --- ClassificationIndex ---------------------------------------------------------------------------------

ClassificationIndex::ClassificationIndex ()
{
	Reload ();
}


void ClassificationIndex::Reload ()
{
	systems.Clear ();
	items.Clear ();
	itemIndex.Clear ();
	Check (ACAPI_Classification_GetClassificationSystems (systems), "Cannot list the classification systems");
	for (const API_ClassificationSystem& system : systems) {
		GS::Array<API_ClassificationItem> roots;
		if (ACAPI_Classification_GetClassificationSystemRootItems (system.guid, roots) == NoError)
			LoadChildren (system.guid, APINULLGuid, roots, 0);
	}
}


void ClassificationIndex::LoadChildren (const API_Guid& systemGuid, const API_Guid& parentGuid,
										const GS::Array<API_ClassificationItem>& children, Int32 depth)
{
	if (depth > 64)
		return;
	for (const API_ClassificationItem& child : children) {
		Item it;
		it.item = child;
		it.system = systemGuid;
		it.parent = parentGuid;
		it.depth = depth;
		itemIndex.Put (child.guid, items.GetSize ());
		items.Push (it);
		GS::Array<API_ClassificationItem> sub;
		if (ACAPI_Classification_GetClassificationItemChildren (child.guid, sub) == NoError && !sub.IsEmpty ())
			LoadChildren (systemGuid, child.guid, sub, depth + 1);
	}
}


void ClassificationIndex::AddItem (const API_ClassificationItem& item, const API_Guid& system, const API_Guid& parent)
{
	Int32 depth = 0;
	if (const Item* p = ItemByGuid (parent))
		depth = p->depth + 1;
	Item it;
	it.item = item;
	it.system = system;
	it.parent = parent;
	it.depth = depth;
	itemIndex.Put (item.guid, items.GetSize ());
	items.Push (it);
}


void ClassificationIndex::UpdateItem (const API_ClassificationItem& item)
{
	if (const UIndex* i = itemIndex.GetPtr (item.guid))
		items[*i].item = item;
}


const API_ClassificationSystem* ClassificationIndex::SystemByGuid (const API_Guid& guid) const
{
	for (const API_ClassificationSystem& s : systems)
		if (s.guid == guid)
			return &s;
	return nullptr;
}


const ClassificationIndex::Item* ClassificationIndex::ItemByGuid (const API_Guid& guid) const
{
	const UIndex* i = itemIndex.GetPtr (guid);
	return i != nullptr ? &items[*i] : nullptr;
}


GS::UniString ClassificationIndex::SystemLabel (const API_Guid& systemGuid) const
{
	const API_ClassificationSystem* s = SystemByGuid (systemGuid);
	if (s == nullptr)
		return GS::UniString ();
	return s->editionVersion.IsEmpty () ? s->name : s->name + " (" + s->editionVersion + ")";
}


const API_ClassificationSystem& ClassificationIndex::ResolveSystem (const JScalar& ref) const
{
	GS::UniString text;
	GS::UniString version;
	bool hasVersion = false;
	if (ref.kind == JScalar::String) {
		text = Trimmed (ref.s);
	} else if (ref.kind == JScalar::Object && ref.obj.Contains ("guid")) {
		text = Trimmed (GetString (ref.obj, "guid"));
	} else if (ref.kind == JScalar::Object && ref.obj.Contains ("name")) {
		text = Trimmed (GetString (ref.obj, "name"));
		if (ref.obj.Contains ("editionVersion")) {
			version = Trimmed (GetString (ref.obj, "editionVersion"));
			hasVersion = true;
		}
	} else {
		Fail ("A classification system reference must be a name, a GUID, {\"guid\"} or {\"name\", \"editionVersion\"?}; got " + ref.Describe () + ".");
	}

	if (LooksLikeGuid (text)) {
		if (const API_ClassificationSystem* s = SystemByGuid (ToGuid (text)))
			return *s;
	} else {
		for (int pass = 0; pass < 2; ++pass) {
			GS::Array<const API_ClassificationSystem*> found;
			for (const API_ClassificationSystem& s : systems) {
				const bool nameMatch = pass == 0 ? s.name == text : EqualsIgnoreCase (Trimmed (s.name), text);
				const bool labelMatch = !hasVersion && (EqualsIgnoreCase (s.name + " " + s.editionVersion, text) ||
														EqualsIgnoreCase (s.name + " (" + s.editionVersion + ")", text));
				if ((nameMatch && (!hasVersion || EqualsIgnoreCase (Trimmed (s.editionVersion), version))) || labelMatch)
					found.Push (&s);
			}
			if (found.GetSize () == 1)
				return *found[0];
			if (found.GetSize () > 1) {
				GS::Array<GS::UniString> labels;
				for (const API_ClassificationSystem* s : found)
					labels.Push (SystemLabel (s->guid) + " " + GuidStr (s->guid));
				Fail ("Classification system '" + text + "' is ambiguous: " + JoinNames (labels) +
					  ". Pass {\"name\", \"editionVersion\"} or the GUID.", APIERR_BADID);
			}
		}
	}
	GS::Array<GS::UniString> labels;
	for (const API_ClassificationSystem& s : systems)
		labels.Push (SystemLabel (s.guid));
	Fail ("Unknown classification system " + ref.Describe () + ". Existing systems: " + JoinNames (labels, 30) + ".", APIERR_BADID);
}


const ClassificationIndex::Item& ClassificationIndex::ResolveItem (const JScalar& ref, const API_ClassificationSystem* system) const
{
	GS::UniString text;
	const API_ClassificationSystem* scope = system;
	if (ref.kind == JScalar::String) {
		text = Trimmed (ref.s);
	} else if (ref.kind == JScalar::Object) {
		if (ref.obj.Contains ("system")) {
			JScalar sysRef = ReadJScalar (ref.obj, "system");
			scope = &ResolveSystem (sysRef);
		}
		if (ref.obj.Contains ("guid"))
			text = Trimmed (GetString (ref.obj, "guid"));
		else if (ref.obj.Contains ("id"))
			text = Trimmed (GetString (ref.obj, "id"));
		else
			Fail ("A classification item reference object needs \"guid\" or \"id\" (with optional \"system\").");
	} else {
		Fail ("A classification item reference must be a GUID, an item ID or {\"system\", \"id\"}; got " + ref.Describe () + ".");
	}

	if (LooksLikeGuid (text)) {
		const Item* it = ItemByGuid (ToGuid (text));
		if (it != nullptr && (scope == nullptr || it->system == scope->guid))
			return *it;
		if (it == nullptr) {
			Fail ("No classification item with GUID " + text + (scope != nullptr ? " in system " + SystemLabel (scope->guid) : GS::UniString ()) + ".", APIERR_BADID);
		}
		Fail ("Classification item " + text + " belongs to another system (" + SystemLabel (it->system) + ").", APIERR_BADID);
	}

	for (int pass = 0; pass < 2; ++pass) {
		GS::Array<const Item*> found;
		for (const Item& it : items) {
			if (scope != nullptr && it.system != scope->guid)
				continue;
			const bool match = pass == 0 ? it.item.id == text : EqualsIgnoreCase (Trimmed (it.item.id), text);
			if (match)
				found.Push (&it);
		}
		if (found.GetSize () == 1)
			return *found[0];
		if (found.GetSize () > 1) {
			GS::Array<GS::UniString> labels;
			for (const Item* it : found)
				labels.Push (SystemLabel (it->system) + ": " + it->item.id + " " + GuidStr (it->item.guid));
			Fail ("Classification item ID '" + text + "' exists in several systems: " + JoinNames (labels) +
				  ". Pass {\"system\": \"...\", \"id\": \"...\"} or the GUID.", APIERR_BADID);
		}
	}
	// Fallback: item name (Russian templates often have empty names, but try).
	for (const Item& it : items) {
		if (scope != nullptr && it.system != scope->guid)
			continue;
		if (!it.item.name.IsEmpty () && EqualsIgnoreCase (Trimmed (it.item.name), text))
			return it;
	}
	Fail ("Unknown classification item '" + text + "'" +
		  (scope != nullptr ? " in system " + SystemLabel (scope->guid) : GS::UniString ()) +
		  ToUni (". Item IDs are localized (e.g. 'Стена' in 'Классификация Archicad'); list them with get_classification_tree "
				 "(or get_classification_systems for the system names)."), APIERR_BADID);
}


GS::Array<API_Guid> ClassificationIndex::ResolveItemList (const JValue& refs) const
{
	GS::Array<API_Guid> result;
	auto add = [&] (const API_Guid& g) { if (!result.Contains (g)) result.Push (g); };
	for (const JScalar& j : refs.items) {
		if (j.kind == JScalar::Object && j.obj.Contains ("system") && !j.obj.Contains ("id") && !j.obj.Contains ("guid")) {
			const API_ClassificationSystem& system = ResolveSystem (ReadJScalar (j.obj, "system"));
			for (const API_Guid& g : SystemItemGuids (system.guid))
				add (g);
			continue;
		}
		add (ResolveItem (j).item.guid);
	}
	return result;
}


GS::Array<API_Guid> ClassificationIndex::AllItemGuids () const
{
	GS::Array<API_Guid> result;
	for (const Item& it : items)
		result.Push (it.item.guid);
	return result;
}


GS::Array<API_Guid> ClassificationIndex::SystemItemGuids (const API_Guid& systemGuid) const
{
	GS::Array<API_Guid> result;
	for (const Item& it : items)
		if (it.system == systemGuid)
			result.Push (it.item.guid);
	return result;
}


GS::Array<API_Guid> ClassificationIndex::Descendants (const API_Guid& itemGuid) const
{
	GS::Array<API_Guid> result;
	GS::Array<API_Guid> frontier;
	frontier.Push (itemGuid);
	while (!frontier.IsEmpty ()) {
		const API_Guid parent = frontier.Pop ();
		for (const Item& it : items) {
			if (it.parent == parent && it.parent != APINULLGuid) {
				result.Push (it.item.guid);
				frontier.Push (it.item.guid);
			}
		}
	}
	return result;
}


OS ClassificationIndex::ItemRef (const API_Guid& itemGuid) const
{
	OS ref ("guid", GuidStr (itemGuid));
	if (const Item* it = ItemByGuid (itemGuid)) {
		ref.Add ("id", it->item.id);
		if (!it->item.name.IsEmpty ())
			ref.Add ("name", it->item.name);
		ref.Add ("system", SystemLabel (it->system));
	}
	return ref;
}


OS ClassificationIndex::SystemJson (const API_ClassificationSystem& system)
{
	OS out ("guid", GuidStr (system.guid), "name", system.name, "editionVersion", system.editionVersion);
	if (!system.description.IsEmpty ())
		out.Add ("description", system.description);
	if (!system.source.IsEmpty ())
		out.Add ("source", system.source);
	out.Add ("editionDate", DateToString (system.editionDate));
	return out;
}

// --- Misc ----------------------------------------------------------------------------------------------------

GS::Array<API_Guid> ClassificationItemsOfElements (const GS::Array<API_Guid>& elements, GS::Array<GS::UniString>& warnings)
{
	GS::Array<API_Guid> result;
	for (const API_Guid& elem : elements) {
		GS::Array<GS::Pair<API_Guid, API_Guid>> pairs;
		const GSErrCode err = ACAPI_Element_GetClassificationItems (elem, pairs);
		if (err != NoError) {
			warnings.Push ("Cannot read the classification of element " + GuidStr (elem) + ": " + ErrorName (err));
			continue;
		}
		bool any = false;
		for (const auto& pair : pairs) {
			if (pair.second == APINULLGuid)
				continue;
			any = true;
			if (!result.Contains (pair.second))
				result.Push (pair.second);
		}
		if (!any)
			warnings.Push ("Element " + GuidStr (elem) + " has no classification, so custom properties cannot be available for it. "
						   "Classify it first (official classification tools), then call modify_property_definitions {availableForElements}.");
	}
	return result;
}


GSDateRecord ParseDate (const GS::UniString& text)
{
	int y = 0, m = 0, d = 0;
	const GS::String s = ToStr (Trimmed (text));
	if (std::sscanf (s.ToCStr (), "%d-%d-%d", &y, &m, &d) != 3 || y < 1 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31)
		Fail ("Invalid date '" + text + "': use YYYY-MM-DD.");
	return GSDateRecord ((unsigned short) y, (unsigned short) m, (unsigned short) d);
}


GS::UniString DateToString (const GSDateRecord& date)
{
	char buf[32];
	std::snprintf (buf, sizeof (buf), "%04u-%02u-%02u", (unsigned) date.year, (unsigned) date.month, (unsigned) date.day);
	return GS::UniString (buf);
}


GSDateRecord Today ()
{
	GSTimeRecord rec;
	TIGetTimeRecord (GSTime (), &rec, TI_CURRENT_TIME);
	return GSDateRecord (rec.year, rec.month, rec.day);
}


API_Guid NewGuid ()
{
	GS::Guid guid;
	guid.Generate ();
	return GSGuid2APIGuid (guid);
}


void CheckWithHint (GSErrCode err, const GS::UniString& what)
{
	if (err == NoError)
		return;
	GS::UniString hint;
	switch (err) {
		case APIERR_NAMEALREADYUSED:
			hint = " The name (or classification item ID) is already used; choose another one or modify the existing item.";
			break;
		case APIERR_BADNAME:
			hint = " Names, edition versions and classification item IDs must not be empty or whitespace only.";
			break;
		case APIERR_BADPARS:
			hint = " Archicad rejected the data as inconsistent (value type vs. default value / options, duplicate option keys, invalid parent/next item, or an invalid expression).";
			break;
		case APIERR_BADVALUE:
			hint = " The value does not match the property type.";
			break;
		case APIERR_BADEXPRESSION:
			hint = " Check the expression syntax (Archicad expression functions/operators); reference other properties as {ref:Group/Name}, {ref:GUID} "
				   "or {builtIn:General_Area} (converted automatically), or with the exact expressionReference from get_property_definitions {detail: 'full'}. "
				   "The referenced property's type must fit (e.g. no text in arithmetic).";
			break;
		case APIERR_PARMISSING:
			hint = " A required part is missing (e.g. an empty expression list).";
			break;
		case APIERR_BADID:
			hint = " A referenced GUID (group, definition, classification system/item, element) does not exist.";
			break;
		case APIERR_NOACCESSRIGHT:
		case APIERR_NOTMINE:
			hint = " In a Teamwork project, reserve the Property Manager / Classification Manager data (or the element) first.";
			break;
		case APIERR_READONLY:
			hint = " Built-in properties, groups and calculated values are read-only.";
			break;
		case APIERR_BADPROPERTY:
			hint = " The property is not available for this element/attribute (check its availability / the element's classification).";
			break;
		case APIERR_NOTEDITABLE:
			hint = " The element is not editable (locked, on a locked layer, or not reserved in Teamwork).";
			break;
		default:
			break;
	}
	throw Error (what + ": " + ErrorName (err) + "." + hint, err);
}

} // namespace props
} // namespace cc
