# run_v4.ps1 <ironc.exe> <corpus dir>: build + run every fixture with an .expected sibling.
param([string]$Ironc, [string]$Corpus)
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$OutputEncoding = [Text.Encoding]::UTF8
$pass = 0; $fail = 0; $skip = 0
$work = Join-Path $env:TEMP "iron-v4"
New-Item -ItemType Directory -Force -Path $work | Out-Null
Get-ChildItem -Path $Corpus -Recurse -Filter *.iron | Sort-Object FullName | ForEach-Object {
    $src = $_.FullName
    $exp = [IO.Path]::ChangeExtension($src, ".expected")
    $head = Get-Content $src -TotalCount 10 | Out-String
    # The same rules as tests/run_tests.sh: @posix-only fixtures are skipped,
    # @compile-only ones are built and not run, @expect-panic ones must exit
    # non-zero with the substring on stderr, and @expected-pass-after
    # fixtures run like any other (every parked phase is in the past).
    if ($head -match '@posix-only') { $skip++; return }
    $compileOnly = $head -match '@compile-only'
    $panic = $null
    if ($head -match '@expect-panic:\s*(.+)') { $panic = $Matches[1].Trim() }
    if (-not $compileOnly -and -not $panic -and -not (Test-Path $exp)) { $skip++; return }
    $name = $_.BaseName
    $exe = Join-Path $work ($name + ".exe")
    $build = & $Ironc build -o $exe $src 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exe)) {
        $fail++; Write-Output ("[FAIL] " + $name + " (build)"); Write-Output (($build -split "`n" | Where-Object { $_ -match 'error' } | Select-Object -First 3) -join "`n"); return
    }
    if ($compileOnly) { $pass++; return }
    # Capture through a file: the console pipeline would re-decode UTF-8 as ANSI.
    $outFile = Join-Path $work ($name + ".out")
    $errFile = Join-Path $work ($name + ".err")
    $p = Start-Process -FilePath $exe -RedirectStandardOutput $outFile -RedirectStandardError $errFile -NoNewWindow -Wait -PassThru
    $stdout = [IO.File]::ReadAllText($outFile, [Text.Encoding]::UTF8) -replace "`r", ""
    $stderr = [IO.File]::ReadAllText($errFile, [Text.Encoding]::UTF8) -replace "`r", ""
    if ($panic) {
        if ($p.ExitCode -ne 0 -and $stderr.Contains($panic)) { $pass++ } else {
            $fail++; Write-Output ("[FAIL] " + $name + " (@expect-panic: exit " + $p.ExitCode + ", stderr missing '" + $panic + "')")
            Write-Output ("  got:  " + (($stderr.TrimEnd() -split "`n" | Select-Object -First 2) -join " | "))
        }
        return
    }
    $out = $stdout + $stderr
    $want = (([IO.File]::ReadAllText($exp, [Text.Encoding]::UTF8)) -replace "`r", "")
    if ($out.TrimEnd() -eq $want.TrimEnd()) { $pass++ } else {
        $fail++; Write-Output ("[FAIL] " + $name + " (output)")
        Write-Output ("  want: " + (($want.TrimEnd() -split "`n" | Select-Object -First 2) -join " | "))
        Write-Output ("  got:  " + (($out.TrimEnd() -split "`n" | Select-Object -First 2) -join " | "))
    }
}
Write-Output ("PASS=" + $pass + " FAIL=" + $fail + " SKIPPED=" + $skip)
if ($fail -gt 0) { exit 1 }
