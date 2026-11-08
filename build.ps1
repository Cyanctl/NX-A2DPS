param([string]$DevkitPro = 'C:/msys64/opt/devkitpro')
$ErrorActionPreference = 'Stop'
$env:DEVKITPRO = $DevkitPro
Set-Location $PSScriptRoot
$gcc = Join-Path $DevkitPro 'devkitA64/bin/aarch64-none-elf-gcc.exe'
if (!(Test-Path -LiteralPath $gcc)) { throw "devkitA64 not found at $DevkitPro" }
New-Item -ItemType Directory -Force build,dist | Out-Null
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed ($LASTEXITCODE)" }
}
$arch = @('-march=armv8-a+crc+crypto','-mtune=cortex-a57','-mtp=soft','-fPIE')
Invoke-Checked $gcc @('-Os','-march=armv8-a','-mgeneral-regs-only','-ffreestanding',
    '-fno-builtin','-fno-stack-protector','-fno-pic','-fno-pie','-fno-asynchronous-unwind-tables',
    '-Wall','-Wextra','-Werror','-c','firmware/bridge.c','-o','build/bridge.o')
Invoke-Checked $gcc @('-c','firmware/bridge_entry.S','-o','build/bridge_entry.o')
Invoke-Checked $gcc @('-nostdlib','-Wl,-T,firmware/bridge.ld','build/bridge_entry.o','build/bridge.o','-o','build/bridge.elf')
Invoke-Checked 'python' @('tools/embed_bridge.py',$DevkitPro)
$common = $arch + @('-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror',
    '-ffunction-sections','-fdata-sections','-D__SWITCH__',"-I$DevkitPro/libnx/include",'-Ithird_party/libsbc','-Ibuild','-Ifirmware')
$objects = @()
foreach ($source in @('source/main.c','source/sbc_receiver.c','source/media_selftest.c','source/native_bridge.c','source/native_sink.c','source/audio_pipeline.c')) {
    $object = 'build/' + [IO.Path]::GetFileNameWithoutExtension($source) + '.o'
    Invoke-Checked $gcc ($common + @('-c',$source,'-o',$object))
    $objects += $object
}
foreach ($source in @('sbc.c','sbc_primitives.c')) {
    $object = 'build/' + [IO.Path]::GetFileNameWithoutExtension($source) + '.o'
    Invoke-Checked $gcc ($arch + @('-O2','-g','-ffunction-sections','-fdata-sections','-U__ARM_NEON__',
        '-D__BYTE_ORDER=__BYTE_ORDER__','-D__LITTLE_ENDIAN=__ORDER_LITTLE_ENDIAN__','-D__BIG_ENDIAN=__ORDER_BIG_ENDIAN__',
        '-c',"third_party/libsbc/$source",'-o',$object))
    $objects += $object
}
Invoke-Checked $gcc ($arch + @("-specs=$DevkitPro/libnx/switch.specs",
    '-g','-Wl,-Map,build/switch-audio-probe.map') + $objects + @(
    "-L$DevkitPro/libnx/lib",'-lnx','-lm','-o','build/switch-audio-probe.elf'))
Invoke-Checked "$DevkitPro/tools/bin/nacptool.exe" @('--create','Switch Audio Sink v11','Cyan','0.11.0','build/switch-audio-probe.nacp')
Invoke-Checked "$DevkitPro/tools/bin/elf2nro.exe" @('build/switch-audio-probe.elf','dist/switch-audio-sink-v11.nro',
    '--nacp=build/switch-audio-probe.nacp',"--icon=$DevkitPro/libnx/default_icon.jpg")
Get-FileHash dist/switch-audio-sink-v11.nro -Algorithm SHA256
