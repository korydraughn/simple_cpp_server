#! /bin/bash

g++ -std=c++17 -g -Og -o fbs_server -pthread server2.cpp -lboost_filesystem -lboost_system -lfmt
g++ -std=c++17 -g -Og -o fbs_client -pthread client.cpp -lboost_system -lfmt
