// *****************************************************************************
// ApiCompat.hpp — keeps the Archicad 26 API call style on Archicad 27 and newer.
//
// Archicad 27 replaced the dispatcher functions ACAPI_Environment / _Database / _Goodies / _Automate / _Navigator
// (one function, an operation code and void* parameters) by one function per operation, e.g.
// ACAPI_Database (APIDb_GetCurrentWindowID, &info) became ACAPI_Window_GetCurrentWindow (&info). The operation codes
// are still declared by the newer headers, so on 27+ this file re-creates the five dispatchers on top of the new
// functions (ApiCompat.cpp) and the command code keeps working unchanged on every supported version.
// *****************************************************************************

#pragma once

#include "APIEnvir.h"
#include "ACAPinc.h"

#include <utility>

#if defined (CC_AC_VERSION) && CC_AC_VERSION >= 27

// Operation codes the newer headers no longer declare (same values as in Archicad 26).
constexpr API_AutomateID	APIDo_ShowSelectionIn3DID		= static_cast<API_AutomateID> ('SS3D');
constexpr API_AutomateID	APIDo_ShowAllIn3DID				= static_cast<API_AutomateID> ('SA3D');
#if CC_AC_VERSION >= 29
constexpr API_GoodiesID		APIAny_GetHttpConnectionPortID	= static_cast<API_GoodiesID> ('GHCP');
#endif

GSErrCode	ACAPI_Environment	(API_EnvironmentID code, void* par1 = nullptr, void* par2 = nullptr, void* par3 = nullptr);
GSErrCode	ACAPI_Database		(API_DatabaseID code, void* par1 = nullptr, void* par2 = nullptr, void* par3 = nullptr);
GSErrCode	ACAPI_Goodies		(API_GoodiesID code, void* par1 = nullptr, void* par2 = nullptr, void* par3 = nullptr, void* par4 = nullptr);
GSErrCode	ACAPI_Automate		(API_AutomateID code, void* par1 = nullptr, void* par2 = nullptr, void* par3 = nullptr);
GSErrCode	ACAPI_Navigator		(API_NavigatorID code, void* par1 = nullptr, void* par2 = nullptr, void* par3 = nullptr, void* par4 = nullptr);

// Functions that Archicad 27 only moved to another family (same parameters): the old names forward to the new ones.
#define CC_API_RENAMED(oldName, newName) \
	template <typename... Args> \
	inline auto oldName (Args&&... args) -> decltype (newName (std::forward<Args> (args)...)) { return newName (std::forward<Args> (args)...); }

CC_API_RENAMED (ACAPI_3D_GetComponent, ACAPI_ModelAccess_GetComponent)
CC_API_RENAMED (ACAPI_3D_GetCurrentWindowSight, ACAPI_Sight_GetCurrentWindowSight)
CC_API_RENAMED (ACAPI_3D_GetNum, ACAPI_ModelAccess_GetNum)
CC_API_RENAMED (ACAPI_3D_SelectSight, ACAPI_Sight_SelectSight)
CC_API_RENAMED (ACAPI_ElementGroup_Create, ACAPI_Grouping_CreateGroup)
CC_API_RENAMED (ACAPI_ElementGroup_GetAllGroupedElems, ACAPI_Grouping_GetAllGroupedElems)
CC_API_RENAMED (ACAPI_ElementGroup_GetRootGroup, ACAPI_Grouping_GetRootGroup)
CC_API_RENAMED (ACAPI_Element_DeselectAll, ACAPI_Selection_DeselectAll)
CC_API_RENAMED (ACAPI_Element_Get3DInfo, ACAPI_ModelAccess_Get3DInfo)
CC_API_RENAMED (ACAPI_Element_GetCategoryValue, ACAPI_Category_GetCategoryValue)
CC_API_RENAMED (ACAPI_Element_GetCategoryValueDefault, ACAPI_Category_GetCategoryValueDefault)
CC_API_RENAMED (ACAPI_Element_GetConnectedElements, ACAPI_Grouping_GetConnectedElements)
CC_API_RENAMED (ACAPI_Element_Select, ACAPI_Selection_Select)
CC_API_RENAMED (ACAPI_Element_SetCategoryValue, ACAPI_Category_SetCategoryValue)
CC_API_RENAMED (ACAPI_Element_SetCategoryValueDefault, ACAPI_Category_SetCategoryValueDefault)
CC_API_RENAMED (ACAPI_Element_Tool, ACAPI_Grouping_Tool)
CC_API_RENAMED (ACAPI_Goodies_GetMarkerParent, ACAPI_LibraryPart_GetMarkerParent)
CC_API_RENAMED (ACAPI_Goodies_NeigIDToElemType, ACAPI_Element_NeigIDToElemType)
CC_API_RENAMED (ACAPI_Install_AddOnCommandHandler, ACAPI_AddOnAddOnCommunication_InstallAddOnCommandHandler)
CC_API_RENAMED (ACAPI_LibPart_AddSection, ACAPI_LibraryPart_AddSection)
CC_API_RENAMED (ACAPI_LibPart_Create, ACAPI_LibraryPart_Create)
CC_API_RENAMED (ACAPI_LibPart_EndSection, ACAPI_LibraryPart_EndSection)
CC_API_RENAMED (ACAPI_LibPart_Get, ACAPI_LibraryPart_Get)
CC_API_RENAMED (ACAPI_LibPart_GetDetails, ACAPI_LibraryPart_GetDetails)
CC_API_RENAMED (ACAPI_LibPart_GetNum, ACAPI_LibraryPart_GetNum)
CC_API_RENAMED (ACAPI_LibPart_GetParams, ACAPI_LibraryPart_GetParams)
CC_API_RENAMED (ACAPI_LibPart_GetSect_ParamDef, ACAPI_LibraryPart_GetSect_ParamDef)
CC_API_RENAMED (ACAPI_LibPart_GetSection, ACAPI_LibraryPart_GetSection)
CC_API_RENAMED (ACAPI_LibPart_GetSectionList, ACAPI_LibraryPart_GetSectionList)
CC_API_RENAMED (ACAPI_LibPart_NewSection, ACAPI_LibraryPart_NewSection)
CC_API_RENAMED (ACAPI_LibPart_Save, ACAPI_LibraryPart_Save)
CC_API_RENAMED (ACAPI_LibPart_Search, ACAPI_LibraryPart_Search)
CC_API_RENAMED (ACAPI_LibPart_SetDetails_ParamDef, ACAPI_LibraryPart_SetDetails_ParamDef)
CC_API_RENAMED (ACAPI_LibPart_WriteSection, ACAPI_LibraryPart_WriteSection)
CC_API_RENAMED (ACAPI_MarkUp_AddComment, ACAPI_Markup_AddComment)
CC_API_RENAMED (ACAPI_MarkUp_AttachElements, ACAPI_Markup_AttachElements)
CC_API_RENAMED (ACAPI_MarkUp_Create, ACAPI_Markup_Create)
CC_API_RENAMED (ACAPI_MarkUp_Delete, ACAPI_Markup_Delete)
CC_API_RENAMED (ACAPI_MarkUp_DetachElements, ACAPI_Markup_DetachElements)
CC_API_RENAMED (ACAPI_MarkUp_ExportToBCF, ACAPI_Markup_ExportToBCF)
CC_API_RENAMED (ACAPI_MarkUp_GetAttachedElements, ACAPI_Markup_GetAttachedElements)
CC_API_RENAMED (ACAPI_MarkUp_GetComments, ACAPI_Markup_GetComments)
CC_API_RENAMED (ACAPI_MarkUp_GetList, ACAPI_Markup_GetList)
CC_API_RENAMED (ACAPI_MarkUp_ImportFromBCF, ACAPI_Markup_ImportFromBCF)
CC_API_RENAMED (ACAPI_Override_GetOverrideCombination, ACAPI_GraphicalOverride_GetOverrideCombination)
CC_API_RENAMED (ACAPI_Override_GetOverrideCombinationList, ACAPI_GraphicalOverride_GetOverrideCombinationList)
CC_API_RENAMED (ACAPI_Element_ShapePrims, ACAPI_DrawingPrimitive_ShapePrims)
CC_API_RENAMED (ACAPI_TeamworkControl_FindLockableObjectSet, ACAPI_Teamwork_FindLockableObjectSet)
CC_API_RENAMED (ACAPI_TeamworkControl_GetHotlinkCacheManagementOwner, ACAPI_Teamwork_GetHotlinkCacheManagementOwner)
CC_API_RENAMED (ACAPI_TeamworkControl_GetLockableStatus, ACAPI_Teamwork_GetLockableStatus)
CC_API_RENAMED (ACAPI_TeamworkControl_GetTeamworkProjectDetails, ACAPI_Teamwork_GetTeamworkProjectDetails)
CC_API_RENAMED (ACAPI_TeamworkControl_GetUsernameFromId, ACAPI_Teamwork_GetUsernameFromId)
CC_API_RENAMED (ACAPI_TeamworkControl_HasConnection, ACAPI_Teamwork_HasConnection)
CC_API_RENAMED (ACAPI_TeamworkControl_HasCreateRight, ACAPI_Teamwork_HasCreateRight)
CC_API_RENAMED (ACAPI_TeamworkControl_HasDeleteModifyRight, ACAPI_Teamwork_HasDeleteModifyRight)
CC_API_RENAMED (ACAPI_TeamworkControl_IsOnline, ACAPI_Teamwork_IsOnline)
CC_API_RENAMED (ACAPI_TeamworkControl_ReceiveChanges, ACAPI_Teamwork_ReceiveChanges)
CC_API_RENAMED (ACAPI_TeamworkControl_ReleaseElements, ACAPI_Teamwork_ReleaseElements)
CC_API_RENAMED (ACAPI_TeamworkControl_ReleaseHotlinkCacheManagement, ACAPI_Teamwork_ReleaseHotlinkCacheManagement)
CC_API_RENAMED (ACAPI_TeamworkControl_ReleaseLockable, ACAPI_Teamwork_ReleaseLockable)
CC_API_RENAMED (ACAPI_TeamworkControl_ReserveElements, ACAPI_Teamwork_ReserveElements)
CC_API_RENAMED (ACAPI_TeamworkControl_ReserveHotlinkCacheManagement, ACAPI_Teamwork_ReserveHotlinkCacheManagement)
CC_API_RENAMED (ACAPI_TeamworkControl_ReserveLockable, ACAPI_Teamwork_ReserveLockable)
CC_API_RENAMED (ACAPI_TeamworkControl_SendChanges, ACAPI_Teamwork_SendChanges)

#undef CC_API_RENAMED

#endif

// --- Attribute indices --------------------------------------------------------------------------------
// Archicad 26: API_AttributeIndex is an integer. Archicad 27+: a class without arithmetic or ordering.
// Use these two instead of casts, so the code compiles on every version.

#if defined (CC_AC_VERSION) && CC_AC_VERSION >= 27
inline Int32				AttrIndexToInt (const API_AttributeIndex& index)	{ return index.ToInt32_Deprecated (); }
inline API_AttributeIndex	AttrIndexFromInt (Int32 index)						{ return ACAPI_CreateAttributeIndex (index); }
#else
inline Int32				AttrIndexToInt (API_AttributeIndex index)			{ return (Int32) index; }
inline API_AttributeIndex	AttrIndexFromInt (Int32 index)						{ return (API_AttributeIndex) index; }
#endif

#if defined (CC_AC_VERSION) && CC_AC_VERSION >= 27
// Comparisons the integer indices of Archicad 26 had (ordering for std::map / sorting, tests against plain numbers).
inline bool operator<  (const API_AttributeIndex& a, const API_AttributeIndex& b)	{ return a.ToInt32_Deprecated () <  b.ToInt32_Deprecated (); }
inline bool operator>  (const API_AttributeIndex& a, const API_AttributeIndex& b)	{ return a.ToInt32_Deprecated () >  b.ToInt32_Deprecated (); }
inline bool operator<= (const API_AttributeIndex& a, const API_AttributeIndex& b)	{ return a.ToInt32_Deprecated () <= b.ToInt32_Deprecated (); }
inline bool operator>= (const API_AttributeIndex& a, const API_AttributeIndex& b)	{ return a.ToInt32_Deprecated () >= b.ToInt32_Deprecated (); }
inline bool operator== (const API_AttributeIndex& a, Int32 b)						{ return a.ToInt32_Deprecated () == b; }
inline bool operator!= (const API_AttributeIndex& a, Int32 b)						{ return a.ToInt32_Deprecated () != b; }
inline bool operator<  (const API_AttributeIndex& a, Int32 b)						{ return a.ToInt32_Deprecated () <  b; }
inline bool operator>  (const API_AttributeIndex& a, Int32 b)						{ return a.ToInt32_Deprecated () >  b; }
inline bool operator<= (const API_AttributeIndex& a, Int32 b)						{ return a.ToInt32_Deprecated () <= b; }
inline bool operator>= (const API_AttributeIndex& a, Int32 b)						{ return a.ToInt32_Deprecated () >= b; }
#endif

// Ordering for std::map / std::set / sorting (Archicad 27+ indices have none).
struct AttrIndexLess {
	bool operator() (const API_AttributeIndex& a, const API_AttributeIndex& b) const { return AttrIndexToInt (a) < AttrIndexToInt (b); }
};

// --- Overridden attributes (surface overrides of elements) --------------------------------------------
// Archicad 26: struct {attributeIndex, overridden}. Archicad 27+: APIOptional<API_AttributeIndex> {value, hasValue}.

#if defined (CC_AC_VERSION) && CC_AC_VERSION >= 27
inline bool					IsOverridden (const API_OverriddenAttribute& attr)						{ return attr.hasValue; }
inline API_AttributeIndex	OverriddenIndex (const API_OverriddenAttribute& attr)					{ return attr.value; }
inline void					SetOverridden (API_OverriddenAttribute& attr, API_AttributeIndex index)	{ attr = index; }
inline void					ClearOverridden (API_OverriddenAttribute& attr)							{ attr = APINullValue; }
#else
inline bool					IsOverridden (const API_OverriddenAttribute& attr)						{ return attr.overridden; }
inline API_AttributeIndex	OverriddenIndex (const API_OverriddenAttribute& attr)					{ return attr.attributeIndex; }
inline void					SetOverridden (API_OverriddenAttribute& attr, API_AttributeIndex index)	{ attr.attributeIndex = index; attr.overridden = true; }
inline void					ClearOverridden (API_OverriddenAttribute& attr)							{ attr.overridden = false; }
#endif

// --- Misc renames ----------------------------------------------------------------------------------------

#if defined (CC_AC_VERSION) && CC_AC_VERSION >= 28
#define CC_CASE_INSENSITIVE	GS::CaseInsensitive
#define CC_CASE_SENSITIVE	GS::CaseSensitive
#else
#define CC_CASE_INSENSITIVE	GS::UniString::CaseInsensitive
#define CC_CASE_SENSITIVE	GS::UniString::CaseSensitive
#endif

// Archicad 27 dropped the calling-convention macros from the add-on entry points.
#if !defined (__ACDLL_CALL)
#define __ACDLL_CALL
#endif
#if !defined (__ACENV_CALL)
#define __ACENV_CALL
#endif

// --- Attribute types moved out of the attribute system -------------------------------------------------
// Archicad 27 moved pens (now items of the active pen table), fonts, dimension standards and model view options to
// their own functions. The connector keeps them as attribute types with the same names on every version; on 27+ these
// IDs lie outside API_AttrTypeID's range and AttrIndices / GetAttrHeader route them to the new functions.

#if defined (CC_AC_VERSION) && CC_AC_VERSION >= 27
constexpr API_AttrTypeID	API_PenID				= static_cast<API_AttrTypeID> (101);
constexpr API_AttrTypeID	API_FontID				= static_cast<API_AttrTypeID> (102);
constexpr API_AttrTypeID	API_DimStandID			= static_cast<API_AttrTypeID> (103);
constexpr API_AttrTypeID	API_ModelViewOptionsID	= static_cast<API_AttrTypeID> (104);
#endif

// True for the four types above on Archicad 27+ (they cannot go through ACAPI_Attribute_Get / Search / Create there).
bool			IsRelocatedAttrType (API_AttrTypeID typeID);

// Indices of the existing attributes of a type, ascending (pens: 1..255 of the active pen table). Indices can have
// gaps (deleted attributes) and on Archicad 27+ need not be small numbers, so never loop 1..count.
GS::Array<Int32>	AttrIndices (API_AttrTypeID typeID);

// Name and GUID of an attribute of any type above (either pointer may be null). False when it does not exist.
// Pens have no name and no GUID on Archicad 27+; name is then empty.
bool			GetAttrHeader (API_AttrTypeID typeID, API_AttributeIndex index, GS::UniString* name, API_Guid* guid);

// --- Cut fill pen overrides of model elements -------------------------------------------------------------
// Archicad 26: element.penOverride {cutFillPen, cutFillBackgroundPen, overrideCutFillPen, overrideCutFillBackgroundPen}.
// Archicad 27+: element.cutFillPen and element.cutFillBackgroundPen, each an APIOptional<API_PenIndex>.

struct CutFillPens {
#if defined (CC_AC_VERSION) && CC_AC_VERSION >= 27
	API_OverriddenPen*	cutFill;
	API_OverriddenPen*	background;
	bool	Has (bool bg) const				{ return (bg ? background : cutFill)->hasValue; }
	short	Pen (bool bg) const				{ return (bg ? background : cutFill)->value; }
	void	Set (bool bg, short pen) const	{ *(bg ? background : cutFill) = (API_PenIndex) pen; }
	void	Clear (bool bg) const			{ *(bg ? background : cutFill) = APINullValue; }
#else
	API_PenOverrideType* po;
	bool	Has (bool bg) const				{ return bg ? po->overrideCutFillBackgroundPen : po->overrideCutFillPen; }
	short	Pen (bool bg) const				{ return bg ? po->cutFillBackgroundPen : po->cutFillPen; }
	void	Set (bool bg, short pen) const	{ (bg ? po->cutFillBackgroundPen : po->cutFillPen) = pen; (bg ? po->overrideCutFillBackgroundPen : po->overrideCutFillPen) = true; }
	void	Clear (bool bg) const			{ (bg ? po->overrideCutFillBackgroundPen : po->overrideCutFillPen) = false; }
#endif
};

template <typename Elem>
CutFillPens CutFillPensOf (const Elem& elem)
{
	Elem& e = const_cast<Elem&> (elem);
#if defined (CC_AC_VERSION) && CC_AC_VERSION >= 27
	return CutFillPens { &e.cutFillPen, &e.cutFillBackgroundPen };
#else
	return CutFillPens { &e.penOverride };
#endif
}

// Element masks: ACAPI_ELEMENT_MASK_SET marks only the first byte of a field, so every sub-field is named.
// SET is a one-argument macro such as COL_SET (field); fn receives each field by reference (byte-range masks).
#if defined (CC_AC_VERSION) && CC_AC_VERSION >= 27
#define CC_MASK_CUT_FILL_PENS(SET)	SET (cutFillPen.hasValue); SET (cutFillPen.value); SET (cutFillBackgroundPen.hasValue); SET (cutFillBackgroundPen.value)
template <typename Elem, typename Fn> void ForCutFillPenFields (Elem& elem, Fn&& fn) { fn (elem.cutFillPen); fn (elem.cutFillBackgroundPen); }
#else
#define CC_MASK_CUT_FILL_PENS(SET)	SET (penOverride.cutFillPen); SET (penOverride.cutFillBackgroundPen); SET (penOverride.overrideCutFillPen); SET (penOverride.overrideCutFillBackgroundPen)
template <typename Elem, typename Fn> void ForCutFillPenFields (Elem& elem, Fn&& fn) { fn (elem.penOverride); }
#endif

// --- GS::HashTable iteration ---------------------------------------------------------------------------------
// Archicad 26: the iterated pairs hold pointers (pair.key, pair.value); 27+: references. These give a
// reference on both (HashKeyOf / HashValueOf).
template <typename T> inline T& CcDeref (T* p)	{ return *p; }
template <typename T> inline T& CcDeref (T& r)	{ return r; }
template <typename Pair> inline auto& HashKeyOf (Pair&& pair)		{ return CcDeref (pair.key); }
template <typename Pair> inline auto& HashValueOf (Pair&& pair)	{ return CcDeref (pair.value); }

// --- IFC global ids in API_IFCRelationshipData -------------------------------------------------------------------
// Archicad 26-27: API_Guid; 28+: GS::UniString (the IFC GlobalId text).
#if defined (CC_AC_VERSION) && CC_AC_VERSION >= 28
using IfcRelationshipKey = GS::UniString;
#else
using IfcRelationshipKey = API_Guid;
#endif

// --- Attribute indices of the GS drawing modules (ProfileVectorImage, ...) ------------------------------------
// Archicad 26: plain integers. Archicad 27+: ADB::AttributeIndex, converted with API2AC_AttrIndex.
#if defined (CC_AC_VERSION) && CC_AC_VERSION >= 27
#include "API_AttributeIndexUtils.hpp"
inline ADB::AttributeIndex	ToModuleAttrIndex (const API_AttributeIndex& index)	{ return API2AC_AttrIndex (index); }
#else
inline API_AttributeIndex	ToModuleAttrIndex (API_AttributeIndex index)			{ return index; }
#endif

// --- Model view option combinations --------------------------------------------------------------------------
// Archicad 26: attributes (API_Attribute::modelViewOpt); 27+: ACAPI_Navigator_ModelViewOptions_Get.
// Fills `options`; the caller frees options.gdlOptions with ACAPI_FreeGDLModelViewOptionsPtr. False when missing.
bool			GetModelViewOptions (API_AttributeIndex index, API_ModelViewOptions& options, GS::UniString* name, API_Guid* guid);
