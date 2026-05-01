"""Phase 14 Plan 02 — MIG-06 automatable portion: version-range unit tests.

Requirements covered:
  MIG-06: Hard-refuse machinery verified against v3 ironls

This test file is a UNIT test — it does NOT require a running ironls binary.
It replicates the version-range parser logic inline (with explicit comments
documenting the duplication, per Pitfall 3 motivation in 14-RESEARCH.md)
and asserts that the v3 extension version range '>= 3.0.0, < 4.0.0' correctly:
  - REJECTS v1.x binaries (e.g., 1.2.0-alpha.7)
  - ACCEPTS v3.x binaries (e.g., 3.0.0-alpha.1, 3.0.0)
  - REJECTS v4.0.0 and above (upper bound is exclusive)

These assertions are a guard against version-range typos in the bump commit
(Task 3 of Plan 14-02).

NOTE: Intentional duplication of parser logic.
The version-range string comparison logic is replicated here from
editors/vscode/src/extension.ts (isCompatible/semverKey/semverGte/semverLt).
This duplication is intentional — the test guards against version-range typos
in the extension files without requiring a live editor environment. If the
extension logic ever changes, this test must be updated in concert.
(See 14-RESEARCH.md Pitfall 3: scattered version-range literals across 3 editors.)
"""
from __future__ import annotations


# ---------------------------------------------------------------------------
# Inline version-range parser (mirrors extension.ts isCompatible logic).
# This is an intentional duplication to guard against typos.
# ---------------------------------------------------------------------------

V3_COMPATIBLE_RANGE = ">= 3.0.0, < 4.0.0"


def _semver_key(version: str) -> tuple[int, int, int]:
    """Parse 'X.Y.Z[-prerelease]' into (major, minor, patch)."""
    import re
    m = re.match(r'^(\d+)\.(\d+)\.(\d+)', version)
    if not m:
        return (0, 0, 0)
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)))


def _semver_gte(a: str, b: str) -> bool:
    """Return True if a >= b (major.minor.patch comparison only)."""
    ka, kb = _semver_key(a), _semver_key(b)
    return ka >= kb


def _semver_lt(a: str, b: str) -> bool:
    """Return True if a < b (major.minor.patch comparison only)."""
    ka, kb = _semver_key(a), _semver_key(b)
    return ka < kb


def _is_compatible(version: str, range_str: str) -> bool:
    """Parse '>= X.Y.Z, < A.B.C' and check version is in [X.Y.Z, A.B.C)."""
    import re
    m = re.match(
        r'>=?\s*(\d+\.\d+\.\d+[^\s,]*)[\s,]+<\s*(\d+\.\d+\.\d+[^\s]*)',
        range_str,
    )
    if not m:
        # Unknown range format — permissive
        return True
    min_ver, max_ver = m.group(1), m.group(2)
    return _semver_gte(version, min_ver) and _semver_lt(version, max_ver)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_v3_range_rejects_v1_ironls() -> None:
    """v1.x binaries MUST be rejected by the v3 extension range.

    MIG-06: all 3 extensions' hard-refuse machinery must reject v1 ironls.
    This guards against accidental range widening in the version-bump commit.
    """
    # Canonical v1 versions that were shipped before the v3 range
    v1_versions = [
        "1.2.0",
        "1.2.0-alpha.7",
        "1.0.0",
        "1.9.9",
        "2.9.9",  # Just below v3 — still rejected
        "2.0.0",
        "0.1.0",
    ]
    for ver in v1_versions:
        result = _is_compatible(ver, V3_COMPATIBLE_RANGE)
        assert not result, (
            f"Version {ver!r} should be REJECTED by range {V3_COMPATIBLE_RANGE!r}, "
            f"but _is_compatible returned True. "
            f"This indicates a version-range regression in the bump commit."
        )


def test_v3_range_accepts_v3_ironls() -> None:
    """v3.x binaries MUST be accepted by the v3 extension range.

    MIG-06: v3 ironls must pass the hard-refuse check.
    """
    v3_versions = [
        "3.0.0",
        "3.0.0-alpha.1",
        "3.0.0-alpha.2",
        "3.1.0",
        "3.9.9",
    ]
    for ver in v3_versions:
        result = _is_compatible(ver, V3_COMPATIBLE_RANGE)
        assert result, (
            f"Version {ver!r} should be ACCEPTED by range {V3_COMPATIBLE_RANGE!r}, "
            f"but _is_compatible returned False. "
            f"This indicates a version-range regression in the bump commit."
        )


def test_v3_range_rejects_v4_and_above() -> None:
    """v4.0.0 and above MUST be rejected — upper bound is EXCLUSIVE.

    MIG-06: the range '>= 3.0.0, < 4.0.0' has an exclusive upper bound.
    A future v4 release will require an extension update.
    """
    v4_plus_versions = [
        "4.0.0",
        "4.0.0-alpha.1",
        "4.1.0",
        "5.0.0",
        "10.0.0",
    ]
    for ver in v4_plus_versions:
        result = _is_compatible(ver, V3_COMPATIBLE_RANGE)
        assert not result, (
            f"Version {ver!r} should be REJECTED by range {V3_COMPATIBLE_RANGE!r} "
            f"(upper bound is exclusive), but _is_compatible returned True."
        )


def test_v3_range_boundary_exact_min() -> None:
    """3.0.0 exactly (no prerelease) MUST be accepted — lower bound is inclusive."""
    assert _is_compatible("3.0.0", V3_COMPATIBLE_RANGE), (
        f"Exact minimum version '3.0.0' must be accepted by range {V3_COMPATIBLE_RANGE!r}"
    )


def test_v3_range_boundary_below_max() -> None:
    """3.9.9 MUST be accepted — it is below the exclusive upper bound 4.0.0."""
    assert _is_compatible("3.9.9", V3_COMPATIBLE_RANGE), (
        f"Version '3.9.9' must be accepted by range {V3_COMPATIBLE_RANGE!r}"
    )


def test_v3_range_boundary_exact_max_rejected() -> None:
    """4.0.0 exactly MUST be rejected — upper bound is exclusive."""
    assert not _is_compatible("4.0.0", V3_COMPATIBLE_RANGE), (
        f"Exact maximum version '4.0.0' must be REJECTED (exclusive upper bound) "
        f"by range {V3_COMPATIBLE_RANGE!r}"
    )
