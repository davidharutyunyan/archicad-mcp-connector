// *****************************************************************************
// Documentation — databases, layouts and drawings (family "documentation").
// Exports / publishing live in DocumentationExport.cpp, the DXF and 3D model
// writers in DocumentationFiles.cpp, hotlinks and merging in
// DocumentationHotlinks.cpp; shared helpers in DocumentationShared.hpp.
//
// Commands (units: meters, degrees; layout coordinates are PAPER meters,
// origin = bottom-left corner of the sheet):
//   GetDatabases       {types?, search?, includeViews?, includeLayoutInfo?, navigatorItems?}
//   GetLayoutDrawings  {layouts?, includeMasterLayouts?, includeFrame?, includeLinkInfo?}
//   PlaceDrawings      {drawings: [{layout, view | database (+storyIndex), position?, anchor?, scale? | ratio?,
//                       angle?, name?, nameType?, number?, numbering?, frame?, title?, ...}], restoreWindow?, undoName?}
//   ModifyDrawings     {drawings: [{guid, ...same fields}], undoName?}
//   DeleteDrawings     {drawings: [guid]}
//   UpdateDrawings     {drawings? | layouts? | all?, includeManual?, restoreWindow?}
// *****************************************************************************

#include "Commands/Commands.hpp"
#include "Commands/DocumentationShared.hpp"
#include "Core/LibParts.hpp"
#include "Core/Polygon.hpp"

#include <cmath>
#include <map>

namespace cc {

namespace doc {
namespace {

// =============================================================================
// Enum tables
// =============================================================================

const NamedValue kAnchors[] = {
	{ "LeftTop",		APIAnc_LT },
	{ "CenterTop",		APIAnc_MT },
	{ "RightTop",		APIAnc_RT },
	{ "LeftCenter",		APIAnc_LM },
	{ "Center",			APIAnc_MM },
	{ "RightCenter",	APIAnc_RM },
	{ "LeftBottom",		APIAnc_LB },
	{ "CenterBottom",	APIAnc_MB },
	{ "RightBottom",	APIAnc_RB },
};

const NamedValue kNameTypes[] = {
	{ "ViewOrSourceFileName",	APIName_ViewOrSrcFileName },
	{ "ViewIdAndName",			APIName_ViewIdAndName },
	{ "Custom",					APIName_CustomName },
};

const NamedValue kNumbering[] = {
	{ "ByLayout",	APINumbering_ByLayout },
	{ "ByViewId",	APINumbering_ByViewId },
	{ "Custom",		APINumbering_CustomNum },
};

const NamedValue kColorModes[] = {
	{ "OriginalColors",	APIColorMode_OriginalColors },
	{ "BlackAndWhite",	APIColorMode_BlackAndWhite },
	{ "GrayScale",		APIColorMode_GrayScale },
};

const NamedValue kPenTableModes[] = {
	{ "Own",		APIPenTableUsageMode_UseOwnPenTable },
	{ "Model",		APIPenTableUsageMode_UseModelPenTable },
	{ "PenSet",		APIPenTableUsageMode_UsePenTableIndex },
};

const NamedValue kLinkTypes[] = {
	{ "Unknown",		API_DrawingLink_UnknownID },
	{ "InternalView",	API_DrawingLink_InternalViewID },
	{ "ExternalView",	API_DrawingLink_ExternalViewID },
	{ "Drawing",		API_DrawingLink_DrawingID },
	{ "DxfDwgFile",		API_DrawingLink_DXF_DWGID },
	{ "PdfFile",		API_DrawingLink_PDFID },
	{ "Image",			API_DrawingLink_ImageID },
	{ "PlotMakerFile",	API_DrawingLink_PMKID },
	{ "AddOnFile",		API_DrawingLink_APIID },
	{ "Info",			API_DrawingLink_InfoID },
	{ "Other",			API_DrawingLink_OtherID },
};

const NamedValue kViewNodeTypes[] = {
	{ "Undefined",			API_ViewNodeUndefined },
	{ "Story",				API_ViewNodeStory },
	{ "Section",			API_ViewNodeSection },
	{ "Detail",				API_ViewNodeDetailDrawing },
	{ "Perspective",		API_ViewNodePerspective },
	{ "Axonometry",			API_ViewNodeAxonometry },
	{ "List",				API_ViewNodeList },
	{ "Schedule",			API_ViewNodeSchedule },
	{ "Toc",				API_ViewNodeToc },
	{ "Camera",				API_ViewNodeCamera },
	{ "CameraSet",			API_ViewNodeCameraSet },
	{ "Elevation",			API_ViewNodeElevation },
	{ "InteriorElevation",	API_ViewNodeInteriorElevation },
	{ "Worksheet",			API_ViewNodeWorksheetDrawing },
	{ "DocumentFrom3D",		API_ViewNodeDocumentFrom3D },
	{ "AddOnList",			API_ViewNodeAPIList },
};

// =============================================================================
// Drawing helpers
// =============================================================================

// API_DrawingLinkInfo with RAII cleanup of the location / view path it allocates.
struct DrawingLink {
	API_DrawingLinkInfo	info;
	bool				ok = false;

	explicit DrawingLink (const API_Guid& drawing)
	{
		BNZeroMemory (&info, sizeof (info));
		API_Guid g = drawing;
		ok = ACAPI_Database (APIDb_GetDrawingLinkID, &g, &info) == NoError;
	}
	~DrawingLink ()
	{
		delete info.linkPath;
		info.linkPath = nullptr;
		if (info.viewPath != nullptr)
			BMKillPtr (&info.viewPath);
	}
	DrawingLink (const DrawingLink&) = delete;
	DrawingLink& operator= (const DrawingLink&) = delete;
};


Int32 DrawingStatusCode (const API_Guid& drawing)
{
	API_Guid g = drawing;
	Int32 status = -1;
	if (ACAPI_Database (APIDb_CheckDrawingStatusID, &g, &status) != NoError)
		return -1;
	return status;
}


GS::UniString DrawingStatusName (Int32 status)
{
	if (status == 1)	return "UpToDate";
	if (status == 0)	return "Modified";
	return "Unknown";
}


// Serializes a drawing element. Must be called with the drawing's database current.
OS DrawingJson (const API_Element& e, bool withFrame, bool withLink)
{
	const API_DrawingType& d = e.drawing;
	OS out;
	out.Add ("guid", GuidStr (e.header.guid));
	out.Add ("layer", AttrRef (API_LayerID, e.header.layer));
	if (withLink) {
		DrawingLink link (e.header.guid);
		if (link.ok) {
			out.Add ("name", ApiText (link.info.name));
			out.Add ("number", ApiText (link.info.number));
		}
	}
	out.Add ("nameType", NameOf (kNameTypes, d.nameType));
	const GS::UniString customName = ApiText (d.name);
	if (!customName.IsEmpty ())
		out.Add ("customName", customName);
	out.Add ("numbering", NameOf (kNumbering, d.numberingType));
	const GS::UniString customNumber = ApiText (d.customNumber);
	if (!customNumber.IsEmpty ())
		out.Add ("customNumber", customNumber);
	out.Add ("includeInNumbering", d.isInNumbering);
	out.Add ("includeInAutoTexts", d.includeInAutoTextsAndIES);
	out.Add ("manualUpdate", d.manualUpdate);
	out.Add ("storedInProject", d.storedInProject);
	out.Add ("status", DrawingStatusName (DrawingStatusCode (e.header.guid)));

	OS src ("navigatorItemGuid", GuidStr (d.drawingGuid));
	API_NavigatorItem item;
	if (d.drawingGuid != APINULLGuid && GetNavigatorItem (d.drawingGuid, item)) {
		src.Add ("name", GS::UniString (item.uName));
		src.Add ("id", ApiText (item.uiId));
		src.Add ("itemType", NavItemTypeName (item.itemType));
		src.Add ("map", NavMapName (item.mapId));
	}
	if (withLink) {
		DrawingLink link (e.header.guid);
		if (link.ok) {
			src.Add ("linkType", NameOf (kLinkTypes, link.info.linkTypeID));
			if (link.info.linkTypeID == API_DrawingLink_InternalViewID || link.info.linkTypeID == API_DrawingLink_ExternalViewID)
				src.Add ("viewType", NameOf (kViewNodeTypes, link.info.viewType));
			if (link.info.viewDeleted)
				src.Add ("viewDeleted", true);
			if (link.info.linkGuid != APINULLGuid && link.info.linkGuid != d.drawingGuid)
				src.Add ("linkGuid", GuidStr (link.info.linkGuid));
			if (link.info.linkPath != nullptr)
				src.Add ("file", LocationPath (*link.info.linkPath));
		}
	}
	out.Add ("source", src);

	out.Add ("position", CoordObj (d.pos));
	out.Add ("anchor", NameOf (kAnchors, d.anchorPoint));
	out.Add ("useViewOrigin", d.useOwnOrigoAsAnchor);
	AddAngle (out, "angle", d.angle);
	out.Add ("ratio", d.ratio);
	if (d.drawingScale > 1e-12) {
		const double viewScale = 1.0 / d.drawingScale;
		out.Add ("viewScale", viewScale);
		out.Add ("scale", viewScale / (d.ratio > 1e-12 ? d.ratio : 1.0));
	}
	out.Add ("colorMode", NameOf (kColorModes, d.colorMode));
	out.Add ("penTableMode", NameOf (kPenTableModes, d.penTableUsageMode));
	if (d.penTableUsageMode == APIPenTableUsageMode_UsePenTableIndex)
		out.Add ("penTable", AttrRef (API_PenTableID, d.penTableIndex));
	out.Add ("transparentBackground", d.isTransparentBk);
	out.Add ("clipToFrame", d.isCutWithFrame);
	OS border;
	border.Add ("show", d.hasBorderLine);
	border.Add ("pen", (Int32) d.borderPen);
	border.Add ("lineType", AttrRef (API_LinetypeID, d.borderLineType));
	border.Add ("size", d.borderSize);
	out.Add ("border", border);
	if (d.title.libInd > 0) {
		OS t;
		try {
			t = LibPartToJson (GetLibPartByIndex (d.title.libInd));
		} catch (const Error&) {
			t.Add ("index", (Int32) d.title.libInd);
		}
		if (d.title.guid != APINULLGuid)
			t.Add ("titleElement", GuidStr (d.title.guid));
		out.Add ("title", t);
	} else {
		out.Add ("hasTitle", false);
	}
	out.Add ("bounds", BoxObj (d.bounds));
	if (d.isMultiPageDrawing)
		out.Add ("multiPage", true);
	if (withFrame && d.poly.nCoords >= 4) {
		Memo memo;
		if (ACAPI_Element_GetMemo (e.header.guid, memo.Ptr (), APIMemoMask_Polygon) == NoError && memo->coords != nullptr)
			out.Add ("frame", PolygonToJson (d.poly, *memo));
	}
	return out;
}


// Applies drawing fields shared by PlaceDrawings and ModifyDrawings (mask == nullptr when creating).
// "scale" is handled by the callers because it needs the view scale.
void ApplyDrawingFields (API_Element& e, API_Element* mask, const OS& spec, API_ElementMemo& memo, UInt64& memoMask)
{
#define DRW_SET(field) if (mask != nullptr) ACAPI_ELEMENT_MASK_SET (*mask, API_DrawingType, field)
	API_DrawingType& d = e.drawing;

	// On create 'position' is a first guess; the callers then move the drawing so that its 'anchor' point lands
	// on 'position' (ShiftDrawingToAnchor: Archicad 26 ignores anchorPoint itself).
	if (mask == nullptr) {
		if (auto c = OptCoord (spec, "position"))		d.pos = *c;
	}
	if (Has (spec, "anchor"))							{ d.anchorPoint = (API_AnchorID) ParseNamed (kAnchors, spec, "anchor"); DRW_SET (anchorPoint); }
	if (auto v = OptBool (spec, "useViewOrigin"))		{ d.useOwnOrigoAsAnchor = *v; DRW_SET (useOwnOrigoAsAnchor); }
	if (auto v = OptAngle (spec, "angle"))				{ d.angle = *v; DRW_SET (angle); }
	if (auto v = OptDouble (spec, "ratio")) {
		if (*v <= 0.0)
			Fail ("'ratio' must be positive (1 = the view's own scale, 2 = twice as large).");
		d.ratio = *v;
		DRW_SET (ratio);
	}
	if (auto s = OptString (spec, "name")) {
		CopyApiText (*s, d.name, sizeof (d.name));
		DRW_SET (name);
		if (!Has (spec, "nameType")) {
			d.nameType = APIName_CustomName;
			DRW_SET (nameType);
		}
	}
	if (Has (spec, "nameType"))							{ d.nameType = (API_NameTypeValues) ParseNamed (kNameTypes, spec, "nameType"); DRW_SET (nameType); }
	if (auto s = OptString (spec, "number")) {
		CopyApiText (*s, d.customNumber, sizeof (d.customNumber));
		DRW_SET (customNumber);
		if (!Has (spec, "numbering")) {
			d.numberingType = APINumbering_CustomNum;
			DRW_SET (numberingType);
		}
	}
	if (Has (spec, "numbering"))						{ d.numberingType = (API_NumberingTypeValues) ParseNamed (kNumbering, spec, "numbering"); DRW_SET (numberingType); }
	if (auto v = OptBool (spec, "includeInNumbering"))	{ d.isInNumbering = *v; DRW_SET (isInNumbering); }
	if (auto v = OptBool (spec, "includeInAutoTexts"))	{ d.includeInAutoTextsAndIES = *v; DRW_SET (includeInAutoTextsAndIES); }
	if (auto v = OptBool (spec, "manualUpdate"))		{ d.manualUpdate = *v; DRW_SET (manualUpdate); }
	if (Has (spec, "colorMode"))						{ d.colorMode = (API_ColorModeValues) ParseNamed (kColorModes, spec, "colorMode"); DRW_SET (colorMode); }
	if (Has (spec, "penTable")) {
		GS::UniString s;
		if (spec.IsString ("penTable"))
			spec.Get ("penTable", s);
		if (EqualsIgnoreCase (s, "Own")) {
			d.penTableUsageMode = APIPenTableUsageMode_UseOwnPenTable;
		} else if (EqualsIgnoreCase (s, "Model")) {
			d.penTableUsageMode = APIPenTableUsageMode_UseModelPenTable;
		} else {
			d.penTableIndex = GetAttr (API_PenTableID, spec, "penTable");
			d.penTableUsageMode = APIPenTableUsageMode_UsePenTableIndex;
			DRW_SET (penTableIndex);
		}
		DRW_SET (penTableUsageMode);
	}
	if (auto v = OptBool (spec, "transparentBackground"))	{ d.isTransparentBk = *v; DRW_SET (isTransparentBk); }

	OS border;
	if (TryGetObject (spec, "border", border)) {
		if (auto v = OptBool (border, "show"))			{ d.hasBorderLine = *v; DRW_SET (hasBorderLine); }
		if (auto v = OptInt (border, "pen")) {
			if (*v < 1 || *v > 255)
				Fail ("border.pen must be a pen index 1-255.");
			d.borderPen = (short) *v;
			DRW_SET (borderPen);
		}
		if (Has (border, "lineType"))					{ d.borderLineType = GetAttr (API_LinetypeID, border, "lineType"); DRW_SET (borderLineType); }
		if (auto v = OptDouble (border, "size"))		{ d.borderSize = *v; DRW_SET (borderSize); }
		if (!Has (border, "show") && mask == nullptr)	d.hasBorderLine = true;
	}

	if (Has (spec, "title")) {
		if (spec.IsBool ("title")) {
			bool on = true;
			spec.Get ("title", on);
			if (on)
				Fail ("'title' accepts false (no title) or a drawing-title library part name / {guid} / {index}; true is not meaningful.");
			d.title.libInd = 0;
		} else {
			const API_LibPart lp = FindLibPart (spec, "title");
			d.title.libInd = lp.index;
		}
		DRW_SET (title.libInd);
	}

	if (Has (spec, "frame")) {
		if (spec.IsBool ("frame")) {
			bool on = true;
			spec.Get ("frame", on);
			if (on)
				Fail ("'frame' accepts false (remove the clip frame) or a polygon in layout coordinates.");
			d.isCutWithFrame = false;
			DRW_SET (isCutWithFrame);
		} else {
			PolygonData pd = GetPolygon (spec, "frame");
			pd.holes.Clear ();
			NormalizeOrientation (pd);
			WritePolygonToMemo (pd, d.poly, memo);
			d.isCutWithFrame = true;
			DRW_SET (isCutWithFrame);
			DRW_SET (poly);
			memoMask |= APIMemoMask_Polygon;
		}
	}
#undef DRW_SET
}


// Default placement: center of the sheet (layout coordinates, meters).
API_Coord LayoutCenter (const API_DatabaseInfo& layout)
{
	API_LayoutInfo li;
	BNZeroMemory (&li, sizeof (li));
	API_DatabaseUnId unId = layout.databaseUnId;
	API_Coord c = { 0.21, 0.1485 };
	if (ACAPI_Environment (APIEnv_GetLayoutSetsID, &li, &unId, nullptr) == NoError) {
		if (li.sizeX > 1.0 && li.sizeY > 1.0) {
			c.x = li.sizeX / 2000.0;
			c.y = li.sizeY / 2000.0;
		}
	}
	delete li.customData;
	return c;
}


API_Coord AnchorPointOf (const API_Box& b, API_AnchorID anchor)
{
	API_Coord c;
	switch (anchor) {
		case APIAnc_LT: case APIAnc_LM: case APIAnc_LB:	c.x = b.xMin; break;
		case APIAnc_RT: case APIAnc_RM: case APIAnc_RB:	c.x = b.xMax; break;
		default:										c.x = (b.xMin + b.xMax) / 2.0; break;
	}
	switch (anchor) {
		case APIAnc_LT: case APIAnc_MT: case APIAnc_RT:	c.y = b.yMax; break;
		case APIAnc_LB: case APIAnc_MB: case APIAnc_RB:	c.y = b.yMin; break;
		default:										c.y = (b.yMin + b.yMax) / 2.0; break;
	}
	return c;
}


// Archicad 26 ignores API_DrawingType::anchorPoint on create and change (verified live: "centered" drawings
// were placed by their origin and ran off the sheet), and a drawing's frame stays where it is when only 'pos'
// changes (the content slides out of its clip frame, the title stays behind). So the drawing is moved here:
// by the offset between the requested anchor point of its current bounds and 'target', together with its
// frame unless the caller sets a new frame (keepFrame). Fills e / mask / memo for one ACAPI_Element_Change.
void ShiftDrawingToAnchor (API_Element& e, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask,
						   API_AnchorID anchor, const API_Coord& target, bool keepFrame)
{
	const API_Box b = e.drawing.bounds;
	if (b.xMax - b.xMin <= 1e-9 && b.yMax - b.yMin <= 1e-9)
		Fail ("Archicad reports no extent for this drawing yet (open its layout once), so it cannot be positioned by its anchor.");
	const API_Coord now = AnchorPointOf (b, anchor);
	const double dx = target.x - now.x;
	const double dy = target.y - now.y;
	if (std::fabs (dx) < 1e-9 && std::fabs (dy) < 1e-9)
		return;
	e.drawing.pos.x += dx;
	e.drawing.pos.y += dy;
	ACAPI_ELEMENT_MASK_SET (mask, API_DrawingType, pos);
	// Unclipped drawings have a frame polygon too: Archicad derives the bounds (and the drawing title's place)
	// from it, so it moves along even when isCutWithFrame is false (verified live: the title stayed behind).
	if (keepFrame || e.drawing.poly.nCoords <= 0)
		return;
	if ((memoMask & APIMemoMask_Polygon) == 0 && ACAPI_Element_GetMemo (e.header.guid, &memo, APIMemoMask_Polygon) != NoError)
		Fail ("Cannot read the clip frame of the drawing.");
	if (memo.coords == nullptr)
		return;
	for (Int32 i = 1; i <= e.drawing.poly.nCoords; ++i) {
		(*memo.coords)[i].x += dx;
		(*memo.coords)[i].y += dy;
	}
	ACAPI_ELEMENT_MASK_SET (mask, API_DrawingType, poly);
	memoMask |= APIMemoMask_Polygon;
	// Writing the polygon switches clipping on: callers restore it afterwards with KeepUnclipped.
}


OS LayoutInfoJson (const API_DatabaseInfo& layout)
{
	API_LayoutInfo li;
	BNZeroMemory (&li, sizeof (li));
	API_DatabaseUnId unId = layout.databaseUnId;
	OS out;
	if (ACAPI_Environment (APIEnv_GetLayoutSetsID, &li, &unId, nullptr) == NoError) {
		out.Add ("width", li.sizeX / 1000.0);
		out.Add ("height", li.sizeY / 1000.0);
		OS m;
		m.Add ("left", li.leftMargin / 1000.0);
		m.Add ("top", li.topMargin / 1000.0);
		m.Add ("right", li.rightMargin / 1000.0);
		m.Add ("bottom", li.bottomMargin / 1000.0);
		out.Add ("margins", m);
		if (layout.typeID == APIWind_LayoutID) {
			out.Add ("layoutName", GS::UniString (li.layoutName));
			if (li.customLayoutNumbering)
				out.Add ("customNumber", ApiText (li.customLayoutNumber));
			out.Add ("includeInNumbering", !li.doNotIncludeInNumbering);
			out.Add ("pageCount", (Int32) std::max<UInt32> (li.layoutPageNumber, 1));
			const GS::UniString rev (li.currentRevisionId);
			if (!rev.IsEmpty ())
				out.Add ("currentRevisionId", rev);
		}
	}
	delete li.customData;
	return out;
}


// Scale of the view behind a navigator item (100 = 1:100); 0 when unknown.
Int32 SourceScale (const API_NavigatorItem& item)
{
	Int32 s = ViewDrawingScale (item);
	if (s > 0)
		return s;
	if (item.db.typeID == API_ZombieWindowID)
		return 0;
	try {
		API_DatabaseInfo db = item.db;
		FillDatabaseInfo (db);
		db.typeID = item.db.typeID;
		DatabaseScope scope (db);
		double scale = 0.0;
		if (ACAPI_Database (APIDb_GetDrawingScaleID, &scale, nullptr) == NoError && scale > 0.0)
			return (Int32) std::lround (scale < 1.0 ? 1.0 / scale : scale);
	} catch (const Error&) {
	}
	return 0;
}


// Finds a placeable navigator item for a database (+ story for the floor plan):
// a View Map view first, then the Project Map viewpoint.
API_NavigatorItem SourceForDatabase (const API_DatabaseInfo& db, std::optional<short> story)
{
	// Saved views (View Map) are preferred over the Project Map (stable scale/layer settings). A floor plan
	// story can have several saved views (e.g. a site plan on the ground floor): prefer the one named
	// like the story, otherwise the first one.
	const short floorInd = story.value_or (CurrentStoryIndex ());
	const GS::UniString storyName = StoryName (floorInd);
	for (API_NavigatorMapID map : { API_PublicViewMap, API_ProjectMap }) {
		std::optional<API_NavigatorItem> first;
		for (const API_NavigatorItem& item : CollectNavigatorItems (map)) {
			if (!IsPlaceableAsDrawing (item.itemType))
				continue;
			if (item.db.typeID != db.typeID)
				continue;
			if (db.typeID == APIWind_FloorPlanID) {
				if (item.itemType != API_StoryNavItem || item.floorNum != floorInd)
					continue;
				if (!storyName.IsEmpty () && EqualsIgnoreCase (GS::UniString (item.uName), storyName))
					return item;
				if (!first.has_value ())
					first = item;
				continue;
			} else if (item.db.databaseUnId.elemSetId != db.databaseUnId.elemSetId) {
				continue;
			}
			return item;
		}
		if (first.has_value ())
			return *first;
	}
	Fail ("No view or viewpoint found in the Navigator for this database. Pass 'view' with a navigator item guid "
		  "(get_databases {includeViews: true}, or get_navigator_tree).", APIERR_BADID);
}


API_NavigatorItem ResolveDrawingSource (const OS& spec)
{
	const bool hasView = Has (spec, "view");
	const bool hasDb = Has (spec, "database");
	if (hasView == hasDb)
		Fail ("Each drawing needs exactly one source: 'view' (navigator item guid of a view or viewpoint) or 'database' (databaseRef).");
	API_NavigatorItem item;
	if (hasView) {
		const API_Guid guid = GetGuid (spec, "view");
		if (!GetNavigatorItem (guid, item))
			Fail ("No navigator item with guid " + GuidStr (guid) + ". Use get_databases {includeViews: true} or get_navigator_tree to find views.", APIERR_BADID);
	} else {
		const API_DatabaseInfo db = ResolveDatabase (GetString (spec, "database"));
		item = SourceForDatabase (db, OptStory (spec, "storyIndex"));
	}
	if (!IsPlaceableAsDrawing (item.itemType))
		Fail ("Navigator item '" + GS::UniString (item.uName) + "' is a " + NavItemTypeName (item.itemType) +
			  "; only views/viewpoints (Story, Section, Elevation, InteriorElevation, Detail, Worksheet, DocumentFrom3D, Perspective, "
			  "Axonometry, Schedule, List, TextList, Toc) can be placed as drawings.", APIERR_BADID);
	return item;
}


API_DatabaseInfo ContainingDatabase (const API_Guid& guid)
{
	API_Guid g = guid;
	API_DatabaseInfo db;
	BNZeroMemory (&db, sizeof (db));
	const GSErrCode err = ACAPI_Database (APIDb_GetContainingDatabaseID, &g, &db);
	if (err != NoError)
		Fail ("Element " + GuidStr (guid) + " was not found in any database (" + ErrorName (err) + "). Use get_layout_drawings to list drawings.", APIERR_BADID);
	return db;
}


API_Element GetDrawing (const API_Guid& guid)
{
	API_Element e = GetElement (guid);
	if (e.header.type.typeID != API_DrawingID)
		Fail ("Element " + GuidStr (guid) + " is a " + ElemTypeName (e.header.type) + ", not a drawing.", APIERR_BADELEMENTTYPE);
	return e;
}


// Writing the frame polygon (ShiftDrawingToAnchor) switches clipping on, even with isCutWithFrame masked in the same change
// (verified live): a separate change switches it off again for drawings that were not clipped before. Inside an undo scope.
void KeepUnclipped (const API_Guid& guid, bool wasCut)
{
	if (wasCut)
		return;
	API_Element e = GetDrawing (guid);
	if (!e.drawing.isCutWithFrame)
		return;
	API_Element mask;
	ACAPI_ELEMENT_MASK_CLEAR (mask);
	e.drawing.isCutWithFrame = false;
	ACAPI_ELEMENT_MASK_SET (mask, API_DrawingType, isCutWithFrame);
	Check (ACAPI_Element_Change (&e, &mask, nullptr, 0, true), "Cannot keep the drawing unclipped");
}


GS::Array<API_DatabaseInfo> ResolveLayoutList (const OS& params, const char* key, bool includeMasterDefault)
{
	GS::Array<API_DatabaseInfo> layouts;
	const GS::Array<GS::UniString> refs = GetRefArray (params, key, { "databaseGuid", "databaseRef", "navigatorItemGuid", "name" });
	if (refs.IsEmpty ())
		return AllLayouts (includeMasterDefault);
	for (const GS::UniString& r : refs)
		layouts.Push (ResolveLayout (r, true));
	return layouts;
}


OS WithLayout (OS result, const API_DatabaseInfo& layout)
{
	if (result.Contains ("error") && !result.Contains ("layout"))
		result.Add ("layout", DatabaseJson (layout));
	return result;
}


OS WithGuid (OS result, const API_Guid& guid)
{
	if (result.Contains ("error") && !result.Contains ("guid"))
		result.Add ("guid", GuidStr (guid));
	return result;
}

// =============================================================================
// GetDatabases
// =============================================================================

OS GetDatabases (const OS& params)
{
	GS::Array<API_DatabaseTypeID> types;
	for (const GS::UniString& t : GetRefArray (params, "types")) {
		auto parsed = ParseDbType (t);
		if (!parsed.has_value ())
			Fail ("Unknown database type '" + t + "'. Allowed: " + GS::UniString (kDbTypeList) + ".");
		types.Push (*parsed);
	}
	if (types.IsEmpty ()) {
		for (API_DatabaseTypeID t : { APIWind_FloorPlanID, APIWind_SectionID, APIWind_ElevationID, APIWind_InteriorElevationID,
									  APIWind_DetailID, APIWind_WorksheetID, APIWind_DocumentFrom3DID, APIWind_LayoutID, APIWind_MasterLayoutID })
			types.Push (t);
	}
	const GS::UniString search = GetString (params, "search", GS::UniString ());
	const bool includeViews = GetBool (params, "includeViews", false);
	const bool includeLayoutInfo = GetBool (params, "includeLayoutInfo", true);

	// Layout Book items by database (navigator guid / ID of layouts).
	std::map<GS::UniString, API_NavigatorItem> layoutItems;
	if (types.Contains (APIWind_LayoutID) || types.Contains (APIWind_MasterLayoutID)) {
		for (const API_NavigatorItem& item : CollectNavigatorItems (API_LayoutMap)) {
			if (item.itemType == API_LayoutNavItem || item.itemType == API_MasterLayoutNavItem)
				layoutItems[GuidStr (item.db.databaseUnId.elemSetId)] = item;
		}
	}

	GS::Array<API_NavigatorItem> views;
	if (includeViews) {
		for (API_NavigatorMapID map : { API_PublicViewMap, API_MyViewMap }) {
			for (const API_NavigatorItem& item : CollectNavigatorItems (map)) {
				if (item.itemType != API_FolderNavItem && item.itemType != API_UndefinedNavItem)
					views.Push (item);
			}
		}
	}

	GS::Array<OS> list;
	for (API_DatabaseTypeID t : types) {
		for (const API_DatabaseInfo& info : ListDatabases (t)) {
			if (!search.IsEmpty ()) {
				const GS::UniString name (info.name), title (info.title), ref (info.ref);
				if (!name.Contains (search, GS::UniString::CaseInsensitive) && !title.Contains (search, GS::UniString::CaseInsensitive) &&
					!ref.Contains (search, GS::UniString::CaseInsensitive))
					continue;
			}
			OS db = DatabaseJson (info);
			if (info.typeID == APIWind_LayoutID || info.typeID == APIWind_MasterLayoutID) {
				auto it = layoutItems.find (GuidStr (info.databaseUnId.elemSetId));
				if (it != layoutItems.end ()) {
					db.Add ("navigatorItemGuid", GuidStr (it->second.guid));
					const GS::UniString id = ApiText (it->second.uiId);
					if (!id.IsEmpty ())
						db.Add ("layoutId", id);
				}
				if (includeLayoutInfo)
					db.Add ("sheet", LayoutInfoJson (info));
			}
			if (includeViews) {
				GS::Array<OS> vs;
				for (const API_NavigatorItem& v : views) {
					if (v.db.typeID != info.typeID)
						continue;
					if (info.typeID != APIWind_FloorPlanID && info.typeID != APIWind_3DModelID &&
						v.db.databaseUnId.elemSetId != info.databaseUnId.elemSetId)
						continue;
					OS vj ("navigatorItemGuid", GuidStr (v.guid), "name", GS::UniString (v.uName));
					vj.Add ("id", ApiText (v.uiId));
					vj.Add ("itemType", NavItemTypeName (v.itemType));
					vj.Add ("map", NavMapName (v.mapId));
					if (v.itemType == API_StoryNavItem)
						vj.Add ("storyIndex", (Int32) v.floorNum);
					const Int32 scale = ViewDrawingScale (v);
					if (scale > 0)
						vj.Add ("scale", scale);
					vs.Push (vj);
				}
				db.Add ("views", vs);
			}
			list.Push (db);
		}
	}

	OS out;
	try {
		API_DatabaseInfo cur = CurrentDatabase ();
		API_DatabaseInfo filled = cur;
		FillDatabaseInfo (filled);
		filled.typeID = cur.typeID;
		out.Add ("currentDatabase", DatabaseJson (filled));
		API_WindowInfo win = CurrentWindow ();
		API_DatabaseInfo wf = win;
		FillDatabaseInfo (wf);
		wf.typeID = win.typeID;
		out.Add ("currentWindow", DatabaseJson (wf));
		if (win.typeID == APIWind_FloorPlanID)
			out.Add ("currentStoryIndex", (Int32) CurrentStoryIndex ());
	} catch (const Error&) {
	}
	out.Add ("databases", list);
	out.Add ("count", (Int32) list.GetSize ());

	const GS::Array<GS::UniString> navRefs = GetRefArray (params, "navigatorItems", { "guid", "navigatorItemGuid" });
	if (!navRefs.IsEmpty ()) {
		GS::Array<OS> navs;
		for (const GS::UniString& r : navRefs) {
			navs.Push (Try ([&] () -> OS {
				const API_Guid g = ParseGuid (r);
				API_NavigatorItem item;
				if (!GetNavigatorItem (g, item))
					Fail ("No navigator item with guid " + r + ".", APIERR_BADID);
				OS j = NavItemJson (item, false);
				if (item.db.typeID != API_ZombieWindowID) {
					API_DatabaseInfo db = item.db;
					FillDatabaseInfo (db);
					db.typeID = item.db.typeID;
					j.Add ("database", DatabaseJson (db));
				}
				const Int32 scale = ViewDrawingScale (item);
				if (scale > 0)
					j.Add ("scale", scale);
				return j;
			}));
		}
		out.Add ("navigatorItems", navs);
	}
	return out;
}

// =============================================================================
// GetLayoutDrawings
// =============================================================================

OS GetLayoutDrawings (const OS& params)
{
	const bool includeMaster = GetBool (params, "includeMasterLayouts", false);
	const bool withFrame = GetBool (params, "includeFrame", true);
	const bool withLink = GetBool (params, "includeLinkInfo", true);
	const GS::Array<API_DatabaseInfo> layouts = ResolveLayoutList (params, "layouts", includeMaster);

	GS::Array<OS> results;
	Int32 total = 0;
	for (const API_DatabaseInfo& layout : layouts) {
		results.Push (WithLayout (Try ([&] () -> OS {
			DatabaseScope scope (layout);
			GS::Array<OS> drawings;
			for (const API_Guid& g : ListElements (API_DrawingID)) {
				drawings.Push (WithGuid (Try ([&] () -> OS {
					return DrawingJson (GetDrawing (g), withFrame, withLink);
				}), g));
			}
			total += (Int32) drawings.GetSize ();
			OS item;
			item.Add ("layout", DatabaseJson (layout));
			item.Add ("drawings", drawings);
			item.Add ("drawingCount", (Int32) drawings.GetSize ());
			return item;
		}), layout));
	}
	return OS ("layouts", results, "layoutCount", (Int32) results.GetSize (), "drawingCount", total);
}

// =============================================================================
// PlaceDrawings
// =============================================================================

struct PlaceJob {
	OS					spec;
	API_DatabaseInfo	layout {};
	API_NavigatorItem	source {};
	double				ratio = 1.0;
	double				wantedScale = 0.0;
	bool				fixRatioAfter = false;
	bool				ready = false;
	OS					result;
	API_Guid			guid = APINULLGuid;
};


OS PlaceDrawings (const OS& params)
{
	const GS::Array<OS> specs = GetObjectArray (params, "drawings");
	if (specs.IsEmpty ())
		Fail ("Pass at least one drawing in 'drawings'.");
	const GS::UniString undoName = GetString (params, "undoName", "Place drawings (Claude)");
	WindowRestorer restorer;
	restorer.Arm (GetBool (params, "restoreWindow", false));

	// 1) Resolve everything before touching the database.
	GS::Array<PlaceJob> jobs;
	for (const OS& spec : specs) {
		PlaceJob job;
		job.spec = spec;
		job.result = Try ([&] () -> OS {
			if (!Has (spec, "layout"))
				Fail ("Each drawing needs 'layout' (layout databaseRef, navigator item guid or name from get_databases).");
			job.layout = ResolveLayout (GetString (spec, "layout"), true);
			job.source = ResolveDrawingSource (spec);
			if (Has (spec, "scale") && Has (spec, "ratio"))
				Fail ("Pass either 'scale' (e.g. 50 for 1:50) or 'ratio', not both.");
			if (auto s = OptDouble (spec, "scale")) {
				if (*s <= 0.0)
					Fail ("'scale' is the scale denominator and must be positive, e.g. 100 for 1:100.");
				job.wantedScale = *s;
				const Int32 viewScale = SourceScale (job.source);
				if (viewScale > 0) {
					job.ratio = (double) viewScale / *s;
				} else {
					job.ratio = 1.0;
					job.fixRatioAfter = true;
				}
			} else if (auto r = OptDouble (spec, "ratio")) {
				job.ratio = *r;
			}
			if (job.ratio <= 0.0)
				Fail ("The drawing ratio must be positive.");
			if (Has (spec, "view") && Has (spec, "storyIndex"))
				Fail ("'storyIndex' is only used together with 'database' (the view already defines its story).");
			job.ready = true;
			return OS ();
		});
		jobs.Push (job);
	}

	// 2) Create (one undo step). The (first) target layout is brought to the front beforehand, like Archicad does
	//    when placing by hand; window changes are not allowed inside an undo scope, so the others are only made current.
	for (const PlaceJob& job : jobs) {
		if (!job.ready)
			continue;
		try {
			if (!SameDatabase (CurrentWindow (), job.layout))
				SwitchToWindow (job.layout);
		} catch (const Error&) {
		}
		break;
	}
	Undoable (undoName, [&] () {
		for (PlaceJob& job : jobs) {
			if (!job.ready)
				continue;
			job.result = Try ([&] () -> OS {
				DatabaseScope scope (job.layout);

				API_Element element = NewElement (API_DrawingID);
				GetDefaults (element, nullptr);
				element.drawing.drawingGuid = job.source.guid;
				element.drawing.anchorPoint = APIAnc_MM;
				element.drawing.pos = LayoutCenter (job.layout);
				element.drawing.ratio = job.ratio;
				element.drawing.angle = 0.0;
				// Tool defaults carry the crop of the last manually placed drawing: start unclipped.
				element.drawing.isCutWithFrame = false;
				element.drawing.poly.nCoords = 0;
				element.drawing.poly.nSubPolys = 0;
				element.drawing.poly.nArcs = 0;

				Memo memo;
				UInt64 memoMask = 0;
				OS spec = job.spec;
				ApplyDrawingFields (element, nullptr, spec, *memo, memoMask);
				element.drawing.ratio = job.ratio;
				if (Has (spec, "layer"))
					element.header.layer = GetAttr (API_LayerID, spec, "layer");

				const GSErrCode err = ACAPI_Element_Create (&element, memo.Ptr ());
				if (err != NoError)
					Fail ("Cannot place '" + GS::UniString (job.source.uName) + "' on " + GS::UniString (job.layout.name) + ": " + ErrorName (err) +
						  ". Check that the source view still exists and can be generated (open it once).", err);
				job.guid = element.header.guid;
				return OS ("guid", GuidStr (element.header.guid));
			});
		}
	});

	// 3) Scale for sources without a known view scale (a separate step: changing a drawing in the same
	//    undo step that created it is unreliable in Archicad).
	bool anyFix = false;
	for (const PlaceJob& job : jobs)
		anyFix = anyFix || (job.fixRatioAfter && job.guid != APINULLGuid);
	if (anyFix) {
		Undoable ("Set drawing scale (Claude)", [&] () {
			for (PlaceJob& job : jobs) {
				if (!job.fixRatioAfter || job.guid == APINULLGuid)
					continue;
				OS r = Try ([&] () -> OS {
					DatabaseScope scope (job.layout);
					API_Element e = GetDrawing (job.guid);
					if (e.drawing.drawingScale <= 1e-12)
						Fail ("Archicad reports no scale for the source view; set 'ratio' with modify_drawings.");
					API_Element mask;
					ACAPI_ELEMENT_MASK_CLEAR (mask);
					e.drawing.ratio = (1.0 / e.drawing.drawingScale) / job.wantedScale;
					ACAPI_ELEMENT_MASK_SET (mask, API_DrawingType, ratio);
					Check (ACAPI_Element_Change (&e, &mask, nullptr, 0, true), "Cannot set the drawing scale");
					return OS ();
				});
				if (r.Contains ("error"))
					job.result.Add ("scaleWarning", r);
			}
		});
	}

	// 4) Position by the requested anchor (default: the drawing centre on 'position' / the sheet centre). A separate
	//    step after the scale is final, because the bounds of the new drawing are needed (see ShiftDrawingToAnchor).
	Undoable ("Position drawings (Claude)", [&] () {
		for (PlaceJob& job : jobs) {
			if (job.guid == APINULLGuid || GetBool (job.spec, "useViewOrigin", false))
				continue;
			OS r = Try ([&] () -> OS {
				DatabaseScope scope (job.layout);
				const API_AnchorID anchor = Has (job.spec, "anchor") ? (API_AnchorID) ParseNamed (kAnchors, job.spec, "anchor") : APIAnc_MM;
				const API_Coord target = OptCoord (job.spec, "position").value_or (LayoutCenter (job.layout));
				API_Element e = GetDrawing (job.guid);
				const bool wasCut = e.drawing.isCutWithFrame;
				API_Element mask;
				ACAPI_ELEMENT_MASK_CLEAR (mask);
				Memo memo;
				UInt64 memoMask = 0;
				ShiftDrawingToAnchor (e, mask, *memo, memoMask, anchor, target, Has (job.spec, "frame"));
				Check (ACAPI_Element_Change (&e, &mask, memoMask != 0 ? memo.Ptr () : nullptr, memoMask, true), "Cannot position the drawing");
				if (!Has (job.spec, "frame"))
					KeepUnclipped (job.guid, wasCut);
				return OS ();
			});
			if (r.Contains ("error"))
				job.result.Add ("positionWarning", r);
		}
	});

	// 5) Report.
	restorer.Finish ();
	GS::Array<OS> results;
	for (PlaceJob& job : jobs) {
		if (job.guid == APINULLGuid) {
			results.Push (job.result);
			continue;
		}
		OS r = job.result;
		r.Add ("layout", DatabaseJson (job.layout));
		r.Add ("source", NavItemJson (job.source, false));
		OS details = Try ([&] () -> OS {
			DatabaseScope scope (job.layout);
			return DrawingJson (GetDrawing (job.guid), false, true);
		});
		if (!details.Contains ("error"))
			r.Add ("drawing", details);
		results.Push (r);
	}
	OS out ("results", results);
	try {
		API_WindowInfo win = CurrentWindow ();
		API_DatabaseInfo wf = win;
		FillDatabaseInfo (wf);
		wf.typeID = win.typeID;
		out.Add ("activeWindow", DatabaseJson (wf));
	} catch (const Error&) {
	}
	return out;
}

// =============================================================================
// ModifyDrawings / DeleteDrawings
// =============================================================================

OS ModifyDrawings (const OS& params)
{
	const GS::Array<OS> specs = GetObjectArray (params, "drawings");
	if (specs.IsEmpty ())
		Fail ("Pass at least one {guid, ...} item in 'drawings'.");
	const GS::UniString undoName = GetString (params, "undoName", "Modify drawings (Claude)");

	struct Job { OS spec; API_Guid guid = APINULLGuid; API_DatabaseInfo db {}; OS result; bool ready = false; };
	GS::Array<Job> jobs;
	for (const OS& spec : specs) {
		Job job;
		job.spec = spec;
		job.result = Try ([&] () -> OS {
			job.guid = GetGuid (spec, "guid");
			if (Has (spec, "view") || Has (spec, "database"))
				Fail ("The Archicad 26 API cannot relink a placed drawing to another source. Place a new drawing with place_drawing "
					  "(copy position / scale / name from get_layout_drawings) and remove this one with delete_drawings.");
			if (Has (spec, "scale") && Has (spec, "ratio"))
				Fail ("Pass either 'scale' or 'ratio', not both.");
			job.db = ContainingDatabase (job.guid);
			job.ready = true;
			return OS ("guid", GuidStr (job.guid));
		});
		jobs.Push (job);
	}

	Undoable (undoName, [&] () {
		for (Job& job : jobs) {
			if (!job.ready)
				continue;
			job.result = WithGuid (Try ([&] () -> OS {
				DatabaseScope scope (job.db);
				API_Element e = GetDrawing (job.guid);
				const bool wasCut = e.drawing.isCutWithFrame;
				API_Element mask;
				ACAPI_ELEMENT_MASK_CLEAR (mask);
				Memo memo;
				UInt64 memoMask = 0;
				ApplyDrawingFields (e, &mask, job.spec, *memo, memoMask);
				if (auto s = OptDouble (job.spec, "scale")) {
					if (*s <= 0.0)
						Fail ("'scale' must be a positive scale denominator (e.g. 50 for 1:50).");
					if (e.drawing.drawingScale <= 1e-12)
						Fail ("Archicad reports no scale for this drawing's source; set 'ratio' instead.");
					e.drawing.ratio = (1.0 / e.drawing.drawingScale) / *s;
					ACAPI_ELEMENT_MASK_SET (mask, API_DrawingType, ratio);
				}
				if (Has (job.spec, "layer")) {
					e.header.layer = GetAttr (API_LayerID, job.spec, "layer");
					ACAPI_ELEMENT_MASK_SET (mask, API_Elem_Head, layer);
				}
				const bool rescaled = Has (job.spec, "scale") || Has (job.spec, "ratio");
				const bool reposition = Has (job.spec, "position") && !GetBool (job.spec, "useViewOrigin", false);
				const API_AnchorID anchor = Has (job.spec, "anchor") ? (API_AnchorID) ParseNamed (kAnchors, job.spec, "anchor") : APIAnc_MM;
				if (reposition && !rescaled)		// one change: shift by the current bounds
					ShiftDrawingToAnchor (e, mask, *memo, memoMask, anchor, GetCoord (job.spec, "position"), Has (job.spec, "frame"));
				Check (ACAPI_Element_Change (&e, &mask, memoMask != 0 ? memo.Ptr () : nullptr, memoMask, true), "Cannot change the drawing");
				if (reposition && rescaled) {		// the bounds change with the scale: position afterwards
					API_Element e2 = GetDrawing (job.guid);
					API_Element mask2;
					ACAPI_ELEMENT_MASK_CLEAR (mask2);
					Memo memo2;
					UInt64 memoMask2 = 0;
					ShiftDrawingToAnchor (e2, mask2, *memo2, memoMask2, anchor, GetCoord (job.spec, "position"), Has (job.spec, "frame"));
					Check (ACAPI_Element_Change (&e2, &mask2, memoMask2 != 0 ? memo2.Ptr () : nullptr, memoMask2, true), "Cannot position the drawing");
				}
				if (reposition && !Has (job.spec, "frame"))
					KeepUnclipped (job.guid, wasCut);
				return OS ("guid", GuidStr (job.guid));
			}), job.guid);
		}
	});

	GS::Array<OS> results;
	for (Job& job : jobs) {
		if (job.result.Contains ("error") || !job.ready) {
			results.Push (job.result);
			continue;
		}
		OS r = job.result;
		OS details = Try ([&] () -> OS {
			DatabaseScope scope (job.db);
			return DrawingJson (GetDrawing (job.guid), false, true);
		});
		if (!details.Contains ("error"))
			r.Add ("drawing", details);
		results.Push (r);
	}
	return OS ("results", results);
}


OS DeleteDrawings (const OS& params)
{
	const GS::Array<API_Guid> guids = GetGuidArray (params, "drawings", true);
	if (guids.IsEmpty ())
		Fail ("Pass at least one drawing guid in 'drawings'.");

	GS::Array<OS> results;
	Undoable (GetString (params, "undoName", "Delete drawings (Claude)"), [&] () {
		for (const API_Guid& guid : guids) {
			results.Push (WithGuid (Try ([&] () -> OS {
				const API_DatabaseInfo db = ContainingDatabase (guid);
				DatabaseScope scope (db);
				GetDrawing (guid);
				GS::Array<API_Guid> one;
				one.Push (guid);
				Check (ACAPI_Element_Delete (one), "Cannot delete drawing " + GuidStr (guid));
				return OS ("guid", GuidStr (guid), "deleted", true);
			}), guid));
		}
	});
	return OS ("results", results);
}

// =============================================================================
// UpdateDrawings
// =============================================================================

OS UpdateDrawings (const OS& params)
{
	const bool includeManual = GetBool (params, "includeManual", true);
	WindowRestorer restorer;
	restorer.Arm (GetBool (params, "restoreWindow", true));

	struct Target { API_Guid guid; API_DatabaseInfo layout; Int32 before = -1; bool manual = false; bool toggled = false; OS error; };
	GS::Array<Target> targets;
	GS::Array<OS> errors;

	auto addLayout = [&] (const API_DatabaseInfo& layout) {
		DatabaseScope scope (layout);
		for (const API_Guid& g : ListElements (API_DrawingID))
			targets.Push ({ g, layout });
	};

	if (Has (params, "drawings")) {
		for (const API_Guid& g : GetGuidArray (params, "drawings", true)) {
			OS r = Try ([&] () -> OS {
				targets.Push ({ g, ContainingDatabase (g) });
				return OS ();
			});
			if (r.Contains ("error"))
				errors.Push (WithGuid (r, g));
		}
	} else if (Has (params, "layouts")) {
		for (const API_DatabaseInfo& l : ResolveLayoutList (params, "layouts", false))
			addLayout (l);
	} else if (GetBool (params, "all", false)) {
		for (const API_DatabaseInfo& l : AllLayouts (false))
			addLayout (l);
	} else {
		Fail ("Pass 'drawings' (guids), 'layouts' (layout refs) or all: true.");
	}

	// Status before.
	for (Target& t : targets) {
		OS r = Try ([&] () -> OS {
			DatabaseScope scope (t.layout);
			API_Element e = GetDrawing (t.guid);
			t.manual = e.drawing.manualUpdate;
			t.before = DrawingStatusCode (t.guid);
			return OS ();
		});
		if (r.Contains ("error"))
			t.error = r;
	}

	// Temporarily switch manual drawings to automatic update so that Archicad refreshes them.
	auto setManual = [&] (bool manual, const char* undo) {
		Undoable (undo, [&] () {
			for (Target& t : targets) {
				if (t.error.Contains ("error") || !t.manual || (manual && !t.toggled))
					continue;
				OS r = Try ([&] () -> OS {
					DatabaseScope scope (t.layout);
					API_Element e = GetDrawing (t.guid);
					API_Element mask;
					ACAPI_ELEMENT_MASK_CLEAR (mask);
					e.drawing.manualUpdate = manual;
					ACAPI_ELEMENT_MASK_SET (mask, API_DrawingType, manualUpdate);
					Check (ACAPI_Element_Change (&e, &mask, nullptr, 0, true), "Cannot change the update mode");
					return OS ();
				});
				if (!manual && !r.Contains ("error"))
					t.toggled = true;
			}
		});
	};
	bool anyManual = false;
	for (const Target& t : targets)
		anyManual = anyManual || (t.manual && !t.error.Contains ("error"));
	if (includeManual && anyManual)
		setManual (false, "Update drawings: automatic (Claude)");

	// Open every affected layout: Archicad updates outdated auto-update drawings when a layout is displayed.
	GS::Array<API_DatabaseInfo> opened;
	for (const Target& t : targets) {
		if (t.error.Contains ("error"))
			continue;
		bool seen = false;
		for (const API_DatabaseInfo& o : opened)
			seen = seen || SameDatabase (o, t.layout);
		if (seen)
			continue;
		opened.Push (t.layout);
		if (t.layout.typeID == APIWind_LayoutID || t.layout.typeID == APIWind_MasterLayoutID) {
			if (SwitchToWindow (t.layout)) {
				ACAPI_Database (APIDb_RebuildCurrentDatabaseID, nullptr, nullptr);
				ACAPI_Automate (APIDo_RedrawID, nullptr, nullptr);
			}
		}
	}

	if (includeManual && anyManual)
		setManual (true, "Update drawings: restore manual (Claude)");

	GS::Array<OS> results = errors;
	Int32 updated = 0, upToDate = 0;
	for (Target& t : targets) {
		if (t.error.Contains ("error")) {
			results.Push (WithGuid (t.error, t.guid));
			continue;
		}
		OS r = Try ([&] () -> OS {
			DatabaseScope scope (t.layout);
			const Int32 after = DrawingStatusCode (t.guid);
			OS item ("guid", GuidStr (t.guid));
			item.Add ("layout", GS::UniString (t.layout.name));
			item.Add ("layoutRef", DatabaseRef (t.layout));
			item.Add ("manualUpdate", t.manual);
			item.Add ("statusBefore", DrawingStatusName (t.before));
			item.Add ("statusAfter", DrawingStatusName (after));
			if (after == 1)
				++upToDate;
			if (t.before != 1 && after == 1)
				++updated;
			return item;
		});
		results.Push (WithGuid (r, t.guid));
	}

	OS out ("results", results);
	out.Add ("drawingCount", (Int32) targets.GetSize ());
	out.Add ("updatedCount", updated);
	out.Add ("upToDateCount", upToDate);
	out.Add ("note", GS::UniString ("Archicad 26 has no direct 'update drawing' API (added in Archicad 27). The connector opens each "
									"layout (Archicad refreshes auto-update drawings when a layout is shown), temporarily switching manual "
									"drawings to automatic. Drawings still 'Modified' afterwards: open the layout in Archicad and use "
									"Document > Drawing Manager > Update, or publish the layout (publishing updates drawings)."));
	return out;
}

} // namespace
} // namespace doc


void RegisterDocumentationCommands ()
{
	using namespace doc;

	RegisterCommand ("GetDatabases",
		"Lists Archicad databases (floor plan, sections, elevations, interior elevations, details, worksheets, 3D documents, layouts, "
		"master layouts) with databaseRef (guid, or \"FloorPlan\"), type, name, ref, title, linked element. Layouts add their Layout "
		"Book navigatorItemGuid, layoutId and sheet size/margins (meters). Input: {types?: [type], search?: substring, includeViews?: "
		"bool (View Map views per database, with scale), includeLayoutInfo?: bool (default true), navigatorItems?: [guid] (resolve "
		"navigator items to their database)}. Output also has currentDatabase / currentWindow.",
		[] (const OS& params) -> OS { return GetDatabases (params); });

	RegisterCommand ("GetLayoutDrawings",
		"Drawings placed on layouts: guid, name, number, source view (navigator item), link type, status (UpToDate/Modified), "
		"position (paper meters), anchor, angle, ratio, viewScale, effective scale, frame polygon, title, border, pen table mode. "
		"Input: {layouts?: [layout ref] (default all), includeMasterLayouts?: bool, includeFrame?: bool (default true), includeLinkInfo?: "
		"bool (default true)}.",
		[] (const OS& params) -> OS { return GetLayoutDrawings (params); });

	RegisterCommand ("PlaceDrawings",
		"Places views on layouts as drawings (one undo step). Input: {drawings: [{layout, view (navigator item guid of a view/viewpoint) | "
		"database (databaseRef; + storyIndex for the floor plan), position?: {x,y} paper meters (default sheet center), anchor?: "
		"LeftTop|CenterTop|RightTop|LeftCenter|Center|RightCenter|LeftBottom|CenterBottom|RightBottom (default Center), scale?: "
		"denominator (50 = 1:50) | ratio?: number, angle?: degrees, name?, nameType?, number?, numbering?, includeInNumbering?, "
		"manualUpdate?, frame?: polygon (clip, paper meters), title?: false | library part, colorMode?, penTable?, "
		"transparentBackground?, border?: {show, pen, lineType, size}, layer?}], restoreWindow?: bool (default false)}. Output: "
		"results [{guid, layout, source, drawing} | {error}].",
		[] (const OS& params) -> OS { return PlaceDrawings (params); });

	RegisterCommand ("ModifyDrawings",
		"Changes placed drawings (one undo step). Input: {drawings: [{guid, position?, anchor?, angle?, scale? | ratio?, name?, nameType?, "
		"number?, numbering?, includeInNumbering?, includeInAutoTexts?, manualUpdate?, frame?: polygon | false, title?: false | library "
		"part, colorMode?, penTable?: \"Own\"|\"Model\"|pen set name, transparentBackground?, border?, layer?}]}.",
		[] (const OS& params) -> OS { return ModifyDrawings (params); });

	RegisterCommand ("DeleteDrawings",
		"Deletes placed drawings from layouts (or model views). Input: {drawings: [guid]}. One undo step.",
		[] (const OS& params) -> OS { return DeleteDrawings (params); });

	RegisterCommand ("UpdateDrawings",
		"Refreshes drawings from their source views. Input: {drawings?: [guid] | layouts?: [layout ref] | all?: true, includeManual?: "
		"bool (default true: temporarily switch manual drawings to auto update), restoreWindow?: bool (default true)}. Output per "
		"drawing: statusBefore / statusAfter (UpToDate | Modified | Unknown).",
		[] (const OS& params) -> OS { return UpdateDrawings (params); });

	// Drawing elements through the generic element tools (get_element_details / modify_elements). Placing needs
	// a layout + source view and goes through PlaceDrawings, so there is no create function.
	RegisterAdapter ({
		API_DrawingID,
		nullptr,
		[] (const API_Element& e, OS& out) { out = DrawingJson (e, true, true); },
		[] (API_Element& e, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask, const OS& patch) {
			if (Has (patch, "view") || Has (patch, "database"))
				Fail ("A placed drawing cannot be relinked to another source in Archicad 26; place a new one with place_drawing.");
			if (Has (patch, "scale") && Has (patch, "ratio"))
				Fail ("Pass either 'scale' or 'ratio', not both.");
			if (Has (patch, "position") && (Has (patch, "scale") || Has (patch, "ratio")))
				Fail ("Change the scale and the position of a drawing in separate calls (or use modify_drawings, which does both).");
			ApplyDrawingFields (e, &mask, patch, memo, memoMask);
			if (Has (patch, "position") && !GetBool (patch, "useViewOrigin", false)) {
				const API_AnchorID anchor = Has (patch, "anchor") ? (API_AnchorID) ParseNamed (kAnchors, patch, "anchor") : APIAnc_MM;
				ShiftDrawingToAnchor (e, mask, memo, memoMask, anchor, GetCoord (patch, "position"), Has (patch, "frame"));
			}
			if (auto s = OptDouble (patch, "scale")) {
				if (*s <= 0.0)
					Fail ("'scale' must be a positive scale denominator (e.g. 50 for 1:50).");
				if (e.drawing.drawingScale <= 1e-12)
					Fail ("Archicad reports no scale for this drawing's source; set 'ratio' instead.");
				e.drawing.ratio = (1.0 / e.drawing.drawingScale) / *s;
				ACAPI_ELEMENT_MASK_SET (mask, API_DrawingType, ratio);
			}
		},
		[] (const API_Guid& guid, const API_Element& before, const OS& patch) {
			if (Has (patch, "position") && !Has (patch, "frame") && !GetBool (patch, "useViewOrigin", false))
				KeepUnclipped (guid, before.drawing.isCutWithFrame);
		}
	});

	RegisterDocumentationExportCommands ();
	RegisterDocumentationFileCommands ();
	RegisterDocumentationHotlinkCommands ();
}

} // namespace cc
