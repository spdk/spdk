# Get the ID and security principal of the current user account
$myWindowsID=[System.Security.Principal.WindowsIdentity]::GetCurrent()
$myWindowsPrincipal=new-object System.Security.Principal.WindowsPrincipal($myWindowsID)

# Get the security principal for the Administrator role
$adminRole=[System.Security.Principal.WindowsBuiltInRole]::Administrator

# Check to see if we are currently running "as Administrator"
if ($myWindowsPrincipal.IsInRole($adminRole)) {
	# We are running "as Administrator" - so change the title and background color to indicate this
	$Host.UI.RawUI.WindowTitle = $myInvocation.MyCommand.Definition + "(Elevated)"
	$Host.UI.RawUI.BackgroundColor = "DarkBlue"
	clear-host
} else
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

# Set bash -e equivalent
$ErrorActionPreference = "Stop"

$filesystems=@("NTFS", "FAT32", "FAT")
$disks = get-disk | Where-Object FriendlyName -NotMatch "QEMU"
Start-Sleep 2
foreach($disk in $disks)
{
	$size = $disk.Size
	$number = $disk.Number
	$serial = $disk.SerialNumber
	$model = $disk.model.Trim()
	$size = $size -replace " ", "_"
	$model = $model -replace " ", "_"

	$label = "${number}_${model}_${serial}_${size}"
	echo "Running tests for disk $label"
	start-sleep 2

	Try {
		Initialize-Disk -Number $disk.Number -PartitionStyle MBR
	} Catch {
		Clear-Disk -Number $disk.Number -RemoveData -Confirm:$false
		Initialize-Disk -Number $disk.Number -PartitionStyle MBR
	}
	echo "`tDisk initialized"
	start-sleep 2

	$part = New-Partition -DiskNumber $disk.Number -UseMaximumSize -AssignDriveLetter
	echo "`tCreated partition $($part.DriveLetter)"
	start-sleep 2

	foreach($fs in $filesystems) {
		echo "`tTrying to format $($part.DriveLetter) with $fs"
		Try {
			$vol = Format-Volume -DriveLetter $part.DriveLetter -FileSystem $fs -Confirm:$false
		} Catch [Exception] {
			echo $_.Exception.GetType().FullName, $_.Exception.Message
			echo $_.Exception | format-list -force
			exit 1
		}
		echo "`tPartition $($part.DriveLetter) formatted with $fs filesystem"
		start-sleep 2
	}
}
