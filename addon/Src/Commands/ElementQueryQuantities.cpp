// *****************************************************************************
// ElementQueryQuantities — GetElementQuantities (family "element-query").
//
// ACAPI_Element_GetQuantities per element: every field of the type's
// API_*Quantity struct with descriptive camelCase names, composite (skin)
// volumes per building material, optional element-part quantities, per-type
// totals and exposed surfaces (ACAPI_Element_GetSurfaceQuantities).
// Units: m, m2, m3, degrees (angles converted from radians), counts as integers.
// *****************************************************************************

#include "Commands/ElementQueryShared.hpp"
#include "Core/Command.hpp"

#include <map>
#include <string>

namespace cc {
namespace eq {

namespace {

using Sums = std::map<std::string, double>;

// Writes quantity fields; additive ones (lengths, areas, volumes, counts) are also summed
// into `sums` for the per-type totals.
class QWriter {
public:
	QWriter (OS& out, Sums* sums) : out (out), sums (sums) {}

	void Len (const char* k, double v)		{ out.Add (k, v); Sum (k, v); }	// additive length (m)
	void Dim (const char* k, double v)		{ out.Add (k, v); }				// non-additive length: height, thickness, level (m)
	void Area (const char* k, double v)		{ out.Add (k, v); Sum (k, v); }	// m2
	void AreaNA (const char* k, double v)	{ out.Add (k, v); }				// m2, not summed (cross sections)
	void Vol (const char* k, double v)		{ out.Add (k, v); Sum (k, v); }	// m3
	void Count (const char* k, Int32 v)		{ out.Add (k, v); Sum (k, (double) v); }
	void Ang (const char* k, double rad)	{ out.Add (k, RadToDeg (rad)); }	// degrees
	void Raw (const char* k, double v)		{ out.Add (k, v); }
	void RawInt (const char* k, Int32 v)	{ out.Add (k, v); }
	void Text (const char* k, const GS::uchar_t* s)
	{
		GS::UniString str (s);
		if (!str.IsEmpty ())
			out.Add (k, str);
	}
	void Surf (const char* k, API_AttributeIndex idx)
	{
		if (idx > 0)
			out.Add (k, AttrRef (API_MaterialID, idx));
	}

private:
	void Sum (const char* k, double v)
	{
		if (sums != nullptr)
			(*sums)[k] += v;
	}

	OS&		out;
	Sums*	sums;
};


template <typename T>
void RailPart (const T& q, QWriter& w)
{
	w.Vol ("volume", q.volume);
	w.Len ("length3D", q.length3D);
}


// Returns false when the type has no quantity structure.
bool WriteQuantities (API_ElemTypeID typeID, const API_ElementQuantity& q, QWriter& w)
{
	switch (typeID) {
		case API_WallID: {
			const API_WallQuantity& x = q.wall;
			w.Vol ("volume", x.volume);
			w.Vol ("volumeConditional", x.volume_cond);
			w.Vol ("grossVolume", x.grossVolume);
			w.Vol ("volumeSkinA", x.volumeASkin);
			w.Vol ("volumeSkinB", x.volumeBSkin);
			w.Vol ("volumeSkinAConditional", x.volumeASkin_cond);
			w.Vol ("volumeSkinBConditional", x.volumeBSkin_cond);
			w.Area ("surfaceReferenceSide", x.surface1);
			w.Area ("surfaceOppositeSide", x.surface2);
			w.Area ("surfaceEdges", x.surface3);
			w.Area ("surfaceReferenceSideConditional", x.surface1_cond);
			w.Area ("surfaceOppositeSideConditional", x.surface2_cond);
			w.Area ("grossSurfaceReferenceSide", x.grossSurf1);
			w.Area ("grossSurfaceOppositeSide", x.grossSurf2);
			w.Area ("area", x.area);
			w.Len ("perimeter", x.perimeter);
			w.Len ("length", x.length);
			w.Len ("centerLength", x.centerLength);
			w.Len ("referenceLineLength", x.refLineLength);
			w.Len ("lengthReferenceSide", x.length12);
			w.Len ("lengthOppositeSide", x.length34);
			w.Len ("lengthReferenceSideConditional", x.length12_cond);
			w.Len ("lengthOppositeSideConditional", x.length34_cond);
			w.Dim ("minHeight", x.minHeight);
			w.Dim ("maxHeight", x.maxHeight);
			w.Dim ("minHeightSkinA", x.minHeightASkin);
			w.Dim ("maxHeightSkinA", x.maxHeightASkin);
			w.Dim ("minHeightSkinB", x.minHeightBSkin);
			w.Dim ("maxHeightSkinB", x.maxHeightBSkin);
			w.Dim ("skinAThickness", x.skinAThickness);
			w.Dim ("skinBThickness", x.skinBThickness);
			w.Dim ("insulationThickness", x.insuThickness);
			w.Dim ("airGapThickness", x.airThickness);
			w.Area ("windowsSurface", x.windowsSurf);
			w.Area ("doorsSurface", x.doorsSurf);
			w.Area ("emptyOpeningsSurface", x.emptyholesSurf);
			w.Len ("windowsWidth", x.windowsWidth);
			w.Len ("doorsWidth", x.doorsWidth);
			w.Vol ("emptyOpeningsVolume", x.emptyHolesVolume);
			w.Area ("emptyOpeningsSurfaceReferenceSide", x.emptyHolesSurf1);
			w.Area ("emptyOpeningsSurfaceOppositeSide", x.emptyHolesSurf2);
			w.Count ("columnsCount", x.columnsNumber);
			return true;
		}
		case API_ColumnID: {
			const API_ColumnQuantity& x = q.column;
			w.Vol ("coreVolume", x.coreVolume);
			w.Vol ("veneerVolume", x.veneVolume);
			w.Vol ("coreGrossVolume", x.coreGrossVolume);
			w.Vol ("veneerGrossVolume", x.veneGrossVolume);
			w.Area ("coreSurface", x.coreSurface);
			w.Area ("veneerSurface", x.veneSurface);
			w.Area ("coreGrossSurface", x.coreGrossSurf);
			w.Area ("veneerGrossSurface", x.veneGrossSurf);
			w.Area ("coreTopSurface", x.coreTopSurf);
			w.Area ("coreBottomSurface", x.coreBotSurf);
			w.Area ("veneerTopSurface", x.veneTopSurf);
			w.Area ("veneerBottomSurface", x.veneBotSurf);
			w.Area ("coreGrossTopBottomSurface", x.coreGrossTopBotSurf);
			w.Area ("veneerGrossTopBottomSurface", x.veneGrossTopBotSurf);
			w.Area ("area", x.area);
			w.Len ("perimeter", x.perimeter);
			w.AreaNA ("crossSectionAreaCutBegin", x.crossSectionAreaCutBegin);
			w.AreaNA ("crossSectionAreaCutEnd", x.crossSectionAreaCutEnd);
			w.Dim ("minHeight", x.minHeight);
			w.Dim ("maxHeight", x.maxHeight);
			return true;
		}
		case API_ColumnSegmentID: {
			const API_ColumnSegmentQuantity& x = q.columnSegment;
			w.Vol ("volume", x.volume);
			w.Vol ("grossVolume", x.grossVolume);
			w.Vol ("coreVolume", x.coreVolume);
			w.Vol ("coreGrossVolume", x.coreGrossVolume);
			w.Vol ("veneerNetVolume", x.veneerNetVolume);
			w.Vol ("veneerGrossVolume", x.veneerGrossVolume);
			w.Area ("coreNetSurface", x.coreNetSurface);
			w.Area ("coreNetTopSurface", x.coreNetTopSurface);
			w.Area ("coreNetBottomSurface", x.coreNetBottomSurface);
			w.Area ("coreGrossSurface", x.coreGrossSurface);
			w.Area ("coreGrossTopSurface", x.coreGrossTopSurface);
			w.Area ("coreGrossBottomSurface", x.coreGrossBottomSurface);
			w.Area ("veneerSideSurface", x.veneerSideSurface);
			w.Area ("veneerTopSurface", x.veneerTopSurface);
			w.Area ("veneerBottomSurface", x.veneerBottomSurface);
			w.Area ("veneerGrossSurface", x.veneerGrossSurface);
			w.Area ("veneerGrossTopSurface", x.veneerGrossTopSurface);
			w.Area ("veneerGrossBottomSurface", x.veneerGrossBottomSurface);
			w.Area ("area", x.area);
			w.Len ("length", x.length);
			w.AreaNA ("crossSectionAreaCutBegin", x.crossSectionAreaCutBegin);
			w.AreaNA ("crossSectionAreaCutEnd", x.crossSectionAreaCutEnd);
			w.Dim ("height", x.height);
			w.Dim ("minHeight", x.minHeight);
			w.Dim ("maxHeight", x.maxHeight);
			return true;
		}
		case API_BeamID: {
			const API_BeamQuantity& x = q.beam;
			w.Vol ("volume", x.volume);
			w.Vol ("volumeConditional", x.volume_cond);
			w.Vol ("holesVolume", x.holesVolume);
			w.Area ("area", x.area);
			w.Area ("topSurface", x.topSurface);
			w.Area ("bottomSurface", x.bottomSurface);
			w.Area ("edgeSurface", x.edgeSurface);
			w.Area ("edgeSurfaceLeft", x.edgeSurfaceLeft);
			w.Area ("edgeSurfaceRight", x.edgeSurfaceRight);
			w.Area ("holesSurface", x.holesSurface);
			w.Area ("holesEdgeSurface", x.holesEdgeSurface);
			w.Len ("leftLength", x.leftLength);
			w.Len ("rightLength", x.rightLength);
			w.AreaNA ("crossSectionAreaCutBegin", x.crossSectionAreaCutBegin);
			w.AreaNA ("crossSectionAreaCutEnd", x.crossSectionAreaCutEnd);
			w.Count ("holesCount", x.holesNumber);
			return true;
		}
		case API_BeamSegmentID: {
			const API_BeamSegmentQuantity& x = q.beamSegment;
			w.Vol ("volume", x.volume);
			w.Vol ("holesVolume", x.holesVolume);
			w.Area ("topSurface", x.topSurface);
			w.Area ("bottomSurface", x.bottomSurface);
			w.Area ("leftSurface", x.leftSurface);
			w.Area ("rightSurface", x.rightSurface);
			w.Area ("endSurface", x.endSurface);
			w.Area ("holesSurface", x.holesSurface);
			w.Area ("holesEdgeSurface", x.holesEdgeSurface);
			w.Len ("length", x.length);
			w.Len ("leftLength", x.leftLength);
			w.Len ("rightLength", x.rightLength);
			w.AreaNA ("crossSectionAreaCutBegin", x.crossSectionAreaCutBegin);
			w.AreaNA ("crossSectionAreaCutEnd", x.crossSectionAreaCutEnd);
			w.Count ("holesCount", x.holesNumber);
			return true;
		}
		case API_WindowID:
		case API_DoorID: {
			const API_WindowQuantity& x = (typeID == API_DoorID) ? q.door : q.window;
			w.Area ("surface", x.surface);
			w.Area ("grossSurface", x.grossSurf);
			w.Vol ("volume", x.volume);
			w.Vol ("grossVolume", x.grossVolume);
			w.Len ("revealSideWidth", x.width1);
			w.Len ("oppositeSideWidth", x.width2);
			w.Dim ("revealSideHeight", x.height1);
			w.Dim ("oppositeSideHeight", x.height2);
			w.Area ("revealSideSurface", x.surface1);
			w.Area ("oppositeSideSurface", x.surface2);
			w.Len ("nominalRevealSideWidth", x.nWidth1);
			w.Len ("nominalOppositeSideWidth", x.nWidth2);
			w.Dim ("nominalRevealSideHeight", x.nHeight1);
			w.Dim ("nominalOppositeSideHeight", x.nHeight2);
			w.Area ("nominalRevealSideSurface", x.nSurface1);
			w.Area ("nominalOppositeSideSurface", x.nSurface2);
			w.Dim ("sillHeight", x.sillHeight);
			w.Dim ("revealSideSillHeight", x.sillHeight1);
			w.Dim ("oppositeSideSillHeight", x.sillHeight2);
			w.Dim ("sillHeight3", x.sillHeight3);
			w.Dim ("headHeight", x.headHeight);
			w.Dim ("revealSideHeadHeight", x.headHeight1);
			w.Dim ("oppositeSideHeadHeight", x.headHeight2);
			w.Dim ("headHeight3", x.headHeight3);
			return true;
		}
		case API_SkylightID: {
			const API_SkylightQuantity& x = q.skylight;
			w.Len ("openingWidth", x.openingWidth);
			w.Dim ("openingHeight", x.openingHeight);
			w.Area ("openingSurface", x.openingSurface);
			w.Vol ("openingVolume", x.openingVolume);
			w.Dim ("sillHeight", x.sillHeight);
			w.Dim ("headerHeight", x.headerHeight);
			return true;
		}
		case API_ObjectID:
			w.Area ("surface", q.symb.surface);
			w.Vol ("volume", q.symb.volume);
			return true;
		case API_LampID:
			w.Area ("surface", q.light.surface);
			w.Vol ("volume", q.light.volume);
			return true;
		case API_MorphID: {
			const API_MorphQuantity& x = q.morph;
			w.Area ("surface", x.surface);
			w.Vol ("volume", x.volume);
			w.Area ("floorPlanArea", x.floorPlanArea);
			w.Len ("floorPlanPerimeter", x.floorPlanPerimeter);
			w.Dim ("baseLevel", x.baseLevel);
			w.Dim ("baseHeight", x.baseHeight);
			w.Dim ("wholeHeight", x.wholeHeight);
			w.Count ("nodesCount", (Int32) x.nodesNr);
			w.Count ("edgesCount", (Int32) x.edgesNr);
			w.Count ("hiddenEdgesCount", (Int32) x.hiddenEdgesNr);
			w.Count ("softEdgesCount", (Int32) x.softEdgesNr);
			w.Count ("visibleEdgesCount", (Int32) x.visibleEdgesNr);
			w.Count ("facesCount", (Int32) x.facesNr);
			return true;
		}
		case API_SlabID: {
			const API_SlabQuantity& x = q.slab;
			w.Vol ("volume", x.volume);
			w.Vol ("volumeConditional", x.volume_cond);
			w.Vol ("grossVolume", x.grossVolume);
			w.Vol ("grossVolumeWithHoles", x.grossVolumeWithHoles);
			w.Area ("topSurface", x.topSurface);
			w.Area ("bottomSurface", x.bottomSurface);
			w.Area ("edgeSurface", x.edgeSurface);
			w.Area ("topSurfaceConditional", x.topSurface_cond);
			w.Area ("bottomSurfaceConditional", x.bottomSurface_cond);
			w.Area ("grossTopSurface", x.grossTopSurf);
			w.Area ("grossBottomSurface", x.grossBotSurf);
			w.Area ("grossEdgeSurface", x.grossEdgeSurf);
			w.Area ("grossTopSurfaceWithHoles", x.grossTopSurfWithHoles);
			w.Area ("grossBottomSurfaceWithHoles", x.grossBotSurfWithHoles);
			w.Area ("grossEdgeSurfaceWithHoles", x.grossEdgeSurfWithHoles);
			w.Len ("perimeter", x.perimeter);
			w.Area ("holesSurface", x.holesSurf);
			w.Len ("holesPerimeter", x.holesPrm);
			return true;
		}
		case API_MeshID: {
			const API_MeshQuantity& x = q.mesh;
			w.Vol ("volume", x.volume);
			w.Area ("topSurface", x.topSurface);
			w.Area ("bottomSurface", x.bottomSurface);
			w.Area ("edgeSurface", x.edgeSurface);
			w.Area ("projectedArea", x.projectedArea);
			w.Len ("perimeter", x.perimeter);
			w.Area ("holesSurface", x.holesSurf);
			w.Len ("holesPerimeter", x.holesPrm);
			return true;
		}
		case API_RoofID: {
			const API_RoofQuantity& x = q.roof;
			w.Vol ("volume", x.volume);
			w.Vol ("volumeConditional", x.volume_cond);
			w.Vol ("grossVolume", x.grossVolume);
			w.Area ("topSurface", x.topSurface);
			w.Area ("bottomSurface", x.bottomSurface);
			w.Area ("edgeSurface", x.edgeSurface);
			w.Area ("topSurfaceConditional", x.topSurface_cond);
			w.Area ("bottomSurfaceConditional", x.bottomSurface_cond);
			w.Area ("grossTopSurface", x.grossTopSurf);
			w.Area ("grossBottomSurface", x.grossBotSurf);
			w.Area ("grossEdgeSurface", x.grossEdgeSurf);
			w.Area ("contourArea", x.contourArea);
			w.Len ("perimeter", x.perimeter);
			w.Area ("holesSurface", x.holesSurf);
			w.Len ("holesPerimeter", x.holesPrm);
			w.Area ("openingsSurface", x.openingSurf);
			w.Dim ("insulationThickness", x.insuThickness);
			w.Len ("ridgesLength", x.ridgesLength);
			w.Len ("valleysLength", x.valleysLength);
			w.Len ("gablesLength", x.gablesLength);
			w.Len ("hipsLength", x.hipsLength);
			w.Len ("eavesLength", x.eavesLength);
			w.Len ("peaksLength", x.peaksLength);
			w.Len ("sideWallConnectionLength", x.sideWallConLength);
			w.Len ("endWallConnectionLength", x.endWallConLength);
			w.Len ("domeConnectionLength", x.domeConLength);
			w.Len ("hollowConnectionLength", x.hollowConLength);
			w.Count ("holesCount", x.numOfHoles);
			w.Count ("skylightsCount", x.numOfSkylight);
			return true;
		}
		case API_ShellID: {
			const API_ShellQuantity& x = q.shell;
			w.Vol ("volume", x.volume);
			w.Vol ("volumeConditional", x.volume_cond);
			w.Vol ("grossVolume", x.grossVolume);
			w.Area ("referenceSurface", x.referenceSurface);
			w.Area ("oppositeSurface", x.oppositeSurface);
			w.Area ("edgeSurface", x.edgeSurface);
			w.Area ("referenceSurfaceConditional", x.referenceSurface_cond);
			w.Area ("oppositeSurfaceConditional", x.oppositeSurface_cond);
			w.Area ("grossReferenceSurface", x.grossReferenceSurf);
			w.Area ("grossOppositeSurface", x.grossOppositeSurf);
			w.Area ("grossEdgeSurface", x.grossEdgeSurf);
			w.Area ("floorPlanArea", x.floorplanArea);
			w.Len ("perimeter", x.perimeter);
			w.Area ("holesSurface", x.holesSurf);
			w.Len ("holesPerimeter", x.holesPrm);
			w.Area ("openingsSurface", x.openingSurf);
			w.Dim ("insulationThickness", x.insuThickness);
			w.Len ("ridgesLength", x.ridgesLength);
			w.Len ("valleysLength", x.valleysLength);
			w.Len ("gablesLength", x.gablesLength);
			w.Len ("hipsLength", x.hipsLength);
			w.Len ("eavesLength", x.eavesLength);
			w.Len ("peaksLength", x.peaksLength);
			w.Len ("sideWallConnectionLength", x.sideWallConLength);
			w.Len ("endWallConnectionLength", x.endWallConLength);
			w.Len ("domeConnectionLength", x.domeConLength);
			w.Len ("hollowConnectionLength", x.hollowConLength);
			w.Count ("holesCount", x.numOfHoles);
			w.Count ("skylightsCount", x.numOfSkylight);
			return true;
		}
		case API_HatchID: {
			const API_HatchQuantity& x = q.hatch;
			w.Area ("surface", x.surface);
			w.Len ("perimeter", x.perimeter);
			w.Area ("holesSurface", x.holesSurf);
			w.Len ("holesPerimeter", x.holesPrm);
			return true;
		}
		case API_ZoneID: {
			const API_ZoneAllQuantity& x = q.zone;
			w.Area ("area", x.area);
			w.Area ("netArea", x.netarea);
			w.Area ("calculatedArea", x.calcArea);
			w.Area ("reductionArea", x.reducementArea);
			w.Vol ("volume", x.volume);
			w.Len ("perimeter", x.perimeter);
			w.Len ("netPerimeter", x.netperimeter);
			w.Len ("holesPerimeter", x.holesPrm);
			w.Len ("wallsPerimeter", x.wallsPrm);
			w.Area ("wallsSurface", x.wallsSurf);
			w.Len ("doorsWidth", x.doorsWidth);
			w.Area ("doorsSurface", x.doorsSurf);
			w.Len ("windowsWidth", x.windowsWidth);
			w.Area ("windowsSurface", x.windowsSurf);
			w.Dim ("baseLevel", x.baseLevel);
			w.Dim ("floorThickness", x.floorThick);
			w.Dim ("height", x.height);
			w.Count ("cornersCount", x.allCorners);
			w.Count ("concaveCornersCount", x.concaveCorners);
			w.Area ("totalExtractedArea", x.totalExtrArea);
			w.Area ("reducedExtractedArea", x.reducedExtrArea);
			w.Area ("lowHeightExtractedArea", x.lowExtrArea);
			w.Area ("wallExtractedArea", x.wallExtrArea);
			w.Area ("curtainWallExtractedArea", x.curtainWallExtrArea);
			w.Area ("columnExtractedArea", x.coluExtrArea);
			w.Area ("fillExtractedArea", x.fillExtrArea);
			w.Area ("insetTopSurface", x.insetTopSurf);
			w.Area ("insetBackSurface", x.insetBackSurf);
			w.Area ("insetSideSurface", x.insetSideSurf);
			return true;
		}
		case API_CurtainWallID: {
			const API_CurtainWallQuantity& x = q.cw;
			w.Len ("length", x.length);
			w.Dim ("height", x.height);
			w.Ang ("slantAngle", x.slantAngle);
			w.Area ("contourSurface", x.contourSurface);
			w.Area ("boundarySurface", x.boundarySurface);
			w.Area ("panelsSurface", x.panelsSurface);
			w.Area ("panelsSurfaceNorth", x.panelsSurfaceN);
			w.Area ("panelsSurfaceSouth", x.panelsSurfaceS);
			w.Area ("panelsSurfaceEast", x.panelsSurfaceE);
			w.Area ("panelsSurfaceWest", x.panelsSurfaceW);
			w.Area ("panelsSurfaceNorthEast", x.panelsSurfaceNE);
			w.Area ("panelsSurfaceNorthWest", x.panelsSurfaceNW);
			w.Area ("panelsSurfaceSouthEast", x.panelsSurfaceSE);
			w.Area ("panelsSurfaceSouthWest", x.panelsSurfaceSW);
			w.Len ("framesLength", x.framesLength);
			w.Len ("contourFramesLength", x.contourFramesLength);
			w.Len ("mainAxisFramesLength", x.mainaxisFramesLength);
			w.Len ("secondaryAxisFramesLength", x.secaxisFramesLength);
			w.Len ("customFramesLength", x.customFramesLength);
			w.Count ("panelsCount", (Int32) x.numOfPanels);
			return true;
		}
		case API_CurtainWallFrameID: {
			const API_CWFrameQuantity& x = q.cwFrame;
			w.Text ("typeText", x.typeText);
			w.Text ("categoryText", x.categoryText);
			w.Len ("length", x.length);
			w.Dim ("width", x.width);
			w.Dim ("depth", x.depth);
			w.Ang ("direction", x.direction);
			w.Surf ("surface", x.material);
			w.RawInt ("position", x.position);
			return true;
		}
		case API_CurtainWallPanelID: {
			const API_CWPanelQuantity& x = q.cwPanel;
			w.Text ("typeText", x.typeText);
			w.Text ("orientationText", x.orientationText);
			w.Area ("surface", x.surface);
			w.Area ("grossSurface", x.grossSurface);
			w.Len ("perimeter", x.perimeter);
			w.Len ("grossPerimeter", x.grossPerimeter);
			w.Dim ("thickness", x.thickness);
			w.Dim ("width", x.width);
			w.Dim ("nominalWidth", x.nominalWidth);
			w.Dim ("height", x.height);
			w.Dim ("nominalHeight", x.nominalHeight);
			w.Ang ("verticalDirection", x.vertDirection);
			w.Ang ("horizontalDirection", x.horDirection);
			w.Surf ("outerSurface", x.material_outer);
			w.Surf ("innerSurface", x.material_inner);
			w.Surf ("cutSurface", x.material_cut);
			w.RawInt ("function", x.function);
			return true;
		}
		case API_CurtainWallJunctionID:
			w.Text ("typeText", q.cwJunction.typeText);
			return true;
		case API_CurtainWallAccessoryID: {
			const API_CWAccessoryQuantity& x = q.cwAccessory;
			w.Text ("typeText", x.typeText);
			w.Len ("length", x.length);
			w.Dim ("width", x.width);
			w.Dim ("height", x.height);
			return true;
		}
		case API_StairID: {
			const API_StairQuantity& x = q.stair;
			w.Area ("area", x.area);
			w.Vol ("volume", x.volume);
			w.Dim ("height", x.height);
			w.Len ("walkingLineLength", x.walklineLength);
			w.Raw ("gradient", x.gradient);
			w.Count ("risersCount", x.numOfRisers);
			w.Count ("treadsCount", x.numOfTreads);
			return true;
		}
		case API_TreadID: {
			const API_StairTreadQuantity& x = q.stairTread;
			w.Area ("area", x.area);
			w.Vol ("volume", x.volume);
			w.Dim ("thickness", x.thickness);
			w.Dim ("elevation", x.elevation);
			w.RawInt ("stepIndex", x.stepIndex);
			return true;
		}
		case API_RiserID: {
			const API_StairRiserQuantity& x = q.stairRiser;
			w.Vol ("volume", x.volume);
			w.Area ("frontArea", x.frontArea);
			w.Dim ("width", x.width);
			w.Ang ("slantAngle", x.slantAngle);
			w.RawInt ("stepIndex", x.stepIndex);
			return true;
		}
		case API_StairStructureID: {
			const API_StairStructureQuantity& x = q.stairStructure;
			w.Vol ("volume", x.volume);
			w.Len ("length3D", x.length3D);
			w.Dim ("height", x.height);
			w.Dim ("thickness", x.thickness);
			return true;
		}
		case API_RailingID:
			w.Area ("area", q.railing.area);
			w.Vol ("volume", q.railing.volume);
			w.Len ("length3D", q.railing.length3D);
			return true;
		case API_RailingToprailID:				RailPart (q.railingToprail, w);				return true;
		case API_RailingHandrailID:				RailPart (q.railingHandrail, w);			return true;
		case API_RailingRailID:					RailPart (q.railingRail, w);				return true;
		case API_RailingToprailEndID:			RailPart (q.railingToprailEnd, w);			return true;
		case API_RailingHandrailEndID:			RailPart (q.railingHandrailEnd, w);			return true;
		case API_RailingRailEndID:				RailPart (q.railingRailEnd, w);				return true;
		case API_RailingToprailConnectionID:	RailPart (q.railingToprailConnection, w);	return true;
		case API_RailingHandrailConnectionID:	RailPart (q.railingHandrailConnection, w);	return true;
		case API_RailingRailConnectionID:		RailPart (q.railingRailConnection, w);		return true;
		case API_RailingPostID:					RailPart (q.railingPost, w);				return true;
		case API_RailingInnerPostID:			RailPart (q.railingInnerPost, w);			return true;
		case API_RailingBalusterID:				RailPart (q.railingBaluster, w);			return true;
		case API_RailingSegmentID:				RailPart (q.railingSegment, w);				return true;
		case API_RailingPanelID:
			w.Vol ("volume", q.railingPanel.volume);
			return true;
		default:
			return false;
	}
}


OS CompositeJson (const API_CompositeQuantity& c)
{
	OS out;
	out.Add ("buildingMaterial", AttrRef (API_BuildingMaterialID, c.buildMatIndices));
	out.Add ("volume", c.volumes);
	out.Add ("projectedArea", c.projectedArea);
	if ((c.flags & APISkin_Core) != 0)
		out.Add ("core", true);
	if ((c.flags & APISkin_Finish) != 0)
		out.Add ("finish", true);
	if (c.compositeId.subElementGuid != APINULLGuid)
		out.Add ("subElementGuid", GuidStr (c.compositeId.subElementGuid));
	return out;
}


OS PartIdJson (const API_ElemPartId& id)
{
	return OS ("main", (Int32) id.main, "sub", (Int32) id.sub, "floor", (Int32) id.floor);
}


struct TypeTotals {
	Int32	count = 0;
	Sums	sums;
};


struct MaterialTotals {
	double	volume = 0.0;
	double	projectedArea = 0.0;
};

} // namespace


void RegisterQuantityCommands ()
{
	RegisterCommand ("GetElementQuantities",
		"Calculated quantities of elements (ACAPI_Element_GetQuantities): every field of the type's quantity record with descriptive "
		"names (m, m2, m3, degrees, counts). Input: {elements: [guid], includeComposites (default true: skin volumes per building "
		"material), includeParts (default false: per element-part values, e.g. roof planes / morph floors), minOpeningSize (m2, default 0; openings "
		"smaller than this do not reduce wall surfaces), includeTotals (default true: per-type sums of additive fields and building "
		"material volumes), includeExposedSurfaces (default false), coverElements: [guid] (elements that cover surfaces for the exposed "
		"surface calculation)}. Output: {elements: [{guid, type, quantities, composites?, parts?} | {guid, error}], totals?, "
		"buildingMaterialTotals?, exposedSurfaces?}.",
		[] (const OS& params) -> OS {
			GS::Array<API_Guid> guids = GetGuidArray (params, "elements", true);
			if (guids.IsEmpty ())
				Fail ("Pass at least one element GUID in 'elements' (use find_elements to get GUIDs).");
			const bool withComposites = GetBool (params, "includeComposites", true);
			const bool withParts = GetBool (params, "includeParts", false);
			const bool withTotals = GetBool (params, "includeTotals", true);
			const bool withExposed = GetBool (params, "includeExposedSurfaces", false);
			std::optional<double> minOpening = OptDouble (params, "minOpeningSize");

			// Walls require the parameter block (nullptr -> APIERR_BADPARS); default: every opening reduces.
			API_QuantityPar par;
			BNZeroMemory (&par, sizeof (par));
			par.minOpeningSize = minOpening.has_value () ? *minOpening : 1e-5;

			std::map<std::string, TypeTotals> totals;
			std::map<Int32, MaterialTotals> materialTotals;
			GS::Array<OS> results;

			for (const API_Guid& guid : guids) {
				OS result = Try ([&] () -> OS {
					API_Elem_Head head = GetHeader (guid);
					const API_ElemTypeID typeID = head.type.typeID;
					const GS::UniString typeName = ElemTypeName (head.type);

					API_ElementQuantity quantity;
					BNZeroMemory (&quantity, sizeof (quantity));
					GS::Array<API_CompositeQuantity>			composites;
					GS::Array<API_ElemPartQuantity>				partQuantities;
					GS::Array<API_ElemPartCompositeQuantity>	partComposites;

					API_Quantities quantities;
					quantities.elements = &quantity;
					if (withComposites)
						quantities.composites = &composites;
					if (withParts) {
						quantities.elemPartQuantities = &partQuantities;
						quantities.elemPartComposites = &partComposites;
					}
					API_QuantitiesMask mask;
					ACAPI_ELEMENT_QUANTITIES_MASK_SETFULL (mask);

					GSErrCode err = ACAPI_Element_GetQuantities (guid, &par, &quantities, &mask);
					if (err != NoError)
						Fail ("Quantities are not available for this " + typeName + " (" + ErrorName (err) + "). Quantities exist for 3D "
							  "construction elements, openings, objects, zones, hatches, curtain wall / stair / railing parts.", err);

					OS item;
					item.Add ("guid", GuidStr (guid));
					item.Add ("type", typeName);

					const std::string key (ToStr (typeName).ToCStr ());
					TypeTotals* tt = withTotals ? &totals[key] : nullptr;
					OS q;
					QWriter writer (q, tt != nullptr ? &tt->sums : nullptr);
					if (!WriteQuantities (typeID, quantity, writer))
						Fail ("Archicad provides no quantity record for element type '" + typeName + "'.", APIERR_BADELEMENTTYPE);
					if (tt != nullptr)
						++tt->count;
					item.Add ("quantities", q);

					if (withComposites && !composites.IsEmpty ()) {
						GS::Array<OS> list;
						for (const API_CompositeQuantity& c : composites) {
							list.Push (CompositeJson (c));
							if (withTotals) {
								MaterialTotals& mt = materialTotals[(Int32) c.buildMatIndices];
								mt.volume += c.volumes;
								mt.projectedArea += c.projectedArea;
							}
						}
						item.Add ("composites", list);
					}

					if (withParts) {
						GS::Array<OS> parts;
						for (const API_ElemPartQuantity& pq : partQuantities) {
							OS pqJson;
							QWriter pw (pqJson, nullptr);
							WriteQuantities (typeID, pq.quantity, pw);
							OS part ("partId", PartIdJson (pq.partId), "quantities", pqJson);
							for (const API_ElemPartCompositeQuantity& pc : partComposites) {
								if (pc.partId == pq.partId) {
									GS::Array<OS> list;
									for (const API_CompositeQuantity& c : pc.composites)
										list.Push (CompositeJson (c));
									part.Add ("composites", list);
									break;
								}
							}
							parts.Push (part);
						}
						if (!parts.IsEmpty ())
							item.Add ("parts", parts);
					}
					return item;
				});
				if (result.Contains ("error") && !result.Contains ("guid"))
					result.Add ("guid", GuidStr (guid));
				results.Push (result);
			}

			OS out;
			out.Add ("elements", results);

			if (withTotals) {
				OS byType;
				for (const auto& kv : totals) {
					if (kv.second.count == 0)
						continue;
					OS t ("count", kv.second.count);
					for (const auto& s : kv.second.sums)
						t.Add (GS::String (s.first.c_str ()), s.second);
					byType.Add (GS::String (kv.first.c_str ()), t);
				}
				out.Add ("totals", byType);
				if (!materialTotals.empty ()) {
					GS::Array<OS> list;
					for (const auto& kv : materialTotals) {
						list.Push (OS ("buildingMaterial", AttrRef (API_BuildingMaterialID, kv.first),
									   "volume", kv.second.volume, "projectedArea", kv.second.projectedArea));
					}
					out.Add ("buildingMaterialTotals", list);
				}
			}

			if (withExposed) {
				GS::Array<API_Guid> cover = OptGuidArray (params, "coverElements");
				GS::Array<API_ElemPartSurfaceQuantity> surfaces;
				GSErrCode err = ACAPI_Element_GetSurfaceQuantities (&guids, &cover, &surfaces);
				if (err != NoError) {
					out.Add ("exposedSurfacesError", GS::UniString (GS::UniString ("Exposed surfaces could not be calculated: ") + ErrorName (err)));
				} else {
					GS::Array<OS> list;
					for (const API_ElemPartSurfaceQuantity& s : surfaces) {
						OS item;
						item.Add ("element", GuidStr (s.elemGUID));
						if (s.materialIndex > 0)
							item.Add ("surface", AttrRef (API_MaterialID, s.materialIndex));
						if (s.buildMatIdx > 0)
							item.Add ("buildingMaterial", AttrRef (API_BuildingMaterialID, s.buildMatIdx));
						item.Add ("exposedArea", s.exposedSurface);
						list.Push (item);
					}
					out.Add ("exposedSurfaces", list);
				}
			}
			return out;
		});
}

} // namespace eq
} // namespace cc
