# HTTP test suite

This directory contains the HTTP/1.1 conformance and randomized differential
tests used to baseline the C implementation before its planned pure Iron
rewrite.

`vendor/picohttpparser` is the pinned MIT licensed reference parser used by
the differential driver and benchmark. Its exact source and license are kept
beside the vendored files so the tests remain reproducible without network
access.

`differential/difffuzz.py` compares canonical parser results. Its default is
a longer 10000-message run; ctest runs a fixed 1500-message seed. A saved hex
batch can be replayed with `--batch`, and mismatches are preserved under
`http-difffuzz-out` as raw HTTP messages plus both results.
