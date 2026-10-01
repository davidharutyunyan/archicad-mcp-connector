// *****************************************************************************
// AttributesCommon.hpp — shared (family-internal) helpers of the attribute
// commands (Attributes.cpp, AttributesProfiles.cpp).
//
// AttrData / DefsExt own everything ACAPI_Attribute_Get / GetDefExt hand out
// (surface texture locations, model view option GDL data, definition handles),
// so every attribute access in this family is leak-free by construction.
// *****************************************************************************

#pragma once

#include "Core/Json.hpp"
#include "Core/Types.hpp"

class ProfileVectorImage;

namespace cc {
namespace attr {

// --- RAII attribute holder ------------------------------------------------------

class AttrData {
public:
	AttrData ();
	explicit AttrData (API_AttrTypeID typeID);
	~AttrData ();
	AttrData (const AttrData&) = delete;
	AttrData& operator= (const AttrData&) = delete;

	// Loads attribute (type, index). Returns the API error (APIERR_DELETED for deleted slots).
	GSErrCode		Load (API_AttrTypeID typeID, API_AttributeIndex index);
	// Frees the dynamic data returned by ACAPI_Attribute_Get and zeroes the struct (type kept).
	void			Reset ();
	// Re-points the string pointers (name, building material texts) at the members before a write.
	void			PrepareForWrite ();

	API_AttrTypeID	Type () const	{ return attr.header.typeID; }
	API_AttributeIndex Index () const { return attr.header.index; }

	API_Attribute	attr;
	GS::UniString	name;			// display name (the Archicad layer's placeholder name is replaced by its localized name)
	GS::UniString	rawName;		// non-empty only when 'name' is a substitute: the stored name, written back unchanged
	GS::UniString	bmId;
	GS::UniString	bmManufacturer;
	GS::UniString	bmDescription;
};

// --- RAII definition holder -------------------------------------------------------

class DefsExt {
public:
	DefsExt ();
	~DefsExt ();
	DefsExt (const DefsExt&) = delete;
	DefsExt& operator= (const DefsExt&) = delete;

	// Loads the dynamic definition data; types without definitions yield empty defs (NoError).
	GSErrCode			Load (API_AttrTypeID typeID, API_AttributeIndex index);
	void				Clear ();

	API_AttributeDefExt	defs;
};

// --- Lookup -------------------------------------------------------------------------

// Resolves an attribute reference at os[key]: index | exact name (then case-insensitive) |
// {index} | {name} | {guid} | {attributeId: {guid}}. Throws an actionable error (with name suggestions).
API_AttributeIndex					ResolveRef (API_AttrTypeID typeID, const OS& os, const char* key);
std::optional<API_AttributeIndex>	OptRef (API_AttrTypeID typeID, const OS& os, const char* key);

// {index, name, guid} (pens: {index}); {index} only when the attribute cannot be read.
OS									Ref (API_AttrTypeID typeID, API_AttributeIndex index);

// Pen index 1..255 (allowSpecial: also 0 = transparent and -1 = window background).
std::optional<short>				OptPen (const OS& os, const char* key, bool allowSpecial = false);

// --- Profiles (AttributesProfiles.cpp) -------------------------------------------------

// Builds a new profile vector image from JSON shapes:
//   [{polygon, buildingMaterial, core?, finish?, contourPen?, contourLineType?, contourVisible?,
//     cutEndLinePen?, cutEndLineType?}]
// The caller owns the result (it is handed to ACAPI_Attribute_CreateExt / ModifyExt, whose
// ACAPI_DisposeAttrDefsHdlsExt deletes it).
ProfileVectorImage*					NewProfileImage (const GS::Array<OS>& shapes);

// Replaces the profile geometry in defs (deletes the previous image and its parameter names).
void								SetProfileImage (API_AttributeDefExt& defs, ProfileVectorImage* image);

// Adds width/height/bounds/hatchCount of a profile image to out.
void								AddProfileGeometryJson (const ProfileVectorImage& image, OS& out);

} // namespace attr
} // namespace cc
