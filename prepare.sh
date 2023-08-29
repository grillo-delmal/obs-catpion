#!/usr/bin/env bash

if [ ! -f "./data/april-english-dev-01110_en.april" ]; then
    pushd data
    wget https://april.sapples.net/april-english-dev-01110_en.april
    popd
fi