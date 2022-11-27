#! /bin/sh

OCIJAIL=${OCIJAIL:-ocijail}

devfs rule -s 100 delset
devfs rule -s 100 add include 1
devfs rule -s 100 add include 2
devfs rule -s 100 add include 3
devfs rule -s 100 add include 4
devfs rule -s 100 add path shm unhide mode 1777

bundle=$(mktemp -d)
cp config.json ${bundle}
cd ${bundle}
fetch https://dl-cdn.alpinelinux.org/alpine/v3.17/releases/x86_64/alpine-minirootfs-3.17.0-x86_64.tar.gz
mkdir root
sudo tar -C root -xf alpine-minirootfs-3.17.0-x86_64.tar.gz

if ${OCIJAIL} create -b ${bundle} alpine; then
    ${OCIJAIL} start alpine
    sleep 1
    ${OCIJAIL} delete alpine
fi
