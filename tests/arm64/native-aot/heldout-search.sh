#!/bin/sh
# Fixed BusyBox search/sort evaluation, never training input.
set -eu
cd "$1"
for pass in 1 2 3; do
    grep -c 'tag=needle' records.txt
    grep 'tag=needle' records.txt | cut -d ' ' -f 2 | sort | uniq -c | cksum
    cksum < records.txt
done
echo HELDOUT_SEARCH_OK
