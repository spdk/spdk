# Get the ID and security principal of the current user account
$myWindowsID=[System.Security.Principal.WindowsIdentity]::GetCurrent()
$myWindowsPrincipal=new-object System.Security.Principal.WindowsPrincipal($myWindowsID)

# Get the security principal for the Administrator role
$adminRole=[System.Security.Principal.WindowsBuiltInRole]::Administrator

# Check to see if we are currently running "as Administrator"
if ($myWindowsPrincipal.IsInRole($adminRole))
   {
   # We are running "as Administrator" - so change the title and background color to indicate this
   $Host.UI.RawUI.WindowTitle = $myInvocation.MyCommand.Definition + "(Elevated)"
   $Host.UI.RawUI.BackgroundColor = "DarkBlue"
   clear-host
   }
else
   {
   # We are not running "as Administrator" - so relaunch as administrator

   # Create a new process object that starts PowerShell
   $newProcess = new-object System.Diagnostics.ProcessStartInfo "PowerShell";

   # Specify the current script path and name as a parameter
   $newProcess.Arguments = $myInvocation.MyCommand.Definition;

   # Indicate that the process should be elevated
   $newProcess.Verb = "runas";

   # Start the new process
   [System.Diagnostics.Process]::Start($newProcess);

   # Exit from the current, unelevated, process
   exit
   }
# Run your code that needs to be elevated here
get-disk | Where-Object FriendlyName -NotMatch "QEMU" | Initialize-Disk -PartitionStyle MBR
Start-Sleep 2
get-disk | Where-Object FriendlyName -NotMatch "QEMU" | Clear-Disk -RemoveData -Confirm:$false
Start-Sleep 2
get-disk | Where-Object FriendlyName -NotMatch "QEMU" | Initialize-Disk -PartitionStyle MBR
Start-Sleep 2

$disks = get-disk | Where-Object FriendlyName -NotMatch "QEMU"
Start-Sleep 2
foreach($disk in $disks)
{

    $phy_bs = $disk.PhysicalSectorSize
    $model = $disk.model
    $serial = $disk.SerialNumber

    $label = ""
    $label += $model.Trim() + "_" + $serial + "_" + $phy_bs
    $label = $label -replace " ", "_"
    echo $label
    start-sleep 2

    $part = New-Partition -DiskNumber $disk.Number -UseMaximumSize -AssignDriveLetter
    echo $part.DriveLetter
    start-sleep 2

    $vol = Format-Volume -DriveLetter $part.DriveLetter -FileSystem NTFS -Confirm:$false
    echo $vol
    start-sleep 2

    cd C:\SCSI
    .\scsicompliancetest.exe \\.\$($vol.DriveLetter): -full | tee "C:\SCSI\WIN_SCSI_1_$label.log"
    start-sleep 2
    mv .\scsicompliance.log.wtl ".\WIN_SCSI_1_$label.wtl"
    .\scsicompliance.exe /Device \\.\$($vol.DriveLetter): /Operation Test /Scenario Common | tee "C:\SCSI\WIN_SCSI_2_$label.log"
    start-sleep 2
    mv .\scsicompliance.wtl ".\WIN_SCSI_2_$label.wtl"
}
