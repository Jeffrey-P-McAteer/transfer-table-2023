#!/bin/bash

if [[ -z "$pi_ip" ]] ; then
  #pi_ip=$(lanipof 'b8:27:eb:ce:37:8f')
  pi_ip=$(lanipof '6c:5a:b0:38:d2:b6')
  echo "Pi IP = $pi_ip"
fi

if ! [[ -z "NO_VERBOSE" ]] ; then
  exec rsync -az \
    --checksum --delete \
    --exclude 'build' --exclude 'zig-out' --exclude 'zig-cache' \
       $PWD/. user@$pi_ip:/home/user/transfer-table-2023
else
  exec rsync -avz \
    --checksum --delete \
    --exclude 'build' --exclude 'zig-out' --exclude 'zig-cache' \
       $PWD/. user@$pi_ip:/home/user/transfer-table-2023
fi

