#! /bin/bash

g++ -std=c++17 -O2 -o test_server2 -pthread server2.cpp -lboost_filesystem -lboost_system -lfmt
