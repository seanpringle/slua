all:
	gcc -Wall -Werror -std=c99 -O0 -g -o slua -I/usr/include/lua5.2 -DLUA52 slua.c -lpthread -llua5.2
