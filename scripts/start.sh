#!/bin/bash

trap "kill 0" EXIT

if [[ -z "$1" ]]; then
   echo "Number of servers not passed to start.sh"
   exit 1
fi

if [[ -z "$QUANTRA_SERVER_PORT" ]]; then
   echo "BASE_PORT env variable not set"
   exit 1
fi

if [[ -z "$QUANTRA_PORT" ]]; then
   echo "QUANTRA_PORT env variable not set"
   exit 1
fi

if [[ -z "$QUANTRA_HOME" ]]; then
   echo "QUANTRA_HOME env variable not set"
   exit 1
fi

start_port=$QUANTRA_SERVER_PORT
n_servers=$1

end_port=$[$start_port+$n_servers]

while [ $start_port -lt $end_port ]
do
$QUANTRA_HOME/build/server/sync_server $start_port &
start_port=$[$start_port+1]
done

python3 -c "from envoy_config import config; config($QUANTRA_SERVER_PORT, $QUANTRA_PORT, $1, '$QUANTRA_HOME')"
envoy -c "$QUANTRA_HOME/scripts/quantra.yaml" &
sleep 3
python3 -c "from envoy_config import check_clusters_health; check_clusters_health()"
python3 -m http.server 80

wait
