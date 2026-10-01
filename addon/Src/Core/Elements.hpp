// *****************************************************************************
// Elements.hpp — element access helpers and the per-type "element adapter"
// registry that powers the generic CreateElements / GetElementDetails /
// ModifyElements commands.
//
// Each element family (walls, slabs, openings, 2D, ...) registers one adapter
// per API_ElemTypeID from its Register*Commands() function:
//
//   cc::RegisterAdapter ({
//       API_WallID,
//       /* create    */ [] (const OS& spec) -> API_Guid { ... ACAPI_Element_Create ...; return guid; },
//       /* serialize */ [] (const API_Element& e, OS& out) { out.Add ("height", e.wall.height); ... },
//       /* modify    */ [] (API_Element& e, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask, const OS& patch) { ... }
//   });
//
// Common fields handled by the core for every type (do not duplicate them):
//   create/modify: "layer", "storyIndex", "renovationStatus", "elementId" (Element ID string)
//   serialize:     "guid", "type", "storyIndex", "layer", "elementId", "groupGuid",
//                  "hotlinkGuid", "renovationStatus", "drawIndex", "locked"
// *****************************************************************************

#pragma once

#include "Core/Json.hpp"
#include "Core/Types.hpp"

namespace cc {

// --- RAII memo -----------------------------------------------------------------

class Memo {
public:
	Memo () { BNZeroMemory (&memo, sizeof (memo)); }
	~Memo () { ACAPI_DisposeElemMemoHdls (&memo); }
	Memo (const Memo&) = delete;
	Memo& operator= (const Memo&) = delete;

	API_ElementMemo*		operator-> ()		{ return &memo; }
	API_ElementMemo&		operator* ()		{ return memo; }
	API_ElementMemo*		Ptr ()				{ return &memo; }

	API_ElementMemo memo;
};

// --- Current database ------------------------------------------------------------

// True for 3D model element types (they live in the floor plan database). 2D types (lines, texts,
// dimensions, labels, hatches, ...) are created in whatever database is current (plan, section, layout...).
bool				IsModelElementType (API_ElemTypeID typeID);

// Makes the floor plan the current database for the object's lifetime when another database
// (layout, section, elevation, detail, worksheet...) is current, and restores it afterwards.
// Archicad refuses to create/change model elements (APIERR_REFUSEDPAR / APIERR_BADDATABASE) otherwise.
class ModelDatabaseScope {
public:
	explicit ModelDatabaseScope (bool enable = true);
	~ModelDatabaseScope ();
	ModelDatabaseScope (const ModelDatabaseScope&) = delete;
	ModelDatabaseScope& operator= (const ModelDatabaseScope&) = delete;

private:
	API_DatabaseInfo	previous;
	bool				switched = false;
};

// --- Element access --------------------------------------------------------------

API_Element			GetElement (const API_Guid& guid);				// throws when missing
API_Elem_Head		GetHeader (const API_Guid& guid);
bool				ElementExists (const API_Guid& guid);
void				LoadMemo (const API_Guid& guid, API_ElementMemo& memo, UInt64 mask = APIMemoMask_All);
API_Element			NewElement (API_ElemTypeID typeID);				// zeroed element with header.type set
// Fills element + memo with the current tool defaults of the given type.
void				GetDefaults (API_Element& element, API_ElementMemo* memo);

GS::Array<API_Guid>	ListElements (API_ElemTypeID typeID);			// all elements of a type (API_ZombieElemID = all types)

GS::UniString		GetElementInfoString (const API_Guid& guid);	// Element ID shown in Info box
void				SetElementInfoString (const API_Guid& guid, const GS::UniString& id);

// --- Common header fields --------------------------------------------------------

OS					HeaderToJson (const API_Elem_Head& head);
// Applies "layer", "storyIndex", "renovationStatus" to element (mask may be null for creation).
void				ApplyCommonFields (API_Element& element, API_Element* mask, const OS& spec);
// Applies fields that need the GUID (after create/change): "elementId".
void				ApplyPostFields (const API_Guid& guid, const OS& spec);

GS::UniString		RenovationStatusName (API_RenovationStatusType status);
API_RenovationStatusType ParseRenovationStatus (const GS::UniString& name);

// --- Structure / surface helpers (walls, columns, beams, slabs, roofs, shells) -----

// Reads "buildingMaterial" | "composite" | "profile" (attribute name or index). The field
// present decides the structure type (Basic / Composite / Profile). Returns true if any was given.
// Pass profile = nullptr for element types that cannot be profiled.
bool				ApplyStructure (const OS& spec, API_ModelElemStructureType& structureType,
									API_AttributeIndex& buildingMaterial, API_AttributeIndex& composite,
									API_AttributeIndex* profile);
// {"structure": "Basic"|"Composite"|"Profile", "buildingMaterial"|"composite"|"profile": {index,name}}
void				AddStructureJson (OS& out, API_ModelElemStructureType structureType,
									  API_AttributeIndex buildingMaterial, API_AttributeIndex composite,
									  API_AttributeIndex profile);

// Surface override: value = surface name/index to override, or false/null-like "" to remove the override.
bool				ApplyOverriddenSurface (const OS& spec, const char* key, API_OverriddenAttribute& attr);
// {index,name} when overridden, otherwise the field is omitted.
void				AddOverriddenSurfaceJson (OS& out, const char* key, const API_OverriddenAttribute& attr);

// --- Adapters --------------------------------------------------------------------

using CreateFn		= std::function<API_Guid (const OS& spec)>;
using SerializeFn	= std::function<void (const API_Element& element, OS& out)>;
using ModifyFn		= std::function<void (API_Element& element, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask, const OS& patch)>;
// Runs after the change with the element as it was before (for settings Archicad resets during the change).
using AfterModifyFn	= std::function<void (const API_Guid& guid, const API_Element& before, const OS& patch)>;

struct ElementAdapter {
	API_ElemTypeID	typeID = API_ZombieElemID;
	CreateFn		create;			// may be empty (type cannot be created)
	SerializeFn		serialize;		// may be empty (only common fields are returned)
	ModifyFn		modify;			// may be empty (only common fields can be modified)
	AfterModifyFn	afterModify;	// may be empty
};

void					RegisterAdapter (const ElementAdapter& adapter);
const ElementAdapter*	FindAdapter (API_ElemTypeID typeID);

// Serializes one element (common header + adapter details).
OS					ElementToJson (const API_Guid& guid);

// Creates one element through its adapter. Must be called inside an undo scope.
API_Guid			CreateElementFromSpec (const OS& spec, API_ElemTypeID typeID);

// Applies the patch to one element through its adapter. Must be called inside an undo scope.
void				ModifyElementFromPatch (const API_Guid& guid, const OS& patch);

// Registers CreateElements, GetElementDetails, ModifyElements, GetSupportedElementTypes.
void				RegisterGenericElementCommands ();

} // namespace cc
