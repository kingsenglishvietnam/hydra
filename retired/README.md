# Retired

Approaches that were built, tested on hardware, and rejected. Kept so the
reasoning survives and nobody re-attempts them.

- **install-mirror-task.ps1** — mirror as a logon scheduled task. Starts before
  the capture agent is publishing and ends up stuck (~2 MB working set, CPU
  climbing, nothing on the panel).
- **install-mirror-startup.ps1** — mirror from the Startup folder. Same failure,
  same cause.

mirror is started by `hydra-start.ps1` instead, *after* `capture:B` is confirmed
running. Order is the whole problem; launch context was a red herring.

Also retired, still in the tree because the build scripts reference them:

- **build-driver.ps1 / sign-driver.ps1 / iddseat** — the IDD virtual monitor.
  Superseded by Desktop Duplication. The remote-session IDD variant was tested
  and cannot work under RDP-Wrapper (`CM_PROB_REINSTALL`; the RDP stack never
  enumerates a matching device).
- **build-overlay.ps1 / cursor_overlay** — the cursor is now composited into the
  captured frame, so it cannot be covered by the Start menu. The overlay always
  could be.
