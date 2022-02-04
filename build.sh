#!/bin/bash

gcc -o find.so find.c $(yed --print-cflags) $(yed --print-ldflags)
