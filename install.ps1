# Installs xted on Windows 10/11 from a GitHub release: xted.exe, the xt.exe link and the
# language catalogs, in the user's programs folder, and puts it in the user's PATH; and for the
# desktop xtedw.exe (no console, with the icon): the Start menu, "Open with" of the text files,
# "Open with xted" in the menu of the files and of the folders. Nothing is compiled, no
# administrator is needed (HKCU only). Windows PowerShell 5.1 (the one of Windows 10) is enough.
#
#   irm https://github.com/lejubila/xted/releases/latest/download/install.ps1 | iex
#
# Settings, in the environment:
#   $env:XTED_DIR = 'C:\dir'      where to install (default: %LOCALAPPDATA%\Programs\xted)
#   $env:XTED_VERSION = '0.11.0'  a given release (default: the latest)
#   $env:XTED_URL = 'url'         where the release files are (a mirror; default: GitHub)
#   $env:XTED_UNINSTALL = '1'     removes what it installs
#   $env:XTED_DESKTOP = '0'       without the Start menu, "Open with" and the menu of the files
# or, with the script saved: .\install.ps1 [-Uninstall]
#
# Everything is in a function and an error is an exception: through "| iex" an exit would close
# the user's PowerShell window.

$ErrorActionPreference = 'Stop'
$Repo = 'lejubila/xted'
$Uninstall = ($args -contains '-Uninstall') -or $env:XTED_UNINSTALL -eq '1'

function Say($msg) { Write-Host "xted: $msg" }
function Die($msg) { throw "xted: $msg" }

function Install-Xted {
$Dir = if ($env:XTED_DIR) { $env:XTED_DIR } else { Join-Path $env:LOCALAPPDATA 'Programs\xted' }

# the user's PATH, as the registry has it (not this session's, which has the system's too)
function Get-UserPath { [Environment]::GetEnvironmentVariable('Path', 'User') }
function Set-UserPath($value) { [Environment]::SetEnvironmentVariable('Path', $value, 'User') }
function Split-Path-List($value) { if ($value) { $value -split ';' | Where-Object { $_ } } else { @() } }

# ---- the desktop: HKCU\Software\Classes (the .NET calls: "*" is a key there, not a wildcard) --
$Classes = 'Software\Classes'
$Exts = @('txt', 'md', 'markdown', 'log', 'ini', 'cfg', 'conf', 'env', 'csv', 'json', 'yaml', 'yml',
          'toml', 'xml', 'html', 'htm', 'css', 'scss', 'js', 'mjs', 'ts', 'tsx', 'jsx', 'vue', 'php',
          'py', 'rb', 'pl', 'lua', 'sh', 'bash', 'ps1', 'bat', 'cmd', 'c', 'h', 'cpp', 'hpp', 'cc',
          'cxx', 'cs', 'java', 'kt', 'go', 'rs', 'swift', 'dart', 'sql', 'diff', 'patch', 'mk',
          'cmake', 'gradle', 'properties', 'gitignore', 'editorconfig')
$Menu = @('*\shell\xted', 'Directory\shell\xted', 'Directory\Background\shell\xted')
$Link = Join-Path ([Environment]::GetFolderPath('Programs')) 'xted.lnk'

function Set-Reg($sub, $name, $value) {
    $k = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey("$Classes\$sub")
    $k.SetValue($name, $value)
    $k.Close()
}

function Remove-Reg($sub) {
    [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree("$Classes\$sub", $false)
}

# Explorer reads the associations again
function Send-AssocChanged {
    try {
        Add-Type -Namespace XtedNative -Name Shell -MemberDefinition `
            '[DllImport("shell32.dll")] public static extern void SHChangeNotify(int e, uint f, System.IntPtr a, System.IntPtr b);'
        [XtedNative.Shell]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
    } catch { }
}

function Install-Desktop($w) {
    $it = (Get-UICulture).TwoLetterISOLanguageName -eq 'it'
    $open = "`"$w`" `"%1`""
    # "Open with": xted among the programs of the text files (the default is the user's choice:
    # Windows lets no program take it by itself)
    Set-Reg 'Applications\xtedw.exe' 'FriendlyAppName' 'xted'
    Set-Reg 'Applications\xtedw.exe\DefaultIcon' '' "`"$w`",0"
    Set-Reg 'Applications\xtedw.exe\shell\open\command' '' $open
    Set-Reg 'xted.file' '' $(if ($it) { 'File di testo (xted)' } else { 'Text file (xted)' })
    Set-Reg 'xted.file\DefaultIcon' '' "`"$w`",0"
    Set-Reg 'xted.file\shell\open\command' '' $open
    foreach ($e in $Exts) {
        Set-Reg "Applications\xtedw.exe\SupportedTypes" ".$e" ''
        Set-Reg ".$e\OpenWithProgids" 'xted.file' ''
    }
    # "Open with xted" in the menu of the files, of the folders and of the inside of a folder (on
    # Windows 11 under "Show more options")
    $labels = @('Open with xted', 'Open folder in xted', 'Open xted here')
    if ($it) { $labels = @('Apri con xted', 'Apri cartella in xted', 'Apri xted qui') }
    $what = @('%1', '%1', '%V')
    for ($i = 0; $i -lt 3; $i++) {
        Set-Reg $Menu[$i] '' $labels[$i]
        Set-Reg $Menu[$i] 'Icon' "`"$w`",0"
        Set-Reg "$($Menu[$i])\command" '' "`"$w`" `"$($what[$i])`""
    }
    # the Start menu
    $sh = New-Object -ComObject WScript.Shell
    $lnk = $sh.CreateShortcut($Link)
    $lnk.TargetPath = $w
    $lnk.IconLocation = "$w,0"
    $lnk.WorkingDirectory = $env:USERPROFILE
    $lnk.Description = 'Text editor for the terminal'
    if ($it) { $lnk.Description = 'Editor di testo nel terminale' }
    $lnk.Save()
    Send-AssocChanged
}

function Remove-Desktop {
    foreach ($m in $Menu) { Remove-Reg $m }
    Remove-Reg 'Applications\xtedw.exe'
    Remove-Reg 'xted.file'
    foreach ($e in $Exts) {
        $k = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey("$Classes\.$e\OpenWithProgids", $true)
        if ($k) { $k.DeleteValue('xted.file', $false); $k.Close() }
    }
    Remove-Item -Force $Link -ErrorAction SilentlyContinue
    Send-AssocChanged
}

# ---- uninstall ------------------------------------------------------------
if ($Uninstall) {
    if (-not (Test-Path (Join-Path $Dir 'xted.exe'))) {
        Die "xted is not installed in $Dir (set XTED_DIR to the folder used)"
    }
    Remove-Desktop
    Remove-Item -Recurse -Force $Dir
    $rest = Split-Path-List (Get-UserPath) | Where-Object { $_.TrimEnd('\') -ne $Dir.TrimEnd('\') }
    Set-UserPath ($rest -join ';')
    Say "removed from $Dir (the settings in $env:APPDATA\xted are left)"
    return
}

# ---- the system -----------------------------------------------------------
$build = [Environment]::OSVersion.Version.Build
if ([Environment]::OSVersion.Platform -ne 'Win32NT' -or $build -lt 17763) {
    Die "Windows 10 1809 or later is needed (its console has the VT mode xted draws with)"
}
$arch = $env:PROCESSOR_ARCHITECTURE
if ($env:PROCESSOR_ARCHITEW6432) { $arch = $env:PROCESSOR_ARCHITEW6432 }
if ($arch -ne 'AMD64' -and $arch -ne 'ARM64') {
    Die "only 64-bit Windows has a ready xted.exe (this is $arch)"
}
if ($build -lt 18362) {
    Say "note: file names outside ASCII need Windows 10 1903 or later"
}
# Windows PowerShell 5.1 does not offer TLS 1.2 by itself, and GitHub wants it
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor
    [Net.SecurityProtocolType]::Tls12
$ProgressPreference = 'SilentlyContinue'    # the progress bar makes downloads very slow on 5.1

if ($env:XTED_URL) {
    $base = $env:XTED_URL.TrimEnd('/')
} elseif ($env:XTED_VERSION) {
    $base = "https://github.com/$Repo/releases/download/v$($env:XTED_VERSION.TrimStart('v'))"
} else {
    $base = "https://github.com/$Repo/releases/latest/download"
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("xted-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    # ---- download and check ---------------------------------------------------
    # SHA256SUMS names the archive, so it also says which version "latest" is
    $sums = Join-Path $tmp 'SHA256SUMS'
    try { Invoke-WebRequest -UseBasicParsing -Uri "$base/SHA256SUMS" -OutFile $sums }
    catch { Die "cannot download $base/SHA256SUMS" }
    $line = Get-Content $sums | Where-Object { $_ -match 'windows-x86_64\.zip$' } | Select-Object -First 1
    if (-not $line) { Die "the release has no archive for Windows" }
    $want, $name = $line -split '\s+', 2
    $name = $name.TrimStart('*')
    Say "downloading $name"
    $zip = Join-Path $tmp $name
    try { Invoke-WebRequest -UseBasicParsing -Uri "$base/$name" -OutFile $zip }
    catch { Die "cannot download $base/$name" }
    if ((Get-FileHash -Algorithm SHA256 $zip).Hash -ne $want.ToUpper()) {
        Die "$name does not match its SHA256SUMS: not installed"
    }
    Expand-Archive -Path $zip -DestinationPath $tmp
    $src = Get-ChildItem -Directory $tmp | Where-Object { Test-Path (Join-Path $_.FullName 'xted.exe') } |
        Select-Object -First 1
    if (-not $src) { Die "$name has no xted.exe" }

    # ---- install --------------------------------------------------------------
    New-Item -ItemType Directory -Force -Path (Join-Path $Dir 'lang') | Out-Null
    $exe = Join-Path $Dir 'xted.exe'
    $xt = Join-Path $Dir 'xt.exe'
    $xw = Join-Path $Dir 'xtedw.exe'
    # a running xted keeps its file open: it can be renamed, not overwritten
    foreach ($f in @($xt, $exe, $xw)) {
        if (Test-Path $f) {
            $old = "$f.old"
            Remove-Item -Force $old -ErrorAction SilentlyContinue
            try { Move-Item -Force $f $old } catch { Die "cannot replace $f" }
        }
    }
    Copy-Item (Join-Path $src.FullName 'xted.exe') $exe
    # xt: the same program under a short name, a hard link (no administrator needed), or a copy
    try { New-Item -ItemType HardLink -Path $xt -Target $exe | Out-Null }
    catch { Copy-Item $exe $xt }
    $hasw = Test-Path (Join-Path $src.FullName 'xtedw.exe')
    if ($hasw) { Copy-Item (Join-Path $src.FullName 'xtedw.exe') $xw }
    Copy-Item (Join-Path $src.FullName 'lang\*') (Join-Path $Dir 'lang') -Force
    foreach ($f in @('README.md', 'LICENSE')) {
        $p = Join-Path $src.FullName $f
        if (Test-Path $p) { Copy-Item $p $Dir -Force }
    }
    foreach ($f in @("$xt.old", "$exe.old", "$xw.old")) { Remove-Item -Force $f -ErrorAction SilentlyContinue }
    if ($hasw -and $env:XTED_DESKTOP -ne '0') {
        Install-Desktop $xw
        $msg = "in the Start menu, in `"Open with`" of the text files and in `"Open with xted`" of the menu of the files"
        if ((Get-UICulture).TwoLetterISOLanguageName -eq 'it') {
            $msg = "nel menu Start, in `"Apri con`" dei file di testo e in `"Apri con xted`" del menu dei file"
        }
        Say $msg
    }

    $version = (& $exe --version | Select-Object -First 1)
    Say "installed $version in $Dir (also as xt)"
    $path = Split-Path-List (Get-UserPath)
    if (-not ($path | Where-Object { $_.TrimEnd('\') -eq $Dir.TrimEnd('\') })) {
        Set-UserPath ((@($path) + $Dir) -join ';')
        Say "$Dir added to your PATH: open a new terminal to use xted"
    }
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
}

try { Install-Xted } catch { Write-Host $_.Exception.Message -ForegroundColor Red }
