#include "sh61.hh"
#include <cstring>
#include <cerrno>
#include <vector>
#include <sys/stat.h>
#include <sys/wait.h>

// For the love of God
#undef exit
#define exit __DO_NOT_CALL_EXIT__READ_PROBLEM_SET_DESCRIPTION__


// struct command
//    Data structure describing a command. Add your own stuff.

struct command {
    std::vector<std::string> args;
    pid_t pid = -1;      // process ID running this command, -1 if none
    int pipe_in = -1;
    int pipe_out = -1;
    int exit_status;
    command();
    ~command();

    void run(bool);
};


// command::command()
//    This constructor function initializes a `command` structure. You may
//    add stuff to it as you grow the command structure.

command::command() {
}


// command::~command()
//    This destructor function is called to delete a command.

command::~command() {
}


// COMMAND EXECUTION

// command::run()
//    Creates a single child process running the command in `this`, and
//    sets `this->pid` to the pid of the child process.
//
//    If a child process cannot be created, this function should call
//    `_exit(EXIT_FAILURE)` (that is, `_exit(1)`) to exit the containing
//    shell or subshell. If this function returns to its caller,
//    `this->pid > 0` must always hold.
//
//    Note that this function must return to its caller *only* in the parent
//    process. The code that runs in the child process must `execvp` and/or
//    `_exit`.
//
//    PHASE 1: Fork a child process and run the command using `execvp`.
//       This will require creating a vector of `char*` arguments using
//       `this->args[N].c_str()`. Note that the last element of the vector
//       must be a `nullptr`.
//    PHASE 4: Set up a pipeline if appropriate. This may require creating a
//       new pipe (`pipe` system call), and/or replacing the child process's
//       standard input/output with parts of the pipe (`dup2` and `close`).
//       Draw pictures!
//    PHASE 7: Handle redirections.

void command::run(bool wait_for_completion = true) {
    assert(this->pid == -1);
    assert(this->args.size() > 0);
    
    // create a vector of char* arguments
    char* c_args[this->args.size() + 1];
    for (size_t i = 0; i < args.size(); i++){
        c_args[i] = const_cast<char*> (this->args[i].c_str());
    }
    c_args[this->args.size()] = nullptr;

    // create the child program using fork and return the pid to the parent
    pid_t p = fork();
    assert (p>=0);

    // within the child run execv using the array of args
    if (p == 0){

        // redirect input to std in
        if (this -> pipe_in != -1){
            dup2(this -> pipe_in, 0);
            close (this -> pipe_in);
        }

        // redirect ouput to std out
        if (this -> pipe_out != -1){
            dup2(this -> pipe_out, 1);
            close (this -> pipe_out);
        }

        // now we call execv because the environment is set up properly
        int r = execvp (c_args[0], c_args);
        fprintf(stderr, "Error using execvp: pid %d, status %d\n", getpid(), r);
        _exit(EXIT_FAILURE);

    }else {
        // set this -> pid as the child process
        assert(p > 0);
        this -> pid = p;

        // // close pipes in parent
        if (pipe_in != -1) close (this -> pipe_in);
        if (pipe_out != -1) close (this -> pipe_out);

        // wait for child to exit and check its status
        if (wait_for_completion){
            int status;
            pid_t exited_pid = waitpid(p, &status, 0);
            assert(exited_pid == p);

            if (WIFEXITED(status)) {
                this -> exit_status = WEXITSTATUS(status);
            } 
        }
        

    }
}


// run_list(c)
//    Run the command *list* contained in `section`.
//
//    PHASE 1: Use `waitpid` to wait for the command started by `c->run()`
//        to finish.
//
//    The remaining phases may require that you introduce helper functions
//    (e.g., to process a pipeline), write code in `command::run`, and/or
//    change `struct command`.
//
//    It is possible, and not too ugly, to handle lists, conditionals,
//    *and* pipelines entirely within `run_list`, but in general it is clearer
//    to introduce `run_conditional` and `run_pipeline` functions that
//    are called by `run_list`. It’s up to you.
//
//    PHASE 2: Introduce a loop to run a list of commands, waiting for each
//       to finish before going on to the next.
//    PHASE 3: Change the loop to handle conditional chains.
//    PHASE 4: Change the loop to handle pipelines. Start all processes in
//       the pipeline in parallel. The status of a pipeline is the status of
//       its LAST command.
//    PHASE 5: Change the loop to handle background conditional chains.
//       This may require adding another call to `fork()`!

command* run_command (shell_parser, bool, bool, int, int);
void run_conditional (shell_parser sec);
int run_pipeline (shell_parser sec);

void run_list(shell_parser sec) {
    shell_parser line_parser(sec);
    for (auto par = line_parser.first_conditional(); par; par.next_conditional()) {
        // for background conditionals
        if (par.op() == 3){
            pid_t p = fork();
            if (p == 0){
                run_conditional(par);
                _exit(0);
            }else if (p > 0){
                continue;
            }else{
                perror("fork failed");
            }
        }else{
            run_conditional(par);
        }
        
    }
}

int run_pipeline (shell_parser sec){
    shell_parser line_parser(sec);
    int pipeline_status = 1;

    // we initialize a previous pipe
    int prev_pipe[2] = {-1, -1};
    std::vector<command*> commands; 
    command* last_command = nullptr;

    // get the first command and see if its token is "|"
    for (auto par = line_parser.first_command(); par; par.next_command()) {
        // create a pipe if the op is |
        int new_pipe[2];
        if (par.op() == 4){
            pipe(new_pipe);
        }else{
            new_pipe[0] = -1;
            new_pipe[1] = -1;
        }

        // now we have to connect the previous pipe to the first and then run the pipe
        int pipe_in = prev_pipe[0];
        int pipe_out = new_pipe[1];
        command* c = run_command(par, false, false, pipe_in, pipe_out);

        // Store the command in the vector for later deletion
        commands.push_back(c);

        // close pipes that we are not using anymore
        if (prev_pipe[0] != -1) close(prev_pipe[0]);
        if (new_pipe[1] != -1) close(new_pipe[1]);
        
        // update prev pipes
        prev_pipe[0] = new_pipe[0];
        prev_pipe[1] = new_pipe[1];

        // Track the last command for waiting at the end
        last_command = c;

    }

    // Wait for the last command in the pipeline to complete
    if (last_command && last_command->pid > 0) {
        int status;
        pid_t exited_pid = waitpid(last_command->pid, &status, 0);
        assert(exited_pid == last_command->pid);

        if (WIFEXITED(status)) {
            last_command->exit_status = WEXITSTATUS(status);
            pipeline_status = last_command->exit_status;
        }
    }

    // Delete all commands in the pipeline
    for (command* cmd : commands) {
        delete cmd;
    }

    return pipeline_status;
}


void run_conditional (shell_parser sec){
    shell_parser line_parser(sec);
    bool run_next = true;
    bool cumulative_status = false;
    for (auto par = line_parser.first_pipeline(); par; par.next_pipeline()) {
        int exit_status = -1;
        if (run_next){
            exit_status = run_pipeline (par);
            cumulative_status = (exit_status == 0);    
        }
        // if &&, only run next command if cumulative command succesful
        if (par.op() == 5){
            run_next = cumulative_status;
        } // if ||, only run next command if cumulative command unsuccesful
        else if(par.op() == 6){ 
            run_next = !cumulative_status;
        }else{
            run_next = true;
        }
    }     
    
}

command* run_command (shell_parser sec, bool auto_delete = true, bool wait_for_completion = true, int pipe_in = -1, int pipe_out = -1){
    command* c = new command;
    auto tok = sec.first_token();
    while (tok) {
        c->args.push_back(tok.str());
        tok.next();
    }

    c -> pipe_in = pipe_in;
    c -> pipe_out = pipe_out;

    if (wait_for_completion){
        c->run(true);
    }else{
        c->run(false);
    }
    
    if (auto_delete){
        delete c;
        return nullptr;
    }else{
        return c;
    }
    
}

void clean_up_zombies (){
    int status;
    pid_t p;

    do{
        p = waitpid(-1, &status, WNOHANG);
    } while (p > 0);
}


int main(int argc, char* argv[]) {
    FILE* command_file = stdin;
    bool quiet = false;

    // Check for `-q` option: be quiet (print no prompts)
    if (argc > 1 && strcmp(argv[1], "-q") == 0) {
        quiet = true;
        --argc, ++argv;
    }

    // Check for filename option: read commands from file
    if (argc > 1) {
        command_file = fopen(argv[1], "rb");
        if (!command_file) {
            perror(argv[1]);
            return 1;
        }
    }

    // - Put the shell into the foreground
    // - Ignore the SIGTTOU signal, which is sent when the shell is put back
    //   into the foreground
    claim_foreground(0);
    set_signal_handler(SIGTTOU, SIG_IGN);

    char buf[BUFSIZ];
    int bufpos = 0;
    bool needprompt = true;

    while (!feof(command_file)) {
        // Print the prompt at the beginning of the line
        if (needprompt && !quiet) {
            printf("sh61[%d]$ ", getpid());
            fflush(stdout);
            needprompt = false;
        }

        // Read a string, checking for error or EOF
        if (fgets(&buf[bufpos], BUFSIZ - bufpos, command_file) == nullptr) {
            if (ferror(command_file) && errno == EINTR) {
                // ignore EINTR errors
                clearerr(command_file);
                buf[bufpos] = 0;
            } else {
                if (ferror(command_file)) {
                    perror("sh61");
                }
                break;
            }
        }

        // If a complete command line has been provided, run it
        bufpos = strlen(buf);
        if (bufpos == BUFSIZ - 1 || (bufpos > 0 && buf[bufpos - 1] == '\n')) {
            run_list(shell_parser{buf});
            bufpos = 0;
            needprompt = 1;
        }

        // Handle zombie processes and/or interrupt requests
        clean_up_zombies ();
        
    }

    return 0;
}
