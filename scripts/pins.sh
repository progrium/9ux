# Pinned versions. Bump deliberately, in their own commit, with CI green.

# h9 (github.com/progrium/hyper9) is under active development, so 9/UX
# pins a known-good commit. scripts/h9.sh builds it into .local/hyper9.
# To use your own checkout instead, set H9=/path/to/zig-out/bin/h9.
H9_REPO=https://github.com/progrium/hyper9
H9_REV=b27cacd71a7ea3d0770099fc8fbad63739042edf

# 9front release: must match h9's NINEFRONT_RELEASE and rio/UPSTREAM.
NINEFRONT_RELEASE=11952
