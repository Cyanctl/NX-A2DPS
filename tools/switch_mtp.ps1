param(
    [ValidateSet('List','Upload','Download')][string]$Action = 'List',
    [string]$RemoteFolder = 'switch',
    [string]$LocalFile,
    [string]$RemoteFile
)
$ErrorActionPreference = 'Stop'
$shell = New-Object -ComObject Shell.Application
$devices = @($shell.Namespace(17).Items() | Where-Object Name -eq 'Nintendo Switch')
if ($devices.Count -ne 1) { throw 'Open USB File Transfer on exactly one connected Switch.' }
$storage = @($devices[0].GetFolder.Items() | Where-Object Name -eq 'SD Card')
if ($storage.Count -ne 1) { throw 'SD Card storage not found.' }
$folder = $storage[0].GetFolder
foreach ($part in ($RemoteFolder -split '[/\\]' | Where-Object { $_ })) {
    if ($part -in @('.','..')) { throw 'Relative path segments are not supported.' }
    $entry = @($folder.Items() | Where-Object { $_.Name -eq $part -and $_.IsFolder })
    if ($entry.Count -ne 1) { throw "Remote folder not found: $part" }
    $folder = $entry[0].GetFolder
}
if ($Action -eq 'List') {
    $folder.Items() | Select-Object Name,@{Name='Bytes';Expression={$_.ExtendedProperty('System.Size')}},IsFolder
    exit
}
if (!$LocalFile) { throw 'LocalFile is required.' }
if ($Action -eq 'Upload') {
    $source = Get-Item -LiteralPath $LocalFile
    if ($source.PSIsContainer) { throw 'Upload expects one file.' }
    if (@($folder.Items() | Where-Object Name -eq $source.Name).Count) {
        throw 'Destination already exists. Preserve it or remove it explicitly before uploading.'
    }
    $folder.CopyHere($source.FullName, 4)
    $deadline = (Get-Date).AddSeconds(30)
    do {
        Start-Sleep -Milliseconds 250
        $target = @($folder.Items() | Where-Object Name -eq $source.Name)
        if ($target.Count -eq 1 -and $target[0].ExtendedProperty('System.Size') -eq $source.Length) {
            Write-Output "Transferred $($source.Name): $($source.Length) bytes"
            exit
        }
    } while ((Get-Date) -lt $deadline)
    throw 'Transfer could not be verified within 30 seconds.'
}
if (!$RemoteFile) { throw 'RemoteFile is required for download.' }
$source = @($folder.Items() | Where-Object { $_.Name -eq $RemoteFile -and !$_.IsFolder })
if ($source.Count -ne 1) { throw 'Remote file not found.' }
$destination = [IO.Path]::GetFullPath($LocalFile)
if (Test-Path -LiteralPath $destination) { throw 'Local destination already exists.' }
New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($destination)) | Out-Null
# MTP CopyHere preserves names, so first copy to a new staging directory.
$stage = Join-Path ([IO.Path]::GetDirectoryName($destination)) ([Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
$shell.Namespace($stage).CopyHere($source[0], 4)
$copied = Join-Path $stage $RemoteFile
$deadline = (Get-Date).AddSeconds(30)
do {
    Start-Sleep -Milliseconds 250
    if ((Test-Path -LiteralPath $copied) -and (Get-Item -LiteralPath $copied).Length -eq $source[0].ExtendedProperty('System.Size')) {
        Move-Item -LiteralPath $copied -Destination $destination
        Remove-Item -LiteralPath $stage
        Write-Output $destination
        exit
    }
} while ((Get-Date) -lt $deadline)
throw "Download not verified; staging files preserved in $stage"
