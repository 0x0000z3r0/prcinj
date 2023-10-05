#ifndef _PRCINJ_H_
#define _PRCINJ_H_

#include <linux/ioctl.h>
#include <linux/types.h>

#define PRCINJ_DEV_NAME  "prcinj"
#define PRCINJ_IOCTL_INJ _IOW('9', 1, struct prcinj_req)

struct prcinj_req {
	pid_t 		pid;
	unsigned char 	*shell;
	size_t		len;
};

#endif
