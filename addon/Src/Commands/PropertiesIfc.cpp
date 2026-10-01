// *****************************************************************************
// PropertiesIfc — IFC data of elements.
//
//   GetIfcData        {elements?: [guid], ifcGlobalIds?: [22-char IFC GUID], include?: [identity|type|properties|
//                      attributes|classificationReferences], propertySets?: [name], storedOnly?: false}
//   SetIfcProperties  {properties?: [{elements, propertySet, name, type?, valueType?, value | values | lower/upper |
//                      options, description?, remove?}], attributes?: [{elements, name, value | clear}],
//                      classificationReferences?: [{elements, referenceName?, identification?, name?, location?,
//                      source: {name, ...}, remove?}]}
//
// IFC values: {value, valueType} where valueType is an IFC type name (IfcLabel, IfcText, IfcBoolean,
// IfcInteger, IfcReal, IfcLengthMeasure, ...). Values are passed unchanged (no unit conversion).
// *****************************************************************************

#include "Commands/PropertiesCommon.hpp"
#include "Core/Command.hpp"
#include "Core/Elements.hpp"
#include "Core/Enums.hpp"

#include <cmath>

namespace cc {
namespace props {

namespace {

// --- Serialization ------------------------------------------------------------------------------

GS::UniString IfcPropertyTypeName (API_IFCPropertyType type)
{
	switch (type) {
		case API_IFCPropertySingleValueType:		return "Single";
		case API_IFCPropertyListValueType:			return "List";
		case API_IFCPropertyBoundedValueType:		return "Bounded";
		case API_IFCPropertyEnumeratedValueType:	return "Enumerated";
		case API_IFCPropertyTableValueType:			return "Table";
	}
	return "Unknown";
}


void AddIfcAnyValue (OS& out, const char* key, const API_IFCPropertyAnyValue& v)
{
	switch (v.primitiveType) {
		case API_IFCPropertyAnyValueIntegerType:	out.Add (key, v.intValue); break;
		case API_IFCPropertyAnyValueRealType:		out.Add (key, v.doubleValue); break;
		case API_IFCPropertyAnyValueBooleanType:	out.Add (key, v.boolValue); break;
		case API_IFCPropertyAnyValueLogicalType:
			if (!v.stringValue.IsEmpty ())
				out.Add (key, v.stringValue);
			else
				out.Add (key, v.boolValue);
			break;
		case API_IFCPropertyAnyValueStringType:		out.Add (key, v.stringValue); break;
	}
}


OS IfcValueJson (const API_IFCPropertyValue& v)
{
	OS out;
	AddIfcAnyValue (out, "value", v.value);
	out.Add ("valueType", v.valueType);
	return out;
}


GS::Array<OS> IfcValuesJson (const GS::Array<API_IFCPropertyValue>& values)
{
	GS::Array<OS> list;
	for (const API_IFCPropertyValue& v : values)
		list.Push (IfcValueJson (v));
	return list;
}


OS IfcPropertyJson (const API_IFCProperty& p)
{
	OS out ("propertySet", p.head.propertySetName, "name", p.head.propertyName);
	if (!p.head.propertyDescription.IsEmpty ())
		out.Add ("description", p.head.propertyDescription);
	out.Add ("type", IfcPropertyTypeName (p.head.propertyType));
	if (p.head.readOnly)
		out.Add ("readOnly", true);
	switch (p.head.propertyType) {
		case API_IFCPropertySingleValueType:
			AddIfcAnyValue (out, "value", p.singleValue.nominalValue.value);
			out.Add ("valueType", p.singleValue.nominalValue.valueType);
			break;
		case API_IFCPropertyListValueType:
			out.Add ("values", IfcValuesJson (p.listValue.listValues));
			break;
		case API_IFCPropertyBoundedValueType:
			if (!p.boundedValue.lowerBoundValue.valueType.IsEmpty ())
				out.Add ("lower", IfcValueJson (p.boundedValue.lowerBoundValue));
			if (!p.boundedValue.upperBoundValue.valueType.IsEmpty ())
				out.Add ("upper", IfcValueJson (p.boundedValue.upperBoundValue));
			break;
		case API_IFCPropertyEnumeratedValueType:
			out.Add ("values", IfcValuesJson (p.enumeratedValue.enumerationValues));
			out.Add ("options", IfcValuesJson (p.enumeratedValue.enumerationReference));
			break;
		case API_IFCPropertyTableValueType:
			out.Add ("definingValues", IfcValuesJson (p.tableValue.definingValues));
			out.Add ("definedValues", IfcValuesJson (p.tableValue.definedValues));
			break;
	}
	return out;
}


OS IfcAttributeJson (const API_IFCAttribute& a)
{
	OS out ("name", a.attributeName, "type", a.attributeType);
	if (a.hasValue)
		out.Add ("value", a.attributeValue);
	else
		out.Add ("hasValue", false);
	if (a.readOnly)
		out.Add ("readOnly", true);
	return out;
}


void AddOptional (OS& out, const char* key, const GS::Optional<GS::UniString>& value)
{
	if (value.HasValue () && !value.Get ().IsEmpty ())
		out.Add (key, value.Get ());
}


OS IfcClassificationReferenceJson (const API_IFCClassificationReference& r)
{
	OS out ("referenceName", r.referenceName);
	AddOptional (out, "identification", r.identification);
	AddOptional (out, "name", r.name);
	AddOptional (out, "location", r.location);
	if (r.readOnly)
		out.Add ("readOnly", true);
	OS source ("name", r.referencedSource.name);
	AddOptional (source, "source", r.referencedSource.source);
	AddOptional (source, "edition", r.referencedSource.edition);
	AddOptional (source, "editionDate", r.referencedSource.editionDate);
	AddOptional (source, "description", r.referencedSource.description);
	AddOptional (source, "location", r.referencedSource.location);
	out.Add ("source", source);
	return out;
}


GS::UniString IfcGuidString (const API_Guid& guid)
{
	GS::UniString s;
	if (ACAPI_IFC_APIGuidToIFCGuid (guid, s) != NoError)
		return GS::UniString ();
	return s;
}


bool Includes (const GS::Array<GS::UniString>& include, const char* what)
{
	if (include.IsEmpty ())
		return true;
	for (const GS::UniString& s : include)
		if (EqualsIgnoreCase (s, what))
			return true;
	return false;
}


OS IfcDataOfElement (const API_Guid& guid, const GS::Array<GS::UniString>& include, const GS::Array<GS::UniString>& psetFilter, bool storedOnly)
{
	if (!ElementExists (guid))
		Fail ("Element " + GuidStr (guid) + " does not exist.", APIERR_BADID);
	OS out ("guid", GuidStr (guid));

	if (Includes (include, "identity")) {
		API_Guid archicadId = APINULLGuid, externalId = APINULLGuid;
		const GSErrCode err = ACAPI_Element_GetIFCIdentifier (guid, archicadId, externalId);
		if (err == NoError) {
			out.Add ("ifcGlobalId", IfcGuidString (archicadId));
			out.Add ("archicadIfcId", GuidStr (archicadId));
			if (externalId != APINULLGuid) {
				out.Add ("externalIfcGlobalId", IfcGuidString (externalId));
				out.Add ("externalIfcId", GuidStr (externalId));
			}
		} else {
			out.Add ("identityError", ErrorName (err));
		}
	}

	if (Includes (include, "type")) {
		GS::UniString ifcType, typeObjectType;
		const GSErrCode err = ACAPI_Element_GetIFCType (guid, &ifcType, &typeObjectType);
		if (err == NoError) {
			out.Add ("ifcType", ifcType);
			if (!typeObjectType.IsEmpty ())
				out.Add ("typeObjectIfcType", typeObjectType);
		} else {
			out.Add ("typeError", ErrorName (err));
		}
	}

	if (Includes (include, "properties")) {
		GS::Array<API_IFCProperty> properties;
		const GSErrCode err = ACAPI_Element_GetIFCProperties (guid, storedOnly, &properties);
		if (err == NoError) {
			GS::Array<OS> list;
			for (const API_IFCProperty& p : properties) {
				if (!psetFilter.IsEmpty ()) {
					bool match = false;
					for (const GS::UniString& f : psetFilter)
						if (EqualsIgnoreCase (Trimmed (f), p.head.propertySetName))
							match = true;
					if (!match)
						continue;
				}
				list.Push (IfcPropertyJson (p));
			}
			out.Add ("properties", list);
		} else {
			out.Add ("propertiesError", ErrorName (err));
		}
	}

	if (Includes (include, "attributes")) {
		GS::Array<API_IFCAttribute> attributes;
		const GSErrCode err = ACAPI_Element_GetIFCAttributes (guid, storedOnly, &attributes);
		if (err == NoError) {
			GS::Array<OS> list;
			for (const API_IFCAttribute& a : attributes)
				list.Push (IfcAttributeJson (a));
			out.Add ("attributes", list);
		} else {
			out.Add ("attributesError", ErrorName (err));
		}
	}

	if (Includes (include, "classificationReferences")) {
		GS::Array<API_IFCClassificationReference> refs;
		const GSErrCode err = ACAPI_Element_GetIFCClassificationReferences (guid, storedOnly, &refs);
		if (err == NoError) {
			GS::Array<OS> list;
			for (const API_IFCClassificationReference& r : refs)
				list.Push (IfcClassificationReferenceJson (r));
			out.Add ("classificationReferences", list);
		} else {
			out.Add ("classificationReferencesError", ErrorName (err));
		}
	}
	return out;
}

// --- Parsing ----------------------------------------------------------------------------------------

const char* const kIfcTypeHint =
	"Use an IFC value type name such as IfcLabel, IfcText, IfcIdentifier, IfcBoolean, IfcLogical, IfcInteger, IfcReal, "
	"IfcCountMeasure, IfcLengthMeasure, IfcPositiveLengthMeasure, IfcAreaMeasure, IfcVolumeMeasure, IfcPlaneAngleMeasure, "
	"IfcMassMeasure, IfcThermalTransmittanceMeasure.";


API_IFCPropertyValue BuildIfcValue (const JScalar& jIn, const GS::UniString& defaultValueType, const GS::UniString& what)
{
	JScalar j = jIn;
	GS::UniString valueType = defaultValueType;
	if (j.kind == JScalar::Object) {
		if (j.obj.Contains ("valueType"))
			valueType = Trimmed (GetString (j.obj, "valueType"));
		j = ReadJScalar (j.obj, "value");
	}
	if (valueType.IsEmpty ()) {
		switch (j.kind) {
			case JScalar::Bool:		valueType = "IfcBoolean"; break;
			case JScalar::Int:		valueType = "IfcInteger"; break;
			case JScalar::Real:		valueType = "IfcReal"; break;
			default:				valueType = "IfcLabel"; break;
		}
	}

	API_IFCPropertyValue v;
	v.valueType = valueType;
	v.value.intValue = 0;
	v.value.doubleValue = 0.0;
	v.value.boolValue = false;
	API_IFCPropertyValuePrimitiveType primitive = API_IFCPropertyAnyValueStringType;
	if (ACAPI_Element_GetIFCPropertyValuePrimitiveType (valueType, &primitive) != NoError)
		Fail (what + ": unknown IFC value type '" + valueType + "'. " + kIfcTypeHint);
	v.value.primitiveType = primitive;

	switch (primitive) {
		case API_IFCPropertyAnyValueIntegerType: {
			double d = 0.0;
			if (j.IsNumber ())
				d = j.Number ();
			else if (j.kind != JScalar::String || !ParseNumberString (j.s, d))
				Fail (what + ": " + valueType + " needs an integer, got " + j.Describe () + ".", APIERR_BADVALUE);
			if (std::fabs (d - std::round (d)) > 1e-9)
				Fail (what + ": " + valueType + " needs an integer, got " + j.Describe () + ".", APIERR_BADVALUE);
			v.value.intValue = (Int64) std::llround (d);
			break;
		}
		case API_IFCPropertyAnyValueRealType: {
			double d = 0.0;
			if (j.IsNumber ())
				d = j.Number ();
			else if (j.kind != JScalar::String || !ParseNumberString (j.s, d))
				Fail (what + ": " + valueType + " needs a number, got " + j.Describe () + ".", APIERR_BADVALUE);
			v.value.doubleValue = d;
			break;
		}
		case API_IFCPropertyAnyValueBooleanType:
		case API_IFCPropertyAnyValueLogicalType: {
			const API_Variant b = ScalarToVariant (j, API_PropertyBooleanValueType, API_PropertyDefaultMeasureType, what);
			v.value.boolValue = b.boolValue;
			v.value.intValue = b.boolValue ? 1 : 0;
			break;
		}
		case API_IFCPropertyAnyValueStringType: {
			const API_Variant s = ScalarToVariant (j, API_PropertyStringValueType, API_PropertyDefaultMeasureType, what);
			v.value.stringValue = s.uniStringValue;
			break;
		}
	}
	return v;
}


GS::Array<API_IFCPropertyValue> BuildIfcValues (const OS& spec, const char* key, const GS::UniString& valueType, const GS::UniString& what)
{
	GS::Array<API_IFCPropertyValue> values;
	for (const JScalar& j : ReadJValue (spec, key).items)
		values.Push (BuildIfcValue (j, valueType, what));
	return values;
}


const NamedValue kIfcPropertyTypes[] = {
	{ "Single",		API_IFCPropertySingleValueType },
	{ "List",		API_IFCPropertyListValueType },
	{ "Bounded",	API_IFCPropertyBoundedValueType },
	{ "Enumerated",	API_IFCPropertyEnumeratedValueType },
	{ "Table",		API_IFCPropertyTableValueType },
};


void InitIfcValue (API_IFCPropertyValue& v)
{
	v.value.primitiveType = API_IFCPropertyAnyValueStringType;
	v.value.doubleValue = 0.0;
	v.value.intValue = 0;
	v.value.boolValue = false;
}


API_IFCProperty BuildIfcProperty (const OS& spec)
{
	API_IFCProperty p;
	InitIfcValue (p.singleValue.nominalValue);
	InitIfcValue (p.boundedValue.lowerBoundValue);
	InitIfcValue (p.boundedValue.upperBoundValue);
	p.head.propertySetName = Trimmed (GetString (spec, "propertySet"));
	p.head.propertyName = Trimmed (GetString (spec, "name"));
	p.head.propertyDescription = GetString (spec, "description", GS::UniString ());
	p.head.readOnly = false;
	if (p.head.propertySetName.IsEmpty () || p.head.propertyName.IsEmpty ())
		Fail ("IFC properties need a non-empty propertySet and name.");
	const GS::UniString what = "IFC property '" + p.head.propertySetName + "." + p.head.propertyName + "'";
	const GS::UniString valueType = Trimmed (GetString (spec, "valueType", GS::UniString ()));

	API_IFCPropertyType type = API_IFCPropertySingleValueType;
	if (spec.Contains ("type")) {
		const GS::UniString typeName = Trimmed (GetString (spec, "type"));
		bool known = false;
		for (const NamedValue& nv : kIfcPropertyTypes) {
			if (EqualsIgnoreCase (typeName, nv.name) || EqualsIgnoreCase (typeName, GS::UniString (nv.name) + "Value")) {
				type = (API_IFCPropertyType) nv.value;
				known = true;
			}
		}
		if (!known)
			Fail (what + ": type must be Single, List, Bounded, Enumerated or Table.");
	} else if (spec.Contains ("lower") || spec.Contains ("upper")) {
		type = API_IFCPropertyBoundedValueType;
	} else if (spec.Contains ("options")) {
		type = API_IFCPropertyEnumeratedValueType;
	} else if (spec.Contains ("values")) {
		type = API_IFCPropertyListValueType;
	}
	p.head.propertyType = type;

	switch (type) {
		case API_IFCPropertySingleValueType:
			p.singleValue.nominalValue = BuildIfcValue (ReadJScalar (spec, "value"), valueType, what);
			break;
		case API_IFCPropertyListValueType:
			if (!spec.Contains ("values"))
				Fail (what + ": a List property needs \"values\".");
			p.listValue.listValues = BuildIfcValues (spec, "values", valueType, what);
			break;
		case API_IFCPropertyBoundedValueType:
			if (!spec.Contains ("lower") && !spec.Contains ("upper"))
				Fail (what + ": a Bounded property needs \"lower\" and/or \"upper\".");
			if (spec.Contains ("lower"))
				p.boundedValue.lowerBoundValue = BuildIfcValue (ReadJScalar (spec, "lower"), valueType, what);
			if (spec.Contains ("upper"))
				p.boundedValue.upperBoundValue = BuildIfcValue (ReadJScalar (spec, "upper"), valueType, what);
			break;
		case API_IFCPropertyEnumeratedValueType:
			if (!spec.Contains ("values"))
				Fail (what + ": an Enumerated property needs \"values\" (the selected values) and usually \"options\".");
			p.enumeratedValue.enumerationValues = BuildIfcValues (spec, "values", valueType, what);
			if (spec.Contains ("options"))
				p.enumeratedValue.enumerationReference = BuildIfcValues (spec, "options", valueType, what);
			break;
		case API_IFCPropertyTableValueType:
			if (!spec.Contains ("definingValues") || !spec.Contains ("definedValues"))
				Fail (what + ": a Table property needs \"definingValues\" and \"definedValues\".");
			p.tableValue.definingValues = BuildIfcValues (spec, "definingValues", GetString (spec, "definingValueType", valueType), what);
			p.tableValue.definedValues = BuildIfcValues (spec, "definedValues", GetString (spec, "definedValueType", valueType), what);
			if (p.tableValue.definingValues.GetSize () != p.tableValue.definedValues.GetSize ())
				Fail (what + ": definingValues and definedValues must have the same length.");
			break;
	}
	return p;
}


const API_IFCProperty* FindIfcProperty (const GS::Array<API_IFCProperty>& list, const GS::UniString& pset, const GS::UniString& name)
{
	for (const API_IFCProperty& p : list)
		if (EqualsIgnoreCase (p.head.propertySetName, pset) && EqualsIgnoreCase (p.head.propertyName, name))
			return &p;
	return nullptr;
}


OS TryWithGuid (const API_Guid& guid, const std::function<void ()>& fn)
{
	OS r = Try ([&] () -> OS { fn (); return OS ("ok", true); });
	if (r.Contains ("error")) {
		OS error;
		r.Get ("error", error);
		return OS ("guid", GuidStr (guid), "error", error);
	}
	return r;
}


OS SetIfcPropertyEntry (const OS& spec)
{
	return Try ([&] () -> OS {
		const GS::Array<API_Guid> elements = GetGuidArray (spec, "elements");
		const bool remove = GetBool (spec, "remove", false);
		const GS::UniString pset = Trimmed (GetString (spec, "propertySet"));
		const GS::UniString name = Trimmed (GetString (spec, "name"));
		const GS::UniString label = pset + "." + name;
		API_IFCProperty property;
		if (!remove)
			property = BuildIfcProperty (spec);

		Int32 succeeded = 0;
		GS::Array<OS> failed;
		for (const API_Guid& guid : elements) {
			const OS r = TryWithGuid (guid, [&] () {
				if (!ElementExists (guid))
					Fail ("Element does not exist.", APIERR_BADID);
				if (remove) {
					GS::Array<API_IFCProperty> stored;
					CheckWithHint (ACAPI_Element_GetIFCProperties (guid, true, &stored), "Cannot read the IFC properties");
					const API_IFCProperty* existing = FindIfcProperty (stored, pset, name);
					if (existing == nullptr)
						Fail ("No stored IFC property '" + label + "' on this element (only properties stored on the element can be removed; "
							  "mapped/translator properties come from the IFC translator settings).", APIERR_BADPROPERTY);
					CheckWithHint (ACAPI_Element_RemoveIFCProperty (guid, *existing), "Cannot remove IFC property '" + label + "'");
				} else {
					GS::Array<API_IFCProperty> all;
					if (ACAPI_Element_GetIFCProperties (guid, false, &all) == NoError) {
						const API_IFCProperty* existing = FindIfcProperty (all, pset, name);
						if (existing != nullptr && existing->head.readOnly)
							Fail ("IFC property '" + label + "' is read-only on this element (computed by Archicad / the IFC translator).", APIERR_READONLY);
					}
					CheckWithHint (ACAPI_Element_SetIFCProperty (guid, property), "Cannot set IFC property '" + label + "'");
				}
			});
			if (r.Contains ("error"))
				failed.Push (r);
			else
				++succeeded;
		}
		OS out ("propertySet", pset, "name", name, "succeeded", succeeded);
		if (remove)
			out.Add ("removed", true);
		if (!failed.IsEmpty ())
			out.Add ("failed", failed);
		return out;
	});
}


OS SetIfcAttributeEntry (const OS& spec)
{
	return Try ([&] () -> OS {
		const GS::Array<API_Guid> elements = GetGuidArray (spec, "elements");
		const GS::UniString name = Trimmed (GetString (spec, "name"));
		const bool clear = GetBool (spec, "clear", false);
		GS::UniString value;
		if (!clear) {
			const API_Variant v = ScalarToVariant (ReadJScalar (spec, "value"), API_PropertyStringValueType, API_PropertyDefaultMeasureType,
												   "IFC attribute '" + name + "'");
			value = v.uniStringValue;
		}

		Int32 succeeded = 0;
		GS::Array<OS> failed;
		for (const API_Guid& guid : elements) {
			const OS r = TryWithGuid (guid, [&] () {
				if (!ElementExists (guid))
					Fail ("Element does not exist.", APIERR_BADID);
				GS::Array<API_IFCAttribute> attributes;
				CheckWithHint (ACAPI_Element_GetIFCAttributes (guid, false, &attributes), "Cannot read the IFC attributes");
				API_IFCAttribute* target = nullptr;
				GS::Array<GS::UniString> names;
				for (API_IFCAttribute& a : attributes) {
					names.Push (a.attributeName);
					if (target == nullptr && EqualsIgnoreCase (a.attributeName, name))
						target = &a;
				}
				if (target == nullptr)
					Fail ("The element has no IFC attribute '" + name + "'. Available: " + JoinNames (names, 30) + ".", APIERR_BADPROPERTY);
				if (target->readOnly)
					Fail ("IFC attribute '" + name + "' is read-only (e.g. GlobalId is derived from the element).", APIERR_READONLY);
				target->attributeValue = value;
				target->hasValue = !clear;
				CheckWithHint (ACAPI_Element_SetIFCAttribute (guid, *target), "Cannot set IFC attribute '" + name + "'");
			});
			if (r.Contains ("error"))
				failed.Push (r);
			else
				++succeeded;
		}
		OS out ("name", name, "succeeded", succeeded);
		if (!failed.IsEmpty ())
			out.Add ("failed", failed);
		return out;
	});
}

void ReadOptionalString (GS::Optional<GS::UniString>& target, const OS& spec, const char* key)
{
	if (auto v = OptString (spec, key))
		target = *v;
}


// referenceName is the key of a classification reference on an element; it defaults to the
// identification (item code) or the name.
GS::UniString ClassificationReferenceKey (const OS& spec)
{
	GS::UniString key = Trimmed (GetString (spec, "referenceName", GS::UniString ()));
	if (key.IsEmpty ())
		key = Trimmed (GetString (spec, "identification", GS::UniString ()));
	if (key.IsEmpty ())
		key = Trimmed (GetString (spec, "name", GS::UniString ()));
	if (key.IsEmpty ())
		Fail ("IFC classification references need referenceName, identification or name.");
	return key;
}


API_IFCClassificationReference BuildIfcClassificationReference (const OS& spec, const GS::UniString& key)
{
	API_IFCClassificationReference r;
	r.referenceName = key;
	r.readOnly = false;
	ReadOptionalString (r.identification, spec, "identification");
	ReadOptionalString (r.name, spec, "name");
	ReadOptionalString (r.location, spec, "location");
	OS source;
	if (!TryGetObject (spec, "source", source))
		Fail ("IFC classification reference '" + key + "' needs \"source\": {name, source?, edition?, editionDate?, description?, location?} "
			  "(the classification system, e.g. {name: 'Uniclass', edition: '2015'}).");
	r.referencedSource.name = Trimmed (GetString (source, "name"));
	if (r.referencedSource.name.IsEmpty ())
		Fail ("IFC classification reference '" + key + "': source.name must not be empty.");
	ReadOptionalString (r.referencedSource.source, source, "source");
	ReadOptionalString (r.referencedSource.edition, source, "edition");
	ReadOptionalString (r.referencedSource.editionDate, source, "editionDate");
	ReadOptionalString (r.referencedSource.description, source, "description");
	ReadOptionalString (r.referencedSource.location, source, "location");
	return r;
}


bool MatchesClassificationReference (const API_IFCClassificationReference& r, const GS::UniString& key, const GS::UniString& sourceName)
{
	if (!sourceName.IsEmpty () && !EqualsIgnoreCase (Trimmed (r.referencedSource.name), sourceName))
		return false;
	if (EqualsIgnoreCase (Trimmed (r.referenceName), key))
		return true;
	return r.identification.HasValue () && EqualsIgnoreCase (Trimmed (r.identification.Get ()), key);
}


OS SetIfcClassificationReferenceEntry (const OS& spec)
{
	return Try ([&] () -> OS {
		const GS::Array<API_Guid> elements = GetGuidArray (spec, "elements");
		const bool remove = GetBool (spec, "remove", false);
		const GS::UniString key = ClassificationReferenceKey (spec);
		GS::UniString sourceName;
		OS source;
		if (TryGetObject (spec, "source", source))
			sourceName = Trimmed (GetString (source, "name", GS::UniString ()));
		API_IFCClassificationReference reference;
		if (!remove)
			reference = BuildIfcClassificationReference (spec, key);

		Int32 succeeded = 0;
		GS::Array<OS> failed;
		for (const API_Guid& guid : elements) {
			const OS r = TryWithGuid (guid, [&] () {
				if (!ElementExists (guid))
					Fail ("Element does not exist.", APIERR_BADID);
				GS::Array<API_IFCClassificationReference> existing;
				const GSErrCode readErr = ACAPI_Element_GetIFCClassificationReferences (guid, remove, &existing);
				if (remove) {
					CheckWithHint (readErr, "Cannot read the IFC classification references");
					const API_IFCClassificationReference* found = nullptr;
					GS::Array<GS::UniString> names;
					for (const API_IFCClassificationReference& e : existing) {
						names.Push (e.referenceName);
						if (found == nullptr && MatchesClassificationReference (e, key, sourceName))
							found = &e;
					}
					if (found == nullptr)
						Fail ("No stored IFC classification reference '" + key + "' on this element" +
							  (names.IsEmpty () ? GS::UniString (" (it has none).") : ". Stored: " + JoinNames (names, 20) + ".") +
							  " References derived from Archicad classifications cannot be removed here; change the element's classification instead.",
							  APIERR_BADPROPERTY);
					CheckWithHint (ACAPI_Element_RemoveIFCClassificationReference (guid, *found), "Cannot remove IFC classification reference '" + key + "'");
				} else {
					if (readErr == NoError) {
						for (const API_IFCClassificationReference& e : existing) {
							if (MatchesClassificationReference (e, key, sourceName) && e.readOnly)
								Fail ("IFC classification reference '" + key + "' is read-only on this element (derived from its Archicad classification).", APIERR_READONLY);
						}
					}
					CheckWithHint (ACAPI_Element_SetIFCClassificationReference (guid, reference), "Cannot set IFC classification reference '" + key + "'");
				}
			});
			if (r.Contains ("error"))
				failed.Push (r);
			else
				++succeeded;
		}
		OS out ("referenceName", key, "succeeded", succeeded);
		if (remove)
			out.Add ("removed", true);
		if (!failed.IsEmpty ())
			out.Add ("failed", failed);
		return out;
	});
}

} // namespace


void RegisterIfcCommands ()
{
	RegisterCommand ("GetIfcData",
		"Returns IFC data of elements: ifcGlobalId (22-char IFC GUID), archicadIfcId, externalIfcGlobalId (imported elements), "
		"ifcType, typeObjectIfcType, properties (psets), attributes, classificationReferences. Input: {elements?: [guid], "
		"ifcGlobalIds?: [IFC GUID] (find elements by IFC GlobalId), include?: [identity|type|properties|attributes|"
		"classificationReferences], propertySets?: [name] (filter), storedOnly?: false (true = only data stored on the element, "
		"false = also values computed by the IFC translator)}.",
		[] (const OS& params) -> OS {
			GS::Array<API_Guid> elements = GetGuidArray (params, "elements", false);
			const GS::Array<GS::UniString> include = GetStringArray (params, "include", false);
			for (const GS::UniString& s : include) {
				bool known = false;
				for (const char* k : { "identity", "type", "properties", "attributes", "classificationReferences" })
					if (EqualsIgnoreCase (s, k))
						known = true;
				if (!known)
					Fail ("Unknown include item '" + s + "'. Allowed: identity, type, properties, attributes, classificationReferences.");
			}
			const GS::Array<GS::UniString> psetFilter = GetStringArray (params, "propertySets", false);
			const bool storedOnly = GetBool (params, "storedOnly", false);

			GS::Array<OS> lookup;
			for (const GS::UniString& ifcId : GetStringArray (params, "ifcGlobalIds", false)) {
				OS entry ("ifcGlobalId", ifcId);
				API_Guid apiGuid = APINULLGuid;
				if (ACAPI_IFC_IFCGuidToAPIGuid (Trimmed (ifcId), apiGuid) != NoError || apiGuid == APINULLGuid) {
					entry.Add ("error", OS ("code", (Int32) APIERR_BADPARS, "message",
											GS::UniString ("Invalid IFC GlobalId (expected the 22-character IFC GUID, e.g. 2Hn3tQ$Pz0Hwv2k1cA7t2W).")));
					lookup.Push (entry);
					continue;
				}
				GS::Array<API_Guid> found;
				GS::Array<API_Guid> byArchicadId, byExternalId;
				if (ACAPI_Element_GetElemListByIFCIdentifier (&apiGuid, nullptr, byArchicadId) == NoError)
					found.Append (byArchicadId);
				if (ACAPI_Element_GetElemListByIFCIdentifier (nullptr, &apiGuid, byExternalId) == NoError)
					for (const API_Guid& g : byExternalId)
						if (!found.Contains (g))
							found.Push (g);
				GS::Array<GS::UniString> guids;
				for (const API_Guid& g : found) {
					guids.Push (GuidStr (g));
					if (!elements.Contains (g))
						elements.Push (g);
				}
				entry.Add ("elements", guids);
				lookup.Push (entry);
			}
			if (elements.IsEmpty () && lookup.IsEmpty ())
				Fail ("Give elements (GUIDs) and/or ifcGlobalIds.");

			GS::Array<OS> results;
			for (const API_Guid& guid : elements) {
				OS r = Try ([&] () -> OS { return IfcDataOfElement (guid, include, psetFilter, storedOnly); });
				if (r.Contains ("error")) {
					OS error;
					r.Get ("error", error);
					r = OS ("guid", GuidStr (guid), "error", error);
				}
				results.Push (r);
			}
			OS out ("elements", results);
			if (!lookup.IsEmpty ())
				out.Add ("lookup", lookup);
			return out;
		});

	RegisterCommand ("SetIfcProperties",
		"Adds/changes/removes IFC properties stored on elements and sets IFC attributes, in one undo step. Input: {properties?: "
		"[{elements, propertySet, name, type?: Single|List|Bounded|Enumerated|Table, valueType? (IfcLabel, IfcReal, ...; inferred "
		"from the JSON value), value | values | lower/upper | options, description?, remove?: true}], attributes?: [{elements, "
		"name (e.g. Name, Description, ObjectType, Tag, LongName), value | clear: true}], classificationReferences?: [{elements, "
		"referenceName? (key; default identification or name), identification?, name?, location?, source: {name, source?, edition?, "
		"editionDate?, description?, location?}, remove?: true}]}.",
		[] (const OS& params) -> OS {
			const GS::Array<OS> properties = GetObjectArray (params, "properties", false);
			const GS::Array<OS> attributes = GetObjectArray (params, "attributes", false);
			const GS::Array<OS> references = GetObjectArray (params, "classificationReferences", false);
			if (properties.IsEmpty () && attributes.IsEmpty () && references.IsEmpty ())
				Fail ("Give properties, attributes and/or classificationReferences to set.");
			GS::Array<OS> propertyResults;
			GS::Array<OS> attributeResults;
			GS::Array<OS> referenceResults;
			Undoable (GetString (params, "undoName", "Set IFC properties"), [&] () {
				for (const OS& spec : properties)
					propertyResults.Push (SetIfcPropertyEntry (spec));
				for (const OS& spec : attributes)
					attributeResults.Push (SetIfcAttributeEntry (spec));
				for (const OS& spec : references)
					referenceResults.Push (SetIfcClassificationReferenceEntry (spec));
			});
			OS out;
			if (!properties.IsEmpty ())
				out.Add ("properties", propertyResults);
			if (!attributes.IsEmpty ())
				out.Add ("attributes", attributeResults);
			if (!references.IsEmpty ())
				out.Add ("classificationReferences", referenceResults);
			return out;
		});
}

} // namespace props
} // namespace cc
