# Builds AMETHYST 2026, makes the Windows installer and the update manifest, and (optionally) publishes them.
#
#   .\tools\make-release.ps1 -Notes "Count-in, punch-in and autosave"
#   .\tools\make-release.ps1 -Notes "..." -Publish        # uploads to Supabase (needs SUPABASE_SERVICE_ROLE_KEY)
#
# Version: the one in CMakeLists.txt  (project(VocalDAW VERSION x.y.z)). Bump it before each release.
# Output:  dist\AMETHYST-2026-Setup-<version>.exe  and  dist\latest.json
#
# Publishing uses the service-role key from the SUPABASE_SERVICE_ROLE_KEY environment variable.
# It is only sent to your own Supabase project and is never written to disk or printed.
param(
    [string] $Notes = "",
    [switch] $Publish,
    [switch] $SkipBuild
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

# version from CMakeLists.txt
$cm = Get-Content "CMakeLists.txt" -Raw
if ($cm -notmatch 'project\(VocalDAW VERSION ([0-9]+\.[0-9]+\.[0-9]+)') { throw "Couldn't find the version in CMakeLists.txt" }
$version = $Matches[1]

# Supabase URL from config/supabase.json (public URL only)
$cfg = Get-Content "config/supabase.json" -Raw | ConvertFrom-Json
$base = $cfg.url.TrimEnd('/')
if (-not $base.StartsWith("https://")) { throw "config/supabase.json needs an https url" }

Write-Host "AMETHYST 2026 $version"

if (-not $SkipBuild) {
    & "C:\Program Files\CMake\bin\cmake.exe" -S . -B build | Out-Null
    & "C:\Program Files\CMake\bin\cmake.exe" --build build --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw "Build failed" }
}

# installer
$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "C:\Program Files (x86)\Inno Setup 6\ISCC.exe", "C:\Program Files\Inno Setup 6\ISCC.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw "Inno Setup 6 isn't installed (winget install JRSoftware.InnoSetup)" }
New-Item -ItemType Directory -Force "dist" | Out-Null
& $iscc "/DAppVersion=$version" "/Qp" "installer\AMETHYST2026.iss"
if ($LASTEXITCODE -ne 0) { throw "Installer build failed" }

$setupName = "AMETHYST-2026-Setup-$version.exe"
$setup = Join-Path "dist" $setupName
$hash = (Get-FileHash $setup -Algorithm SHA256).Hash.ToLower()
$manifest = [ordered]@{
    version = $version
    url     = "$base/storage/v1/object/public/releases/$setupName"
    sha256  = $hash
    notes   = $Notes
}
$manifest | ConvertTo-Json | Set-Content -Encoding utf8 "dist\latest.json"
Write-Host "Installer: $setup  ($([math]::Round((Get-Item $setup).Length / 1MB, 1)) MB)"
Write-Host "Manifest:  dist\latest.json"

if ($Publish) {
    $key = $env:SUPABASE_SERVICE_ROLE_KEY
    if (-not $key) { throw "Set SUPABASE_SERVICE_ROLE_KEY for this PowerShell session to publish (Supabase > Project settings > API)." }
    $headers = @{ "Authorization" = "Bearer $key"; "apikey" = $key; "x-upsert" = "true" }
    # installer first, so the manifest never points at a file that isn't there yet
    Invoke-RestMethod -Method Post -Uri "$base/storage/v1/object/releases/$setupName" -Headers ($headers + @{ "Content-Type" = "application/octet-stream" }) -InFile $setup | Out-Null
    Invoke-RestMethod -Method Post -Uri "$base/storage/v1/object/releases/latest.json" -Headers ($headers + @{ "Content-Type" = "application/json"; "Cache-Control" = "max-age=60" }) -InFile "dist\latest.json" | Out-Null
    Write-Host "Published $version - running copies will offer the update within a few hours (or at their next start)."
} else {
    Write-Host "Not published. To publish: upload both files from dist\ to the 'releases' bucket in the Supabase dashboard (installer first), or run again with -Publish."
}
