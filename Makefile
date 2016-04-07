all:
	gcc -Wall -Werror -std=c99 -O0 -g -o slua -I/usr/include/lua5.2 slua.c -lpthread -llua5.2
