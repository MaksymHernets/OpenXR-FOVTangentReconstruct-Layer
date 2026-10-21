# Official MIT-licensed NVAPI headers only; no vendor redistributable or import library.
$nvapiRoot = Join-Path $PWD 'external/nvapi'
New-Item -ItemType Directory -Force $nvapiRoot | Out-Null
$nvapiFiles = @{
    'License.txt' = '84726B60C17FE0F71F92A6C074C3D4087370886EBA5D21CCC4EC55DB37D6C8BA'
    'nvapi.h' = '76BBB71107C1388134D41712BED56BB6D80EC426F9BB2C168E2D6941B90F16CA'
    'nvapi_interface.h' = 'D279F81E7F92F2743BFEA2EC7ADA693001805E13645EB1C6F2AF44EF9C342834'
    'nvapi_lite_common.h' = '3904BAAB8E8501A53746DDF4E5B13A2E1D5ACD6EE50F8C3C2EADD8E4D62C55F2'
    'nvapi_lite_d3dext.h' = '2322DB28A2F7384FF6DF6FD1110B48E99660FA29956A2CBCD28A9D7157017E1A'
    'nvapi_lite_salend.h' = '92017F8E26F585601F4EAAC475FA9E542EAA2F99A80E376DDE52F2F73AB18E67'
    'nvapi_lite_salstart.h' = '9DB770E52947B33D68B0C1F17BC21EDD9D736B824CCECEE2C5C527D48A89BE93'
    'nvapi_lite_sli.h' = '2A9C75FAECB32BC7A5237A4B83CC28A6A488C75493E44BF28688ABF3FEC01A34'
    'nvapi_lite_stereo.h' = 'BD6FE69136CFF36C73A9967A7CEF37186791D9B7C3EB3AE7AC61B4EFA628325B'
    'nvapi_lite_surround.h' = '197AD1E4762B2243904B0A13418747436AE70E4824099BF56BE4804C4C29956F'
}
foreach ($name in $nvapiFiles.Keys) {
    $target = Join-Path $nvapiRoot $name
    if (!(Test-Path -LiteralPath $target)) {
        Invoke-WebRequest ("https://raw.githubusercontent.com/NVIDIA/nvapi/70d337db9186e968eab622f7e786de7e437faf3d/" + $name) -OutFile $target
    }
    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $nvapiFiles[$name]) {
        throw "NVAPI header checksum mismatch: $name"
    }
}
