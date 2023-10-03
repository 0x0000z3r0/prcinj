#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>

int
main(void)
{
	void *addr;
	addr = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (addr == MAP_FAILED) {
		perror("ERR: failed to allocate memory");
		return 1;
	}

	printf("PID: %i, ADDR: %p\n", getpid(), addr);

	while (1) {
		sleep(1);
	}

	return 0;
}
