#! /bin/sh

OCIJAIL=${OCIJAIL:-ocijail}

bundle=$(mktemp -d)
cp config.json ${bundle}
mkdir ${bundle}/root
mkdir ${bundle}/root/tmpdir
echo Hello World > ${bundle}/root/tmpdir/file
tar -C / -cf - rescue | sudo tar -C $bundle/root -xf -

if ${OCIJAIL} create -b ${bundle} filemount; then
    ${OCIJAIL} start filemount
    sleep 1
    ${OCIJAIL} delete filemount
fi

rm -rf $bundle
