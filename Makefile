CC=gcc
LIB=`pkg-config --libs gtk+-2.0`
INCLUDE=`pkg-config --cflags gtk+-2.0`
PLUGIN_INCLUDE=-I/usr/include/mozilla/java -I/usr/include/mozilla/plugin -I/usr/include/mozilla -I/usr/include/mozilla/xpcom -I/usr/include/mozilla/string -I/usr/include/mozilla/nspr -I/usr/include/firefox -I/usr/include/firefox/nspr

#CFLAGS=-g
CFLAGS=-O

#Do not change the lines below
BIN=./mozilla-tiff-viewer


viewer: mozilla-tiff-viewer.c
	$(CC)  -o $(BIN) mozilla-tiff-viewer.c $(LIB) ${INCLUDE} 

plugin: npunix.o mozilla-tiff-plugin.o
	$(CC) -o mozilla-tiff-plugin.so -shared npunix.o mozilla-tiff-plugin.o

npunix.o: npunix.c
	$(CC) -fPIC -c npunix.c ${PLUGIN_INCLUDE} 
	
mozilla-tiff-plugin.o: mozilla-tiff-plugin.c
	$(CC) -fPIC -c mozilla-tiff-plugin.c ${PLUGIN_INCLUDE} 

install: mozilla-tiff-viewer mozilla-tiff-plugin.so
	cp $(BIN) /usr/bin/
	if [ ! -d /usr/lib/mozilla-tiff-plugin ]; then mkdir /usr/lib/mozilla-tiff-plugin ;  fi
	cp mozilla-tiff-plugin.so /usr/lib/mozilla-tiff-plugin
	if [ -h /usr/lib/mozilla/plugins/mozilla-tiff-plugin.so ]; then rm /usr/lib/mozilla/plugins/mozilla-tiff-plugin.so; fi
	if [ -h /usr/lib/mozilla-firefox/plugins/mozilla-tiff-plugin.so ]; then rm /usr/lib/mozilla-firefox/plugins/mozilla-tiff-plugin.so; fi
	if [ -h /usr/lib/firefox/plugins/mozilla-tiff-plugin.so ]; then rm /usr/lib/firefox/plugins/mozilla-tiff-plugin.so; fi
	cd /usr/lib/mozilla/plugins && ln -s ../../mozilla-tiff-plugin/mozilla-tiff-plugin.so
	if [ -d /usr/lib/mozilla-firefox/plugins ]; then cd /usr/lib/mozilla-firefox/plugins && ln -s ../../mozilla-tiff-plugin/mozilla-tiff-plugin.so; fi
	if [ -d /usr/lib/firefox/plugins ]; then cd /usr/lib/firefox/plugins && ln -s ../../mozilla-tiff-plugin/mozilla-tiff-plugin.so; fi

	
clean:
	if [ -f $(BIN) ]; then rm $(BIN); fi
	if [ -f npunix.o ]; then rm npunix.o; fi
	if [ -f mozilla-tiff-plugin.o ]; then rm mozilla-tiff-plugin.o ; fi
	if [ -f mozilla-tiff-plugin.so ]; then rm mozilla-tiff-plugin.so; fi
	
