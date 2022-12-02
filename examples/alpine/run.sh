#! /bin/sh

OCIJAIL=${OCIJAIL:-ocijail}

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
