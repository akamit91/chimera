#!/bin/bash
# Transport loss with no server failure: silently drop one client-visible
# address while a workload runs, then restore it.
# usage: run_blackhole.sh <tag> <addr to drop> <drop at s> <restore at s> <workload s>
set -u
cd "$(dirname "$0")"; . ./env.sh
tag=$1; addr=$2; t_on=$3; t_off=$4; secs=$5
./cli.sh cap-start $tag; ./cli.sh trace-start
./cli.sh sh "nohup python3 /tmp/fo-workload.py $MNT $secs $tag > /tmp/fo-$tag.out 2>&1 &"
sleep $t_on; ./cli.sh blackhole $addr on
sleep $((t_off - t_on)); ./cli.sh state | sed -n '/transports/,$p'; ./cli.sh blackhole $addr off
for i in $(seq 1 150); do out=$(./cli.sh sh "cat /tmp/fo-$tag.out"); echo "$out" | grep -q " done " && break; sleep 2; done
echo "--- client:"; echo "$out" | head -40
./cli.sh state | sed -n '/transports/,$p'
./cli.sh cap-stop $tag; ./cli.sh trace-stop $tag
