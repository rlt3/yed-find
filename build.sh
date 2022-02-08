#!/bin/bash

gcc -Wall -o find-regex.so find-regex.c $(yed --print-cflags) $(yed --print-ldflags)
