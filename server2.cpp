#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/filesystem.hpp>

#include <fmt/format.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#include <memory>
#include <array>

using boost::asio::ip::tcp;

template <typename T>
void ignore_result(T _arg)
{
    (void) _arg;
}

int create_pid_file()
{
    const auto pid_file = boost::filesystem::temp_directory_path() / "simple_cpp_server.pid";

    // Open the PID file. If it does not exist, create it and give the owner
    // permission to read and write to it.
    const auto fd = open(pid_file.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    if (fd == -1) {
        syslog(LOG_ERR | LOG_USER, "Could not open PID file.");
        return -1;
    }

    // Get the current open flags for the open file descriptor.
    const auto flags = fcntl(fd, F_GETFD);
    if (flags == -1) {
        syslog(LOG_ERR | LOG_USER, "Could not retrieve open flags for PID file.");
        return -1;
    }

    // Enable the FD_CLOEXEC option for the open file descriptor.
    // This option will cause successful calls to exec() to close the file descriptor.
    // Keep in mind that record locks are NOT inherited by forked child processes.
    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
        syslog(LOG_ERR | LOG_USER, "Could not set FD_CLOEXEC on PID file.");
        return -1;
    }

    struct flock input;
    input.l_type = F_WRLCK;
    input.l_whence = SEEK_SET;
    input.l_start = 0;
    input.l_len = 0;

    // Try to acquire the write lock on the PID file. If we cannot get the lock,
    // another instance of the application must already be running or something
    // weird is going on.
    if (fcntl(fd, F_SETLK, &input) == -1) {
        if (EAGAIN == errno || EACCES == errno) {
            syslog(LOG_ERR | LOG_USER, "Could not acquire write lock for PID file. Another instance "
                   "could be running already.");
            return -1;
        }
    }
    
    if (ftruncate(fd, 0) == -1) {
        syslog(LOG_ERR | LOG_USER, "Could not truncate PID file's contents.");
        return -1;
    }

    const auto contents = fmt::format("{}\n", getpid());
    if (write(fd, contents.data(), contents.size()) != static_cast<long>(contents.size())) {
        syslog(LOG_ERR | LOG_USER, "Could not write PID to PID file.");
        return -1;
    }

    return 0;
} // create_pid_file

class server
{
public:
    server(boost::asio::io_service& _io_service, int _port)
        : io_service_{_io_service}
        , signals_{_io_service, SIGTERM, SIGINT, SIGCHLD}
        , acceptor_{_io_service, tcp::endpoint(tcp::v4(), _port)}
        , socket_{_io_service}
    {
        wait_for_signal();
        do_accept();
    } // server (constructor)

private:
    void wait_for_signal()
    {
        signals_.async_wait([this](auto, auto _signal)
        {
            const char* signal_name = "?";
            switch (_signal) {
                case SIGTERM: signal_name = "SIGTERM"; break;
                case SIGINT : signal_name = "SIGINT" ; break;
                case SIGCHLD: signal_name = "SIGCHLD"; break;
            }

            // Only the parent process should check for this signal. We can determine
            // whether we are in the parent by checking if the acceptor is still open.
            if (acceptor_.is_open()) {
                syslog(LOG_INFO | LOG_USER, "Caught signal (parent) [pid:%d, signal:%s]", getpid(), signal_name);

                // Reap completed child processes so that we don't end up with zombies.
                if (SIGCHLD == _signal) {
                    for (int status = 0; waitpid(-1, &status, WNOHANG) > 0;) {}
                }

                if (SIGTERM == _signal || SIGINT == _signal) {
                    acceptor_.close();
                    syslog(LOG_INFO | LOG_USER, "Closed acceptor socket");
                }
                else {
                    syslog(LOG_INFO | LOG_USER, "Rescheduled signal handlers");
                    wait_for_signal();
                }
            }
            else {
                syslog(LOG_INFO | LOG_USER, "Caught signal (child) [pid:%d]", getpid());
            }
        });
    } // wait_for_signal

    void do_accept()
    {
        acceptor_.async_accept(socket_, [this](auto _ec)
        {
            if (!acceptor_.is_open()) {
                return;
            }

            if (!_ec) {
                // Inform the io_service that we are about to fork. The io_service cleans
                // up any internal resources, such as threads, that may interfere with
                // forking.
                io_service_.notify_fork(boost::asio::io_service::fork_prepare);

                if (fork() == 0) {
                    // Inform the io_service that the fork is finished and that this is the
                    // child process. The io_service uses this opportunity to create any
                    // internal file descriptors that must be private to the new process.
                    io_service_.notify_fork(boost::asio::io_service::fork_child);
                    
                    // The child won't be accepting new connections, so we can close the
                    // acceptor. It remains open in the parent.
                    acceptor_.close();

                    // The child process is not interested in processing the SIGCHLD signal.
                    signals_.remove(SIGCHLD);

                    syslog(LOG_INFO | LOG_USER, "Forked child [pid:%d]", getpid());

                    // This is where the child starts!
                    //
                    // Start the request-response loop.
                    // 1. Client needs to negotiate with server about communication rules.
                    // 2. Client must authenticate the user and proxy user against the
                    //    server.
                    // 3. Verify the API request information. Is the client allowed to
                    //    perform the operation?

                    // This allows the child process to exit normally. Another way to
                    // achieve this is by replacing signals_.remove(SIGCHLD) with
                    // signals_.cancel().
                    io_service_.stop();
                }
                else {
                    // Inform the io_service that the fork is finished (or failed) and that
                    // this is the parent process. The io_service uses this opportunity to
                    // recreate any internal resources that were cleaned up during
                    // preparation for the fork.
                    io_service_.notify_fork(boost::asio::io_service::fork_parent);

                    socket_.close();
                    do_accept();
                }
            }
            else {
                syslog(LOG_ERR | LOG_USER, "Accept error: %m");
                do_accept();
            }
        });
    } // do_accept

    boost::asio::io_service& io_service_;
    boost::asio::signal_set signals_;
    tcp::acceptor acceptor_;
    tcp::socket socket_;
}; // class server

int main(int _argc, const char** _argv)
{
    if (_argc != 2) {
        fmt::print("Usage: {} <port>\n", _argv[0]);
        return 1;
    }

    try {
        // Fork the process and have the parent exit. If the process was started
        // from a shell, this returns control to the user. Forking a new process is
        // also a prerequisite for the subsequent call to setsid().
        if (pid_t pid = fork()) {
            if (pid > 0) {
                exit(0);
            }
            else {
                syslog(LOG_ERR | LOG_USER, "First fork failed: %m");
                return 1;
            }
        }

        // Make the process a new session leader. This detaches it from the
        // terminal.
        setsid();

        // A process inherits its working directory from its parent. This could be
        // on a mounted filesystem, which means that the running daemon would
        // prevent this filesystem from being unmounted. Changing to the root
        // directory avoids this problem.
        ignore_result(chdir("/"));

        // The file mode creation mask is also inherited from the parent process.
        // We don't want to restrict the permissions on files created by the
        // daemon, so the mask is cleared.
        umask(0);

        // A second fork ensures the process cannot acquire a controlling terminal.
        if (pid_t pid = fork()) {
            if (pid > 0) {
                exit(0);
            }
            else {
                syslog(LOG_ERR | LOG_USER, "Second fork failed: %m");
                return 1;
            }
        }

        // Close the standard streams. This decouples the daemon from the terminal
        // that started it.
        close(0);
        close(1);
        close(2);

        // We don't want the daemon to have any standard input.
        if (open("/dev/null", O_RDONLY) < 0) {
            syslog(LOG_ERR | LOG_USER, "Unable to open /dev/null: %m");
            return 1;
        }

        // Send standard output to a log file.
        const char* output = "/tmp/asio.daemon.out";
        const int flags = O_WRONLY | O_CREAT | O_APPEND;
        const mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
        if (open(output, flags, mode) < 0) {
            syslog(LOG_ERR | LOG_USER, "Unable to open output file %s: %m", output);
            return 1;
        }

        // Also send standard error to the same log file.
        if (dup(1) < 0) {
            syslog(LOG_ERR | LOG_USER, "Unable to dup output descriptor: %m");
            return 1;
        }

        if (create_pid_file()) {
            return 1;
        }

        boost::asio::io_service io_service;

        // Initialize the server before becoming a daemon. If the process is
        // started from a shell, this means any errors will be reported back to the
        // user.
        server svr{io_service, std::stoi(_argv[1])};

        // The io_service can now be used normally.
        syslog(LOG_INFO | LOG_USER, "Daemon started [pid:%d]", getpid());
        io_service.run();
        syslog(LOG_INFO | LOG_USER, "Daemon stopped [pid:%d]", getpid());
    }
    catch (const std::exception& e) {
        syslog(LOG_ERR | LOG_USER, "Exception: %s", e.what());
        fmt::print(stderr, "Exception: {}", e.what());
    }
}

