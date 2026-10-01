// *****************************************************************************
// PropertiesCommon.hpp — helpers shared by the "properties" family:
//   Properties.cpp               property groups / definitions (+ XML import)
//   PropertiesValues.cpp         property values of elements, tool defaults, attributes
//   PropertiesIfc.cpp            IFC identifiers / type / properties / attributes
//   PropertiesClassification.cpp classification system & item authoring
// Everything lives in cc::props to avoid clashes with other families' helpers.
//
// JSON conventions (docs/DEVELOPING.md): meters, DEGREES (angle-measure property
// values are converted from/to radians), camelCase, enums as strings.
//
// Property references (everywhere a property is addressed):
//   "GUID"                         definition GUID
//   "Group/Name"                   localized group + definition name (a "/" inside
//                                  names is handled: every split position is tried)
//   "Name"                         definition name alone, when unique in the project
//   {"guid": "..."} | {"group": "...", "name": "..."}
// Matching is exact first, then case-insensitive; ambiguous or unknown names throw
// an error listing candidates.
// *****************************************************************************

#pragma once

#include "Core/Json.hpp"
#include "Core/Types.hpp"

namespace cc {
namespace props {

// --- Sub-family registration (called from RegisterPropertyCommands) -----------

void RegisterPropertyValueCommands ();			// PropertiesValues.cpp
void RegisterIfcCommands ();					// PropertiesIfc.cpp
void RegisterClassificationAuthoringCommands ();	// PropertiesClassification.cpp

// --- Heterogeneous JSON values ------------------------------------------------------

struct JScalar {
	enum Kind { Bool, Int, Real, String, Object };

	Kind			kind = String;
	bool			b = false;
	Int64			i = 0;
	double			d = 0.0;
	GS::UniString	s;
	OS				obj;

	bool			IsNumber () const	{ return kind == Int || kind == Real; }
	double			Number () const		{ return kind == Int ? (double) i : d; }
	GS::UniString	Describe () const;	// short text for error messages
};

struct JValue {
	bool				present = false;
	bool				isList = false;
	bool				nested = false;	// list inside a list (not supported by property values)
	GS::Array<JScalar>	items;			// exactly one item when !isList
};

// Reads os[key] whatever its JSON type is (scalar, object or list of those).
JValue			ReadJValue (const OS& os, const char* key);
// Single scalar os[key] (throws if it is missing or a list).
JScalar			ReadJScalar (const OS& os, const char* key);

GS::UniString	Trimmed (const GS::UniString& s);
bool			LooksLikeGuid (const GS::UniString& s);
API_Guid		ToGuid (const GS::UniString& s);		// accepts {braces}; throws on invalid GUIDs
bool			ParseNumberString (const GS::UniString& s, double& out);
GS::UniString	NumberToString (double v);
GS::UniString	JoinNames (const GS::Array<GS::UniString>& names, UIndex maxCount = 12);

// --- Property type names -----------------------------------------------------------------
//   "string" | "integer" | "number" | "length" | "area" | "volume" | "angle" | "boolean" | "guid"
//   + "List" suffix for list collections, "singleEnum" / "multiEnum" for option sets.

GS::UniString	ScalarTypeName (API_VariantType valueType, API_PropertyMeasureType measure);
GS::UniString	PropertyTypeName (const API_PropertyDefinition& def);
GS::UniString	DefinitionKindName (API_PropertyDefinitionType type);	// "Custom" | "BuiltIn" | "DynamicBuiltIn"
GS::UniString	GroupKindName (API_PropertyGroupType type);
GS::UniString	CollectionTypeName (API_PropertyCollectionType type);
GS::UniString	ValueTypeName (API_VariantType type);
GS::UniString	MeasureTypeName (API_PropertyMeasureType type);

// Parses a scalar type name ("string", "length", "boolean", ...). Returns false when unknown.
bool			ParseScalarType (const GS::UniString& name, API_VariantType& valueType, API_PropertyMeasureType& measure);

// --- Variant <-> JSON ---------------------------------------------------------------------

bool			VariantEquals (const API_Variant& a, const API_Variant& b);
GS::UniString	VariantToString (const API_Variant& v, API_PropertyMeasureType measure);
// out[key] = variant (angle measure converted to degrees). Undefined variants are omitted.
void			AddVariant (OS& out, const char* key, const API_Variant& v, API_PropertyMeasureType measure);
// out[key] = list of variants, all of the given value type.
void			AddVariantList (OS& out, const char* key, const GS::Array<API_Variant>& variants,
								API_VariantType valueType, API_PropertyMeasureType measure);

// Converts one JSON scalar to a variant of the given type (lenient: numeric strings,
// "true"/"false", numbers for strings...). Angles are read in degrees. `what` names the
// value in error messages.
API_Variant		ScalarToVariant (const JScalar& j, API_VariantType valueType, API_PropertyMeasureType measure,
								 const GS::UniString& what);

// Option lookup of enumeration definitions (display value, non-localized value or key GUID).
const API_SingleEnumerationVariant*	FindEnumOptionByKey (const API_PropertyDefinition& def, const API_Variant& key);
const API_SingleEnumerationVariant&	ResolveEnumOption (const API_PropertyDefinition& def, const JScalar& j,
													   const GS::UniString& propertyName);
GS::Array<GS::UniString>			EnumDisplayValues (const API_PropertyDefinition& def);

// Builds a property value for `def` from JSON (single value, list, option display value(s)).
API_PropertyValue	BuildPropertyValue (const API_PropertyDefinition& def, const JValue& jv, const GS::UniString& propertyName);
// out[key] = typed value of `value` (enum keys are converted to their display values).
void				AddPropertyValue (OS& out, const char* key, const API_PropertyDefinition& def, const API_PropertyValue& value);
// Cell of a property value table: {value, display?, isDefault?} | {status: "NotAvailable"|"NotEvaluated"|"Undefined"|"Empty"}.
OS					PropertyCellJson (const API_Property& property, bool withDisplay);

// --- Property index (all groups + definitions of the project) -----------------------------

class PropertyIndex {
public:
	PropertyIndex ();

	void	Reload ();
	void	AddGroup (const API_PropertyGroup& group);
	void	UpdateGroup (const API_PropertyGroup& group);
	void	RemoveGroup (const API_Guid& guid);
	void	AddDefinition (const API_PropertyDefinition& def);
	void	UpdateDefinition (const API_PropertyDefinition& def);
	void	RemoveDefinition (const API_Guid& guid);

	const GS::Array<API_PropertyGroup>&			Groups () const			{ return groups; }
	const GS::Array<API_PropertyDefinition>&	Definitions () const	{ return definitions; }

	const API_PropertyGroup*		GroupByGuid (const API_Guid& guid) const;
	const API_PropertyDefinition*	DefinitionByGuid (const API_Guid& guid) const;
	const API_PropertyGroup*		FindGroupByName (const GS::UniString& name) const;	// exact, then case-insensitive
	GS::UniString					GroupName (const API_Guid& groupGuid) const;
	GS::UniString					FullName (const API_PropertyDefinition& def) const;	// "Group/Name"
	USize							DefinitionCount (const API_Guid& groupGuid) const;

	// Group reference: name or GUID string, {guid} or {name}.
	const API_PropertyGroup&		ResolveGroup (const JScalar& ref) const;
	// Property reference (see file header).
	const API_PropertyDefinition&	ResolveDefinition (const JScalar& ref) const;
	const API_PropertyDefinition&	ResolveDefinition (const OS& os, const char* key) const;
	// Reads an array of property references (duplicates removed, order kept).
	GS::Array<API_PropertyDefinition> ResolveDefinitions (const OS& os, const char* key) const;

	// {guid, name, group, type}
	OS								DefinitionRef (const API_PropertyDefinition& def) const;

private:
	const API_PropertyDefinition&	ResolveName (const GS::UniString& text) const;
	void							Rehash ();

	GS::Array<API_PropertyGroup>		groups;
	GS::Array<API_PropertyDefinition>	definitions;
	GS::HashTable<API_Guid, UIndex>		groupIndex;
	GS::HashTable<API_Guid, UIndex>		definitionIndex;
};

// --- Classification index (all systems + items, for availability and authoring) ------------

class ClassificationIndex {
public:
	struct Item {
		API_ClassificationItem	item;
		API_Guid				system = APINULLGuid;
		API_Guid				parent = APINULLGuid;
		Int32					depth = 0;
	};

	ClassificationIndex ();
	void	Reload ();
	// Registers an item created after loading (so later references by ID resolve).
	void	AddItem (const API_ClassificationItem& item, const API_Guid& system, const API_Guid& parent);
	void	UpdateItem (const API_ClassificationItem& item);

	const GS::Array<API_ClassificationSystem>&	Systems () const	{ return systems; }
	const GS::Array<Item>&						Items () const		{ return items; }

	const API_ClassificationSystem*	SystemByGuid (const API_Guid& guid) const;
	const Item*						ItemByGuid (const API_Guid& guid) const;
	GS::UniString					SystemLabel (const API_Guid& systemGuid) const;	// "Name (version)"

	// System reference: GUID, name, "Name version" or {guid}|{name, editionVersion?}.
	const API_ClassificationSystem&	ResolveSystem (const JScalar& ref) const;
	// Item reference: GUID, item ID (unique across systems or within `system`), {guid},
	// {system, id}, {id}. `system` may be null.
	const Item&						ResolveItem (const JScalar& ref, const API_ClassificationSystem* system = nullptr) const;
	// Availability-style list: item refs, plus {"system": ref} (no id) = every item of that system.
	GS::Array<API_Guid>				ResolveItemList (const JValue& refs) const;
	GS::Array<API_Guid>				AllItemGuids () const;
	GS::Array<API_Guid>				SystemItemGuids (const API_Guid& systemGuid) const;
	GS::Array<API_Guid>				Descendants (const API_Guid& itemGuid) const;	// excluding the item

	// {guid, id, name?, system}
	OS								ItemRef (const API_Guid& itemGuid) const;
	// {guid, name, editionVersion, description?, source?, editionDate}
	static OS						SystemJson (const API_ClassificationSystem& system);

private:
	void	LoadChildren (const API_Guid& systemGuid, const API_Guid& parentGuid, const GS::Array<API_ClassificationItem>& children, Int32 depth);

	GS::Array<API_ClassificationSystem>	systems;
	GS::Array<Item>						items;
	GS::HashTable<API_Guid, UIndex>		itemIndex;
};

// Classification item GUIDs of elements (union), warnings for unclassified elements.
GS::Array<API_Guid>	ClassificationItemsOfElements (const GS::Array<API_Guid>& elements, GS::Array<GS::UniString>& warnings);

// "YYYY-MM-DD" <-> GSDateRecord
GSDateRecord		ParseDate (const GS::UniString& text);
GS::UniString		DateToString (const GSDateRecord& date);
GSDateRecord		Today ();

API_Guid			NewGuid ();

// Like cc::Check, but appends an actionable hint for the usual property / classification errors
// (name already used, Teamwork reservation, read-only built-ins, bad expression, ...).
void				CheckWithHint (GSErrCode err, const GS::UniString& what);

} // namespace props
} // namespace cc
