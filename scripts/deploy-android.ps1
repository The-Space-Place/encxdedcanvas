# Builds the debug APK (arm64 only), installs it on the connected tablet over adb, launches it, and streams its log.
#
# Run from the repo root in PowerShell, e.g.:
#   powershell -ExecutionPolicy Bypass -File .\scripts\deploy-android.ps1 [options]
#
# One-time wireless setup (tablet: Settings > Developer options > Wireless debugging). Close scrcpy first:
#   ... -Pair 192.168.1.50:37123 [-Code 123456]   # ip:port from "Pair device with pairing code"; prompts for the code if omitted
#   ... -Connect 192.168.1.50:41234               # ip:port shown on the main Wireless debugging screen
#
# Every iteration:
#   ... deploy-android.ps1            # build + install + launch + logcat
#   ... deploy-android.ps1 -NoBuild   # reinstall the last APK
#   ... deploy-android.ps1 -NoLog     # don't attach logcat afterwards
#
# Prerequisite: conan deps for armv8 Debug (see docs/BUILDING.md, Android). Gradle runs the conan install itself.

param(
    [string]$Pair,
    [string]$Code,
    [string]$Connect,
    [switch]$NoBuild,
    [switch]$NoLog,
    [switch]$Release
)

# Not "Stop": adb and gradle write informational text to stderr, which Windows PowerShell would treat as fatal.
# Failures are detected explicitly below instead.
$ErrorActionPreference = "Continue"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$AndroidProject = Join-Path $RepoRoot "android-project"
$Sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { Join-Path $env:LOCALAPPDATA "Android\Sdk" }
$Adb = Join-Path $Sdk "platform-tools\adb.exe"

$AppId = "com.joshcarter.encxdedcanvas"
$Activity = "com.erroratline0.infinipaint.InfiniPaint"
$Variant = if ($Release) { "Release" } else { "Debug" }
if (-not $Release) { $AppId = "$AppId.debug" }

function Fail([string]$msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }

# Runs adb and returns its combined stdout/stderr as plain text.
function Invoke-AdbText { return (& $Adb @args 2>&1 | ForEach-Object { "$_" } | Out-String) }

if (-not (Test-Path $Adb)) { Fail "adb not found at $Adb. Install Android SDK platform-tools or set ANDROID_HOME." }

Invoke-AdbText start-server | Out-Null

if ($Pair) {
    if (-not $Code) { $Code = Read-Host "Enter the 6-digit pairing code shown on the tablet" }
    $pairOut = Invoke-AdbText pair $Pair $Code
    Write-Host $pairOut
    if ($pairOut -notmatch "Successfully paired") {
        Fail "Pairing failed. Close scrcpy (it bundles its own adb), run '& `"$Adb`" kill-server', open a fresh pairing code on the tablet, and retry."
    }
    if (-not $Connect) { Write-Host "Paired. Now run with -Connect <ip:port> shown on the main Wireless debugging screen (not the pairing port)."; exit 0 }
}
if ($Connect) {
    $connOut = Invoke-AdbText connect $Connect
    Write-Host $connOut
    if ($connOut -notmatch "connected to") { Fail "Connect failed. Use the IP:port from the main Wireless debugging screen, and make sure pairing succeeded." }
}

# Target the first online device explicitly so stale/offline entries (e.g. old emulators) don't cause "more than one device".
$serial = (Invoke-AdbText devices) -split "`r?`n" | Where-Object { $_ -match "^(\S+)\s+device$" } |
    ForEach-Object { ($_ -split "\s+")[0] } | Select-Object -First 1
if ($Connect -and ((Invoke-AdbText devices) -match [regex]::Escape($Connect) + "\s+device")) { $serial = $Connect }
if (-not $serial) { Fail "No device connected. Use -Connect <ip:port> (see header of this script)." }
Write-Host "Using device $serial"

if (-not $NoBuild) {
    # Gradle 8.13 can't run on JDK 25 (the system and Android Studio default here); use a JDK 17/21 if one is installed.
    $jdk = Get-ChildItem "$env:USERPROFILE\.jdks" -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '-(17|21)\.' } | Sort-Object Name -Descending | Select-Object -First 1
    if ($jdk) { $env:JAVA_HOME = $jdk.FullName }
    else { Write-Warning "No JDK 17/21 found in ~/.jdks; Gradle may fail on JDK 25. Install one via Android Studio > Settings > Build Tools > Gradle > Gradle JDK > Download JDK." }

    Push-Location $AndroidProject
    & .\gradlew.bat ":app:assemble$Variant" "-Pabis=arm64-v8a"
    $gradleExit = $LASTEXITCODE
    Pop-Location
    if ($gradleExit -ne 0) { Fail "Gradle build failed." }
}

$apkDir = Join-Path $AndroidProject "app\build\outputs\apk\$($Variant.ToLower())"
$apk = Get-ChildItem $apkDir -Filter *.apk -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $apk) { Fail "No APK found in $apkDir" }

Write-Host "Installing $($apk.Name) ($([math]::Round($apk.Length / 1MB)) MB)..."
$installOut = Invoke-AdbText -s $serial install -r -d $apk.FullName
Write-Host $installOut
if ($installOut -notmatch "Success") { Fail "adb install failed." }

Invoke-AdbText -s $serial shell am force-stop $AppId | Out-Null
Invoke-AdbText -s $serial shell am start -n "$AppId/$Activity" | Out-Null
Write-Host "Launched $AppId"

if (-not $NoLog) {
    Start-Sleep -Milliseconds 800
    $procId = (Invoke-AdbText -s $serial shell pidof $AppId).Trim()
    if ($procId -match '^\d+$') {
        Write-Host "Streaming logcat for $AppId (pid $procId). Ctrl+C to stop; the app keeps running."
        & $Adb -s $serial logcat --pid=$procId
    } else {
        Write-Host "App launched but its pid wasn't found; run '& `"$Adb`" logcat' manually."
    }
}
