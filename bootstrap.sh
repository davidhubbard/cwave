#!/bin/bash

if [ -z "$1" ]; then
	echo "Usage: $0 prefix"
	echo "    Please specify configure --prefix"
	echo "    Example: $0 $HOME/local"
	exit 1
fi

cd unbound-svn || exit 1
autoupdate --force || exit 1
autoreconf --force --verbose --install || exit 1
./configure --enable-static --with-ssl "--prefix=$1" || exit 1
cd .. || exit 1

autoupdate --force || exit 1
autoreconf --force --verbose --install || exit 1

DBINCLUDE=
if ls /usr/include/db*/db.h >/dev/null 2>&1; then
	DBINCLUDE="--with-extra-include-path=`ls /usr/include/db*/db.h | sort | tail -n1 | sed -e 's:/db.h$::'`"
fi
./configure --enable-static --enable-developer --enable-debug --enable-ssl --enable-db --with-zlib \
	$DBINCLUDE "--prefix=$1" || exit 1

make || exit 1
make install || exit 1
