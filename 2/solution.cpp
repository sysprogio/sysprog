#include "parser.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

struct pipeline_result {
	int status = 0;
	bool exit_shell = false;
};

static int
status_from_wait(int st)
{
	if (WIFEXITED(st))
		return WEXITSTATUS(st);
	if (WIFSIGNALED(st))
		return 128 + WTERMSIG(st);
	return 1;
}

static int
open_output_fd(const command_line &line)
{
	int flags = O_WRONLY | O_CREAT;
	if (line.out_type == OUTPUT_TYPE_FILE_NEW)
		flags |= O_TRUNC;
	else
		flags |= O_APPEND;
	return open(line.out_file.c_str(), flags, 0666);
}

static int
builtin_cd(const command &cmd)
{
	const char *path = nullptr;
	if (cmd.args.empty()) {
		path = getenv("HOME");
	} else {
		path = cmd.args[0].c_str();
	}
	if (path == nullptr)
		return 1;
	if (chdir(path) != 0) {
		perror("cd");
		return 1;
	}
	return 0;
}

static int
builtin_exit_code(const command &cmd, int last_status)
{
	if (cmd.args.empty())
		return last_status;
	char *end = nullptr;
	long code = strtol(cmd.args[0].c_str(), &end, 10);
	if (end == cmd.args[0].c_str() || *end != '\0')
		return 1;
	return (int)(code & 0xFF);
}

static std::vector<char *>
make_argv(const command &cmd)
{
	std::vector<char *> argv;
	argv.reserve(cmd.args.size() + 2);
	argv.push_back(const_cast<char *>(cmd.exe.c_str()));
	for (const auto &arg : cmd.args)
		argv.push_back(const_cast<char *>(arg.c_str()));
	argv.push_back(nullptr);
	return argv;
}

static pipeline_result
run_pipeline_segment(const std::vector<command> &cmds, const command_line &line,
    bool is_last_pipeline, bool allow_exit, int last_status, int input_fd,
    int output_fd_override)
{
	pipeline_result result{};

	if (cmds.size() == 1) {
		const command &cmd = cmds[0];
		if (cmd.exe == "exit" && allow_exit && line.out_type == OUTPUT_TYPE_STDOUT) {
			result.status = builtin_exit_code(cmd, last_status);
			result.exit_shell = true;
			return result;
		}
		if (cmd.exe == "cd") {
			int saved_stdout = -1;
			int fd = -1;
			if (output_fd_override >= 0) {
				saved_stdout = dup(STDOUT_FILENO);
				dup2(output_fd_override, STDOUT_FILENO);
			} else if (is_last_pipeline && line.out_type != OUTPUT_TYPE_STDOUT) {
				fd = open_output_fd(line);
				if (fd < 0) {
					perror("open");
					result.status = 1;
					return result;
				}
				saved_stdout = dup(STDOUT_FILENO);
				dup2(fd, STDOUT_FILENO);
				close(fd);
			}
			result.status = builtin_cd(cmd);
			if (saved_stdout != -1) {
				dup2(saved_stdout, STDOUT_FILENO);
				close(saved_stdout);
			}
			return result;
		}
	}

	std::vector<pid_t> pids;
	int current_input = input_fd;
	for (size_t i = 0; i < cmds.size(); ++i) {
		int pipefd[2] = {-1, -1};
		if (i + 1 < cmds.size()) {
			if (pipe(pipefd) != 0) {
				perror("pipe");
				result.status = 1;
				return result;
			}
		}
		pid_t pid = fork();
		if (pid == 0) {
			if (current_input != STDIN_FILENO) {
				dup2(current_input, STDIN_FILENO);
			}
			if (pipefd[1] != -1) {
				dup2(pipefd[1], STDOUT_FILENO);
			} else if (output_fd_override >= 0) {
				dup2(output_fd_override, STDOUT_FILENO);
			} else if (is_last_pipeline && line.out_type != OUTPUT_TYPE_STDOUT) {
				int fd = open_output_fd(line);
				if (fd < 0) {
					perror("open");
					_exit(1);
				}
				dup2(fd, STDOUT_FILENO);
				close(fd);
			}
			if (pipefd[0] != -1)
				close(pipefd[0]);
			if (pipefd[1] != -1)
				close(pipefd[1]);
			if (current_input != STDIN_FILENO)
				close(current_input);
			if (output_fd_override >= 0)
				close(output_fd_override);

			const command &cmd = cmds[i];
			if (cmd.exe == "cd") {
				int rc = builtin_cd(cmd);
				_exit(rc);
			}
			if (cmd.exe == "exit") {
				int code = builtin_exit_code(cmd, last_status);
				_exit(code);
			}
			std::vector<char *> argv = make_argv(cmd);
			execvp(argv[0], argv.data());
			perror("execvp");
			_exit(127);
		}
		if (pid < 0) {
			perror("fork");
			result.status = 1;
			if (pipefd[0] != -1)
				close(pipefd[0]);
			if (pipefd[1] != -1)
				close(pipefd[1]);
			if (current_input != STDIN_FILENO)
				close(current_input);
			return result;
		}
		pids.push_back(pid);
		if (current_input != STDIN_FILENO)
			close(current_input);
		if (pipefd[1] != -1)
			close(pipefd[1]);
		current_input = pipefd[0];
	}
	if (current_input != STDIN_FILENO && current_input != -1)
		close(current_input);

	int st = 0;
	for (size_t i = 0; i < pids.size(); ++i) {
		int tmp = 0;
		waitpid(pids[i], &tmp, 0);
		if (i + 1 == pids.size())
			st = tmp;
	}
	result.status = status_from_wait(st);
	return result;
}

static pipeline_result
run_pipeline(const std::vector<command> &cmds, const command_line &line,
    bool is_last_pipeline, bool allow_exit, int last_status)
{
	const size_t chunk_limit = 200;
	if (cmds.size() <= chunk_limit) {
		return run_pipeline_segment(cmds, line, is_last_pipeline, allow_exit,
		    last_status, STDIN_FILENO, -1);
	}

	pipeline_result res{};
	size_t pos = 0;
	int input_fd = STDIN_FILENO;
	int status_acc = last_status;
	while (pos < cmds.size()) {
		size_t end = pos + chunk_limit;
		if (end > cmds.size())
			end = cmds.size();
		bool last_chunk = (end == cmds.size());

		char tmpl[] = "/tmp/mybashXXXXXX";
		int tmp_fd = -1;
		if (!last_chunk) {
			tmp_fd = mkstemp(tmpl);
			if (tmp_fd < 0) {
				perror("mkstemp");
				if (input_fd != STDIN_FILENO)
					close(input_fd);
				res.status = 1;
				return res;
			}
		}

		std::vector<command> segment(cmds.begin() + (long)pos, cmds.begin() + (long)end);
		res = run_pipeline_segment(segment, line,
		    is_last_pipeline && last_chunk, allow_exit, status_acc, input_fd,
		    tmp_fd);
		status_acc = res.status;
		if (res.exit_shell) {
			if (input_fd != STDIN_FILENO)
				close(input_fd);
			if (!last_chunk && tmp_fd != -1)
				close(tmp_fd);
			return res;
		}

		if (!last_chunk) {
			if (input_fd != STDIN_FILENO)
				close(input_fd);
			lseek(tmp_fd, 0, SEEK_SET);
			input_fd = tmp_fd;
			unlink(tmpl);
		} else {
			if (tmp_fd != -1)
				close(tmp_fd);
			if (input_fd != STDIN_FILENO)
				close(input_fd);
		}
		pos = end;
	}
	return res;
}

static void
split_pipelines(const command_line *line, std::vector<std::vector<command>> &pipelines,
    std::vector<expr_type> &ops)
{
	auto it = line->exprs.begin();
	while (it != line->exprs.end()) {
		std::vector<command> current;
		assert(it->type == EXPR_TYPE_COMMAND);
		current.push_back(*it->cmd);
		++it;
		while (it != line->exprs.end() && it->type == EXPR_TYPE_PIPE) {
			++it;
			assert(it != line->exprs.end());
			assert(it->type == EXPR_TYPE_COMMAND);
			current.push_back(*it->cmd);
			++it;
		}
		pipelines.push_back(std::move(current));
		if (it != line->exprs.end()) {
			assert(it->type == EXPR_TYPE_AND || it->type == EXPR_TYPE_OR);
			ops.push_back(it->type);
			++it;
		}
	}
}

static void
reap_background(std::vector<pid_t> &background)
{
	for (size_t i = 0; i < background.size();) {
		int st = 0;
		pid_t res = waitpid(background[i], &st, WNOHANG);
		if (res == 0) {
			++i;
		} else {
			background.erase(background.begin() + i);
		}
	}
}

static bool
execute_sequence(const command_line *line, int &last_status, bool allow_exit)
{
	std::vector<std::vector<command>> pipelines;
	std::vector<expr_type> ops;
	split_pipelines(line, pipelines, ops);

	int current_status = last_status;
	for (size_t i = 0; i < pipelines.size(); ++i) {
		bool should_run = true;
		if (i > 0) {
			if (ops[i - 1] == EXPR_TYPE_AND && current_status != 0)
				should_run = false;
			if (ops[i - 1] == EXPR_TYPE_OR && current_status == 0)
				should_run = false;
		}
		if (!should_run)
			continue;
		bool is_last = (i + 1 == pipelines.size());
		pipeline_result res = run_pipeline(pipelines[i], *line, is_last, allow_exit, current_status);
		current_status = res.status;
		if (res.exit_shell) {
			last_status = current_status;
			return true;
		}
	}
	last_status = current_status;
	return false;
}

static bool
execute_command_line(const struct command_line *line, int &last_status,
    std::vector<pid_t> &background_pids)
{
	if (line->is_background) {
		pid_t pid = fork();
		if (pid == 0) {
			int child_status = last_status;
			execute_sequence(line, child_status, false);
			_exit(child_status);
		}
		if (pid < 0) {
			perror("fork");
			last_status = 1;
			return false;
		}
		background_pids.push_back(pid);
		last_status = 0;
		return false;
	}
	return execute_sequence(line, last_status, true);
}

int
main(void)
{
	const size_t buf_size = 1024;
	char buf[buf_size];
	int rc;
	struct parser *p = parser_new();
	int last_status = 0;
	std::vector<pid_t> background_pids;
	while ((rc = read(STDIN_FILENO, buf, buf_size)) > 0) {
		parser_feed(p, buf, rc);
		struct command_line *line = NULL;
		while (true) {
			enum parser_error err = parser_pop_next(p, &line);
			if (err == PARSER_ERR_NONE && line == NULL)
				break;
			if (err != PARSER_ERR_NONE) {
				printf("Error: %d\n", (int)err);
				continue;
			}
			bool should_exit = execute_command_line(line, last_status, background_pids);
			delete line;
			reap_background(background_pids);
			if (should_exit) {
				parser_delete(p);
				return last_status;
			}
		}
		reap_background(background_pids);
	}
	parser_delete(p);
	reap_background(background_pids);
	return last_status;
}
