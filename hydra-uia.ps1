# hydra-uia.ps1 -- start one program in a user's session, as that user, with the
# UIAccess flag on its token. Runs as SYSTEM: hydra7-avc.ps1 calls it through
# PsExec -s, once for the cursor overlay and once for the seat hold.
#
# WHY. Task View and the Start menu draw above every ordinary window, topmost
# included, on every monitor of the session -- so on seat B's panel too. The
# topmost windows of a UIAccess process sit in a band above them, the on-screen
# keyboard's layer. Windows grants UIAccess to signed exes under Program Files;
# what it checks when a window is created is a flag on the process token, and
# SYSTEM may set that flag. hydra-seat-hold-uia.ps1 proved it on 2026-11-03;
# this is the reusable form, and it runs in session 0 with no window of its own.
#
# -Job is Base64 of UTF-8 JSON, so PsExec and two rounds of command-line parsing
# never see a quote:
#   {"session":1,"cmd":"\"C:\\...\\powershell.exe\" -File ...","dir":"C:\\Programs\\hydra","elevated":true}
# elevated = your full admin token, as the Hydra Shell has; otherwise the
# filtered one. The command starts with its console window minimized.
# Prints "started <pid>" or "failed: <why>"; exit code 0 or 1.

param([Parameter(Mandatory = $true)][string]$Job)

if (-not [Security.Principal.WindowsIdentity]::GetCurrent().IsSystem) {
    'failed: not running as SYSTEM -- hydra7-avc.ps1 calls this through PsExec -s'
    exit 1
}
try {
    $spec = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($Job)) | ConvertFrom-Json
} catch {
    "failed: unreadable -Job ($($_.Exception.Message))"
    exit 1
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class HydraUiaLaunch {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] struct STARTUPINFO {
        public int cb; public string lpReserved, lpDesktop, lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2;
        public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }
    [StructLayout(LayoutKind.Sequential)] struct PROCESS_INFORMATION {
        public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId;
    }

    [DllImport("wtsapi32.dll", SetLastError = true)] static extern bool WTSQueryUserToken(uint session, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool GetTokenInformation(IntPtr token,
        int cls, out IntPtr value, int len, out int returned);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool DuplicateTokenEx(IntPtr token,
        uint access, IntPtr attrs, int level, int type, out IntPtr dup);
    [DllImport("advapi32.dll", SetLastError = true)] static extern bool SetTokenInformation(IntPtr token,
        int cls, ref int value, int len);
    [DllImport("userenv.dll", SetLastError = true)] static extern bool CreateEnvironmentBlock(out IntPtr env,
        IntPtr token, bool inherit);
    [DllImport("userenv.dll")] static extern bool DestroyEnvironmentBlock(IntPtr env);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateProcessAsUser(IntPtr token, string app, string cmd, IntPtr pa, IntPtr ta,
        bool inherit, uint flags, IntPtr env, string dir, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);

    const uint TOKEN_ALL_ACCESS = 0xF01FF, CREATE_NEW_CONSOLE = 0x10, CREATE_UNICODE_ENVIRONMENT = 0x400;
    const int SecurityImpersonation = 2, TokenPrimary = 1;
    const int TokenSessionId = 12, TokenLinkedToken = 19, TokenUIAccess = 26;
    const int STARTF_USESHOWWINDOW = 1;
    const short SW_SHOWMINNOACTIVE = 7;

    // Call straight after the failing P/Invoke, before any other.
    static string Err(string what) { return "failed: " + what + " (error " + Marshal.GetLastWin32Error() + ")"; }

    // "started <pid>", or "failed: <why>".
    public static string Start(uint session, string cmd, string dir, bool elevated) {
        IntPtr user = IntPtr.Zero, linked = IntPtr.Zero, tok = IntPtr.Zero, env = IntPtr.Zero;
        try {
            if (!WTSQueryUserToken(session, out user)) return Err("WTSQueryUserToken(" + session + ")");
            IntPtr from = user;
            if (elevated) {
                // TOKEN_LINKED_TOKEN is one HANDLE; holding SeTcbPrivilege, we get a primary token.
                // None (UAC off, or not an admin): the token we have is all there is.
                int got;
                if (GetTokenInformation(user, TokenLinkedToken, out linked, IntPtr.Size, out got)) from = linked;
                else linked = IntPtr.Zero;
            }
            if (!DuplicateTokenEx(from, TOKEN_ALL_ACCESS, IntPtr.Zero, SecurityImpersonation, TokenPrimary, out tok))
                return Err("DuplicateTokenEx");
            int sid = (int)session;   // the linked token need not carry it; without it the program starts in session 0
            if (!SetTokenInformation(tok, TokenSessionId, ref sid, 4)) return Err("TokenSessionId");
            int on = 1;
            if (!SetTokenInformation(tok, TokenUIAccess, ref on, 4)) return Err("TokenUIAccess");
            CreateEnvironmentBlock(out env, tok, false);   // your TEMP and profile, not SYSTEM's
            STARTUPINFO si = new STARTUPINFO();
            si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
            si.lpDesktop = "winsta0\\default";
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_SHOWMINNOACTIVE;
            PROCESS_INFORMATION pi;
            uint flags = CREATE_NEW_CONSOLE | (env != IntPtr.Zero ? CREATE_UNICODE_ENVIRONMENT : 0);
            if (!CreateProcessAsUser(tok, null, cmd, IntPtr.Zero, IntPtr.Zero, false, flags, env, dir, ref si, out pi))
                return Err("CreateProcessAsUser");
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return "started " + pi.dwProcessId;
        } finally {
            if (env != IntPtr.Zero) DestroyEnvironmentBlock(env);
            if (tok != IntPtr.Zero) CloseHandle(tok);
            if (linked != IntPtr.Zero) CloseHandle(linked);
            if (user != IntPtr.Zero) CloseHandle(user);
        }
    }
}
'@

$r = [HydraUiaLaunch]::Start([uint32]$spec.session, [string]$spec.cmd, [string]$spec.dir, [bool]$spec.elevated)
$r
if ($r -like 'started *') { exit 0 }
exit 1
