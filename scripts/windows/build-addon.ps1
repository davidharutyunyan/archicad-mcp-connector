<#
.SYNOPSIS
  Builds the Claude Connector add-on for Archicad on Windows (x64 .apx) and optionally installs it.

.DESCRIPTION
  Needs Visual Studio 2019, 2022 or 2026 (or its Build Tools) with the C++ toolset the Archicad API headers accept:
  Archicad 25-28: the Visual Studio 2019 toolset (v142; Visual Studio 2022/2026: Visual Studio Installer > Modify >
  Individual components > "MSVC v142 - VS 2019 C++ x64/x86 build tools"); Archicad 29: the Visual Studio 2022
  toolset (v143). Also needs CMake (on PATH, or the copy bundled with Visual Studio) and Python 3. Downloads the
  official Archicad API DevKit for Windows into .devkit\ on the first run. Works on x64 and ARM64 Windows (the
  add-on is always x64, like Archicad).

  The add-on ID (MDID) comes from addon\mdid.local.cmake, or from -MdidDeveloper / -MdidLocal; without one the
  add-on builds, but licensed Archicad refuses to load it.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\windows\build-addon.ps1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\windows\build-addon.ps1 -Install
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\windows\build-addon.ps1 -ArchicadVersion 29 -Install
#>
param(
	[string] $ArchicadVersion = "26",
	[string] $DevKitTag = "",
	[string] $BuildDir = "",
	[string] $Toolset = "",			# default: v142 (Visual Studio 2019) for Archicad 25-28, v143 (Visual Studio 2022) for 29
	[string] $MdidDeveloper = "",
	[string] $MdidLocal = "",
	[switch] $Install,
	[string] $AddOnsDir = ""		# default: the Add-Ons folder of Archicad <ArchicadVersion>
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"	# Invoke-WebRequest is very slow with the progress bar

$repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$tags = @{ "25" = "25.3002"; "26" = "26.3000"; "27" = "27.6003"; "28" = "28.4001"; "29" = "29.3100" }
if (-not $DevKitTag) {
	if (-not $tags.ContainsKey($ArchicadVersion)) { throw "No DevKit tag known for Archicad $ArchicadVersion; pass -DevKitTag." }
	$DevKitTag = $tags[$ArchicadVersion]
}
if (-not $Toolset) { $Toolset = if ([int]$ArchicadVersion -ge 29) { "v143" } else { "v142" } }
if (-not $BuildDir) { $BuildDir = Join-Path $repo "addon\build-win-ac$ArchicadVersion" }

function Find-Tool([string] $name, [string[]] $extra) {
	$cmd = Get-Command $name -ErrorAction SilentlyContinue
	if ($cmd) { return $cmd.Source }
	foreach ($p in $extra) { if ($p -and (Test-Path $p)) { return $p } }
	return $null
}

# --- Visual Studio and CMake ----------------------------------------------------
# Pick the newest installation that has the requested toolset (v142 = MSVC 14.2x) and use its CMake generator.
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
	throw "Visual Studio was not found. Install 'Build Tools for Visual Studio' with the 'Desktop development with C++' workload (Archicad 25-28 also need the 'MSVC v142 - VS 2019 C++ x64/x86 build tools' component)."
}
# MSVC directory versions per toolset (v143 = Visual Studio 2022 = MSVC 14.30-14.4x).
$toolsetPrefixes = @{ "v142" = @("14.2"); "v143" = @("14.3", "14.4"); "v145" = @("14.5") }[$Toolset]
function Test-Toolset([string] $vsPath) {
	if (-not $toolsetPrefixes) { return $true }
	$msvc = Get-ChildItem (Join-Path $vsPath "VC\Tools\MSVC") -Directory -ErrorAction SilentlyContinue
	foreach ($prefix in $toolsetPrefixes) { if ($msvc | Where-Object { $_.Name -like "$prefix*" }) { return $true } }
	return $false
}
$generators = @{ "16" = "Visual Studio 16 2019"; "17" = "Visual Studio 17 2022"; "18" = "Visual Studio 18 2026" }
$vs = & $vswhere -all -prerelease -products * -format json | ConvertFrom-Json |
	Where-Object { $generators.ContainsKey($_.installationVersion.Split(".")[0]) } |
	Where-Object { Test-Toolset $_.installationPath } |
	Sort-Object { [version]$_.installationVersion } -Descending | Select-Object -First 1
if (-not $vs) {
	if ($Toolset -eq "v142") {
		throw "No Visual Studio installation with the v142 C++ toolset was found. In the Visual Studio Installer choose Modify > Individual components > 'MSVC v142 - VS 2019 C++ x64/x86 build tools' (Archicad 25-28 add-ons must be built with it)."
	}
	throw "No Visual Studio installation with the $Toolset C++ toolset was found (Archicad 29 add-ons need Visual Studio 2022 or its Build Tools, toolset v143)."
}
$vsPath = $vs.installationPath
$generator = $generators[$vs.installationVersion.Split(".")[0]]
$cmake = Find-Tool "cmake" @(Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe")
if (-not $cmake) { throw "CMake was not found. Install it (https://cmake.org) or the 'C++ CMake tools for Windows' Visual Studio component." }

$python = Find-Tool "py" @()
if (-not $python) {
	$python = Find-Tool "python" @()
	if ($python -and $python -like "*\WindowsApps\*") { $python = $null }	# Microsoft Store alias, not an interpreter
}
if (-not $python) { throw "Python 3 was not found (the DevKit compiles the add-on resources with a Python script). Install it from https://www.python.org." }

Write-Host "Visual Studio: $vsPath ($generator, toolset $Toolset)"
Write-Host "CMake:         $cmake"

# --- DevKit ---------------------------------------------------------------------
$devkitRoot = Join-Path $repo ".devkit"
$devkit = Join-Path $devkitRoot "devkit$ArchicadVersion"
$hasLib = (Test-Path (Join-Path $devkit "Support\Lib\Win\ACAP_STAT.lib")) -or (Test-Path (Join-Path $devkit "Support\Lib\ACAP_STAT.lib"))
if (-not $hasLib) {
	New-Item -ItemType Directory -Force -Path $devkitRoot | Out-Null
	$zipName = "API.Development.Kit.WIN.$DevKitTag.zip"
	$zip = Join-Path $devkitRoot $zipName
	if (-not (Test-Path $zip)) {
		Write-Host "Downloading $zipName from github.com/GRAPHISOFT/archicad-api-devkit ..."
		Invoke-WebRequest -Uri "https://github.com/GRAPHISOFT/archicad-api-devkit/releases/download/$DevKitTag/$zipName" -OutFile $zip
	}
	if (Test-Path $devkit) { Remove-Item -Recurse -Force $devkit }
	New-Item -ItemType Directory -Force -Path $devkit | Out-Null
	tar -xf $zip -C $devkit		# much faster than Expand-Archive
	if ($LASTEXITCODE -ne 0) { throw "Could not unpack $zip" }
}
Write-Host "DevKit:        $devkit"

# --- Configure and build ----------------------------------------------------------
$configureArgs = @("-S", (Join-Path $repo "addon"), "-B", $BuildDir, "-G", $generator, "-A", "x64", "-DCMAKE_GENERATOR_INSTANCE=$vsPath", "-DAC_API_DEVKIT_DIR=$devkit")
if ($Toolset) { $configureArgs += @("-T", $Toolset) }
if ($MdidDeveloper -and $MdidLocal) { $configureArgs += @("-DAC_MDID_DEVELOPER=$MdidDeveloper", "-DAC_MDID_LOCAL=$MdidLocal") }
& $cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }
& $cmake --build $BuildDir --config Release
if ($LASTEXITCODE -ne 0) { throw "The build failed." }

$apx = Join-Path $BuildDir "Release\ClaudeConnector.apx"
if (-not (Test-Path $apx)) { throw "The build finished but $apx is missing." }
Write-Host "Built: $apx"

# --- Install ----------------------------------------------------------------------
if ($Install) {
	if (-not $AddOnsDir) {
		$root = Join-Path $env:ProgramFiles "GRAPHISOFT\Archicad $ArchicadVersion"
		if (-not (Test-Path $root)) { throw "Archicad $ArchicadVersion was not found at $root; pass -AddOnsDir." }
		# The Add-Ons folder name is localized: take the top-level folder holding the most .apx files.
		$AddOnsDir = Get-ChildItem -Directory $root |
			Sort-Object { (Get-ChildItem -Recurse -Filter *.apx -Path $_.FullName -ErrorAction SilentlyContinue | Measure-Object).Count } -Descending |
			Select-Object -First 1 -ExpandProperty FullName
		if (-not $AddOnsDir) { throw "Could not find the Add-Ons folder of Archicad $ArchicadVersion; pass -AddOnsDir." }
	}
	$target = Join-Path $AddOnsDir "ClaudeConnector"
	New-Item -ItemType Directory -Force -Path $target | Out-Null
	Copy-Item -Force $apx $target
	Write-Host "Installed: $(Join-Path $target 'ClaudeConnector.apx') (restart Archicad; Options > Add-On Manager lists it)"
}
