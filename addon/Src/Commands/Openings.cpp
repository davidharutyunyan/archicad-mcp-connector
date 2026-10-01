// *****************************************************************************
// Openings — Window / Door / Skylight adapters (this file) and the Opening-tool
// adapter (OpeningsExtrusion.cpp), plus GetHostOpenings.
//
// Window / Door create / modify fields (lengths in meters, angles in degrees):
//   wall*            GUID of the host wall (alias "owner"; cannot be changed afterwards)
//   position | point* distance along the wall reference line from the wall's begin point to the
//                    opening CENTRE (Archicad objLoc), or a plan point {x,y} projected onto the
//                    reference line;  positionReference "Center" (default) | "Begin" | "End" makes
//                    position/point refer to the edge nearer the wall begin / end instead
//   sillHeight       Archicad "lower" (parapet / sill height, from the reference of verticalAnchor)
//   width, height, libraryPart (Window / Door library part), emptyHole (libInd 0), gdlParams
//   flipped (oSide: opens to the other side of the wall), referenceSideFlipped (refSide, follows
//   flipped by default), mirrored (reflected: hinge / handle side swapped), anchor ("Begin" |
//   "Center" | "End" = fixPoint), verticalAnchor ("SillToWallBottom" | "SillToStory" |
//   "HeaderToWallBottom" | "HeaderToStory" | "SillToWallTop" | "HeaderToWallTop"),
//   verticalAnchorStory, reveal, revealDepth, revealDepthLocation ("WallSide" | "Core"),
//   jambDepthHead / jambDepthSill / jambDepthLeft / jambDepthRight, orientationInSlantedWall
//   ("FollowWall" | "Vertical"), inheritWallCut, subFloorThickness
//   + opening-base attributes (surface, pen, lineType, cutFill..., displayOption, useObject*)
//   + common: layer, renovationStatus, elementId   (the story is always the wall's story)
//
// Skylight fields: owner* (Roof or Shell), point* {x,y} (anchor position), anchorLevel, anchor
//   ("BottomCenter" | "BottomLeft" | "BottomRight" | "TopCenter" | "TopLeft" | "TopRight"),
//   fixMode ("Horizontal" | "Vertical"), azimuthAngle, pivotVertexId, verticalAnchor
//   ("ToRoofPivot" | "ToStory" | "ToShellBase"), verticalAnchorStory, width, height, libraryPart,
//   emptyHole, gdlParams, flipped, mirrored + opening-base attributes + common fields.
// *****************************************************************************

#include "Commands/Commands.hpp"
#include "Commands/Openings.hpp"
#include "Core/Command.hpp"
#include "Core/Elements.hpp"
#include "Core/Enums.hpp"
#include "Core/LibParts.hpp"

#include <cmath>
#include <cstdio>

namespace cc {

using namespace openings;

namespace {

// --- Enum tables -------------------------------------------------------------------

const NamedValue kFixPoints[] = {
	{ "Begin",				APIHoleAnchor_BegFix },
	{ "Center",				APIHoleAnchor_CenterFix },
	{ "End",				APIHoleAnchor_EndFix },
};

enum PositionReference { PosRef_Center = 0, PosRef_Begin = 1, PosRef_End = 2 };
const NamedValue kPositionReferences[] = {
	{ "Center",				PosRef_Center },
	{ "Begin",				PosRef_Begin },
	{ "End",				PosRef_End },
};

const NamedValue kWallVerticalLinks[] = {
	{ "SillToWallBottom",	API_LinkSillToWallBottom },
	{ "SillToStory",		API_LinkSillToStory },
	{ "HeaderToWallBottom",	API_LinkHeaderToWallBottom },
	{ "HeaderToStory",		API_LinkHeaderToStory },
	{ "SillToWallTop",		API_LinkSillToWallTop },
	{ "HeaderToWallTop",	API_LinkHeaderToWallTop },
};

const NamedValue kSkylightVerticalLinks[] = {
	{ "ToRoofPivot",		API_LinkSkylightToRoofPivot },
	{ "ToStory",			API_LinkSkylightToStory },
	{ "ToShellBase",		API_LinkSkylightToShellBase },
};

const NamedValue kAllVerticalLinks[] = {
	{ "None",				API_NoLink },
	{ "SillToWallBottom",	API_LinkSillToWallBottom },
	{ "SillToStory",		API_LinkSillToStory },
	{ "HeaderToWallBottom",	API_LinkHeaderToWallBottom },
	{ "HeaderToStory",		API_LinkHeaderToStory },
	{ "ToRoofPivot",		API_LinkSkylightToRoofPivot },
	{ "ToStory",			API_LinkSkylightToStory },
	{ "ToShellBase",		API_LinkSkylightToShellBase },
	{ "SillToWallTop",		API_LinkSillToWallTop },
	{ "HeaderToWallTop",	API_LinkHeaderToWallTop },
};

const NamedValue kRevealLocations[] = {
	{ "WallSide",			APIWDRevealDepth_Side },
	{ "Core",				APIWDRevealDepth_Core },
};

const NamedValue kDirectionTypes[] = {
	{ "FollowWall",			API_WDAssociativeToWall },
	{ "Vertical",			API_WDVertical },
};

const NamedValue kDisplayOptions[] = {
	{ "Projected",				API_Standard },
	{ "ProjectedWithOverhead",	API_StandardWithAbstract },
	{ "CutOnly",				API_CutOnly },
	{ "OutlinesOnly",			API_OutLinesOnly },
	{ "OverheadAll",			API_AbstractAll },
	{ "Symbolic",				API_CutAll },
};

const NamedValue kSkylightAnchors[] = {
	{ "BottomCenter",		APISkylightAnchor_BC },
	{ "BottomLeft",			APISkylightAnchor_BL },
	{ "BottomRight",		APISkylightAnchor_BR },
	{ "TopCenter",			APISkylightAnchor_TC },
	{ "TopLeft",			APISkylightAnchor_TL },
	{ "TopRight",			APISkylightAnchor_TR },
};

const NamedValue kSkylightFixModes[] = {
	{ "Horizontal",			APISkylightFixMode_Horizontal },
	{ "Vertical",			APISkylightFixMode_Vertical },
};

constexpr UInt32 kParamSummaryMax = 25;

// --- RAII for the window / door main marker sub-element ------------------------------

struct MarkerSubElement {
	API_SubElement sub;
	MarkerSubElement ()
	{
		BNZeroMemory (&sub, sizeof (sub));
		sub.subType = APISubElement_MainMarker;
	}
	~MarkerSubElement () { ACAPI_DisposeElemMemoHdls (&sub.memo); }
	MarkerSubElement (const MarkerSubElement&) = delete;
	MarkerSubElement& operator= (const MarkerSubElement&) = delete;
};


// When the tool default carries no marker parameters (e.g. the Window tool was never opened in
// this project), fall back to the marker parent library part like the DevKit sample does —
// ACAPI_Element_CreateExt refuses a marker without parameters.
void EnsureMarkerParams (const API_ElemType& type, API_SubElement& marker)
{
	if (marker.memo.params != nullptr && BMhGetSize (reinterpret_cast<GSHandle> (marker.memo.params)) >= (GSSize) sizeof (API_AddParType))
		return;

	API_LibPart lp;
	BNZeroMemory (&lp, sizeof (lp));
	if (ACAPI_Goodies_GetMarkerParent (type, lp) != NoError)
		return;
	const GSErrCode err = ACAPI_LibPart_Search (&lp, false, true);
	if (lp.location != nullptr) {
		delete lp.location;
		lp.location = nullptr;
	}
	if (err != NoError || lp.index <= 0)
		return;

	double a = 0.0, b = 0.0;
	Int32 addParNum = 0;
	API_AddParType** pars = nullptr;
	if (ACAPI_LibPart_GetParams (lp.index, &a, &b, &addParNum, &pars) != NoError || pars == nullptr)
		return;
	if (marker.memo.params != nullptr)
		ACAPI_DisposeAddParHdl (&marker.memo.params);
	marker.memo.params = pars;
	if (marker.subElem.object.pen <= 0)
		marker.subElem.object.pen = 1;
	marker.subElem.object.useObjPens = true;
}

// --- Opening base (common to windows, doors and skylights) ---------------------------

// T is the element struct used for masking (API_WindowType or API_SkylightType).
template <typename T>
void ApplyOpeningBaseFields (API_OpeningBaseType& base, API_Element* mask, const OS& spec)
{
#define OB_SET(field) if (mask) ACAPI_ELEMENT_MASK_SET (*mask, T, openingBase.field)
	if (auto v = OptDouble (spec, "width")) {
		if (*v <= 0.0) Fail ("'width' must be greater than 0 (meters).");
		base.width = *v; OB_SET (width);
	}
	if (auto v = OptDouble (spec, "height")) {
		if (*v <= 0.0) Fail ("'height' must be greater than 0 (meters).");
		base.height = *v; OB_SET (height);
	}
	if (auto v = OptDouble (spec, "subFloorThickness"))	{ base.subFloorThickness = *v; OB_SET (subFloorThickness); }

	if (auto v = OptBool (spec, "flipped")) {
		base.oSide = *v; OB_SET (oSide);
		if (!spec.Contains ("referenceSideFlipped")) { base.refSide = *v; OB_SET (refSide); }
	}
	if (auto v = OptBool (spec, "referenceSideFlipped"))	{ base.refSide = *v; OB_SET (refSide); }
	if (auto v = OptBool (spec, "mirrored"))				{ base.reflected = *v; OB_SET (reflected); }
	if (auto v = OptBool (spec, "inheritWallCut"))			{ base.wallCutUsing = *v; OB_SET (wallCutUsing); }

	// Attributes: giving an explicit attribute switches off "use the library part's own" for that
	// group unless the matching useObject* flag is given as well.
	if (auto v = OptAttr (API_MaterialID, spec, "surface")) {
		base.mat = *v; OB_SET (mat);
		if (!spec.Contains ("useObjectSurfaces")) { base.useObjMaterials = false; OB_SET (useObjMaterials); }
	}
	if (auto v = OptPen (spec, "pen")) {
		base.pen = *v; OB_SET (pen);
		if (!spec.Contains ("useObjectPens")) { base.useObjPens = false; OB_SET (useObjPens); }
	}
	if (auto v = OptAttr (API_LinetypeID, spec, "lineType")) {
		base.ltypeInd = *v; OB_SET (ltypeInd);
		if (!spec.Contains ("useObjectLineTypes")) { base.useObjLtypes = false; OB_SET (useObjLtypes); }
	}
	bool sectionGiven = false;
	if (auto v = OptAttr (API_FilltypeID, spec, "cutFill"))	{ base.sectFill = *v; OB_SET (sectFill); sectionGiven = true; }
	if (auto v = OptPen (spec, "cutFillPen"))					{ base.sectFillPen = *v; OB_SET (sectFillPen); sectionGiven = true; }
	if (auto v = OptPen (spec, "cutFillBackgroundPen", 0))		{ base.sectBGPen = *v; OB_SET (sectBGPen); sectionGiven = true; }
	if (auto v = OptPen (spec, "cutContourPen"))				{ base.sectContPen = *v; OB_SET (sectContPen); sectionGiven = true; }
	if (sectionGiven && !spec.Contains ("useObjectSectionAttributes")) { base.useObjSectAttrs = false; OB_SET (useObjSectAttrs); }

	if (auto v = OptBool (spec, "useObjectSurfaces"))			{ base.useObjMaterials = *v; OB_SET (useObjMaterials); }
	if (auto v = OptBool (spec, "useObjectPens"))				{ base.useObjPens = *v; OB_SET (useObjPens); }
	if (auto v = OptBool (spec, "useObjectLineTypes"))			{ base.useObjLtypes = *v; OB_SET (useObjLtypes); }
	if (auto v = OptBool (spec, "useObjectSectionAttributes"))	{ base.useObjSectAttrs = *v; OB_SET (useObjSectAttrs); }

	if (auto v = OptAttr (API_LinetypeID, spec, "cutLineType"))		{ base.cutLineType = *v; OB_SET (cutLineType); }
	if (auto v = OptPen (spec, "overheadLinePen"))					{ base.aboveViewLinePen = *v; OB_SET (aboveViewLinePen); }
	if (auto v = OptAttr (API_LinetypeID, spec, "overheadLineType"))	{ base.aboveViewLineType = *v; OB_SET (aboveViewLineType); }
	if (auto v = OptPen (spec, "uncutLinePen"))						{ base.belowViewLinePen = *v; OB_SET (belowViewLinePen); }
	if (auto v = OptAttr (API_LinetypeID, spec, "uncutLineType"))		{ base.belowViewLineType = *v; OB_SET (belowViewLineType); }
	if (spec.Contains ("displayOption")) {
		base.displayOption = (API_ElemDisplayOptionsID) ParseNamed (kDisplayOptions, spec, "displayOption");
		OB_SET (displayOption);
	}
#undef OB_SET
}


template <typename T>
void ApplyVerticalLink (API_OpeningBaseType& base, API_Element* mask, const OS& spec, const NamedValue* table, size_t tableSize,
						bool (*isStoryLink) (Int32), short defaultStory)
{
#define OB_SET(field) if (mask) ACAPI_ELEMENT_MASK_SET (*mask, T, openingBase.field)
	if (!spec.Contains ("verticalAnchor") && !spec.Contains ("verticalAnchorStory"))
		return;
	if (spec.Contains ("verticalAnchor")) {
		const GS::UniString s = GetString (spec, "verticalAnchor");
		bool found = false;
		for (size_t i = 0; i < tableSize; ++i) {
			if (EqualsIgnoreCase (s, table[i].name)) {
				base.verticalLink.linkType = (API_VerticalLinkID) table[i].value;
				found = true;
				break;
			}
		}
		if (!found) {
			GS::UniString allowed;
			for (size_t i = 0; i < tableSize; ++i) {
				if (!allowed.IsEmpty ()) allowed += ", ";
				allowed += table[i].name;
			}
			Fail ("Invalid value '" + s + "' for 'verticalAnchor'. Allowed: " + allowed + ".");
		}
		OB_SET (verticalLink.linkType);
	}
	if (isStoryLink (base.verticalLink.linkType)) {
		short story = defaultStory;
		if (auto st = OptStory (spec, "verticalAnchorStory"))
			story = *st;
		base.verticalLink.linkValue = story;
	} else {
		if (spec.Contains ("verticalAnchorStory"))
			Fail ("'verticalAnchorStory' is only used with a story-based verticalAnchor (e.g. 'SillToStory', 'HeaderToStory', 'ToStory').");
		base.verticalLink.linkValue = 0;
	}
	OB_SET (verticalLink.linkValue);
#undef OB_SET
}


bool IsWallStoryLink (Int32 t)		{ return t == API_LinkSillToStory || t == API_LinkHeaderToStory; }
bool IsSkylightStoryLink (Int32 t)	{ return t == API_LinkSkylightToStory; }


void AddOpeningBaseJson (OS& out, const API_OpeningBaseType& base)
{
	out.Add ("width", base.width);
	out.Add ("height", base.height);
	if (base.libInd > 0)
		out.Add ("libraryPart", LibPartBrief (base.libInd));
	else
		out.Add ("emptyHole", true);
	out.Add ("flipped", base.oSide);
	out.Add ("referenceSideFlipped", base.refSide);
	out.Add ("mirrored", base.reflected);
	out.Add ("inheritWallCut", base.wallCutUsing);
	out.Add ("subFloorThickness", base.subFloorThickness);
	out.Add ("verticalAnchor", NameOf (kAllVerticalLinks, base.verticalLink.linkType));
	if (IsWallStoryLink (base.verticalLink.linkType) || IsSkylightStoryLink (base.verticalLink.linkType))
		out.Add ("verticalAnchorStory", (Int32) base.verticalLink.linkValue);
	out.Add ("displayOption", NameOf (kDisplayOptions, base.displayOption));

	out.Add ("useObjectSurfaces", base.useObjMaterials);
	out.Add ("surface", AttrRef (API_MaterialID, base.mat));
	out.Add ("useObjectPens", base.useObjPens);
	out.Add ("pen", (Int32) base.pen);
	out.Add ("useObjectLineTypes", base.useObjLtypes);
	out.Add ("lineType", AttrRef (API_LinetypeID, base.ltypeInd));
	out.Add ("useObjectSectionAttributes", base.useObjSectAttrs);
	out.Add ("cutFill", AttrRef (API_FilltypeID, base.sectFill));
	out.Add ("cutFillPen", (Int32) base.sectFillPen);
	out.Add ("cutFillBackgroundPen", (Int32) base.sectBGPen);
	out.Add ("cutContourPen", (Int32) base.sectContPen);
	out.Add ("cutLineType", AttrRef (API_LinetypeID, base.cutLineType));
	out.Add ("overheadLinePen", (Int32) base.aboveViewLinePen);
	out.Add ("overheadLineType", AttrRef (API_LinetypeID, base.aboveViewLineType));
	out.Add ("uncutLinePen", (Int32) base.belowViewLinePen);
	out.Add ("uncutLineType", AttrRef (API_LinetypeID, base.belowViewLineType));
	if (base.markGuid != APINULLGuid)
		out.Add ("markerGuid", GuidStr (base.markGuid));
}


void AddGdlSummaryJson (OS& out, const API_Guid& guid)
{
	API_ElementMemo memo;
	BNZeroMemory (&memo, sizeof (memo));
	if (ACAPI_Element_GetMemo (guid, &memo, APIMemoMask_AddPars) == NoError && memo.params != nullptr) {
		UInt32 total = 0;
		OS summary = GdlParamsSummary (memo.params, kParamSummaryMax, &total);
		out.Add ("gdlParams", summary);
		out.Add ("gdlParamCount", (Int32) total);
		if (total > kParamSummaryMax)
			out.Add ("gdlParamsNote", GS::UniString ("Only the first visible parameters are listed; use get_gdl_parameters for all of them (with descriptions)."));
	}
	ACAPI_DisposeElemMemoHdls (&memo);
}

// --- Library part / GDL parameter handling -----------------------------------------------

std::optional<OS> OptParamValues (const OS& spec)
{
	OS values;
	if (!TryGetObject (spec, "gdlParams", values))
		return std::nullopt;
	return values;
}


// Before creation: swaps in the requested library part (or an empty hole). The part's default
// parameters are run through its parameter script together with gdlParams and the requested
// size, so memo.params and width/height stay consistent. Returns true when gdlParams have been
// applied here (no post-create step needed).
bool PrepareLibraryPartForCreate (const OS& spec, API_LibTypeID libType, const API_ElemType& elemType,
								  API_OpeningBaseType& base, API_ElementMemo& memo)
{
	if (GetBool (spec, "emptyHole", false)) {
		if (spec.Contains ("libraryPart") || spec.Contains ("gdlParams"))
			Fail ("'emptyHole' creates a plain hole without a library part; do not combine it with 'libraryPart' / 'gdlParams'.");
		base.libInd = 0;
		if (memo.params != nullptr)
			ACAPI_DisposeAddParHdl (&memo.params);
		return true;
	}
	if (!spec.Contains ("libraryPart"))
		return false;

	const API_LibPart lp = FindOpeningLibPart (spec, "libraryPart", elemType.typeID);

	const OS values = WithSizeParams (OptParamValues (spec).value_or (OS ()), OptDouble (spec, "width"), OptDouble (spec, "height"));
	double a = 0.0, b = 0.0;
	API_AddParType** params = ChangeParamsWithScriptForLibPart (lp.index, elemType, values, &a, &b);
	if (memo.params != nullptr)
		ACAPI_DisposeAddParHdl (&memo.params);
	memo.params = params;
	base.libInd = lp.index;
	if (a > 0.0) base.width = a;
	if (b > 0.0) base.height = b;
	return true;
}


// Modification: library part swap and/or GDL parameter changes (through the parameter script).
// Keeps width/height in sync with the resulting A/B parameters.
template <typename T>
void ApplyLibraryPartForModify (const API_Guid& guid, const API_ElemType& elemType, API_LibTypeID libType,
								API_OpeningBaseType& base, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask, const OS& patch)
{
#define OB_SET(field) ACAPI_ELEMENT_MASK_SET (mask, T, openingBase.field)
	if (patch.Contains ("emptyHole"))
		Fail ("'emptyHole' can only be used on creation. To change the library part pass 'libraryPart'.");
	const bool newPart = patch.Contains ("libraryPart");
	const std::optional<OS> values = OptParamValues (patch);
	if (!newPart && !values.has_value ())
		return;

	const std::optional<double> width = patch.Contains ("width") ? OptDouble (patch, "width") : std::optional<double> (base.width);
	const std::optional<double> height = patch.Contains ("height") ? OptDouble (patch, "height") : std::optional<double> (base.height);

	API_AddParType** params = nullptr;
	if (newPart) {
		const API_LibPart lp = FindOpeningLibPart (patch, "libraryPart", elemType.typeID);
		// Keep the current size unless a new one is given.
		const OS all = WithSizeParams (values.value_or (OS ()), width, height);
		double a = 0.0, b = 0.0;
		params = ChangeParamsWithScriptForLibPart (lp.index, elemType, all, &a, &b);
		base.libInd = lp.index;
		OB_SET (libInd);
	} else {
		if (base.libInd <= 0)
			Fail ("This element is an empty hole without a library part; give 'libraryPart' before setting 'gdlParams'.");
		const OS all = WithSizeParams (*values,
									   patch.Contains ("width") ? width : std::nullopt,
									   patch.Contains ("height") ? height : std::nullopt);
		params = ChangeParamsWithScript (guid, elemType, base.libInd, all);
	}
	if (memo.params != nullptr)
		ACAPI_DisposeAddParHdl (&memo.params);
	memo.params = params;
	memoMask |= APIMemoMask_AddPars;

	if (auto a = ParamReal (params, "A"); a.has_value () && *a > 0.0) { base.width = *a; OB_SET (width); }
	if (auto b = ParamReal (params, "B"); b.has_value () && *b > 0.0) { base.height = *b; OB_SET (height); }
#undef OB_SET
}


// After creation (tool-default library part + gdlParams): run the parameter script on the placed
// element. On failure the new element is deleted so the item fails atomically.
void ApplyParamsAfterCreate (const API_Guid& guid, const OS& values, const char* noun)
{
	try {
		API_Element element = GetElement (guid);
		const bool isSkylight = element.header.type.typeID == API_SkylightID;
		API_OpeningBaseType& base = isSkylight ? element.skylight.openingBase : element.window.openingBase;
		if (base.libInd <= 0)
			Fail ("The tool default is an empty hole; give 'libraryPart' together with 'gdlParams'.");

		Memo memo;
		memo->params = ChangeParamsWithScript (guid, element.header.type, base.libInd, values);
		API_Element mask;
		ACAPI_ELEMENT_MASK_CLEAR (mask);
		if (auto a = ParamReal (memo->params, "A"); a.has_value () && *a > 0.0) {
			base.width = *a;
			if (isSkylight) ACAPI_ELEMENT_MASK_SET (mask, API_SkylightType, openingBase.width);
			else			ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, openingBase.width);
		}
		if (auto b = ParamReal (memo->params, "B"); b.has_value () && *b > 0.0) {
			base.height = *b;
			if (isSkylight) ACAPI_ELEMENT_MASK_SET (mask, API_SkylightType, openingBase.height);
			else			ACAPI_ELEMENT_MASK_SET (mask, API_WindowType, openingBase.height);
		}
		Check (ACAPI_Element_Change (&element, &mask, memo.Ptr (), APIMemoMask_AddPars, true), "Cannot apply GDL parameters");
	} catch (const Error& e) {
		GS::Array<API_Guid> created;
		created.Push (guid);
		ACAPI_Element_Delete (created);
		throw Error ("The " + ToUni (noun) + " was not created because its GDL parameters could not be applied: " + e.message, e.code);
	}
}


GS::UniString CreateFailureHint (GSErrCode err, const char* noun)
{
	switch (err) {
		case APIERR_BADDATABASE:
			return " Archicad only creates " + ToUni (noun) + "s while the floor plan database is available; open the floor plan window and retry.";
		case APIERR_REFUSEDPAR:
		case APIERR_BADPARS:
			return " Check that the " + ToUni (noun) + " fits into its host (width vs. wall length, sillHeight + height vs. wall height), "
				   "that the library part has the right type, and that the host is editable (not on a locked/hidden layer).";
		case APIERR_NOTEDITABLE:
		case APIERR_LOCKEDLAY:
		case APIERR_HIDDENLAY:
			return " The host element or the target layer is not editable (locked / hidden layer or not reserved in Teamwork).";
		default:
			return GS::UniString ();
	}
}

// --- Windows / Doors -------------------------------------------------------------------

struct WindowDoorKind {
	API_ElemTypeID	typeID;
	API_LibTypeID	libType;
	const char*		noun;
};

const WindowDoorKind kWindowKind { API_WindowID, APILib_WindowID, "window" };
const WindowDoorKind kDoorKind { API_DoorID, APILib_DoorID, "door" };


// Applies all window / door fields except the library part. Width/height are applied before the
// position so positionReference "Begin"/"End" uses the final width.
void ApplyWindowDoorFields (API_WindowType& wd, API_Element* mask, const OS& spec, const WallGeometry& wall)
{
#define WD_SET(field) if (mask) ACAPI_ELEMENT_MASK_SET (*mask, API_WindowType, field)
	ApplyOpeningBaseFields<API_WindowType> (wd.openingBase, mask, spec);
	ApplyVerticalLink<API_WindowType> (wd.openingBase, mask, spec, kWallVerticalLinks, sizeof (kWallVerticalLinks) / sizeof (kWallVerticalLinks[0]),
									   IsWallStoryLink, wall.floorInd);

	// Vertical position. Archicad's 'lower' is ALWAYS the sill above the wall bottom, whatever the
	// verticalAnchor is (verified live: sill 0.9 in a wall with bottomOffset 0.5 sits at 1.4 with
	// SillToWallBottom, SillToStory and HeaderToWallTop alike). verticalAnchor only decides what
	// stays fixed when the wall / stories change later.
	const int verticalGiven = (spec.Contains ("sillHeight") ? 1 : 0) + (spec.Contains ("headerHeight") ? 1 : 0) + (spec.Contains ("sillElevation") ? 1 : 0);
	if (verticalGiven > 1)
		Fail ("Give only one of 'sillHeight' (sill above the wall bottom), 'headerHeight' (top above the wall bottom) or 'sillElevation' (sill above the home story level).");
	if (auto v = OptDouble (spec, "sillHeight")) {
		wd.lower = *v; WD_SET (lower);
	} else if (auto hh = OptDouble (spec, "headerHeight")) {
		wd.lower = *hh - wd.openingBase.height; WD_SET (lower);
	} else if (auto se = OptDouble (spec, "sillElevation")) {
		wd.lower = *se - wall.bottomOffset; WD_SET (lower);
	}
	if (spec.Contains ("anchor")) {
		wd.fixPoint = (short) ParseNamed (kFixPoints, spec, "anchor");
		WD_SET (fixPoint);
	}
	if (auto v = OptBool (spec, "reveal"))			{ wd.reveal = *v; WD_SET (reveal); }
	if (auto v = OptDouble (spec, "revealDepth"))	{ wd.revealDepthOffset = *v; WD_SET (revealDepthOffset); }
	if (spec.Contains ("revealDepthLocation")) {
		wd.revealDepthLocation = (API_WindowDoorRevealDepthLocationID) ParseNamed (kRevealLocations, spec, "revealDepthLocation");
		WD_SET (revealDepthLocation);
	}
	if (auto v = OptDouble (spec, "jambDepthHead"))	{ wd.jambDepthHead = *v; WD_SET (jambDepthHead); }
	if (auto v = OptDouble (spec, "jambDepthSill"))	{ wd.jambDepthSill = *v; WD_SET (jambDepthSill); }
	if (auto v = OptDouble (spec, "jambDepthLeft"))	{ wd.jambDepth = *v; WD_SET (jambDepth); }
	if (auto v = OptDouble (spec, "jambDepthRight"))	{ wd.jambDepth2 = *v; WD_SET (jambDepth2); }
	if (spec.Contains ("orientationInSlantedWall")) {
		wd.directionType = (API_WindowDoorDirectionTypes) ParseNamed (kDirectionTypes, spec, "orientationInSlantedWall");
		WD_SET (directionType);
	}

	// Position along the wall (last: needs the final width).
	//   position + Center (default): wall begin -> opening centre
	//   position + Begin: wall begin -> the opening edge facing the wall begin (clear gap at the begin)
	//   position + End:   wall END   -> the opening edge facing the wall end   (clear gap at the end)
	//   point + Center / Begin / End: the projected point is the centre / begin-side edge / end-side edge
	std::optional<double> distance;
	bool fromPoint = false;
	if (spec.Contains ("point")) {
		if (spec.Contains ("position"))
			Fail ("Give either 'position' (distance along the wall) or 'point' ({x,y}), not both.");
		distance = ProjectOntoWall (wall, GetCoord (spec, "point"), "point").distance;
		fromPoint = true;
	} else if (auto v = OptDouble (spec, "position")) {
		distance = *v;
	}
	if (spec.Contains ("positionReference") && !distance.has_value ())
		Fail ("'positionReference' needs 'position' or 'point' in the same item.");
	if (distance.has_value ()) {
		const Int32 ref = spec.Contains ("positionReference") ? ParseNamed (kPositionReferences, spec, "positionReference") : PosRef_Center;
		const double half = wd.openingBase.width / 2.0;
		double center = *distance;
		if (ref == PosRef_Begin)
			center = *distance + half;
		else if (ref == PosRef_End)
			center = fromPoint ? *distance - half : wall.length - *distance - half;
		CheckDistanceOnWall (wall, center, ref == PosRef_Center ? "The opening centre" : "The opening centre computed from positionReference");
		wd.objLoc = center;
		WD_SET (objLoc);
	}
#undef WD_SET
}


API_Guid CreateWindowOrDoor (const WindowDoorKind& kind, const OS& spec)
{
	const std::optional<API_Guid> wallGuid = OptGuidAny (spec, { "wall", "owner" });
	if (!wallGuid.has_value ())
		Fail ("A " + ToUni (kind.noun) + " needs 'wall': the GUID of the host wall (see the wall's details / element queries).");
	if (!spec.Contains ("position") && !spec.Contains ("point"))
		Fail ("A " + ToUni (kind.noun) + " needs 'position' (m from the wall's begin point to the opening centre, along the reference line) or 'point' ({x,y}, projected onto the wall).");
	const WallGeometry wall = LoadHostWall (*wallGuid, kind.noun);

	FloorPlanDatabaseScope floorPlan;

	API_Element element = NewElement (kind.typeID);
	Memo memo;
	MarkerSubElement marker;
	Check (ACAPI_Element_GetDefaultsExt (&element, memo.Ptr (), 1, &marker.sub),
		   "Cannot get the default settings of the " + ToUni (kind.noun) + " tool");
	EnsureMarkerParams (element.header.type, marker.sub);

	ApplyCommonFields (element, nullptr, spec);
	element.header.floorInd = wall.floorInd;			// always the wall's home story

	API_WindowType& wd = element.window;				// API_DoorType is the same struct
	const bool paramsApplied = PrepareLibraryPartForCreate (spec, kind.libType, element.header.type, wd.openingBase, *memo);
	if (wd.openingBase.libInd <= 0 && !GetBool (spec, "emptyHole", false))
		Fail ("The " + ToUni (kind.noun) + " tool has no default library part; pass 'libraryPart' (find one with search_library_parts, query '" +
			  ToUni (kind.typeID == API_DoorID ? "дверь" : "окно") + "').");

	wd.owner = *wallGuid;
	ApplyWindowDoorFields (wd, nullptr, spec, wall);

	const GSErrCode err = ACAPI_Element_CreateExt (&element, memo.Ptr (), 1, &marker.sub);
	if (err != NoError)
		Fail ("Cannot create " + ToUni (kind.noun) + ": " + ErrorName (err) + "." + CreateFailureHint (err, kind.noun), err);

	if (!paramsApplied) {
		if (auto values = OptParamValues (spec))
			ApplyParamsAfterCreate (element.header.guid, *values, kind.noun);
	}
	return element.header.guid;
}


void SerializeWindowDoor (const API_Element& element, OS& out)
{
	const API_WindowType& wd = element.window;
	out.Add ("wall", GuidStr (wd.owner));
	out.Add ("position", wd.objLoc);
	std::optional<WallGeometry> wall;
	try {
		wall = LoadHostWall (wd.owner, "window");
	} catch (const Error&) {
		// host not readable: position alone is still meaningful
	}
	if (wall.has_value ()) {
		const double half = wd.openingBase.width / 2.0;
		out.Add ("wallLength", wall->length);
		out.Add ("edgeDistanceFromBegin", wd.objLoc - half);			// = position for positionReference 'Begin'
		out.Add ("edgeDistanceFromEnd", wall->length - wd.objLoc - half);	// = position for positionReference 'End'
		API_Coord location;
		double tangent = 0.0;
		if (PointOnWall (*wall, wd.objLoc, location, tangent)) {
			out.Add ("location", CoordObj (location));
			AddAngle (out, "wallDirection", tangent);
		}
	}
	out.Add ("sillHeight", wd.lower);
	out.Add ("headerHeight", wd.lower + wd.openingBase.height);
	if (wall.has_value ()) {
		out.Add ("sillElevation", wall->bottomOffset + wd.lower);
		out.Add ("headerElevation", wall->bottomOffset + wd.lower + wd.openingBase.height);
	}
	out.Add ("anchor", NameOf (kFixPoints, wd.fixPoint));
	AddOpeningBaseJson (out, wd.openingBase);
	out.Add ("reveal", wd.reveal);
	out.Add ("revealDepth", wd.revealDepthOffset);
	out.Add ("revealDepthLocation", NameOf (kRevealLocations, wd.revealDepthLocation));
	out.Add ("revealDepthFromSide", wd.revealDepthFromSide);
	out.Add ("jambDepthHead", wd.jambDepthHead);
	out.Add ("jambDepthSill", wd.jambDepthSill);
	out.Add ("jambDepthLeft", wd.jambDepth);
	out.Add ("jambDepthRight", wd.jambDepth2);
	out.Add ("orientationInSlantedWall", NameOf (kDirectionTypes, wd.directionType));
	AddGdlSummaryJson (out, element.header.guid);
}


void ModifyWindowOrDoor (const WindowDoorKind& kind, API_Element& element, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask, const OS& patch)
{
	API_WindowType& wd = element.window;
	if (auto newWall = OptGuidAny (patch, { "wall", "owner" }); newWall.has_value () && *newWall != wd.owner)
		Fail ("A " + ToUni (kind.noun) + " cannot be moved into another wall. Delete it and create a new one in the target wall.", APIERR_NOTSUPPORTED);

	ApplyLibraryPartForModify<API_WindowType> (element.header.guid, element.header.type, kind.libType, wd.openingBase, mask, memo, memoMask, patch);

	WallGeometry wall;
	if (patch.Contains ("position") || patch.Contains ("point") || patch.Contains ("verticalAnchor") || patch.Contains ("verticalAnchorStory") ||
		patch.Contains ("sillElevation"))
		wall = LoadHostWall (wd.owner, kind.noun);
	else
		wall.floorInd = element.header.floorInd;
	ApplyWindowDoorFields (wd, &mask, patch, wall);
}

// --- Skylights -----------------------------------------------------------------------------

short LoadSkylightHost (const API_Guid& guid)
{
	const API_Elem_Head head = GetHeader (guid);
	if (head.type.typeID != API_RoofID && head.type.typeID != API_ShellID) {
		Fail ("Element " + GuidStr (guid) + " is a " + ElemTypeName (head.type) +
			  ", but a skylight can only be placed into a Roof or a Shell. Pass the GUID of a roof/shell.", APIERR_BADELEMENTTYPE);
	}
	return head.floorInd;
}


void ApplySkylightFields (API_SkylightType& sk, API_Element* mask, const OS& spec, short hostStory)
{
#define SK_SET(field) if (mask) ACAPI_ELEMENT_MASK_SET (*mask, API_SkylightType, field)
	ApplyOpeningBaseFields<API_SkylightType> (sk.openingBase, mask, spec);
	ApplyVerticalLink<API_SkylightType> (sk.openingBase, mask, spec, kSkylightVerticalLinks, sizeof (kSkylightVerticalLinks) / sizeof (kSkylightVerticalLinks[0]),
										 IsSkylightStoryLink, hostStory);
	if (auto p = OptCoord (spec, "point")) {
		sk.anchorPosition = *p;
		SK_SET (anchorPosition.x); SK_SET (anchorPosition.y);
	}
	if (auto v = OptDouble (spec, "anchorLevel"))	{ sk.anchorLevel = *v; SK_SET (anchorLevel); }
	if (spec.Contains ("anchor")) {
		sk.anchorPoint = (API_SkylightAnchorID) ParseNamed (kSkylightAnchors, spec, "anchor");
		SK_SET (anchorPoint);
	}
	if (spec.Contains ("fixMode")) {
		sk.fixMode = (API_SkylightFixModeID) ParseNamed (kSkylightFixModes, spec, "fixMode");
		SK_SET (fixMode);
	}
	if (auto v = OptAngle (spec, "azimuthAngle"))	{ sk.azimuthAngle = *v; SK_SET (azimuthAngle); }
	if (auto v = OptInt (spec, "pivotVertexId")) {
		if (*v < 0) Fail ("'pivotVertexId' must be >= 0.");
		sk.vertexID = (UInt32) *v; SK_SET (vertexID);
	}
#undef SK_SET
}


// Even-odd point-in-polygon test over all contours of a memo polygon (straight edges only).
// Returns nullopt when the polygon cannot be tested (arcs, missing data).
std::optional<bool> PointInMemoPolygon (const API_ElementMemo& memo, const API_Coord& p)
{
	if (memo.coords == nullptr || memo.pends == nullptr)
		return std::nullopt;
	if (memo.parcs != nullptr && BMhGetSize (reinterpret_cast<GSHandle> (memo.parcs)) >= (GSSize) sizeof (API_PolyArc))
		return std::nullopt;
	const Int32 nPends = (Int32) (BMhGetSize (reinterpret_cast<GSHandle> (memo.pends)) / sizeof (Int32));
	const Int32 nCoords = (Int32) (BMhGetSize (reinterpret_cast<GSHandle> (memo.coords)) / sizeof (API_Coord));
	bool inside = false;
	Int32 begin = 1;
	for (Int32 k = 1; k < nPends; ++k) {
		const Int32 end = (*memo.pends)[k];						// index of the closing vertex (== first vertex)
		if (end >= nCoords || end <= begin)
			return std::nullopt;
		for (Int32 i = begin; i < end; ++i) {
			const API_Coord& a = (*memo.coords)[i];
			const API_Coord& b = (*memo.coords)[i + 1];
			if ((a.y > p.y) != (b.y > p.y)) {
				const double x = a.x + (p.y - a.y) * (b.x - a.x) / (b.y - a.y);
				if (p.x < x)
					inside = !inside;
			}
		}
		begin = end + 1;
	}
	return inside;
}


// Throws when a skylight point lies outside its host in plan (Archicad itself accepts such a
// point and creates an orphan skylight at level 0).
void CheckPointOnSkylightHost (const API_Guid& host, const API_Coord& p)
{
	API_Elem_Head head = GetHeader (host);
	char buf[160];
	std::snprintf (buf, sizeof (buf), "(%.3f, %.3f)", p.x, p.y);
	const GS::UniString where = ToUni (buf);
	API_Box3D box;
	BNZeroMemory (&box, sizeof (box));
	if (ACAPI_Database (APIDb_CalcBoundsID, &head, &box) == NoError && box.xMax > box.xMin) {
		const double tol = 0.001;
		if (p.x < box.xMin - tol || p.x > box.xMax + tol || p.y < box.yMin - tol || p.y > box.yMax + tol) {
			std::snprintf (buf, sizeof (buf), "%.3f..%.3f, y %.3f..%.3f", box.xMin, box.xMax, box.yMin, box.yMax);
			Fail ("The skylight point " + where + " is outside the host " + ElemTypeName (head.type) + " (its plan extent is x " +
				  ToUni (buf) + "). Give a point on the roof/shell (see get_element_details / get_bounding_boxes of the host).", APIERR_BADPARS);
		}
	}
	if (head.type.typeID == API_RoofID) {
		API_ElementMemo memo;
		BNZeroMemory (&memo, sizeof (memo));
		std::optional<bool> inside;
		if (ACAPI_Element_GetMemo (host, &memo, APIMemoMask_Polygon) == NoError)
			inside = PointInMemoPolygon (memo, p);
		ACAPI_DisposeElemMemoHdls (&memo);
		if (inside.has_value () && !*inside)
			Fail ("The skylight point " + where + " is not on the roof in plan (outside its contour or inside a hole). "
				  "Give a point inside the roof outline (get_element_details of the roof shows its polygon).", APIERR_BADPARS);
	}
}


// Best-effort absolute Z of a single-plane roof's pivot plane above a plan point, used as the
// skylight anchorLevel when none is given (multi-plane roofs / shells keep the tool value).
std::optional<double> PlaneRoofLevelAt (const API_Guid& roofGuid, const API_Coord& p)
{
	API_Element roof;
	BNZeroMemory (&roof, sizeof (roof));
	roof.header.guid = roofGuid;
	if (ACAPI_Element_Get (&roof) != NoError || roof.header.type.typeID != API_RoofID || roof.roof.roofClass != API_PlaneRoofID)
		return std::nullopt;
	const API_PlaneRoofData& pr = roof.roof.u.planeRoof;
	const double dx = pr.baseLine.c2.x - pr.baseLine.c1.x;
	const double dy = pr.baseLine.c2.y - pr.baseLine.c1.y;
	const double len = std::hypot (dx, dy);
	if (len < 1e-9)
		return std::nullopt;
	double signedDist = (dx * (p.y - pr.baseLine.c1.y) - dy * (p.x - pr.baseLine.c1.x)) / len;
	if (!pr.posSign)
		signedDist = -signedDist;
	return StoryLevel (roof.header.floorInd) + roof.roof.shellBase.level + signedDist * std::tan (pr.angle);
}


API_Guid CreateSkylight (const OS& spec)
{
	const std::optional<API_Guid> owner = OptGuidAny (spec, { "owner", "roof", "shell" });
	if (!owner.has_value ())
		Fail ("A skylight needs 'owner': the GUID of the host Roof or Shell.");
	if (!spec.Contains ("point"))
		Fail ("A skylight needs 'point' ({x,y}): the plan position of its anchor point on the roof.");
	const short hostStory = LoadSkylightHost (*owner);

	FloorPlanDatabaseScope floorPlan;

	API_Element element = NewElement (API_SkylightID);
	Memo memo;
	GetDefaults (element, memo.Ptr ());
	ApplyCommonFields (element, nullptr, spec);
	element.header.floorInd = hostStory;

	API_SkylightType& sk = element.skylight;
	const bool paramsApplied = PrepareLibraryPartForCreate (spec, APILib_SkylightID, element.header.type, sk.openingBase, *memo);
	if (sk.openingBase.libInd <= 0 && !GetBool (spec, "emptyHole", false))
		Fail (ToUni ("The Skylight tool has no default library part; pass 'libraryPart' (search_library_parts, query 'мансардное окно')."));

	sk.owner = *owner;
	ApplySkylightFields (sk, nullptr, spec, hostStory);
	CheckPointOnSkylightHost (*owner, sk.anchorPosition);
	if (!spec.Contains ("anchorLevel")) {
		if (auto level = PlaneRoofLevelAt (*owner, sk.anchorPosition))
			sk.anchorLevel = *level;
	}

	const GSErrCode err = ACAPI_Element_Create (&element, memo.Ptr ());
	if (err != NoError)
		Fail ("Cannot create skylight: " + ErrorName (err) + "." + CreateFailureHint (err, "skylight") +
			  " The point must lie on the roof/shell in plan.", err);

	if (!paramsApplied) {
		if (auto values = OptParamValues (spec))
			ApplyParamsAfterCreate (element.header.guid, *values, "skylight");
	}
	return element.header.guid;
}


void SerializeSkylight (const API_Element& element, OS& out)
{
	const API_SkylightType& sk = element.skylight;
	out.Add ("owner", GuidStr (sk.owner));
	API_Elem_Head head;
	BNZeroMemory (&head, sizeof (head));
	head.guid = sk.owner;
	if (ACAPI_Element_GetHeader (&head) == NoError)
		out.Add ("ownerType", ElemTypeName (head.type));
	out.Add ("point", CoordObj (sk.anchorPosition));
	out.Add ("anchorLevel", sk.anchorLevel);
	out.Add ("anchor", NameOf (kSkylightAnchors, sk.anchorPoint));
	out.Add ("fixMode", NameOf (kSkylightFixModes, sk.fixMode));
	AddAngle (out, "azimuthAngle", sk.azimuthAngle);
	AddAngle (out, "roofSlope", sk.elevationAngle);
	out.Add ("pivotVertexId", (Int32) sk.vertexID);
	AddOpeningBaseJson (out, sk.openingBase);
	AddGdlSummaryJson (out, element.header.guid);
}


void ModifySkylight (API_Element& element, API_Element& mask, API_ElementMemo& memo, UInt64& memoMask, const OS& patch)
{
	API_SkylightType& sk = element.skylight;
	if (auto newOwner = OptGuidAny (patch, { "owner", "roof", "shell" }); newOwner.has_value () && *newOwner != sk.owner)
		Fail ("A skylight cannot be moved into another roof/shell. Delete it and create a new one.", APIERR_NOTSUPPORTED);
	ApplyLibraryPartForModify<API_SkylightType> (element.header.guid, element.header.type, APILib_SkylightID, sk.openingBase, mask, memo, memoMask, patch);
	ApplySkylightFields (sk, &mask, patch, element.header.floorInd);
	if (patch.Contains ("point"))
		CheckPointOnSkylightHost (sk.owner, sk.anchorPosition);
	if (patch.Contains ("point") && !patch.Contains ("anchorLevel")) {
		if (auto level = PlaneRoofLevelAt (sk.owner, sk.anchorPosition)) {
			sk.anchorLevel = *level;
			ACAPI_ELEMENT_MASK_SET (mask, API_SkylightType, anchorLevel);
		}
	}
}

// --- GetHostOpenings ---------------------------------------------------------------------

void AddConnected (OS& item, const char* key, const API_Guid& host, API_ElemTypeID typeID, bool withDetails)
{
	GS::Array<API_Guid> list;
	if (ACAPI_Element_GetConnectedElements (host, API_ElemType (typeID), &list) != NoError)
		list.Clear ();
	if (withDetails) {
		GS::Array<OS> items;
		for (const API_Guid& g : list)
			items.Push (Try ([&] () { return ElementToJson (g); }));
		item.Add (key, items);
	} else {
		GS::Array<GS::UniString> guids;
		for (const API_Guid& g : list)
			guids.Push (GuidStr (g));
		item.Add (key, guids);
	}
}

} // namespace


void RegisterOpeningCommands ()
{
	RegisterAdapter ({ API_WindowID,
		[] (const OS& spec) { return CreateWindowOrDoor (kWindowKind, spec); },
		SerializeWindowDoor,
		[] (API_Element& e, API_Element& m, API_ElementMemo& memo, UInt64& memoMask, const OS& patch) { ModifyWindowOrDoor (kWindowKind, e, m, memo, memoMask, patch); } });
	RegisterAdapter ({ API_DoorID,
		[] (const OS& spec) { return CreateWindowOrDoor (kDoorKind, spec); },
		SerializeWindowDoor,
		[] (API_Element& e, API_Element& m, API_ElementMemo& memo, UInt64& memoMask, const OS& patch) { ModifyWindowOrDoor (kDoorKind, e, m, memo, memoMask, patch); } });
	RegisterAdapter ({ API_SkylightID, CreateSkylight, SerializeSkylight, ModifySkylight });
	RegisterOpeningToolAdapter ();

	RegisterCommand ("GetHostOpenings",
		"Lists the windows, doors, skylights and Opening-tool openings placed in host elements (walls, roofs, shells, slabs, beams, ...). "
		"Input: {hosts: [guid], details?: bool (default false: GUIDs only; true: full element details)}. "
		"Output: {hosts: [{guid, type, windows, doors, skylights, openings} | {error}]}.",
		[] (const OS& params) -> OS {
			const GS::Array<API_Guid> hosts = GetGuidArray (params, "hosts", true);
			const bool details = GetBool (params, "details", false);
			GS::Array<OS> out;
			for (const API_Guid& host : hosts) {
				out.Push (Try ([&] () -> OS {
					const API_Elem_Head head = GetHeader (host);
					OS item ("guid", GuidStr (host), "type", ElemTypeName (head.type));
					AddConnected (item, "windows", host, API_WindowID, details);
					AddConnected (item, "doors", host, API_DoorID, details);
					AddConnected (item, "skylights", host, API_SkylightID, details);
					AddConnected (item, "openings", host, API_OpeningID, details);
					return item;
				}));
			}
			return OS ("hosts", out);
		});
}

} // namespace cc
