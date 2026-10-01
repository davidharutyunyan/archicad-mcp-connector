cmake_minimum_required (VERSION 3.16)

# -----------------------------------------------------------------------------
# Claude Connector — Archicad add-on exposing JSON commands for the MCP server.
# Build (macOS):
#   cmake -S addon -B addon/build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build addon/build
# The bundle is produced at addon/build/ClaudeConnector.bundle
# -----------------------------------------------------------------------------

set (CMAKE_EXPORT_COMPILE_COMMANDS ON)
set (CMAKE_OSX_DEPLOYMENT_TARGET 10.15)
if (NOT CMAKE_OSX_ARCHITECTURES)
	set (CMAKE_OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}" CACHE STRING "" FORCE)
endif ()
if (NOT CMAKE_BUILD_TYPE)
	set (CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif ()

set (AC_API_DEVKIT_DIR "${CMAKE_CURRENT_LIST_DIR}/../.devkit/devkit26" CACHE PATH "API DevKit directory.")
set (AC_ADDON_NAME "ClaudeConnector" CACHE STRING "Add-On name.")
set (AC_ADDON_LANGUAGE "INT" CACHE STRING "Add-On language code.")
get_filename_component (AC_API_DEVKIT_DIR "${AC_API_DEVKIT_DIR}" ABSOLUTE)

message (STATUS "APIDevKit directory: ${AC_API_DEVKIT_DIR}")
set (ACAPINC_FILE_LOCATION ${AC_API_DEVKIT_DIR}/Support/Inc/ACAPinc.h)
if (EXISTS ${ACAPINC_FILE_LOCATION})
	file (READ ${ACAPINC_FILE_LOCATION} ACAPIncContent)
	string (REGEX MATCHALL "#define[ \t]+ServerMainVers_([0-9][0-9])" VersionList ${ACAPIncContent})
	set (ARCHICAD_VERSION ${CMAKE_MATCH_1})
	message (STATUS "Archicad Version: ${ARCHICAD_VERSION}")
else ()
	message (FATAL_ERROR "Archicad API DevKit not found at ${AC_API_DEVKIT_DIR}. Run scripts/fetch-devkit.sh first.")
endif ()

if (WIN32)
	add_definitions (-DUNICODE -D_UNICODE)
else ()
	add_definitions (-Dmacintosh=1)
endif ()
add_definitions (-DACExtension)

project (${AC_ADDON_NAME} CXX)

set (AddOnSourcesFolder ${CMAKE_CURRENT_LIST_DIR}/Src)
set (AddOnResourcesFolder ${CMAKE_CURRENT_LIST_DIR})
set (ResourceObjectsDir ${CMAKE_BINARY_DIR}/ResourceObjects)
set (BundleDir ${CMAKE_BINARY_DIR}/${AC_ADDON_NAME}.bundle)

# --- Add-on identifier (MDID) ------------------------------------------------
# Licensed Archicad only loads add-ons whose MDID (Developer ID + Local ID) was issued by Graphisoft:
# register at https://archicadapi.graphisoft.com and create the add-on under Profile > Add-ons.
# Put the two numbers into addon/mdid.local.cmake (git-ignored), e.g.
#   set (AC_MDID_DEVELOPER 123456789)
#   set (AC_MDID_LOCAL     987654321)
# or pass -DAC_MDID_DEVELOPER=... -DAC_MDID_LOCAL=... to cmake.

set (MDID_LOCAL_FILE "${CMAKE_CURRENT_LIST_DIR}/mdid.local.cmake")
set_property (DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MDID_LOCAL_FILE}")
if (EXISTS "${MDID_LOCAL_FILE}")
	include ("${MDID_LOCAL_FILE}")
endif ()
if (NOT AC_MDID_DEVELOPER OR NOT AC_MDID_LOCAL)
	message (WARNING "No MDID configured (addon/mdid.local.cmake). The add-on will build, but licensed Archicad will refuse to load it ('cannot verify the authenticity of this add-on').")
	set (AC_MDID_DEVELOPER 1)
	set (AC_MDID_LOCAL 1)
endif ()
message (STATUS "Add-on MDID: ${AC_MDID_DEVELOPER} ${AC_MDID_LOCAL}")
set (ResourceIncludeDir ${CMAKE_BINARY_DIR}/ResourceInclude)
file (MAKE_DIRECTORY "${ResourceIncludeDir}")
set (MDID_HEADER "${ResourceIncludeDir}/ClaudeConnectorMDID.h")
file (WRITE "${MDID_HEADER}.tmp" "#define CC_MDID_DEVELOPER ${AC_MDID_DEVELOPER}\n#define CC_MDID_LOCAL ${AC_MDID_LOCAL}\n")
file (COPY_FILE "${MDID_HEADER}.tmp" "${MDID_HEADER}" ONLY_IF_DIFFERENT)

# --- Resources ---------------------------------------------------------------

find_program (PYTHON_EXECUTABLE NAMES python3 python REQUIRED)

file (GLOB AddOnResourceFiles
	${AddOnResourcesFolder}/R${AC_ADDON_LANGUAGE}/*.grc
	${AddOnResourcesFolder}/RFIX/*.grc
	${AddOnResourcesFolder}/RFIX.mac/*.plist
)

add_custom_command (
	OUTPUT "${ResourceObjectsDir}/AddOnResources.stamp"
	DEPENDS ${AddOnResourceFiles} "${MDID_HEADER}"
	COMMENT "Compiling resources..."
	COMMAND ${CMAKE_COMMAND} -E make_directory "${ResourceObjectsDir}"
	COMMAND ${CMAKE_COMMAND} -E make_directory "${BundleDir}/Contents/Resources"
	COMMAND ${PYTHON_EXECUTABLE} "${AC_API_DEVKIT_DIR}/Support/Tools/CompileResources.py" "${AC_ADDON_LANGUAGE}" "${AC_API_DEVKIT_DIR}" "${ResourceIncludeDir}" "${AddOnResourcesFolder}" "${ResourceObjectsDir}" "${BundleDir}/Contents/Resources"
	COMMAND ${CMAKE_COMMAND} -E copy "${AC_API_DEVKIT_DIR}/Support/Inc/PkgInfo" "${BundleDir}/Contents/PkgInfo"
	COMMAND ${CMAKE_COMMAND} -E touch "${ResourceObjectsDir}/AddOnResources.stamp"
)
add_custom_target (AddOnResources ALL DEPENDS "${ResourceObjectsDir}/AddOnResources.stamp" SOURCES ${AddOnResourceFiles})

# --- Sources -----------------------------------------------------------------

file (GLOB_RECURSE AddOnHeaderFiles CONFIGURE_DEPENDS ${AddOnSourcesFolder}/*.h ${AddOnSourcesFolder}/*.hpp)
file (GLOB_RECURSE AddOnSourceFiles CONFIGURE_DEPENDS ${AddOnSourcesFolder}/*.cpp)

add_library (AddOn MODULE ${AddOnHeaderFiles} ${AddOnSourceFiles})
set_target_properties (AddOn PROPERTIES
	# The GS frameworks must resolve (via @rpath) to the ones inside the running Archicad.app,
	# never to the DevKit copies, so no build-tree RPATH may be embedded.
	SKIP_BUILD_RPATH TRUE
	INSTALL_RPATH ""
	OUTPUT_NAME ${AC_ADDON_NAME}
	BUNDLE TRUE
	MACOSX_BUNDLE_INFO_PLIST "${AddOnResourcesFolder}/RFIX.mac/Info.plist"
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

target_include_directories (AddOn PUBLIC
	${AddOnSourcesFolder}
	${AC_API_DEVKIT_DIR}/Support/Inc
)

find_library (CocoaFramework Cocoa)
target_link_libraries (AddOn "${AC_API_DEVKIT_DIR}/Support/Lib/Mactel/libACAP_STAT.a" ${CocoaFramework})

target_compile_features (AddOn PUBLIC cxx_std_17)
target_compile_options (AddOn PUBLIC -Wall -Wextra -fvisibility=hidden
	-Wno-multichar
	-Wno-ctor-dtor-privacy
	-Wno-invalid-offsetof
	-Wno-ignored-qualifiers
	-Wno-reorder
	-Wno-overloaded-virtual
	-Wno-unused-parameter
	-Wno-missing-field-initializers
	-Wno-unknown-pragmas
	-Wno-missing-braces
	-Wno-unused-private-field
	-Wno-return-std-move
	-Wno-unused-value
	-Wno-switch
	-Wno-deprecated
	-Wno-deprecated-declarations
	-Wno-unused-function
	-Wno-unused-variable
	-Wno-unused-but-set-variable
	-Wno-sign-compare
	-Wno-implicit-fallthrough
	-Wno-nontrivial-memcall
	-Wno-cast-function-type-mismatch
	-Wno-unknown-warning-option
)
add_dependencies (AddOn AddOnResources)

# Link every GS module framework shipped with the DevKit (same as the official examples).
function (GetModuleNameFromModuleFolderName module folder)
	if (${folder} MATCHES "^.*Lib$")
		string (REGEX REPLACE "^(.*)Lib$" "\\1" result ${folder})
	else ()
		set (result ${folder})
	endif ()
	set (${module} ${result} PARENT_SCOPE)
endfunction ()

file (GLOB GSModuleDirs RELATIVE "${AC_API_DEVKIT_DIR}/Support/Modules" "${AC_API_DEVKIT_DIR}/Support/Modules/*")
foreach (folder ${GSModuleDirs})
	if (IS_DIRECTORY "${AC_API_DEVKIT_DIR}/Support/Modules/${folder}")
		target_include_directories (AddOn PUBLIC "${AC_API_DEVKIT_DIR}/Support/Modules/${folder}")
		GetModuleNameFromModuleFolderName (module ${folder})
		set (ModuleLibPath "${AC_API_DEVKIT_DIR}/Support/Frameworks/${module}.framework")
		if (EXISTS ${ModuleLibPath})
			target_link_libraries (AddOn ${ModuleLibPath})
		endif ()
	endif ()
endforeach ()
