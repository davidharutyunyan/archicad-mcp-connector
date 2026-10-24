cmake_minimum_required (VERSION 3.16)

# -----------------------------------------------------------------------------
# Claude Connector — Archicad add-on exposing JSON commands for the MCP server.
# Build (macOS):
#   cmake -S addon -B addon/build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build addon/build
# The bundle is produced at addon/build/ClaudeConnector.bundle
# Build (Windows, Visual Studio 2019 or newer; scripts/windows/build-addon.ps1 does all of this):
#   cmake -S addon -B addon/build -A x64 -DAC_API_DEVKIT_DIR=<DevKit for Windows>
#   cmake --build addon/build --config Release
# The add-on is produced at addon/build/Release/ClaudeConnector.apx
# -----------------------------------------------------------------------------

set (CMAKE_EXPORT_COMPILE_COMMANDS ON)
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

# Archicad 27 reorganized the API (C++20, new functions, new library layout); 26 is the original target.
if (ARCHICAD_VERSION GREATER_EQUAL 27)
	set (AC_MODERN_API ON)
	set (CMAKE_OSX_DEPLOYMENT_TARGET 11.0)
else ()
	set (AC_MODERN_API OFF)
	set (CMAKE_OSX_DEPLOYMENT_TARGET 10.15)
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
set (ResourceFile "${ResourceObjectsDir}/${AC_ADDON_NAME}.res")	# Windows: compiled native resources, linked into the .apx

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

if (WIN32)
	# FindPython3 skips the Microsoft Store 'python.exe' alias, which is not a real interpreter.
	find_package (Python3 REQUIRED COMPONENTS Interpreter)
	set (PYTHON_EXECUTABLE "${Python3_EXECUTABLE}")
else ()
	find_program (PYTHON_EXECUTABLE NAMES python3 python REQUIRED)
endif ()

if (WIN32)
	file (GLOB AddOnResourceFiles
		${AddOnResourcesFolder}/R${AC_ADDON_LANGUAGE}/*.grc
		${AddOnResourcesFolder}/RFIX/*.grc
		${AddOnResourcesFolder}/RFIX.win/*.rc2
	)
	# CompileResources.py runs cl (preprocessor) and rc: the Visual Studio generator puts both on the PATH of
	# custom build steps; with Ninja, configure from a Visual Studio developer prompt.
	add_custom_command (
		OUTPUT "${ResourceObjectsDir}/AddOnResources.stamp"
		DEPENDS ${AddOnResourceFiles} "${MDID_HEADER}"
		COMMENT "Compiling resources..."
		COMMAND ${CMAKE_COMMAND} -E make_directory "${ResourceObjectsDir}"
		COMMAND "${PYTHON_EXECUTABLE}" "${AC_API_DEVKIT_DIR}/Support/Tools/CompileResources.py" "${AC_ADDON_LANGUAGE}" "${AC_API_DEVKIT_DIR}" "${ResourceIncludeDir}" "${AddOnResourcesFolder}" "${ResourceObjectsDir}" "${ResourceFile}"
		COMMAND ${CMAKE_COMMAND} -E touch "${ResourceObjectsDir}/AddOnResources.stamp"
	)
else ()
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
endif ()
add_custom_target (AddOnResources ALL DEPENDS "${ResourceObjectsDir}/AddOnResources.stamp" SOURCES ${AddOnResourceFiles})

# --- Sources -----------------------------------------------------------------

file (GLOB_RECURSE AddOnHeaderFiles CONFIGURE_DEPENDS ${AddOnSourcesFolder}/*.h ${AddOnSourcesFolder}/*.hpp)
file (GLOB_RECURSE AddOnSourceFiles CONFIGURE_DEPENDS ${AddOnSourcesFolder}/*.cpp)

if (WIN32)
	# Archicad loads a Windows add-on as a DLL with the .apx extension and the two exported entry points below.
	add_library (AddOn SHARED ${AddOnHeaderFiles} ${AddOnSourceFiles})
	set_target_properties (AddOn PROPERTIES
		OUTPUT_NAME ${AC_ADDON_NAME}
		SUFFIX ".apx"
	)
	target_link_options (AddOn PRIVATE "${ResourceFile}" /export:GetExportedFuncAddrs,@1 /export:SetImportedFuncAddrs,@2)
else ()
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
endif ()

target_include_directories (AddOn PUBLIC
	${AddOnSourcesFolder}
	${AC_API_DEVKIT_DIR}/Support/Inc
)

target_compile_definitions (AddOn PUBLIC CC_AC_VERSION=${ARCHICAD_VERSION})
if (AC_MODERN_API)
	target_compile_features (AddOn PUBLIC cxx_std_20)
else ()
	target_compile_features (AddOn PUBLIC cxx_std_17)
endif ()
if (ARCHICAD_VERSION GREATER_EQUAL 29)
	# As in the Archicad 29 DevKit examples: the GS allocator headers are force-included into every source file.
	# (Not on 27: its GSMalloc.hpp turns realloc into a macro that breaks the C++ standard library headers.)
	get_filename_component (GSNewHeader "${AC_API_DEVKIT_DIR}/Support/Modules/GSRoot/GSNew.hpp" REALPATH)
	get_filename_component (GSMallocHeader "${AC_API_DEVKIT_DIR}/Support/Modules/GSRoot/GSMalloc.hpp" REALPATH)
	if (WIN32)
		target_compile_options (AddOn PRIVATE "SHELL:/FI \"${GSNewHeader}\"" "SHELL:/FI \"${GSMallocHeader}\"")
	else ()
		target_compile_options (AddOn PRIVATE "SHELL:-include \"${GSNewHeader}\"" "SHELL:-include \"${GSMallocHeader}\"")
	endif ()
endif ()
if (WIN32 AND AC_MODERN_API)
	target_link_libraries (AddOn "${AC_API_DEVKIT_DIR}/Support/Lib/ACAP_STAT.lib")
elseif (WIN32)
	target_link_libraries (AddOn
		"$<$<CONFIG:Debug>:${AC_API_DEVKIT_DIR}/Support/Lib/Win/ACAP_STATD.lib>"
		"$<$<NOT:$<CONFIG:Debug>>:${AC_API_DEVKIT_DIR}/Support/Lib/Win/ACAP_STAT.lib>"
	)
	# /Zc:wchar_t- as in the DevKit examples (the GS modules are built with wchar_t as a typedef);
	# /utf-8: the sources are UTF-8 (Cyrillic and other non-ASCII literals); /bigobj: large command files.
	target_compile_options (AddOn PUBLIC /W3 /Zc:wchar_t- /utf-8 /bigobj /EHsc /MP
		/wd4996		# deprecated CRT / API functions
		/wd4068		# unknown pragma
		/wd4244		# conversion, possible loss of data
		/wd4267		# size_t conversion
		/wd4305		# truncation from double to float
	)
	target_compile_definitions (AddOn PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS $<$<CONFIG:Debug>:DEBUG>)
else ()
	find_library (CocoaFramework Cocoa)
	if (AC_MODERN_API)
		target_link_libraries (AddOn "${AC_API_DEVKIT_DIR}/Support/Lib/libACAP_STAT.a" ${CocoaFramework})
	else ()
		target_link_libraries (AddOn "${AC_API_DEVKIT_DIR}/Support/Lib/Mactel/libACAP_STAT.a" ${CocoaFramework})
	endif ()
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
	-Wno-shorten-64-to-32
	)
endif ()
add_dependencies (AddOn AddOnResources)

# Link every GS module shipped with the DevKit (same as the official examples):
# macOS: Support/Frameworks/<module>.framework, Windows: Support/Modules/<folder>/Win/<module>Imp.lib.
function (GetModuleNameFromModuleFolderName module folder)
	if (${folder} MATCHES "^.*Lib$")
		string (REGEX REPLACE "^(.*)Lib$" "\\1" result ${folder})
	elseif (WIN32 AND ${folder} MATCHES "^Communication.*$")
		string (REGEX REPLACE "^Communication(.*)$" "Communication.\\1" result ${folder})
	else ()
		set (result ${folder})
	endif ()
	set (${module} ${result} PARENT_SCOPE)
endfunction ()

if (AC_MODERN_API)
	# Archicad 27+: link every framework (macOS) / every import library of every module (Windows), as the examples do.
	if (WIN32)
		file (GLOB GSLinkLibs "${AC_API_DEVKIT_DIR}/Support/Modules/*/Win/*.lib")
	else ()
		file (GLOB GSLinkLibs "${AC_API_DEVKIT_DIR}/Support/Frameworks/*.framework")
	endif ()
	target_link_libraries (AddOn ${GSLinkLibs})
endif ()

file (GLOB GSModuleDirs RELATIVE "${AC_API_DEVKIT_DIR}/Support/Modules" "${AC_API_DEVKIT_DIR}/Support/Modules/*")
foreach (folder ${GSModuleDirs})
	if (IS_DIRECTORY "${AC_API_DEVKIT_DIR}/Support/Modules/${folder}")
		target_include_directories (AddOn PUBLIC "${AC_API_DEVKIT_DIR}/Support/Modules/${folder}")
		GetModuleNameFromModuleFolderName (module ${folder})
		if (AC_MODERN_API)
			set (ModuleLibPath "")	# linked above
		elseif (WIN32)
			set (ModuleLibPath "${AC_API_DEVKIT_DIR}/Support/Modules/${folder}/Win/${module}Imp.lib")
		else ()
			set (ModuleLibPath "${AC_API_DEVKIT_DIR}/Support/Frameworks/${module}.framework")
		endif ()
		if (ModuleLibPath AND EXISTS ${ModuleLibPath})
			target_link_libraries (AddOn ${ModuleLibPath})
		endif ()
	endif ()
endforeach ()
