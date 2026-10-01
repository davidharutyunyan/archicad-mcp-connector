// *****************************************************************************
// SlabsRoofsCommon.hpp — helpers shared by the Slab / Roof / Shell / Mesh adapters
// (slabs-roofs family). Everything lives in cc::slabroof to avoid clashes with
// other families' helpers.
//
// JSON conventions (see docs/DEVELOPING.md): meters, DEGREES, camelCase, enums as strings.
// *****************************************************************************

#pragma once

#include "Core/Elements.hpp"
#include "Core/Enums.hpp"
#include "Core/Polygon.hpp"

namespace cc {
namespace slabroof {

// --- Enum tables ---------------------------------------------------------------

inline const NamedValue kEdgeTrims[] = {
	{ "Vertical",		APIEdgeTrim_Vertical },
	{ "Perpendicular",	APIEdgeTrim_Perpendicular },
	{ "Horizontal",		APIEdgeTrim_Horizontal },
	{ "CustomAngle",	APIEdgeTrim_CustomAngle },
	{ "AlignWithCut",	APIEdgeTrim_AlignWithCut },
};

// Same numeric values for API_RoofEdgeTypeID and API_ShellBaseContourEdgeTypeID.
inline const NamedValue kEdgeTypes[] = {
	{ "Undefined",	APIRoofEdgeType_Undefined },
	{ "Ridge",		APIRoofEdgeType_Ridge },
	{ "Valley",		APIRoofEdgeType_Valley },
	{ "Gable",		APIRoofEdgeType_Gable },
	{ "Hip",		APIRoofEdgeType_Hip },
	{ "Eaves",		APIRoofEdgeType_Eaves },
	{ "Peak",		APIRoofEdgeType_Peak },
	{ "SideWall",	APIRoofEdgeType_SideWall },
	{ "EndWall",	APIRoofEdgeType_EndWall },
	{ "Dome",		APIRoofEdgeType_RTDome },
	{ "Hollow",		APIRoofEdgeType_RTHollow },
};

inline const NamedValue kConnectionBodies[] = {
	{ "Editable",			APIShellBaseCutBody_Editable },
	{ "ContoursDown",		APIShellBaseCutBody_ContoursDown },
	{ "PivotLinesDown",		APIShellBaseCutBody_PivotLinesDown },
	{ "UpwardsExtrusion",	APIShellBaseCutBody_UpwardsExtrusion },
	{ "DownwardsExtrusion",	APIShellBaseCutBody_DownwardsExtrusion },
};

inline const NamedValue kDisplayOptions[] = {
	{ "Projected",				API_Standard },
	{ "ProjectedWithOverhead",	API_StandardWithAbstract },
	{ "CutOnly",				API_CutOnly },
	{ "OutlinesOnly",			API_OutLinesOnly },
	{ "OverheadAll",			API_AbstractAll },
	{ "CutAll",					API_CutAll },
};

inline const NamedValue kViewDepths[] = {
	{ "FloorPlanRange",	API_ToFloorPlanRange },
	{ "AbsoluteLimit",	API_ToAbsoluteLimit },
	{ "EntireElement",	API_EntireElement },
};

inline const NamedValue kSegmentTypes[] = {
	{ "ByCircle",	APIShellBase_SegmentsByCircle },
	{ "ByArc",		APIShellBase_SegmentsByArc },
};

// --- Mask helper -----------------------------------------------------------------
// Marks ALL mask bytes of `field` inside `element` (ACAPI_ELEMENT_MASK_SET marks only the first
// byte, which Archicad ignores for some struct members, e.g. API_StoryVisibility: only showOnHome
// changed — verified live). Usable for fields shared by several element types (e.g.
// API_ShellBaseType inside both API_RoofType and API_ShellType). No-op when mask is null.
class FieldMask {
public:
	FieldMask (const API_Element& element, API_Element* mask) : element (element), mask (mask) {}
	template <class T>
	void Set (const T* field) const { SetBytes (field, sizeof (T)); }
	void SetBytes (const void* field, size_t size) const;
	bool Creating () const { return mask == nullptr; }

private:
	const API_Element&	element;
	API_Element*		mask;
};

bool			AnyMasked (const API_Element& mask);
// Applies the masked struct changes now (ACAPI_Element_Change without memo) and clears the mask,
// so a following memo-only change (ChangeMemo / Change with memo) sees the new struct.
void			CommitStructChange (API_Element& element, API_Element& mask, const GS::UniString& what);

// --- Small field helpers -------------------------------------------------------------

// Pen index 1..255. Returns true when the field was present.
bool			ApplyPen (const OS& spec, const char* key, short& pen);
// Optional attribute of the given type (index / name / {guid}). Returns true when present.
bool			ApplyAttr (const OS& spec, const char* key, API_AttrTypeID type, API_AttributeIndex& index);
bool			ApplyFlag (const OS& spec, const char* key, bool& value);
bool			ApplyLength (const OS& spec, const char* key, double& value, bool positive = false);
bool			ApplyAngleDeg (const OS& spec, const char* key, double& radians);

// When spec has "composite": fails unless the composite is enabled for the element type
// (usageFlag = APICWall_ForSlab / ForRoof / ForShell; Archicad itself accepts any composite via the API,
// leaving an element the UI cannot show in its settings).
void			CheckCompositeUsage (const OS& spec, API_AttributeIndex composite, short usageFlag, const char* elementPlural);

// "cutFillPen" / "cutFillBackgroundPen": pen index, or false to remove the override.
bool			ApplyPenOverride (const OS& spec, API_PenOverrideType& po);
void			AddPenOverrideJson (OS& out, const API_PenOverrideType& po);

// "storyVisibility": {onHomeStory, allAbove, allBelow, storiesAbove, storiesBelow} (applied to contour and fill).
bool			ApplyStoryVisibility (const OS& spec, API_StoryVisibility& contour, API_StoryVisibility& fill);
OS				StoryVisibilityJson (const API_StoryVisibility& v);

// "edgeTrim" (kEdgeTrims) + "edgeAngle" (degrees; implies CustomAngle when edgeTrim is omitted).
bool			ApplyEdgeTrim (const OS& spec, API_EdgeTrim& trim, const char* trimKey = "edgeTrim", const char* angleKey = "edgeAngle");
void			AddEdgeTrimJson (OS& out, const API_EdgeTrim& trim, const char* trimKey = "edgeTrim", const char* angleKey = "edgeAngle");
bool			SameTrim (const API_EdgeTrim& a, const API_EdgeTrim& b);
bool			SameOverride (const API_OverriddenAttribute& a, const API_OverriddenAttribute& b);

// --- Polygon helpers -----------------------------------------------------------------

struct ContourRange {
	Int32 first = 0;	// memo index of the first vertex
	Int32 count = 0;	// number of real vertices (= number of edges); closing vertex is first + count
};

// Contours described by a memo pends handle (pends[0] = 0).
GS::Array<ContourRange>	ContoursOf (Int32** pends);
Int32					HandleCount (GSConstHandle h, size_t itemSize);	// items in a handle (0 for null)
Int32					PtrCount (GSConstPtr p, size_t itemSize);
// API_Polygon counts computed from the memo handles (robust against stale struct counts).
API_Polygon				PolygonCounts (API_Coord** coords, Int32** pends, API_PolyArc** parcs);

// Which contours WritePolygonToMemo / NormalizeOrientation will reverse (outline CW, holes CCW).
GS::Array<bool>			ReversedContours (const PolygonData& data);
// Maps an edge index given for the caller's point order to the stored (normalized) order.
Int32					MapEdgeIndex (Int32 index, Int32 count, bool reversed);

// Reshapes an EXISTING polygon memo (loaded with ACAPI_Element_GetMemo) to the new polygon with
// Archicad's own polygon primitives (delete holes, resize outline, rewrite coords, re-add holes).
// This is the pattern that works reliably with ACAPI_Element_ChangeMemo for slabs/roofs
// (a from-scratch memo fails with APIERR_BADPOLY when the vertex count changes).
// Edge data (edgeTrims / sideMaterials / roofEdgeTypes) is NOT updated — rebuild it afterwards.
void					ReshapePolygonInPlace (API_ElementMemo& memo, const PolygonData& polygon);

// Replaces memo.coords/pends/parcs (+ vertexIDs, meshPolyZ, edgeTrims, sideMaterials when present)
// with the regularized polygon after ACAPI_Element_Create returned APIERR_IRREGULARPOLY.
// Returns true when the memo was replaced (retry the creation), false when regularization
// failed; throws an actionable error when the polygon splits into several parts.
bool					RegularizeMemoPolygon (API_ElementMemo& memo, API_Polygon& poly, const char* elementName);

// Resizes (or allocates) a memo handle to `count` zero-initialized items of T (existing items kept).
template <class T>
void ResizeHandle (T**& handle, Int32 count)
{
	const GSSize bytes = (GSSize) (count > 0 ? count : 0) * (GSSize) sizeof (T);
	GSHandle h = (handle == nullptr)
		? BMAllocateHandle (bytes, ALLOCATE_CLEAR, 0)
		: BMReallocHandle (reinterpret_cast<GSHandle> (handle), bytes, REALLOC_CLEAR, 0);
	if (h == nullptr)
		Fail ("Out of memory.", APIERR_MEMFULL);
	handle = reinterpret_cast<T**> (h);
}

// Releases memo parts through Archicad's own disposer (safe for memos returned by GetMemo):
// moves the given member into a scratch memo and disposes it.
void					DisposePivotEdges (API_ElementMemo& memo);

// --- Edge data (slab and single-plane roof contour edges) --------------------------------

// (Re)allocates memo.edgeTrims / memo.sideMaterials for nCoords and fills every entry.
void					AllocEdgeData (API_ElementMemo& memo, Int32 nCoords, const API_EdgeTrim& trim, const API_OverriddenAttribute& sideMat);
// Allocates memo.roofEdgeTypes (all Undefined) when missing.
void					EnsureRoofEdgeTypes (API_ElementMemo& memo, Int32 nCoords);
// True when memo edge arrays exist and match the polygon size.
bool					HasEdgeData (const API_ElementMemo& memo, Int32 nCoords);
// Applies "edges": [{contour?, index, trim?, angle?, surface?, edgeType?}].
// reversed = per-contour flags when the polygon was given in the same spec (else null).
// Returns true when anything was applied.
bool					ApplyEdgeOverrides (const OS& spec, API_ElementMemo& memo, const GS::Array<bool>* reversed, bool allowEdgeTypes);
// Copies each contour's first-edge data to its closing vertex (Archicad convention).
void					SyncClosingEdgeData (API_ElementMemo& memo);
// "edges": only edges that differ from the defaults (trim, side surface, edge type).
void					AddEdgesJson (OS& out, const API_ElementMemo& memo, const API_EdgeTrim& defTrim,
									  const API_OverriddenAttribute& defSide, bool withEdgeTypes);

// --- Shell base (roofs and shells) -------------------------------------------------------------

void					ApplyShellBaseFields (API_ShellBaseType& base, const FieldMask& m, const OS& spec);
void					AddShellBaseJson (OS& out, const API_ShellBaseType& base);

// --- 3D frames (shell planes) -----------------------------------------------------------------
// JSON: {origin:{x,y,z}, xAxis:{x,y,z}, yAxis:{x,y,z}} (axes are normalized/orthogonalized, zAxis = x × y)
//    or {matrix: [12 numbers]} (row-major 3x4, translation in elements 3, 7, 11).
API_Tranmat				IdentityTranmat ();
API_Tranmat				FrameFrom (const OS& spec, const char* key);
OS						FrameJson (const API_Tranmat& tm);
// Frame whose X axis is (cos a, sin a, 0), Y axis is world up, origin as given.
API_Tranmat				VerticalFrame (const API_Coord3D& origin, double rotationRadians);

API_Coord3D				Point3DFrom (const OS& obj, const char* key, double defZ);
std::optional<API_Coord3D> OptPoint3D (const OS& spec, const char* key, double defZ);

} // namespace slabroof

// Registration of the individual adapters (called from RegisterSlabRoofCommands in SlabsRoofs.cpp).
void RegisterSlabAdapter ();
void RegisterRoofAdapter ();
void RegisterShellAdapter ();
void RegisterMeshAdapter ();

} // namespace cc
