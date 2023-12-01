#! /bin/sh

OCIJAIL=${OCIJAIL:-ocijail}

bundle=$(mktemp -d)
cp config.json ${bundle}
mkdir ${bundle}/root
tar -C / -cf - rescue | sudo tar -C $bundle/root -xf -

if=$(ifconfig lo create)
if ${OCIJAIL} create -b ${bundle} ipaddr; then
    jls -j ipaddr jid name ip4.addr ip6.addr
    ${OCIJAIL} delete ipaddr
fi
ifconfig ${if} destroy

rm -rf $bundle
