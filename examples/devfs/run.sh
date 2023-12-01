#! /bin/sh

OCIJAIL=${OCIJAIL:-ocijail}

bundle=$(mktemp -d)
cp config.json ${bundle}
mkdir ${bundle}/root
mkdir ${bundle}/root/tmpdir
echo Hello World > ${bundle}/root/tmpdir/file
tar -C / -cf - rescue | sudo tar -C $bundle/root -xf -

if ${OCIJAIL} create -b ${bundle} devfs; then
    ${OCIJAIL} start devfs
    sleep 1
    ${OCIJAIL} delete devfs
fi

rm -rf $bundle
