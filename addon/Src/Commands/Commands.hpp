// *****************************************************************************
// Commands.hpp — one Register*Commands() function per command family.
// Each family lives in its own .cpp file under Src/Commands/ and registers its
// JSON commands (cc::RegisterCommand) and element adapters (cc::RegisterAdapter).
// *****************************************************************************

#pragma once

namespace cc {

void RegisterSystemCommands ();			// System.cpp        — ping, add-on info, command list
void RegisterProjectCommands ();		// Project.cpp       — project info, save/open/close, autotexts, geo location, units
void RegisterStoryCommands ();			// Stories.cpp       — stories CRUD
void RegisterAttributeCommands ();		// Attributes.cpp    — all attribute types CRUD, layer combinations
void RegisterElementQueryCommands ();	// ElementQuery.cpp  — element listing/filtering, quantities, relations, selection
void RegisterElementEditCommands ();	// ElementEdit.cpp   — move/copy/rotate/mirror/delete/group/lock/trim/solid ops
void RegisterWallCommands ();			// Walls.cpp         — Wall adapter (reference implementation)
void RegisterColumnBeamCommands ();		// ColumnsBeams.cpp  — Column / Beam adapters
void RegisterSlabRoofCommands ();		// SlabsRoofs.cpp    — Slab / Roof / Shell / Mesh adapters
void RegisterOpeningCommands ();		// Openings.cpp      — Window / Door / Skylight / Opening adapters
void RegisterObjectLibraryCommands ();	// ObjectsLibrary.cpp — Object / Lamp adapters, libraries, GDL parameters, library part creation
void RegisterZoneCommands ();			// Zones.cpp         — Zone adapter and zone utilities
void RegisterDraftingCommands ();		// Drafting.cpp      — Line / Arc / Circle / PolyLine / Spline / Hatch / Text / Label / Hotspot / Picture
void RegisterDimensionCommands ();		// Dimensions.cpp    — linear / level / radial / angle dimensions
void RegisterComplexElementCommands ();	// ComplexElements.cpp — Morph / CurtainWall / Stair / Railing
void RegisterViewCommands ();			// Views.cpp         — windows, zoom, 3D view, image capture, rendering, cut planes
void RegisterDocumentationCommands ();	// Documentation.cpp — layouts/drawings, publishing, export/import, hotlinks
void RegisterPropertyCommands ();		// Properties.cpp    — property definitions/values, IFC data, classifications
void RegisterCollaborationCommands ();	// Collaboration.cpp — Teamwork, issues/BCF, favorites, tool defaults, revisions

} // namespace cc
