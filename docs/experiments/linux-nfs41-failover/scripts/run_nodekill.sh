#!/bin/bash
# Kill node 1 under load and see whether the client carries on by itself.
# Node 2 is told at once that node 1 is dead, so the server side is ready.
#   single   one mount of node 1's address, nothing else
#   trunk    node 1 mounted at two addresses (session trunking); at the kill
#            the second address starts answering from node 2.  This stands in
#            for a cluster where node 2 was in the trunk all along.
#   dns      mount by name; the name lists node 1 then node 2; after the kill
#            the name lists node 2 only
#   restart  as single, but node 1 is started again at <fix_s>
# At <fix_s> (except restart) node 1's own address is pointed at node 2, which
# is what a moved address would do, so the run can end.
# usage: run_nodekill.sh <tag> <single|trunk|dns|restart> [opts] [kill_s] [fix_s] [workload_s]
set -u
cd "$(dirname "$0")"; . ./env.sh
tag=$1; var=$2; opts=${3:-vers=4.1,hard}; t_kill=${4:-8}; t_fix=${5:-70}; secs=${6:-100}
./cli.sh umount >/dev/null 2>&1; ./cli.sh umount ${MNT}2 >/dev/null 2>&1
./srv.sh kill9 1 >/dev/null 2>&1; ./srv.sh kill9 2 >/dev/null 2>&1; sleep 1
./cli.sh setup >/dev/null; ./cli.sh hosts >/dev/null
./srv.sh start 1 || exit 1; ./srv.sh start 2 || exit 1; sleep 2
./cli.sh cap-start $tag; ./cli.sh trace-start
case $var in
single|restart) ./cli.sh mount $ADDR_A $opts >/dev/null || exit 1 ;;
trunk)  ./cli.sh mount $ADDR_A $opts,max_connect=2 >/dev/null || exit 1
        ./cli.sh mount $ADDR_A2 $opts,max_connect=2 ${MNT}2 >/dev/null; ./cli.sh umount ${MNT}2 >/dev/null ;;
dns)    ./cli.sh hosts $ADDR_A $ADDR_B; ./cli.sh mount $NAME $opts || exit 1 ;;
esac
./cli.sh state | sed -n '/transports/,$p'
./cli.sh sh "nohup python3 /tmp/fo-workload.py $MNT $secs $tag > /tmp/fo-$tag.out 2>&1 &"
sleep $t_kill
[ $var = trunk ] && ./cli.sh map $ADDR_A2 2
./srv.sh declare-dead 2 1; ./srv.sh kill9 1
[ $var = dns ] && ./cli.sh hosts $ADDR_B
sleep $((t_fix - t_kill)); ./cli.sh state | sed -n '/transports/,$p'
if [ $var = restart ]; then ./srv.sh start 1; else ./cli.sh map $ADDR_A 2; fi
for i in $(seq 1 150); do out=$(./cli.sh sh "cat /tmp/fo-$tag.out"); echo "$out" | grep -q " done " && break; sleep 2; done
echo "--- client:"; echo "$out" | head -30
./cli.sh state | sed -n '/transports/,$p'
./cli.sh cap-stop $tag; ./cli.sh trace-stop $tag
grep -E "hydrat|resynced|grace|is down|reclaim" $RUN/node2.log | sed 's/ process=.*//; s/^time=[0-9-]*T//' | cut -c1-200 | head -8
