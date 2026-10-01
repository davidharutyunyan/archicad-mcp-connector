// *****************************************************************************
// Properties — property groups and definitions (built-in + user-defined), plus
// registration of the whole "properties" family:
//   Properties.cpp               GetPropertyDefinitions, Create/Modify/DeletePropertyGroups,
//                                Create/Modify/DeletePropertyDefinitions, ImportPropertyDefinitionsXml
//   PropertiesValues.cpp         Get/SetPropertyValues (elements, tool defaults),
//                                Get/SetAttributePropertyValues
//   PropertiesIfc.cpp            GetIfcData, SetIfcProperties
//   PropertiesClassification.cpp CreateClassificationSystem, CreateClassificationItems, ...
//
// Property types (JSON "type"): string | integer | number | length | area | volume | angle |
// boolean, the same with a "List" suffix (stringList, lengthList, ...), singleEnum | multiEnum
// (option sets; option display values in "enumValues"). Lengths in m, areas m², volumes m³,
// angles in DEGREES.
//
// Availability: custom properties are shown only on elements whose classification item is in
// the definition's availability list. "availability": "all" (default on create) = every item
// of every classification system, "none", or a list of item refs (GUID | item ID |
// {system, id} | {system} = every item of that system).
// *****************************************************************************

#include "Commands/Commands.hpp"
#include "Commands/PropertiesCommon.hpp"
#include "Core/Command.hpp"
#include "Core/Elements.hpp"
#include "Core/Enums.hpp"

#include <algorithm>
#include <optional>

namespace cc {

using namespace props;

namespace {

// --- Serialization ----------------------------------------------------------------------------

bool IsEnum (const API_PropertyDefinition& def)
{
	return def.collectionType == API_PropertySingleChoiceEnumerationCollectionType ||
		   def.collectionType == API_PropertyMultipleChoiceEnumerationCollectionType;
}


OS DefinitionJson (const PropertyIndex& index, const API_PropertyDefinition& def, bool full, const ClassificationIndex* classifications)
{
	OS out = index.DefinitionRef (def);
	out.Add ("kind", DefinitionKindName (def.definitionType));
	out.Add ("editable", def.canValueBeEditable);
	if (!def.description.IsEmpty ())
		out.Add ("description", def.description);
	if (def.defaultValue.hasExpression)
		out.Add ("expressionBased", true);
	if (IsEnum (def))
		out.Add ("enumValues", EnumDisplayValues (def));
	if (def.definitionType == API_PropertyCustomDefinitionType)
		out.Add ("availabilityCount", (Int32) def.availability.GetSize ());

	if (!full)
		return out;

	out.Add ("groupGuid", GuidStr (def.groupGuid));
	out.Add ("collectionType", CollectionTypeName (def.collectionType));
	out.Add ("valueType", ValueTypeName (def.valueType));
	out.Add ("measureType", MeasureTypeName (def.measureType));

	if (def.defaultValue.hasExpression) {
		out.Add ("defaultExpressions", def.defaultValue.propertyExpressions);
	} else if (def.defaultValue.basicValue.variantStatus == API_VariantStatusNormal) {
		AddPropertyValue (out, "defaultValue", def, def.defaultValue.basicValue);
	} else {
		out.Add ("defaultValueStatus", GS::UniString (def.defaultValue.basicValue.variantStatus == API_VariantStatusUserUndefined ? "Undefined" : "Empty"));
	}

	if (IsEnum (def)) {
		GS::Array<OS> options;
		for (const API_SingleEnumerationVariant& opt : def.possibleEnumValues) {
			OS o;
			AddVariant (o, "value", opt.displayVariant, def.measureType);
			o.Add ("key", VariantToString (opt.keyVariant, API_PropertyDefaultMeasureType));
			if (opt.nonLocalizedValue.HasValue ())
				o.Add ("nonLocalizedValue", opt.nonLocalizedValue.Get ());
			options.Push (o);
		}
		out.Add ("enumOptions", options);
	}

	API_PropertyDefinition copy = def;
	GS::UniString reference;
	if (ACAPI_Goodies (APIAny_GetPropertyExprReferenceStringID, &copy, &reference) == NoError && !reference.IsEmpty ())
		out.Add ("expressionReference", reference);

	if (def.definitionType == API_PropertyCustomDefinitionType && classifications != nullptr) {
		GS::Array<OS> availability;
		for (const API_Guid& itemGuid : def.availability)
			availability.Push (classifications->ItemRef (itemGuid));
		out.Add ("availability", availability);
	}
	return out;
}


OS GroupJson (const PropertyIndex& index, const API_PropertyGroup& group)
{
	OS out ("guid", GuidStr (group.guid), "name", group.name, "kind", GroupKindName (group.groupType));
	if (!group.description.IsEmpty ())
		out.Add ("description", group.description);
	out.Add ("definitionCount", (Int32) index.DefinitionCount (group.guid));
	return out;
}

// --- Type / options / default / availability parsing -------------------------------------------

struct TypeSpec {
	API_PropertyCollectionType	collection = API_PropertySingleCollectionType;
	API_VariantType				valueType = API_PropertyStringValueType;
	API_PropertyMeasureType		measure = API_PropertyDefaultMeasureType;
};


bool MatchesAny (const GS::UniString& s, std::initializer_list<const char*> names)
{
	for (const char* n : names)
		if (EqualsIgnoreCase (s, n))
			return true;
	return false;
}


TypeSpec ParseTypeSpec (const OS& spec)
{
	const GS::UniString type = Trimmed (GetString (spec, "type"));
	TypeSpec t;
	const bool single = MatchesAny (type, { "singleEnum", "singleChoice", "optionSet", "enum", "singleOptionSet" });
	const bool multi = MatchesAny (type, { "multiEnum", "multipleChoice", "multiChoice", "multiOptionSet", "multipleEnum" });
	if (single || multi) {
		t.collection = single ? API_PropertySingleChoiceEnumerationCollectionType : API_PropertyMultipleChoiceEnumerationCollectionType;
		const GS::UniString optionType = GetString (spec, "enumValueType", "string");
		if (!ParseScalarType (optionType, t.valueType, t.measure))
			Fail ("Unknown enumValueType '" + optionType + "'. Allowed: string (default), integer, number, length, area, volume, angle, boolean.");
		return t;
	}
	if (spec.Contains ("enumValueType"))
		Fail ("enumValueType is only valid for type singleEnum / multiEnum.");

	GS::UniString base = type;
	if (type.GetLength () > 4 && EqualsIgnoreCase (GS::UniString (type.GetSuffix (4)), "List")) {
		base = GS::UniString (type.GetPrefix (type.GetLength () - 4));
		t.collection = API_PropertyListCollectionType;
	}
	if (!ParseScalarType (base, t.valueType, t.measure))
		Fail ("Unknown property type '" + type + "'. Allowed: string, integer, number, length, area, volume, angle, boolean, "
			  "the same with a List suffix (stringList, integerList, lengthList, ...), singleEnum, multiEnum.");
	return t;
}


GS::Array<API_SingleEnumerationVariant> BuildEnumOptions (const JValue& jv, const API_PropertyDefinition& def,
														  const GS::Array<API_SingleEnumerationVariant>* existing,
														  const GS::UniString& what)
{
	if (jv.nested)
		Fail (what + ": enumValues must be a flat list.");
	GS::Array<API_SingleEnumerationVariant> result;
	for (const JScalar& j : jv.items) {
		JScalar valueJ = j;
		GS::Optional<GS::UniString> nonLocalized;
		if (j.kind == JScalar::Object) {
			valueJ = ReadJScalar (j.obj, "value");
			if (j.obj.Contains ("nonLocalizedValue"))
				nonLocalized = GetString (j.obj, "nonLocalizedValue");
		}
		const API_Variant display = ScalarToVariant (valueJ, def.valueType, def.measureType, what + " option");
		const GS::UniString displayText = VariantToString (display, def.measureType);
		if (display.type == API_PropertyStringValueType && Trimmed (display.uniStringValue).IsEmpty ())
			Fail (what + ": option values must not be empty.");
		for (const API_SingleEnumerationVariant& r : result)
			if (EqualsIgnoreCase (VariantToString (r.displayVariant, def.measureType), displayText))
				Fail (what + ": duplicate option '" + displayText + "'.");

		API_SingleEnumerationVariant option;
		option.keyVariant.type = API_PropertyGuidValueType;
		option.keyVariant.guidValue = NewGuid ();
		option.nonLocalizedValue = GS::NoValue;
		if (existing != nullptr) {
			for (const API_SingleEnumerationVariant& e : *existing) {
				if (EqualsIgnoreCase (VariantToString (e.displayVariant, def.measureType), displayText)) {
					option.keyVariant = e.keyVariant;	// keep the key so element values survive
					option.nonLocalizedValue = e.nonLocalizedValue;
					break;
				}
			}
		}
		option.displayVariant = display;
		if (nonLocalized.HasValue ())
			option.nonLocalizedValue = nonLocalized;
		result.Push (option);
	}
	return result;
}


API_PropertyValue UndefinedValue (const API_PropertyDefinition& def)
{
	API_PropertyValue value;
	switch (def.collectionType) {
		case API_PropertySingleCollectionType:
			value.singleVariant.variant.type = def.valueType;
			break;
		case API_PropertySingleChoiceEnumerationCollectionType:
			if (!def.possibleEnumValues.IsEmpty ())
				value.singleVariant.variant = def.possibleEnumValues[0].keyVariant;
			else
				value.singleVariant.variant.type = API_PropertyGuidValueType;
			break;
		default:
			break;	// lists: empty
	}
	value.variantStatus = API_VariantStatusUserUndefined;
	return value;
}


// Replaces {ref:<property reference>} tokens (GUID, "Group/Name" or a unique name) with Archicad's
// own expression reference string of that property (APIAny_GetPropertyExprReferenceStringID), so
// callers do not need to know the internal reference syntax. Other text is kept unchanged.
GS::UniString ExpandExpressionRefs (const PropertyIndex& index, const GS::UniString& expression, const GS::UniString& what)
{
	const GS::UniString open ("{ref:");
	const USize length = expression.GetLength ();
	GS::UniString result;
	UIndex pos = 0;
	while (pos < length) {
		const UIndex start = expression.FindFirst (open, pos);
		if (start == MaxUIndex) {
			result += GS::UniString (expression.GetSubstring (pos, length - pos));
			break;
		}
		const UIndex refBegin = start + open.GetLength ();
		const UIndex end = refBegin < length ? expression.FindFirst ('}', refBegin) : MaxUIndex;
		if (end == MaxUIndex)
			Fail (what + ": unterminated {ref:...} in expression '" + expression + "'.");
		result += GS::UniString (expression.GetSubstring (pos, start - pos));

		JScalar ref;
		ref.kind = JScalar::String;
		ref.s = Trimmed (GS::UniString (expression.GetSubstring (refBegin, end - refBegin)));
		API_PropertyDefinition target = index.ResolveDefinition (ref);
		GS::UniString reference;
		const GSErrCode err = ACAPI_Goodies (APIAny_GetPropertyExprReferenceStringID, &target, &reference);
		if (err != NoError || reference.IsEmpty ())
			Fail (what + ": property '" + index.FullName (target) + "' cannot be referenced in an expression (" + ErrorName (err) + ").", APIERR_BADEXPRESSION);
		result += reference;
		pos = end + 1;
	}
	return result;
}


// Like CheckWithHint for Create/ChangePropertyDefinition, but on failure also names the
// expressions Archicad considers invalid (APIAny_CheckPropertyExpressionStringID).
void CheckDefinitionResult (GSErrCode err, const API_PropertyDefinition& def, const GS::UniString& what)
{
	if (err == NoError)
		return;
	GS::UniString details;
	if (def.defaultValue.hasExpression) {
		for (UIndex i = 0; i < def.defaultValue.propertyExpressions.GetSize (); ++i) {
			GS::UniString expression = def.defaultValue.propertyExpressions[i];
			const GSErrCode checkErr = ACAPI_Goodies (APIAny_CheckPropertyExpressionStringID, &expression);
			if (checkErr != NoError)
				details += " Expression #" + NumberToString ((double) (i + 1)) + " '" + def.defaultValue.propertyExpressions[i] +
						   "' is invalid (" + ErrorName (checkErr) + ").";
		}
	}
	CheckWithHint (err, what + (details.IsEmpty () ? GS::UniString () : ":" + details));
}


// Applies defaultValue / defaultExpressions / defaultUndefined. On create without any of them
// the default is "Undefined" (elements show an empty value until one is set).
void ApplyDefault (const PropertyIndex& index, API_PropertyDefinition& def, const OS& spec, const GS::UniString& what, bool isCreate)
{
	const bool hasExpr = spec.Contains ("defaultExpressions") || spec.Contains ("defaultExpression");
	const bool hasValue = spec.Contains ("defaultValue");
	const bool undefined = GetBool (spec, "defaultUndefined", false);
	if ((hasExpr ? 1 : 0) + (hasValue ? 1 : 0) + (undefined ? 1 : 0) > 1)
		Fail (what + ": give only one of defaultValue, defaultExpressions, defaultUndefined.");

	if (hasExpr) {
		GS::Array<GS::UniString> expressions;
		if (spec.Contains ("defaultExpressions"))
			expressions = GetStringArray (spec, "defaultExpressions");
		else
			expressions.Push (GetString (spec, "defaultExpression"));
		for (GS::UniString& e : expressions) {
			if (Trimmed (e).IsEmpty ())
				Fail (what + ": expressions must not be empty.");
			e = ExpandExpressionRefs (index, e, what);
		}
		if (expressions.IsEmpty ())
			Fail (what + ": defaultExpressions must contain at least one expression.");
		def.defaultValue.hasExpression = true;
		def.defaultValue.propertyExpressions = expressions;
		def.defaultValue.basicValue = UndefinedValue (def);
		def.defaultValue.basicValue.variantStatus = API_VariantStatusNormal;
		return;
	}
	if (hasValue) {
		def.defaultValue.hasExpression = false;
		def.defaultValue.propertyExpressions.Clear ();
		def.defaultValue.basicValue = BuildPropertyValue (def, ReadJValue (spec, "defaultValue"), what);
		if (!ACAPI_Property_IsValidValue (def.defaultValue.basicValue, def))
			Fail (what + ": the default value is not valid for type " + PropertyTypeName (def) + ".", APIERR_BADVALUE);
		return;
	}
	if (undefined || isCreate) {
		def.defaultValue.hasExpression = false;
		def.defaultValue.propertyExpressions.Clear ();
		def.defaultValue.basicValue = UndefinedValue (def);
	}
}


// After options changed: drop default keys that are no longer options.
void FixEnumDefault (API_PropertyDefinition& def, GS::Array<GS::UniString>& warnings)
{
	API_PropertyValue& basic = def.defaultValue.basicValue;
	if (def.defaultValue.hasExpression || basic.variantStatus != API_VariantStatusNormal)
		return;
	if (def.collectionType == API_PropertySingleChoiceEnumerationCollectionType) {
		if (FindEnumOptionByKey (def, basic.singleVariant.variant) == nullptr) {
			basic = UndefinedValue (def);
			warnings.Push ("The default option was removed; the default is now Undefined.");
		}
	} else if (def.collectionType == API_PropertyMultipleChoiceEnumerationCollectionType) {
		const USize before = basic.listVariant.variants.GetSize ();
		basic.listVariant.variants.DeleteAll ([&] (const API_Variant& k) { return FindEnumOptionByKey (def, k) == nullptr; });
		if (basic.listVariant.variants.GetSize () != before)
			warnings.Push ("Removed options were also removed from the default value.");
	}
}


ClassificationIndex& Ensure (std::optional<ClassificationIndex>& cls)
{
	if (!cls.has_value ())
		cls.emplace ();
	return *cls;
}


bool IsKeyword (const JValue& jv, const char* keyword)
{
	return jv.present && !jv.isList && jv.items.GetSize () == 1 && jv.items[0].kind == JScalar::String &&
		   EqualsIgnoreCase (Trimmed (jv.items[0].s), keyword);
}


void ApplyAvailability (API_PropertyDefinition& def, const OS& spec, std::optional<ClassificationIndex>& cls,
						GS::Array<GS::UniString>& warnings, bool isCreate)
{
	auto addAll = [&] (const GS::Array<API_Guid>& guids) {
		for (const API_Guid& g : guids)
			if (!def.availability.Contains (g))
				def.availability.Push (g);
	};

	if (spec.Contains ("availability")) {
		const JValue jv = ReadJValue (spec, "availability");
		if (IsKeyword (jv, "all"))
			def.availability = Ensure (cls).AllItemGuids ();
		else if (IsKeyword (jv, "none"))
			def.availability.Clear ();
		else
			def.availability = Ensure (cls).ResolveItemList (jv);
	} else if (isCreate && !spec.Contains ("availableForElements") && !spec.Contains ("addAvailability")) {
		def.availability = Ensure (cls).AllItemGuids ();
	}

	if (spec.Contains ("addAvailability")) {
		const JValue jv = ReadJValue (spec, "addAvailability");
		addAll (IsKeyword (jv, "all") ? Ensure (cls).AllItemGuids () : Ensure (cls).ResolveItemList (jv));
	}
	if (spec.Contains ("removeAvailability")) {
		const JValue jv = ReadJValue (spec, "removeAvailability");
		const GS::Array<API_Guid> remove = IsKeyword (jv, "all") ? def.availability : Ensure (cls).ResolveItemList (jv);
		def.availability.DeleteAll ([&] (const API_Guid& g) { return remove.Contains (g); });
	}
	if (spec.Contains ("availableForElements")) {
		const GS::Array<API_Guid> elements = GetGuidArray (spec, "availableForElements");
		addAll (ClassificationItemsOfElements (elements, warnings));
	}
	if (def.availability.IsEmpty ())
		warnings.Push ("The property is not available for any classification item, so no element shows it. "
					   "Set availability ('all', item IDs or {system}) or availableForElements.");
}


const API_PropertyGroup* FindGroup (const PropertyIndex& index, const JScalar& ref)
{
	GS::UniString text;
	if (ref.kind == JScalar::String)
		text = ref.s;
	else if (ref.kind == JScalar::Object && ref.obj.Contains ("guid"))
		text = GetString (ref.obj, "guid");
	else if (ref.kind == JScalar::Object && ref.obj.Contains ("name"))
		text = GetString (ref.obj, "name");
	else
		Fail ("\"group\" must be a group name, a GUID, {\"guid\"} or {\"name\"}; got " + ref.Describe () + ".");
	if (LooksLikeGuid (text))
		return index.GroupByGuid (ToGuid (text));
	return index.FindGroupByName (text);
}


API_PropertyGroup CreateGroup (PropertyIndex& index, const GS::UniString& rawName, const GS::UniString& description)
{
	const GS::UniString name = Trimmed (rawName);
	if (name.IsEmpty ())
		Fail ("Property group name must not be empty.", APIERR_BADNAME);
	API_PropertyGroup group;
	group.groupType = API_PropertyCustomGroupType;
	group.guid = APINULLGuid;
	group.name = name;
	group.description = description;
	CheckWithHint (ACAPI_Property_CreatePropertyGroup (group), "Cannot create property group '" + name + "'");
	index.AddGroup (group);
	return group;
}


void RefreshDefinition (API_PropertyDefinition& def)
{
	API_PropertyDefinition fresh;
	fresh.guid = def.guid;
	if (ACAPI_Property_GetPropertyDefinition (fresh) == NoError)
		def = fresh;
}


OS ResultWithWarnings (OS result, const GS::Array<GS::UniString>& warnings)
{
	if (!warnings.IsEmpty ())
		result.Add ("warnings", warnings);
	return result;
}

// --- Create / modify definitions ----------------------------------------------------------------------

OS CreateDefinition (PropertyIndex& index, const OS& spec, bool createGroups, std::optional<ClassificationIndex>& cls)
{
	GS::Array<GS::UniString> warnings;
	const GS::UniString name = Trimmed (GetString (spec, "name"));
	if (name.IsEmpty ())
		Fail ("Property name must not be empty.", APIERR_BADNAME);

	const JScalar groupRef = ReadJScalar (spec, "group");
	API_PropertyGroup group;
	if (const API_PropertyGroup* found = FindGroup (index, groupRef)) {
		group = *found;
	} else {
		const bool byName = (groupRef.kind == JScalar::String && !LooksLikeGuid (groupRef.s)) ||
							(groupRef.kind == JScalar::Object && groupRef.obj.Contains ("name") && !groupRef.obj.Contains ("guid"));
		if (!createGroups || !byName)
			index.ResolveGroup (groupRef);	// throws with the list of groups
		group = CreateGroup (index, groupRef.kind == JScalar::String ? groupRef.s : GetString (groupRef.obj, "name"), GS::UniString ());
		warnings.Push ("Created property group '" + group.name + "'.");
	}
	if (group.groupType != API_PropertyCustomGroupType)
		Fail ("'" + group.name + "' is a built-in property group; user-defined properties must be created in a custom group "
			  "(create_property_groups, or pass a new group name).", APIERR_READONLY);

	const GS::UniString fullName = group.name + "/" + name;
	for (const API_PropertyDefinition& d : index.Definitions ()) {
		if (d.groupGuid == group.guid && EqualsIgnoreCase (Trimmed (d.name), name))
			Fail ("Property '" + fullName + "' already exists (" + GuidStr (d.guid) + "). Use modify_property_definitions to change it.", APIERR_NAMEALREADYUSED);
	}

	API_PropertyDefinition def;
	def.definitionType = API_PropertyCustomDefinitionType;
	def.guid = APINULLGuid;
	def.groupGuid = group.guid;
	def.name = name;
	def.description = GetString (spec, "description", GS::UniString ());
	const TypeSpec type = ParseTypeSpec (spec);
	def.collectionType = type.collection;
	def.valueType = type.valueType;
	def.measureType = type.measure;
	def.canValueBeEditable = true;

	if (IsEnum (def)) {
		if (!spec.Contains ("enumValues"))
			Fail ("Property '" + fullName + "' of type " + PropertyTypeName (def) + " needs \"enumValues\" (the option list).");
		def.possibleEnumValues = BuildEnumOptions (ReadJValue (spec, "enumValues"), def, nullptr, "Property '" + fullName + "'");
		if (def.possibleEnumValues.IsEmpty ())
			Fail ("Property '" + fullName + "': enumValues must contain at least one option.");
	} else if (spec.Contains ("enumValues")) {
		Fail ("enumValues is only valid for type singleEnum / multiEnum (property '" + fullName + "').");
	}

	ApplyDefault (index, def, spec, "Property '" + fullName + "'", true);
	ApplyAvailability (def, spec, cls, warnings, true);

	CheckDefinitionResult (ACAPI_Property_CreatePropertyDefinition (def), def, "Cannot create property '" + fullName + "'");
	RefreshDefinition (def);
	index.AddDefinition (def);

	OS result = index.DefinitionRef (def);
	result.Add ("availabilityCount", (Int32) def.availability.GetSize ());
	return ResultWithWarnings (result, warnings);
}


OS ModifyDefinition (PropertyIndex& index, const OS& spec, std::optional<ClassificationIndex>& cls)
{
	GS::Array<GS::UniString> warnings;
	const API_PropertyDefinition& found = index.ResolveDefinition (spec, "property");
	const GS::UniString oldName = index.FullName (found);
	if (found.definitionType != API_PropertyCustomDefinitionType)
		Fail ("'" + oldName + "' is a built-in property; only user-defined (Custom) properties can be modified.", APIERR_READONLY);
	API_PropertyDefinition def = found;
	const GS::UniString what = "Property '" + oldName + "'";

	if (auto n = OptString (spec, "name")) {
		const GS::UniString name = Trimmed (*n);
		if (name.IsEmpty ())
			Fail (what + ": the new name must not be empty.", APIERR_BADNAME);
		def.name = name;
	}
	if (auto d = OptString (spec, "description"))
		def.description = *d;
	if (spec.Contains ("group")) {
		const API_PropertyGroup& group = index.ResolveGroup (ReadJScalar (spec, "group"));
		if (group.groupType != API_PropertyCustomGroupType)
			Fail (what + ": cannot move into the built-in group '" + group.name + "'.", APIERR_READONLY);
		def.groupGuid = group.guid;
	}
	if (spec.Contains ("type"))
		Fail (what + ": the value type of an existing property cannot be changed; delete it and create a new one.");

	const bool optionChange = spec.Contains ("enumValues") || spec.Contains ("addEnumValues") ||
							  spec.Contains ("removeEnumValues") || spec.Contains ("renameEnumValues");
	if (optionChange) {
		if (!IsEnum (def))
			Fail (what + " is not an option set (type " + PropertyTypeName (def) + "); enum options cannot be changed.");
		API_PropertyDefinition work = def;
		if (spec.Contains ("enumValues"))
			work.possibleEnumValues = BuildEnumOptions (ReadJValue (spec, "enumValues"), def, &def.possibleEnumValues, what);
		for (const OS& rename : GetObjectArray (spec, "renameEnumValues", false)) {
			const API_SingleEnumerationVariant& opt = ResolveEnumOption (work, ReadJScalar (rename, "from"), oldName);
			const API_Variant key = opt.keyVariant;
			const API_Variant display = ScalarToVariant (ReadJScalar (rename, "to"), def.valueType, def.measureType, what + " option");
			for (API_SingleEnumerationVariant& o : work.possibleEnumValues)
				if (VariantEquals (o.keyVariant, key))
					o.displayVariant = display;
		}
		if (spec.Contains ("addEnumValues")) {
			const GS::Array<API_SingleEnumerationVariant> added = BuildEnumOptions (ReadJValue (spec, "addEnumValues"), def, nullptr, what);
			for (const API_SingleEnumerationVariant& a : added) {
				const GS::UniString text = VariantToString (a.displayVariant, def.measureType);
				bool exists = false;
				for (const API_SingleEnumerationVariant& o : work.possibleEnumValues)
					if (EqualsIgnoreCase (VariantToString (o.displayVariant, def.measureType), text))
						exists = true;
				if (exists)
					warnings.Push ("Option '" + text + "' already exists.");
				else
					work.possibleEnumValues.Push (a);
			}
		}
		if (spec.Contains ("removeEnumValues")) {
			for (const JScalar& j : ReadJValue (spec, "removeEnumValues").items) {
				const API_Variant key = ResolveEnumOption (work, j, oldName).keyVariant;
				work.possibleEnumValues.DeleteAll ([&] (const API_SingleEnumerationVariant& o) { return VariantEquals (o.keyVariant, key); });
			}
		}
		if (work.possibleEnumValues.IsEmpty ())
			Fail (what + ": an option set needs at least one option.");
		def.possibleEnumValues = work.possibleEnumValues;
		FixEnumDefault (def, warnings);
	}

	ApplyDefault (index, def, spec, what, false);
	if (spec.Contains ("availability") || spec.Contains ("addAvailability") || spec.Contains ("removeAvailability") ||
		spec.Contains ("availableForElements"))
		ApplyAvailability (def, spec, cls, warnings, false);

	CheckDefinitionResult (ACAPI_Property_ChangePropertyDefinition (def), def, "Cannot change property '" + oldName + "'");
	RefreshDefinition (def);
	index.UpdateDefinition (def);

	OS result = index.DefinitionRef (def);
	result.Add ("availabilityCount", (Int32) def.availability.GetSize ());
	return ResultWithWarnings (result, warnings);
}


const NamedValue kImportPolicies[] = {
	{ "Append",		API_AppendConflictingProperties },
	{ "Replace",	API_ReplaceConflictingProperties },
	{ "Skip",		API_SkipConflictingProperties },
};

} // namespace


void RegisterPropertyCommands ()
{
	RegisterCommand ("GetPropertyDefinitions",
		"Lists property definitions (built-in and user-defined) with guid, localized name, group, type, kind, editable, enum options. "
		"Filters: properties (refs), groups, search (substring of name/group), kind (All|Custom|BuiltIn), elements / elementTypes "
		"(only definitions available for them; elementMatch all|any), detail summary|full (full adds default value/expressions, "
		"option keys, expressionReference, availability). Pagination: offset, limit (default 500). includeGroups adds the group list.",
		[] (const OS& params) -> OS {
			PropertyIndex index;
			const GS::UniString detail = GetString (params, "detail", "summary");
			if (!EqualsIgnoreCase (detail, "summary") && !EqualsIgnoreCase (detail, "full"))
				Fail ("detail must be 'summary' or 'full'.");
			const bool full = EqualsIgnoreCase (detail, "full");
			const GS::UniString kind = GetString (params, "kind", "All");
			if (!MatchesAny (kind, { "All", "Custom", "BuiltIn", "UserDefined" }))
				Fail ("kind must be All, Custom or BuiltIn.");
			const bool includeAvailability = GetBool (params, "includeAvailability", full);
			const Int32 offset = std::max (0, GetInt (params, "offset", 0));
			const Int32 limit = std::max (1, GetInt (params, "limit", 500));

			GS::Array<API_PropertyDefinition> candidates = params.Contains ("properties")
				? index.ResolveDefinitions (params, "properties") : index.Definitions ();

			if (params.Contains ("groups")) {
				GS::Array<API_Guid> groupGuids;
				for (const JScalar& j : ReadJValue (params, "groups").items)
					groupGuids.Push (index.ResolveGroup (j).guid);
				candidates.DeleteAll ([&] (const API_PropertyDefinition& d) { return !groupGuids.Contains (d.groupGuid); });
			}
			if (auto search = OptString (params, "search")) {
				const GS::UniString needle = Trimmed (*search).ToLowerCase ();
				candidates.DeleteAll ([&] (const API_PropertyDefinition& d) {
					return !index.FullName (d).ToLowerCase ().Contains (needle) && !d.description.ToLowerCase ().Contains (needle);
				});
			}
			if (!EqualsIgnoreCase (kind, "All")) {
				const bool wantCustom = !EqualsIgnoreCase (kind, "BuiltIn");
				candidates.DeleteAll ([&] (const API_PropertyDefinition& d) {
					return (d.definitionType == API_PropertyCustomDefinitionType) != wantCustom;
				});
			}

			const GS::Array<API_Guid> elements = GetGuidArray (params, "elements", false);
			const GS::Array<GS::UniString> elementTypes = GetStringArray (params, "elementTypes", false);
			if (!elements.IsEmpty () || !elementTypes.IsEmpty ()) {
				const GS::UniString match = GetString (params, "elementMatch", "all");
				if (!MatchesAny (match, { "all", "any" }))
					Fail ("elementMatch must be 'all' or 'any'.");
				GS::HashTable<API_Guid, Int32> counts;
				Int32 targets = 0;
				auto count = [&] (const GS::Array<API_PropertyDefinition>& defs) {
					++targets;
					for (const API_PropertyDefinition& d : defs)
						counts.Put (d.guid, counts.Retrieve (d.guid, 0) + 1);
				};
				for (const API_Guid& e : elements) {
					GS::Array<API_PropertyDefinition> defs;
					CheckWithHint (ACAPI_Element_GetPropertyDefinitions (e, API_PropertyDefinitionFilter_All, defs),
								   "Cannot get the property definitions of element " + GuidStr (e));
					count (defs);
				}
				for (const GS::UniString& typeName : elementTypes) {
					const auto typeID = ParseElemType (typeName);
					if (!typeID.has_value ())
						Fail ("Unknown element type '" + typeName + "' in elementTypes (e.g. Wall, Slab, Column, Beam, Object, Zone, Window, Door).");
					GS::Array<API_PropertyDefinition> defs;
					CheckWithHint (ACAPI_Element_GetPropertyDefinitionsOfDefaultElem (API_ElemType (*typeID), API_PropertyDefinitionFilter_All, defs),
								   "Cannot get the property definitions of the " + typeName + " tool defaults");
					count (defs);
				}
				const bool matchAll = EqualsIgnoreCase (match, "all");
				candidates.DeleteAll ([&] (const API_PropertyDefinition& d) {
					const Int32 n = counts.Retrieve (d.guid, 0);
					return matchAll ? n < targets : n == 0;
				});
			}

			std::optional<ClassificationIndex> cls;
			if (includeAvailability)
				cls.emplace ();

			GS::Array<OS> list;
			const Int32 total = (Int32) candidates.GetSize ();
			for (Int32 i = offset; i < total && i < offset + limit; ++i)
				list.Push (DefinitionJson (index, candidates[i], full, cls.has_value () ? &*cls : nullptr));

			OS out;
			out.Add ("definitions", list);
			out.Add ("total", total);
			out.Add ("offset", offset);
			out.Add ("returned", (Int32) list.GetSize ());
			out.Add ("hasMore", offset + (Int32) list.GetSize () < total);
			if (GetBool (params, "includeGroups", false)) {
				GS::Array<OS> groups;
				for (const API_PropertyGroup& g : index.Groups ())
					groups.Push (GroupJson (index, g));
				out.Add ("groups", groups);
			}
			return out;
		});

	RegisterCommand ("CreatePropertyGroups",
		"Creates custom property groups in one undo step. Input: {groups: [{name, description?}]}. An existing custom group with "
		"the same name is returned with alreadyExisted: true. Output: {results: [{guid, name, alreadyExisted?} | {error}]}.",
		[] (const OS& params) -> OS {
			const GS::Array<OS> specs = GetObjectArray (params, "groups");
			PropertyIndex index;
			GS::Array<OS> results;
			Undoable (GetString (params, "undoName", "Create property groups"), [&] () {
				for (const OS& spec : specs) {
					results.Push (Try ([&] () -> OS {
						const GS::UniString name = Trimmed (GetString (spec, "name"));
						if (const API_PropertyGroup* existing = index.FindGroupByName (name)) {
							if (existing->groupType != API_PropertyCustomGroupType)
								Fail ("'" + name + "' is a built-in property group name; choose another name.", APIERR_NAMEALREADYUSED);
							return OS ("guid", GuidStr (existing->guid), "name", existing->name, "alreadyExisted", true);
						}
						const API_PropertyGroup group = CreateGroup (index, name, GetString (spec, "description", GS::UniString ()));
						return OS ("guid", GuidStr (group.guid), "name", group.name);
					}));
				}
			});
			return OS ("results", results);
		});

	RegisterCommand ("ModifyPropertyGroups",
		"Renames custom property groups / changes their description in one undo step. Input: {groups: [{group: name|guid, name?, description?}]}.",
		[] (const OS& params) -> OS {
			const GS::Array<OS> specs = GetObjectArray (params, "groups");
			PropertyIndex index;
			GS::Array<OS> results;
			Undoable (GetString (params, "undoName", "Modify property groups"), [&] () {
				for (const OS& spec : specs) {
					results.Push (Try ([&] () -> OS {
						API_PropertyGroup group = index.ResolveGroup (ReadJScalar (spec, "group"));
						if (group.groupType != API_PropertyCustomGroupType)
							Fail ("'" + group.name + "' is a built-in property group and cannot be changed.", APIERR_READONLY);
						const GS::UniString oldName = group.name;
						if (auto n = OptString (spec, "name")) {
							group.name = Trimmed (*n);
							if (group.name.IsEmpty ())
								Fail ("The new group name must not be empty.", APIERR_BADNAME);
						}
						if (auto d = OptString (spec, "description"))
							group.description = *d;
						CheckWithHint (ACAPI_Property_ChangePropertyGroup (group), "Cannot change property group '" + oldName + "'");
						index.UpdateGroup (group);
						return OS ("guid", GuidStr (group.guid), "name", group.name);
					}));
				}
			});
			return OS ("results", results);
		});

	RegisterCommand ("DeletePropertyGroups",
		"Deletes custom property groups in one undo step. Input: {groups: [name|guid], deleteDefinitions?: false}. A group that still "
		"contains definitions is only deleted (together with them) when deleteDefinitions is true.",
		[] (const OS& params) -> OS {
			const JValue refs = ReadJValue (params, "groups");
			if (!refs.present || refs.items.IsEmpty ())
				Fail ("Missing required array field 'groups'.");
			const bool deleteDefinitions = GetBool (params, "deleteDefinitions", false);
			PropertyIndex index;
			GS::Array<OS> results;
			Undoable (GetString (params, "undoName", "Delete property groups"), [&] () {
				for (const JScalar& ref : refs.items) {
					results.Push (Try ([&] () -> OS {
						const API_PropertyGroup group = index.ResolveGroup (ref);
						if (group.groupType != API_PropertyCustomGroupType)
							Fail ("'" + group.name + "' is a built-in property group and cannot be deleted.", APIERR_READONLY);
						GS::Array<GS::UniString> contained;
						GS::Array<API_Guid> containedGuids;
						for (const API_PropertyDefinition& d : index.Definitions ()) {
							if (d.groupGuid == group.guid) {
								contained.Push (d.name);
								containedGuids.Push (d.guid);
							}
						}
						if (!contained.IsEmpty () && !deleteDefinitions)
							Fail ("Group '" + group.name + "' contains " + NumberToString ((double) contained.GetSize ()) + " definition(s) (" +
								  JoinNames (contained, 8) + "). Pass deleteDefinitions: true to delete them too, or move them first.");
						// Delete the definitions explicitly first (does not rely on the group deletion cascading).
						for (UIndex i = 0; i < containedGuids.GetSize (); ++i)
							CheckWithHint (ACAPI_Property_DeletePropertyDefinition (containedGuids[i]),
										   "Cannot delete property '" + group.name + "/" + contained[i] + "' of the group");
						CheckWithHint (ACAPI_Property_DeletePropertyGroup (group.guid), "Cannot delete property group '" + group.name + "'");
						index.RemoveGroup (group.guid);
						return OS ("guid", GuidStr (group.guid), "name", group.name, "deletedDefinitions", (Int32) contained.GetSize ());
					}));
				}
			});
			return OS ("results", results);
		});

	RegisterCommand ("CreatePropertyDefinitions",
		"Creates user-defined properties in one undo step. Input: {definitions: [{group, name, type, description?, enumValues?, "
		"enumValueType?, defaultValue? | defaultExpressions? ({ref:Group/Name|GUID} tokens are replaced by Archicad property references) | "
		"defaultUndefined?, availability? ('all' default | 'none' | item refs), "
		"addAvailability?, availableForElements?}], createMissingGroups?: true}. Output: {results: [{guid, name, group, type, "
		"availabilityCount, warnings?} | {error}]}.",
		[] (const OS& params) -> OS {
			const GS::Array<OS> specs = GetObjectArray (params, "definitions");
			const bool createGroups = GetBool (params, "createMissingGroups", true);
			PropertyIndex index;
			std::optional<ClassificationIndex> cls;
			GS::Array<OS> results;
			Undoable (GetString (params, "undoName", "Create property definitions"), [&] () {
				for (const OS& spec : specs)
					results.Push (Try ([&] () -> OS { return CreateDefinition (index, spec, createGroups, cls); }));
			});
			return OS ("results", results);
		});

	RegisterCommand ("ModifyPropertyDefinitions",
		"Changes user-defined properties in one undo step. Input: {definitions: [{property (ref), name?, description?, group?, "
		"enumValues? (replace, keeps keys of unchanged options), addEnumValues?, removeEnumValues?, renameEnumValues? [{from, to}], "
		"defaultValue? | defaultExpressions? | defaultUndefined?, availability?, addAvailability?, removeAvailability?, availableForElements?}]}.",
		[] (const OS& params) -> OS {
			const GS::Array<OS> specs = GetObjectArray (params, "definitions");
			PropertyIndex index;
			std::optional<ClassificationIndex> cls;
			GS::Array<OS> results;
			Undoable (GetString (params, "undoName", "Modify property definitions"), [&] () {
				for (const OS& spec : specs)
					results.Push (Try ([&] () -> OS { return ModifyDefinition (index, spec, cls); }));
			});
			return OS ("results", results);
		});

	RegisterCommand ("DeletePropertyDefinitions",
		"Deletes user-defined property definitions (and their values on all elements) in one undo step. Input: {properties: [ref]}.",
		[] (const OS& params) -> OS {
			const JValue refs = ReadJValue (params, "properties");
			if (!refs.present || refs.items.IsEmpty ())
				Fail ("Missing required array field 'properties'.");
			PropertyIndex index;
			GS::Array<OS> results;
			Undoable (GetString (params, "undoName", "Delete property definitions"), [&] () {
				for (const JScalar& ref : refs.items) {
					results.Push (Try ([&] () -> OS {
						const API_PropertyDefinition def = index.ResolveDefinition (ref);
						const GS::UniString name = index.FullName (def);
						if (def.definitionType != API_PropertyCustomDefinitionType)
							Fail ("'" + name + "' is a built-in property and cannot be deleted.", APIERR_READONLY);
						CheckWithHint (ACAPI_Property_DeletePropertyDefinition (def.guid), "Cannot delete property '" + name + "'");
						OS r = index.DefinitionRef (def);
						index.RemoveDefinition (def.guid);
						r.Add ("deleted", true);
						return r;
					}));
				}
			});
			return OS ("results", results);
		});

	RegisterCommand ("ImportPropertyDefinitionsXml",
		"Imports property groups/definitions from Archicad's property XML (Property Manager export format). Input: {xml, "
		"conflictPolicy: Append|Replace|Skip (default Skip)}. Output: {created: [definition refs], groupsCreated: [...]}.",
		[] (const OS& params) -> OS {
			const GS::UniString xml = GetString (params, "xml");
			if (Trimmed (xml).IsEmpty ())
				Fail ("xml must not be empty.");
			const API_PropertyDefinitionNameConflictResolutionPolicy policy = params.Contains ("conflictPolicy")
				? (API_PropertyDefinitionNameConflictResolutionPolicy) ParseNamed (kImportPolicies, params, "conflictPolicy")
				: API_SkipConflictingProperties;

			PropertyIndex before;
			Undoable (GetString (params, "undoName", "Import property definitions"), [&] () {
				CheckWithHint (ACAPI_Property_Import (xml, policy), "Property XML import failed (check that the XML is an Archicad property export)");
			});
			PropertyIndex after;
			GS::Array<OS> created;
			for (const API_PropertyDefinition& d : after.Definitions ())
				if (before.DefinitionByGuid (d.guid) == nullptr)
					created.Push (after.DefinitionRef (d));
			GS::Array<OS> groups;
			for (const API_PropertyGroup& g : after.Groups ())
				if (before.GroupByGuid (g.guid) == nullptr)
					groups.Push (OS ("guid", GuidStr (g.guid), "name", g.name));
			return OS ("created", created, "groupsCreated", groups, "definitionCount", (Int32) after.Definitions ().GetSize ());
		});

	props::RegisterPropertyValueCommands ();
	props::RegisterIfcCommands ();
	props::RegisterClassificationAuthoringCommands ();
}

} // namespace cc
