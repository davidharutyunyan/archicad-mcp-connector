// *****************************************************************************
// Types.hpp — element type and attribute type names (same spelling as the
// official Archicad JSON API: "Wall", "Column", "CurtainWall", ...).
// *****************************************************************************

#pragma once

#include "Core/Json.hpp"

namespace cc {

// --- Element types -----------------------------------------------------------

GS::UniString					ElemTypeName (API_ElemTypeID typeID);
GS::UniString					ElemTypeName (const API_ElemType& type);
std::optional<API_ElemTypeID>	ParseElemType (const GS::UniString& name);		// case-insensitive
API_ElemTypeID					GetElemType (const OS& os, const char* key);		// throws on unknown
const GS::Array<API_ElemTypeID>& AllElemTypes ();									// every concrete type

// --- Attribute types ---------------------------------------------------------
//   "Pen", "Layer", "Line", "Fill", "Composite", "Surface", "LayerCombination",
//   "ZoneCategory", "Font", "Profile", "PenTable", "DimensionStandard",
//   "ModelViewOption", "MEPSystem", "OperationProfile", "BuildingMaterial"

GS::UniString					AttrTypeName (API_AttrTypeID typeID);
std::optional<API_AttrTypeID>	ParseAttrType (const GS::UniString& name);		// case-insensitive, accepts aliases (e.g. "Material", "LineType", "FillType")
API_AttrTypeID					GetAttrType (const OS& os, const char* key);
const GS::Array<API_AttrTypeID>& AllAttrTypes ();

// --- Attribute lookup --------------------------------------------------------

GS::UniString					AttrName (API_AttrTypeID typeID, API_AttributeIndex index);	// "" when not found
std::optional<API_AttributeIndex> FindAttrByName (API_AttrTypeID typeID, const GS::UniString& name);
bool							AttrExists (API_AttrTypeID typeID, API_AttributeIndex index);

// Reads an attribute reference from os[key]. Accepted forms:
//   5                    (index)
//   "Structural - Bearing" (name, exact match first, then case-insensitive)
//   {"index": 5} | {"name": "..."} | {"attributeId": {"guid": "..."}} | {"guid": "..."}
std::optional<API_AttributeIndex> OptAttr (API_AttrTypeID typeID, const OS& os, const char* key);
API_AttributeIndex				GetAttr (API_AttrTypeID typeID, const OS& os, const char* key);

// {"index": n, "name": "...", "guid": "..."}
OS								AttrRef (API_AttrTypeID typeID, API_AttributeIndex index);

// --- Stories -----------------------------------------------------------------

// Story index accepted forms: integer floor index, or {"name": "..."} / story name string.
std::optional<short>			OptStory (const OS& os, const char* key);
short							CurrentStoryIndex ();
double							StoryLevel (short floorInd);						// absolute elevation of a story
GS::UniString					StoryName (short floorInd);

// Makes `floorInd` the current story for the lifetime of the object and restores the previous
// current story afterwards (no-op when it is current already). Needed where Archicad reads the
// CURRENT story instead of the element's own one, e.g. the boundary detection of automatic zones.
class CurrentStoryScope {
public:
	explicit CurrentStoryScope (short floorInd);
	~CurrentStoryScope ();
	CurrentStoryScope (const CurrentStoryScope&) = delete;
	CurrentStoryScope& operator= (const CurrentStoryScope&) = delete;

private:
	std::optional<short> previous;
};

} // namespace cc
