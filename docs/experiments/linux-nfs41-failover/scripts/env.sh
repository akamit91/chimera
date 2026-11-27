# Common settings.  Put site values in a file and point LAB_ENV at it.
#   CLIENT     ssh name of the Linux NFS client machine (needs passwordless sudo)
#   SRV_IP     address of the host that runs the chimera daemons
#   CLIENT_IP  client address on the network that reaches SRV_IP
#   CLIENT_IF  client interface for that network
#   SRC        chimera source tree with a Release build (branch poc-inline-restore)
#   RUN        scratch directory on the server host (configs, logs, store, captures)
#   MOD        built repkv module (poc/repkv/build.sh)
#   IMG        chimera dev image used to run the daemons
[ -n "${LAB_ENV:-}" ] && . "$LAB_ENV"
: "${CLIENT:?}" "${SRV_IP:?}" "${CLIENT_IP:?}" "${CLIENT_IF:?}" "${SRC:?}" "${RUN:?}" "${MOD:?}" "${IMG:?}"
# Addresses the client sees.  They are documentation addresses (RFC 5737); a
# DNAT rule on the client maps each one to the port of one daemon on SRV_IP.
ADDR_A=192.0.2.11      # node 1
ADDR_B=192.0.2.12      # node 2
ADDR_A2=192.0.2.21     # a second address of node 1 (same daemon)
NAME=nfs.example.test
MNT=/mnt/chimera-fo
NET=192.0.2.0/24
nfs_port() { echo $((2048 + $1)); }
