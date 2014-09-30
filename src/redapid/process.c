/*
 * redapid
 * Copyright (C) 2014 Matthias Bolte <matthias@tinkerforge.com>
 *
 * process.c: Process object implementation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define _GNU_SOURCE // for execvpe from unistd.h

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <daemonlib/event.h>
#include <daemonlib/log.h>
#include <daemonlib/utils.h>

#include "process.h"

#include "api.h"
#include "file.h"
#include "list.h"
#include "inventory.h"
#include "string.h"

#define LOG_CATEGORY LOG_CATEGORY_API

typedef struct {
	ProcessState state;
	uint64_t timestamp;
	uint8_t exit_code;
} ProcessStateChange;

static bool process_state_is_alive(ProcessState state) {
	switch (state) {
	default:
	case PROCESS_STATE_UNKNOWN: return true;
	case PROCESS_STATE_RUNNING: return true;
	case PROCESS_STATE_ERROR:   return false;
	case PROCESS_STATE_EXITED:  return false;
	case PROCESS_STATE_KILLED:  return false;
	case PROCESS_STATE_STOPPED: return true;
	}
}

static void process_destroy(Object *object) {
	Process *process = (Process *)object;
	int rc;
	bool stuck = false;

	// remove the state change pipe from the event loop to avoid sending
	// callbacks in case the child process is still alive and has to be killed
	event_remove_source(process->state_change_pipe.read_end, EVENT_SOURCE_TYPE_GENERIC);

	// FIXME: this code here has the same race condition as process_kill
	if (process_state_is_alive(process->state)) {
		log_warn("Destroying process object (id: %u, executable: %s) while child process (pid: %u) is still alive",
		         process->base.id, process->executable->buffer, process->pid);

		rc = kill(process->pid, SIGKILL);

		if (rc < 0) {
			if (errno != ESRCH) {
				stuck = true;
			}

			log_error("Could not send SIGKILL signal to child process (executable: %s, pid: %u): %s (%d)",
			          process->executable->buffer, process->pid, get_errno_name(errno), errno);
		}
	}

	if (!stuck) {
		thread_join(&process->wait_thread);
	}

	thread_destroy(&process->wait_thread);

	pipe_destroy(&process->state_change_pipe);

	file_vacate(process->stderr);
	file_vacate(process->stdout);
	file_vacate(process->stdin);
	string_vacate(process->working_directory);
	list_vacate(process->environment);
	list_vacate(process->arguments);
	string_vacate(process->executable);

	free(process);
}

static void process_wait(void *opaque) {
	Process *process = opaque;
	int status;
	int rc;
	ProcessStateChange change;

	for (;;) {
		do {
			rc = waitpid(process->pid, &status, WUNTRACED | WCONTINUED);
		} while (rc < 0 && errno_interrupted());

		if (rc < 0) {
			log_error("Could not wait for child process (executable: %s, pid: %u) state change: %s (%d)",
			          process->executable->buffer, process->pid, get_errno_name(errno), errno);

			break;
		}

		change.timestamp = time(NULL);

		if (WIFEXITED(status)) {
			change.state = PROCESS_STATE_EXITED;
			change.exit_code = WEXITSTATUS(status);

			// the child process has limited capabilities to report errors. the
			// coreutils env executable that executes other programs reserves
			// three exit codes to report errors (125, 126 and 127). our child
			// process uses the same mechanism. check for these three exit codes
			// and change state to error if found. the downside of this approach
			// is that these three exit codes can be used by the program to be
			// executed as normal exit codes with a different meaning, leading
			// to a misinterpretation here. but the coreutils env executable has
			// the same problem, so we will live with this
			if (change.exit_code == PROCESS_ERROR_CODE_INTERNAL_ERROR ||
			    change.exit_code == PROCESS_ERROR_CODE_CANNOT_EXECUTE ||
			    change.exit_code == PROCESS_ERROR_CODE_DOES_NOT_EXIST) {
				change.state = PROCESS_STATE_ERROR;
			}
		} else if (WIFSIGNALED(status)) {
			change.state = PROCESS_STATE_KILLED;
			change.exit_code = WTERMSIG(status);
		} else if (WIFSTOPPED(status)) {
			change.state = PROCESS_STATE_STOPPED;
			change.exit_code = WSTOPSIG(status);
		} else if (WIFCONTINUED(status)) {
			change.state = PROCESS_STATE_RUNNING;
			change.exit_code = 0; // invalid
		} else {
			change.state = PROCESS_STATE_UNKNOWN;
			change.exit_code = 0; // invalid
		}

		log_debug("State of child process (executable: %s, pid: %u) changed (state: %u, exit_code: %u)",
		          process->executable->buffer, process->pid, change.state, change.exit_code);

		if (pipe_write(&process->state_change_pipe, &change, sizeof(change)) < 0) {
			log_error("Could not write to state change pipe for child process (executable: %s, pid: %u): %s (%d)",
			          process->executable->buffer, process->pid, get_errno_name(errno), errno);

			break;
		}

		if (!process_state_is_alive(change.state)) {
			break;
		}
	}
}

static void process_handle_state_change(void *opaque) {
	Process *process = opaque;
	ProcessStateChange change;

	if (pipe_read(&process->state_change_pipe, &change, sizeof(change)) < 0) {
		log_error("Could not read from state change pipe for child process (executable: %s, pid: %u): %s (%d)",
		          process->executable->buffer, process->pid, get_errno_name(errno), errno);

		return;
	}

	process->state = change.state;
	process->timestamp = change.timestamp;
	process->exit_code = change.exit_code;

	if (!process_state_is_alive(process->state)) {
		process->pid = 0;
	}

	// only send a process-state-changed callback if there is at least one
	// external reference to the process object. otherwise there is no one that
	// could be interested in this callback anyway. also this logic avoids
	// sending process-state-changed callbacks for scheduled program executions
	if (process->base.external_reference_count > 0) {
		api_send_process_state_changed_callback(process->base.id, change.state,
		                                        change.timestamp, process->pid,
		                                        change.exit_code);
	}

	if (!process_state_is_alive(process->state)) {
		object_remove_internal_reference(&process->base);
	}
}

APIE process_fork(pid_t *pid) {
	sigset_t oldmask, newmask;
	struct sigaction action;
	int i;

	// block signals now, so that child process can safely disable caller's
	// signal handlers without a race
	sigfillset(&newmask);

	if (pthread_sigmask(SIG_SETMASK, &newmask, &oldmask) != 0) {
		log_error("Could not block signals: %s (%d)",
		          get_errno_name(errno), errno);

		return API_E_INTERNAL_ERROR;
	}

	// ensure to hold the logging mutex, to protect child processes
	// from deadlocking on another thread's inherited mutex state
	log_lock();

	*pid = fork();

	// unlock for both parent and child process
	log_unlock();

	if (*pid < 0) { // error
		pthread_sigmask(SIG_SETMASK, &oldmask, NULL);

		log_error("Could not fork child process: %s (%d)",
		          get_errno_name(errno), errno);

		return API_E_INTERNAL_ERROR;
	} else if (*pid != 0) { // parent
		pthread_sigmask(SIG_SETMASK, &oldmask, NULL);

		return API_E_SUCCESS;
	} else { // child
		// reset all signal handlers from parent so nothing unexpected can
		// happen in the child once signals are unblocked
		action.sa_handler = SIG_DFL;
		action.sa_flags = 0;

		sigemptyset(&action.sa_mask);

		for (i = 1; i < NSIG; ++i) {
			sigaction(i, &action, NULL);
		}

		// unblock all signals in the child
		sigemptyset(&newmask);

		if (pthread_sigmask(SIG_SETMASK, &newmask, NULL) != 0) {
			log_error("Could not unblock signals: %s (%d)",
			          get_errno_name(errno), errno);

			_exit(PROCESS_ERROR_CODE_INTERNAL_ERROR);
		}

		return API_E_SUCCESS;
	}
}

// public API
APIE process_spawn(ObjectID executable_id, ObjectID arguments_id,
                   ObjectID environment_id, ObjectID working_directory_id,
                   uint32_t uid, uint32_t gid, ObjectID stdin_id,
                   ObjectID stdout_id, ObjectID stderr_id,
                   uint16_t object_create_flags,
                   ObjectID *id, Process **object) {
	int phase = 0;
	APIE error_code;
	String *executable;
	List *arguments;
	Array arguments_array;
	int i;
	char **item;
	List *environment;
	Array environment_array;
	String *working_directory;
	File *stdin;
	File *stdout;
	File *stderr;
	pid_t pid;
	int status_pipe[2];
	int sc_open_max;
	FILE *log_file;
	Process *process;

	// occupy executable string object
	error_code = string_occupy(executable_id, &executable);

	if (error_code != API_E_SUCCESS) {
		goto cleanup;
	}

	phase = 1;

	// occupy arguments list object
	error_code = list_occupy(arguments_id, OBJECT_TYPE_STRING, &arguments);

	if (error_code != API_E_SUCCESS) {
		goto cleanup;
	}

	phase = 2;

	// prepare arguments array for execvpe
	if (array_create(&arguments_array, 1 + arguments->items.count + 1, sizeof(char *), true) < 0) {
		error_code = api_get_error_code_from_errno();

		log_error("Could not create arguments array for spawning child process (executable: %s): %s (%d)",
		          executable->buffer, get_errno_name(errno), errno);

		goto cleanup;
	}

	phase = 3;

	item = array_append(&arguments_array);

	if (item == NULL) {
		error_code = api_get_error_code_from_errno();

		log_error("Could not append to arguments array for spawning child process (executable: %s): %s (%d)",
		          executable->buffer, get_errno_name(errno), errno);

		goto cleanup;
	}

	*item = executable->buffer;

	for (i = 0; i < arguments->items.count; ++i) {
		item = array_append(&arguments_array);

		if (item == NULL) {
			error_code = api_get_error_code_from_errno();

			log_error("Could not append to arguments array for spawning child process (executable: %s): %s (%d)",
			          executable->buffer, get_errno_name(errno), errno);

			goto cleanup;
		}

		*item = (*(String **)array_get(&arguments->items, i))->buffer;
	}

	item = array_append(&arguments_array);

	if (item == NULL) {
		error_code = api_get_error_code_from_errno();

		log_error("Could not append to arguments array for spawning child process (executable: %s): %s (%d)",
		          executable->buffer, get_errno_name(errno), errno);

		goto cleanup;
	}

	*item = NULL;

	// occupy environment list object
	error_code = list_occupy(environment_id, OBJECT_TYPE_STRING, &environment);

	if (error_code != API_E_SUCCESS) {
		goto cleanup;
	}

	phase = 4;

	// prepare environment array for execvpe
	if (array_create(&environment_array, environment->items.count + 1, sizeof(char *), true) < 0) {
		error_code = api_get_error_code_from_errno();

		log_error("Could not create environment array for spawning child process (executable: %s): %s (%d)",
		          executable->buffer, get_errno_name(errno), errno);

		goto cleanup;
	}

	phase = 5;

	for (i = 0; i < environment->items.count; ++i) {
		item = array_append(&environment_array);

		if (item == NULL) {
			error_code = api_get_error_code_from_errno();

			log_error("Could not append to environment array for spawning child process (executable: %s): %s (%d)",
			          executable->buffer, get_errno_name(errno), errno);

			goto cleanup;
		}

		// FIXME: if item ist not <name>=<value>, but just <name> then use the parent <value>

		*item = (*(String **)array_get(&environment->items, i))->buffer;
	}

	item = array_append(&environment_array);

	if (item == NULL) {
		error_code = api_get_error_code_from_errno();

		log_error("Could not append to environment array for spawning child process (executable: %s): %s (%d)",
		          executable->buffer, get_errno_name(errno), errno);

		goto cleanup;
	}

	*item = NULL;

	// occupy working directory string object
	error_code = string_occupy(working_directory_id, &working_directory);

	if (error_code != API_E_SUCCESS) {
		goto cleanup;
	}

	phase = 6;

	// occupy stdin file object
	error_code = file_occupy(stdin_id, &stdin);

	if (error_code != API_E_SUCCESS) {
		goto cleanup;
	}

	phase = 7;

	// occupy stdout file object
	error_code = file_occupy(stdout_id, &stdout);

	if (error_code != API_E_SUCCESS) {
		goto cleanup;
	}

	phase = 8;

	// occupy stderr file object
	error_code = file_occupy(stderr_id, &stderr);

	if (error_code != API_E_SUCCESS) {
		goto cleanup;
	}

	phase = 9;

	// create status pipe
	if (pipe(status_pipe) < 0) {
		error_code = api_get_error_code_from_errno();

		log_error("Could not create status pipe for spawning child process (executable: %s): %s (%d)",
		          executable->buffer, get_errno_name(errno), errno);

		goto cleanup;
	}

	phase = 10;

	// fork
	log_debug("Forking to spawn child process (executable: %s)", executable->buffer);

	error_code = process_fork(&pid);

	if (error_code != API_E_SUCCESS) {
		goto cleanup;
	}

	if (pid == 0) { // child
		close(status_pipe[0]);

		// change group
		if (setregid(gid, gid) < 0) {
			error_code = api_get_error_code_from_errno();

			log_error("Could not change to group %u for child process (executable: %s, pid: %u): %s (%d)",
			          gid, executable->buffer, getpid(), get_errno_name(errno), errno);

			goto child_error;
		}

		// change user
		if (setreuid(uid, uid) < 0) {
			error_code = api_get_error_code_from_errno();

			log_error("Could not change to user %u for child process (executable: %s, pid: %u): %s (%d)",
			          uid, executable->buffer, getpid(), get_errno_name(errno), errno);

			goto child_error;
		}

		// change directory
		if (chdir(working_directory->buffer) < 0) {
			error_code = api_get_error_code_from_errno();

			log_error("Could not change directory to '%s' for child process (executable: %s, pid: %u): %s (%d)",
			          working_directory->buffer, executable->buffer, getpid(), get_errno_name(errno), errno);

			goto child_error;
		}

		// get open FD limit
		sc_open_max = sysconf(_SC_OPEN_MAX);

		if (sc_open_max < 0) {
			error_code = api_get_error_code_from_errno();

			log_error("Could not get SC_OPEN_MAX value: %s (%d)",
			          get_errno_name(errno), errno);

			goto child_error;
		}

		// redirect stdin
		if (dup2(file_get_read_handle(stdin), STDIN_FILENO) != STDIN_FILENO) {
			error_code = api_get_error_code_from_errno();

			log_error("Could not redirect stdin for child process (executable: %s, pid: %u): %s (%d)",
			          executable->buffer, getpid(), get_errno_name(errno), errno);

			goto child_error;
		}

		// redirect stdout
		if (dup2(file_get_write_handle(stdout), STDOUT_FILENO) != STDOUT_FILENO) {
			error_code = api_get_error_code_from_errno();

			log_error("Could not redirect stdout for child process (executable: %s, pid: %u): %s (%d)",
			          executable->buffer, getpid(), get_errno_name(errno), errno);

			goto child_error;
		}

		// stderr is the default log output in non-daemon mode. if this is
		// the case then disable the log output before redirecting stderr to
		// avoid polluting stderr for the new process
		log_file = log_get_file();

		if (log_file != NULL && fileno(log_file) == STDERR_FILENO) {
			log_debug("Disable logging to stderr for child process (executable: %s, pid: %u)",
			          executable->buffer, getpid());

			log_set_file(NULL);
		}

		// redirect stderr
		if (dup2(file_get_write_handle(stderr), STDERR_FILENO) != STDERR_FILENO) {
			error_code = api_get_error_code_from_errno();

			log_error("Could not redirect stderr for child process (executable: %s, pid: %u): %s (%d)",
			          executable->buffer, getpid(), get_errno_name(errno), errno);

			goto child_error;
		}

		// notify parent
		if (robust_write(status_pipe[1], &error_code, sizeof(error_code)) < 0) {
			error_code = api_get_error_code_from_errno();

			log_error("Could not write to status pipe for child process (executable: %s, pid: %u): %s (%d)",
			          executable->buffer, getpid(), get_errno_name(errno), errno);

			goto child_error;
		}

		// disable log output. if stderr was not the current log output then
		// the log file is still open at this point. the next step is to close
		// all remaining file descriptors. just for good measure disable the
		// log output beforehand
		log_set_file(NULL);

		// close all file descriptors except the std* ones
		for (i = STDERR_FILENO + 1; i < sc_open_max; ++i) {
			close(i);
		}

		// execvpe only returns in case of an error
		execvpe(executable->buffer, (char **)arguments_array.bytes, (char **)environment_array.bytes);

		if (errno == ENOENT) {
			_exit(PROCESS_ERROR_CODE_DOES_NOT_EXIST);
		} else {
			_exit(PROCESS_ERROR_CODE_CANNOT_EXECUTE);
		}

	child_error:
		// notify parent in all cases
		if (robust_write(status_pipe[1], &error_code, sizeof(error_code)) < 0) {
			error_code = api_get_error_code_from_errno();

			log_error("Could not write to status pipe for child process (executable: %s, pid: %u): %s (%d)",
			          executable->buffer, getpid(), get_errno_name(errno), errno);
		}

		close(status_pipe[1]);

		_exit(PROCESS_ERROR_CODE_INTERNAL_ERROR);
	}

	phase = 11;

	// wait for child to start successfully
	if (robust_read(status_pipe[0], &error_code, sizeof(error_code)) < 0) {
		error_code = api_get_error_code_from_errno();

		log_error("Could not read from status pipe for child process (executable: %s, pid: %u): %s (%d)",
		          executable->buffer, pid, get_errno_name(errno), errno);

		goto cleanup;
	}

	if (error_code != API_E_SUCCESS) {
		goto cleanup;
	}

	// create process object
	process = calloc(1, sizeof(Process));

	if (process == NULL) {
		error_code = API_E_NO_FREE_MEMORY;

		log_error("Could not allocate process object: %s (%d)",
		          get_errno_name(ENOMEM), ENOMEM);

		goto cleanup;
	}

	phase = 12;

	// setup process object
	process->executable = executable;
	process->arguments = arguments;
	process->environment = environment;
	process->working_directory = working_directory;
	process->uid = uid;
	process->gid = gid;
	process->stdin = stdin;
	process->stdout = stdout;
	process->stderr = stderr;
	process->state = PROCESS_STATE_RUNNING;
	process->timestamp = time(NULL);
	process->pid = pid;
	process->exit_code = 0; // invalid

	if (pipe_create(&process->state_change_pipe, 0) < 0) {
		error_code = api_get_error_code_from_errno();

		log_error("Could not create state change pipe child process (executable: %s, pid: %u): %s (%d)",
		          executable->buffer, pid, get_errno_name(errno), errno);

		goto cleanup;
	}

	phase = 13;

	if (event_add_source(process->state_change_pipe.read_end, EVENT_SOURCE_TYPE_GENERIC,
	                     EVENT_READ, process_handle_state_change, process) < 0) {
		goto cleanup;
	}

	phase = 14;

	// create process object
	error_code = object_create(&process->base, OBJECT_TYPE_PROCESS,
	                           object_create_flags, process_destroy);

	if (error_code != API_E_SUCCESS) {
		goto cleanup;
	}

	if (id != NULL) {
		*id = process->base.id;
	}

	if (object != NULL) {
		*object = process;
	}

	// start thread to wait for child process state changes
	thread_create(&process->wait_thread, process_wait, process);

	log_debug("Spawned process object (id: %u, executable: %s, pid: %u)",
	          process->base.id, executable->buffer, process->pid);

	phase = 15;

	close(status_pipe[0]);
	close(status_pipe[1]);
	array_destroy(&arguments_array, NULL);
	array_destroy(&environment_array, NULL);

cleanup:
	switch (phase) { // no breaks, all cases fall through intentionally
	case 14:
		event_remove_source(process->state_change_pipe.read_end, EVENT_SOURCE_TYPE_GENERIC);

	case 13:
		pipe_destroy(&process->state_change_pipe);

	case 12:
		free(process);

	case 11:
		kill(pid, SIGKILL);

	case 10:
		close(status_pipe[0]);
		close(status_pipe[1]);

	case 9:
		file_vacate(stderr);

	case 8:
		file_vacate(stdout);

	case 7:
		file_vacate(stdin);

	case 6:
		string_vacate(working_directory);

	case 5:
		array_destroy(&environment_array, NULL);

	case 4:
		list_vacate(environment);

	case 3:
		array_destroy(&arguments_array, NULL);

	case 2:
		list_vacate(arguments);

	case 1:
		string_vacate(executable);

	default:
		break;
	}

	return phase == 15 ? API_E_SUCCESS : error_code;
}

// public API
APIE process_kill(Process *process, ProcessSignal signal) {
	int rc;
	APIE error_code;

	// FIXME: here is a race condition, because the child process might already
	//        be dead at this point, but the process state didn't get updated
	//        yet. this can result in trying to kill a process that's not
	//        existing anymore. or even worse, the process ID has already been
	//        reused and an unrelated process gets killed here
	if (!process_state_is_alive(process->state)) {
		log_warn("Cannot send signal (number: %d) to an already dead child process (executable: %s)",
		         signal, process->executable->buffer);

		return API_E_INVALID_OPERATION;
	}

	rc = kill(process->pid, signal);

	if (rc < 0) {
		error_code = api_get_error_code_from_errno();

		log_warn("Could not send signal (number: %d) to child process (executable: %s, pid: %u): %s (%d)",
		         signal, process->executable->buffer, process->pid, get_errno_name(errno), errno);

		return error_code;
	}

	return API_E_SUCCESS;
}

// public API
APIE process_get_command(Process *process, ObjectID *executable_id,
                         ObjectID *arguments_id, ObjectID *environment_id,
                         ObjectID *working_directory_id) {
	object_add_external_reference(&process->executable->base);
	object_add_external_reference(&process->arguments->base);
	object_add_external_reference(&process->environment->base);
	object_add_external_reference(&process->working_directory->base);

	*executable_id = process->executable->base.id;
	*arguments_id = process->arguments->base.id;
	*environment_id = process->environment->base.id;
	*working_directory_id = process->working_directory->base.id;

	return API_E_SUCCESS;
}

// public API
APIE process_get_identity(Process *process, uint32_t *uid, uint32_t *gid) {
	*uid = process->uid;
	*gid = process->gid;

	return API_E_SUCCESS;
}

// public API
APIE process_get_stdio(Process *process, ObjectID *stdin_id, ObjectID *stdout_id,
                       ObjectID *stderr_id) {
	object_add_external_reference(&process->stdin->base);
	object_add_external_reference(&process->stdout->base);
	object_add_external_reference(&process->stderr->base);

	*stdin_id = process->stdin->base.id;
	*stdout_id = process->stdout->base.id;
	*stderr_id = process->stderr->base.id;

	return API_E_SUCCESS;
}

// public API
APIE process_get_state(Process *process, uint8_t *state, uint64_t *timestamp,
                       uint32_t *pid, uint8_t *exit_code) {
	*state = process->state;
	*timestamp = process->timestamp;
	*pid = process->pid;
	*exit_code = process->exit_code;

	return API_E_SUCCESS;
}
