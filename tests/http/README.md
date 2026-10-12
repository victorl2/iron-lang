# HTTP test suite

This directory contains the HTTP/1.1 conformance and randomized differential
tests used to baseline the C implementation before its planned pure Iron
rewrite.

`vendor/picohttpparser` is the pinned MIT licensed reference parser used by
the differential driver and benchmark. Its exact source and license are kept
beside the vendored files so the tests remain reproducible without network
access.
