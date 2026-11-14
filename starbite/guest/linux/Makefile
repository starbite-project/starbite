# sbguest.ko, out of tree against the Starbite Linux 7.1 build (run-qemu.sh / build-guest.sh call this):
#   make -C $KSRC O=$KOUT M=$PWD modules
obj-m := sbguest.o
sbguest-objs := sbg_core.o sbnet.o sbp9.o sbsnd.o sbgpu.o
ccflags-y := -I$(src)/..
