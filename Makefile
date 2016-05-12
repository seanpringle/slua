local:
	gcc -Wall -Werror -std=c99 -O0 -g -o slua -I/usr/include/lua5.2 -DLUA52 slua.c -lpthread -llua5.2 -lsqlite3 -lpcre

el6:
	gcc -Wall -Werror -std=c99 -O0 -g -o /usr/local/bin/slua -DLUA51 slua.c -lpthread -llua -lsqlite3 -lrt -lpcre
