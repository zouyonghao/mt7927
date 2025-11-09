
cd /home/zyh/mt7927/mt76-outoftree

sudo modprobe mt7925e

make -C /lib/modules/$(uname -r)/build M=/home/zyh/mt7927/mt76-outoftree clean -j10
make -C /lib/modules/$(uname -r)/build M=/home/zyh/mt7927/mt76-outoftree modules -j10
sudo rmmod mt7925e mt7925_common mt792x_lib mt76_connac_lib mt76
sudo dmesg -C
sudo insmod mt76.ko && sudo insmod mt76-connac-lib.ko && sudo insmod mt792x-lib.ko && sudo insmod mt7925/mt7925-common.ko && sudo insmod mt7925/mt7925e.ko