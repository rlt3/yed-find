#!/bin/bash

gcc -Wall -o find.so find.c $(yed --print-cflags) $(yed --print-ldflags)
