all:usbkbd_sim.o
	gcc usbkbd_sim.o -pthread -o usbkbd_sim
usbkbd_sim.o:usbkbd_sim.c
	gcc -c usbkbd_sim.c -pthread
clean:
	rm -rf *.o
