#!/bin/bash

#pi_ip=$(lanipof 'b8:27:eb:ce:37:8f')
pi_ip=$(lanipof '6c:5a:b0:38:d2:b6')
echo "Pi IP = $pi_ip"

rsync -av $PWD user@$pi_ip:/home/user/transfer-table-2023

