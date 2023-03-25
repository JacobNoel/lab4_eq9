#!/bin/bash
set -e

# Sync executable
bn=$(basename $1)
rpiaddr="10.248.16.151"

rsync -az $1/*.ko "pi@$rpiaddr:/home/pi/projects/laboratoire4/"

