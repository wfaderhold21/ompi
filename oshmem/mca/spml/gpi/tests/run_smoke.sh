#!/bin/bash
# spml/gpi 2-rank smoke runner (S1 item 2, Rule-4 runtime acceptance).
# Run on the Linux venue AFTER smoke/build_on_venue.sh reports DONE rc=0.
#
#   bash ~/gpi-smoke/smoke/run_smoke.sh
#
# Prints the spml selection line (must show gpi, priority 5) then the TAP-ish
# check lines from spml_gpi_smoke. Any "FAIL:" line means a real defect.
set -u
ROOT=$HOME/gpi-smoke
BIN=$ROOT/bin
mkdir -p $BIN

echo "=== build the smoke binary ==="
if [ -x $ROOT/install/bin/oshcc ]; then
  $ROOT/install/bin/oshcc -O0 -g -Wall -o $BIN/spml_gpi_smoke \
      $ROOT/smoke/spml_gpi_smoke.c 2>&1 | tail -20
else
  echo "oshcc missing; falling back to gcc -lshmem"
  gcc -O0 -g -Wall -I$ROOT/install/include -o $BIN/spml_gpi_smoke \
      $ROOT/smoke/spml_gpi_smoke.c -L$ROOT/install/lib -lshmem 2>&1 | tail -20
fi
[ -x $BIN/spml_gpi_smoke ] || { echo "SMOKE_BUILD=FAIL"; exit 1; }
echo "SMOKE_BUILD=OK"

echo
echo "=== spml component inventory (is gpi present?) ==="
ls -l $ROOT/install/lib/openmpi/ 2>/dev/null | grep -i spml || \
  ls -l $ROOT/install/lib/openmpi/mca_spml* 2>/dev/null

echo
echo "=== run: oshrun -np 2 --mca spml gpi ==="
timeout --signal=KILL 240 \
$ROOT/install/bin/oshrun -np 2 \
    --mca spml gpi \
    --mca spml_base_verbose 20 \
    --mca mca_base_component_show_load_errors 1 \
    --mca btl self,sm \
    --mca btl_base_verbose 0 \
    $BIN/spml_gpi_smoke 2>&1
rc=$?
echo "=== smoke rc=$rc (124/137 = timed out) ==="
exit $rc
