#include "Core/Elements.hpp"
#include "Core/Command.hpp"

namespace cc {

// --- Current database --------------------------------------------------------------

bool IsModelElementType (API_ElemTypeID typeID)
{
	switch (typeID) {
		case API_WallID: case API_ColumnID: case API_BeamID: case API_WindowID: case API_DoorID:
		case API_ObjectID: case API_LampID: case API_SlabID: case API_RoofID: case API_MeshID:
		case API_ZoneID: case API_CurtainWallID: case API_ShellID: case API_SkylightID: case API_MorphID:
		case API_StairID: case API_RailingID: case API_OpeningID: case API_BeamSegmentID: case API_ColumnSegmentID:
		case API_CurtainWallSegmentID: case API_CurtainWallFrameID: case API_CurtainWallPanelID:
		case API_CurtainWallJunctionID: case API_CurtainWallAccessoryID:
			return true;
		default:
			return false;
	}
}


ModelDatabaseScope::ModelDatabaseScope (bool enable)
{
	BNZeroMemory (&previous, sizeof (previous));
	if (!enable || ACAPI_Database (APIDb_GetCurrentDatabaseID, &previous) != NoError)
		return;
	if (previous.typeID == APIWind_FloorPlanID)
		return;
	API_DatabaseInfo plan;
	BNZeroMemory (&plan, sizeof (plan));
	plan.typeID = APIWind_FloorPlanID;
	switched = ACAPI_Database (APIDb_ChangeCurrentDatabaseID, &plan) == NoError;
}


ModelDatabaseScope::~ModelDatabaseScope ()
{
	if (switched)
		ACAPI_Database (APIDb_ChangeCurrentDatabaseID, &previous);
}

// --- Element access ----------------------------------------------------------------

API_Element NewElement (API_ElemTypeID typeID)
{
	API_Element element;
	BNZeroMemory (&element, sizeof (element));
	element.header.type = API_ElemType (typeID);
	return element;
}


API_Element GetElement (const API_Guid& guid)
{
	API_Element element;
	BNZeroMemory (&element, sizeof (element));
	element.header.guid = guid;
	GSErrCode err = ACAPI_Element_Get (&element);
	if (err != NoError)
		Fail ("Element " + GuidStr (guid) + " not found or not accessible: " + ErrorName (err), err);
	return element;
}


API_Elem_Head GetHeader (const API_Guid& guid)
{
	API_Elem_Head head;
	BNZeroMemory (&head, sizeof (head));
	head.guid = guid;
	GSErrCode err = ACAPI_Element_GetHeader (&head);
	if (err != NoError)
		Fail ("Element " + GuidStr (guid) + " not found: " + ErrorName (err), err);
	return head;
}


bool ElementExists (const API_Guid& guid)
{
	API_Elem_Head head;
	BNZeroMemory (&head, sizeof (head));
	head.guid = guid;
	return ACAPI_Element_GetHeader (&head) == NoError;
}


void LoadMemo (const API_Guid& guid, API_ElementMemo& memo, UInt64 mask)
{
	Check (ACAPI_Element_GetMemo (guid, &memo, mask), "Cannot read memo of element " + GuidStr (guid));
}


void GetDefaults (API_Element& element, API_ElementMemo* memo)
{
	Check (ACAPI_Element_GetDefaults (&element, memo), "Cannot get default settings of " + ElemTypeName (element.header.type));
}


GS::Array<API_Guid> ListElements (API_ElemTypeID typeID)
{
	GS::Array<API_Guid> result;
	Check (ACAPI_Element_GetElemList (API_ElemType (typeID), &result), "Cannot list elements");
	return result;
}


GS::UniString GetElementInfoString (const API_Guid& guid)
{
	GS::UniString info;
	ACAPI_Database (APIDb_GetElementInfoStringID, const_cast<API_Guid*> (&guid), &info);
	return info;
}


void SetElementInfoString (const API_Guid& guid, const GS::UniString& id)
{
	GS::UniString info = id;
	Check (ACAPI_Database (APIDb_ChangeElementInfoStringID, const_cast<API_Guid*> (&guid), &info), "Cannot set Element ID");
}

// --- Common fields -----------------------------------------------------------------

GS::UniString RenovationStatusName (API_RenovationStatusType status)
{
	switch (status) {
		case API_ExistingStatus:	return "Existing";
		case API_NewStatus:			return "New";
		case API_DemolishedStatus:	return "Demolished";
		case API_DefaultStatus:		return "Default";
		default:					return "Undefined";
	}
}


API_RenovationStatusType ParseRenovationStatus (const GS::UniString& name)
{
	if (EqualsIgnoreCase (name, "Existing"))	return API_ExistingStatus;
	if (EqualsIgnoreCase (name, "New"))			return API_NewStatus;
	if (EqualsIgnoreCase (name, "Demolished"))	return API_DemolishedStatus;
	if (EqualsIgnoreCase (name, "Default"))		return API_DefaultStatus;
	Fail ("renovationStatus must be one of Existing, New, Demolished, Default.");
}


OS HeaderToJson (const API_Elem_Head& head)
{
	OS out;
	out.Add ("guid", GuidStr (head.guid));
	out.Add ("type", ElemTypeName (head.type));
	out.Add ("storyIndex", (Int32) head.floorInd);
	out.Add ("layer", AttrRef (API_LayerID, head.layer));
	out.Add ("elementId", GetElementInfoString (head.guid));
	if (head.groupGuid != APINULLGuid)
		out.Add ("groupGuid", GuidStr (head.groupGuid));
	if (head.hotlinkGuid != APINULLGuid)
		out.Add ("hotlinkGuid", GuidStr (head.hotlinkGuid));
	out.Add ("renovationStatus", RenovationStatusName (head.renovationStatus));
	out.Add ("drawIndex", (Int32) head.drwIndex);
	out.Add ("locked", head.lockId != 0);
	return out;
}


// The "link to story" settings of the element types that have them (nullptr otherwise).
static API_LinkToSettings* LinkToSettingsOf (API_Element& element)
{
	switch (element.header.type.typeID) {
		case API_WallID:			return &element.wall.linkToSettings;
		case API_CurtainWallID:		return &element.curtainWall.linkToSettings;
		case API_ColumnID:			return &element.column.linkToSettings;
		case API_BeamID:			return &element.beam.linkToSettings;
		case API_ObjectID:			return &element.object.linkToSettings;
		case API_LampID:			return &element.lamp.linkToSettings;
		case API_SlabID:			return &element.slab.linkToSettings;
		case API_RoofID:			return &element.roof.shellBase.linkToSettings;
		case API_ShellID:			return &element.shell.shellBase.linkToSettings;
		case API_MorphID:			return &element.morph.linkToSettings;
		case API_MeshID:			return &element.mesh.linkToSettings;
		case API_ZoneID:			return &element.zone.linkToSettings;
		case API_StairID:			return &element.stair.linkToSettings;
		case API_RailingID:			return &element.railing.linkToSettings;
		default:					return nullptr;
	}
}


void ApplyCommonFields (API_Element& element, API_Element* mask, const OS& spec)
{
	if (auto layer = OptAttr (API_LayerID, spec, "layer")) {
		element.header.layer = *layer;
		if (mask) ACAPI_ELEMENT_MASK_SET (*mask, API_Elem_Head, layer);
	}
	if (auto story = OptStory (spec, "storyIndex")) {
		element.header.floorInd = *story;
		if (mask) {
			ACAPI_ELEMENT_MASK_SET (*mask, API_Elem_Head, floorInd);
		} else if (API_LinkToSettings* link = LinkToSettingsOf (element)) {
			// On creation an explicit story must win over a "relative to the current story" tool default.
			link->newCreationMode = false;
			link->homeStoryDifference = 0;
		}
	}
	if (auto reno = OptString (spec, "renovationStatus")) {
		element.header.renovationStatus = ParseRenovationStatus (*reno);
		if (mask) ACAPI_ELEMENT_MASK_SET (*mask, API_Elem_Head, renovationStatus);
	}
	// drawIndex: honoured on creation / tool defaults. ACAPI_Element_Change may ignore drwIndex, so
	// ModifyElementFromPatch additionally steps it with the Bring Forward / Send Backward tools.
	if (auto drw = OptInt (spec, "drawIndex")) {
		if (*drw < 1 || *drw > 14)
			Fail ("'drawIndex' must be between 1 (bottom) and 14 (top).");
		element.header.drwIndex = (char) *drw;
		if (mask) ACAPI_ELEMENT_MASK_SET (*mask, API_Elem_Head, drwIndex);
	}
}


// Steps the element's drawing order to the target level (1..14) with the Bring Forward / Send
// Backward tools. Must run inside an undo scope.
static void ApplyDrawIndex (const API_Guid& guid, Int32 level)
{
	if (level < 1 || level > 14)
		Fail ("'drawIndex' must be between 1 (bottom) and 14 (top).");
	Int32 cur = (Int32) GetHeader (guid).drwIndex;
	for (Int32 iter = 0; iter < 20 && cur != level; ++iter) {
		const API_ToolCmdID step = cur < level ? APITool_BringForward : APITool_SendBackward;
		Check (ACAPI_Element_Tool (GS::Array<API_Guid> { guid }, step, nullptr), "Cannot change the drawing order of " + GuidStr (guid));
		const Int32 next = (Int32) GetHeader (guid).drwIndex;
		if (next == cur)
			break;
		cur = next;
	}
	if (cur != level)
		Fail (GS::UniString::Printf ("Drawing order stopped at %d instead of %d (windows/doors cannot go below their host; some types have fixed limits). Use set_draw_order for details.", (int) cur, (int) level), APIERR_REFUSEDPAR);
}


// Verified live (Archicad 26): APIDb_ChangeElementInfoStringID returns APIERR_BADELEMENTTYPE for lines,
// polylines, arcs, circles, splines, texts, labels, hotspots, pictures, linear and level dimensions
// (radial / angle dimensions assumed alike); hatches do have an Element ID.
static bool HasElementId (API_ElemTypeID typeID)
{
	switch (typeID) {
		case API_LineID:
		case API_PolyLineID:
		case API_ArcID:
		case API_CircleID:
		case API_SplineID:
		case API_TextID:
		case API_LabelID:
		case API_HotspotID:
		case API_PictureID:
		case API_DimensionID:
		case API_RadialDimensionID:
		case API_LevelDimensionID:
		case API_AngleDimensionID:
			return false;
		default:
			return true;
	}
}


static void CheckElementIdSupported (API_ElemTypeID typeID, const OS& spec)
{
	if (spec.Contains ("elementId") && !HasElementId (typeID))
		Fail (ElemTypeName (typeID) + " elements have no Element ID in Archicad; omit 'elementId'.", APIERR_BADELEMENTTYPE);
}


void ApplyPostFields (const API_Guid& guid, const OS& spec)
{
	if (auto id = OptString (spec, "elementId"))
		SetElementInfoString (guid, *id);
}

// --- Structure / surface helpers ---------------------------------------------------

bool ApplyStructure (const OS& spec, API_ModelElemStructureType& structureType,
					 API_AttributeIndex& buildingMaterial, API_AttributeIndex& composite,
					 API_AttributeIndex* profile)
{
	if (auto bm = OptAttr (API_BuildingMaterialID, spec, "buildingMaterial")) {
		structureType = API_BasicStructure;
		buildingMaterial = *bm;
		return true;
	}
	if (auto comp = OptAttr (API_CompWallID, spec, "composite")) {
		structureType = API_CompositeStructure;
		composite = *comp;
		return true;
	}
	if (spec.Contains ("profile")) {
		if (profile == nullptr)
			Fail ("This element type does not support complex profiles.", APIERR_NOTSUPPORTED);
		structureType = API_ProfileStructure;
		*profile = GetAttr (API_ProfileID, spec, "profile");
		return true;
	}
	return false;
}


void AddStructureJson (OS& out, API_ModelElemStructureType structureType,
					   API_AttributeIndex buildingMaterial, API_AttributeIndex composite,
					   API_AttributeIndex profile)
{
	switch (structureType) {
		case API_BasicStructure:
			out.Add ("structure", GS::UniString ("Basic"));
			out.Add ("buildingMaterial", AttrRef (API_BuildingMaterialID, buildingMaterial));
			break;
		case API_CompositeStructure:
			out.Add ("structure", GS::UniString ("Composite"));
			out.Add ("composite", AttrRef (API_CompWallID, composite));
			break;
		case API_ProfileStructure:
			out.Add ("structure", GS::UniString ("Profile"));
			out.Add ("profile", AttrRef (API_ProfileID, profile));
			break;
	}
}


bool ApplyOverriddenSurface (const OS& spec, const char* key, API_OverriddenAttribute& attr)
{
	if (!spec.Contains (key))
		return false;
	if (spec.IsBool (key)) {
		if (GetBool (spec, key))
			Fail ("Field '" + GS::UniString (key) + "' must be a surface name/index, or false to remove the override.");
		attr.overridden = false;
		return true;
	}
	if (spec.IsString (key) && GetString (spec, key).IsEmpty ()) {
		attr.overridden = false;
		return true;
	}
	attr.attributeIndex = GetAttr (API_MaterialID, spec, key);
	attr.overridden = true;
	return true;
}


void AddOverriddenSurfaceJson (OS& out, const char* key, const API_OverriddenAttribute& attr)
{
	if (attr.overridden)
		out.Add (key, AttrRef (API_MaterialID, attr.attributeIndex));
}

// --- Adapters ----------------------------------------------------------------------

namespace {
	GS::HashTable<Int32, ElementAdapter>& Adapters ()
	{
		static GS::HashTable<Int32, ElementAdapter> adapters;
		return adapters;
	}
}


void RegisterAdapter (const ElementAdapter& adapter)
{
	Adapters ().Put ((Int32) adapter.typeID, adapter);
}


const ElementAdapter* FindAdapter (API_ElemTypeID typeID)
{
	return Adapters ().GetPtr ((Int32) typeID);
}


OS ElementToJson (const API_Guid& guid)
{
	API_Element element = GetElement (guid);
	OS out = HeaderToJson (element.header);
	const ElementAdapter* adapter = FindAdapter (element.header.type.typeID);
	if (adapter != nullptr && adapter->serialize) {
		OS details;
		adapter->serialize (element, details);
		out.Add ("details", details);
	}
	return out;
}


API_Guid CreateElementFromSpec (const OS& spec, API_ElemTypeID typeID)
{
	const ElementAdapter* adapter = FindAdapter (typeID);
	if (adapter == nullptr || !adapter->create)
		Fail ("Creating elements of type '" + ElemTypeName (typeID) + "' is not supported.", APIERR_NOTSUPPORTED);
	CheckElementIdSupported (typeID, spec);
	ModelDatabaseScope planScope (IsModelElementType (typeID));
	API_Guid guid = adapter->create (spec);
	try {
		ApplyPostFields (guid, spec);
	} catch (...) {
		// The item reports an error, so it must not leave its element behind.
		GS::Array<API_Guid> created;
		created.Push (guid);
		ACAPI_Element_Delete (created);
		throw;
	}
	return guid;
}


void ModifyElementFromPatch (const API_Guid& guid, const OS& patch)
{
	const API_ElemTypeID typeID = GetHeader (guid).type.typeID;
	CheckElementIdSupported (typeID, patch);
	ModelDatabaseScope planScope (IsModelElementType (typeID));
	API_Element element = GetElement (guid);
	const API_Element before = element;
	API_Element mask;
	ACAPI_ELEMENT_MASK_CLEAR (mask);
	Memo memo;
	UInt64 memoMask = 0;

	ApplyCommonFields (element, &mask, patch);

	const ElementAdapter* adapter = FindAdapter (element.header.type.typeID);
	if (adapter != nullptr && adapter->modify)
		adapter->modify (element, mask, *memo, memoMask, patch);

	// Only call Change when something is masked.
	bool anyMasked = memoMask != 0;
	if (!anyMasked) {
		const char* bytes = reinterpret_cast<const char*> (&mask);
		for (size_t i = 0; i < sizeof (mask); ++i) {
			if (bytes[i] != 0) { anyMasked = true; break; }
		}
	}
	if (anyMasked) {
		Check (ACAPI_Element_Change (&element, &mask, memoMask != 0 ? memo.Ptr () : nullptr, memoMask, true),
			   "Cannot modify element " + GuidStr (guid));
		if (adapter != nullptr && adapter->afterModify)
			adapter->afterModify (guid, before, patch);
	}
	if (auto drw = OptInt (patch, "drawIndex"))
		ApplyDrawIndex (guid, *drw);
	ApplyPostFields (guid, patch);
}

// --- Generic commands --------------------------------------------------------------

static GS::Array<API_Guid> GuidsOfItems (const OS& params, const char* key)
{
	return GetGuidArray (params, key, true);
}


void RegisterGenericElementCommands ()
{
	RegisterCommand ("CreateElements",
		"Creates elements of any supported type in one undo step. Input: {elements: [{type: 'Wall', ...type fields}]}. "
		"Output: {results: [{guid, type} | {error}]}.",
		[] (const OS& params) -> OS {
			GS::Array<OS> specs = GetObjectArray (params, "elements");
			GS::Array<OS> results;
			GS::UniString undoName = GetString (params, "undoName", "Create elements (Claude)");
			Undoable (undoName, [&] () {
				for (const OS& spec : specs) {
					results.Push (Try ([&] () -> OS {
						API_ElemTypeID typeID = GetElemType (spec, "type");
						API_Guid guid = CreateElementFromSpec (spec, typeID);
						return OS ("guid", GuidStr (guid), "type", ElemTypeName (typeID));
					}));
				}
			});
			return OS ("results", results);
		});

	RegisterCommand ("GetElementDetails",
		"Returns common header fields and type-specific details for elements. Input: {elements: [guid | {guid}]}.",
		[] (const OS& params) -> OS {
			GS::Array<API_Guid> guids = GuidsOfItems (params, "elements");
			GS::Array<OS> out;
			for (const API_Guid& guid : guids)
				out.Push (Try ([&] () { return ElementToJson (guid); }));
			return OS ("elements", out);
		});

	RegisterCommand ("ModifyElements",
		"Modifies elements in one undo step. Input: {elements: [{guid, ...fields to change}]}. Output: {results: [{guid} | {error}]}.",
		[] (const OS& params) -> OS {
			GS::Array<OS> patches = GetObjectArray (params, "elements");
			GS::Array<OS> results;
			Undoable (GetString (params, "undoName", "Modify elements (Claude)"), [&] () {
				for (const OS& patch : patches) {
					results.Push (Try ([&] () -> OS {
						API_Guid guid = GetGuid (patch, "guid");
						ModifyElementFromPatch (guid, patch);
						return OS ("guid", GuidStr (guid));
					}));
				}
			});
			return OS ("results", results);
		});

	RegisterCommand ("GetSupportedElementTypes",
		"Lists element types and whether they can be created / serialized / modified by this add-on.",
		[] (const OS&) -> OS {
			GS::Array<OS> types;
			for (API_ElemTypeID typeID : AllElemTypes ()) {
				const ElementAdapter* adapter = FindAdapter (typeID);
				types.Push (OS ("type", ElemTypeName (typeID),
								"create", adapter != nullptr && (bool) adapter->create,
								"details", adapter != nullptr && (bool) adapter->serialize,
								"modify", adapter != nullptr && (bool) adapter->modify));
			}
			return OS ("types", types);
		});
}

} // namespace cc
