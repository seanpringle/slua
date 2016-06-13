local:
	gcc -Wall -Werror -std=c99 -O2 -g -o slua -I/usr/include/lua5.2 -DLUA52 slua.c -lrt -lpthread -llua5.2 -lpcre -lssl -lcrypto

el6:
	gcc -Wall -Werror -std=c99 -O0 -g -o /usr/local/bin/slua -DLUA51 slua.c -lrt -lpthread -llua -lrt -lpcre -lssl -lcrypto
