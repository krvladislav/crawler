#!/bin/sh

g++ -Wall -std=c++11 -pthread -o crawler ./crawler.cpp -lcurl -lboost_regex -lboost_filesystem -lboost_system 
