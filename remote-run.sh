#!/bin/bash

set -e 

#export pi_ip=$(lanipof 'b8:27:eb:ce:37:8f')
export pi_ip=$(lanipof '6c:5a:b0:38:d2:b6')
export NO_VERBOSE=1

./sync.sh

./ssh.sh "cd /home/user/transfer-table-2023 && make run"

