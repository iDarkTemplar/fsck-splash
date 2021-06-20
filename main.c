/*
 * fsck-splash, splashscreen wrapper for fsck.
 * Copyright (C) 2021 i.Dark_Templar <darktemplar@dark-templar-archives.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

void get_child_return_code(pid_t child_pid, int *result)
{
	int rc = 0;
	pid_t waitpid_rc;

	waitpid_rc = waitpid(child_pid, &rc, 0);
	if (waitpid_rc == child_pid)
	{
		if ((WIFEXITED(rc)) && (result != NULL))
		{
			*result = WEXITSTATUS(rc);
		}
	}
	else if (waitpid_rc == -1)
	{
		fprintf(stderr, "waitpid() failed with error %d: %s\n", errno, strerror(errno));
	}
	else
	{
		fprintf(stderr, "waitpid() failed\n");
	}
}

int main(int argc, char **argv)
{
	int rc;
	int result = -1;
	int progress_pipes[2] = { -1, -1 };
	int control_pipes[2] = { -1, -1 };
	char *fdstring = NULL;
	char **child_argv = NULL;
	pid_t child_pid;
	struct pollfd poll_fd;
	FILE *progress_file = NULL;
	ssize_t getline_rc;
	char *read_string = NULL;
	size_t read_string_len = 0;

	if (argc < 2)
	{
		fprintf(stderr, "USAGE: %s fsck [fsck options]\n", argv[0]);
		goto error_1;
	}

	rc = pipe(progress_pipes);
	if (rc < 0)
	{
		fprintf(stderr, "pipe() failed with error %d: %s\n", errno, strerror(errno));
		goto error_1;
	}

	progress_file = fdopen(progress_pipes[0], "r");
	if (progress_file == NULL)
	{
		fprintf(stderr, "fdopen() failed with error %d: %s\n", errno, strerror(errno));
		goto error_2;
	}

	rc = pipe2(control_pipes, O_CLOEXEC);
	if (rc < 0)
	{
		fprintf(stderr, "pipe2() failed with error %d: %s\n", errno, strerror(errno));
		goto error_3;
	}

	rc = asprintf(&fdstring, "-C%d", progress_pipes[1]);
	if (rc < 0)
	{
		fprintf(stderr, "asprintf() failed with error %d: %s\n", errno, strerror(errno));
		goto error_4;
	}

	child_argv = (char**) malloc(sizeof(char*) * (argc + 1));
	if (child_argv == NULL)
	{
		fprintf(stderr, "malloc() failed with error %d: %s\n", errno, strerror(errno));
		goto error_5;
	}

	child_pid = fork();
	if (child_pid == -1)
	{
		fprintf(stderr, "fork() failed with error %d: %s\n", errno, strerror(errno));
		goto error_6;
	}

	if (child_pid == 0)
	{
		fclose(progress_file);
		progress_file = NULL;

		close(control_pipes[0]);
		control_pipes[0] = -1;

		child_argv[0] = argv[1];
		child_argv[1] = fdstring;

		for (rc = 2; rc < argc; ++rc)
		{
			child_argv[rc] = argv[rc];
		}

		child_argv[argc] = NULL;

		execvp(child_argv[0], child_argv);

		// if execvp failed, signal error to parent
		write(control_pipes[1], "E", 1);
		return -1;
	}

	close(progress_pipes[1]);
	progress_pipes[1] = -1;

	close(control_pipes[1]);
	control_pipes[1] = -1;

	free(child_argv);
	child_argv = NULL;

	free(fdstring);
	fdstring = NULL;

	poll_fd.events = POLLIN;
	poll_fd.revents = 0;
	poll_fd.fd = control_pipes[0];

	rc = poll(&poll_fd, 1, -1);
	if (rc < 0)
	{
		fprintf(stderr, "poll() failed with error %d: %s\n", errno, strerror(errno));
		get_child_return_code(child_pid, NULL);
		goto error_6;
	}

	if (rc == 0)
	{
		fprintf(stderr, "poll() timed out\n");
		get_child_return_code(child_pid, NULL);
		goto error_6;
	}

	if (poll_fd.revents & POLLIN)
	{
		fprintf(stderr, "Failed to start fsck\n");
		get_child_return_code(child_pid, NULL);
		goto error_6;
	}

	close(control_pipes[0]);
	control_pipes[0] = -1;

	poll_fd.fd = progress_pipes[0];

	for (;;)
	{
		poll_fd.revents = 0;

		rc = poll(&poll_fd, 1, -1);

		if (poll_fd.revents & POLLIN)
		{
			getline_rc = getline(&read_string, &read_string_len, progress_file);

			fprintf(stdout, "Read %zu: %s", read_string_len, read_string);
		}

		if (poll_fd.revents & POLLERR)
		{
			fprintf(stderr, "An error occurred while polling pipe\n");
			get_child_return_code(child_pid, &result);
			goto error_7;
		}

		if (poll_fd.revents & POLLHUP)
		{
			break;
		}
	}

	free(read_string);
	read_string = NULL;

	fclose(progress_file);
	progress_file = NULL;

	result = 0;
	get_child_return_code(child_pid, &result);

	return result;

error_7:
	free(read_string);
	read_string = NULL;

error_6:
	free(child_argv);
	child_argv = NULL;

error_5:
	free(fdstring);
	fdstring = NULL;

error_4:
	close(control_pipes[0]);
	control_pipes[0] = -1;
	close(control_pipes[1]);
	control_pipes[1] = -1;

error_3:
	if (progress_file != NULL)
	{
		fclose(progress_file);
		progress_file = NULL;
		progress_pipes[0] = -1;
	}

error_2:
	close(progress_pipes[0]);
	progress_pipes[0] = -1;
	close(progress_pipes[1]);
	progress_pipes[1] = -1;

error_1:
	return result;
}
