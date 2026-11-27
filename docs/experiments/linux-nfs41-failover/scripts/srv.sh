#!/bin/bash
# Two chimera daemons on one host, sharing NFS client and session records
# through the repkv module.  Both run in one container so the linux store's
# mount ID in file handles is the same for both.
#   node 1: NFS port 2049, repkv 7001 (peer 7002)
#   node 2: NFS port 2050, repkv 7002 (peer 7001)
# usage: srv.sh start <node> [lease_s] [grace_s] | kill9 <node> | status | down
#        srv.sh declare-dead <live node> <dead node>
set -u
. "$(dirname "$0")/env.sh"
CT=chimera-fo
kv_port() { echo $((7000 + $1)); }
ensure_container() {
    docker inspect -f '{{.State.Running}}' $CT 2>/dev/null | grep -q true && return
    docker rm -f $CT >/dev/null 2>&1
    docker run -d --name $CT --net=host --ulimit memlock=-1 --privileged -e HOME=/tmp \
        -v $SRC:$SRC:ro -v $RUN:$RUN -v $(dirname $MOD):$(dirname $MOD):ro $IMG sleep infinity >/dev/null
}
case "${1:-}" in
start)
    node=$2; lease=${3:-30}; grace=${4:-30}; peer=$((3 - node))
    mkdir -p $RUN/store/data $RUN/logs; chmod 1777 $RUN/store/data
    cfg=$RUN/node$node.json
    cat > $cfg <<CFG
{
    "common": { "rcu_reclaim_threads": 4 },
    "server": {
        "nfs_enabled": true, "nfs3_disabled": true,
        "nfs_port": $(nfs_port $node), "metrics_port": $((9000 + node)),
        "threads": 4, "kv_module": "repkv", "nfs4_drc": true,
        "nfs4_node_id": $node, "nfs4_drc_replay_only": ${REPLAY_ONLY:-0},
        "nfs4_lease_time": $lease, "nfs4_grace_time": $grace,
        "vfs": { "repkv": { "path": "$MOD",
                            "config": { "mode": "sync", "listen_host": "127.0.0.1",
                                        "listen_port": $(kv_port $node),
                                        "peer_host": "127.0.0.1", "peer_port": $(kv_port $peer) } } },
        "external_portmap": false
    },
    "mounts": { "share": { "module": "linux", "path": "$RUN/store/data" } },
    "exports": { "/share": { "path": "/share" } }
}
CFG
    log=$RUN/logs/$(date -u +%H%M%S)_node$node.log; ln -sfn $log $RUN/node$node.log
    ensure_container
    docker exec -d $CT sh -c "echo \$\$ > $RUN/node$node.pid; exec $SRC/build/Release/src/daemon/chimera -d -c $cfg > $log 2>&1"
    for i in $(seq 1 100); do
        if (echo > /dev/tcp/127.0.0.1/$(nfs_port $node)) 2>/dev/null; then
            sleep 1
            docker exec $CT kill -0 $(cat $RUN/node$node.pid) 2>/dev/null || { echo "daemon exited"; tail -20 $log; exit 1; }
            echo "up node=$node port=$(nfs_port $node) $(date -u +%T.%3N)"; exit 0
        fi
        sleep 0.1
    done
    echo "port never came up"; tail -30 $log; exit 1 ;;
kill9)  docker exec $CT kill -KILL $(cat $RUN/node$2.pid) 2>/dev/null; echo "killed node $2 $(date -u +%T.%3N)" ;;
status) for n in 1 2; do p=$(cat $RUN/node$n.pid 2>/dev/null) || continue
            docker exec $CT kill -0 $p 2>/dev/null && echo "node $n running" || echo "node $n not running"; done ;;
declare-dead)
    # Stand-in for the cluster liveness signal: delete the dead node's
    # heartbeat record on the live node.  chimera treats that node as down at once.
    python3 - "$2" "$3" <<'PY'
import socket, struct, sys, time
live, dead = int(sys.argv[1]), int(sys.argv[2])
HDR = "<IBBHQIIII"; key = bytes([0xC4, 0x02, 0x02, (dead >> 8) & 0xff, dead & 0xff])
s = socket.create_connection(("127.0.0.1", 7000 + live), timeout=5)
s.sendall(struct.pack(HDR, 0x564b5052, 2, 1, 0, 1, len(key), 0, 0, 0) + key)
r = struct.unpack(HDR, s.recv(struct.calcsize(HDR), socket.MSG_WAITALL))
assert r[0] == 0x564b5052 and r[1] == 3
print(time.strftime("%H:%M:%S", time.gmtime()), f"node {live} told that node {dead} is dead")
PY
    ;;
down)   docker rm -f $CT >/dev/null; echo "container removed" ;;
*) echo "usage: $0 start <node> [lease] [grace] | kill9 <node> | status | declare-dead <live> <dead> | down"; exit 2 ;;
esac
