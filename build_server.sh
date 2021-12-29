#! /bin/bash

g++ -std=c++17 -O2 -o test_server -pthread server.cpp -lboost_filesystem -lboost_system -lfmt
