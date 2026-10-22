#include "Core/Paths.hpp"

#include <cstdlib>

namespace cc {

bool IsPathSeparator (GS::UniChar c)
{
#if defined (WINDOWS)
	return c == '\\' || c == '/';
#else
	return c == '/';
#endif
}


UIndex LastPathSeparator (const GS::UniString& path)
{
	for (UIndex i = path.GetLength (); i > 0; --i) {
		if (IsPathSeparator (path[i - 1]))
			return i - 1;
	}
	return MaxUIndex;
}


bool IsAbsoluteFilePath (const GS::UniString& path)
{
#if defined (WINDOWS)
	if (path.GetLength () >= 3 && path[1] == ':' && IsPathSeparator (path[2]))
		return true;
	return path.BeginsWith ("\\\\") || path.BeginsWith ("//");
#else
	return path.BeginsWith (GS::UniChar ('/'));
#endif
}


GS::UniString HomeFolder ()
{
#if defined (WINDOWS)
	wchar_t buffer[1024];
	const DWORD length = GetEnvironmentVariableW (L"USERPROFILE", buffer, 1024);
	if (length == 0 || length >= 1024)
		return GS::UniString ();
	return GS::UniString (reinterpret_cast<const GS::UniChar::Layout*> (buffer), (USize) length);
#else
	const char* home = std::getenv ("HOME");
	return home != nullptr ? GS::UniString (home, CC_UTF8) : GS::UniString ();
#endif
}


GS::UniString ExpandHome (const GS::UniString& path)
{
	if (path == "~" || (path.GetLength () > 1 && path[0] == '~' && IsPathSeparator (path[1]))) {
		const GS::UniString home = HomeFolder ();
		if (!home.IsEmpty ())
			return home + path.GetSubstring (1, path.GetLength () - 1);
	}
	return path;
}


GS::UniString ExamplePath (const char* relative)
{
#if defined (WINDOWS)
	GS::UniString path = GS::UniString ("C:\\Users\\me\\") + GS::UniString (relative, CC_UTF8);
	path.ReplaceAll (GS::UniChar ('/'), GS::UniChar ('\\'));
	return path;
#else
	return GS::UniString ("/Users/me/") + GS::UniString (relative, CC_UTF8);
#endif
}

} // namespace cc
