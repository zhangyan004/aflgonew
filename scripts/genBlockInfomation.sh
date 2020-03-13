#!/bin/bash
echo "initial Block name and id..."
TMPDIR=$(readlink -e $1)
AFLGO="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
$AFLGO/setBlockInfo.py -b $TMPDIR/BBnames.txt -o $TMPDIR/block.info.txt
