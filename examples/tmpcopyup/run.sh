#! /bin/sh

OCIJAIL=${OCIJAIL:-ocijail}

bundle=$(mktemp -d)
cp config.json ${bundle}
mkdir ${bundle}/root
mkdir ${bundle}/root/tmpdir
echo Hello World > ${bundle}/root/tmpdir/file
tar -C / -cf - rescue | sudo tar -C $bundle/root -xf -

if ${OCIJAIL} create -b ${bundle} tmpcopyup; then
    ${OCIJAIL} start tmpcopyup
    sleep 1
    ${OCIJAIL} delete tmpcopyup
fi

rm -rf $bundle
