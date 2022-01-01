#! /bin/bash

g++ -std=c++17 -O2 -o fbs_server -pthread server2.cpp -lboost_filesystem -lboost_system -lfmt
g++ -std=c++17 -O2 -o fbs_client -pthread client.cpp -lboost_system -lfmt
