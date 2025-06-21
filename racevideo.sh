#!/usr/bin/env bash
export XDG_RUNTIME_DIR="/run/user/1000"
exec chrt -f 99 python3 /home/gavin/ath/racevideo.py
