#!/bin/bash

if [ ! $# -eq 2 ]; then
    echo "usage: $0 [student_id] [name]"
    exit
fi

ID=$1
NAME=$2
ID=$(echo $ID | sed 's/-//g')
echo "student id w/o dash: $ID"
echo "student name: $NAME"

FOLDER="${ID}_${NAME}_assign3"
if [ ! -e $FOLDER ]; then
    mkdir $FOLDER
else
    echo "$FOLDER already exist!"
fi

TRANSPORT="transport.c"
README="readme.pdf"

if [ ! -e $TRANSPORT ]; then
    echo "$TRANSPORT is missing!"
    exit
elif [ ! -e $README ]; then
    echo "$README is missing!"
    exit
fi

cp $TRANSPORT $README $FOLDER

OUTPUT="${FOLDER}.tar.gz"

if [ -e $OUTPUT ]; then
    echo "$OUTPUT already exist, delete old one."
    rm $OUTPUT
fi

tar zcf $OUTPUT $FOLDER