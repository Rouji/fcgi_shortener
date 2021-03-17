BIN=./shortener.fcgi

bin: fcgi_shortener.cpp
	g++ -O0 -ggdb -std=c++17 -I/usr/include/fastcgi fcgi_shortener.cpp -lfcgi -lpthread -llmdb -o${BIN}

run: bin
	spawn-fcgi -n -p 1234 -- ${BIN} /tmp/test.mdb 16 1
