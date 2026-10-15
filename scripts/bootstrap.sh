#!/bin/sh
# Everything from a disc image to a running game, in one command.
#
#   ./scripts/bootstrap.sh <image.iso> [game directory]
#
# The image is checked against the hashes of the two retail releases this
# project knows - European and American - before anything else: an image that
# differs is not necessarily broken, but the function boundaries in
# functions.toml were found in these, and a different build will not line up
# with them. The two lay their code out the same way and differ in the
# territory the title was built for, so the code is generated from whichever
# default.xex the image carries.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
image=$1
game=${2:-$root/game}

# Burnout Revenge (Europe) and Burnout Revenge (USA).
RETAIL_SHA256_EU=34c1bd4d549c2c53f29d814fa5e5d1c04c533c5ca0c39e57b6c2538f44ff59b4
RETAIL_SHA256_US=7abf689f52b70666ba92052bd157cfcae9ed6290dae298fbf639baa1d169f92b

if [ -z "$image" ]; then
    echo "give the path to a disc image" >&2
    echo "  $0 <image.iso> [game directory]" >&2
    exit 1
fi

echo "== checking the environment"
"$root/scripts/check-prerequisites.sh"

echo
echo "== setup tools"
"$root/scripts/build-tools.sh"

echo
echo "== extracting the image into $game"
if [ -f "$game/default.xex" ]; then
    echo "already extracted, skipping"
else
    "$root/scripts/extract-image" --sha256 "$RETAIL_SHA256_EU" \
        --sha256 "$RETAIL_SHA256_US" "$image" "$game"
fi

echo
echo "== SDK"
"$root/scripts/setup-sdk.sh"

echo
echo "== plume and the shader translator"
"$root/scripts/setup-deps.sh"

echo
echo "== building the title"
"$root/scripts/build.sh" --game "$game"

echo
echo "All set. To run:"
echo "  cd $root/rexglue && ./run.sh"
