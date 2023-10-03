#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/ioctl.h>

#include "prcinj.h"

int 
main(int argc, char *argv[])
{
	if (argc < 5) {
		printf("ERR: missing arguments\n");
		exit(1);
	}

	int dev;
	dev = open("/dev/" PRCINJ_DEV_NAME, 0);
	if (dev < 0) {
		perror("ERR: could not open the device\n");
		exit(1);
	}

	struct prcinj_req req;
	req.pid  = atoi(argv[1]);
	req.addr = (void*)strtol(argv[2], NULL, 16);
	req.len  = strtol(argv[3], NULL, 16);
	req.prot = strtol(argv[4], NULL, 16);

	int res;
	res = ioctl(dev, PRCINJ_IOCTL_PROT, &req);
	if (res < 0) {
		perror("ERR: ioctl failed");
	}

	close(dev);

	return 0;
}
