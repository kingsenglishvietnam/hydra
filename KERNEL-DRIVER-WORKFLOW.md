> **WITHDRAWN 2026-11-12 evening.** Premise wrong: termsrv does not block
> provider sessions. Run A (31b1354) is a full desktop under the same patched
> termsrv. KMDOD adds a display to the console session; it does not create a
> second session. A second session still requires termsrv and the patch.
> Do not install KMDOD on the teaching machine. See MODE4-RESULTS.md.
# KERNEL-DRIVER-WORKFLOW.md — Path A, WDDM miniport replacing RDP

Written 2026-11-12. Supersedes MODE4-RETEST-GUARD.md as the active procedure.
Mode 4 docs (MODE4-RETEST-GUARD.md, MODE4-RESULTS.md) remain in the repo as
history. See KERNEL-DRIVER-PLAN.md for why the provider path is closed.

## What this covers

Steps from "WDK is installed" to "kernel miniport owns the display path and
streams frames over the network." Each step has a gate. A failed gate ends the
session; do the rollback for that level and stop.

## What is different about kernel work

- A bug in a display driver bugchecks the whole machine. Every load is a
  boot-affecting change. The safety-gate is not optional.
- The undo mechanism is stronger. WinRE + the recovery stick is the floor,
  not the ceiling: a bad display driver can leave the machine unable to show
  a login screen. If you cannot see the physical console, you cannot fix it.
- Build signing is required. Test-signing is on (from mode 4 work), but the
  miniport needs a real .cat and a signed .sys before pnputil will install it.
- The development loop is slower than user-mode. Plan for more reboots.

## Preconditions

### P1. WDK installed, matching the SDK

```powershell
Test-Path 'C:\Program Files (x86)\Windows Kits\10\Include\26100.0\km'
Test-Path 'C:\Program Files (x86)\Windows Kits\10\Include\26100.0\wdf'
Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\build' -ErrorAction SilentlyContinue |
    Select-Object Name
```

**GATE:** km True, wdf True, build lists WDK .props/.targets.

A "VSIX could not install into BuildTools" warning during WDK install is
harmless. Command-line msbuild uses the build/ props, not the VSIX.

### P2. Test-signing still on

```powershell
bcdedit /enum "{current}" | Select-String 'testsigning'
```

**GATE:** testsigning Yes.

### P3. The signing cert is available

```powershell
Get-ChildItem Cert:\LocalMachine\My | Where-Object Subject -match 'HydraTest' |
    Select-Object Subject, Thumbprint, NotAfter
Get-ChildItem Cert:\LocalMachine\TrustedPublisher -ErrorAction SilentlyContinue |
    Where-Object Subject -match 'HydraTest' | Select-Object Subject, NotAfter
```

**GATE:** cert exists in My, and in TrustedPublisher (so the driver installs
without a prompt). If it is only in My, add it to TrustedPublisher:

```powershell
$c = Get-ChildItem Cert:\LocalMachine\My | Where-Object Subject -match 'HydraTest' | Select-Object -First 1
Export-Certificate -Cert $c -FilePath $env:TEMP\hydra-test.cer -Type CERT
Import-Certificate -FilePath $env:TEMP\hydra-test.cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher
```

### P4. Hydra Shell environment

The Hydra Shell (cmd -> vcvars64 -> pwsh) sets up MSVC x64. From that prompt:

```powershell
where.exe cl
where.exe msbuild
```

**GATE:** both resolve. If msbuild is missing, run `vcvars64.bat` from the
VS 2022 BuildTools path.

### P5. KMDOD sample on disk

```powershell
Get-ChildItem C:\Users\user\Downloads\wdk-samples -Recurse -Filter 'KMDOD*' -Directory |
    Select-Object FullName
```

**GATE:** exactly one directory listed. Typical path is
`wdk-samples\video\KernelModeDisplayOnly\`. Note the actual path.

If not cloned:

```powershell
Set-Location C:\Users\user\Downloads
git clone --depth 1 https://github.com/microsoft/Windows-driver-samples.git wdk-samples
```

## Step 1 - Build the unmodified KMDOD sample

This is the toolchain and signing proof. No code changes.

### 1a. Locate the project file

```powershell
$kmdod = '<path from P5>'
Get-ChildItem $kmdod -Filter '*.vcxproj' | Select-Object Name, FullName
Get-Content "$kmdod\*.vcxproj" | Select-String 'PlatformToolset|WindowsTargetPlatformVersion' |
    Select-Object -First 4
```

Note the current values. Typical for the current sample:
- PlatformToolset: v143 (VS 2022) or v142 (older)
- WindowsTargetPlatformVersion: 10.0.26100.0 or similar

### 1b. Build

```powershell
Set-Location $kmdod
msbuild (Get-ChildItem -Filter '*.vcxproj' | Select-Object -First 1).FullName `
    /t:Rebuild /p:Configuration=Debug /p:Platform=x64 `
    /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0.26100.0
```

**GATE:** "Build succeeded. 0 Errors". Output KMDOD.sys appears under
`$kmdod\x64\Debug\` (or similar). Note the exact path.

If the build fails:
- `MSB8020 v142 not found` -> the `/p:PlatformToolset=v143` is being ignored;
  check the project file is the one listed in 1a
- `SDK not found` -> `/p:WindowsTargetPlatformVersion` doesn't match an
  installed kit; verify with `Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\Include'`
- `error C2001` etc. -> sample uses APIs removed from current SDK; stop, we
  need a different base sample

### 1c. Sign the driver

Test-signing is on, but pnputil still refuses unsigned .sys. Use the cert
from P3.

```powershell
$out = '<output path from 1b>'
$sys = Join-Path $out 'KMDOD.sys'

# Version stamp the INF (stampinf may need the current date)
$inf = Join-Path $kmdod 'KMDOD.inf'
stampinf -f $inf -d '*' -v 1.0.0.0

# Regenerate the .cat from the .inf
inf2cat /driver:$kmdod /os:10_X64
# (if inf2cat complains about missing .sys references, verify the INF matches
# the actual driver name; some samples rename one but not the other)

# Sign the .sys and the .cat
$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object Subject -match 'HydraTest' |
    Select-Object -First 1
signtool sign /fd SHA256 /sha1 $cert.Thumbprint /tr http://timestamp.digicert.com /td SHA256 "$out\KMDOD.sys"
signtool sign /fd SHA256 /sha1 $cert.Thumbprint /tr http://timestamp.digicert.com /td SHA256 "$kmdod\*.cat"
```

**GATE:** both `signtool` calls report "Successfully signed". Verify:

```powershell
Get-AuthenticodeSignature "$out\KMDOD.sys" | Select-Object Status, SignerCertificate
Get-AuthenticodeSignature (Get-ChildItem "$kmdod\*.cat" | Select-Object -First 1).FullName |
    Select-Object Status, SignerCertificate
```

Both should show `Valid` and `CN=HydraTest`.

### 1d. Safety gate before install

A kernel display driver is the highest-risk load this machine has seen.
The safety-gate snapshot is the floor, not the ceiling: verify the recovery
stick is readable from WinPE BEFORE this install, because a bad display
driver can hide the login screen.

```powershell
Set-Location C:\Programs\hydra
.\safety-gate.ps1 -Label 'kmdod-baseline'
```

**GATE:** ALL THREE LEVELS PRESENT.

Also confirm: physical keyboard and mouse at the machine, monitor on, and
you know how to boot to WinRE from the recovery partition (usually: hold
Shift while clicking Restart from the login screen). Write the WinRE
keystroke down. If the display driver fails to initialize and Windows falls
back to the VGA driver, you get a low-res login at 640x480 - usable. If it
BSODs at boot, WinRE is the only path.

### 1e. Install

```powershell
pnputil /add-driver "$kmdod\KMDOD.inf" /install
```

**GATE:** "Driver package added successfully" and "Published Name: oemNN.inf".

Note the oemNN. It will be needed for rollback.

If pnputil fails with signature errors: the .cat is not properly signed, or
the cert is not in TrustedPublisher. Re-check P3.

### 1f. Verify the display exists

```powershell
Start-Sleep -Seconds 5
Get-PnpDevice -Class Display -PresentOnly |
    Select-Object Status, FriendlyName, InstanceId
```

**GATE:** a new display adapter listed with Status=OK. Expected friendly
name contains "KMDOD" or "Microsoft Basic Render" or similar.

Do not proceed until Windows Display Settings shows a second monitor. If
the PnP entry exists but the monitor doesn't appear, the miniport started
but didn't advertise a mode - check KMDOD's VidPn setup in the sample
source. That's a sample-side bug, not a toolchain bug, but it counts as a
failure for today's purpose (proving the toolchain works). Roll back and
report.

### 1g. Rollback if 1f fails

```powershell
pnputil /delete-driver oemNN.inf /uninstall /force
Get-PnpDevice -Class Display -PresentOnly |
    Select-Object Status, FriendlyName, InstanceId
```

**GATE:** the new adapter is gone from Device Manager. Reboot if Windows
still shows it in Display Settings.

Stop. Do not iterate forward. Report what 1f and 1g showed.

## Step 2 - Add a kernel-side log

Only after Step 1 succeeds: the toolchain works, the driver loads, and the
machine is stable. Now instrument.

The goal is to prove you can write from the kernel to a known file.

### 2a. Add a log helper

```cpp
// In KMDOD's main header, add near the top:
static void HydraKmdLog(const char* fmt, ...)
{
    // Note: ZwCreateFile/ZwWriteFile from a display miniport is unusual
    // and slow. For the baseline we do it once at DxgkDdiStartDevice,
    // not per-frame. Per-frame logging is added at Step 3 via a ring
    // buffer that a user-mode reader drains (see below).
}
```

For the baseline, use a one-shot log write from `DxgkDdiStartDevice`.
Do not attempt per-frame logging from the miniport. Per-frame logging
belongs to a ring buffer that a user-mode reader drains (see below).

The simplest correct approach for the baseline:

```cpp
// In DxgkDdiStartDevice, immediately after successful hardware init:
HANDLE hFile;
UNICODE_STRING path;
OBJECT_ATTRIBUTES attr;
IO_STATUS_BLOCK iosb;
RtlInitUnicodeString(&path, L"\\SystemRoot\\Temp\\kmdod.log");
InitializeObjectAttributes(&attr, &path,
    OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
NTSTATUS ns = ZwCreateFile(&hFile,
    FILE_APPEND_DATA | SYNCHRONIZE, &attr, &iosb, NULL,
    FILE_ATTRIBUTE_NORMAL, 0, FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
if (NT_SUCCESS(ns)) {
    const char* msg = "KMDOD: DxgkDdiStartDevice entered\n";
    ZwWriteFile(hFile, NULL, NULL, NULL, &iosb, (PVOID)msg, (ULONG)strlen(msg), NULL, NULL);
    ZwClose(hFile);
}
```

Add the necessary includes (`ntifs.h` in the driver's precompiled header
if not already there).

### 2b. Rebuild, sign, install

Same as 1b-1e, but with `/t:Rebuild` and the incremental install:

```powershell
pnputil /add-driver "$kmdod\KMDOD.inf" /install
```

If pnputil says the driver is already installed and won't update, use:

```powershell
pnputil /delete-driver oemNN.inf /uninstall /force
pnputil /add-driver "$kmdod\KMDOD.inf" /install
```

### 2c. Verify the log

```powershell
Get-Content C:\Windows\Temp\kmdod.log -ErrorAction SilentlyContinue
```

**GATE:** file exists, contains "DxgkDdiStartDevice entered".

If the file does not exist: the kernel write failed (permissions, path,
or the driver never reached that code). Add a `DbgPrint` as fallback and
attach DebugView, or use the WDK's driver logging (`WPP`). Do not proceed
to Step 3 until a file write from the kernel is proven.

## Step 3 - Frame capture, no network

### 3a. The capture point

`DxgkDdiPresent` is where swapchains are flipped. Add a hook that copies
the primary surface (the one DWM composed to) into a ring buffer. The ring
buffer is mapped into user-mode via a shared section that a companion
user-mode process opens.

```cpp
// In DxgkDdiPresent, after the flip completes:
// 1. Read pPresent->pDmaBuffer or the swapchain allocation (depends on sample)
// 2. Copy into gRingBuffer at gWriteOffset
// 3. Advance gWriteOffset mod gRingSize
// 4. Signal an event that the user-mode reader waits on
```

The KMDOD sample presents a virtual adapter with no real display hardware.
Copying from a present may not work as-is depending on how the sample
handles flip vs. blt. Read the sample source for its present path before
writing this code.

### 3b. The user-mode reader

A small companion process (start with `hydra-frame-reader.exe`) that:

1. Opens the shared section (`\BaseNamedObjects\HydraKmdRing`)
2. Waits on the signal event
3. Copies frames to a known file (`C:\HydraLog\frame-NNNN.bin`)
4. Does nothing else

This is the proof that kernel-side pixels reach user-mode. No network yet.

### 3c. Gate

```powershell
Get-ChildItem C:\HydraLog\frame-*.bin | Measure-Object | Select-Object Count
Get-Item (Get-ChildItem C:\HydraLog\frame-*.bin | Sort-Object LastWriteTime -Descending |
    Select-Object -First 1).FullName | Select-Object Length, LastWriteTime
```

**GATE:** frame files exist with size matching the surface (e.g. 1920 * 1080 *
4 = 8294400 for 32-bit BGRA). A file of the right size proves the ring buffer
and reader work. A file of wrong size means the ring buffer indexing is
broken.

Stop. Do not iterate. Report the file count and size.

## Step 4 - Wire to existing encoder/transport

Only after Step 3 succeeds. The frame files prove the kernel->user path
works. This step reuses the encoder and transport from the existing remote
seat work (the `abcap:B` / `abren:B` pipeline).

### 4a. Replace the source

In the encoder's input path, replace "capture from screen" with "read from
`\BaseNamedObjects\HydraKmdRing`". The encoder stays as-is; only the frame
source changes.

### 4b. Latency and CPU budget

Measure:
- frame-to-encoder latency (from present to encode start)
- encoder CPU time per frame at 1920x1080@60
- drop rate

The known budget from mode 7 work: seat B at 1920x1080@60 with H.264 encoder
on this CPU runs at ~X% (check the existing measurements in MODE7 docs).
The kernel path should not add more than a few ms over the previous path.

### 4c. Gate

Run the existing seat B pipeline using the kernel-captured frames. Client
receives video, keyboard and mouse still work through the existing path.
The kernel miniport is now a viable pixel source.

## Step 5 - Input injection

Last step. A HID source driver or filter driver that accepts events from
the network listener and injects them into the session. This replaces the
`GetInputHandles` contract that the provider path could not satisfy.

### 5a. Choose the injection mechanism

Two paths:

- **HID source driver** (recommended): the miniport reports as a HID keyboard
  + mouse device. Input is injected via IOCTL. Works with termsrv-agnostic
  applications. This is the modern approach.
- **Filter driver** on the console HID stack: more invasive, more complex,
  and interacts badly with the physical console. Not recommended.

Start with the HID source driver. Microsoft has a sample:
`hid\hidusbfx2` in the WDK samples is USB-specific; for a virtual HID source
you want `hid\vhidmini` (Virtual HID Framework sample).

### 5b. Gate

Physical keyboard events on the network client appear as keypresses in the
remote session. Cursor movement on the client moves the remote cursor.

## Rollback (every step, every time)

Every kernel-driver load is a boot-affecting change. Rollback for each step:

- **Step 1-3 (display only):** `pnputil /delete-driver oemNN.inf /uninstall /force`,
  reboot. If the machine blue-screens during this, the display driver cannot
  be removed from a broken session. Boot to WinRE, use `dism` to remove the
  oemNN, or use `pnputil` from WinRE command prompt.
- **Step 4 (with network):** delete the oemNN first, then stop the network
  service. If network is up and the display driver is corrupt, you may lose
  the ability to reach the machine remotely. Always have physical access
  before loading Step 4's driver.
- **Step 5 (input injection):** remove the HID driver first, then the
  display driver. Input corruption is worse than display corruption: it can
  leave you unable to interact with the login screen. Test with physical
  keyboard and mouse available.

The safety-gate and the recovery stick (WinRE + D:) are non-negotiable for
every step. The failure mode this workflow exists to prevent is not "driver
doesn't work" - it's "driver broke the machine's ability to boot to the
login screen."

## What ends the day

- Step 1 succeeds: the toolchain works, the miniport loads. Good stopping
  point.
- Step 2 succeeds: kernel writes to disk. Good stopping point.
- Step 3 succeeds: kernel pixels reach user-mode. The core problem is solved.
  Next session is Step 4.
- Step 4 succeeds: kernel pixels reach the client. Mode 4's mission is
  replaced. Next session is Step 5.
- Step 5 succeeds: full kernel-mode remote display + input. The RDP
  replacement exists.
- Anything fails: roll back that step and stop. Do not iterate. Report what
  failed and what the gate showed. The gate exists precisely so the failure
  is a clean stopping point, not a debugging pit.

## What to bring back

For each step: the gate output, plus the log file for that step
(`C:\HydraLog\kmdod.log` for Step 2, `C:\HydraLog\frame-*.bin` for Step 3,
and any diagnostic output). Without these, the next session starts from
zero.

