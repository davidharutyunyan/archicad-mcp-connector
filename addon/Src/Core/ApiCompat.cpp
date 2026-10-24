// ApiCompat.cpp — the Archicad 26 dispatcher functions on Archicad 27 and newer (see ApiCompat.hpp).
// The cases were generated from the operation codes the add-on uses and the Archicad 29 function declarations:
// pointer parameters pass through, value parameters (old style: packed into a void*) are unpacked.

#include "Core/ApiCompat.hpp"

#include <algorithm>
#include <vector>

#if defined (CC_AC_VERSION) && CC_AC_VERSION >= 27

GSErrCode ACAPI_Environment (API_EnvironmentID code, void* par1, void* par2, void* par3)
{
	switch (code) {
		case APIEnv_CanChangePlaceSetsID:
			return ACAPI_GeoLocation_CanChangePlaceSets (reinterpret_cast<bool*> (par1));
		case APIEnv_Change3DCuttingPlanesID:
			return ACAPI_View_Change3DCuttingPlanes (reinterpret_cast<API_3DCutPlanesInfo*> (par1));
		case APIEnv_Change3DImageSetsID:
			return ACAPI_View_Change3DImageSets (reinterpret_cast<API_3DFilterAndCutSettings*> (par1), reinterpret_cast<bool*> (par2));
		case APIEnv_Change3DProjectionSetsID:
			return ACAPI_View_Change3DProjectionSets (reinterpret_cast<API_3DProjectionInfo*> (par1), reinterpret_cast<bool*> (par2));
		case APIEnv_Change3DStyleID:
			return ACAPI_View_Change3DStyle (reinterpret_cast<API_3DStyle*> (par1));
		case APIEnv_Change3DWindowSetsID:
			return ACAPI_View_Change3DWindowSets (reinterpret_cast<API_3DWindowInfo*> (par1));
		case APIEnv_ChangeAutoIntersectID:
			return ACAPI_View_ChangeAutoIntersect (reinterpret_cast<bool*> (par1));
		case APIEnv_ChangeCurrLayerCombID:
			return ACAPI_Navigator_ChangeCurrLayerComb (reinterpret_cast<API_AttributeIndex*> (par1));
		case APIEnv_ChangePlaceSetsID:
			return ACAPI_GeoLocation_ChangePlaceSets (reinterpret_cast<API_PlaceInfo*> (par1));
		case APIEnv_ChangeRenderingSetsID:
			return ACAPI_Rendering_ChangeRenderingSets (reinterpret_cast<void*> (par1), static_cast<API_RendSetTypeID> (reinterpret_cast<GS::IntPtr> (par2)));
		case APIEnv_ChangeStorySettingsID:
			return ACAPI_ProjectSetting_ChangeStorySettings (reinterpret_cast<API_StoryCmdType*> (par1));
		case APIEnv_ChangeStructureDisplayID:
			return ACAPI_Navigator_ChangeStructureDisplay (reinterpret_cast<API_UIStructureDisplay*> (par1));
		case APIEnv_ChangeViewOptionsID:
			return ACAPI_Navigator_ChangeViewOptions (reinterpret_cast<API_ViewOptions*> (par1), reinterpret_cast<short*> (par2));
		case APIEnv_Get3DCuttingPlanesID:
			return ACAPI_View_Get3DCuttingPlanes (reinterpret_cast<API_3DCutPlanesInfo*> (par1));
		case APIEnv_Get3DImageSetsID:
			return ACAPI_View_Get3DImageSets (reinterpret_cast<API_3DFilterAndCutSettings*> (par1));
		case APIEnv_Get3DProjectionSetsID:
			return ACAPI_View_Get3DProjectionSets (reinterpret_cast<API_3DProjectionInfo*> (par1));
		case APIEnv_Get3DStyleID:
			return ACAPI_View_Get3DStyle (reinterpret_cast<API_3DStyle*> (par1));
		case APIEnv_Get3DStyleListID:
			return ACAPI_View_Get3DStyleList (reinterpret_cast<GS::Array<GS::UniString>*> (par1), reinterpret_cast<GS::UniString*> (par2));
		case APIEnv_Get3DWindowSetsID:
			return ACAPI_View_Get3DWindowSets (reinterpret_cast<API_3DWindowInfo*> (par1));
		case APIEnv_GetCurrLayerCombID:
			return ACAPI_Navigator_GetCurrLayerComb (reinterpret_cast<API_AttributeIndex*> (par1));
		case APIEnv_GetCurrPenSetID:
			return ACAPI_Attribute_GetCurrPenSet (reinterpret_cast<API_AttributeIndex*> (par1));
		case APIEnv_GetExportToleranceID:
			return ACAPI_ProjectSetting_GetExportTolerance (reinterpret_cast<double*> (par1));
		case APIEnv_GetGeoLocationID:
			return ACAPI_GeoLocation_GetGeoLocation (reinterpret_cast<API_GeoLocation*> (par1));
		case APIEnv_GetGhostStorySettingsID:
			return ACAPI_View_GetGhostStorySettings (reinterpret_cast<API_GhostStoryType*> (par1));
		case APIEnv_GetLayoutSetsID:
			return ACAPI_Navigator_GetLayoutSets (reinterpret_cast<API_LayoutInfo*> (par1), reinterpret_cast<API_DatabaseUnId*> (par2), reinterpret_cast<const ULong*> (par3));
		case APIEnv_GetLibrariesID:
			return ACAPI_LibraryManagement_GetLibraries (reinterpret_cast<GS::Array<API_LibraryInfo>*> (par1), reinterpret_cast<Int32*> (par2));
		case APIEnv_GetMiscAppInfoID:
			return ACAPI_ProjectOperation_GetMiscAppInfo (reinterpret_cast<API_MiscAppInfo*> (par1));
		case APIEnv_GetPlaceSetsID:
			return ACAPI_GeoLocation_GetPlaceSets (reinterpret_cast<API_PlaceInfo*> (par1));
		case APIEnv_GetPreferencesID:
			return ACAPI_ProjectSetting_GetPreferences (reinterpret_cast<void*> (par1), static_cast<API_PrefsTypeID> (reinterpret_cast<GS::IntPtr> (par2)));
		case APIEnv_GetRenderingSceneNamesID:
			return ACAPI_Rendering_GetRenderingSceneNames (reinterpret_cast<GS::Array<GS::UniString>*> (par1));
		case APIEnv_GetRenderingSetsID:
			return ACAPI_Rendering_GetRenderingSets (reinterpret_cast<void*> (par1), static_cast<API_RendSetTypeID> (reinterpret_cast<GS::IntPtr> (par2)), reinterpret_cast<const GS::UniString*> (par3));
		case APIEnv_GetSpecFolderID:
			return ACAPI_ProjectSettings_GetSpecFolder (reinterpret_cast<API_SpecFolderID*> (par1), reinterpret_cast<IO::Location*> (par2));
		case APIEnv_GetStorySettingsID:
			return ACAPI_ProjectSetting_GetStorySettings (reinterpret_cast<API_StoryInfo*> (par1), static_cast<UInt32> (reinterpret_cast<GS::IntPtr> (par2)));
		case APIEnv_GetStructureDisplayID:
			return ACAPI_Navigator_GetStructureDisplay (reinterpret_cast<API_UIStructureDisplay*> (par1));
		case APIEnv_GetSurveyPointTransformationID:
			return ACAPI_SurveyPoint_GetSurveyPointTransformation (reinterpret_cast<API_Tranmat*> (par1));
		case APIEnv_GetTWAccessRightID:
			return ACAPI_Teamwork_GetTWAccessRight (static_cast<API_TWAccessRights> (reinterpret_cast<GS::IntPtr> (par1)), reinterpret_cast<bool*> (par2));
		case APIEnv_GetToolBoxInfoID:
			return ACAPI_Toolbox_GetToolBoxInfo (reinterpret_cast<API_ToolBoxInfo*> (par1), static_cast<bool> (reinterpret_cast<GS::IntPtr> (par2)));
		case APIEnv_GetViewOptionsID:
			return ACAPI_Navigator_GetViewOptions (reinterpret_cast<API_ViewOptions*> (par1), reinterpret_cast<short*> (par2));
		case APIEnv_IsAutoGroupOnID:
			return ACAPI_View_IsAutoGroupOn (reinterpret_cast<bool*> (par1));
		case APIEnv_IsAutoIntersectOnID:
			return ACAPI_View_IsAutoIntersectOn (reinterpret_cast<bool*> (par1));
		case APIEnv_IsSurveyPointLockedID:
			return ACAPI_SurveyPoint_IsSurveyPointLocked (reinterpret_cast<bool*> (par1));
		case APIEnv_IsSurveyPointVisibleID:
			return ACAPI_SurveyPoint_IsSurveyPointVisible (reinterpret_cast<bool*> (par1));
		case APIEnv_IsSuspendGroupOnID:
			return ACAPI_View_IsSuspendGroupOn (reinterpret_cast<bool*> (par1));
		case APIEnv_OverwriteLibPartID:
			return ACAPI_LibraryManagement_OverwriteLibPart (static_cast<bool> (reinterpret_cast<GS::IntPtr> (par1)));
		case APIEnv_ProjectID:
			return ACAPI_ProjectOperation_Project (reinterpret_cast<API_ProjectInfo*> (par1));
		case APIEnv_ProjectSharingID:
			return ACAPI_Teamwork_ProjectSharing (reinterpret_cast<API_SharingInfo*> (par1));
		case APIEnv_SetCurrent3DStyleID:
			return ACAPI_View_SetCurrent3DStyle (reinterpret_cast<const GS::UniString*> (par1));
		case APIEnv_SetCurrentRenderingSceneID:
			return ACAPI_Rendering_SetCurrentRenderingScene (reinterpret_cast<const GS::UniString*> (par1));
		case APIEnv_SetGeoLocationID:
			return ACAPI_GeoLocation_SetGeoLocation (reinterpret_cast<API_GeoLocation*> (par1));
		case APIEnv_SetLibrariesID:
			return ACAPI_LibraryManagement_SetLibraries (reinterpret_cast<const GS::Array<API_LibraryInfo>*> (par1));
		case APIEnv_SetSurveyPointLockedID:
			return ACAPI_SurveyPoint_SetSurveyPointLocked (reinterpret_cast<bool*> (par1));
		case APIEnv_SetSurveyPointVisibilityID:
			return ACAPI_SurveyPoint_SetSurveyPointVisibility (reinterpret_cast<bool*> (par1));
		default:
			return APIERR_NOTSUPPORTED;
	}
}


GSErrCode ACAPI_Database (API_DatabaseID code, void* par1, void* par2, void* par3)
{
	switch (code) {
		case APIDb_BreakHotlinkNodeID:
			return ACAPI_Hotlink_BreakHotlinkNode (reinterpret_cast<const API_Guid*> (par1));
		case APIDb_CalcBoundsID:
			return ACAPI_Element_CalcBounds (reinterpret_cast<const API_Elem_Head*> (par1), reinterpret_cast<API_Box3D*> (par2));
		case APIDb_ChangeCurrentDatabaseID:
			return ACAPI_Database_ChangeCurrentDatabase (reinterpret_cast<API_DatabaseInfo*> (par1));
		case APIDb_ChangeElementInfoStringID:
			return ACAPI_Element_ChangeElementInfoString (reinterpret_cast<const API_Guid*> (par1), reinterpret_cast<const GS::UniString*> (par2));
		case APIDb_CheckDrawingStatusID:
			return ACAPI_Drawing_CheckDrawingStatus (reinterpret_cast<API_Guid*> (par1), reinterpret_cast<Int32*> (par2));
		case APIDb_CreateHotlinkNodeID:
			return ACAPI_Hotlink_CreateHotlinkNode (reinterpret_cast<API_HotlinkNode*> (par1));
		case APIDb_DeleteHotlinkNodeID:
			return ACAPI_Hotlink_DeleteHotlinkNode (reinterpret_cast<const API_Guid*> (par1));
		case APIDb_DrawOrderGetNextID:
			return ACAPI_DrawOrder_DrawOrderGetNext (static_cast<API_ElemFilterFlags> (reinterpret_cast<GS::IntPtr> (par1)), reinterpret_cast<API_Elem_Head*> (par2));
		case APIDb_DrawOrderInitID:
			return ACAPI_DrawOrder_DrawOrderInit ();
		case APIDb_DrawOrderTermID:
			return ACAPI_DrawOrder_DrawOrderTerm ();
		case APIDb_GetActualRenovationFilterID:
			return ACAPI_Renovation_GetActualRenovationFilter (reinterpret_cast<API_Guid*> (par1));
		case APIDb_GetContainingDatabaseID:
			return ACAPI_Database_GetContainingDatabase (reinterpret_cast<const API_Guid*> (par1), reinterpret_cast<API_DatabaseInfo*> (par2));
		case APIDb_GetCurrentDatabaseID:
			return ACAPI_Database_GetCurrentDatabase (reinterpret_cast<API_DatabaseInfo*> (par1));
		case APIDb_GetCurrentWindowID:
			return ACAPI_Window_GetCurrentWindow (reinterpret_cast<API_WindowInfo*> (par1));
		case APIDb_GetDatabaseInfoID:
			return ACAPI_Window_GetDatabaseInfo (reinterpret_cast<API_DatabaseInfo*> (par1), reinterpret_cast<API_DatabaseUnId*> (par2));
		case APIDb_GetDetailDatabasesID:
			return ACAPI_Database_GetDetailDatabases (reinterpret_cast<API_DatabaseUnId**> (par1), reinterpret_cast<GS::Array<API_DatabaseUnId>*> (par2));
		case APIDb_GetDocumentFrom3DDatabasesID:
			return ACAPI_Database_GetDocumentFrom3DDatabases (reinterpret_cast<API_DatabaseUnId**> (par1), reinterpret_cast<GS::Array<API_DatabaseUnId>*> (par2));
		case APIDb_GetDrawingLinkID:
			return ACAPI_Drawing_GetDrawingLink (reinterpret_cast<const API_Guid*> (par1), reinterpret_cast<API_DrawingLinkInfo*> (par2));
		case APIDb_GetDrawingScaleID:
			return ACAPI_Drawing_GetDrawingScale (reinterpret_cast<double*> (par1));
		case APIDb_GetElementCategoriesID:
			return ACAPI_Category_GetElementCategories (reinterpret_cast<GS::Array<API_ElemCategory>*> (par1));
		case APIDb_GetElementCategoryValuesID:
			return ACAPI_Category_GetElementCategoryValues (reinterpret_cast<const API_ElemCategory*> (par1), reinterpret_cast<GS::Array<API_ElemCategoryValue>*> (par2));
		case APIDb_GetElementFromAnywhereID:
			return ACAPI_Element_GetElementFromAnywhere (reinterpret_cast<const API_Guid*> (par1), reinterpret_cast<API_Element*> (par2));
		case APIDb_GetElementInfoStringID:
			return ACAPI_Element_GetElementInfoString (reinterpret_cast<const API_Guid*> (par1), reinterpret_cast<GS::UniString*> (par2));
		case APIDb_GetElevationDatabasesID:
			return ACAPI_Database_GetElevationDatabases (reinterpret_cast<API_DatabaseUnId**> (par1), reinterpret_cast<GS::Array<API_DatabaseUnId>*> (par2));
		case APIDb_GetExtent3DID:
			return ACAPI_Element_GetExtent3D (reinterpret_cast<API_Box3D*> (par1));
		case APIDb_GetExtentID:
			return ACAPI_Element_GetExtent (reinterpret_cast<API_Box*> (par1), reinterpret_cast<API_DatabaseUnId*> (par2));
		case APIDb_GetHotlinkInstancesID:
			return ACAPI_Hotlink_GetHotlinkInstances (reinterpret_cast<const API_Guid*> (par1), reinterpret_cast<GS::Array<API_Guid>*> (par2));
		case APIDb_GetHotlinkNodeID:
			return ACAPI_Hotlink_GetHotlinkNode (reinterpret_cast<API_HotlinkNode*> (par1), reinterpret_cast<bool*> (par2));
		case APIDb_GetHotlinkNodeTreeID:
			return ACAPI_Hotlink_GetHotlinkNodeTree (reinterpret_cast<const API_Guid*> (par1), reinterpret_cast<GS::HashTable<API_Guid, GS::Array<API_Guid>>*> (par2));
		case APIDb_GetHotlinkNodesID:
			return ACAPI_Hotlink_GetHotlinkNodes (reinterpret_cast<const API_HotlinkTypeID*> (par1), reinterpret_cast<GS::Array<API_Guid>*> (par2), reinterpret_cast<bool*> (par3));
		case APIDb_GetHotlinkProxyElementTableID:
			return ACAPI_Hotlink_GetHotlinkProxyElementTable (reinterpret_cast<const API_Guid*> (par1), reinterpret_cast<GS::HashTable<API_Guid, API_Guid>*> (par2));
		case APIDb_GetHotlinkRootNodeGuidID:
			return ACAPI_Hotlink_GetHotlinkRootNodeGuid (reinterpret_cast<const API_HotlinkTypeID*> (par1), reinterpret_cast<API_Guid*> (par2));
		case APIDb_GetHotlinkSourceStatusID:
			return ACAPI_Hotlink_GetHotlinkSourceStatus (reinterpret_cast<const IO::Location*> (par1), reinterpret_cast<API_HotlinkSourceStatus*> (par2));
		case APIDb_GetInteriorElevationDatabasesID:
			return ACAPI_Database_GetInteriorElevationDatabases (reinterpret_cast<API_DatabaseUnId**> (par1), reinterpret_cast<GS::Array<API_DatabaseUnId>*> (par2));
		case APIDb_GetLayoutDatabasesID:
			return ACAPI_Database_GetLayoutDatabases (reinterpret_cast<API_DatabaseUnId**> (par1), reinterpret_cast<GS::Array<API_DatabaseUnId>*> (par2));
		case APIDb_GetMasterLayoutDatabasesID:
			return ACAPI_Database_GetMasterLayoutDatabases (reinterpret_cast<API_DatabaseUnId**> (par1), reinterpret_cast<GS::Array<API_DatabaseUnId>*> (par2));
		case APIDb_GetOffsetID:
			return ACAPI_ProjectSetting_GetOffset (reinterpret_cast<API_Coord*> (par1));
		case APIDb_GetRVMChangeCustomSchemeID:
			return ACAPI_Revision_GetRVMChangeCustomScheme (reinterpret_cast<GS::HashTable<API_Guid, GS::UniString>*> (par1));
		case APIDb_GetRVMChangeFirstIssueID:
			return ACAPI_Revision_GetRVMChangeFirstIssue (reinterpret_cast<const GS::UniString*> (par1), reinterpret_cast<API_RVMIssue*> (par2));
		case APIDb_GetRVMChangesFromChangeIdsID:
			return ACAPI_Revision_GetRVMChangesFromChangeIds (reinterpret_cast<const GS::Array<GS::UniString>*> (par1), reinterpret_cast<GS::Array<API_RVMChange>*> (par2));
		case APIDb_GetRVMChangesID:
			return ACAPI_Revision_GetRVMChanges (reinterpret_cast<GS::Array<API_RVMChange>*> (par1));
		case APIDb_GetRVMDocumentRevisionChangesID:
			return ACAPI_Revision_GetRVMDocumentRevisionChanges (reinterpret_cast<const API_Guid*> (par1), reinterpret_cast<GS::Array<API_RVMChange>*> (par2));
		case APIDb_GetRVMDocumentRevisionsID:
			return ACAPI_Revision_GetRVMDocumentRevisions (reinterpret_cast<GS::Array<API_RVMDocumentRevision>*> (par1));
		case APIDb_GetRVMElemChangeIdsID:
			return ACAPI_Revision_GetRVMElemChangeIds (reinterpret_cast<const API_Guid*> (par1), reinterpret_cast<GS::Array<GS::UniString>*> (par2));
		case APIDb_GetRVMIssueCustomSchemeID:
			return ACAPI_Revision_GetRVMIssueCustomScheme (reinterpret_cast<GS::HashTable<API_Guid, GS::UniString>*> (par1));
		case APIDb_GetRVMIssueDocumentRevisionsID:
			return ACAPI_Revision_GetRVMIssueDocumentRevisions (reinterpret_cast<const API_Guid*> (par1), reinterpret_cast<GS::Array<API_RVMDocumentRevision>*> (par2));
		case APIDb_GetRVMIssuesID:
			return ACAPI_Revision_GetRVMIssues (reinterpret_cast<GS::Array<API_RVMIssue>*> (par1));
		case APIDb_GetRVMLayoutCurrentRevisionChangesID:
			return ACAPI_Revision_GetRVMLayoutCurrentRevisionChanges (reinterpret_cast<const API_DatabaseUnId*> (par1), reinterpret_cast<GS::Array<API_RVMChange>*> (par2));
		case APIDb_GetRenovationFiltersID:
			return ACAPI_Renovation_GetRenovationFilters (reinterpret_cast<GS::Array<API_Guid>*> (par1));
		case APIDb_GetSectionDatabasesID:
			return ACAPI_Database_GetSectionDatabases (reinterpret_cast<API_DatabaseUnId**> (par1), reinterpret_cast<GS::Array<API_DatabaseUnId>*> (par2));
		case APIDb_GetWorksheetDatabasesID:
			return ACAPI_Database_GetWorksheetDatabases (reinterpret_cast<API_DatabaseUnId**> (par1), reinterpret_cast<GS::Array<API_DatabaseUnId>*> (par2));
		case APIDb_GetZoomID:
			return ACAPI_View_GetZoom (reinterpret_cast<API_Box*> (par1), reinterpret_cast<API_Tranmat*> (par2));
		case APIDb_IsCWPanelDegenerateID:
			return ACAPI_CurtainWall_IsCWPanelDegenerate (reinterpret_cast<const API_Guid*> (par1), reinterpret_cast<bool*> (par2));
		case APIDb_ModifyHotlinkNodeID:
			return ACAPI_Hotlink_ModifyHotlinkNode (reinterpret_cast<API_HotlinkNode*> (par1));
		case APIDb_ReSetZoomID:
			return ACAPI_View_ResetZoom (reinterpret_cast<short*> (par1));
		case APIDb_RebuildCurrentDatabaseID:
			return ACAPI_Database_RebuildCurrentDatabase ();
		case APIDb_RoomReductionsID:
			return ACAPI_Element_RoomReductions (reinterpret_cast<const API_Guid*> (par1), reinterpret_cast<RoomReductionPolyProc*> (par2));
		case APIDb_SetActualRenovationFilterID:
			return ACAPI_Renovation_SetActualRenovationFilter (reinterpret_cast<const API_Guid*> (par1));
		case APIDb_SetZoomID:
			return ACAPI_View_SetZoom (reinterpret_cast<API_Box*> (par1), reinterpret_cast<API_Tranmat*> (par2));
		case APIDb_UpdateHotlinkCacheID:
			return ACAPI_Hotlink_UpdateHotlinkCache (reinterpret_cast<const API_Guid*> (par1), reinterpret_cast<API_HotlinkCacheGenerator*> (par2));
		case APIDb_ChangeDrawingScaleID:	// par2 was an optional bool*, now a bool
			return ACAPI_Drawing_ChangeDrawingScale (reinterpret_cast<double*> (par1), par2 != nullptr && *reinterpret_cast<bool*> (par2));
		default:
			return APIERR_NOTSUPPORTED;
	}
}


GSErrCode ACAPI_Goodies (API_GoodiesID code, void* par1, void* par2, void* par3, void* par4)
{
	switch (code) {
		case APIAny_CalcSunOnPlaceID:
			return ACAPI_GeoLocation_CalcSunOnPlace (reinterpret_cast<API_PlaceInfo*> (par1));
		case APIAny_ChangeAParameterID:
			return ACAPI_LibraryPart_ChangeAParameter (reinterpret_cast<API_ChangeParamType*> (par1));
		case APIAny_ChangeAutoTextFlagID:
			return ACAPI_AutoText_ChangeAutoTextFlag (reinterpret_cast<bool*> (par1));
		case APIAny_CheckLibPartSubtypeOfbyMainID:
			return ACAPI_LibraryPart_CheckLibPartSubtypeOfbyMain (reinterpret_cast<const char*> (par1), reinterpret_cast<const char*> (par2));
		case APIAny_CheckPropertyExpressionStringID:
			return ACAPI_Property_CheckPropertyExpressionString (reinterpret_cast<GS::UniString*> (par1));
		case APIAny_CloseParametersID:
			return ACAPI_LibraryPart_CloseParameters ();
		case APIAny_CreateAnAutoTextID:
			return ACAPI_AutoText_CreateAnAutoText (reinterpret_cast<const API_Guid*> (par1), reinterpret_cast<const char*> (par2));
		case APIAny_DeleteAnAutoTextID:
			return ACAPI_AutoText_DeleteAnAutoText (reinterpret_cast<const char*> (par1));
		case APIAny_DeletePolyNodeID:
			return ACAPI_Polygon_DeletePolyNode (reinterpret_cast<API_ElementMemo*> (par1), reinterpret_cast<Int32*> (par2));
		case APIAny_DeleteSubPolyID:
			return ACAPI_Polygon_DeleteSubPoly (reinterpret_cast<API_ElementMemo*> (par1), reinterpret_cast<Int32*> (par2));
		case APIAny_DisposeRegularizedPolyID:
			return ACAPI_Polygon_DisposeRegularizedPoly (reinterpret_cast<API_RegularizedPoly*> (par1));
		case APIAny_Elem2UIPriorityID:
			return ACAPI_Element_Elem2UIPriority (reinterpret_cast<GS::Int32*> (par1), reinterpret_cast<GS::Int32*> (par2));
		case APIAny_GetActParametersID:
			return ACAPI_LibraryPart_GetActParameters (reinterpret_cast<API_GetParamsType*> (par1));
		case APIAny_GetAutoTextFlagID:
			return ACAPI_AutoText_GetAutoTextFlag (reinterpret_cast<bool*> (par1));
		case APIAny_GetAutoTextsID:
			return ACAPI_AutoText_GetAutoTexts (reinterpret_cast<GS::Array<GS::ArrayFB<GS::UniString, 3>>*> (par1), static_cast<API_AutotextType> (reinterpret_cast<GS::IntPtr> (par2)));
		case APIAny_GetElemLibPartUnIdID:
			return ACAPI_LibraryPart_GetElemLibPartUnId (reinterpret_cast<API_Elem_Head*> (par1), reinterpret_cast<char*> (par2));
		case APIAny_GetHttpConnectionPortID:
			return ACAPI_Command_GetHttpConnectionPort (reinterpret_cast<UShort*> (par1));
		case APIAny_GetLibPartToolVariationID:
			return ACAPI_LibraryPart_GetLibPartToolVariation (reinterpret_cast<API_LibPart*> (par1), reinterpret_cast<API_ToolBoxItem*> (par2));
		case APIAny_GetParamValuesID:
			return ACAPI_LibraryPart_GetParamValues (reinterpret_cast<API_GetParamValuesType*> (par1));
		case APIAny_GetPropertyExprReferenceStringID:
			return ACAPI_Property_GetPropertyExprReferenceString (reinterpret_cast<API_PropertyDefinition*> (par1), reinterpret_cast<GS::UniString*> (par2));
		case APIAny_GetRenovationFilterNameID:
			return ACAPI_Renovation_GetRenovationFilterName (reinterpret_cast<const API_Guid*> (par1), reinterpret_cast<GS::UniString*> (par2));
		case APIAny_InsertPolyNodeID:
			return ACAPI_Polygon_InsertPolyNode (reinterpret_cast<API_ElementMemo*> (par1), reinterpret_cast<Int32*> (par2), reinterpret_cast<API_Coord*> (par3));
		case APIAny_InsertSubPolyID:
			return ACAPI_Polygon_InsertSubPoly (reinterpret_cast<API_ElementMemo*> (par1), reinterpret_cast<API_ElementMemo*> (par2));
		case APIAny_NeigToCoordID:
			return ACAPI_Element_NeigToCoord (reinterpret_cast<API_Neig*> (par1), reinterpret_cast<API_Coord3D*> (par2), reinterpret_cast<API_Coord3D*> (par3));
		case APIAny_OpenParametersID:
			return ACAPI_LibraryPart_OpenParameters (reinterpret_cast<API_ParamOwnerType*> (par1));
		case APIAny_RegularizePolygonID:
			return ACAPI_Polygon_RegularizePolygon (reinterpret_cast<const API_RegularizedPoly*> (par1), reinterpret_cast<Int32*> (par2), reinterpret_cast<API_RegularizedPoly***> (par3));
		case APIAny_SetAnAutoTextID:
			return ACAPI_AutoText_SetAnAutoText (reinterpret_cast<const GS::UniString*> (par1), reinterpret_cast<const GS::UniString*> (par2));
		case APIAny_SetMasterLayoutOnLayoutID:
			return ACAPI_Navigator_SetMasterLayoutOnLayout (reinterpret_cast<const API_DatabaseUnId*> (par1));
		case APIAny_UI2ElemPriorityID:
			return ACAPI_Element_UI2ElemPriority (reinterpret_cast<GS::Int32*> (par1), reinterpret_cast<GS::Int32*> (par2));
		default:
			return APIERR_NOTSUPPORTED;
	}
}


GSErrCode ACAPI_Automate (API_AutomateID code, void* par1, void* par2, void* par3)
{
	switch (code) {
		case APIDo_ChangeWindowID:
			return ACAPI_Window_ChangeWindow (reinterpret_cast<const API_WindowInfo*> (par1));
		case APIDo_CloseID:
			return ACAPI_ProjectOperation_Close ();
		case APIDo_GoToViewID:
			return ACAPI_View_GoToView (reinterpret_cast<const char*> (par1));
		case APIDo_NewProjectID:
			return ACAPI_ProjectOperation_NewProject (reinterpret_cast<const API_NewProjectPars*> (par1));
		case APIDo_OpenID:
			return ACAPI_ProjectOperation_Open (reinterpret_cast<const API_FileOpenPars*> (par1));
		case APIDo_PhotoRenderID:
			return ACAPI_Rendering_PhotoRender (reinterpret_cast<const API_PhotoRenderPars*> (par1), reinterpret_cast<const API_PrintPars*> (par2));
		case APIDo_PublishID:
			return ACAPI_ProjectOperation_Publish (reinterpret_cast<const API_PublishPars*> (par1), reinterpret_cast<const GS::Array<API_Guid>*> (par2));
		case APIDo_QuitID:
#if CC_AC_VERSION >= 28
			return ACAPI_ProjectOperation_Quit ();
#else
			return ACAPI_ProjectOperation_Quit (static_cast<Int32> (reinterpret_cast<GS::IntPtr> (par1)));
#endif
		case APIDo_RebuildID:
			return ACAPI_View_Rebuild (reinterpret_cast<bool*> (par1));
		case APIDo_RedrawID:
			return ACAPI_View_Redraw ();
		case APIDo_ReloadLibrariesID:
			return ACAPI_ProjectOperation_ReloadLibraries ();
		case APIDo_ShowAllIn3DID:
			return ACAPI_View_ShowAllIn3D ();
		case APIDo_ShowSelectionIn3DID:
			return ACAPI_View_ShowSelectionIn3D ();
		case APIDo_ZoomID:
			return ACAPI_View_Zoom (reinterpret_cast<API_Box*> (par1), reinterpret_cast<API_Rect*> (par2), reinterpret_cast<API_Tranmat*> (par3));
		case APIDo_ZoomToElementsID:
			return ACAPI_View_ZoomToElements (reinterpret_cast<const GS::Array<API_Guid>*> (par1));
		case APIDo_ZoomToSelectedID:
			return ACAPI_View_ZoomToSelected ();
		case APIDo_SaveID: {
			// The format parameters (par2) now have one overload per type: pick it from the file type.
			const API_FileSavePars* fsp = reinterpret_cast<const API_FileSavePars*> (par1);
			if (fsp == nullptr)
				return ACAPI_ProjectOperation_Save ();
			if (par2 == nullptr)
				return ACAPI_ProjectOperation_Save (fsp);
			switch (fsp->fileTypeID) {
				case APIFType_IfcFile:		return ACAPI_ProjectOperation_Save (fsp, reinterpret_cast<const API_SavePars_Ifc*> (par2));
				case APIFType_PdfFile:		return ACAPI_ProjectOperation_Save (fsp, reinterpret_cast<const API_SavePars_Pdf*> (par2));
				case APIFType_ObjectFile:	return ACAPI_ProjectOperation_Save (fsp, reinterpret_cast<const API_SavePars_Object*> (par2));
				case APIFType_PNGFile:
				case APIFType_BMPFile:
				case APIFType_TIFFFile:
				case APIFType_JPEGFile:
				case APIFType_GIFFile:
				case APIFType_EMFFile:
				case APIFType_WMFFile:		return ACAPI_ProjectOperation_Save (fsp, reinterpret_cast<const API_SavePars_Picture*> (par2));
				default:					return ACAPI_ProjectOperation_Save (fsp, reinterpret_cast<const API_SavePars_Archive*> (par2));	// .pla archives
			}
		}
		case APIDo_SaveAsModuleFileID: {
			// par2 was an API_Elem_Head handle, now a GS::Array.
			const IO::Location* location = reinterpret_cast<const IO::Location*> (par1);
			API_Elem_Head** heads = reinterpret_cast<API_Elem_Head**> (par2);
			if (heads == nullptr)
				return ACAPI_ProjectOperation_SaveAsModuleFile (location);
			GS::Array<API_Elem_Head> list;
			const GSSize count = BMGetHandleSize (reinterpret_cast<GSHandle> (heads)) / (GSSize) sizeof (API_Elem_Head);
			for (GSSize i = 0; i < count; ++i)
				list.Push ((*heads)[i]);
			return ACAPI_ProjectOperation_SaveAsModuleFile (location, &list);
		}
		default:
			return APIERR_NOTSUPPORTED;
	}
}


GSErrCode ACAPI_Navigator (API_NavigatorID code, void* par1, void* par2, void* par3, void* par4)
{
	switch (code) {
		case APINavigator_ChangeNavigatorViewID:
			return ACAPI_Navigator_ChangeNavigatorView (reinterpret_cast<API_NavigatorItem*> (par1), reinterpret_cast<API_NavigatorView*> (par2));
		case APINavigator_GetNavigatorChildrenItemsID:
			// Archicad 26: par2 = API_NavigatorItem*** (handle, unsupported here), par3 = GS::Array<API_NavigatorItem>*.
			return ACAPI_Navigator_GetNavigatorChildrenItems (reinterpret_cast<API_NavigatorItem*> (par1), reinterpret_cast<GS::Array<API_NavigatorItem>*> (par3));
		case APINavigator_GetNavigatorItemID:
			return ACAPI_Navigator_GetNavigatorItem (reinterpret_cast<const API_Guid*> (par1), reinterpret_cast<API_NavigatorItem*> (par2));
		case APINavigator_GetNavigatorSetID:
			return ACAPI_Navigator_GetNavigatorSet (reinterpret_cast<API_NavigatorSet*> (par1), reinterpret_cast<Int32*> (par2));
		case APINavigator_GetNavigatorSetNumID:
			return ACAPI_Navigator_GetNavigatorSetNum (reinterpret_cast<Int32*> (par1));
		case APINavigator_GetNavigatorViewID:
			return ACAPI_Navigator_GetNavigatorView (reinterpret_cast<API_NavigatorItem*> (par1), reinterpret_cast<API_NavigatorView*> (par2));
		default:
			return APIERR_NOTSUPPORTED;
	}
}

#endif	// CC_AC_VERSION >= 27


// =============================================================================
// Attribute helpers for every version (see ApiCompat.hpp)
// =============================================================================

bool IsRelocatedAttrType (API_AttrTypeID typeID)
{
#if defined (CC_AC_VERSION) && CC_AC_VERSION >= 27
	return typeID == API_PenID || typeID == API_FontID || typeID == API_DimStandID || typeID == API_ModelViewOptionsID;
#else
	(void) typeID;
	return false;
#endif
}


GS::Array<Int32> AttrIndices (API_AttrTypeID typeID)
{
	GS::Array<Int32> indices;
	auto range = [&] (Int32 count) {
		for (Int32 i = 1; i <= count; ++i)
			indices.Push (i);
	};
#if defined (CC_AC_VERSION) && CC_AC_VERSION >= 27
	UInt32 count = 0;
	if (typeID == API_PenID) {
		range (ACAPI_Attribute_GetPenNum (count) == NoError ? (Int32) count : 0);
	} else if (typeID == API_FontID) {
		range (ACAPI_Font_GetFontNum ());
	} else if (typeID == API_DimStandID) {
		range (ACAPI_Navigator_DimStand_GetNum (count) == NoError ? (Int32) count : 0);
	} else if (typeID == API_ModelViewOptionsID) {
		ACAPI_Navigator_ModelViewOptions_GetNum (count);		// returns void
		range ((Int32) count);
	} else {
		std::vector<Int32> found;
		ACAPI_Attribute_EnumerateAttributesByType (typeID, [&] (API_Attribute& attr) {
			found.push_back (AttrIndexToInt (attr.header.index));
		});
		std::sort (found.begin (), found.end ());
		for (Int32 i : found)
			indices.Push (i);
	}
#else
	API_AttributeIndex count = 0;
	range (ACAPI_Attribute_GetNum (typeID, &count) == NoError ? (Int32) count : 0);
#endif
	return indices;
}


bool GetAttrHeader (API_AttrTypeID typeID, API_AttributeIndex index, GS::UniString* name, API_Guid* guid)
{
	GS::UniString nameBuffer;
	GS::UniString* namePtr = name != nullptr ? name : &nameBuffer;
#if defined (CC_AC_VERSION) && CC_AC_VERSION >= 27
	const Int32 i = AttrIndexToInt (index);
	if (typeID == API_PenID) {
		API_Pen pen = {};
		pen.index = (short) i;
		if (i < 1 || ACAPI_Attribute_GetPen (pen) != NoError)
			return false;
		*namePtr = GS::UniString (pen.description, CC_UTF8);
		if (guid != nullptr)
			*guid = APINULLGuid;
		return true;
	}
	if (typeID == API_FontID) {
		API_FontType font = {};
		font.head.index = i;
		font.head.uniStringNamePtr = namePtr;
		if (ACAPI_Font_GetFont (font) != NoError)
			return false;
		if (guid != nullptr)
			*guid = APINULLGuid;
		return true;
	}
	if (typeID == API_DimStandID) {
		API_DimensionStandardsType dim = {};
		dim.head.index = i;
		dim.head.uniStringNamePtr = namePtr;
		if (ACAPI_Navigator_DimStand_Get (dim) != NoError)
			return false;
		if (guid != nullptr)
			*guid = dim.head.guid;
		return true;
	}
	if (typeID == API_ModelViewOptionsID) {
		API_ModelViewOptionsType mvo = {};
		mvo.head.index = i;
		mvo.head.uniStringNamePtr = namePtr;
		const GSErrCode err = ACAPI_Navigator_ModelViewOptions_Get (&mvo);
		ACAPI_FreeGDLModelViewOptionsPtr (&mvo.modelViewOpt.gdlOptions);
		if (err != NoError)
			return false;
		if (guid != nullptr)
			*guid = mvo.head.guid;
		return true;
	}
#endif
	API_Attribute attr = {};
	attr.header.typeID = typeID;
	attr.header.index = index;
	attr.header.uniStringNamePtr = namePtr;
	if (ACAPI_Attribute_Get (&attr) != NoError)
		return false;
	if (typeID == API_MaterialID && attr.material.texture.fileLoc != nullptr) {
		delete attr.material.texture.fileLoc;
		attr.material.texture.fileLoc = nullptr;
	}
#if !(defined (CC_AC_VERSION) && CC_AC_VERSION >= 27)
	if (typeID == API_ModelViewOptionsID && attr.modelViewOpt.modelViewOpt.gdlOptions != nullptr)
		ACAPI_FreeGDLModelViewOptionsPtr (&attr.modelViewOpt.modelViewOpt.gdlOptions);
#endif
	if (guid != nullptr)
		*guid = attr.header.guid;
	return true;
}


bool GetModelViewOptions (API_AttributeIndex index, API_ModelViewOptions& options, GS::UniString* name, API_Guid* guid)
{
	GS::UniString nameBuffer;
#if defined (CC_AC_VERSION) && CC_AC_VERSION >= 27
	API_ModelViewOptionsType mvo = {};
	mvo.head.index = AttrIndexToInt (index);
	mvo.head.uniStringNamePtr = name != nullptr ? name : &nameBuffer;
	if (ACAPI_Navigator_ModelViewOptions_Get (&mvo) != NoError) {
		ACAPI_FreeGDLModelViewOptionsPtr (&mvo.modelViewOpt.gdlOptions);
		return false;
	}
	options = mvo.modelViewOpt;			// gdlOptions now owned by the caller
	if (guid != nullptr)
		*guid = mvo.head.guid;
#else
	API_Attribute attr = {};
	attr.header.typeID = API_ModelViewOptionsID;
	attr.header.index = index;
	attr.header.uniStringNamePtr = name != nullptr ? name : &nameBuffer;
	if (ACAPI_Attribute_Get (&attr) != NoError)
		return false;
	options = attr.modelViewOpt.modelViewOpt;	// gdlOptions now owned by the caller
	if (guid != nullptr)
		*guid = attr.header.guid;
#endif
	return true;
}
