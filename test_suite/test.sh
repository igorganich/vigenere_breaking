#!/bin/bash

ALLCYPHERS=$(ls cyphers)

for CYPHERFILE in $ALLCYPHERS
do
    echo "Test $CYPHERFILE"
    ../bin/vigenere -d $PWD/cyphers/$CYPHERFILE > /tmp/cracked
    DIFF=$(diff /tmp/cracked $PWD/plains/$CYPHERFILE)
    if [ "$DIFF" != "" ]
    then
        echo "Test FAILED! Expected:"
        cat "$PWD/plains/$CYPHERFILE"
        echo "but got:"
        cat "/tmp/cracked"
        echo "for cyphertext:"
        cat "$PWD/cyphers/$CYPHERFILE"
    else
        echo "Test PASSED for cypher:"
        echo "$PWD/cyphers/$CYPHERFILE"
    fi
done