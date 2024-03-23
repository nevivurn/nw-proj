#!/bin/bash

if [ ! $# -eq 2 ]; then
    echo "usage: $0 [student_id] [name]"
    exit
fi

ID=$1
NAME=$2
echo "student id: $ID"
echo "student name: $NAME"

FOLDER="${ID}_${NAME}_assign1"
if [ ! -e $FOLDER ]; then
    mkdir $FOLDER
else
    echo "$FOLDER already exist!"
fi

SCLIENT="sclient.c"
SSERVER="sserver.c"
MACRO="macro.h"
README="readme.pdf"
MAKEFILE="Makefile"

if [ ! -e $SCLIENT ]; then
    echo "$SCLIENT is missing!"
    exit
elif [ ! -e $SSERVER ]; then
    echo "$SSERVER is missing!"
    exit
elif [ ! -e $MACRO ]; then
    echo "$MACRO is missing!"
    exit
elif [ ! -e $README ]; then
    echo "$README is missing!"
    exit
elif [ ! -e $MAKEFILE ]; then
    echo "$MAKEFILE is missing!"
    exit
fi

cp $SCLIENT $SSERVER $MACRO $README $MAKEFILE $FOLDER

OUTPUT="${FOLDER}.tar.gz"

if [ -e $OUTPUT ]; then
    echo "$OUTPUT already exist, delete old one."
    rm $OUTPUT
fi

tar zcf $OUTPUT $FOLDER