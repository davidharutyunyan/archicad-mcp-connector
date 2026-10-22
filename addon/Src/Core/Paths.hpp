// *****************************************************************************
// Paths.hpp — file system path conventions shared by all commands, on macOS
// and on Windows (paths arrive from the MCP server as plain strings).
// *****************************************************************************

#pragma once

#include "Core/Json.hpp"

namespace cc {

// '/' on macOS; '\' and '/' on Windows.
bool			IsPathSeparator (GS::UniChar c);
// Index of the last path separator, or MaxUIndex when there is none.
UIndex			LastPathSeparator (const GS::UniString& path);
// macOS: starts with '/'. Windows: a drive path ("C:\..." or "C:/...") or a UNC path ("\\server\share\...").
bool			IsAbsoluteFilePath (const GS::UniString& path);
// The user's home folder (macOS $HOME, Windows %USERPROFILE%), or "" when unknown.
GS::UniString	HomeFolder ();
// Expands a leading "~" ("~", "~/x", and on Windows also "~\x") to the home folder.
GS::UniString	ExpandHome (const GS::UniString& path);
// An example absolute path for error messages, from a '/'-separated relative path:
// ExamplePath ("Projects/House.pln") is "/Users/me/Projects/House.pln" on macOS and
// "C:\Users\me\Projects\House.pln" on Windows.
GS::UniString	ExamplePath (const char* relative);

} // namespace cc
