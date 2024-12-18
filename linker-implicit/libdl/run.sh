#!/bin/bash

# Dependencies: gcc (tested: 11.4.0, on Ubuntu 22.04)

hex()
{
  printf "0x%X\n" $1
}

hex_pfloor()
{
  hex $(( $1 / 0x1000 * 0x1000))
}

hex_pceil()
{
  a=$(( $1 ))
  n=$(( $1 / 0x1000 * 0x1000))
  if [ $n -lt $a ]; then
    n=$(( ($1 / 0x1000 + 1) * 0x1000))
  fi
  hex $n
}

# Set LD and PLD base addresses. Make sure to map these in 'uninstrumented' address range
# Also make sure these address range do not collide with where the binary itself maps
# Currently: pld_base and ld_base live below the 'moved stack'
# XXX: do this dynamically to avoid any possible collision
pld_base=0x7000A00000
ld_base=0x7000C00000

# Figure out LD VMA properties
ld_so=$( ldd /bin/true | grep ld- | awk '{ print $1; }' )
ld_entry=$(readelf -lh $ld_so | grep -i "Entry point" | awk '{ print $NF; }')
echo "*** Figuring out LD VMA properties ($ld_so, entry: $ld_entry, base: $ld_base)..."
readelf -lh $ld_so | grep -A 1 LOAD | sed "s/R E/RE/g" | xargs | sed "s/LOAD/\n/g" | tail +2 | while read -r line ; do
    off=$(hex_pfloor `echo $line | awk '{ print $1; }'`)
    saddr=$(hex_pfloor `echo $line | awk '{ print $2; }'`)
    len=$(echo $line | awk '{ print $4; }')
    eaddr=$(hex_pceil $(($saddr + $len)))
    prot=$(echo $line | awk '{ print $6; }')
    echo $saddr $eaddr $off $prot F
done > ld.prop
line=$(readelf -S $ld_so | grep -A 1 bss | xargs)
saddr=$(hex_pfloor 0x`echo $line | awk '{ print $4; }'`)
len=0x$(echo $line | awk '{ print $6; }')
eaddr=$(hex_pceil $(($saddr + $len)))
echo $saddr $eaddr 0x0 RW A >> ld.prop
cat ld.prop

# Compute compiler defines
ld_entry=$(( $ld_base + $ld_entry ))
ld_maps=$( cat ld.prop | awk --non-decimal-data -v ld_base="$ld_base" '{ printf( "0x%x,0x%x,0x%x,%s,%s,", $1+ld_base,$2+ld_base,$3,$4,$5); }' | sed "s/,$//g" )
rm -f ld.prop

# Build PLD
gcc -O2 -DLD_SO=\"$ld_so\" -DLD_ENTRY=$ld_entry -DLD_MAPS=$ld_maps \
    -e _pld_start -nostdlib -nodefaultlibs -fPIC pld.S pld.c printf.c -o pld.so -T gcclinker.ld

# Run test program with PLD chaining to default (LD) dynamic linker
gcc -Og -g -no-pie -Wl,--dynamic-linker=$(pwd)/pld.so -o test test.c
chmod +x ./test
./test
