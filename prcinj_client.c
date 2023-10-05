#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/ioctl.h>

#include "prcinj.h"

int 
main(int argc, char *argv[])
{
	if (argc < 2) {
		printf("ERR: missing arguments\n");
		exit(1);
	}

	int dev;
	dev = open("/dev/" PRCINJ_DEV_NAME, 0);
	if (dev < 0) {
		perror("ERR: could not open the device");
		exit(1);
	}

	unsigned char shell[] = {
		0x00, 0x11, 0x22, 0x33,
		0x44, 0x55, 0x66, 0x77,
		0x88, 0x99, 0xAA, 0xBB,
		0xCC, 0xDD, 0xEE, 0xFF,
		'P', 'R', 'C', 'I', 'N', 'J'
	};

	struct prcinj_req req;
	req.pid   = atoi(argv[1]);
	req.shell = shell;
	req.len   = sizeof (shell);

	int res;
	res = ioctl(dev, PRCINJ_IOCTL_INJ, &req);
	if (res < 0) {
		printf("ERR: ioctl failed [res-%i]", res);
		perror("");
	}

	close(dev);

	return 0;
}
