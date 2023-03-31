#!/bin/bash
set -e

# Sync executable
bn=$(basename $1)
rpiaddr="ADRESSE-DE-VOTRE-RASPBERRY-PI-ICI"

rsync -az $1/*.ko "pi@$rpiaddr:/home/pi/projects/laboratoire4/"

