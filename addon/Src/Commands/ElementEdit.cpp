// *****************************************************************************
// ElementEdit — generic editing of ANY element type (one undo step per call).
//
//   Transformations (ACAPI_Element_Edit):
//     MoveElements, CopyElements, RotateElements, MirrorElements, ElevateElements, ResizeElements
//       input: one operation at top level, or {operations: [op, ...]} (all in one undo step)
//   DeleteElements, CopyElementsToStories
//   Groups / locking / drawing order (ACAPI_ElementGroup_*, ACAPI_Element_Tool):
//     GroupElements, UngroupElements, SetSuspendGroups, LockElements, UnlockElements, SetDrawOrder
//   Composition: TrimElements, RemoveTrims, MergeElements, UnmergeElements,
//     CreateSolidOperations, RemoveSolidOperations, GetElementEditRelations
//
// Units: meters and DEGREES. Element references: "GUID" or {"guid": "GUID"}; group GUIDs are
// accepted wherever elements are (they stand for all elements of the group).
//
// Group handling ("includeGroupMembers", default false): when Suspend Groups is OFF Archicad
// silently extends every edit/delete/lock to all members of the groups of the given elements.
// By default these commands therefore switch Suspend Groups ON for the duration of the call
// (and restore it afterwards) so that ONLY the given elements are affected. With
// includeGroupMembers = true all members of the (root) groups are added explicitly instead.
//
// SAFETY: ACAPI_Element_Edit / ACAPI_Element_Tool operate on the current SELECTION when they
// receive an empty element list — every call site below guards against empty lists.
// *****************************************************************************

#include "Commands/Commands.hpp"
#include "Core/Command.hpp"
#include "Core/Elements.hpp"
#include "Core/Enums.hpp"

#include <cmath>

namespace cc {

namespace {

constexpr Int32 kMaxCopyCount		= 1000;
constexpr Int32 kMaxListedCreated	= 2000;
constexpr double kEps				= 1e-9;

// =============================================================================
// Small utilities
// =============================================================================

GS::Array<GS::UniString> GuidStrings (const GS::Array<API_Guid>& guids)
{
	GS::Array<GS::UniString> out;
	for (const API_Guid& g : guids)
		out.Push (GuidStr (g));
	return out;
}


// Runs fn; on cc::Error stores the {code, message} object in err and returns false.
bool Attempt (const std::function<void ()>& fn, OS& err)
{
	OS r = Try ([&] () -> OS { fn (); return OS (); });
	if (r.Contains ("error")) {
		r.Get ("error", err);
		return false;
	}
	return true;
}


OS ErrorOf (GSErrCode code, const GS::UniString& message)
{
	return OS ("code", (Int32) code, "message", message);
}


OS ItemError (const API_Guid& guid, const OS& error)
{
	return OS ("guid", GuidStr (guid), "error", error);
}


GS::UniString DatabaseTypeName (API_DatabaseTypeID typeID)
{
	switch (typeID) {
		case APIWind_FloorPlanID:			return "Floor Plan";
		case APIWind_SectionID:				return "Section";
		case APIWind_DetailID:				return "Detail";
		case APIWind_3DModelID:				return "3D Model";
		case APIWind_LayoutID:				return "Layout";
		case APIWind_DrawingID:				return "Drawing";
		case APIWind_MasterLayoutID:		return "Master Layout";
		case APIWind_ElevationID:			return "Elevation";
		case APIWind_InteriorElevationID:	return "Interior Elevation";
		case APIWind_WorksheetID:			return "Worksheet";
		case APIWind_DocumentFrom3DID:		return "3D Document";
		default:							return "other";
	}
}


GS::UniString CurrentDatabaseName ()
{
	API_DatabaseInfo info;
	BNZeroMemory (&info, sizeof (info));
	if (ACAPI_Database (APIDb_GetCurrentDatabaseID, &info, nullptr) != NoError)
		return "unknown";
	return DatabaseTypeName (info.typeID);
}


// Like Check (), with hints that tell Claude what to do next.
void CheckOp (GSErrCode err, const GS::UniString& what)
{
	if (err == NoError)
		return;
	GS::UniString msg = what + ": " + ErrorName (err);
	switch (err) {
		case APIERR_BADDATABASE:
			msg += ". The active window/database (" + CurrentDatabaseName () + ") does not allow this operation on these elements: "
				   "bring the Floor Plan window to the front (model elements live there; 2D elements of a layout/detail/worksheet need that window) and retry.";
			break;
		case APIERR_NOTMINE:
		case APIERR_NOACCESSRIGHT:
			msg += ". Teamwork: reserve the elements first.";
			break;
		case APIERR_NOTEDITABLE:
		case APIERR_LOCKEDLAY:
			msg += ". Unlock the element (unlock_elements) or its layer, and make sure its layer is visible.";
			break;
		case APIERR_NO3D:
		case APIERR_BADELEMENTTYPE:
			msg += ". Only construction elements (Wall, Column, Beam, Slab, Roof, Shell, Morph, Curtain Wall, Window/Door/Skylight, Object...) support this; check the element types with get_element_details.";
			break;
		default:
			break;
	}
	throw Error (msg, err);
}


bool IsHostedType (API_ElemTypeID t)
{
	return t == API_WindowID || t == API_DoorID || t == API_SkylightID || t == API_OpeningID;
}


// Parts of hierarchical elements (curtain wall, stair, railing, segmented beam/column) and
// section/elevation representations: they follow their parent and cannot be edited alone.
bool IsSubElementType (API_ElemTypeID t)
{
	switch (t) {
		case API_CurtainWallSegmentID:
		case API_CurtainWallFrameID:
		case API_CurtainWallPanelID:
		case API_CurtainWallJunctionID:
		case API_CurtainWallAccessoryID:
		case API_RiserID:
		case API_TreadID:
		case API_StairStructureID:
		case API_RailingToprailID:
		case API_RailingHandrailID:
		case API_RailingRailID:
		case API_RailingPostID:
		case API_RailingInnerPostID:
		case API_RailingBalusterID:
		case API_RailingPanelID:
		case API_RailingSegmentID:
		case API_RailingNodeID:
		case API_RailingBalusterSetID:
		case API_RailingPatternID:
		case API_RailingToprailEndID:
		case API_RailingHandrailEndID:
		case API_RailingRailEndID:
		case API_RailingToprailConnectionID:
		case API_RailingHandrailConnectionID:
		case API_RailingRailConnectionID:
		case API_RailingEndFinishID:
		case API_BeamSegmentID:
		case API_ColumnSegmentID:
		case API_SectElemID:
			return true;
		default:
			return false;
	}
}


bool IsSubElement (const API_Guid& guid)
{
	API_Elem_Head head;
	BNZeroMemory (&head, sizeof (head));
	head.guid = guid;
	return ACAPI_Element_GetHeader (&head) == NoError && IsSubElementType (head.type.typeID);
}


bool IsRoofOrShell (API_ElemTypeID t)
{
	return t == API_RoofID || t == API_ShellID;
}


// Pure 2D (drafting / documentation) types: they have no elevation, Elevate ignores them.
bool Is2DOnlyType (API_ElemTypeID t)
{
	switch (t) {
		case API_DimensionID:
		case API_RadialDimensionID:
		case API_LevelDimensionID:
		case API_AngleDimensionID:
		case API_TextID:
		case API_LabelID:
		case API_HatchID:
		case API_LineID:
		case API_PolyLineID:
		case API_ArcID:
		case API_CircleID:
		case API_SplineID:
		case API_PictureID:
		case API_DrawingID:
		case API_DetailID:
		case API_WorksheetID:
		case API_CutPlaneID:
		case API_ElevationID:
		case API_InteriorElevationID:
		case API_ChangeMarkerID:
			return true;
		default:
			return false;
	}
}

// =============================================================================
// Neigs (element references for ACAPI_Element_Edit) — same mapping as the DevKit
// examples (APICommon.c: ElemHead_To_Neig). Unmapped types use a plain GUID neig.
// =============================================================================

API_Neig NeigOf (const API_Elem_Head& head)
{
	API_Neig neig (head.guid);
	neig.inIndex = 1;
	switch (head.type.typeID) {
		case API_WallID:					neig.neigID = APINeig_Wall;					break;
		case API_ColumnID:					neig.neigID = APINeig_Colu;					neig.inIndex = 0;	break;
		case API_BeamID:					neig.neigID = APINeig_Beam;					break;
		case API_WindowID:					neig.neigID = APINeig_WindHole;				neig.inIndex = 0;	break;
		case API_DoorID:					neig.neigID = APINeig_DoorHole;				neig.inIndex = 0;	break;
		case API_ObjectID:					neig.neigID = APINeig_Symb;					break;
		case API_LampID:					neig.neigID = APINeig_Light;				break;
		case API_SlabID:					neig.neigID = APINeig_Ceil;					break;
		case API_RoofID:					neig.neigID = APINeig_Roof;					break;
		case API_MeshID:					neig.neigID = APINeig_Mesh;					break;
		case API_DimensionID:				neig.neigID = APINeig_DimOn;				break;
		case API_RadialDimensionID:			neig.neigID = APINeig_RadDim;				break;
		case API_LevelDimensionID:			neig.neigID = APINeig_LevDim;				break;
		case API_AngleDimensionID:			neig.neigID = APINeig_AngDimOn;				break;
		case API_TextID:					neig.neigID = APINeig_Word;					break;
		case API_LabelID:					neig.neigID = APINeig_Label;				break;
		case API_ZoneID:					neig.neigID = APINeig_Room;					break;
		case API_HatchID:					neig.neigID = APINeig_Hatch;				break;
		case API_LineID:					neig.neigID = APINeig_Line;					break;
		case API_PolyLineID:				neig.neigID = APINeig_PolyLine;				break;
		case API_ArcID:						neig.neigID = APINeig_Arc;					break;
		case API_CircleID:					neig.neigID = APINeig_Circ;					break;
		case API_SplineID:					neig.neigID = APINeig_Spline;				break;
		case API_HotspotID:					neig.neigID = APINeig_Hot;					break;
		case API_CutPlaneID:				neig.neigID = APINeig_CutPlane;				break;
		case API_ElevationID:				neig.neigID = APINeig_Elevation;			break;
		case API_InteriorElevationID:		neig.neigID = APINeig_InteriorElevation;	break;
		case API_CameraID:					neig.neigID = APINeig_Camera;				break;
		case API_PictureID:					neig.neigID = APINeig_PictObj;				break;
		case API_DetailID:					neig.neigID = APINeig_Detail;				break;
		case API_WorksheetID:				neig.neigID = APINeig_Worksheet;			break;
		case API_SectElemID:				neig.neigID = APINeig_VirtSy;				break;
		case API_DrawingID:					neig.neigID = APINeig_DrawingCenter;		break;
		case API_CurtainWallID:				neig.neigID = APINeig_CurtainWall;			break;
		case API_CurtainWallSegmentID:		neig.neigID = APINeig_CWSegment;			break;
		case API_CurtainWallFrameID:		neig.neigID = APINeig_CWFrame;				break;
		case API_CurtainWallPanelID:		neig.neigID = APINeig_CWPanel;				break;
		case API_CurtainWallJunctionID:		neig.neigID = APINeig_CWJunction;			break;
		case API_CurtainWallAccessoryID:	neig.neigID = APINeig_CWAccessory;			break;
		case API_ShellID:					neig.neigID = APINeig_Shell;				break;
		case API_SkylightID:				neig.neigID = APINeig_SkylightHole;			neig.inIndex = 0;	break;
		case API_MorphID:					neig.neigID = APINeig_Morph;				break;
		case API_ChangeMarkerID:			neig.neigID = APINeig_ChangeMarker;			break;
		case API_StairID:					neig.neigID = APINeig_Stair;				break;
		case API_RiserID:					neig.neigID = APINeig_Riser;				break;
		case API_TreadID:					neig.neigID = APINeig_Tread;				break;
		case API_StairStructureID:			neig.neigID = APINeig_StairStructure;		break;
		case API_RailingID:					neig.neigID = APINeig_Railing;				break;
		case API_RailingToprailID:			neig.neigID = APINeig_RailingToprail;		break;
		case API_RailingHandrailID:			neig.neigID = APINeig_RailingHandrail;		break;
		case API_RailingRailID:				neig.neigID = APINeig_RailingRail;			break;
		case API_RailingPostID:				neig.neigID = APINeig_RailingPost;			break;
		case API_RailingInnerPostID:		neig.neigID = APINeig_RailingInnerPost;		break;
		case API_RailingBalusterID:			neig.neigID = APINeig_RailingBaluster;		break;
		case API_RailingPanelID:			neig.neigID = APINeig_RailingPanel;			break;
		case API_RailingToprailEndID:		neig.neigID = APINeig_RailingToprailEnd;	break;
		case API_RailingHandrailEndID:		neig.neigID = APINeig_RailingHandrailEnd;	break;
		case API_RailingRailEndID:			neig.neigID = APINeig_RailingRailEnd;		break;
		case API_RailingToprailConnectionID:	neig.neigID = APINeig_RailingToprailConnection;		break;
		case API_RailingHandrailConnectionID:	neig.neigID = APINeig_RailingHandrailConnection;	break;
		case API_RailingRailConnectionID:		neig.neigID = APINeig_RailingRailConnection;		break;
		case API_RailingEndFinishID:		neig.neigID = APINeig_RailingEndFinish;		break;
		case API_BeamSegmentID:				neig.neigID = APINeig_BeamSegment;			break;
		case API_ColumnSegmentID:			neig.neigID = APINeig_ColumnSegment;		break;
		default:							neig.neigID = APINeig_None;					neig.inIndex = 0;	break;
	}
	return neig;
}

// =============================================================================
// Element sets / snapshots
// =============================================================================

GS::HashSet<API_Guid> AllElementGuids ()
{
	GS::Array<API_Guid> list;
	ACAPI_Element_GetElemList (API_ElemType (API_ZombieElemID), &list);
	GS::HashSet<API_Guid> set;
	for (const API_Guid& g : list)
		set.Add (g);
	return set;
}


// guid -> element type of every element in the current database.
GS::HashTable<API_Guid, Int32> AllElementTypes ()
{
	GS::HashTable<API_Guid, Int32> table;
	for (API_ElemTypeID typeID : AllElemTypes ()) {
		GS::Array<API_Guid> list;
		if (ACAPI_Element_GetElemList (API_ElemType (typeID), &list) != NoError)
			continue;
		for (const API_Guid& g : list)
			table.Put (g, (Int32) typeID);
	}
	return table;
}


OS GuidTypeObj (const API_Guid& guid)
{
	OS o ("guid", GuidStr (guid));
	API_Elem_Head head;
	BNZeroMemory (&head, sizeof (head));
	head.guid = guid;
	if (ACAPI_Element_GetHeader (&head) == NoError) {
		o.Add ("type", ElemTypeName (head.type));
		o.Add ("storyIndex", (Int32) head.floorInd);
	}
	return o;
}

// =============================================================================
// Groups
// =============================================================================

// Top-level group of an ELEMENT (APINULLGuid when the element is not grouped). The element's
// header holds its direct group; ACAPI_ElementGroup_GetRootGroup climbs from that group to the
// root (it returns the passed group itself when that is already top-level).
API_Guid RootGroupOf (const API_Guid& elemGuid)
{
	API_Elem_Head head;
	BNZeroMemory (&head, sizeof (head));
	head.guid = elemGuid;
	if (ACAPI_Element_GetHeader (&head) != NoError || head.groupGuid == APINULLGuid)
		return APINULLGuid;
	API_Guid root = APINULLGuid;
	if (ACAPI_ElementGroup_GetRootGroup (head.groupGuid, &root) != NoError || root == APINULLGuid)
		return head.groupGuid;
	return root;
}


// True when guid identifies a group; members receives all (leaf) elements of the group tree.
bool GroupElementsOf (const API_Guid& guid, GS::Array<API_Guid>& members)
{
	members.Clear ();
	API_Elem_Head head;
	BNZeroMemory (&head, sizeof (head));
	head.guid = guid;
	if (ACAPI_Element_GetHeader (&head) == NoError && head.type.typeID != API_GroupID)
		return false;		// an ordinary element

	GS::Array<API_Guid> all;
	if (ACAPI_ElementGroup_GetAllGroupedElems (guid, &all) != NoError || all.IsEmpty ())
		return false;
	for (const API_Guid& g : all) {
		API_Elem_Head h;
		BNZeroMemory (&h, sizeof (h));
		h.guid = g;
		if (ACAPI_Element_GetHeader (&h) == NoError && h.type.typeID != API_GroupID)
			members.Push (g);
	}
	return !members.IsEmpty ();
}


bool IsSuspendGroupsOn ()
{
	bool on = false;
	ACAPI_Environment (APIEnv_IsSuspendGroupOnID, &on);
	return on;
}


GSErrCode ToggleSuspendGroups ()
{
	// APITool_SuspendGroups is a mode switch: the (empty) element list is not used.
	GSErrCode err = ACAPI_Element_Tool (GS::Array<API_Guid> (), APITool_SuspendGroups, nullptr);
	if (err == APIERR_NEEDSUNDOSCOPE) {
		err = ACAPI_CallUndoableCommand ("Suspend Groups (Claude)", [] () -> GSErrCode {
			return ACAPI_Element_Tool (GS::Array<API_Guid> (), APITool_SuspendGroups, nullptr);
		});
	}
	return err;
}


// Switches Suspend Groups ON for its lifetime (when needed) and restores the previous state.
// Must be created OUTSIDE of Undoable ().
class SuspendGroupsScope {
public:
	explicit SuspendGroupsScope (bool needed)
	{
		if (!needed || IsSuspendGroupsOn ())
			return;
		if (ToggleSuspendGroups () == NoError && IsSuspendGroupsOn ())
			toggled = true;
		else
			failed = true;
	}
	~SuspendGroupsScope ()
	{
		if (toggled)
			ToggleSuspendGroups ();
	}
	SuspendGroupsScope (const SuspendGroupsScope&) = delete;
	SuspendGroupsScope& operator= (const SuspendGroupsScope&) = delete;

	bool Failed () const { return failed; }

private:
	bool toggled = false;
	bool failed = false;
};

// =============================================================================
// Input resolution / validation
// =============================================================================

enum class Need { Exists, Editable, EditableOrLocked };

bool IsTeamworkProject ()
{
	API_ProjectInfo info;		// its destructor frees the returned locations/strings
	return ACAPI_Environment (APIEnv_ProjectID, &info) == NoError && info.teamwork;
}


// Returns the header when Archicad allows editing the element, otherwise throws an actionable error.
// NOTE: APIFilt_IsEditable is false both for elements on locked/hidden layers AND for locked
// elements (verified live), so the element lock is checked first and the layer flags explicitly.
API_Elem_Head RequireEditable (const API_Guid& guid, bool allowLocked, bool teamwork)
{
	API_Elem_Head head = GetHeader (guid);
	const GS::UniString what = ElemTypeName (head.type) + " " + GuidStr (guid);
	const bool elementLocked = head.lockId != 0;
	if (elementLocked && !allowLocked)
		Fail (what + " is locked: call unlock_elements first.", APIERR_NOTEDITABLE);

	// The layer flags are checked first and always: APIFilt_IsEditable can pass e.g. dimensions on a layer hidden by
	// the active layer combination, and Archicad then refuses the edit / delete silently (verified live).
	{
		API_Attribute layer;
		BNZeroMemory (&layer, sizeof (layer));
		layer.header.typeID = API_LayerID;
		layer.header.index = head.layer;
		if (ACAPI_Attribute_Get (&layer) == NoError) {
			const GS::UniString layerName = AttrName (API_LayerID, head.layer);
			if ((layer.header.flags & APILay_Locked) != 0)
				Fail (what + " is on the locked layer '" + layerName + "': unlock the layer first (set_layer_states), then retry.", APIERR_LOCKEDLAY);
			if ((layer.header.flags & APILay_Hidden) != 0)
				Fail (what + " is on the hidden layer '" + layerName + "': show the layer first (set_layer_states or apply_layer_combination), then retry.", APIERR_HIDDENLAY);
		}
	}
	if (!elementLocked && !ACAPI_Element_Filter (guid, APIFilt_IsEditable)) {
		const GS::UniString layerName = AttrName (API_LayerID, head.layer);
		Fail (what + " is not editable: its layer (or, for windows/doors/skylights, the host's layer) '" + layerName +
			  "' is locked or hidden. Unlock/show that layer first.", APIERR_NOTEDITABLE);
	}
	if (head.hotlinkGuid != APINULLGuid)
		Fail (what + " belongs to a hotlinked module and cannot be edited individually (move the hotlink module element, or edit its source file).", APIERR_NOTEDITABLE);
	if (teamwork && (!ACAPI_Element_Filter (guid, APIFilt_InMyWorkspace) || !ACAPI_Element_Filter (guid, APIFilt_HasAccessRight)))
		Fail (what + " is not in your Teamwork workspace: reserve it first.", APIERR_NOTMINE);
	return head;
}


struct ItemList {
	GS::Array<API_Guid>						order;		// resolved input order (deduplicated)
	GS::HashTable<API_Guid, OS>				errors;		// per-item validation failures ({code, message})
	GS::Array<API_Guid>						valid;		// elements that passed validation, in order
	GS::HashTable<API_Guid, API_Elem_Head>	heads;		// headers of the valid elements
	bool									anyGrouped = false;
};


ItemList ResolveItems (const GS::Array<API_Guid>& input, bool includeGroupMembers, Need need)
{
	ItemList list;
	GS::HashSet<API_Guid> seen;
	auto push = [&] (const API_Guid& g) {
		if (!seen.Contains (g)) {
			seen.Add (g);
			list.order.Push (g);
		}
	};

	for (const API_Guid& g : input) {
		GS::Array<API_Guid> members;
		if (GroupElementsOf (g, members)) {			// a group GUID stands for all its elements
			for (const API_Guid& m : members)
				push (m);
			continue;
		}
		push (g);
		if (includeGroupMembers) {
			const API_Guid root = RootGroupOf (g);
			if (root != APINULLGuid && GroupElementsOf (root, members)) {
				for (const API_Guid& m : members)
					push (m);
			}
		}
	}

	const bool teamwork = need != Need::Exists && IsTeamworkProject ();
	for (const API_Guid& g : list.order) {
		OS err;
		API_Elem_Head head;
		BNZeroMemory (&head, sizeof (head));
		if (!Attempt ([&] () { head = (need == Need::Exists) ? GetHeader (g) : RequireEditable (g, need == Need::EditableOrLocked, teamwork); }, err)) {
			list.errors.Put (g, err);
			continue;
		}
		list.heads.Put (g, head);
		list.valid.Push (g);
		if (head.groupGuid != APINULLGuid)
			list.anyGrouped = true;
	}
	return list;
}


GS::Array<short> GetStoryList (const OS& params, const char* key)
{
	class Collector : public OS::Processor {
	public:
		GS::Array<OS> items;
		void IntFound (const GS::String&, Int64 v) override		{ items.Push (OS ("s", (Int32) v)); }
		void UIntFound (const GS::String&, UInt64 v) override	{ items.Push (OS ("s", (Int32) v)); }
		void RealFound (const GS::String&, double v) override	{ items.Push (OS ("s", v)); }
		void StringFound (const GS::String&, const GS::UniString& v) override { items.Push (OS ("s", v)); }
		bool ObjectFound (const GS::String&, const OS& v) override { items.Push (OS ("s", v)); return false; }
	};
	if (!params.Contains (key) || !params.IsList (key))
		Fail ("Field '" + GS::UniString (key) + "' must be an array of story indices or names (see get_stories).");
	Collector collector;
	params.Enumerate (key, collector);
	GS::Array<short> stories;
	for (const OS& item : collector.items) {
		auto s = OptStory (item, "s");
		if (!s.has_value ())
			Fail ("Invalid story reference in '" + GS::UniString (key) + "'.");
		if (!stories.Contains (*s))
			stories.Push (*s);
	}
	if (stories.IsEmpty ())
		Fail ("Field '" + GS::UniString (key) + "' must contain at least one story.");
	return stories;
}


std::optional<API_Coord> BoundsCenter (const ItemList& items)
{
	bool any = false;
	API_Box3D total;
	BNZeroMemory (&total, sizeof (total));
	for (const API_Guid& g : items.valid) {
		const API_Elem_Head* head = items.heads.GetPtr (g);
		if (head == nullptr)
			continue;
		API_Box3D box;
		BNZeroMemory (&box, sizeof (box));
		if (ACAPI_Database (APIDb_CalcBoundsID, const_cast<API_Elem_Head*> (head), &box) != NoError)
			continue;
		if (!any) {
			total = box;
			any = true;
		} else {
			total.xMin = GS::Min (total.xMin, box.xMin);
			total.yMin = GS::Min (total.yMin, box.yMin);
			total.xMax = GS::Max (total.xMax, box.xMax);
			total.yMax = GS::Max (total.yMax, box.yMax);
		}
	}
	if (!any)
		return std::nullopt;
	API_Coord c;
	c.x = (total.xMin + total.xMax) / 2.0;
	c.y = (total.yMin + total.yMax) / 2.0;
	return c;
}

// =============================================================================
// ACAPI_Element_Edit wrapper
// =============================================================================

// Edits the given elements (never an empty list: that would edit the selection). Returns the
// resulting GUID per source (the new GUID for copies, APINULLGuid where Archicad refused).
GS::Array<API_Guid> RunEdit (const GS::Array<API_Guid>& sources, const API_EditPars& pars)
{
	GS::Array<API_Guid> out;
	if (sources.IsEmpty ())
		return out;
	GS::Array<API_Neig> neigs;
	for (const API_Guid& g : sources)
		neigs.Push (NeigOf (GetHeader (g)));
	CheckOp (ACAPI_Element_Edit (&neigs, pars), "ACAPI_Element_Edit failed");
	for (UIndex i = 0; i < sources.GetSize (); ++i)
		out.Push (i < neigs.GetSize () ? neigs[i].guid : APINULLGuid);
	return out;
}


bool IsNewCopy (const API_Guid& copy, const API_Guid& source)
{
	return copy != APINULLGuid && copy != source && ElementExists (copy);
}


API_EditPars DragPars (double dx, double dy, double dz, bool copy)
{
	API_EditPars pars;
	BNZeroMemory (&pars, sizeof (pars));
	pars.typeID = APIEdit_Drag;
	pars.withDelete = !copy;
	pars.endC.x = dx;
	pars.endC.y = dy;
	pars.endC.z = dz;
	return pars;
}


// Duplicates elements in place. Uses a zero-length drag-copy; when Archicad does not duplicate
// that way (nothing at all was created), the elements are copied 1 m aside and the copies moved
// back. Returns the copy per source (APINULLGuid = not copied).
GS::Array<API_Guid> CopyInPlace (const GS::Array<API_Guid>& sources)
{
	const GS::HashSet<API_Guid> before = AllElementGuids ();
	GS::Array<API_Guid> copies;
	OS zeroErr;
	if (!Attempt ([&] () { copies = RunEdit (sources, DragPars (0, 0, 0, true)); }, zeroErr))
		copies.Clear ();
	while (copies.GetSize () < sources.GetSize ())
		copies.Push (APINULLGuid);

	GS::Array<API_Guid> retry;
	GS::Array<UIndex> retryIdx;
	for (UIndex i = 0; i < sources.GetSize (); ++i) {
		if (!IsNewCopy (copies[i], sources[i])) {
			copies[i] = APINULLGuid;
			retry.Push (sources[i]);
			retryIdx.Push (i);
		}
	}
	if (retry.IsEmpty ())
		return copies;

	// Some copies succeeded: the failed ones are reported as refused (a second attempt could
	// leave stray duplicates next to un-attributed copies).
	if (retry.GetSize () < sources.GetSize ())
		return copies;
	// Nothing was attributed: remove whatever the zero-length copy may have created (only
	// elements created by this call), then take the detour.
	GS::Array<API_Guid> stray;
	for (const API_Guid& g : AllElementGuids ()) {
		if (!before.Contains (g))
			stray.Push (g);
	}
	if (!stray.IsEmpty ())
		ACAPI_Element_Delete (stray);

	GS::Array<API_Guid> aside = RunEdit (retry, DragPars (1.0, 0, 0, true));
	GS::Array<API_Guid> back;
	GS::Array<UIndex> backIdx;
	for (UIndex j = 0; j < retry.GetSize (); ++j) {
		if (IsNewCopy (aside[j], retry[j])) {
			back.Push (aside[j]);
			backIdx.Push (retryIdx[j]);
		} else {
			copies[retryIdx[j]] = APINULLGuid;
		}
	}
	if (!back.IsEmpty ()) {
		GS::Array<API_Guid> moved = RunEdit (back, DragPars (-1.0, 0, 0, false));
		for (UIndex k = 0; k < back.GetSize (); ++k)
			copies[backIdx[k]] = (moved[k] != APINULLGuid) ? moved[k] : back[k];
	}
	return copies;
}

// =============================================================================
// Transformations
// =============================================================================

enum class EditKind { Drag, Rotate, Mirror, Elevate, Resize };

struct TransformOp {
	EditKind					kind = EditKind::Drag;
	bool						copy = false;
	Int32						count = 1;
	bool						includeGroupMembers = false;
	API_Coord3D					vector {};
	std::optional<API_Coord>	center;
	double						angle = 0.0;		// radians
	API_Coord					axisBeg {};
	API_Coord					axisEnd {};
	std::optional<bool>			axisVertical;		// mirror axis given as "Vertical"/"Horizontal"
	double						deltaZ = 0.0;
	double						ratio = 1.0;
	ItemList					items;
	GS::Array<GS::UniString>	warnings;
};


const NamedValue kMirrorAxes[] = {
	{ "Vertical",	1 },
	{ "Horizontal",	0 },
};


// Parses (read-only) and validates one transformation. Throws on invalid parameters.
TransformOp ParseTransform (EditKind kind, const OS& op, bool forceCopy)
{
	TransformOp t;
	t.kind = kind;
	t.copy = forceCopy || GetBool (op, "copy", false);
	t.includeGroupMembers = GetBool (op, "includeGroupMembers", false);
	t.count = GetInt (op, "count", 1);
	if (t.count < 1 || t.count > kMaxCopyCount)
		Fail ("'count' must be between 1 and 1000.");
	if (t.count > 1 && (!t.copy || kind == EditKind::Mirror || kind == EditKind::Resize))
		Fail ("'count' > 1 (multiple copies) is only supported for copy_elements and for rotate/elevate with copy=true.");

	switch (kind) {
		case EditKind::Drag: {
			OS v = GetObject (op, "vector");
			t.vector = Coord3DFrom (v);
			if (std::fabs (t.vector.x) < kEps && std::fabs (t.vector.y) < kEps && std::fabs (t.vector.z) < kEps)
				Fail ("'vector' is zero: give a displacement in meters, e.g. {\"x\": 1.5, \"y\": 0}.");
			break;
		}
		case EditKind::Rotate:
			t.angle = GetAngle (op, "angle");
			if (std::fabs (t.angle) < kEps)
				Fail ("'angle' must be non-zero (degrees, positive = counter-clockwise).");
			t.center = OptCoord (op, "center");
			break;
		case EditKind::Mirror:
			if (op.Contains ("axisStart") || op.Contains ("axisEnd")) {
				t.axisBeg = GetCoord (op, "axisStart");
				t.axisEnd = GetCoord (op, "axisEnd");
				if (std::hypot (t.axisEnd.x - t.axisBeg.x, t.axisEnd.y - t.axisBeg.y) < 1e-6)
					Fail ("'axisStart' and 'axisEnd' must be different points.");
			} else if (op.Contains ("axis")) {
				t.axisVertical = ParseNamed (kMirrorAxes, op, "axis") == 1;
				t.center = OptCoord (op, "through");
			} else {
				Fail ("Mirror needs either 'axisStart' + 'axisEnd' (points of the mirror line) or 'axis': 'Vertical'|'Horizontal' (+ optional 'through' point).");
			}
			break;
		case EditKind::Elevate:
			t.deltaZ = GetDouble (op, "deltaZ");
			if (std::fabs (t.deltaZ) < kEps)
				Fail ("'deltaZ' must be non-zero (meters, positive = up).");
			break;
		case EditKind::Resize:
			t.ratio = GetDouble (op, "ratio");
			if (t.ratio <= kEps)
				Fail ("'ratio' must be a positive scale factor (e.g. 2 = double size, 0.5 = half).");
			if (std::fabs (t.ratio - 1.0) < kEps)
				Fail ("'ratio' = 1 does not change anything.");
			t.center = OptCoord (op, "center");
			break;
	}

	t.items = ResolveItems (GetGuidArray (op, "elements"), t.includeGroupMembers, Need::Editable);
	if (t.items.order.IsEmpty ())
		Fail ("'elements' is empty.");

	if (kind == EditKind::Elevate) {		// Archicad silently skips 2D elements: report them instead
		for (const API_Guid& g : GS::Array<API_Guid> (t.items.valid)) {
			const API_Elem_Head* head = t.items.heads.GetPtr (g);
			if (head != nullptr && Is2DOnlyType (head->type.typeID)) {
				t.items.errors.Put (g, ErrorOf (APIERR_BADELEMENTTYPE, ElemTypeName (head->type) +
					" is a 2D element without elevation: elevate_elements only moves model (3D) elements vertically."));
				t.items.valid.DeleteFirst (g);
			}
		}
	}

	// Default pivot: centre of the elements' combined bounding box.
	const bool needsCenter = (kind == EditKind::Rotate || kind == EditKind::Resize || (kind == EditKind::Mirror && t.axisVertical.has_value ()));
	if (needsCenter && !t.center.has_value () && !t.items.valid.IsEmpty ()) {
		t.center = BoundsCenter (t.items);
		if (!t.center.has_value ())
			Fail ("Cannot compute the bounding box of the elements: pass the pivot point explicitly ('center' / 'through').");
		t.warnings.Push (GS::UniString::Printf ("Pivot point defaulted to the bounding-box centre of the elements: (%.4f, %.4f).", t.center->x, t.center->y));
	}
	if (kind == EditKind::Mirror && t.axisVertical.has_value () && t.center.has_value ()) {
		t.axisBeg = *t.center;
		t.axisEnd = *t.center;
		if (*t.axisVertical)
			t.axisEnd.y += 1.0;
		else
			t.axisEnd.x += 1.0;
	}
	return t;
}


API_EditPars MakeTransformPars (const TransformOp& t, Int32 step)
{
	API_EditPars pars;
	BNZeroMemory (&pars, sizeof (pars));
	pars.withDelete = !t.copy;
	const API_Coord c = t.center.value_or (API_Coord { 0.0, 0.0 });
	switch (t.kind) {
		case EditKind::Drag:
			pars.typeID = APIEdit_Drag;
			pars.endC.x = t.vector.x * step;
			pars.endC.y = t.vector.y * step;
			pars.endC.z = t.vector.z * step;
			break;
		case EditKind::Rotate: {
			const double a = t.angle * step;
			pars.typeID = APIEdit_Rotate;
			pars.origC = c;
			pars.begC.x = c.x + 1.0;
			pars.begC.y = c.y;
			pars.endC.x = c.x + std::cos (a);
			pars.endC.y = c.y + std::sin (a);
			break;
		}
		case EditKind::Mirror:
			pars.typeID = APIEdit_Mirror;
			pars.begC.x = t.axisBeg.x;
			pars.begC.y = t.axisBeg.y;
			pars.endC.x = t.axisEnd.x;
			pars.endC.y = t.axisEnd.y;
			break;
		case EditKind::Elevate:
			pars.typeID = APIEdit_Elevate;
			pars.endC.z = t.deltaZ * step;
			break;
		case EditKind::Resize:
			pars.typeID = APIEdit_Resize;
			pars.begC.x = c.x;
			pars.begC.y = c.y;
			pars.endC.x = c.x + 1.0;
			pars.endC.y = c.y;
			pars.endC2.x = c.x + t.ratio;
			pars.endC2.y = c.y;
			break;
	}
	return pars;
}


const char* const kRefusedMessage =
	"Archicad refused to edit this element (typical causes: hosted element such as a window/door that cannot leave its host, "
	"sub-element of a curtain wall/stair/railing, locked, or outside your Teamwork workspace).";


// Executes one parsed transformation. Must run inside an undo scope.
OS ExecuteTransform (TransformOp& t)
{
	const GS::Array<API_Guid>& src = t.items.valid;
	GS::HashTable<API_Guid, OS> itemErrors = t.items.errors;
	GS::Array<OS> results;

	if (!t.copy) {
		GS::Array<API_Guid> res;
		OS err;
		if (!src.IsEmpty () && !Attempt ([&] () { res = RunEdit (src, MakeTransformPars (t, 1)); }, err)) {
			for (const API_Guid& g : src)
				itemErrors.Put (g, err);
		}
		GS::HashTable<API_Guid, API_Guid> resultOf;
		for (UIndex i = 0; i < src.GetSize () && i < res.GetSize (); ++i)
			resultOf.Put (src[i], res[i]);
		for (const API_Guid& g : t.items.order) {
			if (const OS* e = itemErrors.GetPtr (g)) {
				results.Push (ItemError (g, *e));
				continue;
			}
			const API_Guid* rp = resultOf.GetPtr (g);
			const API_Guid r = rp != nullptr ? *rp : APINULLGuid;
			if (r == APINULLGuid) {
				results.Push (ItemError (g, ErrorOf (APIERR_REFUSEDCMD, kRefusedMessage)));
				continue;
			}
			OS o ("guid", GuidStr (g));
			if (r != g) {
				o.Add ("newGuid", GuidStr (r));
			} else {
				const API_Elem_Head* before = t.items.heads.GetPtr (g);
				API_Elem_Head after;
				BNZeroMemory (&after, sizeof (after));
				after.guid = g;
				if (before != nullptr && ACAPI_Element_GetHeader (&after) == NoError && after.modiStamp == before->modiStamp)
					o.Add ("warning", GS::UniString ("The element's modification stamp did not change: Archicad may have ignored it (check with get_element_details)."));
			}
			results.Push (o);
		}
		OS out ("results", results);
		if (!t.warnings.IsEmpty ())
			out.Add ("warnings", t.warnings);
		return out;
	}

	// Copies (count >= 1)
	const GS::HashSet<API_Guid> before = AllElementGuids ();
	GS::HashTable<API_Guid, GS::Array<API_Guid>> copies;
	GS::HashSet<API_Guid> primary;
	for (const API_Guid& g : src)
		copies.Put (g, GS::Array<API_Guid> ());

	for (Int32 step = 1; step <= t.count && !src.IsEmpty (); ++step) {
		GS::Array<API_Guid> res;
		OS err;
		if (!Attempt ([&] () { res = RunEdit (src, MakeTransformPars (t, step)); }, err)) {
			for (const API_Guid& g : src) {
				if (copies[g].IsEmpty () && !itemErrors.ContainsKey (g))
					itemErrors.Put (g, err);
			}
			if (step > 1)
				t.warnings.Push ("Stopped after " + GS::ValueToUniString (step - 1) + " copies: " + GetString (err, "message", ""));
			break;
		}
		for (UIndex i = 0; i < src.GetSize (); ++i) {
			if (i < res.GetSize () && IsNewCopy (res[i], src[i])) {
				copies[src[i]].Push (res[i]);
				primary.Add (res[i]);
			}
		}
	}

	const GS::HashSet<API_Guid> after = AllElementGuids ();
	GS::Array<OS> additional;
	Int32 createdCount = 0;
	Int32 subElementCount = 0;
	Int32 unlisted = 0;
	for (const API_Guid& g : after) {
		if (before.Contains (g))
			continue;
		++createdCount;
		if (primary.Contains (g))
			continue;
		if (IsSubElement (g)) {
			++subElementCount;
		} else if (additional.GetSize () < (UIndex) kMaxListedCreated) {
			additional.Push (GuidTypeObj (g));
		} else {
			++unlisted;
		}
	}

	for (const API_Guid& g : t.items.order) {
		if (const OS* e = itemErrors.GetPtr (g)) {
			results.Push (ItemError (g, *e));
			continue;
		}
		const GS::Array<API_Guid>* list = copies.GetPtr (g);
		if (list == nullptr || list->IsEmpty ()) {
			results.Push (ItemError (g, ErrorOf (APIERR_REFUSEDCMD, kRefusedMessage)));
			continue;
		}
		OS o ("guid", GuidStr (g), "copies", GuidStrings (*list));
		if ((Int32) list->GetSize () < t.count)
			o.Add ("warning", GS::UniString::Printf ("Only %d of %d copies were created.", (int) list->GetSize (), (int) t.count));
		results.Push (o);
	}

	// Copying whole groups creates new groups holding the copies (groups are not listed as elements).
	GS::HashSet<API_Guid> sourceRoots;
	for (const API_Guid& g : src) {
		const API_Guid root = RootGroupOf (g);
		if (root != APINULLGuid)
			sourceRoots.Add (root);
	}
	GS::Array<API_Guid> createdGroups;
	for (const API_Guid& c : primary) {
		const API_Guid root = RootGroupOf (c);
		if (root != APINULLGuid && !sourceRoots.Contains (root) && !createdGroups.Contains (root))
			createdGroups.Push (root);
	}

	OS out ("results", results);
	out.Add ("createdCount", createdCount);
	if (!createdGroups.IsEmpty ())
		out.Add ("createdGroups", GuidStrings (createdGroups));
	if (!additional.IsEmpty ())
		out.Add ("additionalCreated", additional);		// e.g. openings of copied walls, copied group members, associative labels
	if (subElementCount > 0)
		out.Add ("subElementsCreated", subElementCount);	// curtain wall / stair / railing parts of the copies
	if (unlisted > 0)
		t.warnings.Push (GS::UniString::Printf ("additionalCreated was truncated: %d more elements not listed.", (int) unlisted));
	if (!t.warnings.IsEmpty ())
		out.Add ("warnings", t.warnings);
	return out;
}


void RegisterTransformCommand (const char* name, const char* description, EditKind kind, bool forceCopy, const char* undoName)
{
	RegisterCommand (name, description, [kind, forceCopy, undoName] (const OS& params) -> OS {
		const bool batch = params.Contains ("operations");
		GS::Array<OS> opSpecs = batch ? GetObjectArray (params, "operations") : GS::Array<OS> { params };
		if (opSpecs.IsEmpty ())
			Fail ("'operations' is empty.");

		// 1. Parse + validate everything before touching the model.
		GS::Array<TransformOp> ops;
		GS::Array<OS> opResults;
		GS::Array<bool> parsed;
		for (const OS& spec : opSpecs) {
			if (!batch) {
				ops.Push (ParseTransform (kind, spec, forceCopy));	// single operation: errors are fatal
				parsed.Push (true);
				opResults.Push (OS ());
				continue;
			}
			OS err;
			TransformOp t;
			if (Attempt ([&] () { t = ParseTransform (kind, spec, forceCopy); }, err)) {
				ops.Push (t);
				parsed.Push (true);
				opResults.Push (OS ());
			} else {
				ops.Push (TransformOp ());
				parsed.Push (false);
				opResults.Push (OS ("error", err));
			}
		}

		bool needSuspend = false;
		for (UIndex i = 0; i < ops.GetSize (); ++i) {
			if (parsed[i] && !ops[i].includeGroupMembers && ops[i].items.anyGrouped)
				needSuspend = true;
		}

		// 2. Execute in ONE undo step.
		{
			SuspendGroupsScope suspend (needSuspend);
			Undoable (undoName, [&] () {
				for (UIndex i = 0; i < ops.GetSize (); ++i) {
					if (!parsed[i])
						continue;
					if (suspend.Failed ())
						ops[i].warnings.Push ("Suspend Groups could not be switched on: whole groups may have been edited.");
					OS r = Try ([&] () { return ExecuteTransform (ops[i]); });
					opResults[i] = r;
				}
			});
		}

		if (!batch) {
			if (opResults[0].Contains ("error")) {
				OS e;
				opResults[0].Get ("error", e);
				Fail (GetString (e, "message", "Operation failed."), (GSErrCode) GetInt (e, "code", APIERR_GENERAL));
			}
			return opResults[0];
		}
		return OS ("operations", opResults);
	});
}

// =============================================================================
// Delete / copy to stories
// =============================================================================

OS DeleteElementsCommand (const OS& params)
{
	const bool includeGroupMembers = GetBool (params, "includeGroupMembers", false);
	ItemList items = ResolveItems (GetGuidArray (params, "elements"), includeGroupMembers, Need::Editable);
	if (items.order.IsEmpty ())
		Fail ("'elements' is empty.");

	GS::HashTable<API_Guid, GSErrCode> deleteErrors;
	GS::HashTable<API_Guid, Int32> typesBefore;
	GS::HashTable<API_Guid, Int32> typesAfter;
	GS::Array<GS::UniString> warnings;

	if (!items.valid.IsEmpty ()) {
		typesBefore = AllElementTypes ();
		SuspendGroupsScope suspend (!includeGroupMembers && items.anyGrouped);
		if (suspend.Failed ())
			warnings.Push ("Suspend Groups could not be switched on: whole groups may have been deleted.");
		Undoable ("Delete elements (Claude)", [&] () {
			const GSErrCode err = ACAPI_Element_Delete (items.valid);
			if (err != NoError) {
				// Archicad aborts the whole batch when one element cannot be deleted: retry one by one.
				for (const API_Guid& g : items.valid) {
					if (!ElementExists (g))
						continue;
					const GSErrCode e = ACAPI_Element_Delete (GS::Array<API_Guid> { g });
					if (e != NoError)
						deleteErrors.Put (g, e);
				}
			}
		});
		typesAfter = AllElementTypes ();
	}

	GS::HashSet<API_Guid> inputSet;
	GS::Array<OS> results;
	Int32 deleted = 0;
	for (const API_Guid& g : items.order) {
		inputSet.Add (g);
		if (const OS* e = items.errors.GetPtr (g)) {
			results.Push (ItemError (g, *e));
			continue;
		}
		if (ElementExists (g)) {
			const GSErrCode* e = deleteErrors.GetPtr (g);
			results.Push (ItemError (g, ErrorOf (e != nullptr ? *e : APIERR_REFUSEDCMD,
				"Not deleted" + (e != nullptr ? ": " + ErrorName (*e) : GS::UniString (" (Archicad refused silently)")) +
				". Sub-elements (curtain wall panels, stair treads, railing parts) and elements outside your Teamwork workspace cannot be deleted individually.")));
			continue;
		}
		results.Push (OS ("guid", GuidStr (g), "deleted", true));
		++deleted;
	}

	GS::Array<OS> alsoDeleted;
	Int32 subElementsDeleted = 0;
	for (auto it = typesBefore.EnumeratePairs (); it != nullptr; ++it) {
		const API_Guid& g = *it->key;
		if (inputSet.Contains (g) || typesAfter.ContainsKey (g))
			continue;
		const API_ElemTypeID typeID = (API_ElemTypeID) *it->value;
		if (IsSubElementType (typeID))
			++subElementsDeleted;
		else if (alsoDeleted.GetSize () < (UIndex) kMaxListedCreated)
			alsoDeleted.Push (OS ("guid", GuidStr (g), "type", ElemTypeName (typeID)));
	}

	OS out ("results", results, "deletedCount", deleted);
	if (!alsoDeleted.IsEmpty ())
		out.Add ("alsoDeleted", alsoDeleted);		// dependent elements removed with them (openings, associative dimensions/labels, group members...)
	if (subElementsDeleted > 0)
		out.Add ("subElementsDeleted", subElementsDeleted);
	if (!warnings.IsEmpty ())
		out.Add ("warnings", warnings);
	return out;
}


void ChangeStory (const API_Guid& guid, short floorInd)
{
	API_Element element = GetElement (guid);
	if (element.header.floorInd == floorInd)
		return;
	API_Element mask;
	ACAPI_ELEMENT_MASK_CLEAR (mask);
	element.header.floorInd = floorInd;
	ACAPI_ELEMENT_MASK_SET (mask, API_Elem_Head, floorInd);
	CheckOp (ACAPI_Element_Change (&element, &mask, nullptr, 0, true), "Cannot move the copy to story " + GS::ValueToUniString ((Int32) floorInd));
}


// Copies Element ID, classifications and user-defined property values (best effort).
void CopyIdentity (const API_Guid& src, const API_Guid& dst)
{
	const GS::UniString id = GetElementInfoString (src);
	if (!id.IsEmpty ()) {
		GS::UniString info = id;
		ACAPI_Database (APIDb_ChangeElementInfoStringID, const_cast<API_Guid*> (&dst), &info);
	}

	GS::Array<GS::Pair<API_Guid, API_Guid>> classes;
	if (ACAPI_Element_GetClassificationItems (src, classes) == NoError) {
		for (const auto& pair : classes)
			ACAPI_Element_AddClassificationItem (dst, pair.second);
	}

	GS::Array<API_PropertyDefinition> defs;
	if (ACAPI_Element_GetPropertyDefinitions (src, API_PropertyDefinitionFilter_UserDefined, defs) == NoError && !defs.IsEmpty ()) {
		GS::Array<API_Property> props;
		if (ACAPI_Element_GetPropertyValues (src, defs, props) == NoError) {
			GS::Array<API_Property> toSet;
			for (const API_Property& p : props) {
				if (!p.isDefault && p.status == API_Property_HasValue && ACAPI_Element_IsPropertyDefinitionValueEditable (dst, p.definition.guid))
					toSet.Push (p);
			}
			if (!toSet.IsEmpty ())
				ACAPI_Element_SetProperties (dst, toSet);
		}
	}
}


// Creates a duplicate of src on another story from its data (Get + memo + Create), the way the
// DevKit copies elements between stories. Used where ACAPI_Element_Change ignores the story:
// 2D elements and openings hosted by a wall/roof/slab copy that was moved to the story.
API_Guid RecreateOnStory (const API_Guid& src, short story)
{
	API_Element element = GetElement (src);
	Memo memo;
	const bool hasMemo = ACAPI_Element_GetMemo (src, memo.Ptr (), APIMemoMask_All) == NoError;
	element.header.floorInd = story;
	element.header.guid = APINULLGuid;
	element.header.groupGuid = APINULLGuid;
	CheckOp (ACAPI_Element_Create (&element, hasMemo ? memo.Ptr () : nullptr),
			 "Cannot create the " + ElemTypeName (element.header.type) + " on story " + GS::ValueToUniString ((Int32) story));
	const API_Guid created = element.header.guid;
	if (created == APINULLGuid || !ElementExists (created))
		Fail ("Archicad did not create the " + ElemTypeName (element.header.type) + " on story " + GS::ValueToUniString ((Int32) story) + ".", APIERR_GENERAL);
	CopyIdentity (src, created);
	return created;
}


// Moves a fresh copy to another story. Returns the GUID that now represents it: the copy itself,
// or - where Archicad keeps the story unchanged (2D elements, hosted openings) - a re-created
// element on the target story (the copy is then deleted). Throws when neither works.
API_Guid MoveCopyToStory (const API_Guid& copy, short story)
{
	OS changeErr;
	const bool changed = Attempt ([&] () { ChangeStory (copy, story); }, changeErr);
	if (changed && GetHeader (copy).floorInd == story)
		return copy;

	API_Guid created = APINULLGuid;
	OS createErr;
	if (!Attempt ([&] () { created = RecreateOnStory (copy, story); }, createErr)) {
		const GS::UniString why = changed ? GS::UniString ("Archicad kept the element on its home story")
										  : GetString (changeErr, "message", "Cannot change the story");
		Fail (why + "; re-creating it on story " + GS::ValueToUniString ((Int32) story) + " failed too: " +
			  GetString (createErr, "message", ""), (GSErrCode) GetInt (createErr, "code", APIERR_GENERAL));
	}
	ACAPI_Element_Delete (GS::Array<API_Guid> { copy });
	return created;
}


OS CopyElementsToStoriesCommand (const OS& params)
{
	const bool includeGroupMembers = GetBool (params, "includeGroupMembers", false);
	const GS::Array<short> stories = GetStoryList (params, "stories");
	ItemList items = ResolveItems (GetGuidArray (params, "elements"), includeGroupMembers, Need::Editable);
	if (items.order.IsEmpty ())
		Fail ("'elements' is empty.");

	// Hosted and sub-elements travel with their host/parent.
	for (const API_Guid& g : GS::Array<API_Guid> (items.valid)) {
		const API_Elem_Head* head = items.heads.GetPtr (g);
		if (head == nullptr)
			continue;
		const API_ElemTypeID t = head->type.typeID;
		if (IsHostedType (t) || IsSubElementType (t)) {
			items.errors.Put (g, ErrorOf (APIERR_BADELEMENTTYPE, ElemTypeName (head->type) +
				" cannot be copied to another story on its own: copy its host/parent element (wall, roof, slab, curtain wall, stair, railing) instead."));
			items.valid.DeleteFirst (g);
		}
	}

	GS::HashTable<API_Guid, GS::Array<OS>> perSource;
	for (const API_Guid& g : items.valid)
		perSource.Put (g, GS::Array<OS> ());
	GS::Array<OS> additional;
	GS::Array<GS::UniString> warnings;
	Int32 subElementsCreated = 0;

	if (!items.valid.IsEmpty ()) {
		SuspendGroupsScope suspend (!includeGroupMembers && items.anyGrouped);
		if (suspend.Failed ())
			warnings.Push ("Suspend Groups could not be switched on: whole groups may have been copied.");
		Undoable ("Copy elements to stories (Claude)", [&] () {
			for (short story : stories) {
				const GS::UniString storyLabel = "story " + GS::ValueToUniString ((Int32) story);
				GS::Array<API_Guid> sources;
				for (const API_Guid& g : items.valid) {
					const API_Elem_Head* head = items.heads.GetPtr (g);
					if (head != nullptr && head->floorInd == story)
						perSource[g].Push (OS ("storyIndex", (Int32) story, "error", ErrorOf (APIERR_BADPARS, "The element is already on " + storyLabel + ": use copy_elements to duplicate it on its own story.")));
					else
						sources.Push (g);
				}
				if (sources.IsEmpty ())
					continue;

				const GS::HashSet<API_Guid> before = AllElementGuids ();
				GS::Array<API_Guid> copies;
				OS err;
				if (!Attempt ([&] () { copies = CopyInPlace (sources); }, err)) {
					for (const API_Guid& g : sources)
						perSource[g].Push (OS ("storyIndex", (Int32) story, "error", err));
					continue;
				}
				GS::HashSet<API_Guid> primary;
				for (UIndex i = 0; i < sources.GetSize (); ++i) {
					const API_Guid copy = i < copies.GetSize () ? copies[i] : APINULLGuid;
					if (!IsNewCopy (copy, sources[i])) {
						perSource[sources[i]].Push (OS ("storyIndex", (Int32) story, "error", ErrorOf (APIERR_REFUSEDCMD, kRefusedMessage)));
						continue;
					}
					OS moveErr;
					API_Guid moved = APINULLGuid;
					if (!Attempt ([&] () { moved = MoveCopyToStory (copy, story); }, moveErr)) {
						if (ElementExists (copy))
							ACAPI_Element_Delete (GS::Array<API_Guid> { copy });		// do not leave a stray duplicate behind
						perSource[sources[i]].Push (OS ("storyIndex", (Int32) story, "error", moveErr));
						continue;
					}
					primary.Add (moved);
					perSource[sources[i]].Push (OS ("storyIndex", (Int32) story, "guid", GuidStr (moved)));
				}

				// Dependent copies (openings, group members, labels...) must end up on the same story.
				const GS::HashSet<API_Guid> after = AllElementGuids ();
				for (const API_Guid& g : after) {
					if (before.Contains (g) || primary.Contains (g))
						continue;
					API_Elem_Head head;
					BNZeroMemory (&head, sizeof (head));
					head.guid = g;
					if (ACAPI_Element_GetHeader (&head) != NoError)
						continue;
					if (IsSubElementType (head.type.typeID)) {
						++subElementsCreated;		// parts follow their parent element
						continue;
					}
					API_Guid placed = g;
					if (head.floorInd != story) {
						OS e;
						if (!Attempt ([&] () { placed = MoveCopyToStory (g, story); }, e)) {
							// A dependent that cannot follow (e.g. a label of an element on another story) is removed
							// rather than left behind as a stray duplicate on the original story.
							if (ElementExists (g))
								ACAPI_Element_Delete (GS::Array<API_Guid> { g });
							warnings.Push ("Dependent copy of type " + ElemTypeName (head.type) + " could not be placed on story " +
										   GS::ValueToUniString ((Int32) story) + " and was removed: " + GetString (e, "message", ""));
							continue;
						}
					}
					if (additional.GetSize () < (UIndex) kMaxListedCreated)
						additional.Push (GuidTypeObj (placed));
				}
			}
		});
	}

	GS::Array<OS> results;
	for (const API_Guid& g : items.order) {
		if (const OS* e = items.errors.GetPtr (g)) {
			results.Push (ItemError (g, *e));
			continue;
		}
		const GS::Array<OS>* list = perSource.GetPtr (g);
		results.Push (OS ("guid", GuidStr (g), "copies", list != nullptr ? *list : GS::Array<OS> ()));
	}
	OS out ("results", results);
	if (!additional.IsEmpty ())
		out.Add ("additionalCreated", additional);
	if (subElementsCreated > 0)
		out.Add ("subElementsCreated", subElementsCreated);
	if (!warnings.IsEmpty ())
		out.Add ("warnings", warnings);
	return out;
}

// =============================================================================
// Groups
// =============================================================================

OS GroupOne (const OS& spec)
{
	GS::Array<API_Guid> input = GetGuidArray (spec, "elements");
	const std::optional<API_Guid> parent = OptGuid (spec, "parentGroup");

	GS::Array<API_Guid> items;		// elements or root groups to put into the new group
	GS::Array<API_Guid> leaves;		// all elements that end up in the new group
	GS::HashSet<API_Guid> seen;
	std::optional<short> floor;
	auto noteFloor = [&] (const API_Guid& elem) {
		const API_Elem_Head head = GetHeader (elem);
		if (!floor.has_value ())
			floor = head.floorInd;
		else if (*floor != head.floorInd)
			Fail ("Grouped elements must be on the same story (found stories " + GS::ValueToUniString ((Int32) *floor) + " and " +
				  GS::ValueToUniString ((Int32) head.floorInd) + "). Use copy_elements_to_stories / modify_elements storyIndex first.");
		leaves.Push (elem);
	};
	for (const API_Guid& g : input) {
		GS::Array<API_Guid> members;
		API_Guid item = g;
		if (GroupElementsOf (g, members)) {
			for (const API_Guid& m : members)
				noteFloor (m);
		} else {
			noteFloor (g);						// throws when the element does not exist
			const API_Guid root = RootGroupOf (g);
			if (root != APINULLGuid) {			// already grouped: nest its top-level group (like the Group command in Archicad)
				item = root;
				GS::Array<API_Guid> rootMembers;
				if (GroupElementsOf (root, rootMembers)) {
					for (const API_Guid& m : rootMembers)
						if (!leaves.Contains (m))
							leaves.Push (m);
				}
			}
		}
		if (!seen.Contains (item)) {
			seen.Add (item);
			items.Push (item);
		}
	}
	if (items.GetSize () < 2) {
		if (items.GetSize () == 1 && items[0] != input[0])
			Fail ("These elements already form the group " + GuidStr (items[0]) + ". Add other elements/groups to create a bigger group.");
		Fail ("A group needs at least two elements or groups.");
	}

	API_Guid groupGuid = APINULLGuid;
	GSErrCode err = ACAPI_ElementGroup_Create (items, &groupGuid, parent.has_value () ? &*parent : nullptr);
	if (err != NoError || groupGuid == APINULLGuid) {
		if (parent.has_value ())
			CheckOp (err != NoError ? err : APIERR_GENERAL, "Cannot create the group under parent group " + GuidStr (*parent));
		// Fallback: the Group command of the Edit menu on all underlying elements.
		const API_Guid rootBefore = RootGroupOf (leaves[0]);
		const GSErrCode toolErr = ACAPI_Element_Tool (leaves, APITool_Group, nullptr);
		if (toolErr != NoError)
			CheckOp (err != NoError ? err : toolErr, "Cannot create the group");
		groupGuid = RootGroupOf (leaves[0]);
		if (groupGuid == APINULLGuid || groupGuid == rootBefore)
			CheckOp (err != NoError ? err : APIERR_GENERAL, "Cannot create the group");
	}
	OS out ("groupGuid", GuidStr (groupGuid), "members", GuidStrings (items));
	out.Add ("elementCount", (Int32) leaves.GetSize ());
	return out;
}


OS UngroupOne (const API_Guid& g, bool completely)
{
	GS::Array<API_Guid> members;
	API_Guid group = APINULLGuid;
	if (GroupElementsOf (g, members)) {
		group = g;
	} else {
		GetHeader (g);		// throws when missing
		group = RootGroupOf (g);
		if (group == APINULLGuid)
			Fail ("Element " + GuidStr (g) + " is not grouped.", APIERR_BADPARS);
		if (!GroupElementsOf (group, members))
			Fail ("Cannot read the members of group " + GuidStr (group) + ".", APIERR_GENERAL);
	}
	GS::Array<API_Guid> dissolved;
	for (Int32 iter = 0; iter < 32; ++iter) {
		const API_Guid rootNow = RootGroupOf (members[0]);
		if (rootNow == APINULLGuid)
			break;
		CheckOp (ACAPI_Element_Tool (members, APITool_Ungroup, nullptr), "Cannot ungroup");
		const API_Guid rootAfter = RootGroupOf (members[0]);
		if (rootAfter == rootNow)
			Fail ("Archicad did not ungroup group " + GuidStr (rootNow) + " (locked elements or Teamwork reservation?).", APIERR_REFUSEDCMD);
		dissolved.Push (rootNow);
		if (!completely)
			break;
	}
	OS out ("guid", GuidStr (g), "dissolvedGroups", GuidStrings (dissolved), "elementCount", (Int32) members.GetSize ());
	const API_Guid remaining = RootGroupOf (members[0]);
	if (remaining != APINULLGuid)
		out.Add ("remainingGroup", GuidStr (remaining));
	return out;
}

// =============================================================================
// Lock / draw order
// =============================================================================

OS LockCommand (const OS& params, bool lock)
{
	const bool includeGroupMembers = GetBool (params, "includeGroupMembers", false);
	ItemList items = ResolveItems (GetGuidArray (params, "elements"), includeGroupMembers, Need::EditableOrLocked);	// already-locked elements simply report locked
	if (items.order.IsEmpty ())
		Fail ("'elements' is empty.");

	GS::Array<GS::UniString> warnings;
	OS toolErr;
	bool toolFailed = false;
	if (!items.valid.IsEmpty ()) {
		SuspendGroupsScope suspend (!includeGroupMembers && items.anyGrouped);
		if (suspend.Failed ())
			warnings.Push ("Suspend Groups could not be switched on: whole groups may have been affected.");
		Undoable (lock ? "Lock elements (Claude)" : "Unlock elements (Claude)", [&] () {
			toolFailed = !Attempt ([&] () {
				CheckOp (ACAPI_Element_Tool (items.valid, lock ? APITool_Lock : APITool_Unlock, nullptr), lock ? "Cannot lock" : "Cannot unlock");
			}, toolErr);
		});
	}

	GS::Array<OS> results;
	for (const API_Guid& g : items.order) {
		if (const OS* e = items.errors.GetPtr (g)) {
			results.Push (ItemError (g, *e));
			continue;
		}
		API_Elem_Head head;
		BNZeroMemory (&head, sizeof (head));
		head.guid = g;
		const bool ok = ACAPI_Element_GetHeader (&head) == NoError;
		const bool locked = ok && head.lockId != 0;
		if (ok && locked == lock) {
			results.Push (OS ("guid", GuidStr (g), "locked", locked));
		} else if (toolFailed) {
			results.Push (ItemError (g, toolErr));
		} else {
			results.Push (ItemError (g, ErrorOf (APIERR_REFUSEDCMD, lock ? "Archicad did not lock the element." : "Archicad did not unlock the element (Teamwork: it may be locked by another user).")));
		}
	}
	OS out ("results", results);
	if (!warnings.IsEmpty ())
		out.Add ("warnings", warnings);
	return out;
}


const NamedValue kOrderActions[] = {
	{ "BringToFront",	(Int32) APITool_BringToFront },
	{ "BringForward",	(Int32) APITool_BringForward },
	{ "SendBackward",	(Int32) APITool_SendBackward },
	{ "SendToBack",		(Int32) APITool_SendToBack },
	{ "Reset",			(Int32) APITool_ResetOrder },
};


Int32 DrawIndexOf (const API_Guid& g)
{
	return (Int32) GetHeader (g).drwIndex;
}


OS SetDrawOrderCommand (const OS& params)
{
	const bool includeGroupMembers = GetBool (params, "includeGroupMembers", false);
	const std::optional<Int32> action = OptNamed (kOrderActions, params, "action");
	const std::optional<Int32> level = OptInt (params, "level");
	const Int32 steps = GetInt (params, "steps", 1);
	if (action.has_value () == level.has_value ())
		Fail ("Give exactly one of 'action' (BringToFront|BringForward|SendBackward|SendToBack|Reset) or 'level' (target drawing order 1..14).");
	if (level.has_value () && (*level < 1 || *level > 14))
		Fail ("'level' must be between 1 (bottom) and 14 (top).");
	if (steps < 1 || steps > 14)
		Fail ("'steps' must be between 1 and 14.");

	ItemList items = ResolveItems (GetGuidArray (params, "elements"), includeGroupMembers, Need::Editable);
	if (items.order.IsEmpty ())
		Fail ("'elements' is empty.");

	GS::HashTable<API_Guid, Int32> before;
	for (const API_Guid& g : items.valid)
		before.Put (g, (Int32) items.heads[g].drwIndex);

	GS::Array<GS::UniString> warnings;
	GS::HashTable<API_Guid, OS> toolErrors;
	if (!items.valid.IsEmpty ()) {
		SuspendGroupsScope suspend (!includeGroupMembers && items.anyGrouped);
		if (suspend.Failed ())
			warnings.Push ("Suspend Groups could not be switched on: whole groups may have been affected.");
		Undoable ("Set drawing order (Claude)", [&] () {
			if (action.has_value ()) {
				const Int32 repeat = (*action == (Int32) APITool_BringForward || *action == (Int32) APITool_SendBackward) ? steps : 1;
				OS err;
				for (Int32 i = 0; i < repeat; ++i) {
					if (!Attempt ([&] () { CheckOp (ACAPI_Element_Tool (items.valid, (API_ToolCmdID) *action, nullptr), "Cannot change the drawing order"); }, err)) {
						for (const API_Guid& g : items.valid)
							toolErrors.Put (g, err);
						break;
					}
				}
				return;
			}
			// Target level: step each element individually (Bring Forward / Send Backward are the reliable tools).
			for (const API_Guid& g : items.valid) {
				OS err;
				const bool ok = Attempt ([&] () {
					Int32 cur = DrawIndexOf (g);
					for (Int32 iter = 0; iter < 20 && cur != *level; ++iter) {
						const API_ToolCmdID step = cur < *level ? APITool_BringForward : APITool_SendBackward;
						CheckOp (ACAPI_Element_Tool (GS::Array<API_Guid> { g }, step, nullptr), "Cannot change the drawing order");
						const Int32 next = DrawIndexOf (g);
						if (next == cur)
							break;		// cannot move further (e.g. openings are bound to their host's level)
						cur = next;
					}
				}, err);
				if (!ok)
					toolErrors.Put (g, err);
			}
		});
	}

	GS::Array<OS> results;
	for (const API_Guid& g : items.order) {
		if (const OS* e = items.errors.GetPtr (g)) {
			results.Push (ItemError (g, *e));
			continue;
		}
		if (const OS* e = toolErrors.GetPtr (g)) {
			results.Push (ItemError (g, *e));
			continue;
		}
		const Int32 now = ElementExists (g) ? DrawIndexOf (g) : -1;
		OS o ("guid", GuidStr (g), "drawIndexBefore", before[g], "drawIndex", now);
		if (level.has_value () && now != *level)
			o.Add ("warning", GS::UniString::Printf ("Could not reach level %d (windows/doors/skylights cannot go below their host's level; some types have fixed limits).", (int) *level));
		results.Push (o);
	}
	OS out ("results", results);
	if (!warnings.IsEmpty ())
		out.Add ("warnings", warnings);
	return out;
}

// =============================================================================
// Trim / merge / solid operations
// =============================================================================

const NamedValue kTrimTypesInput[] = {
	{ "KeepInside",		(Int32) APITrim_KeepInside },
	{ "KeepOutside",	(Int32) APITrim_KeepOutside },
	{ "KeepAll",		(Int32) APITrim_KeepAll },
};

const NamedValue kTrimTypes[] = {
	{ "KeepInside",		(Int32) APITrim_KeepInside },
	{ "KeepOutside",	(Int32) APITrim_KeepOutside },
	{ "KeepAll",		(Int32) APITrim_KeepAll },
	{ "None",			(Int32) APITrim_No },
};

const NamedValue kSolidOps[] = {
	{ "Subtract",			(Int32) APISolid_Substract },
	{ "SubtractUpwards",	(Int32) APISolid_SubstUp },
	{ "SubtractDownwards",	(Int32) APISolid_SubstDown },
	{ "Intersect",			(Int32) APISolid_Intersect },
	{ "Add",				(Int32) APISolid_Add },
	{ "Union",				(Int32) APISolid_Add },
	{ "Subtraction",		(Int32) APISolid_Substract },
	{ "Intersection",		(Int32) APISolid_Intersect },
};


GS::Array<API_Guid> TrimmingElementsOf (const API_Guid& g)
{
	GS::Array<API_Guid> list;
	if (ACAPI_Element_Trim_GetTrimmingElements (g, &list) != NoError)
		list.Clear ();
	return list;
}


GS::Array<API_Guid> TrimmedElementsOf (const API_Guid& g)
{
	GS::Array<API_Guid> list;
	if (ACAPI_Element_Trim_GetTrimmedElements (g, &list) != NoError)
		list.Clear ();
	return list;
}


GS::Array<API_Guid> MergedElementsOf (const API_Guid& g)
{
	GS::Array<API_Guid> list;
	if (ACAPI_Element_Merge_GetMergedElements (g, &list) != NoError)
		list.Clear ();
	return list;
}


GS::UniString TrimTypeName (const API_Guid& trimmed, const API_Guid& trimming)
{
	API_TrimTypeID type = APITrim_No;
	if (ACAPI_Element_Trim_GetTrimType (trimmed, trimming, &type) != NoError &&
		ACAPI_Element_Trim_GetTrimType (trimming, trimmed, &type) != NoError)
		return "Unknown";
	return NameOf (kTrimTypes, (Int32) type);
}


OS TrimElementsCommand (const OS& params)
{
	GS::Array<API_Guid> input = GetGuidArray (params, "elements");
	const std::optional<API_Guid> trimWith = OptGuid (params, "trimWith");
	const API_TrimTypeID trimType = (API_TrimTypeID) (params.Contains ("trimType") ? ParseNamed (kTrimTypesInput, params, "trimType") : (Int32) APITrim_KeepInside);
	if (params.Contains ("trimType") && !trimWith.has_value ())
		Fail ("'trimType' needs 'trimWith' (the roof/shell to trim with). Without trimWith the roofs/shells contained in 'elements' trim the others with the default type.");

	ItemList items = ResolveItems (input, false, Need::Editable);
	if (trimWith.has_value ()) {
		const API_Elem_Head tool = GetHeader (*trimWith);
		if (!IsRoofOrShell (tool.type.typeID))
			Fail ("'trimWith' must be a Roof or Shell, got " + ElemTypeName (tool.type) + ".", APIERR_BADELEMENTTYPE);
		items.valid.DeleteFirst (*trimWith);
	} else {
		Int32 cutters = 0, others = 0;
		for (const API_Guid& g : items.valid)
			IsRoofOrShell (items.heads[g].type.typeID) ? ++cutters : ++others;
		if (cutters == 0 || others == 0)
			Fail ("'elements' must contain at least one Roof/Shell (the trimming element) and at least one other construction element; or pass 'trimWith'.", APIERR_BADPARS);
	}
	if (items.valid.IsEmpty ())
		Fail ("No valid elements to trim.");

	GS::HashTable<API_Guid, OS> errors = items.errors;
	Undoable ("Trim elements (Claude)", [&] () {
		if (trimWith.has_value ()) {
			GSErrCode err = ACAPI_Element_Trim_ElementsWith (items.valid, *trimWith, trimType);
			if (err != NoError) {
				// isolate the offending elements
				for (const API_Guid& g : items.valid) {
					OS e;
					if (!Attempt ([&] () { CheckOp (ACAPI_Element_Trim_ElementsWith (GS::Array<API_Guid> { g }, *trimWith, trimType), "Cannot trim"); }, e))
						errors.Put (g, e);
				}
			}
		} else {
			CheckOp (ACAPI_Element_Trim_Elements (items.valid), "Cannot trim the elements");
		}
	});

	GS::Array<OS> results;
	for (const API_Guid& g : items.order) {
		if (trimWith.has_value () && g == *trimWith)
			continue;
		if (const OS* e = errors.GetPtr (g)) {
			results.Push (ItemError (g, *e));
			continue;
		}
		if (!trimWith.has_value () && IsRoofOrShell (items.heads[g].type.typeID))
			continue;		// a trimming element
		GS::Array<OS> by;
		for (const API_Guid& t : TrimmingElementsOf (g))
			by.Push (OS ("guid", GuidStr (t), "trimType", TrimTypeName (g, t)));
		results.Push (OS ("guid", GuidStr (g), "trimmedBy", by));
	}
	return OS ("results", results);
}


struct GuidPair {
	API_Guid a;
	API_Guid b;
};


// Reads [{<keyA>: guid, <keyB>: guid}] from params[listKey].
GS::Array<GuidPair> GetPairs (const OS& params, const char* listKey, const char* keyA, const char* keyB)
{
	GS::Array<GuidPair> pairs;
	for (const OS& p : GetObjectArray (params, listKey, false))
		pairs.Push ({ GetGuid (p, keyA), GetGuid (p, keyB) });
	return pairs;
}


OS PairResult (const char* keyA, const char* keyB, const GuidPair& p)
{
	return OS (keyA, GuidStr (p.a), keyB, GuidStr (p.b));
}


// Removes pairwise connections: explicit pairs + all connections of the given elements.
OS RemovePairsCommand (const OS& params, const char* listKey, const char* keyA, const char* keyB, const char* undoName,
					   const std::function<GS::Array<GuidPair> (const API_Guid&)>& connectionsOf,
					   const std::function<GSErrCode (const GuidPair&)>& removeFn)
{
	if (!params.Contains (listKey) && !params.Contains ("elements"))
		Fail (GS::UniString ("Give '") + listKey + "' and/or 'elements'.");
	GS::Array<GuidPair> pairs = GetPairs (params, listKey, keyA, keyB);
	for (const API_Guid& g : GetGuidArray (params, "elements", false)) {
		GetHeader (g);		// throws when missing
		for (const GuidPair& p : connectionsOf (g)) {
			bool dup = false;
			for (const GuidPair& q : pairs)
				dup = dup || (q.a == p.a && q.b == p.b) || (q.a == p.b && q.b == p.a);
			if (!dup)
				pairs.Push (p);
		}
	}
	GS::Array<OS> results;
	if (pairs.IsEmpty ())
		return OS ("results", results, "removedCount", (Int32) 0);

	Int32 removed = 0;
	Undoable (undoName, [&] () {
		for (const GuidPair& p : pairs) {
			OS e;
			if (Attempt ([&] () { CheckOp (removeFn (p), "Cannot remove the connection"); }, e)) {
				OS o = PairResult (keyA, keyB, p);
				o.Add ("removed", true);
				results.Push (o);
				++removed;
			} else {
				OS o = PairResult (keyA, keyB, p);
				o.Add ("error", e);
				results.Push (o);
			}
		}
	});
	return OS ("results", results, "removedCount", removed);
}


OS MergeElementsCommand (const OS& params)
{
	ItemList items = ResolveItems (GetGuidArray (params, "elements"), false, Need::Editable);
	if (!items.errors.IsEmpty ()) {
		GS::Array<OS> errs;
		for (const API_Guid& g : items.order)
			if (const OS* e = items.errors.GetPtr (g))
				errs.Push (ItemError (g, *e));
		return OS ("merged", false, "results", errs);
	}
	if (items.valid.GetSize () < 2)
		Fail ("Merging needs at least two construction elements.");
	Undoable ("Merge elements (Claude)", [&] () {
		CheckOp (ACAPI_Element_Merge_Elements (items.valid), "Cannot merge the elements");
	});
	GS::Array<OS> results;
	for (const API_Guid& g : items.valid)
		results.Push (OS ("guid", GuidStr (g), "mergedWith", GuidStrings (MergedElementsOf (g))));
	return OS ("merged", true, "results", results);
}


OS SolidLinkJson (const API_Guid& target, const API_Guid& op, const char* otherKey, const API_Guid& other)
{
	OS o (otherKey, GuidStr (other));
	API_SolidOperationID operation = APISolid_Substract;
	if (ACAPI_Element_SolidLink_GetOperation (target, op, &operation) == NoError)
		o.Add ("operation", NameOf (kSolidOps, (Int32) operation));
	GSFlags flags = 0;
	if (ACAPI_Element_SolidLink_GetFlags (target, op, &flags) == NoError) {
		o.Add ("inheritOperatorAttributes", (flags & APISolidFlag_OperatorAttrib) != 0);
		o.Add ("skipOperatorHoles", (flags & APISolidFlag_SkipPolygonHoles) != 0);
	}
	return o;
}


OS CreateSolidOperationsCommand (const OS& params)
{
	const bool permanent = GetBool (params, "permanent", false);
	GS::Array<OS> specs = params.Contains ("operations") ? GetObjectArray (params, "operations") : GS::Array<OS> { params };
	if (specs.IsEmpty ())
		Fail ("'operations' is empty.");

	struct SolidSpec {
		API_Guid				target;
		GS::Array<API_Guid>		operators;
		API_SolidOperationID	operation;
		GSFlags					flags;
	};
	GS::Array<SolidSpec> parsed;
	for (const OS& s : specs) {
		SolidSpec sp;
		sp.target = GetGuid (s, "target");
		sp.operators = s.Contains ("operators") ? GetGuidArray (s, "operators") : GS::Array<API_Guid> { GetGuid (s, "operator") };
		if (sp.operators.IsEmpty ())
			Fail ("'operators' is empty.");
		sp.operation = (API_SolidOperationID) (s.Contains ("operation") ? ParseNamed (kSolidOps, s, "operation") : (Int32) APISolid_Substract);
		sp.flags = 0;
		if (GetBool (s, "inheritOperatorAttributes", false))	sp.flags |= APISolidFlag_OperatorAttrib;
		if (GetBool (s, "skipOperatorHoles", false))			sp.flags |= APISolidFlag_SkipPolygonHoles;
		const API_Elem_Head th = GetHeader (sp.target);
		for (const API_Guid& o : sp.operators) {
			if (o == sp.target)
				Fail ("An element cannot operate on itself (" + GuidStr (o) + ").");
			const API_Elem_Head oh = GetHeader (o);
			if (permanent && (th.type.typeID != API_MorphID || oh.type.typeID != API_MorphID))
				Fail ("permanent=true (boolean that replaces the elements with new morphs) works only when target and operators are Morphs; "
					  "use permanent=false (a live Solid Element Operation) for walls, slabs, roofs, beams, columns, objects...", APIERR_BADELEMENTTYPE);
		}
		parsed.Push (sp);
	}

	GS::Array<OS> results;
	Undoable ("Solid element operations (Claude)", [&] () {
		for (const SolidSpec& sp : parsed) {
			if (!permanent) {
				GS::Array<OS> links;
				for (const API_Guid& o : sp.operators) {
					OS e;
					if (Attempt ([&] () {
							const GSErrCode err = ACAPI_Element_SolidLink_Create (sp.target, o, sp.operation, sp.flags);
							if (err == APIERR_LINKEXIST)
								Fail ("These elements are already linked by a solid operation: remove_solid_operation first to change it.", err);
							if (err == APIERR_REFUSEDPAR)
								Fail ("Hotlinked elements cannot take part in solid operations.", err);
							CheckOp (err, "Cannot create the solid operation");
						}, e)) {
						links.Push (OS ("operator", GuidStr (o), "operation", NameOf (kSolidOps, (Int32) sp.operation)));
					} else {
						links.Push (OS ("operator", GuidStr (o), "error", e));
					}
				}
				results.Push (OS ("target", GuidStr (sp.target), "links", links));
				continue;
			}
			// Permanent morph boolean: target and operator are replaced by the result morph(s).
			OS e;
			GS::Array<API_Guid> current { sp.target };
			GS::Array<GS::UniString> warnings;
			const bool ok = Attempt ([&] () {
				for (UIndex k = 0; k < sp.operators.GetSize (); ++k) {
					if (current.IsEmpty ())
						Fail ("The previous operation produced no result.");
					if (current.GetSize () > 1)
						warnings.Push ("The operation produced several morphs; the next operator was applied to the first one only.");
					GS::Array<API_Guid> res;
					CheckOp (ACAPI_Element_SolidOperation_Create (current[0], sp.operators[k], sp.operation, &res), "Solid operation failed");
					GS::Array<API_Guid> rest;
					for (UIndex r = 1; r < current.GetSize (); ++r)
						rest.Push (current[r]);
					current = res;
					current.Append (rest);
				}
			}, e);
			if (ok) {
				OS o ("target", GuidStr (sp.target), "resultMorphs", GuidStrings (current));
				if (!warnings.IsEmpty ())
					o.Add ("warnings", warnings);
				results.Push (o);
			} else {
				results.Push (OS ("target", GuidStr (sp.target), "error", e, "resultMorphs", GuidStrings (current)));
			}
		}
	});
	return OS ("results", results);
}


OS EditRelationsJson (const API_Guid& g)
{
	const API_Elem_Head head = GetHeader (g);
	OS out ("guid", GuidStr (g), "type", ElemTypeName (head.type), "storyIndex", (Int32) head.floorInd);
	out.Add ("locked", head.lockId != 0);
	out.Add ("drawIndex", (Int32) head.drwIndex);
	out.Add ("editable", ACAPI_Element_Filter (g, APIFilt_IsEditable));
	if (head.groupGuid != APINULLGuid) {
		out.Add ("groupGuid", GuidStr (head.groupGuid));
		const API_Guid root = RootGroupOf (g);
		if (root != APINULLGuid) {
			out.Add ("rootGroupGuid", GuidStr (root));
			GS::Array<API_Guid> members;
			if (GroupElementsOf (root, members)) {
				out.Add ("groupElementCount", (Int32) members.GetSize ());
				if (members.GetSize () <= 200)
					out.Add ("groupElements", GuidStrings (members));
			}
		}
	}
	if (head.hotlinkGuid != APINULLGuid)
		out.Add ("hotlinkGuid", GuidStr (head.hotlinkGuid));

	GS::Array<OS> trimmedBy;
	for (const API_Guid& t : TrimmingElementsOf (g))
		trimmedBy.Push (OS ("guid", GuidStr (t), "trimType", TrimTypeName (g, t)));
	if (!trimmedBy.IsEmpty ())
		out.Add ("trimmedBy", trimmedBy);
	GS::Array<OS> trims;
	for (const API_Guid& t : TrimmedElementsOf (g))
		trims.Push (OS ("guid", GuidStr (t), "trimType", TrimTypeName (t, g)));
	if (!trims.IsEmpty ())
		out.Add ("trims", trims);

	const GS::Array<API_Guid> merged = MergedElementsOf (g);
	if (!merged.IsEmpty ())
		out.Add ("mergedWith", GuidStrings (merged));

	GS::Array<API_Guid> operators, targets;
	if (ACAPI_Element_SolidLink_GetOperators (g, &operators) == NoError && !operators.IsEmpty ()) {
		GS::Array<OS> list;
		for (const API_Guid& o : operators)
			list.Push (SolidLinkJson (g, o, "operator", o));
		out.Add ("solidOperators", list);		// elements cutting/adding to this element (this is the target)
	}
	if (ACAPI_Element_SolidLink_GetTargets (g, &targets) == NoError && !targets.IsEmpty ()) {
		GS::Array<OS> list;
		for (const API_Guid& t : targets)
			list.Push (SolidLinkJson (t, g, "target", t));
		out.Add ("solidTargets", list);		// elements this element operates on (this is the operator)
	}
	return out;
}

} // namespace

// =============================================================================
// Registration
// =============================================================================

void RegisterElementEditCommands ()
{
	// --- Transformations ---------------------------------------------------------

	RegisterTransformCommand ("MoveElements",
		"Moves elements of any type by a displacement vector (one undo step). Input: {elements: [guid], vector: {x, y, z?} (m), includeGroupMembers?} "
		"or {operations: [{elements, vector, ...}]}. Output: {results: [{guid, newGuid?, warning?} | {guid, error}], warnings?} "
		"(batch: {operations: [...]}).",
		EditKind::Drag, false, "Move elements (Claude)");

	RegisterTransformCommand ("CopyElements",
		"Copies elements by a displacement vector; count > 1 makes an array (copy k at k*vector). Input: {elements, vector: {x, y, z?}, count?: 1, "
		"includeGroupMembers?} or {operations: [...]}. Output: {results: [{guid, copies: [newGuid]}], createdCount, additionalCreated?: [{guid, type}], warnings?}.",
		EditKind::Drag, true, "Copy elements (Claude)");

	RegisterTransformCommand ("RotateElements",
		"Rotates elements around a centre point (default: centre of their bounding box). Input: {elements, angle (degrees, CCW positive), center?: {x, y}, "
		"copy?: false, count?: 1 (with copy: polar array, copy k rotated k*angle), includeGroupMembers?} or {operations: [...]}.",
		EditKind::Rotate, false, "Rotate elements (Claude)");

	RegisterTransformCommand ("MirrorElements",
		"Mirrors elements across a line. Input: {elements, axisStart: {x, y}, axisEnd: {x, y}} or {elements, axis: 'Vertical'|'Horizontal', through?: {x, y} "
		"(default: bounding-box centre)}, copy?: false, includeGroupMembers?. Or {operations: [...]}.",
		EditKind::Mirror, false, "Mirror elements (Claude)");

	RegisterTransformCommand ("ElevateElements",
		"Changes the elevation of elements by deltaZ meters (positive = up). Input: {elements, deltaZ, copy?: false, count?: 1, includeGroupMembers?} "
		"or {operations: [...]}.",
		EditKind::Elevate, false, "Elevate elements (Claude)");

	RegisterTransformCommand ("ResizeElements",
		"Scales elements in plan by 'ratio' around a centre point (default: bounding-box centre). Input: {elements, ratio (>0), center?: {x, y}, copy?: false, "
		"includeGroupMembers?} or {operations: [...]}.",
		EditKind::Resize, false, "Resize elements (Claude)");

	// --- Delete / copy to stories ----------------------------------------------

	RegisterCommand ("DeleteElements",
		"Deletes elements of any type (one undo step). Input: {elements: [guid], includeGroupMembers?: false}. Group GUIDs delete the whole group. "
		"Output: {results: [{guid, deleted: true} | {guid, error}], deletedCount, alsoDeleted?: [{guid, type}] (dependent elements removed too: "
		"openings of deleted walls, associative dimensions/labels...), warnings?}.",
		DeleteElementsCommand);

	RegisterCommand ("CopyElementsToStories",
		"Duplicates elements onto other stories keeping their plan position and elevation relative to the home story. Input: {elements, stories: "
		"[index | name], includeGroupMembers?}. Output: {results: [{guid, copies: [{storyIndex, guid} | {storyIndex, error}]}], additionalCreated?, warnings?}.",
		CopyElementsToStoriesCommand);

	// --- Groups -------------------------------------------------------------------

	RegisterCommand ("GroupElements",
		"Creates groups (one undo step). Input: {elements: [guid (element or group)], parentGroup?} or {groups: [{elements, parentGroup?}]}. "
		"Elements already in a group bring their top-level group (nested group). All must be on the same story. "
		"Output: {groupGuid, members, elementCount} (batch: {results: [...]}).",
		[] (const OS& params) -> OS {
			if (!params.Contains ("groups")) {
				OS out;
				Undoable ("Group elements (Claude)", [&] () { out = GroupOne (params); });
				return out;
			}
			GS::Array<OS> specs = GetObjectArray (params, "groups");
			GS::Array<OS> results;
			Undoable ("Group elements (Claude)", [&] () {
				for (const OS& spec : specs)
					results.Push (Try ([&] () { return GroupOne (spec); }));
			});
			return OS ("results", results);
		});

	RegisterCommand ("UngroupElements",
		"Dissolves groups. Input: {elements: [guid (group GUID, or any member element = its top-level group)], completely?: false (true = also dissolve "
		"all nested sub-groups)}. Output: {results: [{guid, dissolvedGroups, remainingGroup?} | {guid, error}]}.",
		[] (const OS& params) -> OS {
			GS::Array<API_Guid> input = GetGuidArray (params, "elements");
			const bool completely = GetBool (params, "completely", false);
			if (input.IsEmpty ())
				Fail ("'elements' is empty.");
			GS::Array<OS> results;
			// Several inputs may belong to the same group: once it is dissolved by an earlier item,
			// the later members are reported as ungrouped (not as "not grouped" errors).
			GS::HashTable<API_Guid, API_Guid> dissolvedWith;
			Undoable ("Ungroup elements (Claude)", [&] () {
				for (const API_Guid& g : input) {
					if (RootGroupOf (g) == APINULLGuid) {
						if (const API_Guid* grp = dissolvedWith.GetPtr (g)) {
							GS::Array<API_Guid> one;
							one.Push (*grp);
							results.Push (OS ("guid", GuidStr (g), "dissolvedGroups", GuidStrings (one), "sameGroupAsEarlierItem", true));
							continue;
						}
					} else {
						GS::Array<API_Guid> members;
						const API_Guid root = RootGroupOf (g);
						if (GroupElementsOf (root, members)) {
							for (const API_Guid& m : members)
								dissolvedWith.Put (m, root);
						}
					}
					OS r = Try ([&] () { return UngroupOne (g, completely); });
					if (r.Contains ("error") && !r.Contains ("guid"))
						r.Add ("guid", GuidStr (g));
					results.Push (r);
				}
			});
			return OS ("results", results);
		});

	RegisterCommand ("SetSuspendGroups",
		"Reads or sets the 'Suspend Groups' mode (when ON, grouped elements can be edited individually). Input: {suspend?: bool} (omit to just read). "
		"Output: {suspendGroups, changed}.",
		[] (const OS& params) -> OS {
			const std::optional<bool> want = OptBool (params, "suspend");
			const bool before = IsSuspendGroupsOn ();
			if (want.has_value () && *want != before) {
				Check (ToggleSuspendGroups (), "Cannot switch Suspend Groups");
				if (IsSuspendGroupsOn () != *want)
					Fail ("Archicad did not switch Suspend Groups.", APIERR_REFUSEDCMD);
			}
			const bool now = IsSuspendGroupsOn ();
			return OS ("suspendGroups", now, "changed", now != before);
		});

	// --- Lock / draw order --------------------------------------------------------

	RegisterCommand ("LockElements",
		"Locks elements (Edit > Locking > Lock) so they cannot be edited until unlocked. Input: {elements, includeGroupMembers?}. "
		"Output: {results: [{guid, locked: true} | {guid, error}]}.",
		[] (const OS& params) -> OS { return LockCommand (params, true); });

	RegisterCommand ("UnlockElements",
		"Unlocks locked elements. Input: {elements, includeGroupMembers?}. Output: {results: [{guid, locked: false} | {guid, error}]}.",
		[] (const OS& params) -> OS { return LockCommand (params, false); });

	RegisterCommand ("SetDrawOrder",
		"Changes the 2D drawing (stacking) order. Input: {elements, action: 'BringToFront'|'BringForward'|'SendBackward'|'SendToBack'|'Reset', steps?: 1} "
		"or {elements, level: 1..14}; includeGroupMembers?. Output: {results: [{guid, drawIndexBefore, drawIndex, warning?}]}.",
		SetDrawOrderCommand);

	// --- Trim / merge / solid operations ------------------------------------------

	RegisterCommand ("TrimElements",
		"Trims construction elements to roofs/shells. Input: {elements, trimWith?: roof/shell guid, trimType?: 'KeepInside'|'KeepOutside'|'KeepAll' "
		"(needs trimWith)}. Without trimWith the roofs/shells inside 'elements' trim the other elements. Output: {results: [{guid, trimmedBy: [{guid, trimType}]}]}.",
		TrimElementsCommand);

	RegisterCommand ("RemoveTrims",
		"Removes trim connections. Input: {pairs?: [{element, trimmingElement}], elements?: [guid] (remove every trim connection of these elements)}. "
		"Output: {results: [{element, trimmingElement, removed} | {..., error}], removedCount}.",
		[] (const OS& params) -> OS {
			return RemovePairsCommand (params, "pairs", "element", "trimmingElement", "Remove trims (Claude)",
				[] (const API_Guid& g) {
					GS::Array<GuidPair> list;
					for (const API_Guid& t : TrimmingElementsOf (g)) list.Push ({ g, t });
					for (const API_Guid& t : TrimmedElementsOf (g)) list.Push ({ t, g });
					return list;
				},
				[] (const GuidPair& p) {
					GSErrCode err = ACAPI_Element_Trim_Remove (p.a, p.b);
					if (err != NoError && ACAPI_Element_Trim_Remove (p.b, p.a) == NoError)
						err = NoError;
					return err;
				});
		});

	RegisterCommand ("MergeElements",
		"Merges construction elements (Design > Connect > Merge: joined 3D/section display). Input: {elements: [>= 2 guids]}. "
		"Output: {merged, results: [{guid, mergedWith}]}.",
		MergeElementsCommand);

	RegisterCommand ("UnmergeElements",
		"Removes merge connections. Input: {pairs?: [{element, otherElement}], elements?: [guid] (remove all merges of these)}. "
		"Output: {results: [{element, otherElement, removed} | {..., error}], removedCount}.",
		[] (const OS& params) -> OS {
			return RemovePairsCommand (params, "pairs", "element", "otherElement", "Unmerge elements (Claude)",
				[] (const API_Guid& g) {
					GS::Array<GuidPair> list;
					for (const API_Guid& m : MergedElementsOf (g)) list.Push ({ g, m });
					return list;
				},
				[] (const GuidPair& p) { return ACAPI_Element_Merge_Remove (p.a, p.b); });
		});

	RegisterCommand ("CreateSolidOperations",
		"Solid Element Operations. Input: {operations: [{target, operators: [guid] | operator, operation?: 'Subtract'|'SubtractUpwards'|'SubtractDownwards'|"
		"'Intersect'|'Add' (default Subtract), inheritOperatorAttributes?, skipOperatorHoles?}], permanent?: false}. permanent=false creates live links "
		"(any construction elements); permanent=true performs a destructive morph boolean (morphs only; inputs are replaced). "
		"Output: {results: [{target, links: [{operator, operation} | {operator, error}]} | {target, resultMorphs}]}.",
		CreateSolidOperationsCommand);

	RegisterCommand ("RemoveSolidOperations",
		"Removes solid operation links. Input: {links?: [{target, operator}], elements?: [guid] (remove every link where the element is target or operator)}. "
		"Output: {results: [{target, operator, removed} | {..., error}], removedCount}.",
		[] (const OS& params) -> OS {
			return RemovePairsCommand (params, "links", "target", "operator", "Remove solid operations (Claude)",
				[] (const API_Guid& g) {
					GS::Array<GuidPair> list;
					GS::Array<API_Guid> ops, targets;
					if (ACAPI_Element_SolidLink_GetOperators (g, &ops) == NoError)
						for (const API_Guid& o : ops) list.Push ({ g, o });
					if (ACAPI_Element_SolidLink_GetTargets (g, &targets) == NoError)
						for (const API_Guid& t : targets) list.Push ({ t, g });
					return list;
				},
				[] (const GuidPair& p) {
					GSErrCode err = ACAPI_Element_SolidLink_Remove (p.a, p.b);
					if (err == APIERR_LINKNOTEXIST && ACAPI_Element_SolidLink_Remove (p.b, p.a) == NoError)
						err = NoError;
					if (err == APIERR_LINKNOTEXIST)
						Fail ("There is no solid operation between these elements (check get_element_edit_relations).", err);
					return err;
				});
		});

	RegisterCommand ("GetElementEditRelations",
		"Read-only. For each element: type, story, locked, drawIndex, editable, group (groupGuid, rootGroupGuid, groupElements), hotlink, "
		"trimmedBy / trims (with trimType), mergedWith, solidOperators (elements operating on it) and solidTargets (elements it operates on). "
		"Input: {elements: [guid]}.",
		[] (const OS& params) -> OS {
			GS::Array<API_Guid> input = GetGuidArray (params, "elements");
			GS::Array<OS> out;
			for (const API_Guid& g : input) {
				OS r = Try ([&] () { return EditRelationsJson (g); });
				if (r.Contains ("error") && !r.Contains ("guid"))
					r.Add ("guid", GuidStr (g));
				out.Push (r);
			}
			return OS ("elements", out);
		});
}

} // namespace cc
