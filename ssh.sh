#!/bin/bash

pi_ip=$(lanipof 'b8:27:eb:ce:37:8f')
echo "Pi IP = $pi_ip"

ssh user@$pi_ip
