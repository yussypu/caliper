#!/usr/bin/env bash
# Builds the gavel bridge and produces a caliper feed file for one symbol from a
# real venue day. Runs the gavel matching engine over a gavel input stream and
# normalizes the published events into caliper book updates. See docs/feed.md.
#
# Prerequisites:
#   - a gavel checkout next to caliper (GAVEL_DIR), built or not
#   - a gavel input stream, e.g. gavel/data/01302020/AAPL.gvl from
#     gavel/scripts/get_data.sh, which streams a NASDAQ TotalView-ITCH day
set -euo pipefail

cd "$(dirname "$0")/.."

GAVEL_DIR="${GAVEL_DIR:-../gavel}"
SYMBOL="${SYMBOL:-AAPL}"
DAY="${DAY:-20200130}"
STREAM="${STREAM:-${GAVEL_DIR}/data/01302020/${SYMBOL}.gvl}"
OUT="${OUT:-data/${SYMBOL}.feed}"

if [[ ! -f "${STREAM}" ]]; then
  echo "gavel stream ${STREAM} not found." >&2
  echo "fetch it first: (cd ${GAVEL_DIR} && cmake -B build && cmake --build build -j && scripts/get_data.sh)" >&2
  exit 1
fi

cmake -S bridge -B bridge/build -DCMAKE_BUILD_TYPE=Release -DGAVEL_DIR="$(cd "${GAVEL_DIR}" && pwd)" >/dev/null
cmake --build bridge/build -j >/dev/null

mkdir -p "$(dirname "${OUT}")"
bridge/build/gavel_to_feed --out "${OUT}" --symbol "${SYMBOL}" --day "${DAY}" "${STREAM}"
