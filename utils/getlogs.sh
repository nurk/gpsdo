#!/usr/bin/env zsh

scp "playground:/home/tom/gpsdo_rewrite/*.log" $(dirname "$0")/../logs/