# broker/tools/set_kite_secret.ps1 -- set a Kite credential without it going
# anywhere it can be read back.
#
# P2-10e. Written because the alternatives are all worse:
#
#   setx ALTAIR_KITE_API_SECRET <value>     puts it in PowerShell history
#   $env:X = "<value>"                      same, and dies with the shell
#   the Settings GUI                        safe, but four dialogs deep and
#                                           easy to edit the wrong field
#
# This prompts with masked input, so the secret is never displayed, never in
# the command line, never in the shell's history buffer, and never in a file.
# It is checked for SHAPE before being stored, because the failure this exists
# to prevent -- a 29-character value sitting where a 32-character secret
# belongs -- produced three failed logins that all reported "Invalid checksum"
# and looked like an expired token.
#
#   powershell -ExecutionPolicy Bypass -File broker\tools\set_kite_secret.ps1
#
# It writes the USER scope, which persists across reboots and is what
# altair_kite_login reads. Processes already running keep the old value: a new
# terminal is required afterwards, and this says so rather than leaving it to
# be discovered.

param(
    [ValidateSet("secret", "key")]
    [string]$Which = "secret"
)

$name   = if ($Which -eq "key") { "ALTAIR_KITE_API_KEY" } else { "ALTAIR_KITE_API_SECRET" }
$expect = if ($Which -eq "key") { 16 } else { 32 }

Write-Host ""
Write-Host "  Setting $name"
Write-Host "  ------------------------------------------------------------"

$current = [Environment]::GetEnvironmentVariable($name, "User")
if ($current) {
    $okNow = ($current.Length -eq $expect) -and ($current -cmatch '^[a-z0-9]+$')
    Write-Host "  currently: $($current.Length) chars, valid shape: $okNow"
} else {
    Write-Host "  currently: not set"
}
Write-Host ""
Write-Host "  Paste the value from the Kite developer console."
Write-Host "  NOTHING WILL APPEAR AS YOU PASTE. That is deliberate."
Write-Host ""

$secure = Read-Host "  $name" -AsSecureString
$bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
try {
    $value = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr)
} finally {
    # Zero the unmanaged copy. The managed string below still exists until GC,
    # which is a real limitation of doing this in PowerShell at all -- but it
    # is a great deal better than a value that persists in a history file.
    [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
}

if ([string]::IsNullOrWhiteSpace($value)) {
    Write-Host ""
    Write-Host "  Nothing entered. Nothing changed." -ForegroundColor Yellow
    exit 1
}

$value = $value.Trim()

# SHAPE IS CHECKED BEFORE STORING, and a mismatch REFUSES.
#
# Storing a wrong-shaped value is what caused this whole detour: the failure
# surfaces later, as a checksum error during a login, which reads like an
# expired token and sends you to log in again. Refusing here costs one retry;
# accepting it costs a login round trip and a burned request_token.
$lenOk = $value.Length -eq $expect
$chrOk = $value -cmatch '^[a-z0-9]+$'

if (-not ($lenOk -and $chrOk)) {
    Write-Host ""
    Write-Host "  REFUSED -- that is not the shape of a Kite $Which." -ForegroundColor Red
    Write-Host "    length   : $($value.Length)   (expected $expect)"
    Write-Host "    charset  : $(if ($chrOk) { 'ok' } else { 'contains something outside a-z 0-9' })"
    Write-Host ""
    Write-Host "  Nothing was stored. Check you copied the whole value from"
    Write-Host "  Apps -> your app -> API $Which, with no spaces or quotes."
    exit 1
}

[Environment]::SetEnvironmentVariable($name, $value, "User")

# Read it back from the registry rather than trusting the write.
$after = [Environment]::GetEnvironmentVariable($name, "User")
$ok = $after -and ($after.Length -eq $expect) -and ($after -cmatch '^[a-z0-9]+$')

Write-Host ""
if ($ok) {
    Write-Host "  STORED. $name is now $($after.Length) chars, valid shape." -ForegroundColor Green
    Write-Host ""
    Write-Host "  OPEN A NEW TERMINAL before the next step. Windows gives the"
    Write-Host "  new value only to processes started after this point, so the"
    Write-Host "  window you are in right now still has the old one."
    Write-Host ""
    Write-Host "  Then:"
    Write-Host "      cd C:\PycharmProjects\altair"
    Write-Host "      python broker/tools/kite_callback.py"
} else {
    Write-Host "  The write did not take effect. Nothing to trust here --" -ForegroundColor Red
    Write-Host "  set it through Settings -> Environment Variables instead."
    exit 1
}
