#!/bin/bash
# Client-side helpers, run from the server host; they act on $CLIENT over ssh.
# usage: cli.sh setup | teardown
#        cli.sh map <addr> <node>          point a client-visible address at a node
#        cli.sh mount <server> [options] [mountpoint]
#        cli.sh umount [mountpoint]
#        cli.sh blackhole <addr> on|off    silently drop all traffic of connections made to <addr>
#        cli.sh hosts <addr...>            make $NAME resolve to these addresses (none = remove)
#        cli.sh cap-start <tag> | cap-stop <tag>
#        cli.sh trace-start | trace-stop <tag>
#        cli.sh state                      transports and sessions as the kernel sees them
#        cli.sh sh '<command>'
set -u
. "$(dirname "$0")/env.sh"
c() { ssh -o BatchMode=yes $CLIENT "$@"; }
rule() { echo "-p tcp -d $1 --dport 2049 -j DNAT --to-destination $SRV_IP:$(nfs_port $2)"; }
unmap() { c "sudo iptables -t nat -S OUTPUT | grep -- '-d $1/32' | sed 's/^-A/-D/' | while read r; do sudo iptables -t nat \$r; done"; }
TR=/sys/kernel/tracing/instances/nfsfo
EV="nfs4/nfs4_exchange_id nfs4/nfs4_create_session nfs4/nfs4_bind_conn_to_session nfs4/nfs4_destroy_session nfs4/nfs4_reclaim_complete nfs4/nfs4_state_mgr nfs4/nfs4_state_mgr_failed nfs4/nfs4_sequence_done nfs4/nfs4_get_fs_locations nfs4/nfs4_trunked_exchange_id nfs4/nfs4_open_reclaim nfs4/nfs4_open_expired sunrpc/xprt_create sunrpc/xprt_destroy sunrpc/xprt_connect sunrpc/xprt_disconnect_done sunrpc/xprt_disconnect_force sunrpc/rpc_connect_status sunrpc/xprt_timer sunrpc/xprt_retransmit sunrpc/rpc_timeout_status sunrpc/rpc_clnt_new"
case "${1:-}" in
setup)    c "sudo ip route replace $NET dev $CLIENT_IF src $CLIENT_IP"
          for p in "$ADDR_A 1" "$ADDR_B 2" "$ADDR_A2 1"; do set -- $p; unmap $1; c "sudo iptables -t nat -I OUTPUT $(rule $1 $2)"; done
          c "sudo iptables -t nat -S OUTPUT | grep 192.0.2; ip route show $NET" ;;
teardown) for a in $ADDR_A $ADDR_B $ADDR_A2; do unmap $a; "$0" blackhole $a off; done
          "$0" hosts; c "sudo ip route del $NET 2>/dev/null; sudo rmdir $TR 2>/dev/null; sudo iptables -t nat -S OUTPUT; sudo iptables -S | grep -c ctorigdst" ;;
map)      unmap $2; c "sudo iptables -t nat -I OUTPUT $(rule $2 $3); date -u +'$2 -> node $3 %T.%3N'" ;;
mount)    srv=$2; opts=${3:-vers=4.1,hard}; m=${4:-$MNT}
          c "sudo mkdir -p $m && sudo mount -t nfs4 -o $opts $srv:/share $m && sudo chmod 1777 $m; grep ' $m ' /proc/mounts" ;;
umount)   m=${2:-$MNT}; c "sudo umount -f -l $m 2>/dev/null; sudo rmdir $m 2>/dev/null; grep -c ' $m ' /proc/mounts" ;;
blackhole) for ch in OUTPUT INPUT; do
              if [ "$3" = on ]; then c "sudo iptables -I $ch -p tcp -m conntrack --ctorigdst $2 -j DROP"
              else c "while sudo iptables -D $ch -p tcp -m conntrack --ctorigdst $2 -j DROP 2>/dev/null; do :; done"; fi; done
          c "date -u +'blackhole $2 $3 %T.%3N'" ;;
hosts)    shift; c "sudo sed -i '/ $NAME\$/d' /etc/hosts"; for a in "$@"; do c "echo '$a $NAME' | sudo tee -a /etc/hosts >/dev/null"; done
          c "getent ahosts $NAME | grep STREAM" ;;
cap-start) c "sudo rm -f /tmp/fo-$2.pcap; sudo sh -c 'nohup tcpdump -i $CLIENT_IF -s 0 -w /tmp/fo-$2.pcap host $SRV_IP and portrange 2049-2050 >/dev/null 2>&1 & echo \$! > /tmp/fo-tcpdump.pid'; sleep 1" ;;
cap-stop)  c "sudo kill \$(cat /tmp/fo-tcpdump.pid); sleep 1"; mkdir -p $RUN/pcap; scp -q $CLIENT:/tmp/fo-$2.pcap $RUN/pcap/$2.pcap; ls -la $RUN/pcap/$2.pcap | awk '{print $5, $9}' ;;
trace-start) c "sudo mkdir -p $TR; sudo sh -c 'echo 0 > $TR/tracing_on; echo > $TR/trace; echo 16384 > $TR/buffer_size_kb; for e in $EV; do [ -e $TR/events/\$e/enable ] && echo 1 > $TR/events/\$e/enable; done; echo 1 > $TR/tracing_on'" ;;
trace-stop)  mkdir -p $RUN/trace; c "sudo sh -c 'echo 0 > $TR/tracing_on; cat $TR/trace; echo 0 > $TR/events/enable'" > $RUN/trace/$2.txt; wc -l $RUN/trace/$2.txt ;;
state)    c 'echo "== nfs servers"; cat /proc/fs/nfsfs/servers; echo "== transports"; for x in /sys/kernel/sunrpc/xprt-switches/*/xprt-*; do a=$(cat $x/dstaddr 2>/dev/null); case "$a" in 192.0.2.*) echo "$(basename $(dirname $x)) $(basename $x) dst=$a $(cat $x/xprt_state | tr "\n" " ") $(grep -E "^(main_xprt|src_port)" $x/xprt_info | tr "\n" " ")";; esac; done' ;;
sh)       shift; c "$@" ;;
*) sed -n '2,13p' "$0"; exit 2 ;;
esac
