# Pinned versions. Bump deliberately, in their own commit, with CI green.

# h9 (github.com/progrium/hyper9) is under active development, so 9/UX
# pins a known-good commit. scripts/h9.sh builds it into .local/hyper9.
# To use your own checkout instead, set H9=/path/to/zig-out/bin/h9.
H9_REPO=https://github.com/progrium/hyper9
H9_REV=b27cacd71a7ea3d0770099fc8fbad63739042edf

# 9front release: must match h9's NINEFRONT_RELEASE and rio/UPSTREAM.
NINEFRONT_RELEASE=11952

# Where 9front's release images are.
NINEFRONT_ISO_URL=https://9front.org/iso

# The release's amd64 ISO, unpacked into the root tree on Linux x86_64
# without KVM (CI). The hash is of the ISO after gunzip.
NINEFRONT_AMD64_ISO_SHA256=ee90b5e8a8d6b4ee8ad950322f91e338f698dae18c7f5e5800a650bbfa173fd9

# Zig, for building h9 and the test harness: h9 at H9_REV needs this
# release. scripts/zig.sh fetches it into .local/zig if it isn't on PATH.
ZIG_VERSION=0.16.0
