#!/usr/bin/env bash
set -e

gcc mini-rv32ima.c -O2 -o mini-rv32ima
./mini-rv32ima -f Image