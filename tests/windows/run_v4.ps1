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
    if (-not (Test-Path $exp)) { $skip++; return }
    $head = Get-Content $src -TotalCount 10 | Out-String
    if ($head -match '@(compile-only|expect-panic|expected-pass-after|posix-only)') { $skip++; return }
    $name = $_.BaseName
    $exe = Join-Path $work ($name + ".exe")
    $build = & $Ironc build -o $exe $src 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exe)) {
        $fail++; Write-Output ("[FAIL] " + $name + " (build)"); Write-Output (($build -split "`n" | Where-Object { $_ -match 'error' } | Select-Object -First 3) -join "`n"); return
    }
    $out = ([Text.Encoding]::UTF8.GetString([Text.Encoding]::Default.GetBytes((& $exe 2>&1 | Out-String)))) -replace "`r`n", "`n"
    $want = (([IO.File]::ReadAllText($exp, [Text.Encoding]::UTF8)) -replace "`r`n", "`n")
    if ($out.TrimEnd() -eq $want.TrimEnd()) { $pass++ } else {
        $fail++; Write-Output ("[FAIL] " + $name + " (output)")
        Write-Output ("  want: " + (($want.TrimEnd() -split "`n" | Select-Object -First 2) -join " | "))
        Write-Output ("  got:  " + (($out.TrimEnd() -split "`n" | Select-Object -First 2) -join " | "))
    }
}
Write-Output ("PASS=" + $pass + " FAIL=" + $fail + " SKIPPED=" + $skip)
