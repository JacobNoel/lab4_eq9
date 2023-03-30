#!/bin/bash
set -e

# Sync executable
bn=$(basename $1)
rpiaddr="10.0.1.20"

rsync -az $1/*.ko "pi@$rpiaddr:/home/pi/projects/laboratoire4/"

