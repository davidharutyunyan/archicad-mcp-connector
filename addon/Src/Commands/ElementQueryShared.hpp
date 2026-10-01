// *****************************************************************************
// ElementQueryShared.hpp — private helpers shared by the ElementQuery*.cpp files
// (family "element-query": find/count elements, quantities, relations,
// sub-elements, selection, 2D/3D geometry). Not part of the Core API.
// *****************************************************************************

#pragma once

#include "Core/Json.hpp"
#include "Core/Types.hpp"
#include "Core/Elements.hpp"

#include <functional>

namespace cc {
namespace eq {

// --- Generic JSON helpers ------------------------------------------------------------

// Items of a JSON array of mixed values (numbers, strings, objects), each wrapped as {"v": item}
// so the Core readers (OptAttr / OptStory / OptGuid ...) can be applied with key "v".
// A missing key returns an empty array; a non-array value is treated as a single item.
GS::Array<OS>		ArrayItems (const OS& os, const char* key);

// Reads an array of element type names ("Wall", ...). Throws listing the valid names on a typo.
GS::Array<API_ElemTypeID> GetElemTypeArray (const OS& os, const char* key);

// Element GUIDs from an array of "guid" | {"guid"} items; empty when the key is missing.
GS::Array<API_Guid>	OptGuidArray (const OS& os, const char* key);

// "Wall, Column, ..." list of every element type name (for error messages).
GS::UniString		AllElemTypeNames ();

// --- Element type classification -----------------------------------------------------

// Sub-element types owned by a hierarchical element (curtain wall parts, stair parts,
// railing parts, beam/column segments). They are skipped by default when listing elements.
bool				IsSubelementType (API_ElemTypeID typeID);
bool				IsHierarchicalType (API_ElemTypeID typeID);	// CurtainWall, Stair, Railing, Beam, Column

// --- Caches (one instance per command call) --------------------------------------------

class NameCache {
public:
	OS					Layer (API_AttributeIndex index);				// {index, name}
	GS::UniString		LayerName (API_AttributeIndex index);
	GS::UniString		LibPartName (Int32 libInd);						// "" when unknown
	GS::UniString		StoryNameOf (short floorInd);
private:
	GS::HashTable<Int32, GS::UniString>	layers;
	GS::HashTable<Int32, GS::UniString>	libParts;
	GS::HashTable<Int32, GS::UniString>	stories;
};

// Compact element description: {guid, type, storyIndex, layer: {index, name}, elementId}.
OS					ElementBrief (const API_Elem_Head& head, NameCache& names, bool withElementId = true);

// Library part index of objects, lamps, windows, doors, skylights and zones (0 = none).
Int32				LibIndOf (const API_Element& element);

// Owner (parent) element of windows/doors/skylights (host wall/roof), labels (labelled element),
// objects placed on elements, and of every sub-element (curtain wall / stair / railing / beam /
// column parts). APINULLGuid when there is none.
API_Guid			OwnerOf (const API_Element& element);

// --- Sub-elements ----------------------------------------------------------------------

struct SubElemInfo {
	API_Elem_Head	head;
	API_ElemTypeID	typeID = API_ZombieElemID;
	UInt32			index = 0;			// position in its memo array
	OS				extra;				// a few type-specific fields (may be empty)
};

// Collects the sub-elements of a hierarchical element (empty for other types). When
// withExtra is true, type-specific fields are filled into SubElemInfo::extra.
GS::Array<SubElemInfo> CollectSubelements (const API_Guid& guid, API_ElemTypeID typeID, bool withExtra);

// --- Polygons stored in raw handles (relation data) ------------------------------------

// Converts coords/pends/parcs handles (memo layout: 1-based coords, closed contours) to the JSON
// polygon format. Counts come from the handle sizes (clamped by poly when given). Returns an
// object with empty "points" when the data is missing or inconsistent. Does not free anything.
OS					HandlesPolygonToJson (API_Coord** coords, Int32** pends, API_PolyArc** parcs, const API_Polygon* poly = nullptr);

// --- Registration (one per file) --------------------------------------------------------

void				RegisterFindCommands ();		// ElementQuery.cpp
void				RegisterSelectionCommands ();	// ElementQuery.cpp
void				RegisterQuantityCommands ();	// ElementQueryQuantities.cpp
void				RegisterRelationCommands ();	// ElementQueryRelations.cpp
void				RegisterGeometryCommands ();	// ElementQueryGeometry.cpp

} // namespace eq
} // namespace cc
