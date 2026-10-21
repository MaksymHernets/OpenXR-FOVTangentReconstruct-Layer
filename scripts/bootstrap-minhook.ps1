$ErrorActionPreference = 'Stop'
$hookRoot = Join-Path (Split-Path $PSScriptRoot -Parent) 'external/minhook'
$hookFiles = @{
    'include/MinHook.h' = '62DFF05E63155C6BF8135C36A0D79DEDBDCAAEDE9DA26FE38DAF0BEA34323E38'
    'src/buffer.c' = '0AB484D7870C3A8E7A3A1EE61E2070072CF445ED85B619D4C4207FED34356D99'
    'src/buffer.h' = '9769702BD625249E59A19EDE05C84365FCC7707E1FA0E5DF08DF42227CEE13E3'
    'src/hook.c' = 'EDCCE4C81985D7DD5B73FEAE96D6498C8F749B1BCA4C3D2D9926EA9A17A84755'
    'src/trampoline.c' = 'F13A169F803FF5A8E45C58CE668B4377A9878C0FC4452369B8A1325065186B2B'
    'src/trampoline.h' = '53ED9934492320617A27BFD952FFE7F5B7A4A1D09963D4512447082FE7B727B4'
    'src/hde/hde64.c' = '824EAFA315322E6DA105A7CE4EC1FC0E0ADEEE0106D32152FBB0B59E709BD15C'
    'src/hde/hde64.h' = 'E99AA4997BDA14B534C614C3D8CB78A72C4ACA91A1212C8B03EC605D1D75E36E'
    'src/hde/table64.h' = 'B774446D2F110CE954FB0A710F4693C5562DDBD8D56FE84106F2EE80DB8B50A2'
    'src/hde/pstdint.h' = '604D1CDF45C5A95BC9CF1BBF39CA05EF83013541101D34971C5CD7577E3D5247'
    'LICENSE.txt' = 'D5C2224982B0B95A16B098A561335FA93F3EAF4D0AA964B7843EDB986DF78DC8'
}
foreach ($name in $hookFiles.Keys) {
    $target = Join-Path $hookRoot $name
    New-Item -ItemType Directory -Force (Split-Path $target -Parent) | Out-Null
    if (!(Test-Path -LiteralPath $target)) {
        Invoke-WebRequest ("https://raw.githubusercontent.com/TsudaKageyu/minhook/c3fcafdc10146beb5919319d0683e44e3c30d537/" + $name) -OutFile $target
    }
    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $hookFiles[$name]) { throw "MinHook checksum mismatch: $name" }
}
