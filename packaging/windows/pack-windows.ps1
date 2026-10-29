# Builds Photino.Native for Windows x64 and packs it as a Photino.Native NuGet package into a feed folder.
#   pwsh packaging/windows/pack-windows.ps1 -Version 4.0.23-fork.1 -Output <feed folder> [-PlatformToolset v145]
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$Output,
    [string]$Configuration = 'Release',
    [string]$PlatformToolset = ''
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '../..')
# The package is stamped with the commit it was built from, so the tree must be exactly that commit.
if (git -C $root status --porcelain) { throw 'The working tree has changes: commit them first.' }
$project = Join-Path $root 'Photino.Native/Photino.Native.vcxproj'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$msbuild = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -find 'MSBuild/**/Bin/MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'No MSBuild with the C++ tools found.' }

$toolset = @()
if ($PlatformToolset) { $toolset = @("-p:PlatformToolset=$PlatformToolset") }
& $msbuild $project -t:restore -p:RestorePackagesConfig=true "-p:Configuration=$Configuration" -p:Platform=x64 -nologo -v:minimal
if ($LASTEXITCODE -ne 0) { throw 'Restore failed.' }
& $msbuild $project "-p:Configuration=$Configuration" -p:Platform=x64 @toolset -m -nologo -v:minimal
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }

$commit = (git -C $root rev-parse HEAD).Trim()
# The repository URL in https form (an ssh remote becomes https://github.com/<owner>/<repo>).
$url = (git -C $root remote get-url origin).Trim() -replace '^git@([^:]+):', 'https://$1/' -replace '\.git$', ''
dotnet pack (Join-Path $PSScriptRoot 'Photino.Native.Windows.csproj') -c $Configuration -o $Output `
    "-p:PackageVersion=$Version" "-p:RepositoryUrl=$url" "-p:RepositoryCommit=$commit" -nologo
if ($LASTEXITCODE -ne 0) { throw 'Pack failed.' }
