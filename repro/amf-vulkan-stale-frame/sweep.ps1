# Runs each configuration -Runs times and sums the verdicts.
#   .\sweep.ps1 -Exe build\Release\amf-vulkan-stale-frame.exe -Runs 10 -Configs "--producer same", "--producer same --reuse-surface"
param([string]$Exe = "build\Release\amf-vulkan-stale-frame.exe", [int]$Runs = 6, [string[]]$Configs)
$Exe = (Resolve-Path $Exe).Path
Push-Location (Split-Path $Exe)
foreach ($c in $Configs) {
	$last = ""; $fail = 0; $stale = 0; $back = 0; $err = 0; $other = 0
	for ($i = 0; $i -lt $Runs; $i++) {
		$out = Invoke-Expression "& '$Exe' $c --quiet" | Select-Object -Last 1
		if ($out -match '^(PASS|FAIL): (\d+) stale \(up to (\d+) frames back\), (\d+) garbled, (\d+) blank') {
			if ($Matches[1] -eq 'FAIL') { $fail++ }
			$stale += [int]$Matches[2]; $back = [Math]::Max($back, [int]$Matches[3]); $other += [int]$Matches[4] + [int]$Matches[5]
		} else { $err++; $last = $out }
	}
	"{0,-60} fail {1}/{2}  stale {3}  max-back {4}  garbled/blank {5}  errors {6} {7}" -f $c, $fail, $Runs, $stale, $back, $other, $err, $last
}
Pop-Location
