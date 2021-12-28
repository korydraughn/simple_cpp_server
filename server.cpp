#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>

#include <fmt/format.h>

#include <syslog.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <iostream>
#include <memory>
#include <array>
#include <string>

using boost::asio::ip::tcp;

class server
{
public:
    server(boost::asio::io_service& _io_service, std::int16_t _port)
        : io_service_{_io_service}
        , signals_{_io_service, SIGCHLD}
        , acceptor_{_io_service, tcp::endpoint(tcp::v4(), _port)}
        , socket_{_io_service}
    {
        do_signal_wait();
        do_accept();
    } // server (constructor)

private:
    void do_signal_wait()
    {
        signals_.async_wait([this](auto, auto) {
            // Only the parent process should check for this signal. We can determine
            // whether we are in the parent by checking if the acceptor is still open.
            if (acceptor_.is_open()) {
                syslog(LOG_INFO | LOG_USER, "Caught signal: %m");
                for (int status = 0; waitpid(-1, &status, WNOHANG) > 0;) {}
                do_signal_wait();
            }
        });
    } // do_signal_wait

    void do_accept()
    {
        acceptor_.async_accept(socket_, [this](auto _ec)
        {
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

                    // This is where the child starts!
                    syslog(LOG_INFO | LOG_USER, "Forked child is done");

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

int main(int _argc, char** _argv)
{
    if (_argc != 2) {
        fmt::print("Usage: {} <port>\n", _argv[0]);
        return 1;
    }

    try {
        boost::asio::io_service io_service;

        // Initialize the server before becoming a daemon. If the process is
        // started from a shell, this means any errors will be reported back to the
        // user.
        server svr{io_service, std::stoi(_argv[1])};

        // Register signal handlers so that the daemon may be shut down. You may
        // also want to register for other signals, such as SIGHUP to trigger a
        // re-read of a configuration file.
        boost::asio::signal_set signals{io_service, SIGINT, SIGTERM};
        signals.async_wait([&io_service](auto, auto) {
            io_service.stop();
        });

        // Inform the io_service that we are about to become a daemon. The
        // io_service cleans up any internal resources, such as threads, that may
        // interfere with forking.
        io_service.notify_fork(boost::asio::io_service::fork_prepare);

        // Fork the process and have the parent exit. If the process was started
        // from a shell, this returns control to the user. Forking a new process is
        // also a prerequisite for the subsequent call to setsid().
        if (pid_t pid = fork()) {
            if (pid > 0) {
                // We're in the parent process and need to exit.
                //
                // When the exit() function is used, the program terminates without
                // invoking local variables' destructors. Only global variables are
                // destroyed. As the io_service object is a local variable, this means
                // we do not have to call:
                //
                //   io_service.notify_fork(boost::asio::io_service::fork_parent);
                //
                // However, this line should be added before each call to exit() if
                // using a global io_service object. An additional call:
                //
                //   io_service.notify_fork(boost::asio::io_service::fork_prepare);
                //
                // should also precede the second fork().
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
        chdir("/");

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

        // Inform the io_service that we have finished becoming a daemon. The
        // io_service uses this opportunity to create any internal file descriptors
        // that need to be private to the new process.
        io_service.notify_fork(boost::asio::io_service::fork_child);

        // The io_service can now be used normally.
        syslog(LOG_INFO | LOG_USER, "Daemon started");
        io_service.run();
        syslog(LOG_INFO | LOG_USER, "Daemon stopped");
    }
    catch (const std::exception& e) {
        syslog(LOG_ERR | LOG_USER, "Exception: %s", e.what());
        std::cerr << "Exception: " << e.what() << std::endl;
    }
}
