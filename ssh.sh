#!/bin/bash

if [[ -z "$pi_ip" ]] ; then
  #pi_ip=$(lanipof 'b8:27:eb:ce:37:8f')
  pi_ip=$(lanipof '6c:5a:b0:38:d2:b6')
  echo "Pi IP = $pi_ip"
fi

if ! [ -z "$@" ] ; then
  exec ssh user@$pi_ip "$@"
else
  exec ssh -t user@$pi_ip "cd /home/user/transfer-table-2023 ; bash --login"
fi


