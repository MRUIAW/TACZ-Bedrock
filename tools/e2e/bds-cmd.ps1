#requires -Version 7
# Sends one command to an already-running Start-Server.ps1 control pipe.
param([Parameter(Mandatory)][string]$Command, [string]$PipeName = 'TaczBds-Control')
$p = [IO.Pipes.NamedPipeClientStream]::new('.', $PipeName, [IO.Pipes.PipeDirection]::InOut, [IO.Pipes.PipeOptions]::CurrentUserOnly)
try {
    $p.Connect(3000)
    $w = [IO.StreamWriter]::new($p); $w.AutoFlush = $true
    $r = [IO.StreamReader]::new($p)
    $w.WriteLine($Command)
    "pipe: " + $r.ReadLine()
} finally { $p.Dispose() }
