// *****************************************************************************
// ComplexElementsCommon.hpp — private helpers of the "complex-elements" family
// (Morph / CurtainWall / Stair / Railing adapters). Only included by
// Commands/ComplexElements*.cpp.
// *****************************************************************************

#pragma once

#include "Core/Command.hpp"
#include "Core/Elements.hpp"
#include "Core/Enums.hpp"
#include "Core/Polygon.hpp"

namespace cc {
namespace complex {

// --- Registration (one per file) -------------------------------------------------

void RegisterMorphFamily ();			// ComplexElementsMorph.cpp
void RegisterCurtainWallFamily ();		// ComplexElementsCurtainWall.cpp
void RegisterStairRailingFamily ();		// ComplexElementsStairRailing.cpp

// --- Memo array helpers ------------------------------------------------------------

// Number of items in a memo array allocated with BMpAll (cWallFrames, stairTreads, railingPosts, ...).
template <class T>
UInt32 PtrCount (const T* p)
{
	if (p == nullptr)
		return 0;
	return (UInt32) (BMGetPtrSize (reinterpret_cast<GSConstPtr> (p)) / (GSSize) sizeof (T));
}

// Number of items in a memo handle (coords, parcs, polyZCoords, ...).
template <class T>
UInt32 HdlCount (T** h)
{
	if (h == nullptr || *h == nullptr)
		return 0;
	return (UInt32) (BMGetHandleSize (reinterpret_cast<GSConstHandle> (h)) / (GSSize) sizeof (T));
}

// Releases a BMpAll pointer and nulls it.
template <class T>
void KillPtr (T*& p)
{
	if (p != nullptr) {
		GSPtr raw = reinterpret_cast<GSPtr> (p);
		BMpKill (&raw);
		p = nullptr;
	}
}

// Releases a handle and nulls it.
template <class T>
void KillHdl (T**& h)
{
	if (h != nullptr) {
		GSHandle raw = reinterpret_cast<GSHandle> (h);
		BMKillHandle (&raw);
		h = nullptr;
	}
}

// Allocates a zeroed BMpAll array (throws on out-of-memory).
template <class T>
T* AllocPtr (UInt32 count)
{
	if (count == 0)
		return nullptr;
	T* p = reinterpret_cast<T*> (BMpAllClear ((GSSize) count * (GSSize) sizeof (T)));
	if (p == nullptr)
		Fail ("Out of memory.", APIERR_MEMFULL);
	return p;
}

// Allocates a zeroed handle (throws on out-of-memory).
template <class T>
T** AllocHdl (UInt32 count)
{
	T** h = reinterpret_cast<T**> (BMhAllClear ((GSSize) (count == 0 ? 1 : count) * (GSSize) sizeof (T)));
	if (h == nullptr)
		Fail ("Out of memory.", APIERR_MEMFULL);
	return h;
}

// GUID strings of the first `limit` items of a memo sub-element array (items have `head`).
template <class T>
GS::Array<GS::UniString> HeadGuids (const T* arr, UInt32 count, UInt32 limit, bool& truncated)
{
	GS::Array<GS::UniString> out;
	if (arr == nullptr)
		return out;
	for (UInt32 i = 0; i < count; ++i) {
		if (out.GetSize () >= limit) {
			truncated = true;
			break;
		}
		out.Push (GuidStr (arr[i].head.guid));
	}
	return out;
}

// Maximum number of sub-element GUIDs listed per kind in element details.
constexpr UInt32 kMaxListedParts = 1000;

// --- Field appliers (return true when the field was present and applied) --------------

bool ApplyLength (const OS& spec, const char* key, double& value, bool positive = false, bool nonNegative = false);
bool ApplyAngleField (const OS& spec, const char* key, double& radians);
bool ApplyFlag (const OS& spec, const char* key, bool& value);
bool ApplyPenField (const OS& spec, const char* key, short& pen);
bool ApplyAttrField (const OS& spec, const char* key, API_AttrTypeID type, API_AttributeIndex& index);
// Cut fill pen overrides: "cutFillPen" / "cutFillBackgroundPen" = pen index or false.
bool ApplyCutFillPenOverride (const OS& spec, API_PenOverrideType& penOverride);
void AddCutFillPenOverrideJson (OS& out, const API_PenOverrideType& penOverride);

// Explicit "storyIndex" wins over "relative to the current story" creation mode.
void ApplyStoryCreationMode (API_LinkToSettings& link, const OS& spec);

// --- Geometry helpers ------------------------------------------------------------------

// Reads an open path: spec[key] as polyline ({points, arcs?} or point array; points may carry z),
// or "begin"/"end" points when allowBeginEnd and spec[key] is missing.
Contour GetPath (const OS& spec, const char* key, bool allowBeginEnd, Int32 minPoints = 2);

// Serializes a 1-based coordinate handle (items 1..nCoords) with arcs; z (1-based) is optional.
OS HandlePolylineJson (API_Coord** coords, API_PolyArc** parcs, Int32 nCoords, Int32 nArcs, double** z = nullptr);

// Adds "boundingBox" (3D, from Archicad's own bounds calculation) when available.
void AddBoundingBox (OS& out, const API_Elem_Head& head);

// Length of a polyline in plan (arcs taken into account).
double PathLength (const Contour& path);

// Appends the path length in plan to `out` as "length".
double HandlePathLength (API_Coord** coords, API_PolyArc** parcs, Int32 nCoords, Int32 nArcs);

// Owner GUID (sub-elements) — omitted when null.
void AddOwner (OS& out, const API_Guid& owner);

} // namespace complex
} // namespace cc
