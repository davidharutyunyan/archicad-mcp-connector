// *****************************************************************************
// PropertiesValues — property values of elements, element tool defaults and
// attributes (building materials, composites, surfaces, ...).
//
//   GetPropertyValues            {elements?, elementDefaults?, properties?, scope?, includeDisplay?}
//   SetPropertyValues            {values: [{elements?, elementDefaults?, property, value | reset | setUndefined}]}
//   GetAttributePropertyValues   {attributes: [{type, attribute}], properties?, scope?, includeDisplay?}
//   SetAttributePropertyValues   {values: [{attributes: [{type, attribute}], property, value | reset | setUndefined}]}
//
// Values: typed JSON (string / number / boolean / list); lengths m, areas m², volumes m³,
// angles DEGREES; option sets by display value (or non-localized value / key GUID).
// Output table: {properties: [{guid, name, group, type}], results: [{guid|elementType|attribute,
// values: [cell per property]}]} with cell = {value, display?, isDefault?} |
// {status: NotAvailable|NotEvaluated|Undefined|Empty}.
// *****************************************************************************

#include "Commands/PropertiesCommon.hpp"
#include "Core/Command.hpp"
#include "Core/Elements.hpp"

namespace cc {
namespace props {

namespace {

// --- Targets ------------------------------------------------------------------------------------

struct Target {
	enum Kind { Element, DefaultElem, Attribute };

	Kind			kind = Element;
	API_Guid		guid = APINULLGuid;
	API_ElemType	elemType = API_ElemType (API_ZombieElemID);
	API_Attr_Head	attrHead = {};
	OS				ref;
	GS::UniString	label;
};


GS::Array<Target> ReadElementTargets (const OS& os)
{
	GS::Array<Target> targets;
	for (const API_Guid& g : GetGuidArray (os, "elements", false)) {
		Target t;
		t.kind = Target::Element;
		t.guid = g;
		t.ref = OS ("guid", GuidStr (g));
		t.label = "element " + GuidStr (g);
		targets.Push (t);
	}
	for (const GS::UniString& typeName : GetStringArray (os, "elementDefaults", false)) {
		const auto typeID = ParseElemType (typeName);
		if (!typeID.has_value ())
			Fail ("Unknown element type '" + typeName + "' in elementDefaults (e.g. Wall, Slab, Column, Beam, Roof, Object, Zone, Window, Door).");
		Target t;
		t.kind = Target::DefaultElem;
		t.elemType = API_ElemType (*typeID);
		t.ref = OS ("elementType", ElemTypeName (*typeID));
		t.label = ElemTypeName (*typeID) + " tool defaults";
		targets.Push (t);
	}
	return targets;
}


API_Attr_Head LoadAttrHead (API_AttrTypeID typeID, API_AttributeIndex index, GS::UniString& name)
{
	API_Attribute attr;
	BNZeroMemory (&attr, sizeof (attr));
	attr.header.typeID = typeID;
	attr.header.index = index;
	attr.header.uniStringNamePtr = &name;
	CheckWithHint (ACAPI_Attribute_Get (&attr), "Cannot read " + AttrTypeName (typeID) + " attribute #" + NumberToString ((double) index));
	if (typeID == API_MaterialID && attr.material.texture.fileLoc != nullptr) {
		delete attr.material.texture.fileLoc;
		attr.material.texture.fileLoc = nullptr;
	}
	API_Attr_Head head = attr.header;
	head.uniStringNamePtr = nullptr;
	return head;
}


GS::Array<Target> ReadAttributeTargets (const OS& os)
{
	GS::Array<Target> targets;
	for (const OS& item : GetObjectArray (os, "attributes")) {
		const API_AttrTypeID typeID = item.Contains ("type") ? GetAttrType (item, "type") : API_BuildingMaterialID;
		const API_AttributeIndex index = GetAttr (typeID, item, "attribute");
		GS::UniString name;
		Target t;
		t.kind = Target::Attribute;
		t.attrHead = LoadAttrHead (typeID, index, name);
		t.guid = t.attrHead.guid;
		OS attrRef ("type", AttrTypeName (typeID), "index", (Int32) index, "name", name, "guid", GuidStr (t.attrHead.guid));
		t.ref = OS ("attribute", attrRef);
		t.label = AttrTypeName (typeID) + " '" + name + "'";
		targets.Push (t);
	}
	return targets;
}


GSErrCode GetDefinitions (const Target& t, API_PropertyDefinitionFilter filter, GS::Array<API_PropertyDefinition>& defs)
{
	switch (t.kind) {
		case Target::Element:		return ACAPI_Element_GetPropertyDefinitions (t.guid, filter, defs);
		case Target::DefaultElem:	return ACAPI_Element_GetPropertyDefinitionsOfDefaultElem (t.elemType, filter, defs);
		case Target::Attribute:		return ACAPI_Attribute_GetPropertyDefinitions (t.attrHead, filter, defs);
	}
	return APIERR_BADPARS;
}


GSErrCode GetValues (const Target& t, const GS::Array<API_Guid>& guids, GS::Array<API_Property>& props)
{
	switch (t.kind) {
		case Target::Element:		return ACAPI_Element_GetPropertyValuesByGuid (t.guid, guids, props);
		case Target::DefaultElem:	return ACAPI_Element_GetPropertyValuesOfDefaultElemByGuid (t.elemType, guids, props);
		case Target::Attribute:		return ACAPI_Attribute_GetPropertyValuesByGuid (t.attrHead, guids, props);
	}
	return APIERR_BADPARS;
}


GSErrCode GetValue (const Target& t, const API_Guid& guid, API_Property& prop)
{
	switch (t.kind) {
		case Target::Element:		return ACAPI_Element_GetPropertyValue (t.guid, guid, prop);
		case Target::DefaultElem:	return ACAPI_Element_GetPropertyValueOfDefaultElem (t.elemType, guid, prop);
		case Target::Attribute:		return ACAPI_Attribute_GetPropertyValue (t.attrHead, guid, prop);
	}
	return APIERR_BADPARS;
}


bool IsEditable (const Target& t, const API_Guid& guid)
{
	switch (t.kind) {
		case Target::Element:		return ACAPI_Element_IsPropertyDefinitionValueEditable (t.guid, guid);
		case Target::DefaultElem:	return ACAPI_Element_IsPropertyDefinitionValueEditableDefault (t.elemType, guid);
		case Target::Attribute:		return ACAPI_Attribute_IsPropertyDefinitionValueEditable (t.attrHead, guid);
	}
	return false;
}


GSErrCode SetValue (const Target& t, const API_Property& prop)
{
	switch (t.kind) {
		case Target::Element:		return ACAPI_Element_SetProperty (t.guid, prop);
		case Target::DefaultElem:	return ACAPI_Element_SetPropertyOfDefaultElem (t.elemType, prop);
		case Target::Attribute:		return ACAPI_Attribute_SetProperty (t.attrHead, prop);
	}
	return APIERR_BADPARS;
}


void CheckTargetExists (const Target& t)
{
	if (t.kind == Target::Element && !ElementExists (t.guid))
		Fail ("Element " + GuidStr (t.guid) + " does not exist (deleted, or not an element GUID).", APIERR_BADID);
}


OS TryWithRef (const OS& ref, const std::function<OS ()>& fn)
{
	OS result = Try (fn);
	if (result.Contains ("error")) {
		OS withRef = ref;
		OS error;
		result.Get ("error", error);
		withRef.Add ("error", error);
		return withRef;
	}
	return result;
}

// --- Get --------------------------------------------------------------------------------------------

OS GetValuesTable (const PropertyIndex& index, const GS::Array<Target>& targets, const OS& params)
{
	const bool withDisplay = GetBool (params, "includeDisplay", true);
	GS::Array<API_PropertyDefinition> columns;

	if (params.Contains ("properties")) {
		columns = index.ResolveDefinitions (params, "properties");
		if (columns.IsEmpty ())
			Fail ("properties is empty: pass property references or omit it to get every available property.");
	} else {
		const GS::UniString scope = GetString (params, "scope", "UserDefined");
		const bool userDefined = EqualsIgnoreCase (scope, "UserDefined") || EqualsIgnoreCase (scope, "Custom");
		const bool builtIn = EqualsIgnoreCase (scope, "BuiltIn");
		if (!userDefined && !builtIn && !EqualsIgnoreCase (scope, "All"))
			Fail ("scope must be UserDefined, BuiltIn or All.");
		const API_PropertyDefinitionFilter filter = userDefined ? API_PropertyDefinitionFilter_UserDefined : API_PropertyDefinitionFilter_All;
		GS::HashSet<API_Guid> seen;
		for (const Target& t : targets) {
			GS::Array<API_PropertyDefinition> defs;
			if (GetDefinitions (t, filter, defs) != NoError)
				continue;	// reported per target below
			for (const API_PropertyDefinition& d : defs) {
				if (builtIn && d.definitionType == API_PropertyCustomDefinitionType)
					continue;
				if (seen.Add (d.guid))
					columns.Push (d);
			}
		}
	}

	GS::Array<API_Guid> guids;
	GS::Array<OS> header;
	for (const API_PropertyDefinition& c : columns) {
		guids.Push (c.guid);
		header.Push (index.DefinitionRef (c));
	}

	GS::Array<OS> results;
	for (const Target& t : targets) {
		results.Push (TryWithRef (t.ref, [&] () -> OS {
			CheckTargetExists (t);
			GS::Array<API_Property> values;
			GS::HashTable<API_Guid, UIndex> byGuid;
			if (!guids.IsEmpty ()) {
				const GSErrCode err = GetValues (t, guids, values);
				if (err == NoError) {
					for (UIndex i = 0; i < values.GetSize (); ++i)
						byGuid.Put (values[i].definition.guid, i);
				} else {
					// One unavailable definition can fail the batch call: read them one by one.
					values.Clear ();
					for (const API_Guid& g : guids) {
						API_Property p;
						if (GetValue (t, g, p) == NoError) {
							byGuid.Put (g, values.GetSize ());
							values.Push (p);
						}
					}
				}
			}
			GS::Array<OS> cells;
			for (const API_Guid& g : guids) {
				const UIndex* i = byGuid.GetPtr (g);
				cells.Push (i != nullptr ? PropertyCellJson (values[*i], withDisplay) : OS ("status", GS::UniString ("NotAvailable")));
			}
			OS r = t.ref;
			r.Add ("values", cells);
			return r;
		}));
	}
	return OS ("properties", header, "results", results);
}

// --- Set ----------------------------------------------------------------------------------------------

OS SetEntry (const PropertyIndex& index, const OS& entry, bool attributes)
{
	OS identity;
	if (entry.Contains ("property")) {
		JScalar ref = ReadJScalar (entry, "property");
		if (ref.kind == JScalar::String)
			identity.Add ("property", ref.s);
	}
	return TryWithRef (identity, [&] () -> OS {
		const API_PropertyDefinition& def = index.ResolveDefinition (entry, "property");
		const GS::UniString name = index.FullName (def);
		const GS::Array<Target> targets = attributes ? ReadAttributeTargets (entry) : ReadElementTargets (entry);
		if (targets.IsEmpty ())
			Fail (attributes ? GS::UniString ("Give the attributes to change.")
							 : GS::UniString ("Give elements (GUIDs) and/or elementDefaults (element type names) to change."));

		const bool reset = GetBool (entry, "reset", false);
		const bool setUndefined = GetBool (entry, "setUndefined", false);
		const bool hasValue = entry.Contains ("value");
		if ((reset ? 1 : 0) + (setUndefined ? 1 : 0) + (hasValue ? 1 : 0) != 1)
			Fail ("Property '" + name + "': give exactly one of value, reset: true (back to the default / expression) or setUndefined: true.");
		if (reset && def.definitionType != API_PropertyCustomDefinitionType)
			Fail ("Property '" + name + "' is built-in: reset only applies to user-defined properties.");

		API_PropertyValue newValue;
		if (hasValue) {
			newValue = BuildPropertyValue (def, ReadJValue (entry, "value"), name);
			if (!ACAPI_Property_IsValidValue (newValue, def))
				Fail ("Property '" + name + "': the value is not valid for type " + PropertyTypeName (def) + ".", APIERR_BADVALUE);
		}

		Int32 succeeded = 0;
		GS::Array<OS> failed;
		for (const Target& t : targets) {
			const OS r = TryWithRef (t.ref, [&] () -> OS {
				CheckTargetExists (t);
				API_Property prop;
				CheckWithHint (GetValue (t, def.guid, prop), "Cannot read '" + name + "' of " + t.label);
				if (prop.status == API_Property_NotAvailable) {
					GS::UniString message = "'" + name + "' is not available for " + t.label + ".";
					if (def.definitionType == API_PropertyCustomDefinitionType)
						message += " Its classification is not in the property's availability: call modify_property_definitions "
								   "{property, availableForElements: [...]} (or addAvailability), or classify the element first.";
					Fail (message, APIERR_BADPROPERTY);
				}
				if (!IsEditable (t, def.guid))
					Fail ("'" + name + "' is read-only for " + t.label + " (calculated built-in value or expression-based property).", APIERR_READONLY);
				if (reset) {
					prop.isDefault = true;
				} else if (setUndefined) {
					prop.value.variantStatus = API_VariantStatusUserUndefined;
					prop.isDefault = false;
				} else {
					prop.value = newValue;
					prop.isDefault = false;
				}
				CheckWithHint (SetValue (t, prop), "Cannot set '" + name + "' on " + t.label);
				return OS ("ok", true);
			});
			if (r.Contains ("error"))
				failed.Push (r);
			else
				++succeeded;
		}
		OS out ("property", name, "guid", GuidStr (def.guid), "succeeded", succeeded);
		if (!failed.IsEmpty ())
			out.Add ("failed", failed);
		return out;
	});
}


OS SetValuesCommand (const OS& params, bool attributes)
{
	const GS::Array<OS> entries = GetObjectArray (params, "values");
	PropertyIndex index;
	GS::Array<OS> results;
	Int32 succeeded = 0, failed = 0;
	Undoable (GetString (params, "undoName", attributes ? "Set attribute property values" : "Set property values"), [&] () {
		for (const OS& entry : entries) {
			const OS r = SetEntry (index, entry, attributes);
			if (r.Contains ("error")) {
				++failed;
			} else {
				Int32 n = 0;
				r.Get ("succeeded", n);
				succeeded += n;
				if (r.Contains ("failed")) {
					GS::Array<OS> f;
					r.Get ("failed", f);
					failed += (Int32) f.GetSize ();
				}
			}
			results.Push (r);
		}
	});
	return OS ("results", results, "succeeded", succeeded, "failed", failed);
}

} // namespace


void RegisterPropertyValueCommands ()
{
	RegisterCommand ("GetPropertyValues",
		"Reads property values of elements and/or element tool defaults as a table. Input: {elements?: [guid], elementDefaults?: "
		"[type], properties?: [ref] (default: every property available for the targets, see scope), scope?: UserDefined|BuiltIn|All "
		"(default UserDefined), includeDisplay?: true}. Output: {properties: [{guid, name, group, type}], results: [{guid | "
		"elementType, values: [{value, display?, isDefault?} | {status}]}]}.",
		[] (const OS& params) -> OS {
			PropertyIndex index;
			const GS::Array<Target> targets = ReadElementTargets (params);
			if (targets.IsEmpty ())
				Fail ("Give elements (GUIDs) and/or elementDefaults (element type names).");
			return GetValuesTable (index, targets, params);
		});

	RegisterCommand ("SetPropertyValues",
		"Sets property values on elements and/or element tool defaults in one undo step. Input: {values: [{elements?: [guid], "
		"elementDefaults?: [type], property: ref, value | reset: true | setUndefined: true}]}. Output: {results: [{property, guid, "
		"succeeded, failed?: [{guid|elementType, error}]} | {error}], succeeded, failed}.",
		[] (const OS& params) -> OS {
			return SetValuesCommand (params, false);
		});

	RegisterCommand ("GetAttributePropertyValues",
		"Reads property values of attributes (building materials, composites, surfaces, ...). Input: {attributes: [{type "
		"(default BuildingMaterial), attribute: name|index|{guid}}], properties?, scope?, includeDisplay?}. Output like GetPropertyValues "
		"with results: [{attribute: {type, index, name, guid}, values}].",
		[] (const OS& params) -> OS {
			PropertyIndex index;
			const GS::Array<Target> targets = ReadAttributeTargets (params);
			if (targets.IsEmpty ())
				Fail ("Give at least one attribute.");
			return GetValuesTable (index, targets, params);
		});

	RegisterCommand ("SetAttributePropertyValues",
		"Sets property values of attributes in one undo step. Input: {values: [{attributes: [{type, attribute}], property, value | "
		"reset: true | setUndefined: true}]}.",
		[] (const OS& params) -> OS {
			return SetValuesCommand (params, true);
		});
}

} // namespace props
} // namespace cc
