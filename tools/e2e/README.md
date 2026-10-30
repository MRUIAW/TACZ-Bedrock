# Manual e2e helpers (Windows / PowerShell 7)

Copied from the 0.4.10 acceptance helpers; the original Temp copies are unchanged.
These scripts do not start, deploy or stop Minecraft/BDS. Start your authorized
test instance and server separately. The client needs LeviHeadlessInput and its
`HeadlessInput.psm1`; console reading needs a visible BDS console.

Dot-source the helpers, supplying all machine paths explicitly:

```powershell
. ./tools/e2e/e2e.ps1 -InstanceRoot 'D:\instances\test-hi2' `
    -BdsRoot 'D:\servers\test' -ShotsDir 'D:\test-output\shots'
$mark = Mark; $serverMark = SrvMark
Give 'tacz:spas_12'; Fire; Shot 'after-shot'
Since $mark; SrvSince $serverMark
SrvConsole -after (Now)
```

`-HeadlessPipe` overrides the original `LeviHeadlessInput-hiN` suffix convention;
`-ServerProcessId` overrides discovery under the supplied BDS root. `Resume`
retains the original fixed menu coordinate (377,345); adapt your window layout.
`Give` **clears the test player's inventory** before giving the requested gun.
`Chat`/`Fire` send real input; `Shot` writes PNGs. Do not use on a valuable world.

Standalone console/control-pipe usage:

```powershell
pwsh -NoProfile -File ./tools/e2e/read-console.ps1 -ProcessId 1234 -Lines 100
pwsh -NoProfile -File ./tools/e2e/bds-cmd.ps1 -Command 'list'
```

The command helper requires an existing `Start-Server.ps1` compatible named-pipe
server (not provided here). `-PipeName` defaults to `TaczBds-Control`. No machine
paths have user-specific defaults. Console reading runs in a separate `pwsh`
process because attaching/detaching changes that process's console association.
