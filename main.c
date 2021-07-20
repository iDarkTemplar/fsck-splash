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

#include <ply-event-loop.h>
#include <ply-boot-client.h>

typedef struct
{
	ply_event_loop_t *loop;
	ply_boot_client_t *client;
	ply_fd_watch_t *fdwatch;
	FILE *progress_file;
	char *read_string;
	size_t read_string_len;
	int watch_closed;
} state_t;

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

void finish_handler(void *user_data, ply_boot_client_t *client)
{
	state_t *state = (state_t*) user_data;

	ply_event_loop_exit(state->loop, 0);
}

void display_message_success(void *user_data, ply_boot_client_t *client)
{
	// do nothing
}

void display_message_failure(void *user_data, ply_boot_client_t *client)
{
	state_t *state = (state_t*) user_data;

	ply_event_loop_exit(state->loop, 1);
}

void fd_has_data_handler(void *user_data, int source_fd)
{
	ssize_t getline_rc;
	int rc;
	int pass;
	unsigned long cur;
	unsigned long max;
	char *device = NULL;
	char *output_string = NULL;

	state_t *state = (state_t*) user_data;

	getline_rc = getline(&(state->read_string), &(state->read_string_len), state->progress_file);

	if (getline_rc >= 0)
	{
		if (sscanf(state->read_string, "%d %lu %lu %ms", &pass, &cur, &max, &device) == 4)
		{
			rc = asprintf(&output_string, "fsck: device %s, pass %d, %3.1f%% complete...", device, pass, ((double) cur) * 100.0f / ((double) max));
			if (rc > 0)
			{
				ply_boot_client_tell_daemon_to_display_message(state->client, output_string, &display_message_success, &display_message_failure, state);
			}

			free(output_string);
		}

		free(device);
	}
}

void fd_closed_handler(void *user_data, int source_fd)
{
	state_t *state = (state_t*) user_data;

	state->watch_closed = 1;

	ply_boot_client_tell_daemon_to_display_message(state->client, "fsck complete", &finish_handler, &finish_handler, state);
}

void disconnect_handler(void *user_data, ply_boot_client_t *client)
{
	state_t *state = (state_t*) user_data;

	ply_event_loop_exit(state->loop, 0);
}

int main(int argc, char **argv)
{
	int rc;
	int result = 0;
	int progress_pipes[2] = { -1, -1 };
	int control_pipes[2] = { -1, -1 };
	int main_ready_pipes[2] = { -1, -1 };
	char *fdstring = NULL;
	char **child_argv = NULL;
	pid_t child_pid;
	struct pollfd poll_fd;
	int exitcode = -1;
	char parent_status = '0';
	int should_run_failover_fsck = 1;
	bool is_connected = false;
	state_t state = { NULL, NULL, NULL, NULL, NULL, 0, 0 };

	if (argc < 2)
	{
		fprintf(stderr, "USAGE: %s fsck [fsck options]\n", argv[0]);
		return -1;
	}

	rc = pipe(progress_pipes);
	if (rc < 0)
	{
		fprintf(stderr, "pipe() failed with error %d: %s\n", errno, strerror(errno));
		goto error_1;
	}

	state.progress_file = fdopen(progress_pipes[0], "r");
	if (state.progress_file == NULL)
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

	rc = pipe(main_ready_pipes);
	if (rc < 0)
	{
		fprintf(stderr, "pipe() failed with error %d: %s\n", errno, strerror(errno));
		goto error_4;
	}

	rc = asprintf(&fdstring, "-C%d", progress_pipes[1]);
	if (rc < 0)
	{
		fprintf(stderr, "asprintf() failed with error %d: %s\n", errno, strerror(errno));
		goto error_5;
	}

	child_argv = (char**) malloc(sizeof(char*) * (argc + 1));
	if (child_argv == NULL)
	{
		fprintf(stderr, "malloc() failed with error %d: %s\n", errno, strerror(errno));
		goto error_6;
	}

	child_pid = fork();
	if (child_pid == -1)
	{
		fprintf(stderr, "fork() failed with error %d: %s\n", errno, strerror(errno));
		goto error_7;
	}

	if (child_pid == 0)
	{
		fclose(state.progress_file);
		state.progress_file = NULL;

		close(control_pipes[0]);
		control_pipes[0] = -1;

		close(main_ready_pipes[1]);
		main_ready_pipes[1] = -1;

		child_argv[0] = argv[1];
		child_argv[1] = fdstring;

		for (rc = 2, result = 2; rc < argc; )
		{
			if (strcmp(argv[rc], "-C") == 0)
			{
				rc += 2;
			}
			else if (strncmp(argv[rc], "-C", sizeof("-C") - 1) == 0)
			{
				++rc;
			}
			else
			{
				child_argv[result++] = argv[rc++];
			}
		}

		child_argv[result] = NULL;

		poll_fd.events = POLLIN;
		poll_fd.revents = 0;
		poll_fd.fd = main_ready_pipes[0];

		rc = poll(&poll_fd, 1, -1);
		if (rc < 0)
		{
			close(main_ready_pipes[0]);
			goto child_error;
		}

		if (rc == 0)
		{
			close(main_ready_pipes[0]);
			goto child_error;
		}

		if (poll_fd.revents & POLLIN)
		{
			if (read(main_ready_pipes[0], &parent_status, 1) != 1)
			{
				close(main_ready_pipes[0]);
				goto child_error;
			}
		}

		close(main_ready_pipes[0]);

		if (parent_status == '0')
		{
			goto child_error;
		}

		execvp(child_argv[0], child_argv);

child_error:
		// if execvp failed, signal error to parent
		write(control_pipes[1], "E", 1);

		free(child_argv);
		free(fdstring);
		close(progress_pipes[1]);
		close(control_pipes[1]);

		return -1;
	}

	close(progress_pipes[1]);
	progress_pipes[1] = -1;

	close(control_pipes[1]);
	control_pipes[1] = -1;

	close(main_ready_pipes[0]);
	main_ready_pipes[0] = -1;

	free(child_argv);
	child_argv = NULL;

	free(fdstring);
	fdstring = NULL;

	state.loop = ply_event_loop_new();
	if (state.loop == NULL)
	{
		fprintf(stderr, "ply_event_loop_new() failed\n");
		goto error_8;
	}

	state.client = ply_boot_client_new();
	if (state.loop == NULL)
	{
		fprintf(stderr, "ply_event_loop_new() failed\n");
		goto error_9;
	}

	state.fdwatch = ply_event_loop_watch_fd(state.loop, progress_pipes[0], PLY_EVENT_LOOP_FD_STATUS_HAS_DATA, &fd_has_data_handler, &fd_closed_handler, &state);
	if (state.fdwatch == NULL)
	{
		fprintf(stderr, "ply_event_loop_watch_fd() failed\n");
		goto error_10;
	}

	is_connected = ply_boot_client_connect(state.client, &disconnect_handler, &state);
	if (!is_connected)
	{
		fprintf(stderr, "ply_boot_client_connect() failed\n");
		goto error_11;
	}

	ply_boot_client_attach_to_event_loop(state.client, state.loop);

	// signal to child that everything is ready and it should run
	write(main_ready_pipes[1], "1", 1);

	close(main_ready_pipes[1]);
	main_ready_pipes[1] = -1;

	poll_fd.events = POLLIN;
	poll_fd.revents = 0;
	poll_fd.fd = control_pipes[0];

	rc = poll(&poll_fd, 1, -1);
	if (rc < 0)
	{
		fprintf(stderr, "poll() failed with error %d: %s\n", errno, strerror(errno));
		get_child_return_code(child_pid, NULL);
		should_run_failover_fsck = 0;
		goto error_12;
	}

	if (rc == 0)
	{
		fprintf(stderr, "poll() timed out\n");
		get_child_return_code(child_pid, NULL);
		should_run_failover_fsck = 0;
		goto error_12;
	}

	if (poll_fd.revents & POLLIN)
	{
		fprintf(stderr, "Failed to start fsck\n");
		get_child_return_code(child_pid, NULL);
		goto error_12;
	}

	close(control_pipes[0]);
	control_pipes[0] = -1;

	exitcode = ply_event_loop_run(state.loop);

	ply_boot_client_disconnect(state.client);

	if (!state.watch_closed)
	{
		ply_event_loop_stop_watching_fd(state.loop, state.fdwatch);
	}

	ply_boot_client_free(state.client);
	ply_event_loop_free(state.loop);

	free(state.read_string);
	state.read_string = NULL;

	fclose(state.progress_file);
	state.progress_file = NULL;

	get_child_return_code(child_pid, &result);

	return (result != 0) ? result : exitcode;

error_12:
	ply_boot_client_disconnect(state.client);

error_11:
	ply_event_loop_stop_watching_fd(state.loop, state.fdwatch);

error_10:
	ply_boot_client_free(state.client);

error_9:
	ply_event_loop_exit(state.loop, 0);
	ply_event_loop_run(state.loop);
	ply_event_loop_free(state.loop);

error_8:
	// signal to child that initialization failed
	write(main_ready_pipes[1], "0", 1);

error_7:
	free(child_argv);
	child_argv = NULL;

error_6:
	free(fdstring);
	fdstring = NULL;

error_5:
	close(main_ready_pipes[0]);
	main_ready_pipes[0] = -1;
	close(main_ready_pipes[1]);
	main_ready_pipes[1] = -1;

error_4:
	close(control_pipes[0]);
	control_pipes[0] = -1;
	close(control_pipes[1]);
	control_pipes[1] = -1;

error_3:
	if (state.progress_file != NULL)
	{
		fclose(state.progress_file);
		state.progress_file = NULL;
		progress_pipes[0] = -1;
	}

error_2:
	close(progress_pipes[0]);
	progress_pipes[0] = -1;
	close(progress_pipes[1]);
	progress_pipes[1] = -1;

error_1:
	if (should_run_failover_fsck)
	{
		// if everything failed, just try running plain fsck without plymouth wrapper
		execvp(argv[1], argv + 1);
	}

	// if execvp failed or skipped, just return error. Nothing can be done anymore
	return -1;
}
